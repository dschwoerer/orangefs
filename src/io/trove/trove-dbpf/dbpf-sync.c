/*
 * (C) 2002 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include "dbpf-op-queue.h"
#include "gossip.h"
#include "pint-perf-counter.h"
#include "dbpf-sync.h"

enum s_sync_context_e{
	COALESCE_CONTEXT_KEYVAL,
	COALESCE_CONTEXT_DSPACE,
	COALESCE_CONTEXT_LAST,
	COALESCE_CONTEXT_NEVER_SYNC
};

static dbpf_sync_context_t sync_array[COALESCE_CONTEXT_LAST][TROVE_MAX_CONTEXTS];

extern dbpf_op_queue_p dbpf_completion_queue_array[TROVE_MAX_CONTEXTS];
extern gen_mutex_t *dbpf_completion_queue_array_mutex[TROVE_MAX_CONTEXTS];
extern pthread_cond_t dbpf_op_completed_cond;

static int dbpf_sync_db(DB * dbp, enum s_sync_context_e sync_context_type, dbpf_sync_context_t * sync_context){
	int ret; 
	gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
          "[SYNC_COALESCE]:\tcoalesce %d sync start in coalesce_queue:%d pending:%d\n",
                 sync_context_type, sync_context->coalesce_counter, 
                 sync_context->sync_counter);
    ret = dbp->sync(dbp, 0);
    if(ret != 0)
    {
        gossip_err("db SYNC failed: %s\n",
                   db_strerror(ret));
        ret = -dbpf_db_error_to_trove_error(ret);
        return ret;
    }
    gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                 "[SYNC_COALESCE]:\tcoalesce %d sync stop\n",
                 sync_context_type);
    return 0;
}

static int dbpf_sync_get_object_sync_context(enum dbpf_op_type type){
	if(! DBPF_OP_DOES_SYNC(type) ){
		return COALESCE_CONTEXT_NEVER_SYNC;	
	}else if(DBPF_OP_IS_KEYVAL(type) ){
		return COALESCE_CONTEXT_KEYVAL;
    }else if(DBPF_OP_IS_DSPACE(type)){
    	return COALESCE_CONTEXT_DSPACE;
    }else{
    	//we should never get here 
    	assert(0);
    }
}

int dbpf_sync_context_init(int context_index)
{
	gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                 "[SYNC_COALESCE]: dbpf_sync_context_init for context %d called\n",
                  context_index);
	int c;
	for(c=0; c < COALESCE_CONTEXT_LAST; c++){
	    bzero(& sync_array[c][context_index], sizeof(dbpf_sync_context_t));

    	sync_array[c][context_index].mutex      = gen_mutex_build();
    	sync_array[c][context_index].sync_queue = dbpf_op_queue_new();
	}
    
    return 0;
}

void dbpf_sync_context_destroy(int context_index)
{
	gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                 "[SYNC_COALESCE]: dbpf_sync_context_destroy for context %d called\n",
                  context_index);	
	int c;
	for(c=0; c < COALESCE_CONTEXT_LAST; c++){
		gen_mutex_lock(sync_array[c][context_index].mutex);
    	gen_mutex_destroy(sync_array[c][context_index].mutex);
    	dbpf_op_queue_cleanup(sync_array[c][context_index].sync_queue);
	}
}
    
int dbpf_sync_coalesce(dbpf_queued_op_t *qop_p)
{
    
    int ret = 0;
	DB * dbp = NULL;
    dbpf_sync_context_t * sync_context;
    dbpf_queued_op_t *ready_op;
    enum s_sync_context_e sync_context_type = dbpf_sync_get_object_sync_context(qop_p->op.type);
    struct dbpf_collection* coll = qop_p->op.coll_p;
     
    gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                 "[SYNC_COALESCE]: sync_coalesce called\n");
	
	if ( sync_context_type == COALESCE_CONTEXT_NEVER_SYNC )
	{ 
		gossip_err("Error in trove-layer, sync coalescence missused!\n");
		assert(0);
	}
	
	if ( ! (qop_p->op.flags & TROVE_SYNC) ) {
		/*
		 * No sync needed at all
		 */
		ret = dbpf_queued_op_complete(qop_p, 0, OP_COMPLETED);
		return 0;
	}
	
	/*
 	* Now we now that this particular op is modifying
 	*/
	sync_context = & sync_array[sync_context_type][qop_p->op.context_id];
	
	if( sync_context_type == COALESCE_CONTEXT_DSPACE ) {
		dbp = qop_p->op.coll_p->ds_db;
	}else if( sync_context_type == COALESCE_CONTEXT_KEYVAL ) {
		dbp = qop_p->op.coll_p->keyval_db;
	}else{
		assert(0);
	}
	
	if ( ! coll->meta_sync_enabled )
		{
			
		ret = dbpf_queued_op_complete(qop_p, 0, OP_COMPLETED);
		/*
		 * Sync periodical if count < lw or if lw = 0 and count > hw 
		 */
		int doSync=0;
		
    	gen_mutex_lock(sync_context->mutex);
		sync_context->coalesce_counter++;
		if( (coll->c_high_watermark > 0 && 
			sync_context->coalesce_counter >= coll->c_high_watermark) 
			|| sync_context->sync_counter < coll->c_low_watermark ){
        	gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                    "[SYNC_COALESCE]:\thigh or low watermark reached:\n"
                    "\t\tcoalesced: %d\n\t\tqueued: %d\n",
                    sync_context->coalesce_counter,
                    sync_context->sync_counter);

			sync_context->coalesce_counter = 0;
			doSync = 1;      				
		}
    	gen_mutex_unlock(sync_context->mutex);
		
		if ( doSync ) {
			ret = dbpf_sync_db(dbp, sync_context_type, sync_context);
		}
			
		return ret;
	}
	
	/*
	 * metadata sync is enabled, either we delay and enqueue this op or we 
	 * coalesync. 
	 */
	gen_mutex_lock(sync_context->mutex);
    if( (sync_context->sync_counter < coll->c_low_watermark) ||
    	( coll->c_high_watermark > 0 && 
    	  sync_context->coalesce_counter >= coll->c_high_watermark ) )
    {
        gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                     "[SYNC_COALESCE]:\thigh or low watermark reached:\n"
                     "\t\tcoalesced: %d\n\t\tqueued: %d\n",
                     sync_context->coalesce_counter,
                     sync_context->sync_counter);

		
        /* sync this operation.  We don't want to use SYNC_IF_NECESSARY
         */
         
        ret = dbpf_sync_db(dbp, sync_context_type, sync_context);

        gen_mutex_lock(
            dbpf_completion_queue_array_mutex[qop_p->op.context_id]);
            
        dbpf_op_queue_add(
            dbpf_completion_queue_array[qop_p->op.context_id], qop_p);
        gen_mutex_lock(&qop_p->mutex);
        qop_p->op.state = OP_COMPLETED;
        qop_p->state = 0;
        gen_mutex_unlock(&qop_p->mutex);


        /* move remaining ops in queue with ready-to-be-synced state
         * to completion queue
         */
        while(!dbpf_op_queue_empty(sync_context->sync_queue))
        {
            ready_op = dbpf_op_queue_shownext(sync_context->sync_queue);
            dbpf_op_queue_remove(ready_op);
            dbpf_op_queue_add(
                dbpf_completion_queue_array[qop_p->op.context_id], 
                ready_op);
            gen_mutex_lock(&ready_op->mutex);
            ready_op->op.state = OP_COMPLETED;
            ready_op->state = 0;
            gen_mutex_unlock(&ready_op->mutex);
        }

        sync_context->coalesce_counter = 0;
        pthread_cond_signal(&dbpf_op_completed_cond);
        gen_mutex_unlock(
            dbpf_completion_queue_array_mutex[qop_p->op.context_id]);
        ret = 1;
    }
    else
    {
        gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                     "[SYNC_COALESCE]:\tcoalescing %d op: %d\n", 
                     sync_context_type, sync_context->coalesce_counter);
        
        dbpf_op_queue_add(
                sync_context->sync_queue, qop_p);
        sync_context->coalesce_counter++;
        ret = 0;
    }

    gen_mutex_unlock(sync_context->mutex);
    return ret;
}

