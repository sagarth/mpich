/*
 * Copyright (C) by Argonne National Laboratory
 *     See COPYRIGHT in top-level directory
 */

#include "mpiimpl.h"
#include "mpi_init.h"

/*
=== BEGIN_MPI_T_CVAR_INFO_BLOCK ===

cvars:
    - name        : MPIR_CVAR_ASYNC_PROGRESS
      category    : THREADS
      type        : boolean
      default     : false
      class       : none
      verbosity   : MPI_T_VERBOSITY_USER_BASIC
      scope       : MPI_T_SCOPE_ALL_EQ
      description : >-
        If set to true, MPICH will initiate an additional thread to
        make asynchronous progress on all communication operations
        including point-to-point, collective, one-sided operations and
        I/O.  Setting this variable will automatically increase the
        thread-safety level to MPI_THREAD_MULTIPLE.  While this
        improves the progress semantics, it might cause a small amount
        of performance overhead for regular MPI operations.  The user
        is encouraged to leave one or more hardware threads vacant in
        order to prevent contention between the application threads
        and the progress thread(s).  The impact of oversubscription is
        highly system dependent but may be substantial in some cases,
        hence this recommendation.

    - name        : MPIR_CVAR_CH4_PROGRESS_THREAD_AFFINITY
      category    : CH4
      type        : string
      default     : ""
      class       : device
      verbosity   : MPI_T_VERBOSITY_USER_BASIC
      scope       : MPI_T_SCOPE_ALL_EQ
      description : >-
        Specifies affinity for all progress threads of local processes. Format - comma-separated list of logical processors.
        In case of N progress threads per process the first N logical processors from list will be assigned
        to threads of first local process, the next N logical processors from list - to second local process and so on.
        For example, thread affinity is "0,1,2,3", 2 progress threads per process and 2 processes per node.
        Progress threads of first local process will be pinned on logical processors "0,1",
        progress threads of second local process - on "2,3".
        Cannot work together with MPIR_CVAR_NUM_CLIQUES or MPIR_CVAR_ODD_EVEN_CLIQUES.

=== END_MPI_T_CVAR_INFO_BLOCK ===
*/

#if defined(MPICH_IS_THREADED) && MPICH_THREAD_LEVEL == MPI_THREAD_MULTIPLE

static int MPIR_async_thread_initialized = 0;
static MPID_Thread_id_t progress_thread_id;
static MPL_atomic_int_t async_done = MPL_ATOMIC_INT_T_INITIALIZER(0);

static void progress_fn(void *data)
{
    MPID_Progress_state state;

    MPID_THREAD_CS_ENTER(GLOBAL, MPIR_THREAD_GLOBAL_ALLFUNC_MUTEX);

    MPID_Progress_start(&state);
    while (MPL_atomic_load_int(&async_done) == 0) {
        MPID_Progress_test(&state);
        MPID_THREAD_CS_YIELD(GLOBAL, MPIR_THREAD_GLOBAL_ALLFUNC_MUTEX);
    }
    MPID_Progress_end(&state);

    MPID_THREAD_CS_EXIT(GLOBAL, MPIR_THREAD_GLOBAL_ALLFUNC_MUTEX);

    return;
}

