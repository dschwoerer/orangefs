/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef WIN32
#include <sys/time.h>
#include <unistd.h>
#endif

#include "gossip.h"
#include "quicklist.h"
#include "src/io/flow/flowproto-support.h"
#include "gen-locks.h"
#include "bmi.h"
#include "trove.h"
#include "thread-mgr.h"
#include "pint-perf-counter.h"
#include "pvfs2-internal.h"

/* the following buffer settings are used by default if none are specified in
 * the flow descriptor
 */
#define BUFFERS_PER_FLOW 8
#define BUFFER_SIZE (256*1024)

#define MAX_REGIONS 64

#define FLOW_CLEANUP_CANCEL_PATH(__flow_data, __cancel_path)          \
do {                                                                  \
    struct flow_descriptor *__flow_d = (__flow_data)->parent;         \
    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, "flowproto completing %p\n",\
                 __flow_d);                                           \
    cleanup_buffers(__flow_data);                                     \
    __flow_d = (__flow_data)->parent;                                 \
    free(__flow_data);                                                \
    __flow_d->release(__flow_d);                                      \
    __flow_d->callback(__flow_d, __cancel_path);                      \
} while(0)

#define FLOW_CLEANUP(___flow_data) FLOW_CLEANUP_CANCEL_PATH(___flow_data, 0)

struct result_chain_entry
{
    PVFS_id_gen_t posted_id;
    char *buffer_offset;
    PINT_Request_result result;
    PVFS_size size_list[MAX_REGIONS];
    PVFS_offset offset_list[MAX_REGIONS];
    struct result_chain_entry *next;
    struct fp_queue_item *q_item;
    struct PINT_thread_mgr_trove_callback trove_callback;
};

/* fp_queue_item describes an individual buffer being used within the flow */
struct fp_queue_item
{
    PVFS_id_gen_t posted_id;
    PVFS_id_gen_t next_id;
    int last;
    int seq;
    void *buffer;
    PVFS_size buffer_used;
    PVFS_size out_size;
    struct result_chain_entry result_chain;
    int result_chain_count;
    struct qlist_head list_link;
    flow_descriptor *parent;
    struct PINT_thread_mgr_bmi_callback bmi_callback;
    struct PINT_thread_mgr_bmi_callback next_bmi_callback;
};

/* fp_private_data is information specific to this flow protocol, stored
 * in flow descriptor but hidden from caller
 */
struct fp_private_data
{
    flow_descriptor *parent;
    struct fp_queue_item* prealloc_array;
    struct qlist_head list_link;
    PVFS_size total_bytes_processed;
    int next_seq;
    int next_seq_to_send;
    int dest_pending;
    int dest_last_posted;
    int initial_posts;
    void *tmp_buffer_list[MAX_REGIONS];
    void *intermediate;
    int cleanup_pending_count;
    int req_proc_done;

    struct qlist_head src_list;
    struct qlist_head dest_list;
    struct qlist_head empty_list;

    /* Additions for forwarding flows */
    int sends_pending;
    int recvs_pending;
    int writes_pending;
    int primary_recvs_throttled;

    PVFS_size total_bytes_req;
    PVFS_size total_bytes_recvd;
    PVFS_size total_bytes_forwarded;
    PVFS_size total_bytes_written;
};
#define PRIVATE_FLOW(target_flow)\
    ((struct fp_private_data*)(target_flow->flow_protocol_data))

static bmi_context_id global_bmi_context = -1;
static void cleanup_buffers(
    struct fp_private_data *flow_data);
static void handle_io_error(
    PVFS_error error_code,
    struct fp_queue_item *q_item,
    struct fp_private_data *flow_data);
static int cancel_pending_bmi(
    struct qlist_head *list);
static int cancel_pending_trove(
    struct qlist_head *list, 
    TROVE_coll_id coll_id);

typedef void (*bmi_recv_callback)(void *, PVFS_size, PVFS_error);
typedef void (*trove_write_callback)(void *, PVFS_error);

static void flow_bmi_recv(struct fp_queue_item* q_item,
                          bmi_recv_callback recv_callback_wrapper,
                          bmi_recv_callback recv_callback);
static inline void server_bmi_recv_callback_wrapper(void *user_ptr,
                                                    PVFS_size actual_size,
                                                    PVFS_error error_code);
static void forwarding_bmi_recv_callback_fn(void *user_ptr,
					    PVFS_size actual_size,
					    PVFS_error error_code);
static void server_bmi_recv_callback_fn(void *user_ptr,
                                        PVFS_size actual_size,
                                        PVFS_error error_code);
static int flow_process_request( struct fp_queue_item* q_item );
static void flow_trove_write(struct fp_queue_item* q_item,
                             PVFS_size actual_size,
                             trove_write_callback write_callback_wrapper,
                             trove_write_callback write_callback);
static inline void server_trove_write_callback_wrapper(void *user_ptr,
                                                       PVFS_error error_code);
static void server_trove_write_callback_fn(void *user_ptr,
                                               PVFS_error error_code);
static TROVE_context_id global_trove_context = -1;
static void forwarding_trove_write_callback_fn(void *user_ptr,
                                               PVFS_error error_code);
int forwarding_is_flow_complete(struct fp_private_data* flow_data);
static inline void forwarding_bmi_recv_callback_wrapper(void *user_ptr,
							PVFS_size actual_size,
							PVFS_error error_code);
static void forwarding_bmi_send(struct fp_queue_item* q_item,
                                PVFS_size actual_size);
static void forwarding_bmi_send_callback_wrapper(void *user_ptr,
						 PVFS_size actual_size,
						 PVFS_error error_code);
static void forwarding_bmi_send_callback_fn(void *user_ptr,
					    PVFS_size actual_size,
					    PVFS_error error_code);
static void handle_forwarding_io_error(PVFS_error error_code,
                                      struct fp_queue_item* q_item,
                                      struct fp_private_data* flow_data);
static inline void server_write_flow_post_init(flow_descriptor *flow_d,
                                               struct fp_private_data *flow_data);
static inline void forwarding_flow_post_init(flow_descriptor* flow_d,
					     struct fp_private_data* flow_data);


#ifdef __PVFS2_TROVE_SUPPORT__
typedef struct
{
    TROVE_coll_id coll_id;
    int sync_mode;
    struct qlist_head link;
} id_sync_mode_t;

static QLIST_HEAD(s_id_sync_mode_list);
static gen_mutex_t id_sync_mode_mutex = GEN_MUTEX_INITIALIZER;




static void bmi_recv_callback_fn(void *user_ptr,
                                 PVFS_size actual_size,
                                 PVFS_error error_code);

static int bmi_send_callback_fn(void *user_ptr,
                                PVFS_size actual_size,
                                PVFS_error error_code,
                                int initial_call_flag);
static void trove_read_callback_fn(void *user_ptr,
                                   PVFS_error error_code);
static void trove_write_callback_fn(void *user_ptr,
                                    PVFS_error error_code);

static int get_data_sync_mode(TROVE_coll_id coll_id);

/* wrappers that let us acquire locks or use return values in different
 * ways, depending on if the function is triggered from an external thread
 * or in a direct invocation
 */
static inline void bmi_send_callback_wrapper(void *user_ptr,
                                             PVFS_size actual_size,
                                             PVFS_error error_code)
{
    struct fp_private_data *flow_data = 
        PRIVATE_FLOW(((struct fp_queue_item*)user_ptr)->parent);
    gen_mutex_lock(&flow_data->parent->flow_mutex);

    bmi_send_callback_fn(user_ptr, actual_size, error_code, 0);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}

static inline void bmi_recv_callback_wrapper(void *user_ptr,
                                             PVFS_size actual_size,
                                             PVFS_error error_code)
{
    struct fp_private_data *flow_data = 
        PRIVATE_FLOW(((struct fp_queue_item*)user_ptr)->parent);
    gen_mutex_lock(&flow_data->parent->flow_mutex);
    bmi_recv_callback_fn(user_ptr, actual_size, error_code);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}

static inline void trove_read_callback_wrapper(void *user_ptr,
                                               PVFS_error error_code)
{
    struct fp_private_data *flow_data = 
        PRIVATE_FLOW(((struct
        result_chain_entry*)user_ptr)->q_item->parent);
    gen_mutex_lock(&flow_data->parent->flow_mutex);
    trove_read_callback_fn(user_ptr, error_code);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}

static inline void trove_write_callback_wrapper(void *user_ptr,
                                                PVFS_error error_code)
{
    struct fp_private_data *flow_data = 
        PRIVATE_FLOW(((struct
                       result_chain_entry*)user_ptr)->q_item->parent);
    gen_mutex_lock(&flow_data->parent->flow_mutex);
    trove_write_callback_fn(user_ptr, error_code);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}

#endif

static void mem_to_bmi_callback_fn(void *user_ptr,
                                   PVFS_size actual_size,
                                   PVFS_error error_code);
static void bmi_to_mem_callback_fn(void *user_ptr,
                                   PVFS_size actual_size,
                                   PVFS_error error_code);

/* wrappers that let us acquire locks or use return values in different
 * ways, depending on if the function is triggered from an external thread
 * or in a direct invocation
 */
static void mem_to_bmi_callback_wrapper(void *user_ptr,
                                        PVFS_size actual_size,
                                        PVFS_error error_code)
{
    struct fp_private_data *flow_data = 
        PRIVATE_FLOW(((struct fp_queue_item*)user_ptr)->parent);
    gen_mutex_lock(&flow_data->parent->flow_mutex);
    mem_to_bmi_callback_fn(user_ptr, actual_size, error_code);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}

static void bmi_to_mem_callback_wrapper(void *user_ptr,
                                        PVFS_size actual_size,
                                        PVFS_error error_code)
{
    struct fp_private_data *flow_data = 
        PRIVATE_FLOW(((struct fp_queue_item*)user_ptr)->parent);

    assert(flow_data);
    assert(flow_data->parent);

    gen_mutex_lock(&flow_data->parent->flow_mutex);
    bmi_to_mem_callback_fn(user_ptr, actual_size, error_code);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}

/* interface prototypes */
static int fp_multiqueue_initialize(int flowproto_id);

static int fp_multiqueue_finalize(void);

static int fp_multiqueue_getinfo(flow_descriptor  *flow_d,
                                 int option,
                                 void *parameter);

static int fp_multiqueue_setinfo(flow_descriptor *flow_d,
                                 int option,
                                 void *parameter);

static int fp_multiqueue_post(flow_descriptor *flow_d);

static int fp_multiqueue_cancel(flow_descriptor *flow_d);

static char fp_multiqueue_name[] = "flowproto_multiqueue";

struct flowproto_ops fp_multiqueue_ops = {
    fp_multiqueue_name,
    fp_multiqueue_initialize,
    fp_multiqueue_finalize,
    fp_multiqueue_getinfo,
    fp_multiqueue_setinfo,
    fp_multiqueue_post,
    fp_multiqueue_cancel
};

/* fp_multiqueue_initialize()
 *
 * starts up the flow protocol
 *
 * returns 0 on succes, -PVFS_error on failure
 */
int fp_multiqueue_initialize(int flowproto_id)
{
    int ret = -1;

    ret = PINT_thread_mgr_bmi_start();
    if(ret < 0)
        return(ret);
    PINT_thread_mgr_bmi_getcontext(&global_bmi_context);

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = PINT_thread_mgr_trove_start();
    if(ret < 0)
    {
        PINT_thread_mgr_bmi_stop();
        return(ret);
    }
    PINT_thread_mgr_trove_getcontext(&global_trove_context);
#endif

    return(0);
}

/* fp_multiqueue_finalize()
 *
 * shuts down the flow protocol
 *
 * returns 0 on success, -PVFS_error on failure
 */
int fp_multiqueue_finalize(void)
{
    PINT_thread_mgr_bmi_stop();
#ifdef __PVFS2_TROVE_SUPPORT__
    {
        id_sync_mode_t *cur_info = NULL;
        struct qlist_head *tmp_link = NULL, *scratch_link = NULL;

        PINT_thread_mgr_trove_stop();

        gen_mutex_lock(&id_sync_mode_mutex);
        qlist_for_each_safe(tmp_link, scratch_link, &s_id_sync_mode_list)
        {
            cur_info = qlist_entry(tmp_link, id_sync_mode_t, link);
            qlist_del(&cur_info->link);
            free(cur_info);
            cur_info = NULL;
        }
        gen_mutex_unlock(&id_sync_mode_mutex);
    }
#endif
    return (0);
}

/* fp_multiqueue_getinfo()
 *
 * retrieves runtime parameters from flow protocol
 *
 * returns 0 on success, -PVFS_error on failure
 */
int fp_multiqueue_getinfo(flow_descriptor *flow_d,
                          int option,
                          void *parameter)
{
    int *type;

    switch(option)
    {
        case FLOWPROTO_TYPE_QUERY:
            type = parameter;
            if(*type == FLOWPROTO_MULTIQUEUE)
                return(0);
            else
                return(-PVFS_ENOPROTOOPT);
        default:
            return(-PVFS_ENOSYS);
    }
}

/* fp_multiqueue_setinfo()
 *
 * sets runtime parameters in flow protocol
 *
 * returns 0 on success, -PVFS_error on failure
 */
