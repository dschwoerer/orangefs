/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/*
 * June 2002
 * 
 * State machine going through changes...
 * Initialization functions to go through server_queue
 * with a check_dep() call
 * this will be for all operations  dw
 *
 * Jan 2002
 *
 * This is a basic state machine.
 * This is meant to be a basic framework.
 */ 

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/vfs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <linux/dirent.h>
#include <signal.h>

#include <bmi.h>
#include <gossip.h>
#include <job.h>
#include <pvfs2-debug.h>
#include <pvfs2-storage.h>
#include <assert.h>
#include <PINT-reqproto-encode.h>

#include <state-machine.h>
#include <pvfs2-server.h>

/* This array is used for common key-val pairs for trove =) */

PINT_server_trove_keys_s Trove_Common_Keys[3] = {{"root_handle",12},{"metadata",9},{"dir_ent",8}};

#define ENCODE_TYPE 0

/* Here is the idea...
 * For each state machine, you start with an initial function.
 * Upon posting/completion, you then check return values.
 * A complete state machine with return values should be located
 * in a contiguous region.  
 * Exception: when states are shared between machines. (Damn)
 * Therefore, I guess it really does not matter... but it would be
 * nice if they were as contiguous as possible just to save on the
 * debugging time. dw
*/

extern PINT_state_machine_s getconfig_req_s;
extern PINT_state_machine_s getattr_req_s;
extern PINT_state_machine_s setattr_req_s;
extern PINT_state_machine_s create_req_s;
extern PINT_state_machine_s crdirent_req_s;
extern PINT_state_machine_s mkdir_req_s;
extern PINT_state_machine_s readdir_req_s;
extern PINT_state_machine_s lookup_req_s;

/* DALE - fill in the rest of these please - WBL */
PINT_state_machine_s *PINT_server_op_table[SERVER_OP_TABLE_SIZE] =
{
	NULL,               /* 0 */
	NULL,
	&create_req_s,     /* create */
	NULL,
	NULL,
	NULL,					  /* 5 */
	NULL,
	&getattr_req_s,    /* get attrib */
	&setattr_req_s,    /* set attrib */
	NULL,
	NULL,					  /* 10 */
	&lookup_req_s,     /* lookup */
	NULL,
	&crdirent_req_s,   /* create dir entry */
	NULL,
	NULL,					  /* 15 */
	NULL,
	NULL,
	&mkdir_req_s,      /* mkdir */
	NULL,
	&readdir_req_s, 	 /* readdir - 20 */
	NULL,
	NULL,
	&getconfig_req_s,  /* get config */
	NULL
};



/* 
 * Function: PINT_state_machine_initialize_unexpected(s_op,ret)
 *
 * Params:   PINT_server_op *s_op
 *           job_status_s *ret
 *    
 * Returns:  int
 * 
 * Synopsis: Intialize request structure, first location, and call
 *           respective init function.
 * 			 
 */

