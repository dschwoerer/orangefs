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

PINT_server_trove_keys_s Trove_Common_Keys[] = {
    {"root_handle", 12},
    {"metadata", 9},
    {"dir_ent", 8},
    {"datafile_handles", 17},
    {"metafile_dist", 14}
};

#define ENCODE_TYPE 0
#define SM_STATE_RETURN -1
#define SM_NESTED_STATE 1

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
extern PINT_state_machine_s io_req_s;
extern PINT_state_machine_s remove_req_s;
extern PINT_state_machine_s rmdirent_req_s;

/* DALE - fill in the rest of these please - WBL */
PINT_state_machine_s *PINT_server_op_table[SERVER_OP_TABLE_SIZE] =
{
	NULL,              /* invalid          */
	NULL,              /* noop             */
	&create_req_s,     /* create           */
	&remove_req_s,     /* remove           */
	&io_req_s,         /* io               */
	NULL,					 /* empty - 5        */
	NULL,              /* batch            */ 
	&getattr_req_s,    /* get attrib       */
	&setattr_req_s,    /* set attrib       */
	NULL,              /* geteattr         */
	NULL,					 /* seteattr - 10    */
	&lookup_req_s,     /* lookup           */
	NULL,              /* getdist ????     */
	&crdirent_req_s,   /* createdir ent    */
	&rmdirent_req_s,   /* rmdirent         */
	NULL,					 /* revlookup? - 15  */
	NULL,              /* allocate         */
	NULL,              /* truncate         */
	&mkdir_req_s,      /* mkdir            */
	NULL,              /* rmdir            */
	&readdir_req_s, 	 /* readdir - 20     */
	NULL,              /* statfs           */
	NULL,              /* iostatfs         */
	&getconfig_req_s,  /* get config       */
	/*                                     */
	/* NULL's continue for a while...      */
	/*                                     */
	NULL               /* extension !99!   */
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

	PINT_decode(s_op->unexp_bmi_buff.buffer,
					PINT_ENCODE_REQ,
					&s_op->decoded,
					s_op->unexp_bmi_buff.addr,
					s_op->unexp_bmi_buff.size,
					&(s_op->enc_type));
	s_op->req  = (struct PVFS_server_req_s *) s_op->decoded.buffer;
	assert(s_op->req != NULL);

	s_op->addr = s_op->unexp_bmi_buff.addr;
	s_op->tag  = s_op->unexp_bmi_buff.tag;
	s_op->op   = s_op->req->op;
	s_op->current_state = PINT_state_machine_locate(s_op);

	if(!s_op->current_state)
	{
		gossip_err("System not init for function\n");
		return(-1);
	}
	/* TODO:  This would be a good place for caching!!! */
	s_op->resp = (struct PVFS_server_resp_s *)
			malloc(sizeof(struct PVFS_server_resp_s));

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
   Returns:   return value of state action
   Synopsis:  Runs through a list of return values to find the
	           next function to call.  Calls that function.  Once
				  that function is called, this one exits and we go
				  back to pvfs2-server.c's while loop.
 */

int PINT_state_machine_next(state_action_struct *s,job_status_s *r)
{

   int code_val = r->error_code; /* temp to hold the return code */
	int retval; /* temp to hold return value of state action */
	PINT_state_array_values *loc; /* temp pointer into state memory */

	/* skip over the current state action to get to the return code list */
	loc = s->current_state + 1;

	/* for each entry in the state machine table there is a return
	 * code followed by a next state pointer to the new state.
	 * This loops through each entry, checking for a match on the
	 * return address, and then sets the new current_state and calls
	 * the new state action function */
	while (loc->return_value != code_val && loc->return_value != DEFAULT_ERROR) 
	{
		/* each entry is two items long */
		loc += 2;
	}

	/* skip over the return code to get to the next state */
	loc += 1;

	/* Update the server_op struct to reflect the new location
	 * see if the selected return value is a STATE_RETURN */
	if (loc->flag == SM_STATE_RETURN)
	{
		s->current_state = PINT_pop_state(s);
	}
	else
	{
		s->current_state = loc->next_state;
	}

	/* To do nested states, we check to see if the next state is
	 * a nested state machine, and if so we push the return state
	 * onto a stack */
	if (s->current_state->flag == SM_NESTED_STATE)
	{
		PINT_push_state(s, NULL);
	}

	/* skip over the flag so we can execute the next state action */
	s->current_state += 1;

	/* Call the new state function then */
	retval = (s->current_state->state_action)(s,r);

	/* return to the while loop in pvfs2-server.c */
	return retval;

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

		/* WBL - adjusted for nested state machines
		 * return PINT_server_op_table[s_op->op]->state_machine */
		return PINT_server_op_table[s_op->op]->state_machine + 1;
	}
	gossip_err("State machine not found for operation %d\n",s_op->op);
	return(NULL);
}

PINT_state_array_values *PINT_pop_state(state_action_struct *s)
{
	return NULL;
}

void PINT_push_state(state_action_struct *s, PINT_state_array_values *p)
{
}