int fp_multiqueue_setinfo(flow_descriptor *flow_d,
                          int option,
                          void *parameter)
{
    int ret = -PVFS_ENOSYS;

    switch(option)
    {
#ifdef __PVFS2_TROVE_SUPPORT__
        case FLOWPROTO_DATA_SYNC_MODE:
        {
            TROVE_coll_id coll_id = 0, sync_mode = 0;
            id_sync_mode_t *new_id_mode = NULL;
            struct qlist_head* iterator = NULL;
            struct qlist_head* scratch = NULL;
            id_sync_mode_t *tmp_mode = NULL;

            assert(parameter && strlen(parameter));
            sscanf((const char *)parameter, "%d,%d",
                   &coll_id, &sync_mode);

            ret = -ENOMEM;

            new_id_mode = (id_sync_mode_t *)malloc(
                sizeof(id_sync_mode_t));
            if (new_id_mode)
            {
                gen_mutex_lock(&id_sync_mode_mutex);
                /* remove any old instances of this fs id */
                qlist_for_each_safe(iterator, scratch, &s_id_sync_mode_list)
                {
                    tmp_mode = qlist_entry(iterator, id_sync_mode_t, link);
                    assert(tmp_mode);
                    if(tmp_mode->coll_id == coll_id)
                    {
                        qlist_del(&tmp_mode->link);
                    }
                }

                /* add new instance */
                new_id_mode->coll_id = coll_id;
                new_id_mode->sync_mode = sync_mode;

                qlist_add_tail(&new_id_mode->link, &s_id_sync_mode_list);
                gen_mutex_unlock(&id_sync_mode_mutex);

                gossip_debug(
                    GOSSIP_FLOW_PROTO_DEBUG, "fp_multiqueue_setinfo: "
                    "data sync mode on coll_id %d set to %d\n",
                    coll_id, sync_mode);
                ret = 0;
            }
        }
        break;
#endif
        default:
            break;
    }
    return ret;
}

/* fp_multiqueue_cancel()
 *
 * cancels a previously posted flow
 *
 * returns 0 on success, 1 on immediate completion, -PVFS_error on failure
 */
int fp_multiqueue_cancel(flow_descriptor  *flow_d)
{
    struct fp_private_data *flow_data = PRIVATE_FLOW(flow_d);

    gossip_err("%s: flow proto cancel called on %p\n", __func__, flow_d);
    gen_mutex_lock(&flow_data->parent->flow_mutex);
    /*
      if the flow is already marked as complete, then there is nothing
      to do
    */
    if(flow_d->state != FLOW_COMPLETE)
    {
        gossip_debug(GOSSIP_CANCEL_DEBUG,
                     "%s: called on active flow, %lld bytes transferred.\n",
                     __func__, lld(flow_d->total_transferred));
        assert(flow_d->state == FLOW_TRANSMITTING);
        /* NOTE: set flow error class bit so that system interface understands
         * that this may be a retry-able error
         */
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(-(PVFS_ECANCEL|PVFS_ERROR_FLOW), NULL, flow_data);
        if(flow_data->parent->state == FLOW_COMPLETE)
        {
            gen_mutex_unlock(&flow_data->parent->flow_mutex);
            FLOW_CLEANUP_CANCEL_PATH(flow_data, 1);
            return(0);
        }
    }
    else
    {
        gossip_debug(GOSSIP_CANCEL_DEBUG,
                     "%s: called on already completed flow; doing nothing.\n",
                     __func__);
    }
    gen_mutex_unlock(&flow_data->parent->flow_mutex);

    return(0);
}

/* fp_multiqueue_post()
 *
 * posts a flow descriptor to begin work
 *
 * returns 0 on success, 1 on immediate completion, -PVFS_error on failure
 */
int fp_multiqueue_post(flow_descriptor  *flow_d)
{
    struct fp_private_data *flow_data = NULL;
    int i;

    gossip_err("Executing %s...\n",__func__);

    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, "flowproto posting %p\n",
                 flow_d);

    assert((flow_d->src.endpoint_id == BMI_ENDPOINT && 
            flow_d->dest.endpoint_id == TROVE_ENDPOINT) ||
           (flow_d->src.endpoint_id == BMI_ENDPOINT &&
            flow_d->dest.endpoint_id == REPLICATION_ENDPOINT) ||
           (flow_d->src.endpoint_id == TROVE_ENDPOINT &&
            flow_d->dest.endpoint_id == BMI_ENDPOINT) ||
           (flow_d->src.endpoint_id == MEM_ENDPOINT &&
            flow_d->dest.endpoint_id == BMI_ENDPOINT) ||
           (flow_d->src.endpoint_id == BMI_ENDPOINT &&
            flow_d->dest.endpoint_id == MEM_ENDPOINT));

    flow_data = (struct fp_private_data*)malloc(sizeof(struct
        fp_private_data));
    if(!flow_data)
        return(-PVFS_ENOMEM);
    memset(flow_data, 0, sizeof(struct fp_private_data));
    
    flow_d->flow_protocol_data = flow_data;
    flow_d->state = FLOW_TRANSMITTING;
    flow_data->parent = flow_d;
    INIT_QLIST_HEAD(&flow_data->src_list);
    INIT_QLIST_HEAD(&flow_data->dest_list);
    INIT_QLIST_HEAD(&flow_data->empty_list);

    /* if a file datatype offset was specified, go ahead and skip ahead 
     * before doing anything else
     */
    if(flow_d->file_req_offset)
        PINT_REQUEST_STATE_SET_TARGET(flow_d->file_req_state,
            flow_d->file_req_offset);

    /* set boundaries on file datatype */
    if(flow_d->aggregate_size > -1)
    {
        PINT_REQUEST_STATE_SET_FINAL(flow_d->file_req_state,
            flow_d->aggregate_size+flow_d->file_req_offset);
    }
    else
    {
        PINT_REQUEST_STATE_SET_FINAL(flow_d->file_req_state,
            flow_d->file_req_offset +
            PINT_REQUEST_TOTAL_BYTES(flow_d->mem_req));
    }

    if(flow_d->buffer_size < 1)
        flow_d->buffer_size = BUFFER_SIZE;
    if(flow_d->buffers_per_flow < 1)
        flow_d->buffers_per_flow = BUFFERS_PER_FLOW;
        
    flow_data->prealloc_array = (struct fp_queue_item*)
        malloc(flow_d->buffers_per_flow*sizeof(struct fp_queue_item));
    if(!flow_data->prealloc_array)
    {
        free(flow_data);
        return(-PVFS_ENOMEM);
    }
    memset(flow_data->prealloc_array, 0,
        flow_d->buffers_per_flow*sizeof(struct fp_queue_item));
    for(i=0; i<flow_d->buffers_per_flow; i++)
    {
        flow_data->prealloc_array[i].parent = flow_d;
        flow_data->prealloc_array[i].bmi_callback.data = 
            &(flow_data->prealloc_array[i]);
    }

    /* remaining setup depends on the endpoints we intend to use */
    if(flow_d->src.endpoint_id == BMI_ENDPOINT &&
        flow_d->dest.endpoint_id == MEM_ENDPOINT)
    {
        flow_data->prealloc_array[0].buffer = flow_d->dest.u.mem.buffer;
        flow_data->prealloc_array[0].bmi_callback.fn =
            bmi_to_mem_callback_wrapper;
        /* put all of the buffers on empty list, we don't really do any
         * queueing for this type of flow
         */
        for(i=0; i<flow_d->buffers_per_flow; i++)
        {
            qlist_add_tail(&flow_data->prealloc_array[i].list_link,
                &flow_data->empty_list);
        }
        gen_mutex_lock(&flow_data->parent->flow_mutex);
        bmi_to_mem_callback_fn(&(flow_data->prealloc_array[0]), 0, 0);
        if(flow_data->parent->state == FLOW_COMPLETE)
        {
            gen_mutex_unlock(&flow_data->parent->flow_mutex);
            FLOW_CLEANUP(flow_data);
        }
        else
        {
            gen_mutex_unlock(&flow_data->parent->flow_mutex);
        }
    }
    else if(flow_d->src.endpoint_id == MEM_ENDPOINT &&
        flow_d->dest.endpoint_id == BMI_ENDPOINT)
    {
        flow_data->prealloc_array[0].buffer = flow_d->src.u.mem.buffer;
        flow_data->prealloc_array[0].bmi_callback.fn =
            mem_to_bmi_callback_wrapper;
        /* put all of the buffers on empty list, we don't really do any
         * queueing for this type of flow
         */
        for(i=0; i<flow_d->buffers_per_flow; i++)
        {
            qlist_add_tail(&flow_data->prealloc_array[i].list_link,
                &flow_data->empty_list);
        }
        gen_mutex_lock(&flow_data->parent->flow_mutex);
        mem_to_bmi_callback_fn(&(flow_data->prealloc_array[0]), 0, 0);
        if(flow_data->parent->state == FLOW_COMPLETE)
        {
            gen_mutex_unlock(&flow_data->parent->flow_mutex);
            FLOW_CLEANUP(flow_data);
        }
        else
        {
            gen_mutex_unlock(&flow_data->parent->flow_mutex);
        }
    }
#ifdef __PVFS2_TROVE_SUPPORT__
    else if(flow_d->src.endpoint_id  == BMI_ENDPOINT   &&
            flow_d->dest.endpoint_id == REPLICATION_ENDPOINT) 
    {
         /* Create a flow that is simultaneously written thru trove and forwarded to
          * an additional bmi address.
          */
         gossip_lerr("Calling forwarding_flow_post_init...\n");

         forwarding_flow_post_init(flow_d, flow_data);
    }
    else if(flow_d->src.endpoint_id == TROVE_ENDPOINT &&
        flow_d->dest.endpoint_id == BMI_ENDPOINT)
    {
        flow_data->initial_posts = flow_d->buffers_per_flow;
        gen_mutex_lock(&flow_data->parent->flow_mutex);
        for(i=0; i<flow_d->buffers_per_flow; i++)
        {
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "flowproto-multiqueue forcing bmi_send_callback_fn.\n");

            bmi_send_callback_fn(&(flow_data->prealloc_array[i]), 0, 0, 1);
            if(flow_data->dest_last_posted)
            {
                break;
            }
        }
        if(flow_data->parent->state == FLOW_COMPLETE)
        {
            gen_mutex_unlock(&flow_data->parent->flow_mutex);
            FLOW_CLEANUP(flow_data);
        }
        else
        {
            gen_mutex_unlock(&flow_data->parent->flow_mutex);
        }
    }
    else if(flow_d->src.endpoint_id  == BMI_ENDPOINT &&
            flow_d->dest.endpoint_id == TROVE_ENDPOINT)
    {
        /* Initiate BMI-rcv,trove-write loop for this server */
        gossip_lerr("Calling server_write_flow_post_init()...\n");
        server_write_flow_post_init(flow_d,flow_data);
    }
#endif
    else
    {
        return(-ENOSYS);
    }

    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, "flowproto posted %p\n",
                 flow_d);
    return (0);
}

#ifdef __PVFS2_TROVE_SUPPORT__
/* bmi_recv_callback_fn()
 *
 * function to be called upon completion of a BMI recv operation
 * 
 * no return value
 */
