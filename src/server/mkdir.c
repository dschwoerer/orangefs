/* 
 * (C) 2001 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */


#include <state-machine.h>
#include <server-config.h>
#include <pvfs2-server.h>
#include <string.h>
#include <pvfs2-attr.h>
#include <assert.h>

static int mkdir_init(state_action_struct *s_op, job_status_s *ret);
static int mkdir_create(state_action_struct *s_op, job_status_s *ret);
static int mkdir_setattrib(state_action_struct *s_op, job_status_s *ret);
static int mkdir_release(state_action_struct *s_op, job_status_s *ret);
static int mkdir_send_bmi(state_action_struct *s_op, job_status_s *ret);
static int mkdir_cleanup(state_action_struct *s_op, job_status_s *ret);
static int mkdir_fill_handle(state_action_struct *s_op, job_status_s *ret);
static int mkdir_error(state_action_struct *s_op, job_status_s *ret);
static int mkdir_critical_error(state_action_struct *s_op, job_status_s *ret);
void mkdir_init_state_machine(void);

extern PINT_server_trove_keys_s Trove_Common_Keys[];

PINT_state_machine_s mkdir_req_s = 
{
	NULL,
	"mkdir",
	mkdir_init_state_machine
};

%%

machine mkdir(init, create, set_attrib, fill_handle, release, send, err_msg, critical_error, cleanup)
{
	state init
	{
		run mkdir_init;
		success => create;
		default => err_msg;
	}

	state create
	{
		run mkdir_create;
		success => set_attrib;
		default => err_msg;
	}

	state set_attrib
	{
		run mkdir_setattrib;
		success => fill_handle;
		default => err_msg;
	}
	
	state fill_handle
	{
	    run mkdir_fill_handle;
	    default => release;
	}

	state release
	{
		run mkdir_release;
		success => send;
		default => critical_error;
	}

	state send
	{
		run mkdir_send_bmi;
		default => cleanup;
	}

	state err_msg 
	{
	    run mkdir_error;
	    default => release;
	}

	state critical_error 
	{
	    run mkdir_critical_error;
	    default => cleanup;
	}

	state cleanup
	{
		run mkdir_cleanup;
		default => init;
	}
}

%%

/*
 * Function: mkdir_init_state_machine
 *
 * Params:   void
 *
 * Returns:  PINT_state_array_values*
 *
 * Synopsis: Set up the state machine for mkdir. 
 *           
 */

void mkdir_init_state_machine(void)
{

    mkdir_req_s.state_machine = mkdir;

}

/*
 * Function: mkdir_init
 *
 * Params:   server_op *s_op, 
 *           job_status_s *ret
 *
 * Returns:  int
 *
 * Synopsis: 
 *           
 */


static int mkdir_init(state_action_struct *s_op, job_status_s *ret)
{

    int job_post_ret;

    job_post_ret = job_req_sched_post(s_op->req,
	    s_op,
	    ret,
	    &(s_op->scheduled_id));

    return(job_post_ret);

}

/*
 * Function: mkdir_create
 *
 * Params:   server_op *s_op, 
 *           job_status_s *ret
 *
 * Returns:  int
 *
 * Synopsis: 
 *           
 */


static int mkdir_create(state_action_struct *s_op, job_status_s *ret)
{

    int job_post_ret;
    job_id_t i;

    job_post_ret = job_trove_dspace_create(s_op->req->u.mkdir.fs_id,
	    s_op->req->u.mkdir.bucket,
	    s_op->req->u.mkdir.handle_mask,
	    ATTR_DIR,
	    NULL,
	    s_op,
	    ret,
	    &i);

    return(job_post_ret);

}

/*
 * Function: mkdir_setattrib
 *
 * Params:   server_op *s_op, 
 *           job_status_s *ret
 *
 * Returns:  int
 *
 * Synopsis: 
 *           
 */


static int mkdir_setattrib(state_action_struct *s_op, job_status_s *ret)
{

    int job_post_ret;
    job_id_t i;
    PVFS_vtag_s j;

    s_op->resp->u.mkdir.handle = ret->handle;

    s_op->key.buffer = Trove_Common_Keys[METADATA_KEY].key;
    s_op->key.buffer_sz = Trove_Common_Keys[METADATA_KEY].size;

    s_op->val.buffer = &(s_op->req->u.mkdir.attr);
    s_op->val.buffer_sz = sizeof(PVFS_object_attr);

    job_post_ret = job_trove_keyval_write(s_op->req->u.mkdir.fs_id,
	    s_op->resp->u.mkdir.handle,
	    &(s_op->key),
	    &(s_op->val),
	    0,
	    j,
	    s_op,
	    ret,
	    &i);

    return(job_post_ret);

}


/*
 * Function: mkdir_release
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Pre:      We are done!
 *
 * Post:     We need to let the next operation go.
 *
 * Returns:  int
 *
 * Synopsis: Free the job from the scheduler to allow next job to proceed.
 */

static int mkdir_release(state_action_struct *s_op, job_status_s *ret)
{

    int job_post_ret=0;
    job_id_t i;

    job_post_ret = job_req_sched_release(s_op->scheduled_id,
	    s_op,
	    ret,
	    &i);
    return job_post_ret;
}


/*
 * Function: mkdir_bmi_send
 *
 * Params:   server_op *s_op, 
 *           job_status_s *ret
 *
 * Returns:  int
 *
 * Synopsis: 
 *           
 */


static int mkdir_send_bmi(state_action_struct *s_op, job_status_s *ret)
{

    int job_post_ret;
    job_id_t i;

    s_op->resp->status = ret->error_code;

    job_post_ret = job_bmi_send(s_op->addr,
	    s_op->resp,
	    sizeof(struct PVFS_server_resp_s),
	    s_op->tag,
	    0,
	    0,
	    s_op, 
	    ret, 
	    &i);

    return(job_post_ret);

}



/*
 * Function: mkdir_cleanup
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Returns:  int
 *
 * Synopsis: free memory and return
 *           
 */


static int mkdir_cleanup(state_action_struct *s_op, job_status_s *ret)
{
    /* TODO: Free Decoded and Encoded Messages */

    if(s_op->resp)
    {
	free(s_op->resp);
    }

    if(s_op->req)
    {
	free(s_op->req);
    }

    free(s_op->unexp_bmi_buff);

    free(s_op);

    return(0);

}

/* TODO: fix comment block */
static int mkdir_fill_handle(state_action_struct *s_op, job_status_s *ret)
{
    /* if the error_code *isn't* zero, we should be in a different state */
    assert(ret->error_code == 0);
    s_op->resp->u.mkdir.handle = ret->handle;

    return(1);
}
    
/* TODO: fix comment block */
static int mkdir_error(state_action_struct *s_op, job_status_s *ret)
{
    ret->error_code = -1;
    return(1);
}
/* TODO: fix comment block */
static int mkdir_critical_error(state_action_struct *s_op, job_status_s *ret)
{
    /* make the server stop processing requests */
    return(-1);
}
/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */

