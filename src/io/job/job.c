/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* this file contains a skeleton implementation of the job interface */

#include <errno.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <assert.h>

#include "flow.h"
#include "job.h"
#include "job-desc-queue.h"
#include "gen-locks.h"
#include "bmi.h"
#include "trove.h"
#include "gossip.h"
#include "request-scheduler.h"
#include "pint-dev.h"
#include "id-generator.h"
#include "pint-event.h"

#define JOB_EVENT_START(__op, __size, __id) \
 PINT_event_timestamp(PINT_EVENT_API_JOB, __op, __size, __id, \
 PINT_EVENT_FLAG_START)

#define JOB_EVENT_END(__op, __size, __id) \
 PINT_event_timestamp(PINT_EVENT_API_JOB, __op, __size, __id, \
 PINT_EVENT_FLAG_END)

/* contexts for use within the job interface */
static bmi_context_id global_bmi_context = -1;
static FLOW_context_id global_flow_context = -1;
#ifdef __PVFS2_TROVE_SUPPORT__
static TROVE_context_id global_trove_context = -1;
#endif

/* queues of pending jobs */
static job_desc_q_p completion_queue_array[JOB_MAX_CONTEXTS] = {NULL};
static int completion_error = 0;
static job_desc_q_p bmi_unexp_queue = NULL;
static int bmi_unexp_pending_count = 0;
static int bmi_pending_count = 0;
static int trove_pending_count = 0;
static int flow_pending_count = 0;
static job_desc_q_p dev_unexp_queue = NULL;
static int dev_unexp_pending_count = 0;
/* mutex locks for each queue */
static gen_mutex_t completion_mutex = GEN_MUTEX_INITIALIZER;
static gen_mutex_t bmi_mutex = GEN_MUTEX_INITIALIZER;
static gen_mutex_t trove_mutex = GEN_MUTEX_INITIALIZER;
static gen_mutex_t flow_mutex = GEN_MUTEX_INITIALIZER;
static gen_mutex_t dev_mutex = GEN_MUTEX_INITIALIZER;
/* NOTE: all of the bmi queues and counts are protected by the same
 * mutex (bmi_mutex)
 */

#ifdef __PVFS2_JOB_THREADED__
/* conditions and mutexes to use for sleeping threads when they have no
 * work to do and waking them when work is available 
 */
static pthread_cond_t completion_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t flow_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t dev_cond = PTHREAD_COND_INITIALIZER;
/* NOTE: the above conditions are protected by the same mutexes that
 * protect the queues
 */

/* code that the threads will run when initialized */
static void *flow_thread_function(void *ptr);
static pthread_t flow_thread_id;
static void *dev_thread_function(void* ptr);
static pthread_t dev_thread_id;
#endif /* __PVFS2_JOB_THREADED__ */

/* number of jobs to test for at once inside of do_one_work_cycle() */
enum
{
    job_work_metric = 5,
    thread_wait_timeout = 10000	/* usecs */
};

/* static arrays to use for testing lower level interfaces */
static flow_descriptor *stat_flow_array[job_work_metric];

static struct PINT_dev_unexp_info stat_dev_unexp_array[job_work_metric];

/********************************************************
 * function prototypes
 */

static int setup_queues(void);
static void teardown_queues(void);
static int do_one_work_cycle_flow(int *num_completed);
static int do_one_work_cycle_dev_unexp(int *num_completed);
static int do_one_test_cycle_req_sched(void);
static void fill_status(struct job_desc *jd,
			void **returned_user_ptr_p,
			job_status_s * status);
static int completion_query_some(job_id_t * id_array,
				 int *inout_count_p,
				 int *out_index_array,
				 void **returned_user_ptr_array,
				 job_status_s * out_status_array_p);
static int completion_query_context(job_id_t * out_id_array_p,
				  int *inout_count_p,
				  void **returned_user_ptr_array,
				  job_status_s *
				  out_status_array_p,
				  job_context_id context_id);
#ifdef __PVFS2_JOB_THREADED__
static void bmi_thread_mgr_callback(void* data, 
    PVFS_size actual_size,
    PVFS_error error_code);
static void bmi_thread_mgr_unexp_handler(struct BMI_unexpected_info* unexp);
#ifdef __PVFS2_TROVE_SUPPORT__
static void trove_thread_mgr_callback(void* data,
    PVFS_error error_code);
#endif
#else /* __PVFS2_JOB_THREADED__ */
static int do_one_work_cycle_all(int *num_completed,
				 int wait_flag);
static int do_one_work_cycle_bmi(int *num_completed,
				 int wait_flag);
static int do_one_work_cycle_bmi_unexp(int *num_completed,
				       int wait_flag);
#ifdef __PVFS2_TROVE_SUPPORT__
static int do_one_work_cycle_trove(int *num_completed);
static TROVE_op_id stat_trove_id_array[job_work_metric];
static void *stat_trove_user_ptr_array[job_work_metric];
static PVFS_error stat_trove_ds_state_array[job_work_metric];
#endif
static bmi_op_id_t stat_bmi_id_array[job_work_metric];
static bmi_error_code_t stat_bmi_error_code_array[job_work_metric];
static bmi_size_t stat_bmi_actual_size_array[job_work_metric];
static struct BMI_unexpected_info stat_bmi_unexp_array[job_work_metric];
static void *stat_bmi_user_ptr_array[job_work_metric];


#endif /* __PVFS2_JOB_THREADED__ */

/********************************************************
 * public interface 
 */

/* job_initialize()
 *
 * start up the job interface
 * 
 * returns 0 on success, -errno on failure
 */
int job_initialize(int flags)
{
    int ret = -1;

    /* ditto for flows */
    ret = PINT_flow_open_context(&global_flow_context);
    if(ret < 0)
    {
	return(ret);
    }

    ret = setup_queues();
    if (ret < 0)
    {
	PINT_flow_close_context(global_flow_context);
	return (ret);
    }

#ifdef __PVFS2_JOB_THREADED__
    /* startup threads */
    ret = PINT_thread_mgr_bmi_start();
    if (ret != 0)
    {
	PINT_flow_close_context(global_flow_context);
	teardown_queues();
	return (-ret);
    }
    ret = PINT_thread_mgr_bmi_getcontext(&global_bmi_context);
    /* this should never fail if the thread startup succeeded */
    assert(ret == 0);

    ret = pthread_create(&flow_thread_id, NULL, flow_thread_function, NULL);
    if (ret != 0)
    {
	PINT_thread_mgr_bmi_stop();
	PINT_flow_close_context(global_flow_context);
	teardown_queues();
	return (-ret);
    }
#ifdef __PVFS2_TROVE_SUPPORT__
    ret = PINT_thread_mgr_trove_start();
    if(ret != 0)
    {
	PINT_thread_mgr_bmi_stop();
	pthread_cancel(flow_thread_id);
	PINT_flow_close_context(global_flow_context);
	teardown_queues();
	return (-ret);
    }
    ret = PINT_thread_mgr_trove_getcontext(&global_trove_context);
    /* this should never fail if thread startup succeeded */
    assert(ret == 0);

#endif
    ret = pthread_create(&dev_thread_id, NULL, dev_thread_function, NULL);
    if (ret != 0)
    {
	PINT_thread_mgr_bmi_stop();
	pthread_cancel(flow_thread_id);
#ifdef __PVFS2_TROVE_SUPPORT__
	PINT_thread_mgr_trove_stop();
#endif
	PINT_flow_close_context(global_flow_context);
	teardown_queues();
	return (-ret);
    }

#else /* __PVFS2_JOB_THREADED__ */

    /* get a bmi context to work in */
    ret = BMI_open_context(&global_bmi_context);
    if(ret < 0)
    {
	PINT_flow_close_context(global_flow_context);
	return(ret);
    }

#ifdef __PVFS2_TROVE_SUPPORT__
    /* ditto for trove */
    ret = trove_open_context(/*FIXME: HACK*/9,&global_trove_context);
    if (ret < 0)
    {
	PINT_flow_close_context(global_flow_context);
	BMI_close_context(global_bmi_context);
        return ret;
    }
#endif

#endif /* __PVFS2_JOB_THREADED__ */

    return (0);
}

/* job_finalize()
 *
 * shuts down the job interface
 *
 * returns 0 on success, -errno on failure
 */
int job_finalize(void)
{

#ifdef __PVFS2_JOB_THREADED__
    PINT_thread_mgr_bmi_stop();
    pthread_cancel(flow_thread_id);
#ifdef __PVFS2_TROVE_SUPPORT__
    PINT_thread_mgr_trove_stop();
#endif
    pthread_cancel(dev_thread_id);
#else /* __PVFS2_JOB_THREADED__ */
    BMI_close_context(global_bmi_context);
#ifdef __PVFS2_TROVE_SUPPORT__
    trove_close_context(/* FIXME: HACK*/9,&global_trove_context);
#endif
#endif /* __PVFS2_JOB_THREADED__ */

    PINT_flow_close_context(global_flow_context);

    teardown_queues();

    return (0);
}


/* job_open_context()
 *
 * opens a new context for the job interface
 *
 * returns 0 on success, -errno on failure
 */
int job_open_context(job_context_id* context_id)
{
    int context_index;

    /* find an unused context id */
    for(context_index=0; context_index<JOB_MAX_CONTEXTS; context_index++)
    {
	if(completion_queue_array[context_index] == NULL)
	{
	    break;
	}
    }

    if(context_index >= JOB_MAX_CONTEXTS)
    {
	/* we don't have any more available! */
	return(-EBUSY);
    }

    /* create a new completion queue for the context */
    completion_queue_array[context_index] = job_desc_q_new();
    if(!completion_queue_array[context_index])
    {
	return(-ENOMEM);
    }

    *context_id = context_index;
    return(0);
}


/* job_close_context()
 *
 * destroys a context previously created with job_open_context()
 *
 * no return value 
 */
void job_close_context(job_context_id context_id)
{
    if(!completion_queue_array[context_id])
    {
	return;
    }

    job_desc_q_cleanup(completion_queue_array[context_id]);

    completion_queue_array[context_id] = NULL;

    return;
}

/* job_bmi_send()
 *
 * posts a job to send a BMI message
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_bmi_send(bmi_addr_t addr,
		 void *buffer,
		 bmi_size_t size,
		 bmi_msg_tag_t tag,
		 enum bmi_buffer_type buffer_type,
		 int send_unexpected,
		 void *user_ptr,
		 job_aint status_user_tag,
		 job_status_s * out_status_p,
		 job_id_t * id,
		 job_context_id context_id)
{
    /* post a bmi send.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal = NULL;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_BMI);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.bmi.actual_size = size;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->bmi_callback.fn = bmi_thread_mgr_callback;
    jd->bmi_callback.data = (void*)jd;
    user_ptr_internal = &jd->bmi_callback;
#else
    user_ptr_internal = jd;
#endif

    /* post appropriate type of send */
    if (!send_unexpected)
    {
	ret = BMI_post_send(&(jd->u.bmi.id), addr, buffer, size,
			    buffer_type, tag, user_ptr_internal, 
			    global_bmi_context);
    }
    else
    {
	ret = BMI_post_sendunexpected(&(jd->u.bmi.id), addr,
				      buffer, size, buffer_type, tag,
				      user_ptr_internal, global_bmi_context);
    }

    if (ret < 0)
    {
	/* error posting */
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->actual_size = size;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall to this point, the job did not immediately complete and
     * we must queue up to test it later 
     */
    gen_mutex_lock(&bmi_mutex);
    *id = jd->job_id;
    bmi_pending_count++;
    gen_mutex_unlock(&bmi_mutex);

    return (0);
}