static void bmi_recv_callback_fn(void *user_ptr,
                                 PVFS_size actual_size,
                                 PVFS_error error_code)
{
    struct fp_queue_item *q_item = user_ptr;
    int ret;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    PVFS_size tmp_actual_size;
    struct result_chain_entry *result_tmp;
    struct result_chain_entry *old_result_tmp;
    PVFS_size bytes_processed = 0;
    void *tmp_buffer;
    void *tmp_user_ptr;
    int sync_mode = 0;

    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
        "flowproto-multiqueue bmi_recv_callback_fn, error code: %d, flow: %p.\n",
        error_code, flow_data->parent);

    q_item->posted_id = 0;

    if(error_code != 0 || flow_data->parent->error_code != 0)
    {
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* remove from current queue */
    qlist_del(&q_item->list_link);
    /* add to dest queue */
    qlist_add_tail(&q_item->list_link, &flow_data->dest_list);
    result_tmp = &q_item->result_chain;
    do{
        assert(result_tmp->result.bytes);
        result_tmp->q_item = q_item;
        result_tmp->trove_callback.data = result_tmp;
        result_tmp->trove_callback.fn = trove_write_callback_wrapper;
        /* XXX: can someone confirm this avoids a segfault in the immediate
         * completion case? */
        tmp_user_ptr = result_tmp;
        assert(result_tmp->result.bytes);

        if(PINT_REQUEST_DONE(q_item->parent->file_req_state))
        {
            /* This is the last write operation for this flow.  Set sync
             * flag if needed
             */ 
            sync_mode = get_data_sync_mode(
                q_item->parent->dest.u.trove.coll_id);
        }

        ret = trove_bstream_write_list(
            q_item->parent->dest.u.trove.coll_id,
            q_item->parent->dest.u.trove.handle,
            (char**)&result_tmp->buffer_offset,
            &result_tmp->result.bytes,
            1,
            result_tmp->result.offset_array,
            result_tmp->result.size_array,
            result_tmp->result.segs,
            &q_item->out_size,
            sync_mode,
            NULL,
            &result_tmp->trove_callback,
            global_trove_context,
            &result_tmp->posted_id,
            q_item->parent->hints);

        result_tmp = result_tmp->next;

        if(ret < 0)
        {
            gossip_err("%s: I/O error occurred\n", __func__);
            handle_io_error(ret, q_item, flow_data);
            return;
        }

        if(ret == 1)
        {
            /* immediate completion; trigger callback ourselves */
            trove_write_callback_fn(tmp_user_ptr, 0);
        }
    }while(result_tmp);

    /* do we need to repost another recv? */

    if((!PINT_REQUEST_DONE(q_item->parent->file_req_state)) 
        && qlist_empty(&flow_data->src_list) 
        && !qlist_empty(&flow_data->empty_list))
    {
        q_item = qlist_entry(flow_data->empty_list.next,
            struct fp_queue_item, list_link);
        qlist_del(&q_item->list_link);
        qlist_add_tail(&q_item->list_link, &flow_data->src_list);

        if(!q_item->buffer)
        {
            /* if the q_item has not been used, allocate a buffer */
            q_item->buffer = BMI_memalloc(
                q_item->parent->src.u.bmi.address,
                q_item->parent->buffer_size, BMI_RECV);
            /* TODO: error handling */
            assert(q_item->buffer);
            q_item->bmi_callback.fn = bmi_recv_callback_wrapper;
        }
        
        result_tmp = &q_item->result_chain;
        old_result_tmp = result_tmp;
        tmp_buffer = q_item->buffer;
        do{
            q_item->result_chain_count++;
            if(!result_tmp)
            {
                result_tmp = (struct result_chain_entry*)malloc(
                    sizeof(struct result_chain_entry));
                assert(result_tmp);
                memset(result_tmp, 0, sizeof(struct result_chain_entry));
                old_result_tmp->next = result_tmp;
            }
            /* process request */
            result_tmp->result.offset_array = 
                result_tmp->offset_list;
            result_tmp->result.size_array = 
                result_tmp->size_list;
            result_tmp->result.bytemax = flow_data->parent->buffer_size - 
                bytes_processed;
            result_tmp->result.bytes = 0;
            result_tmp->result.segmax = MAX_REGIONS;
            result_tmp->result.segs = 0;
            result_tmp->buffer_offset = tmp_buffer;
            ret = PINT_process_request(q_item->parent->file_req_state,
                q_item->parent->mem_req_state,
                &q_item->parent->file_data,
                &result_tmp->result,
                PINT_SERVER);
            /* TODO: error handling */ 
            assert(ret >= 0);

            if(result_tmp->result.bytes == 0)
            {
                if(result_tmp != &q_item->result_chain)
                {
                    free(result_tmp);
                    old_result_tmp->next = NULL;
                }
                q_item->result_chain_count--;
            }
            else
            {
                old_result_tmp = result_tmp;
                result_tmp = result_tmp->next;
                tmp_buffer = (void*)((char*)tmp_buffer + old_result_tmp->result.bytes);
                bytes_processed += old_result_tmp->result.bytes;
            }
        }while(bytes_processed < flow_data->parent->buffer_size && 
            !PINT_REQUEST_DONE(q_item->parent->file_req_state));

        assert(bytes_processed <= flow_data->parent->buffer_size);
        if(bytes_processed == 0)
        {        
            qlist_del(&q_item->list_link);
            qlist_add_tail(&q_item->list_link, &flow_data->empty_list);
            return;
        }

        flow_data->total_bytes_processed += bytes_processed;

        gossip_debug(GOSSIP_DIRECTIO_DEBUG,
                     "offset %llu, buffer ptr: %p\n",
                     llu(q_item->result_chain.result.offset_array[0]),
                     q_item->buffer);

        /* TODO: what if we recv less than expected? */
        ret = BMI_post_recv(&q_item->posted_id,
                            q_item->parent->src.u.bmi.address,
                            ((char *)q_item->buffer),
                            flow_data->parent->buffer_size,
                            &tmp_actual_size,
            BMI_PRE_ALLOC,
            q_item->parent->tag,
            &q_item->bmi_callback,
            global_bmi_context,
            (bmi_hint)q_item->parent->hints);

        if(ret < 0)
        {
            gossip_err("%s: I/O error occurred\n", __func__);
            handle_io_error(ret, q_item, flow_data);
            return;
        }

        if(ret == 1)
        {
            /* immediate completion; trigger callback ourselves */
            bmi_recv_callback_fn(q_item, tmp_actual_size, 0);
        }
    }

    return;
}


/* trove_read_callback_fn()
 *
 * function to be called upon completion of a trove read operation
 *
 * no return value
 */
static void trove_read_callback_fn(void *user_ptr,
                                   PVFS_error error_code)
{
    int ret;
    struct result_chain_entry *result_tmp = user_ptr;
    struct fp_queue_item *q_item = result_tmp->q_item;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    struct result_chain_entry *old_result_tmp;
    int done = 0;
    struct qlist_head *tmp_link;

    q_item = result_tmp->q_item;

    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
        "flowproto-multiqueue trove_read_callback_fn, error_code: %d, flow: %p.\n",
        error_code, flow_data->parent);

    result_tmp->posted_id = 0;

    if(error_code != 0 || flow_data->parent->error_code != 0)
    {
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* don't do anything until the last read completes */
    if(q_item->result_chain_count > 1)
    {
        q_item->result_chain_count--;
        return;
    }

    /* remove from current queue */
    qlist_del(&q_item->list_link);
    /* add to dest queue */
    qlist_add_tail(&q_item->list_link, &flow_data->dest_list);

    result_tmp = &q_item->result_chain;
    do{
        old_result_tmp = result_tmp;
        result_tmp = result_tmp->next;
        if(old_result_tmp != &q_item->result_chain)
            free(old_result_tmp);
    }while(result_tmp);
    q_item->result_chain.next = NULL;
    q_item->result_chain_count = 0;

    /* while we hold dest lock, look for next seq no. to send */
    do{
        qlist_for_each(tmp_link, &flow_data->dest_list)
        {
            q_item = qlist_entry(tmp_link, struct fp_queue_item,
                list_link);
            if(q_item->seq == flow_data->next_seq_to_send)
                break;
        }

        if(q_item->seq == flow_data->next_seq_to_send)
        {
            flow_data->dest_pending++;
            assert(q_item->buffer_used);
            ret = BMI_post_send(&q_item->posted_id,
                q_item->parent->dest.u.bmi.address,
                q_item->buffer,
                q_item->buffer_used,
                BMI_PRE_ALLOC,
                q_item->parent->tag,
                &q_item->bmi_callback,
                global_bmi_context,
                (bmi_hint)q_item->parent->hints);
            flow_data->next_seq_to_send++;
            if(q_item->last)
            {
                flow_data->initial_posts = 0;
                flow_data->dest_last_posted = 1;
            }
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "%s: (post send time) ini posts: %d, pending: %d, last: %d\n",
                __func__,
                flow_data->initial_posts, flow_data->dest_pending,
                flow_data->dest_last_posted);
        }
        else
        {
            ret = 0;
            done = 1;
        }        

        if(ret < 0)
        {
            gossip_err("%s: I/O error occurred\n", __func__);
            handle_io_error(ret, q_item, flow_data);
            return;
        }

        if(ret == 1)
        {
            /* immediate completion; trigger callback ourselves */
            ret = bmi_send_callback_fn(q_item, q_item->buffer_used, 0, 0);
            /* if that callback finished the flow, then return now */
            if(ret == 1)
                return;
        }
    }
    while(!done);

    return;
}

/* bmi_send_callback_fn()
 *
 * function to be called upon completion of a BMI send operation
 *
 * returns 1 if flow completes, 0 otherwise
 */
static int bmi_send_callback_fn(void *user_ptr,
                                PVFS_size actual_size,
                                PVFS_error error_code,
                                int initial_call_flag)
{
    struct fp_queue_item *q_item = user_ptr;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    int ret;
    struct result_chain_entry *result_tmp;
    struct result_chain_entry *old_result_tmp;
    void *tmp_buffer;
    PVFS_size bytes_processed = 0;
    void *tmp_user_ptr = NULL;

    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
        "flowproto-multiqueue bmi_send_callback_fn, error_code: %d, "
        "initial_call_flag: %d, flow: %p.\n", error_code, initial_call_flag,
        flow_data->parent);

    if(flow_data->parent->error_code != 0 && initial_call_flag)
    {
        /* cleanup path already triggered, don't do anything more */
        return(1);
    }

    q_item->posted_id = 0;

    if(error_code != 0 || flow_data->parent->error_code != 0)
    {
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(error_code, q_item, flow_data);
        if(flow_data->parent->state == FLOW_COMPLETE)
            return(1);
        else
            return(0);
    }

    PINT_perf_count(PINT_server_pc,
                    PINT_PERF_READ, 
                    actual_size, 
                    PINT_PERF_ADD);
    PINT_perf_count(PINT_server_pc,
                    PINT_PERF_FLOW_READ, 
                    actual_size, 
                    PINT_PERF_ADD);

    flow_data->parent->total_transferred += actual_size;

    if(initial_call_flag)
        flow_data->initial_posts--;
    else
        flow_data->dest_pending--;

#if 0
    gossip_err(
        "initial_posts: %d, dest_pending: %d, dest_last_posted: %d\n", 
        flow_data->initial_posts, flow_data->dest_pending,
        flow_data->dest_last_posted);
#endif

    /* if this was the last operation, then mark the flow as done */
    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
        "(send callback time) ini posts: %d, pending: %d, last: %d, "
        "src_list emtpy: %s\n",
        flow_data->initial_posts, flow_data->dest_pending,
        flow_data->dest_last_posted,
        qlist_empty(&flow_data->src_list) ? "yes" : "no");
    if(flow_data->initial_posts == 0 &&
        flow_data->dest_pending == 0 && 
        flow_data->dest_last_posted &&
        qlist_empty(&flow_data->src_list))
    {
        /* we are in trouble if more than one callback function thinks that
         * it can trigger completion
         */
        assert(q_item->parent->state != FLOW_COMPLETE);
        q_item->parent->state = FLOW_COMPLETE;
        return(1);
    }
 
    /* if we have finished request processing then there is no need to try
     * to continue
     */
    if(flow_data->req_proc_done)
    {
        if(q_item->buffer)
            qlist_del(&q_item->list_link);
        return(0);
    }

    if(q_item->buffer)
    {
        /* if this q_item has been used before, remove it from its 
         * current queue */
        qlist_del(&q_item->list_link);
    }
    else
    {
        /* if the q_item has not been used, allocate a buffer */
        q_item->buffer = BMI_memalloc(
            q_item->parent->dest.u.bmi.address,
            q_item->parent->buffer_size, BMI_SEND);
        /* TODO: error handling */
        assert(q_item->buffer);
        q_item->bmi_callback.fn = bmi_send_callback_wrapper;
    }

    /* add to src queue */
    qlist_add_tail(&q_item->list_link, &flow_data->src_list);

    result_tmp = &q_item->result_chain;
    old_result_tmp = result_tmp;
    tmp_buffer = q_item->buffer;
    q_item->buffer_used = 0;
    do{
        q_item->result_chain_count++;
        if(!result_tmp)
        {
            result_tmp = (struct result_chain_entry*)malloc(
                sizeof(struct result_chain_entry));
            assert(result_tmp);
            memset(result_tmp, 0 , sizeof(struct result_chain_entry));
            old_result_tmp->next = result_tmp;
        }
        /* process request */
        result_tmp->result.offset_array = 
            result_tmp->offset_list;
        result_tmp->result.size_array = 
            result_tmp->size_list;
        result_tmp->result.bytemax = q_item->parent->buffer_size 
            - bytes_processed;
        result_tmp->result.bytes = 0;
        result_tmp->result.segmax = MAX_REGIONS;
        result_tmp->result.segs = 0;
        result_tmp->buffer_offset = tmp_buffer;
        ret = PINT_process_request(q_item->parent->file_req_state,
            q_item->parent->mem_req_state,
            &q_item->parent->file_data,
            &result_tmp->result,
            PINT_SERVER);
        /* TODO: error handling */ 
        assert(ret >= 0);

        if(result_tmp->result.bytes == 0)
        {
            if(result_tmp != &q_item->result_chain)
            {
                free(result_tmp);
                old_result_tmp->next = NULL;
            }
            q_item->result_chain_count--;
        }
        else
        {
            old_result_tmp = result_tmp;
            result_tmp = result_tmp->next;
            tmp_buffer = (void*)
                ((char*)tmp_buffer + old_result_tmp->result.bytes);
            bytes_processed += old_result_tmp->result.bytes;
            q_item->buffer_used += old_result_tmp->result.bytes;
        }

    }while(bytes_processed < flow_data->parent->buffer_size && 
        !PINT_REQUEST_DONE(q_item->parent->file_req_state));

    assert(bytes_processed <= flow_data->parent->buffer_size);

    /* important to update the sequence /after/ request processed */
    q_item->seq = flow_data->next_seq;
    flow_data->next_seq++;

    flow_data->total_bytes_processed += bytes_processed;
    if(PINT_REQUEST_DONE(q_item->parent->file_req_state))
    {
        q_item->last = 1;
        assert(flow_data->req_proc_done == 0);
        flow_data->req_proc_done = 1;
        /* special case, we never have a "last" operation when there
         * is no work to do, trigger manually
         */
        if(flow_data->total_bytes_processed == 0)
        {
            flow_data->initial_posts = 0;
            flow_data->dest_last_posted = 1;
        }
    }

    if(bytes_processed == 0)
    {        
        if(q_item->buffer)
        {
            qlist_del(&q_item->list_link);
        }

        if(flow_data->dest_pending == 0 && qlist_empty(&flow_data->src_list))
        {
            /* we know 2 things: 
             *
             * 1) all the previously posted trove read and
             *    bmi send operations have completed.
             * 2) there aren't any more bytes to process and therefore
             *    no more trove reads to post.
             *
             * based on that we can complete the flow.
             */
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, 
                         "zero bytes processed.  no dests pending. "
                         "setting flow to done\n");
            assert(q_item->parent->state != FLOW_COMPLETE);
            q_item->parent->state = FLOW_COMPLETE;
            return 1;
        }
        else
        {
            /* no more bytes to process but qitems are still being
             * worked on, so we can only set that the last qitem
             * has been posted
             */
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                         "zero bytes processed, dests pending: %d, "
                         "src_list empty: %s\n",
                         flow_data->dest_pending,
                         qlist_empty(&flow_data->src_list) ? "yes" : "no");

            /* this allows a check in the fp_multiqueue_post function
             * to prevent further trying to start other qitems from being
             * posted
             */
            flow_data->initial_posts = 0;
            flow_data->dest_last_posted = 1;
            return 0;
        }
    }

    assert(q_item->buffer_used);

    result_tmp = &q_item->result_chain;
    do{
        assert(q_item->buffer_used);
        assert(result_tmp->result.bytes);
        result_tmp->q_item = q_item;
        result_tmp->trove_callback.data = result_tmp;
        result_tmp->trove_callback.fn = trove_read_callback_wrapper;
        /* XXX: can someone confirm this avoids a segfault in the immediate
         * completion case? */
        tmp_user_ptr = result_tmp;
        assert(result_tmp->result.bytes);

        ret = trove_bstream_read_list(
            q_item->parent->src.u.trove.coll_id,
            q_item->parent->src.u.trove.handle,
            (char**)&result_tmp->buffer_offset,
            &result_tmp->result.bytes,
            1,
            result_tmp->result.offset_array,
            result_tmp->result.size_array,
            result_tmp->result.segs,
            &q_item->out_size,
            0, /* get_data_sync_mode(
                  q_item->parent->dest.u.trove.coll_id), */
            NULL,
            &result_tmp->trove_callback,
            global_trove_context,
            &result_tmp->posted_id,
            flow_data->parent->hints);

        result_tmp = result_tmp->next;

        if(ret < 0)
        {
            gossip_err("%s: I/O error occurred\n", __func__);
            handle_io_error(ret, q_item, flow_data);
            if(flow_data->parent->state == FLOW_COMPLETE)
                return(1);
            else
                return(0);
        }

        if(ret == 1)
        {
            /* immediate completion; trigger callback ourselves */
            trove_read_callback_fn(tmp_user_ptr, 0);
        }
    }while(result_tmp);

    return(0);
};

