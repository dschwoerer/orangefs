/* 
 * (C) 2001 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "state-machine.h"
#include "server-config.h"
#include "pvfs2-server.h"

static int getconfig_cleanup(state_action_struct *s_op, job_status_s *ret);
static int getconfig_job_bmi_send(state_action_struct *s_op, job_status_s *ret);
static int getconfig_build_bmi_good_msg(state_action_struct *s_op, job_status_s *ret);
static int getconfig_build_bmi_error(state_action_struct *s_op, job_status_s *ret);
static int getconfig_job_trove(state_action_struct *s_op, job_status_s *ret);
static int getconfig_init(state_action_struct *s_op, job_status_s *ret);
void getconfig_init_state_machine(void);

PINT_state_machine_s getconfig_req_s = 
{
	NULL,
	"getconfig",
	getconfig_init_state_machine
};

%%

machine get_config(init, error_msg, good_msg, cleanup, bmi_send, trove)
{
	state init
	{
		run getconfig_init;
		success => trove;
		default => error_msg;
	}

	state error_msg
	{
		run getconfig_build_bmi_error;
		default => bmi_send;
	}

	state good_msg
	{
		run getconfig_build_bmi_good_msg;
		default => bmi_send;
	}

	state cleanup
	{
		run getconfig_cleanup;
		default => init;
	}

	state bmi_send
	{
		run getconfig_job_bmi_send;
		default => cleanup;
	}

	state trove
	{
		run getconfig_job_trove;
		success => good_msg;
		default => error_msg;
	}
}

%%

/*
 * Function: getconfig_init_state_machine
 *
 * Params:   void
 *
 * Returns:  PINT_state_array_values*
 *
 * Synopsis: Set up the state machine for get config and
 *           return a pointer to it.
 *           
 */


void getconfig_init_state_machine(void)
{
	
	getconfig_req_s.state_machine = get_config;
	
}

/*
 * Function: getconfig_init
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Returns:  void
 *
 * Synopsis: Get information from config structure
 *           then move to next state.
 *           
 */

static int getconfig_init(state_action_struct *s_op, job_status_s *ret)
	{

	//PINT_server_op *s_op = (PINT_server_op *) b;
	server_configuration_s *user_opts;
	int err = 1;

	gossip_ldebug(SERVER_DEBUG,"Starting GetConfig Request\n");
	user_opts = get_server_config_struct();
	
	/* Set up the values we have in our Config Struct user_opts */
	s_op->resp->u.getconfig.meta_server_count = user_opts->count_meta_servers;
	s_op->resp->u.getconfig.io_server_count = user_opts->count_io_servers;


#if 0  /* Depricated */
	s_op->resp->u.getconfig.meta_server_mapping = (PVFS_string) BMI_memalloc(s_op->addr,strlen(user_opts->meta_server_list)+1,BMI_SEND_BUFFER);
	memcpy(s_op->resp->u.getconfig.meta_server_mapping,user_opts->meta_server_list,strlen(user_opts->meta_server_list)+1);
	s_op->strsize = strlen(user_opts->meta_server_list)+1;

	s_op->resp->u.getconfig.io_server_mapping = (PVFS_string) BMI_memalloc(s_op->addr,strlen(user_opts->io_server_list)+1,BMI_SEND_BUFFER);
	memcpy(s_op->resp->u.getconfig.io_server_mapping,user_opts->io_server_list,strlen(user_opts->io_server_list)+1);
	s_op->strsize += strlen(user_opts->io_server_list)+1;
#endif 
	
	/* The new way of doing things! dw*/
	s_op->resp->u.getconfig.meta_server_mapping = user_opts->meta_server_list;
	s_op->resp->u.getconfig.io_server_mapping = user_opts->io_server_list;
	s_op->strsize = strlen(user_opts->meta_server_list)+1;
	s_op->strsize += strlen(user_opts->io_server_list)+1;

	/* TODO: what to do with unexpected info struct ? */

	return(err);

	}