/* job_bmi_send_list()
 *
 * posts a job to send a BMI list message
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_bmi_send_list(bmi_addr_t addr,
		      void **buffer_list,
		      bmi_size_t * size_list,
		      int list_count,
		      bmi_size_t total_size,
		      bmi_msg_tag_t tag,
		      enum bmi_buffer_type buffer_type,
		      int send_unexpected,
		      void *user_ptr,
		      job_aint status_user_tag,
		      job_status_s * out_status_p,
		      job_id_t * id,
		      job_context_id context_id)
{
    /* post a bmi send.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal = NULL;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_BMI);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.bmi.actual_size = total_size;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->bmi_callback.fn = bmi_thread_mgr_callback;
    jd->bmi_callback.data = (void*)jd;
    user_ptr_internal = &jd->bmi_callback;
#else
    user_ptr_internal = jd;
#endif

    /* post appropriate type of send */
    if (!send_unexpected)
    {
	ret = BMI_post_send_list(&(jd->u.bmi.id), addr,
				 (const void **) buffer_list, size_list,
				 list_count, total_size, buffer_type,
				 tag, user_ptr_internal, global_bmi_context);
    }
    else
    {
	ret = BMI_post_sendunexpected_list(&(jd->u.bmi.id), addr,
					   (const void **) buffer_list,
					   size_list, list_count,
					   total_size, buffer_type, tag,
					   user_ptr_internal, global_bmi_context);
    }

    if (ret < 0)
    {
	/* error posting */
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->actual_size = total_size;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall to this point, the job did not immediately complete and
     * we must queue up to test it later 
     */
    gen_mutex_lock(&bmi_mutex);
    *id = jd->job_id;
    bmi_pending_count++;
    gen_mutex_unlock(&bmi_mutex);

    return (0);
}

/* job_bmi_recv()
 *
 * posts a job to receive a BMI message
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_bmi_recv(bmi_addr_t addr,
		 void *buffer,
		 bmi_size_t size,
		 bmi_msg_tag_t tag,
		 enum bmi_buffer_type buffer_type,
		 void *user_ptr,
		 job_aint status_user_tag,
		 job_status_s * out_status_p,
		 job_id_t * id,
		 job_context_id context_id)
{

    /* post a bmi recv.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal = NULL;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_BMI);
    if (!jd)
    {
	return (-ENOMEM);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->bmi_callback.fn = bmi_thread_mgr_callback;
    jd->bmi_callback.data = (void*)jd;
    user_ptr_internal = &jd->bmi_callback;
#else
    user_ptr_internal = jd;
#endif

    ret = BMI_post_recv(&(jd->u.bmi.id), addr, buffer, size,
			&(jd->u.bmi.actual_size), buffer_type, tag, 
			user_ptr_internal, 
			global_bmi_context);

    if (ret < 0)
    {
	/* error posting */
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->actual_size = jd->u.bmi.actual_size;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall to this point, the job did not immediately complete and
     * we must queue up to test it later 
     */
    gen_mutex_lock(&bmi_mutex);
    *id = jd->job_id;
    bmi_pending_count++;
    gen_mutex_unlock(&bmi_mutex);

    return (0);

}


/* job_bmi_recv_list()
 *
 * posts a job to receive a BMI list message
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_bmi_recv_list(bmi_addr_t addr,
		      void **buffer_list,
		      bmi_size_t * size_list,
		      int list_count,
		      bmi_size_t total_expected_size,
		      bmi_msg_tag_t tag,
		      enum bmi_buffer_type buffer_type,
		      void *user_ptr,
		      job_aint status_user_tag,
		      job_status_s * out_status_p,
		      job_id_t * id,
		      job_context_id context_id)
{

    /* post a bmi recv.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal = NULL;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_BMI);
    if (!jd)
    {
	return (-ENOMEM);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->bmi_callback.fn = bmi_thread_mgr_callback;
    jd->bmi_callback.data = (void*)jd;
    user_ptr_internal = &jd->bmi_callback;
#else
    user_ptr_internal = jd;
#endif

    ret = BMI_post_recv_list(&(jd->u.bmi.id), addr, buffer_list,
			     size_list, list_count, total_expected_size,
			     &(jd->u.bmi.actual_size), buffer_type, tag,
			     user_ptr_internal, global_bmi_context);

    if (ret < 0)
    {
	/* error posting */
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->actual_size = jd->u.bmi.actual_size;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall to this point, the job did not immediately complete and
     * we must queue up to test it later 
     */
    gen_mutex_lock(&bmi_mutex);
    *id = jd->job_id;
    bmi_pending_count++;
    gen_mutex_unlock(&bmi_mutex);

    return (0);

}

/* job_bmi_unexp()
 *
 * posts a job to receive an unexpected BMI message
 *
 * returns 0 on succcess, 1 on immediate completion, and -errno on
 * failure
 */
int job_bmi_unexp(struct BMI_unexpected_info *bmi_unexp_d,
		  void *user_ptr,
		  job_aint status_user_tag,
		  job_status_s * out_status_p,
		  job_id_t * id,
		  enum job_flags flags,
		  job_context_id context_id)
{
    /* post a bmi recv for an unexpected message.  We will do a quick
     * test to see if an unexpected message is available.  If so, we
     * return the necessary info; if not we queue up to test again later
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    int outcount = 0;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_BMI_UNEXP);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.bmi_unexp.info = bmi_unexp_d;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;

   /*********************************************************
	 * TODO: consider optimizations later, so that we avoid
	 * disabling immediate completion.  See the mailing list thread
	 * started here:
	 * http://www.beowulf-underground.org/pipermail/pvfs2-internal/2003-February/000305.html
	 *
	 * At the moment, the server is not designed to be able to
	 * handle immediate completion upon posting unexpected jobs.
	 */

    /* only look for immediate completion if our flags allow it */
    if (!(flags & JOB_NO_IMMED_COMPLETE))
    {
	ret = BMI_testunexpected(1, &outcount, jd->u.bmi_unexp.info, 0);

	if (ret < 0)
	{
	    /* error testing */
	    dealloc_job_desc(jd);
	    return (ret);
	}

	if (outcount == 1)
	{
	    /* there was an unexpected job available */
	    out_status_p->error_code = jd->u.bmi_unexp.info->error_code;
	    out_status_p->status_user_tag = status_user_tag;
	    dealloc_job_desc(jd);
	    return (ret);
	}
    }

    /* if we fall through to this point, then there were not any
     * uenxpected receive's available; queue up to test later 
     */
    gen_mutex_lock(&bmi_mutex);
    *id = jd->job_id;
    job_desc_q_add(bmi_unexp_queue, jd);
    bmi_unexp_pending_count++;
    gen_mutex_unlock(&bmi_mutex);

#ifdef __PVFS2_JOB_THREADED__
    PINT_thread_mgr_bmi_unexp_handler(bmi_thread_mgr_unexp_handler);
#endif /* __PVFS2_JOB_THREADED__ */

    return (0);
}

/* job_dev_unexp()
 *
 * posts a job for an unexpected device message
 *
 * returns 0 on success, -errno on failure, and 1 on immediate
 * completion
 */