/* trove_write_callback_fn()
 *
 * function to be called upon completion of a trove write operation
 *
 * no return value
 */
static void trove_write_callback_fn(void *user_ptr,
                           PVFS_error error_code)
{
    PVFS_size tmp_actual_size;
    int ret;
    struct result_chain_entry *result_tmp = user_ptr;
    struct fp_queue_item *q_item = result_tmp->q_item;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    struct result_chain_entry *old_result_tmp;
    void *tmp_buffer;
    PVFS_size bytes_processed = 0;

    gossip_debug(
        GOSSIP_FLOW_PROTO_DEBUG,
        "flowproto-multiqueue trove_write_callback_fn, error_code: %d, flow: %p.\n",
        error_code, flow_data->parent);

    result_tmp->posted_id = 0;

    if(error_code != 0 || flow_data->parent->error_code != 0)
    {
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* don't do anything until the last write completes */
    if(q_item->result_chain_count > 1)
    {
        q_item->result_chain_count--;
        return;
    }

    result_tmp = &q_item->result_chain;
    do{
        q_item->parent->total_transferred += result_tmp->result.bytes;
        PINT_perf_count( PINT_server_pc,
                         PINT_PERF_WRITE, 
                         result_tmp->result.bytes,
                         PINT_PERF_ADD);
        PINT_perf_count( PINT_server_pc,
                         PINT_PERF_FLOW_WRITE, 
                         result_tmp->result.bytes,
                         PINT_PERF_ADD);
        old_result_tmp = result_tmp;
        result_tmp = result_tmp->next;
        if(old_result_tmp != &q_item->result_chain)
            free(old_result_tmp);
    }while(result_tmp);
    q_item->result_chain.next = NULL;
    q_item->result_chain_count = 0;

    /* if this was the last operation, then mark the flow as done */
    if(flow_data->parent->total_transferred ==
        flow_data->total_bytes_processed &&
        PINT_REQUEST_DONE(flow_data->parent->file_req_state))
    {
        /* we are in trouble if more than one callback function thinks that
         * it can trigger completion
         */
        assert(q_item->parent->state != FLOW_COMPLETE);
        q_item->parent->state = FLOW_COMPLETE;
        return;
    }

    /* if there are no more receives to post, just return */
    if(PINT_REQUEST_DONE(flow_data->parent->file_req_state))
    {
        return;
    }

    if(q_item->buffer)
    {
        /* if this q_item has been used before, remove it from its 
         * current queue */
        qlist_del(&q_item->list_link);
    }
    else
    {
        /* if the q_item has not been used, allocate a buffer */
        q_item->buffer = BMI_memalloc(
            q_item->parent->src.u.bmi.address,
            q_item->parent->buffer_size, BMI_RECV);
        /* TODO: error handling */
        assert(q_item->buffer);
        q_item->bmi_callback.fn = bmi_recv_callback_wrapper;
    }

    /* if src list is empty, then post new recv; otherwise just queue
     * in empty list
     */
    if(qlist_empty(&flow_data->src_list))
    {
        /* ready to post new recv! */
        qlist_add_tail(&q_item->list_link, &flow_data->src_list);
        
        result_tmp = &q_item->result_chain;
        old_result_tmp = result_tmp;
        tmp_buffer = q_item->buffer;
        do{
            q_item->result_chain_count++;
            if(!result_tmp)
            {
                result_tmp = (struct result_chain_entry*)malloc(
                    sizeof(struct result_chain_entry));
                assert(result_tmp);
                memset(result_tmp, 0 , sizeof(struct result_chain_entry));
                old_result_tmp->next = result_tmp;
            }
            /* process request */
            result_tmp->result.offset_array = 
                result_tmp->offset_list;
            result_tmp->result.size_array = 
                result_tmp->size_list;
            result_tmp->result.bytemax = flow_data->parent->buffer_size 
                - bytes_processed;
            result_tmp->result.bytes = 0;
            result_tmp->result.segmax = MAX_REGIONS;
            result_tmp->result.segs = 0;
            result_tmp->buffer_offset = tmp_buffer;
            assert(!PINT_REQUEST_DONE(q_item->parent->file_req_state));
            ret = PINT_process_request(q_item->parent->file_req_state,
                q_item->parent->mem_req_state,
                &q_item->parent->file_data,
                &result_tmp->result,
                PINT_SERVER);
            /* TODO: error handling */ 
            assert(ret >= 0);

            if(result_tmp->result.bytes == 0)
            {
                if(result_tmp != &q_item->result_chain)
                {
                    free(result_tmp);
                    old_result_tmp->next = NULL;
                }
                q_item->result_chain_count--;
            }
            else
            {
                old_result_tmp = result_tmp;
                result_tmp = result_tmp->next;
                tmp_buffer = (void*)
                    ((char*)tmp_buffer + old_result_tmp->result.bytes);
                bytes_processed += old_result_tmp->result.bytes;
            }
        }while(bytes_processed < flow_data->parent->buffer_size && 
            !PINT_REQUEST_DONE(q_item->parent->file_req_state));

        assert(bytes_processed <= flow_data->parent->buffer_size);
 
        flow_data->total_bytes_processed += bytes_processed;

        if(bytes_processed == 0)
        {        
            if(flow_data->parent->total_transferred ==
                flow_data->total_bytes_processed &&
                PINT_REQUEST_DONE(flow_data->parent->file_req_state))
            {
                assert(q_item->parent->state != FLOW_COMPLETE);
                q_item->parent->state = FLOW_COMPLETE;
            }
            return;
        }

        gossip_debug(GOSSIP_DIRECTIO_DEBUG,
                     "offset %llu, buffer ptr: %p\n",
                     llu(q_item->result_chain.result.offset_array[0]),
                     q_item->buffer);
        /* TODO: what if we recv less than expected? */
        ret = BMI_post_recv(&q_item->posted_id,
            q_item->parent->src.u.bmi.address,
	    ((char *)q_item->buffer),
            flow_data->parent->buffer_size,
            &tmp_actual_size,
            BMI_PRE_ALLOC,
            q_item->parent->tag,
            &q_item->bmi_callback,
            global_bmi_context,
            (bmi_hint)q_item->parent->hints);

        if(ret < 0)
        {
            gossip_err("%s: I/O error occurred\n", __func__);
            handle_io_error(ret, q_item, flow_data);
            return;
        }

        if(ret == 1)
        {
            /* immediate completion; trigger callback ourselves */
            bmi_recv_callback_fn(q_item, tmp_actual_size, 0);
        }
    }
    else
    {
        qlist_add_tail(&q_item->list_link, 
            &(flow_data->empty_list));
    }

    return;
};
#endif

/* cleanup_buffers()
 *
 * releases any resources consumed during flow processing
 *
 * no return value
 */
static void cleanup_buffers(struct fp_private_data *flow_data)
{
    int i;
    struct result_chain_entry *result_tmp;
    struct result_chain_entry *old_result_tmp;
    flow_descriptor *flow_d = flow_data->parent;

    if(flow_d->src.endpoint_id == BMI_ENDPOINT &&
        (flow_d->dest.endpoint_id == TROVE_ENDPOINT ||
         flow_d->dest.endpoint_id == REPLICATION_ENDPOINT) )
    {
        for(i=0; i<flow_d->buffers_per_flow; i++)
        {
            if(flow_data->prealloc_array[i].buffer)
            {
		    BMI_memfree(flow_d->src.u.bmi.address,
				    flow_data->prealloc_array[i].buffer,
				    flow_d->buffer_size,
				    BMI_RECV);
            }
            result_tmp = &(flow_data->prealloc_array[i].result_chain);
            do{
                old_result_tmp = result_tmp;
                result_tmp = result_tmp->next;
                if(old_result_tmp !=
                    &(flow_data->prealloc_array[i].result_chain))
                    free(old_result_tmp);
            }while(result_tmp);
            flow_data->prealloc_array[i].result_chain.next = NULL;
        }
    }
    else if(flow_data->parent->src.endpoint_id == TROVE_ENDPOINT &&
        flow_data->parent->dest.endpoint_id == BMI_ENDPOINT)
    {
        for(i=0; i<flow_data->parent->buffers_per_flow; i++)
        {
            if(flow_data->prealloc_array[i].buffer)
            {
                BMI_memfree(flow_data->parent->dest.u.bmi.address,
                    flow_data->prealloc_array[i].buffer,
                    flow_data->parent->buffer_size,
                    BMI_SEND);
            }
            result_tmp = &(flow_data->prealloc_array[i].result_chain);
            do{
                old_result_tmp = result_tmp;
                result_tmp = result_tmp->next;
                if(old_result_tmp !=
                    &(flow_data->prealloc_array[i].result_chain))
                    free(old_result_tmp);
            }while(result_tmp);
            flow_data->prealloc_array[i].result_chain.next = NULL;
        }
    }
    else if(flow_data->parent->src.endpoint_id == MEM_ENDPOINT &&
        flow_data->parent->dest.endpoint_id == BMI_ENDPOINT)
    {
        if(flow_data->intermediate)
        {
            BMI_memfree(flow_data->parent->dest.u.bmi.address,
                flow_data->intermediate, flow_data->parent->buffer_size, BMI_SEND);
        }
    }
    else if(flow_data->parent->src.endpoint_id == BMI_ENDPOINT &&
        flow_data->parent->dest.endpoint_id == MEM_ENDPOINT)
    {
        if(flow_data->intermediate)
        {
            BMI_memfree(flow_data->parent->src.u.bmi.address,
                flow_data->intermediate, flow_data->parent->buffer_size, BMI_RECV);
        }
    }

    free(flow_data->prealloc_array);
}

/* mem_to_bmi_callback()
 *
 * function to be called upon completion of bmi operations in memory to
 * bmi transfers
 * 
 * no return value
 */
static void mem_to_bmi_callback_fn(void *user_ptr,
                                   PVFS_size actual_size,
                                   PVFS_error error_code)
{
    struct fp_queue_item *q_item = user_ptr;
    int ret;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    int i;
    PVFS_size bytes_processed = 0;
    char *src_ptr, *dest_ptr;
    enum bmi_buffer_type buffer_type = BMI_EXT_ALLOC;

    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
        "flowproto-multiqueue mem_to_bmi_callback_fn, error_code: %d, flow: %p.\n",
        error_code, flow_data->parent);

    q_item->posted_id = 0;

    if(error_code != 0 || flow_data->parent->error_code != 0)
    {
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* remove from current queue, empty or earlier send; add bmi active dest */
    qlist_del(&q_item->list_link);
    qlist_add_tail(&q_item->list_link, &flow_data->dest_list);

    flow_data->parent->total_transferred += actual_size;

    /* are we done? */
    if(PINT_REQUEST_DONE(q_item->parent->file_req_state))
    {
        /* we are in trouble if more than one callback function thinks that
         * it can trigger completion
         */
        assert(q_item->parent->state != FLOW_COMPLETE);
        q_item->parent->state = FLOW_COMPLETE;
        return;
    }

    /* process request */
    q_item->result_chain.result.offset_array = 
        q_item->result_chain.offset_list;
    q_item->result_chain.result.size_array = 
        q_item->result_chain.size_list;
    q_item->result_chain.result.bytemax = flow_data->parent->buffer_size;
    q_item->result_chain.result.bytes = 0;
    q_item->result_chain.result.segmax = MAX_REGIONS;
    q_item->result_chain.result.segs = 0;
    q_item->result_chain.buffer_offset = NULL;
    ret = PINT_process_request(q_item->parent->file_req_state,
        q_item->parent->mem_req_state,
        &q_item->parent->file_data,
        &q_item->result_chain.result,
        PINT_CLIENT);

    /* TODO: error handling */ 
    assert(ret >= 0);

    /* was MAX_REGIONS enough to satisfy this step? */
    if(!PINT_REQUEST_DONE(flow_data->parent->file_req_state) &&
        q_item->result_chain.result.bytes < flow_data->parent->buffer_size)
    {
        /* create an intermediate buffer */
        if(!flow_data->intermediate)
        {
            flow_data->intermediate = BMI_memalloc(
                flow_data->parent->dest.u.bmi.address,
                flow_data->parent->buffer_size,
                BMI_SEND);
            /* TODO: error handling */
            assert(flow_data->intermediate);
        }

        /* copy what we have so far into intermediate buffer */
        for(i=0; i<q_item->result_chain.result.segs; i++)
        {
            src_ptr = ((char*)q_item->parent->src.u.mem.buffer + 
                q_item->result_chain.offset_list[i]);
            dest_ptr = ((char*)flow_data->intermediate + bytes_processed);
            memcpy(dest_ptr, src_ptr, q_item->result_chain.size_list[i]);
            bytes_processed += q_item->result_chain.size_list[i];
        }

        do
        {
            q_item->result_chain.result.bytemax =
                (flow_data->parent->buffer_size - bytes_processed);
            q_item->result_chain.result.bytes = 0;
            q_item->result_chain.result.segmax = MAX_REGIONS;
            q_item->result_chain.result.segs = 0;
            q_item->result_chain.buffer_offset = NULL;
            /* process ahead */
            ret = PINT_process_request(q_item->parent->file_req_state,
                q_item->parent->mem_req_state,
                &q_item->parent->file_data,
                &q_item->result_chain.result,
                PINT_CLIENT);
            /* TODO: error handling */
            assert(ret >= 0);

            /* copy what we have so far into intermediate buffer */
            for(i=0; i<q_item->result_chain.result.segs; i++)
            {
                src_ptr = ((char*)q_item->parent->src.u.mem.buffer + 
                    q_item->result_chain.offset_list[i]);
                dest_ptr = ((char*)flow_data->intermediate + bytes_processed);
                memcpy(dest_ptr, src_ptr, q_item->result_chain.size_list[i]);
                bytes_processed += q_item->result_chain.size_list[i];
            }
        }while(bytes_processed < flow_data->parent->buffer_size &&
            !PINT_REQUEST_DONE(q_item->parent->file_req_state));

        assert (bytes_processed <= flow_data->parent->buffer_size);

        /* setup for BMI operation */
        flow_data->tmp_buffer_list[0] = flow_data->intermediate;
        q_item->result_chain.result.size_array[0] = bytes_processed;
        q_item->result_chain.result.bytes = bytes_processed;
        q_item->result_chain.result.segs = 1;
        buffer_type = BMI_PRE_ALLOC;
    }
    else
    {
        /* go ahead and return if there is nothing to do */
        if(q_item->result_chain.result.bytes == 0)
        {        
            /* we are in trouble if more than one callback function thinks that
             * it can trigger completion
             */
            assert(q_item->parent->state != FLOW_COMPLETE);
            q_item->parent->state = FLOW_COMPLETE;
            return;
        }

        /* convert offsets to memory addresses */
        for(i=0; i<q_item->result_chain.result.segs; i++)
        {
            flow_data->tmp_buffer_list[i] = 
                (char*)(q_item->result_chain.result.offset_array[i] +
                (char *)q_item->buffer);
        }
    }

    assert(q_item->result_chain.result.bytes);

    ret = BMI_post_send_list(&q_item->posted_id,
        q_item->parent->dest.u.bmi.address,
        (const void**)flow_data->tmp_buffer_list,
        q_item->result_chain.result.size_array,
        q_item->result_chain.result.segs,
        q_item->result_chain.result.bytes,
        buffer_type,
        q_item->parent->tag,
        &q_item->bmi_callback,
        global_bmi_context,
        (bmi_hint)q_item->parent->hints);

    if(ret < 0)
    {
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(ret, q_item, flow_data);
        return;
    }

    if(ret == 1)
    {
        mem_to_bmi_callback_fn(q_item, 
            q_item->result_chain.result.bytes, 0);
    }
}


/* bmi_to_mem_callback()
 *
 * function to be called upon completion of bmi operations in bmi to
 * memory transfers
 * 
 * no return value
 */
static void bmi_to_mem_callback_fn(void *user_ptr,
                                   PVFS_size actual_size,
                                   PVFS_error error_code)
{
    struct fp_queue_item *q_item = user_ptr;
    int ret;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    int i;
    PVFS_size tmp_actual_size;
    PVFS_size *size_array;
    int segs;
    PVFS_size total_size;
    enum bmi_buffer_type buffer_type = BMI_EXT_ALLOC;
    PVFS_size bytes_processed = 0;
    char *src_ptr, *dest_ptr;
    PVFS_size region_size;

    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
        "flowproto-multiqueue bmi_to_mem_callback_fn, error_code: %d, flow: %p.\n",
        error_code, flow_data->parent);

    q_item->posted_id = 0;

    if(error_code != 0 || flow_data->parent->error_code != 0)
    {
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* remove from current queue, empty or earlier send; add bmi active src */
    qlist_del(&q_item->list_link);
    qlist_add_tail(&q_item->list_link, &flow_data->src_list);

    flow_data->parent->total_transferred += actual_size;

    /* if this is the result of a receive into an intermediate buffer,
     * then we must copy out */
    if(flow_data->tmp_buffer_list[0] == flow_data->intermediate &&
        flow_data->intermediate != NULL)
    {
        /* copy out what we have so far */
        for(i=0; i<q_item->result_chain.result.segs; i++)
        {
            region_size = q_item->result_chain.size_list[i];
            src_ptr = (char*)((char *)flow_data->intermediate + 
                bytes_processed);
            dest_ptr = (char*)(q_item->result_chain.offset_list[i]
                + (char *)q_item->parent->dest.u.mem.buffer);
            memcpy(dest_ptr, src_ptr, region_size);
            bytes_processed += region_size;
        }

        do
        {
            q_item->result_chain.result.bytemax =
                (q_item->parent->buffer_size - bytes_processed);
            q_item->result_chain.result.bytes = 0;
            q_item->result_chain.result.segmax = MAX_REGIONS;
            q_item->result_chain.result.segs = 0;
            q_item->result_chain.buffer_offset = NULL;
            /* process ahead */
            ret = PINT_process_request(q_item->parent->file_req_state,
                q_item->parent->mem_req_state,
                &q_item->parent->file_data,
                &q_item->result_chain.result,
                PINT_CLIENT);
            /* TODO: error handling */
            assert(ret >= 0);
            /* copy out what we have so far */
            for(i=0; i<q_item->result_chain.result.segs; i++)
            {
                region_size = q_item->result_chain.size_list[i];
                src_ptr = (char*)((char *)flow_data->intermediate + 
                    bytes_processed);
                dest_ptr = (char*)(q_item->result_chain.offset_list[i]
                    + (char *)q_item->parent->dest.u.mem.buffer);
                memcpy(dest_ptr, src_ptr, region_size);
                bytes_processed += region_size;
            }
        }while(bytes_processed < flow_data->parent->buffer_size &&
            !PINT_REQUEST_DONE(q_item->parent->file_req_state));

        assert(bytes_processed <= flow_data->parent->buffer_size);
    }

    /* are we done? */
    if(PINT_REQUEST_DONE(q_item->parent->file_req_state))
    {
        /* we are in trouble if more than one callback function thinks
         * that it can trigger completion
         */
        assert(q_item->parent->state != FLOW_COMPLETE);
        q_item->parent->state = FLOW_COMPLETE;
        return;
    }

    /* process request */
    q_item->result_chain.result.offset_array = 
        q_item->result_chain.offset_list;
    q_item->result_chain.result.size_array = 
        q_item->result_chain.size_list;
    q_item->result_chain.result.bytemax = flow_data->parent->buffer_size;
    q_item->result_chain.result.bytes = 0;
    q_item->result_chain.result.segmax = MAX_REGIONS;
    q_item->result_chain.result.segs = 0;
    q_item->result_chain.buffer_offset = NULL;
    ret = PINT_process_request(q_item->parent->file_req_state,
        q_item->parent->mem_req_state,
        &q_item->parent->file_data,
        &q_item->result_chain.result,
        PINT_CLIENT);
    /* TODO: error handling */ 
    assert(ret >= 0);

    /* was MAX_REGIONS enough to satisfy this step? */
    if(!PINT_REQUEST_DONE(flow_data->parent->file_req_state) &&
        q_item->result_chain.result.bytes < flow_data->parent->buffer_size)
    {
        /* create an intermediate buffer */
        if(!flow_data->intermediate)
        {
            flow_data->intermediate = BMI_memalloc(
                flow_data->parent->src.u.bmi.address,
                flow_data->parent->buffer_size,
                BMI_RECV);
            /* TODO: error handling */
            assert(flow_data->intermediate);
        }
        /* setup for BMI operation */
        flow_data->tmp_buffer_list[0] = flow_data->intermediate;
        buffer_type = BMI_PRE_ALLOC;
        q_item->buffer_used = flow_data->parent->buffer_size;
        total_size = flow_data->parent->buffer_size;
        size_array = &q_item->buffer_used;
        segs = 1;
        /* we will copy data out on next iteration */
    }
    else
    {
        /* normal case */
        segs = q_item->result_chain.result.segs;
        size_array = q_item->result_chain.result.size_array;
        total_size = q_item->result_chain.result.bytes;

        /* convert offsets to memory addresses */
        for(i=0; i<q_item->result_chain.result.segs; i++)
        {
            flow_data->tmp_buffer_list[i] = 
                (void*)(q_item->result_chain.result.offset_array[i] +
                (char *)q_item->buffer);
        }

        /* go ahead and return if there is nothing to do */
        if(q_item->result_chain.result.bytes == 0)
        {        
            /* we are in trouble if more than one callback function
             * thinks that it can trigger completion
             */
            assert(q_item->parent->state != FLOW_COMPLETE);
            q_item->parent->state = FLOW_COMPLETE;
            return;
        }
    }

    assert(total_size);
    ret = BMI_post_recv_list(&q_item->posted_id,
        q_item->parent->src.u.bmi.address,
        flow_data->tmp_buffer_list,
        size_array,
        segs,
        total_size,
        &tmp_actual_size,
        buffer_type,
        q_item->parent->tag,
        &q_item->bmi_callback,
        global_bmi_context,
        (bmi_hint)q_item->parent->hints);

    if(ret < 0)
    {
        gossip_err("%s: I/O error occurred\n", __func__);
        handle_io_error(ret, q_item, flow_data);
        return;
    }

    if(ret == 1)
    {
        bmi_to_mem_callback_fn(q_item, tmp_actual_size, 0);
    }

    return;
}


/* handle_io_error()
 * 
 * called any time a BMI or Trove error code is detected, responsible
 * for safely cleaning up the associated flow
 *
 * NOTE: this function should always be called while holding the flow mutex!
 *
 * no return value
 */
static void handle_io_error(
    PVFS_error error_code,
    struct fp_queue_item *q_item,
    struct fp_private_data *flow_data)
{
    int ret;
    char buf[64] = {0};

    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, 
        "flowproto-multiqueue handle_io_error() called for flow %p.\n",
        flow_data->parent);

    /* is this the first error registered for this particular flow? */
    if(flow_data->parent->error_code == 0)
    {
        enum flow_endpoint_type src, dest;

        PVFS_strerror_r(error_code, buf, 64);
        gossip_err("%s: flow proto error cleanup started on %p: %s\n", __func__, flow_data->parent, buf);

        flow_data->parent->error_code = error_code;
        if(q_item)
        {
            qlist_del(&q_item->list_link);
        }
        flow_data->cleanup_pending_count = 0;

        src = flow_data->parent->src.endpoint_id;
        dest = flow_data->parent->dest.endpoint_id;

        /* cleanup depending on what endpoints are in use */
        if (src == BMI_ENDPOINT && dest == MEM_ENDPOINT)
        {
            ret = cancel_pending_bmi(&flow_data->src_list);
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "flowproto-multiqueue canceled %d bmi-mem BMI ops.\n", ret);
            flow_data->cleanup_pending_count += ret;
        }
        else if (src == MEM_ENDPOINT && dest == BMI_ENDPOINT)
        {
            ret = cancel_pending_bmi(&flow_data->dest_list);
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "flowproto-multiqueue canceled %d mem-bmi BMI ops.\n", ret);
            flow_data->cleanup_pending_count += ret;
        }
        else if (src == TROVE_ENDPOINT && dest == BMI_ENDPOINT)
        {
            ret = cancel_pending_trove(&flow_data->src_list, flow_data->parent->src.u.trove.coll_id);
            flow_data->cleanup_pending_count += ret;
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "flowproto-multiqueue canceled %d trove-bmi Trove ops.\n", ret);
            ret = cancel_pending_bmi(&flow_data->dest_list);
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "flowproto-multiqueue canceled %d trove-bmi BMI ops.\n", ret);
            flow_data->cleanup_pending_count += ret;
        }
        else if (src == BMI_ENDPOINT && dest == TROVE_ENDPOINT)
        {
            ret = cancel_pending_bmi(&flow_data->src_list);
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "flowproto-multiqueue canceled %d bmi-trove BMI ops.\n", ret);
            flow_data->cleanup_pending_count += ret;
            ret = cancel_pending_trove(&flow_data->dest_list, flow_data->parent->dest.u.trove.coll_id);
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "flowproto-multiqueue canceled %d bmi-trove Trove ops.\n", ret);
            flow_data->cleanup_pending_count += ret;
        }
        else
        {
            /* impossible condition */
            assert(0);
        }
        gossip_err("%s: flow proto %p canceled %d operations, will clean up.\n",
                   __func__, flow_data->parent,
                   flow_data->cleanup_pending_count);
    }
    else
    {
        /* one of the previous cancels came through */
        flow_data->cleanup_pending_count--;
    }
    
    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, 
        "flowproto-multiqueue handle_io_error() pending count: %d\n",
        flow_data->cleanup_pending_count);

    if(flow_data->cleanup_pending_count == 0)
    {
        PVFS_strerror_r(flow_data->parent->error_code, buf, 64);
        gossip_err("%s: flow proto %p error cleanup finished: %s\n",
            __func__, flow_data->parent, buf);

        /* we are finished, make sure error is marked and state is set */
        assert(flow_data->parent->error_code);
        /* we are in trouble if more than one callback function thinks that
         * it can trigger completion
         */
        assert(flow_data->parent->state != FLOW_COMPLETE);
        flow_data->parent->state = FLOW_COMPLETE;
    }
}


