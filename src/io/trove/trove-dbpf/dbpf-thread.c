/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#include "gossip.h"
#include "trove.h"
#include "trove-internal.h"
#include "trove-ledger.h"
#include "trove-handle-mgmt.h"
#include "dbpf.h"
#include "dbpf-thread.h"
#include "dbpf-dspace.h"
#include "dbpf-bstream.h"
#include "dbpf-keyval.h"
#include "dbpf-op-queue.h"

extern struct qlist_head dbpf_op_queue;
extern gen_mutex_t dbpf_op_queue_mutex;
extern dbpf_op_queue_p dbpf_completion_queue_array[TROVE_MAX_CONTEXTS];
extern gen_mutex_t *dbpf_completion_queue_array_mutex[TROVE_MAX_CONTEXTS];

#ifdef __PVFS2_TROVE_THREADED__
pthread_cond_t dbpf_op_cond = PTHREAD_COND_INITIALIZER;
static pthread_t dbpf_thread;
static int dbpf_thread_running = 0;
#endif

int dbpf_thread_initialize(void)
{
    int ret = 0;
#ifdef __PVFS2_TROVE_THREADED__
    ret = pthread_create(&dbpf_thread, NULL, dbpf_thread_function, NULL);
    dbpf_thread_running = ((ret == 0) ? 1 : 0);
#endif
    return ret;
}

int dbpf_thread_finalize(void)
{
    int ret = 0;
#ifdef __PVFS2_TROVE_THREADED__
    dbpf_thread_running = 0;
    usleep(500);
    ret = pthread_cancel(dbpf_thread);
    pthread_cond_destroy(&dbpf_op_cond);
#endif
    return ret;
}

void *dbpf_thread_function(void *ptr)
{
#ifdef __PVFS2_TROVE_THREADED__
    int out_count = 0;
    gossip_debug(TROVE_DEBUG, "dbpf_thread_function started\n");

    do
    {

        dbpf_do_one_work_cycle(&out_count);
        usleep((DBPF_OPS_PER_WORK_CYCLE * 10) -
               (out_count * 10));

    } while(dbpf_thread_running);

    gossip_debug(TROVE_DEBUG, "dbpf_thread_function ending\n");
#endif
    return ptr;
}


int dbpf_do_one_work_cycle(int *out_count)
{
#ifdef __PVFS2_TROVE_THREADED__
    int ret = 1;
    int max_num_ops_to_service = DBPF_OPS_PER_WORK_CYCLE;
    dbpf_queued_op_t *cur_op = NULL;
    gen_mutex_t *context_mutex = NULL;
#endif

    assert(out_count);
    *out_count = 0;

#ifdef __PVFS2_TROVE_THREADED__
    do
    {
        /* grab next op from queue and mark it as in service */
        gen_mutex_lock(&dbpf_op_queue_mutex);
        cur_op = dbpf_op_queue_shownext(&dbpf_op_queue);
        if (cur_op)
        {
            dbpf_op_queue_remove(cur_op);

            gen_mutex_lock(&cur_op->mutex);
            assert(cur_op->op.state == OP_QUEUED);
            cur_op->op.state = OP_IN_SERVICE;
            gen_mutex_unlock(&cur_op->mutex);
        }
        gen_mutex_unlock(&dbpf_op_queue_mutex);

        /* if there's no work to be done, return immediately */
        if (cur_op == NULL)
        {
            return ret;
        }
        
        /* otherwise, service the current operation now */
        ret = cur_op->op.svc_fn(&(cur_op->op));
        if (ret != 0)
        {
            /* operation is done and we are telling the caller;
             * ok to pull off queue now.
             *
             * returns error code from operation in queued_op struct
             */
            *out_count++;

            /* this is a macro defined in dbpf-thread.h */
            move_op_to_completion_queue(cur_op);
        }
        else
        {
            gossip_debug(TROVE_DEBUG, "op partially serviced; re-queueing\n");
            dbpf_queued_op_queue(cur_op);
        }

    } while(--max_num_ops_to_service);
#endif

    return 0;
}



/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