MPL_STATIC_INLINE_PREFIX int MPIDI_parse_progress_thread_affinity(int *thread_affinity,
                                                                  int threads_per_node)
{
    int th_idx, read_count = 0, mpi_errno = MPI_SUCCESS;
    char *affinity_copy = NULL;
    const char *affinity_to_parse = MPIR_CVAR_CH4_PROGRESS_THREAD_AFFINITY;
    char *proc_id_str, *tmp;
    size_t proc_count;

    if (!affinity_to_parse || strlen(affinity_to_parse) == 0) {
        /* generate default affinity */
        proc_count = MPL_get_nprocs();
        for (th_idx = 0; th_idx < threads_per_node; th_idx++) {
            if (th_idx < proc_count)
                thread_affinity[th_idx] = proc_count - th_idx - 1;
            else
                thread_affinity[th_idx] = thread_affinity[th_idx % proc_count];
        }
        goto fn_exit;
    }

    /* create copy of original buffer because it will be modified in strsep */
    affinity_copy = MPL_strdup(affinity_to_parse);
    MPIR_Assert(affinity_copy);
    tmp = affinity_copy;

    for (th_idx = 0; th_idx < threads_per_node; th_idx++) {
        proc_id_str = MPL_strsep(&tmp, " \n\t,");
        if (proc_id_str != NULL) {
            if (atoi(proc_id_str) < 0) {
                printf
                    ("ERROR: unexpected proc_id %s, affinity string %s\n",
                     proc_id_str, affinity_to_parse);
                mpi_errno = MPI_ERR_OTHER;
                goto fn_fail;
            }
            thread_affinity[th_idx] = atoi(proc_id_str);
            read_count++;
        } else {
            printf
                ("ERROR: unexpected end of affinity string, expected %d numbers, read %d, affinity string %s\n",
                 threads_per_node, read_count, affinity_to_parse);
            mpi_errno = MPI_ERR_OTHER;
            goto fn_fail;
        }
    }
    if (read_count < threads_per_node) {
        printf
            ("ERROR: unexpected number of processors (specify 1 logical processor per 1 progress thread), affinity string %s\n",
             affinity_to_parse);
        mpi_errno = MPI_ERR_OTHER;
        goto fn_fail;
    }

  fn_exit:
    if (MPIR_Process.comm_world->rank == 0) {
        for (th_idx = 0; th_idx < threads_per_node; th_idx++) {
            MPL_DBG_MSG_FMT(MPIDI_CH4_DBG_GENERAL, VERBOSE,
                            (MPL_DBG_FDEST, "affinity: thread %d, processor %d",
                             th_idx, thread_affinity[th_idx]));
        }
    }
    MPL_free(affinity_copy);
    return mpi_errno;

  fn_fail:
    goto fn_exit;
}