/* cancel_pending_bmi()
 *
 * cancels any pending bmi operations on the given queue list
 *
 * returns the number of operations that were canceled 
 */
static int cancel_pending_bmi(struct qlist_head *list)
{
    struct qlist_head *tmp_link;
    struct fp_queue_item *q_item = NULL;
    int ret = 0;
    int count = 0;

    /* run down the chain of pending operations */
    qlist_for_each(tmp_link, list)
    {
        q_item = qlist_entry(tmp_link, struct fp_queue_item,
            list_link);
        /* skip anything that is in the queue but not actually posted */
        if(q_item->posted_id)
        {
            count++;
            gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                "flowprotocol cleanup: unposting BMI operation.\n");
            ret = PINT_thread_mgr_bmi_cancel(q_item->posted_id,
                &q_item->bmi_callback);
            if(ret < 0)
            {
                gossip_err("WARNING: BMI thread mgr cancel failed, "
                           "proceeding anyway.\n");
            }
        }
    }
    return (count);
}

/* cancel_pending_trove()
 *
 * cancels any pending trove operations on the given queue list
 *
 * returns the number of operations that were canceled 
 */
static int cancel_pending_trove(struct qlist_head *list, TROVE_coll_id coll_id)
{
    struct qlist_head *tmp_link;
    struct fp_queue_item *q_item = NULL;
    int count = 0;
    struct result_chain_entry *result_tmp;
    struct result_chain_entry *old_result_tmp;
    int ret;

    /* run down the chain of pending operations */
    qlist_for_each(tmp_link, list)
    {
        q_item = qlist_entry(tmp_link, struct fp_queue_item,
            list_link);

        result_tmp = &q_item->result_chain;
        do{
            old_result_tmp = result_tmp;
            result_tmp = result_tmp->next;

            if(old_result_tmp->posted_id)
            {
                count++;
                ret = PINT_thread_mgr_trove_cancel(
                    old_result_tmp->posted_id,
                    coll_id,
                    &old_result_tmp->trove_callback);
                if(ret < 0)
                {
                    gossip_err("WARNING: Trove thread mgr cancel "
                               "failed, proceeding anyway.\n");
                }
            }
        }while(result_tmp);
    }
    return (count);
}