int dbpf_sync_coalesce_enqueue(dbpf_queued_op_t *qop_p)
{
    dbpf_sync_context_t * sync_context;
    
    int sync_context_type = dbpf_sync_get_object_sync_context(qop_p->op.type);

    gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                 "[SYNC_COALESCE]: enqueue called\n");

	if ( sync_context_type == COALESCE_CONTEXT_NEVER_SYNC )
	{ 
		return 0;
	} 
    
    sync_context = & sync_array[sync_context_type][qop_p->op.context_id];
    
    gen_mutex_lock(sync_context->mutex);
    
    if( (qop_p->op.flags & TROVE_SYNC) ){ 
    	sync_context->sync_counter++;
    }else{
    	sync_context->non_sync_counter++;
    }
    gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                 "[SYNC_COALESCE]: enqueue %d counter sync:%d non_sync:%d\n",
                 sync_context_type,
                 sync_context->sync_counter, sync_context->non_sync_counter);

    gen_mutex_unlock(sync_context->mutex);

    return 0;
}

int dbpf_sync_coalesce_dequeue(
    dbpf_queued_op_t *qop_p)
{
    
    dbpf_sync_context_t * sync_context;
    int sync_context_type = dbpf_sync_get_object_sync_context(qop_p->op.type);

    gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                 "[SYNC_COALESCE]: dequeue called\n");

	if ( sync_context_type == COALESCE_CONTEXT_NEVER_SYNC )
	{ 
		return 0;
	} 
    
    sync_context = & sync_array[sync_context_type][qop_p->op.context_id];

    gen_mutex_lock(sync_context->mutex);
    if( (qop_p->op.flags & TROVE_SYNC) ){ 
    	sync_context->sync_counter--;
    }else{
    	sync_context->non_sync_counter--;
    }
    gossip_debug(GOSSIP_DBPF_COALESCE_DEBUG,
                 "[SYNC_COALESCE]: dequeue %d counter sync:%d non_sync:%d\n",
                 sync_context_type,
                 sync_context->sync_counter, sync_context->non_sync_counter);

    gen_mutex_unlock(sync_context->mutex);

    return 0;
}

void dbpf_queued_op_set_sync_high_watermark(int high, struct dbpf_collection* coll)
{
    coll->c_high_watermark = high;
}

void dbpf_queued_op_set_sync_low_watermark(int low, struct dbpf_collection* coll)
{
    coll->c_low_watermark = low;
}

void dbpf_queued_op_set_sync_mode(int enabled, struct dbpf_collection* coll){
	coll->meta_sync_enabled = enabled;
	/*
	 * Right now we don't have to check if there are operations queued in the
	 * coalesync queue, because the mode is set on startup...
	 */
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */ 