int job_dev_unexp(struct PINT_dev_unexp_info* dev_unexp_d,
    void* user_ptr,
    job_aint status_user_tag,
    job_status_s * out_status_p,
    job_id_t* id,
    job_context_id context_id)
{
    /* post a dev recv for an unexpected message.  We will do a quick
     * test to see if an unexpected message is available.  If so, we
     * return the necessary info; if not we queue up to test again later
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    int outcount = 0;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the user ptr etc.
     */
    jd = alloc_job_desc(JOB_DEV_UNEXP);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.dev_unexp.info = dev_unexp_d;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;

    ret = PINT_dev_test_unexpected(1, &outcount, jd->u.dev_unexp.info, 0);

    if (ret < 0)
    {
	/* error testing */
	dealloc_job_desc(jd);
	return (ret);
    }

    if (outcount == 1)
    {
	/* there was an unexpected job available */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, then there were not any
     * uenxpected receive's available; queue up to test later 
     */
    gen_mutex_lock(&dev_mutex);
    *id = jd->job_id;
    job_desc_q_add(dev_unexp_queue, jd);
    dev_unexp_pending_count++;
#ifdef __PVFS2_JOB_THREADED__
    pthread_cond_signal(&dev_cond);
#endif /* __PVFS2_JOB_THREADED__ */
    gen_mutex_unlock(&dev_mutex);

    return (0);
}

/* job_dev_write()
 *
 * posts a device write
 *
 * returns 0 on success, -errno on failure, and 1 on immediate completion
 */
int job_dev_write(void* buffer,
    int size,
    PVFS_id_gen_t tag,
    enum PINT_dev_buffer_type buffer_type,
    void* user_ptr,
    job_aint status_user_tag,
    job_status_s * out_status_p,
    job_id_t * id,
    job_context_id context_id)
{
    /* NOTE: This function will _always_ immediately complete for now.  
     * It is really just in the job interface for completeness, in case we 
     * decide later to make the function asynchronous
     */

    int ret = -1;

    ret = PINT_dev_write(buffer, size, buffer_type, tag);
    if(ret < 0)
    {
	/* error posting */
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return(1);
    }

    /* immediate completion */
    out_status_p->error_code = 0;
    out_status_p->status_user_tag = status_user_tag;
    out_status_p->actual_size = size;
    return(1);
}


/* job_dev_write_list()
 *
 * posts a device write for multiple buffers
 *
 * returns 0 on success, -errno on failure, and 1 on immediate completion
 */
int job_dev_write_list(void** buffer_list,
    int* size_list,
    int list_count,
    int total_size,
    PVFS_id_gen_t tag,
    enum PINT_dev_buffer_type buffer_type,
    void* user_ptr,
    job_aint status_user_tag,
    job_status_s* out_status_p,
    job_id_t* id,
    job_context_id context_id)
{
    /* NOTE: This function will _always_ immediately complete for now.  
     * It is really just in the job interface for completeness, in case we 
     * decide later to make the function asynchronous
     */

    int ret = -1;

    ret = PINT_dev_write_list(buffer_list, size_list, list_count,
	total_size, buffer_type, tag);
    if(ret < 0)
    {
	/* error posting */
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return(1);
    }

    /* immediate completion */
    out_status_p->error_code = 0;
    out_status_p->status_user_tag = status_user_tag;
    out_status_p->actual_size = total_size;
    return(1);
}


/* job_req_sched_post()
 *
 * posts a request to the request scheduler
 *
 * returns 0 on success, -errno on failure, and 1 on immediate
 * completion 
 */
int job_req_sched_post(struct PVFS_server_req *in_request,
		       int req_index,
		       void *user_ptr,
		       job_aint status_user_tag,
		       job_status_s * out_status_p,
		       job_id_t * id,
		       job_context_id context_id)
{
    /* post a request to the scheduler.  If it completes (or fails)
     * immediately, then return and fill in the status structure.
     * If it needs to be tested for completion later, then queue up
     * a job_desc structure.        
     */
    /* NOTE: this function is unique in the job interface because
     * we have to track a job id even when it completes (to be
     * matched in release function).  We will use a seperate queue
     * for this.
     */
    struct job_desc *jd = NULL;
    int ret = -1;

    jd = alloc_job_desc(JOB_REQ_SCHED);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.req_sched.post_flag = 1;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;

    ret = PINT_req_sched_post(in_request, req_index, jd, &(jd->u.req_sched.id));

    if (ret < 0)
    {
	/* error posting */
	dealloc_job_desc(jd);
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	*id = jd->job_id;
	/* don't delete the job desc until a matching release comes through */
	return (1);
    }

    /* if we hit this point, job did not immediately complete-
     * queue to test later
     */
    *id = jd->job_id;

    return (0);
}

/* job_req_sched_post_timer()
 *
 * posts a timer to the request scheduler
 *
 * returns 0 on success, -errno on failure, and 1 on immediate
 * completion 
 */
int job_req_sched_post_timer(int msecs,
		       void *user_ptr,
		       job_aint status_user_tag,
		       job_status_s * out_status_p,
		       job_id_t * id,
		       job_context_id context_id)
{
    /* post a timer to the scheduler.  If it completes (or fails)
     * immediately, then return and fill in the status structure.
     * If it needs to be tested for completion later, then queue up
     * a job_desc structure.        
     */

    struct job_desc *jd = NULL;
    int ret = -1;

    jd = alloc_job_desc(JOB_REQ_SCHED);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;

    ret = PINT_req_sched_post_timer(msecs, jd, &(jd->u.req_sched.id));

    if (ret < 0)
    {
	/* error posting */
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (1);
    }

    /* if we hit this point, job did not immediately complete-
     * queue to test later
     */
    *id = jd->job_id;

    return (0);
}


/* job_req_sched_release
 *
 * releases a request from the request scheduler
 *
 * returns 0 on success, -errno on failure, and 1 on immediate
 * completion
 */
int job_req_sched_release(job_id_t in_completed_id,
			  void *user_ptr,
			  job_aint status_user_tag,
			  job_status_s * out_status_p,
			  job_id_t * out_id,
			  job_context_id context_id)
{
    /* this function is a little odd.  We need to
     * do is retrieve the job desc that we queued up in the
     * inprogress queue to match the release properly 
     */

    struct job_desc *match_jd = NULL;
    struct job_desc *jd = NULL;
    int ret = -1;

    jd = alloc_job_desc(JOB_REQ_SCHED);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;

    match_jd = id_gen_fast_lookup(in_completed_id);

    ret = PINT_req_sched_release(match_jd->u.req_sched.id, jd,
				 &(jd->u.req_sched.id));

    /* delete the old req sched job desc; it is no longer needed */
    dealloc_job_desc(match_jd);

    /* NOTE: I am letting the return value propigate here, rather
     * than just setting the status.  Failure here is bad...
     */
    if (ret < 0)
    {
	/* error posting */
	dealloc_job_desc(jd);
	return (ret);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (1);
    }

    /* if we hit this point, job did not immediately complete-
     * queue to test later
     */
    *out_id = jd->job_id;

    return (0);
}


/* job_flow()
 *
 * posts a job to service a flow (where a flow is a complex I/O
 * operation between two endpoints, which may be memory, disk, or
 * network)
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_flow(flow_descriptor * flow_d,
	     void *user_ptr,
	     job_aint status_user_tag,
	     job_status_s * out_status_p,
	     job_id_t * id,
	     job_context_id context_id)
{
    struct job_desc *jd = NULL;
    int ret = -1;

    /* allocate job descriptor first */
    jd = alloc_job_desc(JOB_FLOW);
    if (!jd)
    {
	return (-ENOMEM);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.flow.flow_d = flow_d;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
    flow_d->user_ptr = jd;

    /* post the flow */
    ret = PINT_flow_post(flow_d, global_flow_context);
    if (ret < 0)
    {
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (1);
    }
    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->actual_size = flow_d->total_transfered;
	dealloc_job_desc(jd);
        JOB_EVENT_START(PINT_EVENT_FLOW, 0, 0);
        JOB_EVENT_END(PINT_EVENT_FLOW, flow_d->total_transfered, 0);
	return (1);
    }

    /* queue up the job desc. for later completion */
    gen_mutex_lock(&flow_mutex);
    *id = jd->job_id;
    flow_pending_count++;
#ifdef __PVFS2_JOB_THREADED__
    pthread_cond_signal(&flow_cond);
#endif /* __PVFS2_JOB_THREADED__ */
    gen_mutex_unlock(&flow_mutex);

    JOB_EVENT_START(PINT_EVENT_FLOW, 0, *id);

    return (0);
}


/* job_trove_bstream_write_at()
 *
 * storage byte stream write 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_bstream_write_at(PVFS_fs_id coll_id,
			       PVFS_handle handle,
			       PVFS_offset offset,
			       void *buffer,
			       PVFS_size size,
			       PVFS_ds_flags flags,
			       PVFS_vtag * vtag,
			       void *user_ptr,
			       job_aint status_user_tag,
			       job_status_s * out_status_p,
			       job_id_t * id,
			       job_context_id context_id)
{
    /* post a trove write.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.actual_size = size;
    jd->u.trove.vtag = vtag;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_bstream_write_at(coll_id, handle, buffer,
				 &jd->u.trove.actual_size, offset, flags,
				 jd->u.trove.vtag, user_ptr_internal, 
			   	 global_trove_context,
				 &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->actual_size = jd->u.trove.actual_size;
	out_status_p->vtag = jd->u.trove.vtag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_bstream_read_at()
 *
 * storage byte stream read 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_bstream_read_at(PVFS_fs_id coll_id,
			      PVFS_handle handle,
			      PVFS_offset offset,
			      void *buffer,
			      PVFS_size size,
			      PVFS_ds_flags flags,
			      PVFS_vtag * vtag,
			      void *user_ptr,
			      job_aint status_user_tag,
			      job_status_s * out_status_p,
			      job_id_t * id,
			      job_context_id context_id)
{
    /* post a trove read.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.actual_size = size;
    jd->u.trove.vtag = vtag;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_bstream_read_at(coll_id, handle, buffer,
				&jd->u.trove.actual_size, offset, flags,
				jd->u.trove.vtag, user_ptr_internal,
				global_trove_context,
				&(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->actual_size = jd->u.trove.actual_size;
	out_status_p->vtag = jd->u.trove.vtag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_bstream_flush()
 *
 * ask the storage layer to flush data to disk
 *
 * returns 0 on success, 1 on immediate completion, and -errno on failure
 */

int job_trove_bstream_flush(PVFS_fs_id coll_id,
			    PVFS_handle handle,
			    PVFS_ds_flags flags,
			    void *user_ptr,
			    job_aint status_user_tag,
			    job_status_s * out_status_p,
			    job_id_t * id,
			    job_context_id context_id)
			    