#ifdef __PVFS2_TROVE_SUPPORT__
static int get_data_sync_mode(TROVE_coll_id coll_id)
{
    int mode = TROVE_SYNC;
    id_sync_mode_t *cur_info = NULL;
    struct qlist_head *tmp_link = NULL;

    gen_mutex_lock(&id_sync_mode_mutex);
    qlist_for_each(tmp_link, &s_id_sync_mode_list)
    {
        cur_info = qlist_entry(tmp_link, id_sync_mode_t, link);
        if (cur_info->coll_id == coll_id)
        {
            mode = cur_info->sync_mode;
            break;
        }
    }
    gen_mutex_unlock(&id_sync_mode_mutex);
    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, "get_data_sync_mode "
                 "returning %d\n", mode);
    return mode;
}
#endif

static inline void server_write_flow_post_init(flow_descriptor *flow_d,
                                               struct fp_private_data *flow_data)
{
    int i;

    /* Generic flow initialization */
    flow_data->parent->total_transferred = 0;
    
    /* Iniitialize the pending counts */
    flow_data->recvs_pending = 0;
    flow_data->writes_pending = 0;
    flow_data->primary_recvs_throttled = 0;

    /* Initiailize progress counts */
    flow_data->total_bytes_req = flow_d->aggregate_size;
    flow_data->total_bytes_forwarded = 0;
    flow_data->total_bytes_recvd = 0;
    flow_data->total_bytes_written = 0;
    
    gen_mutex_lock(&flow_d->flow_mutex);

    /* Initialize buffers */
    for (i = 0; i < flow_d->buffers_per_flow; i++)
    {
        /* Trove stuff I don't understand */
        flow_data->prealloc_array[i].result_chain.q_item = 
            &flow_data->prealloc_array[i];

        /* Place available buffers on the empty list */
        qlist_add_tail(&flow_data->prealloc_array[i].list_link,
                       &flow_data->empty_list);

    }

    /* Post the initial receives */
    for (i = 0; i < flow_d->buffers_per_flow; i++)
    {
        /* If there is data to be received, perform the initial recv
           otherwise mark the flow complete */
        if (!PINT_REQUEST_DONE(flow_data->parent->file_req_state))
        {
            /* Remove the buffer from the available list */
            qlist_del(&(flow_data->prealloc_array[i].list_link));

            /* Post the recv operation */
            flow_data->recvs_pending += 1;
            flow_bmi_recv(&(flow_data->prealloc_array[i]),
                          server_bmi_recv_callback_wrapper,
                          server_bmi_recv_callback_fn);
        }
        else
        {
            gossip_lerr("Server flow posted all buffers on initial post.\n");
            break;
        }
    }

    /* If the flow is complete, perform cleanup */
    if(flow_d->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_d->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_d->flow_mutex);
    }
}/*end server_write_flow_post_init*/




static void flow_bmi_recv(struct fp_queue_item* q_item,
                          bmi_recv_callback recv_callback_wrapper,
                          bmi_recv_callback recv_callback)
{
    gossip_err("Executing %s...\n",__func__);
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    flow_descriptor *flow_d = q_item->parent;
    PVFS_size tmp_actual_size;
    PVFS_size bytes_processed = 0;
    int ret;

    /* Create rest of qitem so that we can recv into it */
    if (0 == q_item->buffer)
    {
	q_item->buffer = BMI_memalloc(flow_d->src.u.bmi.address,
				      flow_d->buffer_size,
				      BMI_RECV);
    }
    assert(q_item->buffer);
    q_item->bmi_callback.fn = recv_callback_wrapper;
    q_item->posted_id = 0;
    
    /* Add qitem to src_list */
    qlist_del(&q_item->list_link);
    qlist_add_tail(&q_item->list_link, &flow_data->src_list);

    /* Process the request to determine mapping */
    bytes_processed = flow_process_request(q_item);

    if (0 != bytes_processed)
    {
    
        /* TODO: what if we recv less than expected? */
        ret = BMI_post_recv(&q_item->posted_id,
                            flow_d->src.u.bmi.address,
                            q_item->buffer,
                            flow_d->buffer_size,
                            &tmp_actual_size,
                            BMI_PRE_ALLOC,
                            flow_d->tag,
                            &q_item->bmi_callback,
                            global_bmi_context,
                            NULL);

        /* If there is an error on recv, handle it
           else if the recv completes immediately, trigger callback */
        if (ret < 0)
        {
            gossip_lerr("ERROR: BMI_post_recv returned: %d!\n", ret);
            handle_io_error(ret, q_item, flow_data);
            return;
        }
        else if (ret == 1)
        {
            recv_callback(q_item, tmp_actual_size, 0);
        }
    }
    else
    {
        gossip_lerr("ERROR: Zero size request processed!!??\n");
    }
} /*end flow_bmi_recv*/



static inline void server_bmi_recv_callback_wrapper(void *user_ptr,
                                                    PVFS_size actual_size,
                                                    PVFS_error error_code)
{
    struct fp_private_data *flow_data = PRIVATE_FLOW(((struct fp_queue_item*)user_ptr)->parent);
    gen_mutex_lock(&flow_data->parent->flow_mutex);
    server_bmi_recv_callback_fn(user_ptr, actual_size, error_code);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}/*end server_bmi_recv_callback_wrapper*/



/* server_bmi_recv_callback_fn()
 *
 * Callback invoked when a BMI recv operation completes
 * no return value
 */