/*
 * Function: getconfig_job_trove
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Returns:  void
 *
 * Synopsis: Submit a job to the trove interface 
 *           requesting root handle and fs_id
 *           
 */

static int getconfig_job_trove(state_action_struct *s_op, job_status_s *ret)
	{
	//PINT_server_op *s_op = (PINT_server_op *) b;
	int job_post_ret;
	job_id_t i;

	job_post_ret = job_trove_fs_lookup(s_op->req->u.getconfig.fs_name,s_op,ret,&i);

	return(job_post_ret);

	}

/*
 * Function: getconfig_build_bmi_error
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Returns:  void
 *
 * Synopsis: Build a bmi message containing
 *           an error while processing the request
 *           
 */

static int getconfig_build_bmi_error(state_action_struct *s_op, job_status_s *ret)
	{

	s_op->resp->status = ret->error_code;
	s_op->resp->rsize = sizeof(struct PVFS_server_resp_s);

	/* Clean up Strings if necessary!!!! */
	/* Should not need to do this! dw*/
	//if(s_op->resp->u.getconfig.meta_list)
		/* BMI FREE */

	return(1);

	}

/*
 * Function: getconfig_build_bmi_good_msg
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Returns:  void
 *
 * Synopsis: Build a bmi message containing
 *           the data the client requested
 *           
 */

static int getconfig_build_bmi_good_msg(state_action_struct *s_op, job_status_s *ret)
	{
	//PINT_server_op *s_op = (PINT_server_op *) b;
	int jpret;

	gossip_ldebug(SERVER_DEBUG,"root handle:%Ld\n%d\n",ret->handle,ret->error_code);
	s_op->resp->status = ret->error_code;
	s_op->resp->u.getconfig.root_handle = ret->handle;
	s_op->resp->u.getconfig.fs_id = ret->coll_id;
	s_op->resp->rsize = sizeof(struct PVFS_server_resp_s) + s_op->strsize;
	jpret = PINT_encode(s_op->resp,PINT_ENCODE_RESP,&(s_op->encoded),s_op->addr,s_op->enc_type);
	return(1);

	}

/*
 * Function: getconfig_job_bmi_send
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Returns:  void
 *
 * Synopsis: Send a message to the client with 
 *           either an error or the requested data
 *           
 */

static int getconfig_job_bmi_send(state_action_struct *s_op, job_status_s *ret)
	{
	int job_post_ret;
	job_id_t i;

	assert(s_op->encoded.list_count == 1);
	if(s_op->encoded.list_count == 1)
	{
	job_post_ret = job_bmi_send(s_op->addr,
										 s_op->encoded.buffer_list[0],
										 s_op->encoded.total_size,
										 s_op->tag,
										 0,
										 0,
										 s_op,
										 ret,
										 &i);
	}
	else {
		// Send list!
		job_post_ret = -1;
	}

	return(job_post_ret);

	}

/*
 * Function: getconfig_cleanup
 *
 * Params:   server_op *b, 
 *           job_status_s *ret
 *
 * Returns:  void
 *
 * Synopsis: cleans up string memory
 *           response structure
 *           TODO: should it clean up server_op?
 */

static int getconfig_cleanup(state_action_struct *s_op, job_status_s *ret)
	{
	//PINT_server_op *s_op = (PINT_server_op *) b;
	gossip_ldebug(SERVER_DEBUG,"Completed GetConfig\n");

#if 0
	/* TODO: What do we free? */
	if (s_op->resp->u.getconfig.meta_server_mapping)
	{
		BMI_memfree(s_op->addr,s_op->resp->u.getconfig.meta_server_mapping,strlen(s_op->resp->u.getconfig.meta_server_mapping),BMI_SEND_BUFFER);
	}
#endif

	/* TODO: Free I/O Struct! */
	if (s_op->resp)
	{
		BMI_memfree(s_op->addr,s_op->resp,sizeof(struct PVFS_server_resp_s),BMI_SEND_BUFFER);
	}

	free(s_op->unexp_bmi_buff);

	free(s_op);

	return(0);

	}