int PINT_state_machine_initialize_unexpected(state_action_struct *s_op, job_status_s *ret)
{
	struct PINT_decoded_msg decoded_message;

	PINT_decode(s_op->unexp_bmi_buff->buffer,
					PINT_ENCODE_REQ,
					&decoded_message,
					s_op->unexp_bmi_buff->addr,
					s_op->unexp_bmi_buff->size,
					&(s_op->enc_type));
	s_op->req  = (struct PVFS_server_req_s *) decoded_message.buffer;
	assert(s_op->req != NULL);

	s_op->addr = s_op->unexp_bmi_buff->addr;
	s_op->tag  = s_op->unexp_bmi_buff->tag;
	s_op->op   = s_op->req->op;
	s_op->current_state = PINT_state_machine_locate(s_op);

	/* Walt said this code does not need to be here anymore */
#if 0

	/* What is happening here!!! 
		Right now, we are pointing to the pointer of the first state.... 
		We need to follow that pointer down one. By dereferencing this pointer, we are where we 
	   should be! TODO: WHY IS THIS NOW NECESSARY????  dw
		
		-- More explanation:  Ok, so in gdb, without this line of code below, there is a pointer
		   that is to ST_init which itself is that generated array.  So it looks like what has 
			happened is there is another layer of pointers... but I will find out once I fix this 
			request scheduler problem
	*/
		

	/********************************************************************************/
	/******************************   WARNING  **************************************/
	/******************* Ugly code to handle an issue below *************************/
	/********************************************************************************/

	s_op->current_state = (PINT_state_array_values *) *((PINT_state_array_values **)s_op->current_state);

	/********************************************************************************/
	/****************************** /WARNING ****************************************/
	/********************************************************************************/
#endif


	if(!s_op->current_state)
	{
		gossip_err("System not init for function\n");
		return(-1);
	}
	/* TODO:  This would be a good place for caching!!! */
	s_op->resp = (struct PVFS_server_resp_s *) malloc(sizeof(struct PVFS_server_resp_s));

	if (!s_op->resp)
	{
		gossip_err("Out of Memory");
		ret->error_code = 1;
		return(-ENOMEM);
	}
	memset(s_op->resp, 0, sizeof(struct PVFS_server_resp_s));

	s_op->resp->op = s_op->req->op;

	return(((s_op->current_state->state_action))(s_op,ret));

}


/* Function: PINT_state_machine_init(void)
   Params: None
   Returns: True
   Synopsis: This function is used to initialize the state machine 
				 for operation.
 */

int PINT_state_machine_init(void)
{

	int i;
	for (i = 0 ; i < SERVER_OP_TABLE_SIZE; i++)
	{
		if(PINT_server_op_table[i])
		{
			(PINT_server_op_table[i]->init_fun)();
		}
	}
	return(0);
	
}

/* Function: PINT_state_machine_halt(void)
   Params: None
   Returns: True
   Synopsis: This function is used to shutdown the state machine 
 */

int PINT_state_machine_halt(void)
{
	return(-1);
}

/* Function: PINT_state_machine_next()
   Params: 
   Returns:   void
   Synopsis:  Runs through a list of return values to find the
	           next function to call.  Calls that function.  Once
				  that function is called, this one exits and we go
				  back to server_daemon.c's while loop.
 */

int PINT_state_machine_next(state_action_struct *s,job_status_s *r)
{

   int code_val = r->error_code; 
	/* move current state to the first return code */
	PINT_state_array_values *loc;

	/* for each entry in the state machine table there is a return
	 * code followed by a next state pointer to the new state.
	 * This loops through each entry, checking for a match on the
	 * return address, and then sets the new current_state and calls
	 * the new state action function */

	loc = s->current_state + 1;
	while (loc->return_value != code_val && loc->return_value != DEFAULT_ERROR) 
	{
		/* each entry is two items long */
		loc += 2;
	}

	/* Update the server_op struct to reflect the new location */
	/* NOTE: This remains a pointer pointing to the first return
	 *	      value possibility of the function.  */

	s->current_state = (loc + 1)->next_state;

	/* Call the next function.  NOTE: the function will return
	 * back to the original while loop in server_daemon.c */

	return((s->current_state->state_action)(s,r));

}

/* Function: PINT_state_machine_locate(void)
   Params:  
   Returns:  Pointer to the first return value of the initialization
	          function of a state machine
   Synopsis: This function is used to start a state machines execution.
	          If the operation index is null, we have not trapped that yet
	          TODO: Fix that
				 We should also add in some "text" backup if necessary
 */

PINT_state_array_values *PINT_state_machine_locate(state_action_struct *s_op)
{

	/* do we need to check for s_op->op out of range?  - WBL */
	if(PINT_server_op_table[s_op->op] != NULL)
	{
		/* Return the first return value possible from the init function... =) */
		return PINT_server_op_table[s_op->op]->state_machine;
	}
	gossip_err("State machine not found for operation %d\n",s_op->op);
	return(NULL);
	
}