static void server_bmi_recv_callback_fn(void *user_ptr,
                                        PVFS_size actual_size,
                                        PVFS_error error_code)
{
    gossip_err("Executing %s...\n",__func__);
    struct fp_queue_item *q_item = user_ptr;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    flow_descriptor *flow_d = q_item->parent;

    /* Handle errors from recv */
    if(error_code != 0 || flow_d->error_code != 0)
    {
        gossip_lerr("ERROR occured on recv: %d\n", error_code);
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* Decrement recv pending count */
    flow_data->recvs_pending -= 1;
    flow_data->total_bytes_recvd += actual_size;
    
    /* Remove from current queue */
    qlist_del(&q_item->list_link);

    /* Debug output */
    gossip_lerr("SERVER RECV Callback: Total: %lld AmtRecvd: %lld PendingRecvs: %d Throttled: %d\n",
                 (long long int)flow_data->total_bytes_req,
                 (long long int)flow_data->total_bytes_recvd,
                 flow_data->recvs_pending,
                 flow_data->primary_recvs_throttled);
    
    /* Write the data to trove */
    flow_data->writes_pending += 1;
    flow_trove_write(q_item,
                     actual_size,
                     server_trove_write_callback_wrapper,
                     server_trove_write_callback_fn);
    
    /* If there is more data to recv and a recv buffer exists,
       perform the next recv */
    if (!PINT_REQUEST_DONE(flow_d->file_req_state))
    {
        if (!qlist_empty(&flow_data->empty_list))
        {
            struct fp_queue_item* next_q_item = 0;
        
            /* Setup the next recv buffer */
            next_q_item = qlist_entry(flow_data->empty_list.next,
                                      struct fp_queue_item,
                                      list_link);
            
            /* Post next recv operation */
            flow_data->recvs_pending += 1;
            flow_bmi_recv(next_q_item,
                          server_bmi_recv_callback_wrapper,
                          server_bmi_recv_callback_fn);
        }
        else
        {
            flow_data->primary_recvs_throttled += 1;
        }
    }
    
    /* Hack to end flows when all data is received */
    /*if (0 == flow_data->recvs_pending &&
        PINT_REQUEST_DONE(flow_data->parent->file_req_state))
    {
        fprintf(stderr, "Marking Server Recv flow finished.\n");
        assert(flow_data->parent->state != FLOW_COMPLETE);
        flow_data->parent->state = FLOW_COMPLETE;
    }
    */
}/*end server_bmi_recv_callback_fn*/



/**
 * Performs a trove write on data for a forwarding flow
 *
 * Remove q_item from current list
 * Adds q_item to dest_list
 * Sends data
 * Calls forwarding_bmi_send_callback on success
 *
 */
static void flow_trove_write(struct fp_queue_item* q_item,
                             PVFS_size actual_size,
                             trove_write_callback write_callback_wrapper,
                             trove_write_callback write_callback)
{
    gossip_err("Executing %s...\n",__func__);
    struct fp_private_data* flow_data = PRIVATE_FLOW(q_item->parent);
    flow_descriptor *flow_d = q_item->parent;
    struct result_chain_entry* result_iter = 0;
    int data_sync_mode=0;
    int rc = 0;

    /* Add qitem to dest_list */
    qlist_del(&q_item->list_link);
    qlist_add_tail(&q_item->list_link, &flow_data->dest_list);

#ifdef __PVFS2_TROVE_SUPPORT__
    /* Retrieve the data sync mode */
    data_sync_mode = get_data_sync_mode(flow_d->dest.u.trove.coll_id);
#endif

    gossip_lerr("data sync mode (%d)\n",data_sync_mode);


    /* Perform a write to disk */
    q_item->result_chain_count = 0;
    result_iter = &q_item->result_chain;
    assert(result_iter);
    q_item->out_size=0; /* just to be sure */
    
    while (0 != result_iter)
    {
        /* Construct trove data structure */
        assert(0 != result_iter->result.bytes);
        result_iter->q_item = q_item;
        result_iter->trove_callback.data = result_iter;
        result_iter->trove_callback.fn = write_callback_wrapper;

        rc = trove_bstream_write_list(flow_d->dest.u.trove.coll_id,
                                      flow_d->dest.u.trove.handle,
                                      (char**)&result_iter->buffer_offset,
                                      &result_iter->result.bytes,
                                      1,
                                      result_iter->result.offset_array,
                                      result_iter->result.size_array,
                                      result_iter->result.segs,
                                      //&result_iter->result.bytes,
                                      &q_item->out_size,
                                      data_sync_mode,
                                      NULL,
                                      &result_iter->trove_callback,
                                      global_trove_context,
                                      &result_iter->posted_id,
                                      NULL);
        
        /* if an error occurs, handle it
           else if immediate completion, trigger callback */
        if (rc < 0)
        {
            gossip_lerr("ERROR: Trove Write CATASTROPHIC FAILURE\n");
            handle_io_error(rc, q_item, flow_data);    
        }
        else if (1 == rc)
        {
            write_callback(result_iter, 0);
        }

        /* Increment iterator */
        result_iter = result_iter->next;
        q_item->result_chain_count++;
    };

} /*end flow_trove_write*/


static void forwarding_trove_write_callback_wrapper(void *user_ptr,
                                                    PVFS_error error_code)
{
    struct fp_private_data *flow_data = PRIVATE_FLOW(((struct result_chain_entry*)user_ptr)->q_item->parent);

    gen_mutex_lock(&flow_data->parent->flow_mutex);
    
    forwarding_trove_write_callback_fn(user_ptr, error_code);

    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}/*end forwarding_trove_write_callback_wrapper*/


/**
 * Callback invoked upon completion of a trove write operation
 */
static void forwarding_trove_write_callback_fn(void *user_ptr,
					       PVFS_error error_code)
{
    gossip_err("Executing %s...\n",__func__);

    struct result_chain_entry* result_entry = user_ptr;
    struct fp_queue_item *q_item = result_entry->q_item;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    flow_descriptor *flow_d = q_item->parent;

    /* Handle trove errors */
    if(error_code != 0 || flow_d->error_code != 0)
    {
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* Decrement result chain count */
    q_item->result_chain_count--;
    result_entry->posted_id = 0;

    /* If all results for this qitem are available continue */
    if (0 == q_item->result_chain_count)
    {
        struct result_chain_entry* result_iter = &q_item->result_chain;

        /* Decrement the number of pending writes */
        flow_data->writes_pending--;

        /* Aggregate results */
        while (0 != result_iter)
        {
            struct result_chain_entry* re = result_iter;
            gossip_lerr("total-bytes-written(%d) \tq_item->out_size(%d)\n",(int)flow_data->total_bytes_written
                                                                      ,(int)q_item->out_size);
            flow_data->total_bytes_written += result_iter->result.bytes;
            flow_d->total_transferred += result_iter->result.bytes;
            PINT_perf_count(PINT_server_pc, 
                            PINT_PERF_WRITE,
                            result_iter->result.bytes, 
                            PINT_PERF_ADD);
            result_iter = result_iter->next;

            /* Free memory if this is not the chain head */
            if (re != &q_item->result_chain)
                free(re);
        }

        /* Debug output */
        gossip_lerr(
         "FORWARDING-TROVE-WRITE-FINISHED: Total: %lld TotalAmtWritten: %lld AmtWritten: %lld PendingWrites: %d Throttled: %d\n",
            (long long int)flow_data->total_bytes_req,
            (long long int)flow_data->total_bytes_written,
            (long long int)q_item->out_size,
            flow_data->writes_pending,
            flow_data->primary_recvs_throttled);
    
        /* Cleanup q_item memory */
        q_item->result_chain.next = NULL;
        q_item->result_chain_count = 0;

        /* Remove q_item from in use list */
        qlist_del(&q_item->list_link);

        /* Determine if the flow is complete */
        if (forwarding_is_flow_complete(flow_data))
        {
            gossip_lerr("Server Write flow finished\n");
            assert(flow_data->total_bytes_recvd ==
                   flow_data->total_bytes_written);
            assert(flow_d->state != FLOW_COMPLETE);
            flow_d->state = FLOW_COMPLETE;
        }

        /* If there are recvs to go and stalling has occurred,
           start another recv */
        if (!PINT_REQUEST_DONE(flow_d->file_req_state))
        {
            /* Post another recv operation */
            gossip_lerr("Starting recv from write callback.\n");
            flow_data->primary_recvs_throttled -= 1;
            flow_data->recvs_pending += 1;
            flow_bmi_recv(q_item,
                          forwarding_bmi_recv_callback_wrapper,
                          forwarding_bmi_recv_callback_fn);
        }
        else
        {
            qlist_add_tail(&q_item->list_link, &flow_data->empty_list);
        }
    }/*end if*/  
  
}/*end forwarding_trove_write_callback_fn*/


/**
 * Marks the forwarding flow as complete when finished
 * Return 1 when the flow is complete, otherwise returns 0
 */
int forwarding_is_flow_complete(struct fp_private_data* flow_data)
{
    int is_flow_complete = 0;
    flow_descriptor *flow_d = flow_data->parent;
    
    /* If there are no more recvs, check for completion
       else if there are recvs to go, start one if possible */
    if (PINT_REQUEST_DONE(flow_data->parent->file_req_state))
    {
        /* If all data operations are complete */
        if (0 == flow_data->sends_pending &&
            0 == flow_data->recvs_pending &&
            0 == flow_data->writes_pending)
        {
            gossip_lerr("Forwarding flow finished\n");
            gossip_lerr("flow_data->total_bytes_recvd(%d) \ttotal_bytes_forwarded(%d) \tNumber of Copies(%d) "
                        "\tbytes_per_server(%d)\n"
                       ,(int)flow_data->total_bytes_recvd
                       ,(int)flow_data->total_bytes_forwarded
                       ,(int)flow_d->next_dest_count
                       ,(int)flow_data->total_bytes_forwarded/(int)flow_d->next_dest_count);
            gossip_lerr("flow_data->total_bytes_recv(%d) \ttotal_bytes_written(%d)\n"
                       ,(int)flow_data->total_bytes_recvd,(int)flow_data->total_bytes_written);
            assert(flow_data->total_bytes_recvd ==
                   ((int)flow_data->total_bytes_forwarded/(int)flow_d->next_dest_count));
            assert(flow_data->total_bytes_recvd ==
                   flow_data->total_bytes_written);
            is_flow_complete = 1;
        }
    }
    return is_flow_complete;
}/*end forwarding_is_flow_complete*/



static inline void forwarding_bmi_recv_callback_wrapper(void *user_ptr,
							PVFS_size actual_size,
							PVFS_error error_code)
{
    struct fp_private_data *flow_data = PRIVATE_FLOW(((struct fp_queue_item*)user_ptr)->parent);
    gen_mutex_lock(&flow_data->parent->flow_mutex);
    forwarding_bmi_recv_callback_fn(user_ptr, actual_size, error_code);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}/*end forwarding_bmi_recv_callback_wrapper*/


/* forwarding_bmi_recv_callback_fn()
 *
 * Callback invoked when a BMI recv operation completes
 * no return value
 */
static void forwarding_bmi_recv_callback_fn(void *user_ptr,
					    PVFS_size actual_size,
					    PVFS_error error_code)
{
    gossip_err("Executing %s...\n",__func__);
    struct fp_queue_item *q_item = user_ptr;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    flow_descriptor *flow_d = flow_data->parent;
    int i, ret;

    /* Handle errors from recv */
    if(error_code != 0 || flow_d->error_code != 0)
    {
        gossip_lerr("ERROR occured on recv: %d\n", error_code);
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* Decrement recv pending count; we just got one from the client */
    flow_data->recvs_pending -= 1;

    /* bytes received from the client */
    flow_data->total_bytes_recvd += actual_size;
    
    /* Debug output */
    gossip_lerr("RECV FINISHED: Total: %lld TotalRecvd: %lld RecvdNow: %lld AmtFwd: %lld PendingRecvs: %d "
                "PendingFwds: %d Throttled: %d\n",
                 (long long int)flow_data->total_bytes_req,
                 (long long int)flow_data->total_bytes_recvd,
                 (long long int)actual_size,
                 (long long int)flow_data->total_bytes_forwarded,
                 flow_data->recvs_pending,
                 flow_data->sends_pending,
                 flow_data->primary_recvs_throttled);
    if (q_item->buffer)
    {
           char *tmp;
           tmp = calloc(51,sizeof(char)); 
           gossip_err("First 50 bytes of received buffer:\n(%s)\n",strncpy(tmp,(char *)q_item->buffer,50));
           free(tmp);
    }

    /* Remove from current queue */
    qlist_del(&q_item->list_link);
    qlist_add_tail(&q_item->list_link, &flow_data->dest_list);

    /* Forward data to each replicate server */
    q_item->next_id = 0;
    q_item->next_bmi_callback.fn = forwarding_bmi_send_callback_wrapper;
    q_item->next_bmi_callback.data = q_item;

    for (i=0; i<flow_d->next_dest_count && flow_d->next_dest[i].resp_status == 0; i++)
    {
        flow_data->sends_pending++;

        ret = BMI_post_send( &flow_data->secondary_id
                            ,flow_d->next_dest[i].u.bmi.address
                            ,q_item->buffer
                            ,actual_size
                            ,BMI_PRE_ALLOC
                            ,flow_d->next_dest[i].u.bmi.tag
                            ,&q_item->secondary_bmi_callback
                            ,global_bmi_context
                            ,NULL );
        if (ret < 0)
        {
           gossip_lerr("Error while sending to replcate servers..\n");
           PVFS_perror("Error Code:",ret);
           handle_forwarding_io_error(ret, q_item, flow_data);
        }
        else if (ret == 1)
        {
           forwarding_bmi_send_callback_fn(q_item, actual_size, 0);
        }
    }/*end for*/

}/*end forwarding_bmi_recv_callback_fn*/



/**
 * Perform a process request for a q_item
 *
 * @return the number of bytes processed
 */
static int flow_process_request( struct fp_queue_item* q_item )
{
    struct result_chain_entry *result_tmp;
    struct result_chain_entry *old_result_tmp;
    PVFS_size bytes_processed = 0;
    void* tmp_buffer;
    
    result_tmp = &q_item->result_chain;
    old_result_tmp = result_tmp;
    tmp_buffer = q_item->buffer;
    
    do {
        int ret = 0;
        
        q_item->result_chain_count++;

        /* if no result chain exists, allocate one */
        if (!result_tmp)
        {
            result_tmp = (struct result_chain_entry*)malloc(
                sizeof(struct result_chain_entry));
            assert(result_tmp);
            memset(result_tmp, 0, sizeof(struct result_chain_entry));
            old_result_tmp->next = result_tmp;
        }
        
        /* process request */
        result_tmp->result.offset_array = result_tmp->offset_list;
        result_tmp->result.size_array = result_tmp->size_list;
        result_tmp->result.bytemax = q_item->parent->buffer_size - bytes_processed;
        result_tmp->result.bytes = 0;
        result_tmp->result.segmax = MAX_REGIONS;
        result_tmp->result.segs = 0;
        result_tmp->buffer_offset = tmp_buffer;
        
        ret = PINT_process_request(q_item->parent->file_req_state,
                                   q_item->parent->mem_req_state,
                                   &q_item->parent->file_data,
                                   &result_tmp->result,
                                   PINT_SERVER);
        
        /* TODO: error handling */ 
        assert(ret >= 0);

        /* No documnetation, figure out later */
        if (result_tmp->result.bytes == 0)
        {
            if (result_tmp != &q_item->result_chain)
            {
                free(result_tmp);
                old_result_tmp->next = NULL;
            }
            
            q_item->result_chain_count--;
        }
        else
        {
            old_result_tmp = result_tmp;
            result_tmp = result_tmp->next;
            tmp_buffer = (void*)((char*)tmp_buffer +
                                 old_result_tmp->result.bytes);
            bytes_processed += old_result_tmp->result.bytes;
        }
        
    } while (bytes_processed < q_item->parent->buffer_size && 
             !PINT_REQUEST_DONE(q_item->parent->file_req_state));

    return bytes_processed;
}/*end flow_process_request*/


static inline void server_trove_write_callback_wrapper(void *user_ptr,
                                                       PVFS_error error_code)
{
    struct fp_private_data *flow_data = PRIVATE_FLOW(((struct result_chain_entry*)user_ptr)->q_item->parent);

    gen_mutex_lock(&flow_data->parent->flow_mutex);
    server_trove_write_callback_fn(user_ptr, error_code);
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}/*end server_trove_write_callback_wrapper*/



/* MOVE THIS ENTIRE FUNCTION INTO forwarding_bmi_recv_callback_fn() */

/**
 * Performs a bmi send on data for a forwarding flow
 *
 * Remove q_item from current list
 * Adds q_item to dest_list
 * Sends data
 * Calls forwarding_bmi_send_callback on success
 *
 */
static void forwarding_bmi_send(struct fp_queue_item* q_item,
                                PVFS_size actual_size)
{
    gossip_err("Executing %s...\n",__func__);
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    flow_descriptor *flow_d = q_item->parent;
    int rc = 0;

    /* Add qitem to dest_list */
    /* move this to frowarding_bmi_recv_callback_fn*/
    qlist_del(&q_item->list_link);
    qlist_add_tail(&q_item->list_link, &flow_data->dest_list);

    /* Perform a write to secondary endpoint */
    /* move all of this, too */
    q_item->next_id = 0;
    q_item->next_bmi_callback.fn = forwarding_bmi_send_callback_wrapper;
    q_item->next_bmi_callback.data = q_item;

    rc = BMI_post_send(&q_item->next_id,
                       flow_d->next_dest.u.bmi.address,
                       q_item->buffer,
                       actual_size,
                       BMI_PRE_ALLOC,
                       flow_d->next_tag,
                       &q_item->next_bmi_callback,
                       global_bmi_context,
                       NULL);

    /* if an error occurs, handle it
       else if immediate completion, trigger callback */
    if (rc < 0)
    {
        gossip_err("ERROR: CATASTROPHIC FAILURE while forwarding\n");
        PVFS_perror("Error Code:", rc);
        handle_forwarding_io_error(rc, q_item, flow_data);    
    }
    else if (1 == rc)
    {
        gossip_lerr("Calling forwarding_bmi_send_callback_fn from forwarding_bmi_send...\n");
        forwarding_bmi_send_callback_fn(q_item, actual_size, 0);
    }

}/*end forwading_bmi_send*/

static void forwarding_bmi_send_callback_wrapper(void *user_ptr,
						 PVFS_size actual_size,
						 PVFS_error error_code)
{
    struct fp_private_data *flow_data = PRIVATE_FLOW(((struct fp_queue_item*)user_ptr)->parent);

    gen_mutex_lock(&flow_data->parent->flow_mutex);
    
    forwarding_bmi_send_callback_fn(user_ptr, actual_size, error_code);

    if (flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }
}/*end forwarding_bmi_send_callback_wrapper*/


/**
 * Callback invoked upon completion of a BMI send operation
 */
static void forwarding_bmi_send_callback_fn(void *user_ptr,
                                            PVFS_size actual_size,
                                            PVFS_error error_code)
{
    gossip_err("Executing %s...\n",__func__);
    /* Convert data into flow descriptor */
    struct fp_queue_item* q_item = user_ptr;
    struct fp_private_data *flow_data = PRIVATE_FLOW(((struct fp_queue_item*)user_ptr)->parent);

    /* Error handling */
    if (0 != error_code)
    {
        gossip_lerr("Forwarding completion:CATASTROPHIC ERROR???\n");
    }

    /* Perform bookkeeping for data forward completion */
    flow_data->sends_pending -= 1;


    /* after all sends have completed, this value will represent the amount of data sent
     * times the number of copies.
     */
    flow_data->total_bytes_forwarded += actual_size;

    /* Remove this q_item from the dest list */
    qlist_del(&q_item->list_link);

    /* Debug output */
    gossip_lerr("FWD FINISHED: Total: %lld TotalRecvd: %lld TotalAmtFwd: %lld AmtFwdNow: %lld PendingRecvs: %d "
                "PendingFwds: %d Throttled: %d\n",
                 (long long int)flow_data->total_bytes_req,
                 (long long int)flow_data->total_bytes_recvd,
                 (long long int)flow_data->total_bytes_forwarded,
                 (long long int)actual_size,
                 flow_data->recvs_pending,
                 flow_data->sends_pending,
                 flow_data->primary_recvs_throttled);

    /* Write the data to trove */
    if ( flow_data->sends_pending == 0 )
    {
       flow_data->writes_pending += 1;
       flow_trove_write(q_item, actual_size,
                        forwarding_trove_write_callback_wrapper,
                        forwarding_trove_write_callback_fn);
    }
    
    
    /* If there are no more recvs, check for completion
       else if there are recvs to go, start one if possible */
    /*
    if (forwarding_is_flow_complete(flow_data))
    {
        assert(flow_data->parent->state != FLOW_COMPLETE);
        flow_data->parent->state = FLOW_COMPLETE;
    }
    else if
    if (!PINT_REQUEST_DONE(flow_data->parent->file_req_state) &&
        0 < flow_data->primary_recvs_throttled)
    {
        gossip_debug(GOSSIP_BWS_PRIMARY_DEBUG,
                     "Starting recv from send callback.\n");
        flow_data->primary_recvs_throttled -= 1;
        flow_data->recvs_pending += 1;
        forwarding_bmi_recv(q_item);
    }
    */
}/*end forwarding_bmi_send_callback_fn*/


/**
 * Callback invoked upon completion of a trove write operation
 */
static void server_trove_write_callback_fn(void *user_ptr,
                                           PVFS_error error_code)
{
    gossip_err("Executing %s...\n",__func__);
    struct result_chain_entry* result_entry = user_ptr;
    struct fp_queue_item *q_item = result_entry->q_item;
    struct fp_private_data *flow_data = PRIVATE_FLOW(q_item->parent);
    flow_descriptor *flow_d = q_item->parent;

    gossip_lerr("Server Write Finished\n");

    /* Handle trove errors */
    if(error_code != 0 || flow_d->error_code != 0)
    {
        handle_io_error(error_code, q_item, flow_data);
        return;
    }

    /* Decrement result chain count */
    q_item->result_chain_count--;
    result_entry->posted_id = 0;

    /* If all results for this qitem are available continue */
    if (0 == q_item->result_chain_count)
    {
        struct result_chain_entry* result_iter = &q_item->result_chain;

        /* Decrement the number of pending writes */
        flow_data->writes_pending--;

        /* Aggregate results */
        while (0 != result_iter)
        {
            struct result_chain_entry* re = result_iter;
            flow_data->total_bytes_written += result_iter->result.bytes;
            flow_d->total_transferred += result_iter->result.bytes;
            PINT_perf_count(PINT_server_pc, 
                            PINT_PERF_WRITE,
                            result_iter->result.bytes, 
                            PINT_PERF_ADD);
            result_iter = result_iter->next;

            /* Free memory if this is not the chain head */
            if (re != &q_item->result_chain)
                free(re);
        }

        /* Debug output */
        gossip_lerr(
            "SERVER WRITE FINISHED: Total: %lld AmtWritten: %lld PendingWrites: %d Throttled: %d\n",
            (long long int)flow_data->total_bytes_req,
            (long long int)flow_data->total_bytes_written,
            flow_data->writes_pending,
            flow_data->primary_recvs_throttled);
    
        /* Cleanup q_item memory */
        q_item->result_chain.next = NULL;
        q_item->result_chain_count = 0;

        /* Remove q_item from in use list */
        qlist_del(&q_item->list_link);
        qlist_add_tail(&q_item->list_link, &flow_data->empty_list);

        /* Determine if the flow is complete */
        if (PINT_REQUEST_DONE(flow_d->file_req_state) &&
            0 == flow_data->recvs_pending &&
            0 == flow_data->writes_pending)
        {
            gossip_lerr("Server Write flow finished\n");
            assert(flow_data->total_bytes_recvd ==
                   flow_data->total_bytes_written);
            assert(flow_d->state != FLOW_COMPLETE);
            flow_d->state = FLOW_COMPLETE;
        }

        /* If there are recvs to go and stalling has occurred,
           start another recv */
        if (!PINT_REQUEST_DONE(flow_d->file_req_state) &&
            flow_data->primary_recvs_throttled > 0)
        {
            /* Post another recv operation */
            gossip_lerr("Starting recv from write callback.\n");
            flow_data->primary_recvs_throttled -= 1;
            flow_data->recvs_pending += 1;
            flow_bmi_recv(q_item,
                          server_bmi_recv_callback_wrapper,
                          server_bmi_recv_callback_fn);
        }
    }/*end if*/
}/*end server_trove_write_callback_fn*/


/* handle_forwarding_io_error()
 * 
 * called any time a BMI or Trove error code is detected, responsible for
 * safely cleaning up the associated flow
 *
 * NOTE: this function should always be called while holding the flow mutex!
 *
 * no return value
 */
static void handle_forwarding_io_error(PVFS_error error_code,
                                      struct fp_queue_item* q_item,
                                      struct fp_private_data* flow_data)
{
    int ret;

    PVFS_perror("Error: ", error_code);
    gossip_lerr("Forwarding Flow Error: CATASTROPHIC ERROR!\n");
    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, 
	"flowproto-multiqueue error cleanup path.\n");

    /* is this the first error registered for this particular flow? */
    if(flow_data->parent->error_code == 0)
    {
	gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
	    "flowproto-multiqueue first failure.\n");
	flow_data->parent->error_code = error_code;
	if(q_item)
	{
	    qlist_del(&q_item->list_link);
	}
	flow_data->cleanup_pending_count = 0;

	/* cleanup depending on what endpoints are in use */
        ret = cancel_pending_bmi(&flow_data->dest_list);
        gossip_debug(GOSSIP_FLOW_PROTO_DEBUG,
                     "flowproto-multiqueue canceling 2ndary %d BMI ops.\n", ret);
    }
    else
    {
	/* one of the previous cancels came through */
	flow_data->cleanup_pending_count--;
    }
    
    gossip_debug(GOSSIP_FLOW_PROTO_DEBUG, "cleanup_pending_count: %d\n",
	flow_data->cleanup_pending_count);

    if(flow_data->cleanup_pending_count == 0)
    {
	/* we are finished, make sure error is marked and state is set */
	assert(flow_data->parent->error_code);
	/* we are in trouble if more than one callback function thinks that
	 * it can trigger completion
	 */
	assert(flow_data->parent->state != FLOW_COMPLETE);
	flow_data->parent->state = FLOW_COMPLETE;
    }

    return;
}/*end handle_forwarding_io_error*/

/**
 * Perform initialization steps before this forwarding flow can be posted
 */
static inline void forwarding_flow_post_init(flow_descriptor* flow_d,
					     struct fp_private_data* flow_data)
{
    gossip_err("Executing %s...\n",__func__);
    int i;

    /* Generic flow initialization */
    flow_data->parent->total_transferred = 0;
    
    /* Iniitialize the pending counts */
    flow_data->recvs_pending = 0;
    flow_data->sends_pending = 0;
    flow_data->writes_pending = 0;
    flow_data->primary_recvs_throttled = 0;

    /* Initiailize progress counts */
    flow_data->total_bytes_req = flow_data->parent->aggregate_size;
    flow_data->total_bytes_forwarded = 0;
    flow_data->total_bytes_recvd = 0;
    flow_data->total_bytes_written = 0;
    
    gen_mutex_lock(&flow_data->parent->flow_mutex);

    /* Initialize buffers */
    for (i = 0; i < flow_data->parent->buffers_per_flow; i++)
    {
        /* Trove stuff I don't understand */
        flow_data->prealloc_array[i].result_chain.q_item = 
            &flow_data->prealloc_array[i];

        /* Place available buffers on the empty list */
        qlist_add_tail(&flow_data->prealloc_array[i].list_link,
                       &flow_data->empty_list);

    }

    /* Post the initial receives */
    for (i = 0; i < flow_data->parent->buffers_per_flow; i++)
    {
        /* If there is data to be received, perform the initial recv
           otherwise mark the flow complete */
        if (!PINT_REQUEST_DONE(flow_data->parent->file_req_state))
        {
            /* Remove the buffer from the empty list */
            qlist_del(&(flow_data->prealloc_array[i].list_link));

            /* Post the recv operation */
            flow_data->recvs_pending += 1;
            flow_bmi_recv(&(flow_data->prealloc_array[i]),
                          forwarding_bmi_recv_callback_wrapper,
                          forwarding_bmi_recv_callback_fn);
        }
        else
        {
            gossip_lerr("Server flow posted all on initial post.\n");
            break;
        }
    }

    /* If the flow is complete, perform cleanup */
    if(flow_data->parent->state == FLOW_COMPLETE)
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
        FLOW_CLEANUP(flow_data);
    }
    else
    {
        gen_mutex_unlock(&flow_data->parent->flow_mutex);
    }

}/*end forwarding_flow_post_init*/



/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