{
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_bstream_flush(coll_id, handle, flags, user_ptr_internal,
			      global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }
    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }
    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_keyval_read()
 *
 * storage key/value read 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_keyval_read(PVFS_fs_id coll_id,
			  PVFS_handle handle,
			  PVFS_ds_keyval * key_p,
			  PVFS_ds_keyval * val_p,
			  PVFS_ds_flags flags,
			  PVFS_vtag * vtag,
			  void *user_ptr,
			  job_aint status_user_tag,
			  job_status_s * out_status_p,
			  job_id_t * id,
			  job_context_id context_id)
{
    /* post a trove keyval read.  If it completes (or fails)
     * immediately, then return and fill in the status structure.  
     * If it needs to be tested for completion later, then queue 
     * up a job_desc structure.  */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.vtag = vtag;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_keyval_read(coll_id, handle, key_p, val_p, flags,
			    jd->u.trove.vtag, user_ptr_internal, 
			    global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);

	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->vtag = jd->u.trove.vtag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_keyval_read_list()
 *
 * storage key/value read list
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_keyval_read_list(PVFS_fs_id coll_id,
			       PVFS_handle handle,
			       PVFS_ds_keyval * key_array,
			       PVFS_ds_keyval * val_array,
			       int count,
			       PVFS_ds_flags flags,
			       PVFS_vtag * vtag,
			       void *user_ptr,
			       job_aint status_user_tag,
			       job_status_s * out_status_p,
			       job_id_t * id,
			       job_context_id context_id)
{
    /* post a trove keyval read.  If it completes (or fails)
     * immediately, then return and fill in the status structure.  
     * If it needs to be tested for completion later, then queue 
     * up a job_desc structure.  */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.vtag = vtag;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_keyval_read_list(coll_id, handle, key_array,
				 val_array, count, flags, jd->u.trove.vtag, 
				 user_ptr_internal,
				 global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);

	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->vtag = jd->u.trove.vtag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_keyval_write()
 *
 * storage key/value write 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_keyval_write(PVFS_fs_id coll_id,
			   PVFS_handle handle,
			   PVFS_ds_keyval * key_p,
			   PVFS_ds_keyval * val_p,
			   PVFS_ds_flags flags,
			   PVFS_vtag * vtag,
			   void *user_ptr,
			   job_aint status_user_tag,
			   job_status_s * out_status_p,
			   job_id_t * id,
			   job_context_id context_id)
{
    /* post a trove keyval write.  If it completes (or fails)
     * immediately, then return and fill in the status structure.  
     * If it needs to be tested for completion later, then queue 
     * up a job_desc structure.  */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.vtag = vtag;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_keyval_write(coll_id, handle, key_p, val_p, flags,
			     jd->u.trove.vtag, user_ptr_internal, 
			     global_trove_context,
			     &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);

	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->vtag = jd->u.trove.vtag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_keyval_flush()
 *
 * ask the storage layer to flush keyvals to disk
 *
 * returns 0 on success, 1 on immediate completion, and -errno on failure
 */

int job_trove_keyval_flush(PVFS_fs_id coll_id,
			    PVFS_handle handle,
			    PVFS_ds_flags flags,
			    void * user_ptr,
			    job_aint status_user_tag,
			    job_status_s * out_status_p,
			    job_id_t * id,
			    job_context_id context_id)
{
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_keyval_flush(coll_id, handle, flags, user_ptr_internal,
                             global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}


/* job_trove_dspace_getattr()
 *
 * read generic dspace attributes 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_dspace_getattr(PVFS_fs_id coll_id,
			     PVFS_handle handle,
			     void *user_ptr,
			     job_aint status_user_tag,
			     job_status_s * out_status_p,
			     job_id_t * id,
			     job_context_id context_id)
{
    /* post a trove operation dspace get attr.  If it completes (or
     * fails) immediately, then return and fill in the status
     * structure.  If it needs to be tested for completion later,
     * then queue up a job desc structure.
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif



#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_dspace_getattr(coll_id,
			       handle, &(jd->u.trove.attr), 0 /* flags */ ,
			       user_ptr_internal, 
			       global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->ds_attr = jd->u.trove.attr;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall to this point, the job did not immediately complete and
     * we must queue up to test it later 
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_dspace_setattr()
 *
 * write generic dspace attributes 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_dspace_setattr(PVFS_fs_id coll_id,
			     PVFS_handle handle,
			     PVFS_ds_attributes * ds_attr_p,
			     void *user_ptr,
			     job_aint status_user_tag,
			     job_status_s * out_status_p,
			     job_id_t * id,
			     job_context_id context_id)
{
    /* post a trove operation dspace set attr.  If it completes (or
     * fails) immediately, then return and fill in the status
     * structure.  If it needs to be tested for completion later,
     * then queue up a job desc structure.
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_dspace_setattr(coll_id, handle, ds_attr_p, 0 /* flags */ ,
			       user_ptr_internal, global_trove_context, 
			       &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall to this point, the job did not immediately complete and
     * we must queue up to test it later 
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_bstream_resize()
 *
 * resize (truncate or preallocate) a storage byte stream 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_bstream_resize(PVFS_fs_id coll_id,
			     PVFS_handle handle,
			     PVFS_size size,
			     PVFS_ds_flags flags,
			     PVFS_vtag * vtag,
			     void *user_ptr,
			     job_aint status_user_tag,
			     job_status_s * out_status_p,
			     job_id_t * id,
			     job_context_id context_id)
{
    /* post a resize trove operation.  If it completes (or
     * fails) immediately, then return and fill in the status
     * structure.  If it needs to be tested for completion later,
     * then queue up a job desc structure.
     */

    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_bstream_resize(coll_id, handle, &size, 0 /* flags */ ,
			       vtag, user_ptr_internal, global_trove_context, 
			       &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall to this point, the job did not immediately complete and
     * we must queue up to test it later 
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
    gossip_lerr("Error: unimplemented.\n");
    return (-ENOSYS);
}

/* job_trove_bstream_validate()
 *
 * check consistency of a bytestream for a given vtag 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_bstream_validate(PVFS_fs_id coll_id,
			       PVFS_handle handle,
			       PVFS_vtag * vtag,
			       void *user_ptr,
			       job_aint status_user_tag,
			       job_status_s * out_status_p,
			       job_id_t * id,
			       job_context_id context_id)
{
    gossip_lerr("Error: unimplemented.\n");
    return (-ENOSYS);
}

/* job_trove_keyval_remove()
 *
 * remove a key/value entry 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_keyval_remove(PVFS_fs_id coll_id,
			    PVFS_handle handle,
			    PVFS_ds_keyval * key_p,
			    PVFS_ds_flags flags,
			    PVFS_vtag * vtag,
			    void *user_ptr,
			    job_aint status_user_tag,
			    job_status_s * out_status_p,
			    job_id_t * id,
			    job_context_id context_id)
{
    /* post a trove keyval remove.  If it completes (or fails)
     * immediately, then return and fill in the status structure.  
     * If it needs to be tested for completion later, then queue 
     * up a job_desc structure.  */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.vtag = vtag;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_keyval_remove(coll_id, handle, key_p, flags,
			      jd->u.trove.vtag, user_ptr_internal, 
			      global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);

	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->vtag = jd->u.trove.vtag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_keyval_validate()
 *
 * check consistency of a key/value pair for a given vtag 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_keyval_validate(PVFS_fs_id coll_id,
			      PVFS_handle handle,
			      PVFS_vtag * vtag,
			      void *user_ptr,
			      job_aint status_user_tag,
			      job_status_s * out_status_p,
			      job_id_t * id,
			      job_context_id context_id)
{
    gossip_lerr("Error: unimplemented.\n");
    return (-ENOSYS);
}

/* job_trove_keyval_iterate()
 *
 * iterate through all of the key/value pairs for a data space 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_keyval_iterate(PVFS_fs_id coll_id,
			     PVFS_handle handle,
			     PVFS_ds_position position,
			     PVFS_ds_keyval * key_array,
			     PVFS_ds_keyval * val_array,
			     int count,
			     PVFS_ds_flags flags,
			     PVFS_vtag * vtag,
			     void *user_ptr,
			     job_aint status_user_tag,
			     job_status_s * out_status_p,
			     job_id_t * id,
			     job_context_id context_id)
{
    /* post a trove keyval iterate.  If it completes (or fails)
     * immediately, then return and fill in the status structure.  
     * If it needs to be tested for completion later, then queue 
     * up a job_desc structure.  */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.vtag = vtag;
    jd->u.trove.position = position;
    jd->u.trove.count = count;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_keyval_iterate(coll_id, handle,
			       &(jd->u.trove.position), key_array, val_array,
			       &(jd->u.trove.count), flags, jd->u.trove.vtag,
			       user_ptr_internal, 
			       global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);

	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->vtag = jd->u.trove.vtag;
	out_status_p->position = jd->u.trove.position;
	out_status_p->count = jd->u.trove.count;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_dspace_iterate_handles()
 *
 * iterates through all of the handles in a given collection
 *
 * returns 0 on success, 1 on immediate completion, -PVFS_error on failure
 */
int job_trove_dspace_iterate_handles(PVFS_fs_id coll_id,
    PVFS_ds_position position,
    PVFS_handle* handle_array,
    int count,
    PVFS_ds_flags flags,
    PVFS_vtag* vtag,
    void* user_ptr,
    job_aint status_user_tag,
    job_status_s* out_status_p,
    job_id_t* id,
    job_context_id context_id)
{
    /* post a trove keyval iterate_handles.  If it completes (or fails)
     * immediately, then return and fill in the status structure.  
     * If it needs to be tested for completion later, then queue 
     * up a job_desc structure.  */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.vtag = vtag;
    jd->u.trove.position = position;
    jd->u.trove.count = count;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_dspace_iterate_handles(coll_id,
			       &(jd->u.trove.position), handle_array,
			       &(jd->u.trove.count), flags, jd->u.trove.vtag,
			       user_ptr_internal, 
			       global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);

	out_status_p->error_code = ret;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->vtag = jd->u.trove.vtag;
	out_status_p->position = jd->u.trove.position;
	out_status_p->count = jd->u.trove.count;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}


/* iterate through all of the keys for a data space */
int job_trove_keyval_iterate_keys(PVFS_fs_id coll_id,
				  PVFS_handle handle,
				  PVFS_ds_position position,
				  PVFS_ds_keyval * key_array,
				  int count,
				  PVFS_ds_flags flags,
				  PVFS_vtag * vtag,
				  void *user_ptr,
				  job_aint status_user_tag,
				  job_status_s * out_status_p,
				  job_id_t * id,
				  job_context_id context_id);


/* job_trove_keyval_iterate_keys()
 *
 * iterate through all of the keys for a data space 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_keyval_iterate_keys(PVFS_fs_id coll_id,
				  PVFS_handle handle,
				  PVFS_ds_position position,
				  PVFS_ds_keyval * key_array,
				  int count,
				  PVFS_ds_flags flags,
				  PVFS_vtag * vtag,
				  void *user_ptr,
				  job_aint status_user_tag,
				  job_status_s * out_status_p,
				  job_id_t * id,
				  job_context_id context_id)
{
    gossip_lerr("Error: unimplemented.\n");
    return (-ENOSYS);
}

/* job_trove_dspace_create()
 *
 * create a new data space object 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_dspace_create(PVFS_fs_id coll_id,
			    PVFS_handle_extent_array *handle_extent_array,
			    PVFS_ds_type type,
			    void *hint,
			    void *user_ptr,
			    job_aint status_user_tag,
			    job_status_s * out_status_p,
			    job_id_t * id,
			    job_context_id context_id)
{
    /* post a dspace create.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->u.trove.handle = 0;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_dspace_create(coll_id,
                              handle_extent_array,
			      &(jd->u.trove.handle),
			      type,
			      hint, TROVE_SYNC /* flags -- sync for now */ ,
			      user_ptr_internal, 
			      global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->handle = jd->u.trove.handle;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_dspace_remove()
 *
 * remove an entire data space object (byte stream and key/value) 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_dspace_remove(PVFS_fs_id coll_id,
			    PVFS_handle handle,
			    void *user_ptr,
			    job_aint status_user_tag,
			    job_status_s * out_status_p,
			    job_id_t * id,
			    job_context_id context_id)
{
    /* post a dspace remove.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_dspace_remove(coll_id,
			      handle, TROVE_SYNC /* flags -- sync for now */ ,
			      user_ptr_internal, 
			      global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_dspace_verify()
 *
 * verify that a given dataspace exists and discover its type 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_dspace_verify(PVFS_fs_id coll_id,
			    PVFS_handle handle,
			    void *user_ptr,
			    job_aint status_user_tag,
			    job_status_s * out_status_p,
			    job_id_t * id,
			    job_context_id context_id)
{
    /* post a dspace verify.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_dspace_verify(coll_id,
			      handle, &jd->u.trove.type, 
			      TROVE_SYNC /* flags -- sync for now */ ,
			      user_ptr_internal, global_trove_context, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_fs_create()
 *
 * create a new file system
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_fs_create(char *collname,
			PVFS_fs_id new_coll_id,
			void *user_ptr,
			job_aint status_user_tag,
			job_status_s * out_status_p,
			job_id_t * id,
			job_context_id context_id)
{
    /* post an fs create.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_collection_create(collname, new_coll_id, user_ptr_internal, 
	&(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_fs_remove()
 *
 * remove an existing file system 
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_fs_remove(char *collname,
			void *user_ptr,
			job_aint status_user_tag,
			job_status_s * out_status_p,
			job_id_t * id,
			job_context_id context_id)
{
    gossip_lerr("Error: unimplemented.\n");
    return (-ENOSYS);
}

/* job_trove_fs_lookup()
 *
 * lookup a file system based on a string name
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_fs_lookup(char *collname,
			void *user_ptr,
			job_aint status_user_tag,
			job_status_s * out_status_p,
			job_id_t * id,
			job_context_id context_id)
{
    /* post a collection lookup.  If it completes (or fails) immediately, then
     * return and fill in the status structure.  If it needs to be tested
     * for completion later, then queue up a job_desc structure.
     */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_collection_lookup(collname, &(jd->u.trove.fsid), 
	user_ptr_internal, &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	out_status_p->coll_id = jd->u.trove.fsid;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* there is no way we can test on this if we don't know the coll_id */
    gossip_lerr("Error: trove_collection_lookup() returned 0 ???\n");

    return (-EINVAL);
}

/* job_trove_fs_set_eattr()
 *
 * sets extended attributes for a file system
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_fs_seteattr(PVFS_fs_id coll_id,
			  PVFS_ds_keyval * key_p,
			  PVFS_ds_keyval * val_p,
			  PVFS_ds_flags flags,
			  void *user_ptr,
			  job_aint status_user_tag,
			  job_status_s * out_status_p,
			  job_id_t * id,
			  job_context_id context_id)
{
    /* post a trove collection set eattr.  If it completes (or fails)
     * immediately, then return and fill in the status structure.  
     * If it needs to be tested for completion later, then queue 
     * up a job_desc structure.  */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_collection_seteattr(coll_id, key_p, val_p, flags,
				    user_ptr_internal, global_trove_context,
				    &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}

/* job_trove_fs_get_eattr()
 *
 * gets extended attributes for a file system
 *
 * returns 0 on success, 1 on immediate completion, and -errno on
 * failure
 */
int job_trove_fs_geteattr(PVFS_fs_id coll_id,
			  PVFS_ds_keyval * key_p,
			  PVFS_ds_keyval * val_p,
			  PVFS_ds_flags flags,
			  void *user_ptr,
			  job_aint status_user_tag,
			  job_status_s * out_status_p,
			  job_id_t * id,
			  job_context_id context_id)
{
    /* post a trove collection get eattr.  If it completes (or fails)
     * immediately, then return and fill in the status structure.  
     * If it needs to be tested for completion later, then queue 
     * up a job_desc structure.  */
    int ret = -1;
    struct job_desc *jd = NULL;
    void* user_ptr_internal;

    /* create the job desc first, even though we may not use it.  This
     * gives us somewhere to store the BMI id and user ptr
     */
    jd = alloc_job_desc(JOB_TROVE);
    if (!jd)
    {
	return (-errno);
    }
    jd->job_user_ptr = user_ptr;
    jd->context_id = context_id;
    jd->status_user_tag = status_user_tag;
#if __PVFS2_JOB_THREADED__
    jd->trove_callback.fn = trove_thread_mgr_callback;
    jd->trove_callback.data = (void*)jd;
    user_ptr_internal = &jd->trove_callback;
#else
    user_ptr_internal = jd;
#endif

#ifdef __PVFS2_TROVE_SUPPORT__
    ret = trove_collection_geteattr(coll_id, key_p, val_p, flags,
				    user_ptr_internal, global_trove_context,
				    &(jd->u.trove.id));
#else
    gossip_err("Error: Trove support not enabled.\n");
    ret = -ENOSYS;
#endif

    if (ret < 0)
    {
	/* error posting trove operation */
	dealloc_job_desc(jd);
	/* TODO: handle this correctly */
	out_status_p->error_code = -EINVAL;
	out_status_p->status_user_tag = status_user_tag;
	return (1);
    }

    if (ret == 1)
    {
	/* immediate completion */
	out_status_p->error_code = 0;
	out_status_p->status_user_tag = status_user_tag;
	dealloc_job_desc(jd);
	return (ret);
    }

    /* if we fall through to this point, the job did not
     * immediately complete and we must queue up to test later
     */
    gen_mutex_lock(&trove_mutex);
    *id = jd->job_id;
    trove_pending_count++;
    gen_mutex_unlock(&trove_mutex);

    return (0);
}


/* job_test()
 *
 * check for completion of a particular job, don't return until
 * either job completes or timeout expires 
 *
 * returns 0 on success, -errno on failure 
 */
int job_test(job_id_t id,
	     int *out_count_p,
	     void **returned_user_ptr_p,
	     job_status_s * out_status_p,
	     int timeout_ms,
	     job_context_id context_id)
{
    int ret = -1;
#ifdef __PVFS2_JOB_THREADED__
    struct timespec pthread_timeout;
#else
    int num_completed;
#endif /* __PVFS2_JOB_THREADED__ */
    int timeout_remaining = timeout_ms;
    struct timeval start;
    struct timeval end;
    struct job_desc* query = NULL;

    *out_count_p = 0;

    assert(id != 0);

    /* TODO: this implementation is going to be really clumsy for
     * now, just to get us through with correct semantics.  It will
     * search the completion queue way more than it needs to,
     * because I haven't implemented an intelligent way to only
     * look if the job you were interested in completed.
     */

    /* TODO: here is another cheap shot.  I don't want to special
     * case the -1 (infinite) timeout possibility right now (maybe
     * later), but I want the semantics to work.  So.. if that's the
     * timeout I get, then set the remaining time to the maximum
     * value that an integer can take on.  That will hold it in
     * this function for nearly a month. 
     */
    if (timeout_ms == -1)
	timeout_remaining = INT_MAX;

    /* use this as a chance to do a cheap test on the request
     * scheduler
     */
    if ((ret = do_one_test_cycle_req_sched()) < 0)
    {
	return (ret);
    }

    /* look to see if desired job is already done */
    gen_mutex_lock(&completion_mutex);
    if(completion_error)
    {
	gen_mutex_unlock(&completion_mutex);
	return(completion_error);
    }
    query = id_gen_fast_lookup(id);
    if(query->completed_flag)
    {
	job_desc_q_remove(query);
	gen_mutex_unlock(&completion_mutex);
	goto job_test_complete;
    }
    gen_mutex_unlock(&completion_mutex);

    /* bail out if timeout is zero */
    if (!timeout_ms)
	return (0);

    /* if we fall through to this point, then we need to just try
     * to eat up the timeout until the job that we want hits the
     * completion queue
     */
    do
    {
	ret = gettimeofday(&start, NULL);
	if (ret < 0)
	    return (ret);

#ifdef __PVFS2_JOB_THREADED__
	/* figure out how long to wait */
	pthread_timeout.tv_sec = start.tv_sec + timeout_remaining / 1000;
	pthread_timeout.tv_nsec = start.tv_usec * 1000 +
	    ((timeout_remaining % 1000) * 1000000);
	if (pthread_timeout.tv_nsec > 1000000000)
	{
	    pthread_timeout.tv_nsec = pthread_timeout.tv_nsec - 1000000000;
	    pthread_timeout.tv_sec++;
	}

	/* wait to see if anything completes before the timeout
	 * expires 
	 */
	gen_mutex_lock(&completion_mutex);
	ret = pthread_cond_timedwait(&completion_cond, &completion_mutex,
				     &pthread_timeout);
	gen_mutex_unlock(&completion_mutex);
	if (ret == ETIMEDOUT)
	{
	    /* nothing completed while we were waiting, trust that the
	     * timedwait got the timing right
	     */
	    *out_count_p = 0;
	    return (0);
	}
	else if (ret != 0 && ret != EINTR)
	{
	    /* error */
	    return (-ret);
	}
	else
	{
	    /* check now to see if the job we want is done */
	    gen_mutex_lock(&completion_mutex);
	    if(completion_error)
	    {
		gen_mutex_unlock(&completion_mutex);
		return(completion_error);
	    }
	    query = id_gen_fast_lookup(id);
	    if(query->completed_flag)
	    {
		job_desc_q_remove(query);
		gen_mutex_unlock(&completion_mutex);
		goto job_test_complete;
	    }
	    gen_mutex_unlock(&completion_mutex);
	}

#else /* __PVFS2_JOB_THREADED__ */

	/* push on work for one round */
	ret = do_one_work_cycle_all(&num_completed, 1);
	if (ret < 0)
	{
	    return (ret);
	}
	if (num_completed > 0)
	{
	    /* check now to see if the job we want is done */
	    gen_mutex_lock(&completion_mutex);
	    if(completion_error)
	    {
		gen_mutex_unlock(&completion_mutex);
		return(completion_error);
	    }
	    query = id_gen_fast_lookup(id);
	    if(query->completed_flag)
	    {
		job_desc_q_remove(query);
		gen_mutex_unlock(&completion_mutex);
		goto job_test_complete;
	    }
	    gen_mutex_unlock(&completion_mutex);
	}

#endif /* __PVFS2_JOB_THREADED__ */

	/* if we fall to here, see how much time has expired and
	 * sleep/work again if we need to
	 */
	ret = gettimeofday(&end, NULL);
	if (ret < 0)
	    return (ret);

	timeout_remaining -= (end.tv_sec - start.tv_sec) * 1000 +
	    (end.tv_usec - start.tv_usec) / 1000;

    } while (timeout_remaining > 0);

    /* fall through, nothing done, time is used up */
    *out_count_p = 0;
    return (0);

job_test_complete:
    *out_count_p = 1;
    fill_status(query, returned_user_ptr_p, out_status_p);
    /* special case for request scheduler */
    if (query->type == JOB_REQ_SCHED && query->u.req_sched.post_flag == 1)
    {
	/* in this case, _don't_ delete the job desc, we need it later
	 * to release entry
	 */
    }
    else
    {
	dealloc_job_desc(query);
    }
    return(1);
}


/* job_testsome()
 *
 * check for completion of a set of jobs, don't return until
 * either all jobs complete or timeout expires 
 *
 * returns 0 on success, -errno on failure
 */
int job_testsome(job_id_t * id_array,
		 int *inout_count_p,
		 int *out_index_array,
		 void **returned_user_ptr_array,
		 job_status_s * out_status_array_p,
		 int timeout_ms,
		 job_context_id context_id)
{
    int ret = -1;
#ifdef __PVFS2_JOB_THREADED__
    struct timespec pthread_timeout;
#else
    int num_completed;
#endif /* __PVFS2_JOB_THREADED__ */
    int timeout_remaining = timeout_ms;
    struct timeval start;
    struct timeval end;
    int total_completed = 0;
    int original_count = *inout_count_p;
    int real_id_count = 0;
    job_id_t *tmp_id_array = NULL;
    int i;

    /* TODO: this implementation is going to be really clumsy for
     * now, just to get us through with correct semantics.  It will
     * search the completion queue way more than it needs to,
     * because I haven't implemented an intelligent way to only
     * look if the job you were interested in completed.
     */

    /* count how many of the id's are non zero */
    for (i = 0; i < original_count; i++)
    {
	if (id_array[i])
	    real_id_count++;
    }
    if (!real_id_count)
    {
	gossip_lerr("job_testsome() called with nothing to do.\n");
	return (-EINVAL);
    }

    /* TODO: here is another cheap shot.  I don't want to special
     * case the -1 (infinite) timeout possibility right now (maybe
     * later), but I want the semantics to work.  So.. if that's the
     * timeout I get, then set the remaining time to the maximum
     * value that an integer can take on.  That will hold it in
     * this function for nearly a month. 
     */
    if (timeout_ms == -1)
	timeout_remaining = INT_MAX;

    /* use this as a chance to do a cheap test on the request
     * scheduler
     */
    if ((ret = do_one_test_cycle_req_sched()) < 0)
    {
	return (ret);
    }

    /* need to duplicate the id array, so that I have a copy I can
     * modify
     */
    tmp_id_array = (job_id_t *) malloc(original_count * sizeof(job_id_t));
    if (!tmp_id_array)
    {
	return (-errno);
    }
    memcpy(tmp_id_array, id_array, (original_count * sizeof(job_id_t)));

    /* check before we do anything else to see if the job that we
     * want is in the completion queue
     */
    ret = completion_query_some(tmp_id_array,
				inout_count_p, out_index_array,
				returned_user_ptr_array,
				out_status_array_p);

    /* return here on error or completion */
    if (ret < 0)
    {
	free(tmp_id_array);
	return (ret);
    }
    if (ret == 0 && (*inout_count_p == real_id_count))
    {
	free(tmp_id_array);
	return (1);
    }

    /* bail out if timeout is zero */
    if (!timeout_ms)
    {
	free(tmp_id_array);
	return (0);
    }

    for (i = 0; i < (*inout_count_p); i++)
	tmp_id_array[out_index_array[i]] = 0;
    total_completed += *inout_count_p;
    *inout_count_p = original_count;

    /* if we fall through to this point, then we need to just try
     * to eat up the timeout until the jobs that we want hit the
     * completion queue
     */
    do
    {
	ret = gettimeofday(&start, NULL);
	if (ret < 0)
	{
	    free(tmp_id_array);
	    return (ret);
	}

#ifdef __PVFS2_JOB_THREADED__
	/* figure out how long to wait */
	pthread_timeout.tv_sec = start.tv_sec + timeout_remaining / 1000;
	pthread_timeout.tv_nsec = start.tv_usec * 1000 +
	    ((timeout_remaining % 1000) * 1000000);
	if (pthread_timeout.tv_nsec > 1000000000)
	{
	    pthread_timeout.tv_nsec = pthread_timeout.tv_nsec - 1000000000;
	    pthread_timeout.tv_sec++;
	}

	/* wait to see if anything completes before the timeout
	 * expires 
	 */
	gen_mutex_lock(&completion_mutex);
	ret = pthread_cond_timedwait(&completion_cond, &completion_mutex,
				     &pthread_timeout);
	gen_mutex_unlock(&completion_mutex);
	if (ret == ETIMEDOUT)
	{
	    /* nothing completed while we were waiting, trust that the
	     * timedwait got the timing right
	     */
	    free(tmp_id_array);
	    *inout_count_p = total_completed;
	    return (0);
	}
	else if (ret != 0 && ret != EINTR)
	{
	    /* error */
	    free(tmp_id_array);
	    return (-ret);
	}
	else
	{
	    if(returned_user_ptr_array)
	    {
		ret = completion_query_some(tmp_id_array,
					    inout_count_p,
					    &out_index_array[total_completed],
					    &returned_user_ptr_array
					    [total_completed],
					    &out_status_array_p
					    [total_completed]);
	    }
	    else
	    {
		ret = completion_query_some(tmp_id_array,
					    inout_count_p,
					    &out_index_array[total_completed],
					    NULL,
					    &out_status_array_p
					    [total_completed]);
	    }

	    /* return here on error or completion */
	    if (ret < 0)
	    {
		free(tmp_id_array);
		return (ret);
	    }
	    if (ret == 0 && (*inout_count_p + total_completed == real_id_count))
	    {
		*inout_count_p = real_id_count;
		free(tmp_id_array);
		return (1);
	    }

	    for (i = 0; i < (*inout_count_p); i++)
		tmp_id_array[out_index_array[i + total_completed]] = 0;
	    total_completed += *inout_count_p;
	    *inout_count_p = original_count;
	}

#else /* __PVFS2_JOB_THREADED__ */

	/* push on work for one round */
	ret = do_one_work_cycle_all(&num_completed, 1);
	if (ret < 0)
	{
	    free(tmp_id_array);
	    return (ret);
	}
	if (num_completed > 0)
	{
	    /* check queue now to see of the op we want is done */
	    if(returned_user_ptr_array)
	    {
		ret = completion_query_some(tmp_id_array,
					    inout_count_p,
					    &out_index_array[total_completed],
					    &returned_user_ptr_array
					    [total_completed],
					    &out_status_array_p
					    [total_completed]);
	    }
	    else
	    {
		ret = completion_query_some(tmp_id_array,
					    inout_count_p,
					    &out_index_array[total_completed],
					    NULL,
					    &out_status_array_p
					    [total_completed]);
	    }
	    
	    /* return here on error or completion */
	    if (ret < 0)
	    {
		free(tmp_id_array);
		return (ret);
	    }
	    if (ret == 0 && (*inout_count_p + total_completed == real_id_count))
	    {
		*inout_count_p = real_id_count;
		free(tmp_id_array);
		return (1);
	    }

	    for (i = 0; i < (*inout_count_p); i++)
		tmp_id_array[out_index_array[i + total_completed]] = 0;
	    total_completed += *inout_count_p;
	    *inout_count_p = original_count;
	}

#endif /* __PVFS2_JOB_THREADED__ */

	/* if we fall to here, see how much time has expired and
	 * sleep/work again if we need to
	 */
	ret = gettimeofday(&end, NULL);
	if (ret < 0)
	{
	    free(tmp_id_array);
	    return (ret);
	}

	timeout_remaining -= (end.tv_sec - start.tv_sec) * 1000 +
	    (end.tv_usec - start.tv_usec) / 1000;

    } while (timeout_remaining > 0);

    /* fall through, not everything is done, time is used up */
    *inout_count_p = total_completed;
    free(tmp_id_array);
    if (total_completed > 0)
	return (1);
    else
	return (0);
}

/* job_testcontext()
 *
 * check for completion of any jobs currently in progress.  Don't return
 * until either at least one job has completed or the timeout has
 * expired
 *
 * returns 0 on success, -errno on failure
 */
int job_testcontext(job_id_t * out_id_array_p,
		  int *inout_count_p,
		  void **returned_user_ptr_array,
		  job_status_s * out_status_array_p,
		  int timeout_ms,
		  job_context_id context_id)
{
    int ret = -1;
#ifdef __PVFS2_JOB_THREADED__
    struct timespec pthread_timeout;
#else
    int num_completed;
#endif /* __PVFS2_JOB_THREADED__ */
    int timeout_remaining = timeout_ms;
    struct timeval start;
    struct timeval end;
    int original_count = *inout_count_p;

    /* TODO: this implementation is going to be really clumsy for
     * now, just to get us through with correct semantics.  It will
     * search the completion queue way more than it needs to,
     * because I haven't implemented an intelligent way to only
     * look if the job you were interested in completed.
     */

    /* TODO: here is another cheap shot.  I don't want to special
     * case the -1 (infinite) timeout possibility right now (maybe
     * later), but I want the semantics to work.  So.. if that's the
     * timeout I get, then set the remaining time to the maximum
     * value that an integer can take on.  That will hold it in
     * this function for nearly a month. 
     */
    if (timeout_ms == -1)
	timeout_remaining = INT_MAX;

    /* use this as a chance to do a cheap test on the request
     * scheduler
     */
    if ((ret = do_one_test_cycle_req_sched()) < 0)
    {
	return (ret);
    }

    /* check before we do anything else to see if the completion queue
     * has anything in it
     */
    ret = completion_query_context(out_id_array_p,
				 inout_count_p,
				 returned_user_ptr_array,
				 out_status_array_p, context_id);
    /* return here on error or completion */
    if (ret < 0)
	return (ret);
    if (ret == 0 && (*inout_count_p > 0))
	return (1);

    /* bail out if timeout is zero */
    if (!timeout_ms)
    {
	return (0);
    }

    *inout_count_p = original_count;

    /* if we fall through to this point, then we need to just try
     * to eat up the timeout until the jobs that we want hit the
     * completion queue
     */
    do
    {
	ret = gettimeofday(&start, NULL);
	if (ret < 0)
	{
	    return (ret);
	}

#ifdef __PVFS2_JOB_THREADED__
	/* figure out how long to wait */
	pthread_timeout.tv_sec = start.tv_sec + timeout_remaining / 1000;
	pthread_timeout.tv_nsec = start.tv_usec * 1000 +
	    ((timeout_remaining % 1000) * 1000000);
	if (pthread_timeout.tv_nsec > 1000000000)
	{
	    pthread_timeout.tv_nsec = pthread_timeout.tv_nsec - 1000000000;
	    pthread_timeout.tv_sec++;
	}

	/* wait to see if anything completes before the timeout
	 * expires 
	 */
	gen_mutex_lock(&completion_mutex);
	ret = pthread_cond_timedwait(&completion_cond, &completion_mutex,
				     &pthread_timeout);
	gen_mutex_unlock(&completion_mutex);
	if (ret == ETIMEDOUT)
	{
	    /* nothing completed while we were waiting, trust that the
	     * timedwait got the timing right
	     */
	    *inout_count_p = 0;
	    return (0);
	}
	else if (ret != 0 && ret != EINTR)
	{
	    /* error */
	    return (-ret);
	}
	else
	{
	    /* check queue now to see if anything is done */
	    ret = completion_query_context(out_id_array_p,
					 inout_count_p,
					 returned_user_ptr_array,
					 out_status_array_p,
					 context_id);
	    /* return here on error or completion */
	    if (ret < 0)
		return (ret);
	    if (ret == 0 && (*inout_count_p > 0))
		return (1);

	    *inout_count_p = original_count;
	}

#else /* __PVFS2_JOB_THREADED__ */

	/* push on work for one round */
	ret = do_one_work_cycle_all(&num_completed, 1);
	if (ret < 0)
	{
	    return (ret);
	}
	if (num_completed > 0)
	{
	    /* check queue now to see if anything is done */
	    ret = completion_query_context(out_id_array_p,
					 inout_count_p,
					 returned_user_ptr_array,
					 out_status_array_p,
					 context_id);
	    /* return here on error or completion */
	    if (ret < 0)
		return (ret);
	    if (ret == 0 && (*inout_count_p > 0))
		return (1);

	    *inout_count_p = original_count;
	}

#endif /* __PVFS2_JOB_THREADED__ */

	/* if we fall to here, see how much time has expired and
	 * sleep/work again if we need to
	 */
	ret = gettimeofday(&end, NULL);
	if (ret < 0)
	{
	    return (ret);
	}

	timeout_remaining -= (end.tv_sec - start.tv_sec) * 1000 +
	    (end.tv_usec - start.tv_usec) / 1000;

    } while (timeout_remaining > 0);

    /* fall through, nothing done, time is used up */
    *inout_count_p = 0;

    return (0);
}


/*********************************************************
 * Internal utility functions
 */


/* setup_queues()
 *
 * initializes all of the job queues needed by the interface
 *
 * returns 0 on success, -errno on failure
 */
static int setup_queues(void)
{

    bmi_unexp_queue = job_desc_q_new();
    dev_unexp_queue = job_desc_q_new();

    if (!bmi_unexp_queue || !dev_unexp_queue)
    {
	/* cleanup any that were initialized */
	teardown_queues();
	return (-ENOMEM);
    }
    return (0);
}

/* teardown_queues()
 *
 * tears down any existing queues used by the job interface
 *
 * no return value
 */
static void teardown_queues(void)
{

    if (bmi_unexp_queue)
	job_desc_q_cleanup(bmi_unexp_queue);
    if (dev_unexp_queue)
	job_desc_q_cleanup(dev_unexp_queue);

    return;
}

#ifndef __PVFS2_JOB_THREADED__
/* do_one_work_cycle_all()
 *
 * performs one job work cycle (checks to see which jobs are pending,
 * tests those that neede it, etc)
 *
 * returns 0 on success, -errno on failure
 */
static int do_one_work_cycle_all(int *num_completed,
				 int wait_flag)
{
    int bmi_pending_flag, bmi_unexp_pending_flag, flow_pending_flag, 
	trove_pending_flag, dev_unexp_pending_flag;
    int bmi_completed = 0;
    int bmi_unexp_completed = 0;
    int flow_completed = 0;
    int trove_completed = 0;
    int dev_unexp_completed = 0;
    int total_interfaces_pending = 0;
    int ret = -1;

    /* the first thing to do is to determine which interfaces have jobs
     * pending
     */
    gen_mutex_lock(&bmi_mutex);
	if(bmi_pending_count > 0)
	    bmi_pending_flag = 1;
	else
	    bmi_pending_flag = 0;
	if(bmi_unexp_pending_count > 0)
	    bmi_unexp_pending_flag = 1;
	else
	    bmi_unexp_pending_flag = 0;
    gen_mutex_unlock(&bmi_mutex);
    gen_mutex_lock(&flow_mutex);
	if(flow_pending_count > 0)
	    flow_pending_flag = 1;
	else
	    flow_pending_flag = 0;
    gen_mutex_unlock(&flow_mutex);
    gen_mutex_lock(&trove_mutex);
	if(trove_pending_count > 0)
	    trove_pending_flag = 1;
	else
	    trove_pending_flag = 0;
    gen_mutex_unlock(&trove_mutex);
    gen_mutex_lock(&dev_mutex);
	if(dev_unexp_pending_count > 0)
	    dev_unexp_pending_flag = 1;
	else
	    dev_unexp_pending_flag = 0;
    gen_mutex_unlock(&dev_mutex);

    /* count the number of interfaces with jobs pending */
    total_interfaces_pending = bmi_pending_flag + bmi_unexp_pending_flag 
	+ flow_pending_flag + trove_pending_flag + dev_unexp_pending_flag;

    /* TODO: need to check return values in here! */

    /* handle BMI first */
    if (bmi_pending_flag && (total_interfaces_pending == 1) && wait_flag)
    {
	/* nothing else going on, we can afford to wait */
	if ((ret = do_one_work_cycle_bmi(&bmi_completed, 1)) < 0)
	{
	    return (ret);
	}
    }
    else if (bmi_pending_flag)
    {
	/* just test */
	if ((ret = do_one_work_cycle_bmi(&bmi_completed, 0)) < 0)
	{
	    return (ret);
	}
    }

    /* now check for unexpected BMI messages */
    if (bmi_unexp_pending_flag && (total_interfaces_pending == 1) && wait_flag)
    {
	if ((ret = do_one_work_cycle_bmi_unexp(&bmi_unexp_completed, 1)) < 0)
	{
	    return (ret);
	}
    }
    else if (bmi_unexp_pending_flag)
    {
	if ((ret = do_one_work_cycle_bmi_unexp(&bmi_unexp_completed, 0)) < 0)
	{
	    return (ret);
	}
    }

#ifdef __PVFS2_TROVE_SUPPORT__
    /* trove operations */
    if (trove_pending_flag)
    {
	if ((ret = do_one_work_cycle_trove(&trove_completed)) < 0)
	{
	    return (ret);
	}
    }
#endif

    /* flow operations */
    if (flow_pending_flag)
    {
	if ((ret = do_one_work_cycle_flow(&flow_completed)) < 0)
	{
	    return (ret);
	}
    }

    /* device operations */
    if (dev_unexp_pending_flag)
    {
	if ((ret = do_one_work_cycle_dev_unexp(&dev_unexp_completed)) < 0)
	{
	    return(ret);
	}
    }

    *num_completed = (flow_completed + trove_completed + bmi_completed +
		      bmi_unexp_completed + dev_unexp_completed);

    return (0);
}

/* do_one_work_cycle_bmi_unexp()
 *
 * performs one job work cycle, just on pending BMI unexpected operations 
 * (checks to see which jobs are pending, tests those that need it, etc)
 *
 * returns 0 on success, -errno on failure
 */
static int do_one_work_cycle_bmi_unexp(int *num_completed,
				       int wait_flag)
{
    int incount, outcount;
    struct job_desc *tmp_desc;
    int ret = -1;
    int i = 0;

    /* the first thing to do is to find out how many unexpected
     * operations we are allowed to look for right now
     */
    gen_mutex_lock(&bmi_mutex);
    if(bmi_unexp_pending_count > job_work_metric)
	incount = job_work_metric;
    else
	incount = bmi_unexp_pending_count;
    gen_mutex_unlock(&bmi_mutex);

    if (wait_flag)
    {
	ret = BMI_testunexpected(incount, &outcount, stat_bmi_unexp_array, 10);
    }
    else
    {
	ret = BMI_testunexpected(incount, &outcount, stat_bmi_unexp_array, 0);
    }

    if (ret < 0)
    {
	/* critical failure */
	/* TODO: can I clean up anything else here? */
	gossip_lerr("Error: critical BMI failure.\n");
	return (ret);
    }

    for (i = 0; i < outcount; i++)
    {
	/* remove the operation from the pending bmi_unexp queue */
	gen_mutex_lock(&bmi_mutex);
	tmp_desc = job_desc_q_shownext(bmi_unexp_queue);
	job_desc_q_remove(tmp_desc);
	bmi_unexp_pending_count--;
	gen_mutex_unlock(&bmi_mutex);
	/* set appropriate fields and store incompleted queue */
	*(tmp_desc->u.bmi_unexp.info) = stat_bmi_unexp_array[i];
	gen_mutex_lock(&completion_mutex);
	/* set completed flag while holding queue lock */
	tmp_desc->completed_flag = 1;
	job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
	    tmp_desc);
	gen_mutex_unlock(&completion_mutex);
    }

    *num_completed = outcount;

    return (0);
}


/* do_one_work_cycle_bmi()
 *
 * performs one job work cycle, just on pending BMI operations 
 * (checks to see which jobs are pending, tests those that need it, etc)
 *
 * returns 0 on success, -errno on failure
 */
static int do_one_work_cycle_bmi(int *num_completed,
				 int wait_flag)
{
    struct job_desc *tmp_desc = NULL;
    int incount, outcount;
    int ret = -1;
    int i = 0;

    if(bmi_pending_count > job_work_metric)
	incount = job_work_metric;
    else
	incount = bmi_pending_count;

    if (wait_flag)
    {
	/* nothing else going on, we can afford to wait */
	ret = BMI_testcontext(incount, stat_bmi_id_array, &outcount,
			   stat_bmi_error_code_array,
			   stat_bmi_actual_size_array, stat_bmi_user_ptr_array,
			   10, global_bmi_context);
    }
    else
    {
	/* just test */
	ret = BMI_testcontext(incount, stat_bmi_id_array, &outcount,
			   stat_bmi_error_code_array,
			   stat_bmi_actual_size_array, stat_bmi_user_ptr_array,
			   0, global_bmi_context);
    }

    if (ret < 0)
    {
	/* critical failure */
	/* TODO: can I clean up anything else here? */
	gossip_lerr("Error: critical BMI failure.\n");
	return (ret);
    }

    bmi_pending_count -= outcount;

    for (i = 0; i < outcount; i++)
    {
	tmp_desc = (struct job_desc *) stat_bmi_user_ptr_array[i];
	/* set appropriate fields and store in completed queue */
	tmp_desc->u.bmi.error_code = stat_bmi_error_code_array[i];
	tmp_desc->u.bmi.actual_size = stat_bmi_actual_size_array[i];
	gen_mutex_lock(&completion_mutex);
	job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
	    tmp_desc);
	/* set completed flag while holding queue lock */
	tmp_desc->completed_flag = 1;
	gen_mutex_unlock(&completion_mutex);
    }

    *num_completed = outcount;
    return (0);
}

#endif /* __PVFS2_JOB_THREADED__ */

#ifdef __PVFS2_JOB_THREADED__
/* trove_thread_mgr_callback()
 *
 * callback function executed by the thread manager for Trove when a Trove 
 * job completes
 *
 * no return value
 */
static void trove_thread_mgr_callback(void* data, 
    PVFS_error error_code)
{
    struct job_desc* tmp_desc = (struct job_desc*)data; 

    /* set job descriptor fields and put into completion queue */
    tmp_desc->u.trove.state = error_code;
    gen_mutex_lock(&completion_mutex);
    job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
	tmp_desc);
    /* set completed flag while holding queue lock */
    tmp_desc->completed_flag = 1;
    gen_mutex_unlock(&completion_mutex);

    trove_pending_count--;

    /* wake up anyone waiting for completion */
    pthread_cond_signal(&completion_cond);
    return;
}
#endif

#ifdef __PVFS2_JOB_THREADED__
/* bmi_thread_mgr_callback()
 *
 * callback function executed by the thread manager for BMI when a BMI
 * job completes
 *
 * no return value
 */
static void bmi_thread_mgr_callback(void* data, 
    PVFS_size actual_size,
    PVFS_error error_code)
{
    struct job_desc* tmp_desc = (struct job_desc*)data; 

    /* set job descriptor fields and put into completion queue */
    tmp_desc->u.bmi.error_code = error_code;
    tmp_desc->u.bmi.actual_size = actual_size;
    gen_mutex_lock(&completion_mutex);
    job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
	tmp_desc);
    /* set completed flag while holding queue lock */
    tmp_desc->completed_flag = 1;
    gen_mutex_unlock(&completion_mutex);

    bmi_pending_count--;

    /* wake up anyone waiting for completion */
    pthread_cond_signal(&completion_cond);

    return;
}

/* bmi_thread_mgr_unexp_handler()
 *
 * callback function executed by the thread manager for BMI when an unexpected 
 * BMI message arrives
 *
 * no return value
 */
static void bmi_thread_mgr_unexp_handler(struct BMI_unexpected_info* unexp)
{
    struct job_desc* tmp_desc = NULL; 

    /* remove the operation from the pending bmi_unexp queue */
    gen_mutex_lock(&bmi_mutex);
    tmp_desc = job_desc_q_shownext(bmi_unexp_queue);
    assert(tmp_desc != NULL);	/* TODO: fix this */
    job_desc_q_remove(tmp_desc);
    bmi_unexp_pending_count--;
    gen_mutex_unlock(&bmi_mutex);
    /* set appropriate fields and store in completed queue */
    *(tmp_desc->u.bmi_unexp.info) = *unexp;
    gen_mutex_lock(&completion_mutex);
    /* set completed flag while holding queue lock */
    tmp_desc->completed_flag = 1;
    assert(completion_queue_array[tmp_desc->context_id] != 0);
    job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
	tmp_desc);
    gen_mutex_unlock(&completion_mutex);

    /* wake up anyone waiting for completion */
    pthread_cond_signal(&completion_cond);
;
    return;
}
#endif /* __PVFS2_JOB_THREADED__ */

/* do_one_work_cycle_dev_unexp()
 *
 * performs one job work cycle, just on pending device operations 
 * (checks to see which jobs are pending, tests those that need it, etc)
 *
 * returns 0 on success, -errno on failure
 */
static int do_one_work_cycle_dev_unexp(int *num_completed)
{
    int ret = -1;
    struct job_desc *tmp_desc = NULL;
    int incount, outcount;
    int i = 0;

    /* the first thing to do is to find out how many unexpected
     * operations we are allowed to look for right now
     */
    gen_mutex_lock(&dev_mutex);
    if(dev_unexp_pending_count > job_work_metric)
	incount = job_work_metric;
    else
	incount = dev_unexp_pending_count;
    gen_mutex_unlock(&dev_mutex);

    ret = PINT_dev_test_unexpected(incount, &outcount, 
	stat_dev_unexp_array, 10);

    if (ret < 0)
    {
	/* critical failure */
	/* TODO: can I clean up anything else here? */
	gossip_lerr("Error: critical device failure.\n");
	return (ret);
    }

    for (i = 0; i < outcount; i++)
    {
	/* remove the operation from the pending dev_unexp queue */
	gen_mutex_lock(&dev_mutex);
	tmp_desc = job_desc_q_shownext(dev_unexp_queue);
	job_desc_q_remove(tmp_desc);
	dev_unexp_pending_count--;
	gen_mutex_unlock(&dev_mutex);
	/* set appropriate fields and store incompleted queue */
	*(tmp_desc->u.dev_unexp.info) = stat_dev_unexp_array[i];
	gen_mutex_lock(&completion_mutex);
	/* set completed flag while holding queue lock */
	tmp_desc->completed_flag = 1;
	job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
	    tmp_desc);
	gen_mutex_unlock(&completion_mutex);

    }

    *num_completed = outcount;
    return (0);
}


/* do_one_work_cycle_flow()
 *
 * performs one job work cycle, just on pending flow operations 
 * (checks to see which jobs are pending, tests those that need it, etc)
 *
 * returns 0 on success, -errno on failure
 */
static int do_one_work_cycle_flow(int *num_completed)
{
    int ret = -1;
    struct job_desc *tmp_desc = NULL;
    int incount, outcount;
    int i = 0;

    incount = job_work_metric;

    ret = PINT_flow_testcontext(incount, stat_flow_array, &outcount,
	10, global_flow_context);
    if (ret < 0)
    {
	/* critical failure */
	/* TODO: can I clean up anything else here? */
	gossip_lerr("Error: critical Flow failure.\n");
	return (ret);
    }

    for (i = 0; i < outcount; i++)
    {
	/* remove the operation from the pending flow queue */
	tmp_desc = (struct job_desc *) stat_flow_array[i]->user_ptr;
	/* place in completed queue */
	gen_mutex_lock(&completion_mutex);
	/* set completed flag while holding queue lock */
	tmp_desc->completed_flag = 1;
	job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
	    tmp_desc);
	gen_mutex_unlock(&completion_mutex);
    }

    flow_pending_count -= outcount;

    *num_completed = outcount;
    return (0);
}

#ifndef __PVFS2_JOB_THREADED__
#ifdef __PVFS2_TROVE_SUPPORT__
/* do_one_work_cycle_trove()
 *
 * performs one job work cycle, just on pending trove operations 
 * (checks to see which jobs are pending, tests those that need it, etc)
 *
 * returns 0 on success, -errno on failure
 */
static int do_one_work_cycle_trove(int *num_completed)
{
    struct job_desc *tmp_desc = NULL;
    int count = 0;
    int ret = -1;
    int i = 0;

    /* TODO: this is a HACK! */
    /* fix later, I just want to try out testcontext right now */
    PVFS_fs_id HACK_coll_id = 9;

    *num_completed = 0;

    if(trove_pending_count > job_work_metric)
	count = job_work_metric;
    else
	count = trove_pending_count;

    if(count > 0)
    {
	/* TODO: fix timeout later */
	ret = trove_dspace_testcontext(HACK_coll_id,
	    stat_trove_id_array,
	    &count,
	    stat_trove_ds_state_array,
	    stat_trove_user_ptr_array,
	    TROVE_DEFAULT_TEST_TIMEOUT,
	    global_trove_context);
	if (ret < 0)
	{
	    /* critical failure */
	    /* TODO: can I clean up anything else here? */
	    gossip_lerr("Error: critical trove failure.\n");
	    return (ret);
	}

	for (i = 0; i < count; i++)
	{
	    tmp_desc = (struct job_desc *) stat_trove_user_ptr_array[i];
	    gen_mutex_lock(&trove_mutex);
	    trove_pending_count--;
	    gen_mutex_unlock(&trove_mutex);
	    /* set appropriate fields and store in completed queue */
	    tmp_desc->u.trove.state = stat_trove_ds_state_array[i];

	    gen_mutex_lock(&completion_mutex);
	    /* set completed flag while holding queue lock */
	    tmp_desc->completed_flag = 1;
	    job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
		tmp_desc);
	    gen_mutex_unlock(&completion_mutex);
	}

	*num_completed = count;
    }

    return(0);
}
#endif
#endif

/* fill_status()
 *
 * fills in the completion status based on the given job descriptor
 *
 * no return value
 */
static void fill_status(struct job_desc *jd,
			void **returned_user_ptr_p,
			job_status_s * status)
{
    status->status_user_tag = jd->status_user_tag;

    if (returned_user_ptr_p)
    {
	*returned_user_ptr_p = jd->job_user_ptr;
    }
    switch (jd->type)
    {
    case JOB_BMI:
	status->error_code = jd->u.bmi.error_code;
	status->actual_size = jd->u.bmi.actual_size;
	break;
    case JOB_BMI_UNEXP:
	status->error_code = jd->u.bmi_unexp.info->error_code;
	status->actual_size = jd->u.bmi_unexp.info->size;
	break;
    case JOB_FLOW:
	status->error_code = jd->u.flow.flow_d->error_code;
	status->actual_size = jd->u.flow.flow_d->total_transfered;
        JOB_EVENT_END(PINT_EVENT_FLOW, status->actual_size, jd->job_id);
	break;
    case JOB_REQ_SCHED:
	status->error_code = jd->u.req_sched.error_code;
	break;
    case JOB_TROVE:
	/* TODO: make this work out for whatever type of trove
	 * operation this is...
	 */
	status->error_code = jd->u.trove.state;
	status->actual_size = jd->u.trove.actual_size;
	status->vtag = jd->u.trove.vtag;
	status->coll_id = jd->u.trove.fsid;
	status->handle = jd->u.trove.handle;
	status->position = jd->u.trove.position;
	status->count = jd->u.trove.count;
	status->ds_attr = jd->u.trove.attr;
	status->type = jd->u.trove.type;
	break;
    case JOB_DEV_UNEXP:
	status->error_code = 0;
	status->actual_size = jd->u.dev_unexp.info->size;
	break;
    case JOB_REQ_SCHED_TIMER:
	status->error_code = jd->u.req_sched.error_code;
	break;
    }

    return;
}

#ifdef __PVFS2_JOB_THREADED__

/* flow_thread_function()
 *
 * function executed by the thread in charge of flows
 */
static void *flow_thread_function(void *ptr)
{
    int flow_pending_flag;
    int num_completed;
    int ret = -1;

    while (1)
    {
	/* figure out if there is any flow work to do */
	gen_mutex_lock(&flow_mutex);
	if(flow_pending_count > 0)
	    flow_pending_flag = 1;
	else
	    flow_pending_flag = 0;
	gen_mutex_unlock(&flow_mutex);

	num_completed = 0;
	if (flow_pending_flag)
	{
	    ret = do_one_work_cycle_flow(&num_completed);
	    if (ret < 0)
	    {
		/* set an error flag to get propigated out later */
		gen_mutex_lock(&completion_mutex);
		completion_error = ret;
		gen_mutex_unlock(&completion_mutex);
	    }
	}
	else
	{
	    /* nothing to do; block on condition */
	    gen_mutex_lock(&flow_mutex);
	    ret = pthread_cond_wait(&flow_cond, &flow_mutex);
	    gen_mutex_unlock(&flow_mutex);
	    if (ret != ETIMEDOUT && ret != EINTR && ret != 0)
	    {
		/* set an error flag to get propigated out later */
		gen_mutex_lock(&completion_mutex);
		completion_error = -ret;
		gen_mutex_unlock(&completion_mutex);
	    }
	}

	if (num_completed > 0)
	{
	    /* signal anyone blocking on completion queue */
	    pthread_cond_signal(&completion_cond);
	}
    }

    return (NULL);
}

/* dev_thread_function()
 *
 * function executed by the thread in charge of the device interface
 */
static void *dev_thread_function(void *ptr)
{
    int dev_unexp_pending_flag;
    int num_completed;
    int ret = -1;

    while (1)
    {
	/* see if there is any device work to do */
	gen_mutex_lock(&dev_mutex);
	if(dev_unexp_pending_count > 0)
	    dev_unexp_pending_flag = 1;
	else
	    dev_unexp_pending_flag = 0;
	gen_mutex_unlock(&dev_mutex);

	num_completed = 0;
	if(dev_unexp_pending_flag)
	{
	    ret = do_one_work_cycle_dev_unexp(&num_completed);
	    if(ret < 0)
	    {
		/* set an error flag to be propigated out later */
		gen_mutex_lock(&completion_mutex);
		completion_error = ret;
		gen_mutex_unlock(&completion_mutex);
	    }
	}
	else
	{
	    /* nothing to do; block on condition */
	    gen_mutex_lock(&dev_mutex);
	    ret = pthread_cond_wait(&dev_cond, &dev_mutex);
	    gen_mutex_unlock(&dev_mutex);
	    if (ret != ETIMEDOUT && ret != EINTR && ret != 0)
	    {
		/* set an error flag to get propigated out later */
		gen_mutex_lock(&completion_mutex);
		completion_error = -ret;
		gen_mutex_unlock(&completion_mutex);
	    }
	}

	if (num_completed > 0)
	{
	    /* signal anyone blocking on completion queue */
	    pthread_cond_signal(&completion_cond);
	}
    }

    return (NULL);
}


#endif /* __PVFS2_JOB_THREADED__ */

/* do_one_test_cycle_req_sched()
 *
 * tests the request scheduler to see if anything has completed.
 * Does not block at all.  
 *
 * returns 0 on success, -errno on failure
 */
static int do_one_test_cycle_req_sched(void)
{
    int ret = -1;
    int count = job_work_metric;
    req_sched_id id_array[job_work_metric];
    void *user_ptr_array[job_work_metric];
    int error_code_array[job_work_metric];
    int i;
    struct job_desc *tmp_desc = NULL;


    ret = PINT_req_sched_testworld(&count, id_array,
				   user_ptr_array, error_code_array);

    if (ret < 0)
    {
	/* critical failure */
	/* TODO: can I clean up anything else here? */
	gossip_lerr("Error: critical BMI failure.\n");
	return (ret);
    }

    for (i = 0; i < count; i++)
    {
	/* remove operation from queue */
	tmp_desc = (struct job_desc *) user_ptr_array[i];
	/* set appropriate fields and place in completed queue */
	tmp_desc->u.req_sched.error_code = error_code_array[i];
	gen_mutex_lock(&completion_mutex);
	/* set completed flag while holding queue lock */
	tmp_desc->completed_flag = 1;
	job_desc_q_add(completion_queue_array[tmp_desc->context_id], 
	    tmp_desc);
	gen_mutex_unlock(&completion_mutex);
    }

    return (0);
}


/* TODO: fill in comment */
static int completion_query_some(job_id_t * id_array,
				 int *inout_count_p,
				 int *out_index_array,
				 void **returned_user_ptr_array,
				 job_status_s * out_status_array_p)
{
    int i;
    struct job_desc *tmp_desc;
    int incount = *inout_count_p;

    *inout_count_p = 0;

    gen_mutex_lock(&completion_mutex);
    if (completion_error)
    {
	gen_mutex_unlock(&completion_mutex);
	return (completion_error);
    }

    for(i=0; i<incount; i++)
    {
	tmp_desc = id_gen_fast_lookup(id_array[i]);
	if(tmp_desc && tmp_desc->completed_flag)
	{
	    if(returned_user_ptr_array)
	    {
		fill_status(tmp_desc,
		    &(returned_user_ptr_array[*inout_count_p]),
		    &(out_status_array_p[*inout_count_p]));
	    }
	    else
	    {
		fill_status(tmp_desc, NULL,
		    &(out_status_array_p[*inout_count_p]));
	    }
	    job_desc_q_remove(tmp_desc);
	    if (tmp_desc->type == JOB_REQ_SCHED &&
		tmp_desc->u.req_sched.post_flag == 1)
	    {
		/* special case; don't delete job desc for req sched
		 * jobs; we need to hang on to them for use when entry
		 * is released
		 */
	    }
	    else
	    {
		dealloc_job_desc(tmp_desc);
	    }
	    out_index_array[*inout_count_p] = i;
	    (*inout_count_p)++;
	}
    }
    gen_mutex_unlock(&completion_mutex);
    return(0);
}

/* TODO: fill in comment */
static int completion_query_context(job_id_t * out_id_array_p,
				  int *inout_count_p,
				  void **returned_user_ptr_array,
				  job_status_s *
				  out_status_array_p,
				  job_context_id context_id)
{
    struct job_desc *query;
    int incount = *inout_count_p;
    *inout_count_p = 0;

    gen_mutex_lock(&completion_mutex);
    if (completion_error)
    {
	gen_mutex_unlock(&completion_mutex);
	return (completion_error);
    }
    while (*inout_count_p < incount && (query =
					job_desc_q_shownext(
					completion_queue_array[context_id])))
    {
	if (returned_user_ptr_array)
	{
	    fill_status(query, &(returned_user_ptr_array[*inout_count_p]),
			&(out_status_array_p[*inout_count_p]));
	}
	else
	{
	    fill_status(query, NULL, &(out_status_array_p[*inout_count_p]));
	}
	out_id_array_p[*inout_count_p] = query->job_id;
	job_desc_q_remove(query);
	(*inout_count_p)++;
	/* special case for request scheduler */
	if (query->type == JOB_REQ_SCHED && query->u.req_sched.post_flag == 1)
	{
	    /* special case; don't delete job desc for req sched
	     * jobs; we need to hang on to them for use when entry
	     * is released
	     */
	}
	else
	{
	    dealloc_job_desc(query);
	}
    }
    gen_mutex_unlock(&completion_mutex);

    return (0);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