/* called inside MPID_Init_async_thread to provide device override */
int MPIR_Init_async_thread(void)
{
    int mpi_errno = MPI_SUCCESS, thr_err;
    int global_rank, local_rank, local_size, threads_per_node;
    int *thread_affinity = NULL;
    int num_cliques = 1, affinity_idx;
    MPIR_FUNC_TERSE_STATE_DECL(MPID_STATE_MPIR_INIT_ASYNC_THREAD);

    MPIR_FUNC_TERSE_ENTER(MPID_STATE_MPIR_INIT_ASYNC_THREAD);

    /* Consider nodemap cliques when using debugging CVARs */
    if (MPIR_CVAR_NUM_CLIQUES > 1) {
        num_cliques = MPIR_CVAR_NUM_CLIQUES;
    } else if (MPIR_CVAR_ODD_EVEN_CLIQUES) {
        num_cliques = 2;
    }

    if (num_cliques > 1 && MPIR_CVAR_CH4_PROGRESS_THREAD_AFFINITY &&
        strlen(MPIR_CVAR_CH4_PROGRESS_THREAD_AFFINITY) > 0) {
        fprintf(stderr,
                "Setting affinity for progress threads cannot work correctly with MPIR_CVAR_NUM_CLIQUES or MPIR_CVAR_ODD_EVEN_CLIQUES.\n");
    }

    global_rank = MPIR_Process.comm_world->rank;
    local_rank =
        (MPIR_Process.comm_world->node_comm) ? MPIR_Process.comm_world->node_comm->rank : 0;
    if (num_cliques > 1) {
        /* If num_cliques > 1, using local_size from node_comm will have conflict on thread idx.
         * In multiple nodes case, this would cost extra memory for allocating thread affinity on every
         * node, but it is okay to solve progress thread oversubscription. */
        local_size = MPIR_Process.comm_world->local_size;
    } else {
        local_size =
            (MPIR_Process.comm_world->node_comm) ? MPIR_Process.comm_world->
            node_comm->local_size : 1;
    }

    threads_per_node = local_size;
    thread_affinity = (int *) MPL_malloc(threads_per_node * sizeof(int), MPL_MEM_OTHER);

    MPL_DBG_MSG_FMT(MPIDI_CH4_DBG_GENERAL, VERBOSE,
                    (MPL_DBG_FDEST,
                     " global_rank %d, local_rank %d, local_size %d, threads_per_node %d",
                     global_rank, local_rank, local_size, threads_per_node));

    mpi_errno = MPIDI_parse_progress_thread_affinity(thread_affinity, threads_per_node);
    MPIR_ERR_CHKANDJUMP(mpi_errno, mpi_errno, MPI_ERR_OTHER, "**ch4|parse_thread_affinity");

    int err = 0;
    MPID_Thread_create((MPID_Thread_func_t) progress_fn, NULL, &progress_thread_id, &err);
    MPIR_ERR_CHKANDJUMP1(err, mpi_errno, MPI_ERR_OTHER, "**mutex_create", "**mutex_create %s",
                         strerror(err));

    if (num_cliques > 1) {
        /* In this case, procs on one physical node are partitioned into different virtual nodes,
         * global_rank should be used to avoid binding progress threads from different ranks to the same core. */
        affinity_idx = global_rank;
    } else {
        affinity_idx = local_rank;
    }

    MPL_thread_set_affinity(progress_thread_id, &(thread_affinity[affinity_idx]), 1, &thr_err);
    if (MPIR_CVAR_CH4_PROGRESS_THREAD_AFFINITY &&
        strlen(MPIR_CVAR_CH4_PROGRESS_THREAD_AFFINITY) > 0) {
        /* If the user did not specify the affinity, ignore affinity setting failure */
        MPIR_ERR_CHKANDJUMP(thr_err, mpi_errno, MPI_ERR_OTHER, "**ch4|set_thread_affinity");
    }

    MPIR_FUNC_TERSE_EXIT(MPID_STATE_MPIR_INIT_ASYNC_THREAD);

  fn_exit:
    MPL_free(thread_affinity);
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

/* called inside MPID_Finalize_async_thread to provide device override */
int MPIR_Finalize_async_thread(void)
{
    int mpi_errno = MPI_SUCCESS;
    MPIR_FUNC_TERSE_STATE_DECL(MPID_STATE_MPIR_FINALIZE_ASYNC_THREAD);

    MPIR_FUNC_TERSE_ENTER(MPID_STATE_MPIR_FINALIZE_ASYNC_THREAD);

    MPL_atomic_store_int(&async_done, 1);
    MPID_Thread_join(progress_thread_id);

    MPIR_FUNC_TERSE_EXIT(MPID_STATE_MPIR_FINALIZE_ASYNC_THREAD);

    return mpi_errno;
}

/* called inside MPIR_Init_thread_impl */
int MPII_init_async(void)
{
    int mpi_errno = MPI_SUCCESS;

    if (MPIR_CVAR_ASYNC_PROGRESS) {
        if (MPIR_ThreadInfo.thread_provided == MPI_THREAD_MULTIPLE) {
            mpi_errno = MPID_Init_async_thread();
            if (mpi_errno)
                goto fn_fail;

            MPIR_async_thread_initialized = 1;
        } else {
            printf("WARNING: No MPI_THREAD_MULTIPLE support (needed for async progress)\n");
        }
    }
  fn_exit:
    return mpi_errno;
  fn_fail:
    goto fn_exit;
}

/* called inside MPI_Finalize */
int MPII_finalize_async(void)
{
    int mpi_errno = MPI_SUCCESS;

    /* If the user requested for asynchronous progress, we need to
     * shutdown the progress thread */
    if (MPIR_async_thread_initialized) {
        mpi_errno = MPID_Finalize_async_thread();
    }

    return mpi_errno;
}

#else
int MPIR_Finalize_async_thread(void)
{
    return MPI_SUCCESS;
}

int MPIR_Init_async_thread(void)
{
    return MPI_SUCCESS;
}

int MPII_init_async(void)
{
    return MPI_SUCCESS;
}

int MPII_finalize_async(void)
{
    return MPI_SUCCESS;
}
#endif
