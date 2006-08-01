/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#ifndef __STATE_MACHINE_FNS_H
#define __STATE_MACHINE_FNS_H

#include <string.h>
#include <assert.h>

#include "gossip.h"
#include "pvfs2-debug.h"
#include "state-machine.h"

/* STATE-MACHINE-FNS.C
 *
 * This file implements a small collection of functions used when
 * interacting with the state machine system implemented in
 * state-machine.h.  Probably you'll only need these functions in one
 * file per instance of a state machine implementation.
 *
 * Note that state-machine.h must be included before this is included.
 * This is usually accomplished through including some *other* file that
 * includes state-machine.h, because state-machine.h needs a key #define
 * before it can be included.
 *
 * The PINT_OP_STATE_TABLE has been replaced with a macro that must be #defined
 * instead: PINT_OP_STATE_GET_MACHINE.  
 * This allows the _locate function to be used in the client as well.
 *
 * A good example of this is the pvfs2-server.h in the src/server directory,
 * which includes state-machine.h at the bottom, and server-state-machine.c,
 * which includes first pvfs2-server.h and then state-machine-fns.h.
 */


/* Function: PINT_state_machine_halt(void)
   Params: None
   Returns: True
   Synopsis: This function is used to shutdown the state machine 
 */
int PINT_state_machine_halt(void)
{
    return 0;
}

/* Function: PINT_state_machine_terminate
   Params: smcb, job status
   Returns: 0 on sucess, otherwise error
   Synopsis: This function cleans up and terminates a SM
        in some cases we may need to keep the SM alive until
        its children terminate or something - in which case we
        will post the relevant job here.
 */
int PINT_state_machine_terminate(struct PINT_smcb *smcb, job_status_s *r)
{
    /* notify parent */
    if (smcb->parent_smcb)
    {
        job_id_t id;
        /* acquire lock here */
        if (--smcb->parent_smcb->children_running == 0)
        {
            /* wake up parent, through job interface */
            job_null(0, smcb, 0, r, &id, smcb->context);
        }
        /* release lock here */
    }
    /* call state machine completion function */
    if (smcb->terminate_fn)
    {
        (*smcb->terminate_fn)(smcb, r);
    }
    return 0;
}

/* Function: PINT_state_machine_invoke
   Params: smcb pointer and job status pointer
   Returns: return value of state action
   Synopsis: runs the current state action, produces debugging
        output if needed, checls return value and takes action
        if needed (sets op_terminate if SM_ACTION_TERMINATED is
        returned)
 */
int PINT_state_machine_invoke(struct PINT_smcb *smcb, job_status_s *r)
{
    int retval;
    const char * state_name;
    const char * machine_name;

    if (!smcb || !smcb->current_state ||
            !smcb->current_state->flag == SM_RUN ||
            !smcb->current_state->action.func)
    {
        gossip_err("SM invoke called in invalid smcb\n");
        return -1;
    }

    /* print pre-call debugging info */
    gossip_debug(GOSSIP_STATE_MACHINE_DEBUG,
            "SM invoke smcb %p op %d\n",smcb,(smcb)->op);

    state_name = PINT_state_machine_current_state_name(smcb);
    machine_name = PINT_state_machine_current_machine_name(smcb);

    gossip_debug(GOSSIP_STATE_MACHINE_DEBUG, 
                 "[SM Entering]: (%p) %s:%s (status: %d)\n",
                 smcb,
                 /* skip pvfs2_ */
                 machine_name,
                 state_name,
                 (int32_t)r->status_user_tag);
     
    /* call state action function */
    retval = (smcb->current_state->action.func)(smcb,r);

    /* print post-call debugging info */
    gossip_debug(GOSSIP_STATE_MACHINE_DEBUG, 
                 "[SM Exiting]: (%p) %s:%s (error code: %d)\n",
                 smcb,
                 /* skip pvfs2_ */
                 machine_name,
                 state_name,
                 r->error_code);

    /* process return code */
    switch (retval)
    {
    case SM_ACTION_TERMINATE :
        {
            smcb->op_terminate = 1;
        }
    case SM_ACTION_COMPLETE :
    case SM_ACTION_DEFERRED :
        {
            return retval;
        }
    default :
        {
            /* error */
            gossip_err("SM returned invalid return code %d\n", retval);
            break;
        }
    }

    return retval;
}

/* Function: PINT_state_machine_next()
   Params: smcb pointer and job status pointer
   Returns: return value of last state action
   Synopsis: runs the state action pointed to by the
        current state, then continues to run the SM
        as long asreturn code is SM_ACTION_COMPLETE
 */

int PINT_state_machine_start(struct PINT_smcb *smcb, job_status_s *r)
{
    int ret;
    /* run the current state action function */
    ret = PINT_state_machine_invoke(smcb, r);
    if (ret == SM_ACTION_COMPLETE || ret == SM_ACTION_TERMINATE)
        /* keep running until state machine deferrs or terminates */
        ret = PINT_state_machine_next(smcb, r);
    return ret;
}

/* Function: PINT_state_machine_next()
   Params: smcb pointer and job status pointer
   Returns: return value of last state action
   Synopsis: Runs through a list of return values to find the next function to
   call.  Calls that function.  If that function returned COMPLETED loop
   and repeat.
 */
int PINT_state_machine_next(struct PINT_smcb *smcb, job_status_s *r)
{
    struct PINT_state_s *loc; /* temp pointer into state memory */
    struct PINT_tran_tbl_s *tloc;
    int ret;                            /* holes state action return code */

    if (!smcb && !smcb->current_state)
    {
        gossip_err("SM next called on invald smcb\n");
        return -1;
    }
    gossip_debug(GOSSIP_STATE_MACHINE_DEBUG,
            "SM next smcb %p op %d\n",smcb,(smcb)->op);
    /* loop while returns COMPLETED */
    do {
        /* loop while returning from nested SM */
        do {
	    /* skip over the current state action to
             * get to the return code list
             */
	    loc = smcb->current_state;
            tloc = loc->trtbl;
    
	    /* for each entry in the state machine table there is a return
	    * code followed by a next state pointer to the new state.
	    * This loops through each entry, checking for a match on the
	    * return address, and then sets the new current_state and calls
	    * the new state action function */
	    while (tloc->return_value != r->error_code &&
	           tloc->return_value != DEFAULT_ERROR) 
	    {
	        /* each entry has a return value followed by a next state
	        * pointer, so we increment by two.
	        */
	        tloc += 1;
	    }

	    /* skip over the return code to get to the pointer for the
	    * next state
	    */
	    /*loc += 1;*/

	    /* we expect the last state action function to return
            * SM_ACTION_TERMINATE which sets the smcb->op_terminate
            * flag.  ALSO the state machine much direct the next state
            * to be terminate, which sets loc->flag to SM_TERMINATE.
	    * We'll terminate for EITHER, but print an error if not
            * both.
	    */
	    if(tloc->flag == SM_TERM || smcb->op_terminate)
	    {
                if (!(tloc->flag == SM_TERM))
                {
	            gossip_lerr("Error: state machine reached terminate"
                            " without returning SM_ACTION_TERMINATE\n");
                }
                {
                if (!smcb->op_terminate)
	            gossip_lerr("Error: state machine returned"
                           " SM_ACTION_TERMINATE but didn't reach terminate\n");
                }
                /* process terminating SM */
                PINT_state_machine_terminate(smcb, r);

                return SM_ACTION_TERMINATE;
	    }

	    /* Update the server_op struct to reflect the new location
	    * see if the selected return value is a STATE_RETURN */
	    if (tloc->flag == SM_RETURN)
	    {
	        smcb->current_state = PINT_pop_state(smcb);
                /* skip state flags */
	        /*smcb->current_state += 1;*/
	    }
        } while (tloc->flag == SM_RETURN);

        smcb->current_state = tloc->next_state;
        /* skip state name and parent SM ref */
        /*smcb->current_state += 2;*/

        /* To do nested states, we check to see if the next state is
        * a nested state machine, and if so we push the return state
        * onto a stack */
        while (smcb->current_state->flag == SM_JUMP)
        {
	    PINT_push_state(smcb, smcb->current_state);

            /* skip state flag; now we point to the SM */
	    /*smcb->current_state += 1;*/

	    smcb->current_state =
                    smcb->current_state->action.nested->state_machine;
            /* skip state name a parent SM ref */
            /*smcb->current_state += 2;*/
        }

        /* skip over the flag so that we point to the function for the next
        * state.  then call it.
        */
        /*smcb->current_state += 1;*/

        /* runs state_action and returns the return code */
        ret = PINT_state_machine_invoke(smcb, r);

    } while (ret == SM_ACTION_COMPLETE || ret == SM_ACTION_TERMINATE);

    return ret;
}

/* Function: PINT_state_machine_locate(void)
   Params:   smcb pointer with op correctly set
   Returns:  Pointer to the start of the state machine indicated by
	          smcb->op
   Synopsis: This function locates the state associated with the op
             specified in smcb->op in order to start a state machine's
             execution.
 */
int PINT_state_machine_locate(struct PINT_smcb *smcb)
{
    struct PINT_state_s *current_tmp;
    struct PINT_state_machine_s *op_sm;

    /* check for valid inputs */
    if (!smcb || smcb->op < 0 || !smcb->op_get_state_machine)
    {
	gossip_err("State machine requested not valid\n");
	return -PVFS_EINVAL;
    }
    gossip_debug(GOSSIP_STATE_MACHINE_DEBUG,
            "SM locate smcb %p op %d\n",smcb,(smcb)->op);
    /* this is a the usage dependant routine to look up the SM */
    op_sm = (*smcb->op_get_state_machine)(smcb->op);
    if (op_sm != NULL)
    {
	current_tmp = op_sm->state_machine;
        /* skip SM name and parent */
        /*current_tmp += 2;*/
	/* handle the case in which the first state points to a nested
	 * machine, rather than a simple function
	 */
	while(current_tmp->flag == SM_JUMP)
	{
	    PINT_push_state(smcb, current_tmp);
            /* skip state flag */
	    /*current_tmp += 1;*/
	    current_tmp = ((struct PINT_state_machine_s *)
                           current_tmp->action.nested)->state_machine;
            /* skip SM name and parent */
            /*current_tmp += 2;*/
	}

	/* this sets a pointer to a "PINT_state_array_values"
	 * structure, whose state_action member is the function to call.
	 */
        /* skip state flag */
        /*smcb->current_state = current_tmp + 1;*/
	return 1; /* indicates successful locate */
    }

    gossip_err("State machine not found for operation %d\n",smcb->op);
    return 0; /* indicates failed to locate */
}

/* Function: PINT_smcb_set_op
   Params: pointer to an smcb pointer, and an op code (int)
   Returns: nothing
   Synopsis: sets op on existing smcb and reruns locate if
            we have a valid locate func
 */
int PINT_smcb_set_op(struct PINT_smcb *smcb, int op)
{
    gossip_debug(GOSSIP_STATE_MACHINE_DEBUG,
            "SM set op smcb %p op %d\n",smcb,op);
    smcb->op = op;
    return PINT_state_machine_locate(smcb);
}

/* Function: PINT_smcb_op
   Params: pointer to an smcb pointer
   Returns: op (int)
   Synopsis: returns the op currently set in the smcb
 */
int PINT_smcb_op(struct PINT_smcb *smcb)
{
    return smcb->op;
}

/* Function: PINT_smcb_set_complete
   Params: pointer to an smcb pointer
   Returns: nothing
   Synopsis: sets op_terminate on existing smcb
 */
void PINT_smcb_set_complete(struct PINT_smcb *smcb)
{
    smcb->op_terminate = 1;
}

/* Function: PINT_smcb_complete
   Params: pointer to an smcb pointer
   Returns: op (int)
   Synopsis: returns the op_terminate currently set in the smcb
 */
int PINT_smcb_complete(struct PINT_smcb *smcb)
{
    return smcb->op_terminate;
}

/* Function: PINT_smcb_set_cancelled
   Params: pointer to an smcb pointer
   Returns: nothing
   Synopsis: sets op_cancelled on existing smcb
 */
void PINT_smcb_set_cancelled(struct PINT_smcb *smcb)
{
    smcb->op_cancelled = 1;
}

/* Function: PINT_smcb_cancelled
   Params: pointer to an smcb pointer
   Returns: op (int)
   Synopsis: returns the op_cancelled currently set in the smcb
 */
int PINT_smcb_cancelled(struct PINT_smcb *smcb)
{
    return smcb->op_cancelled;
}

/* Function: PINT_smcb_alloc
   Params: pointer to an smcb pointer, an op code (int), size of frame
            (int), pinter to function to locate SM
   Returns: nothing, but fills in pointer argument
   Synopsis: this allocates an smcb struct, including its frame stack
             and sets the op code so you can start the state machine
 */
int PINT_smcb_alloc(
        struct PINT_smcb **smcb,
        int op,
        int frame_size,
        struct PINT_state_machine_s *(*getmach)(int),
        int (*term_fn)(struct PINT_smcb *, job_status_s *),
        job_context_id context_id)
{
    *smcb = (struct PINT_smcb *)malloc(sizeof(struct PINT_smcb));
    if (!(*smcb))
    {
        return -PVFS_ENOMEM;
    }
    gossip_debug(GOSSIP_STATE_MACHINE_DEBUG,
            "SM allocate smcb %p op %d\n",*smcb,op);
    /* zero out all members */
    memset(*smcb, 0, sizeof(struct PINT_smcb));
    /* if frame_size given, allocate a frame */
    if (frame_size > 0)
    {
        (*smcb)->frame_stack[0] = malloc(frame_size);
        if (!((*smcb)->frame_stack[0]))
        {
            free(*smcb);
            *smcb = NULL;
            return -PVFS_ENOMEM;
        }
        /* zero out all members */
        memset((*smcb)->frame_stack[0], 0, frame_size);
    }
    (*smcb)->op = op;
    (*smcb)->op_get_state_machine = getmach;
    (*smcb)->terminate_fn = term_fn;
    (*smcb)->context = context_id;
    /* if a getmach given, lookup state machine */
    if (getmach)
        return PINT_state_machine_locate(*smcb);
    return 0; /* success */
}

/* Function: PINT_smcb_free
   Params: pointer to an smcb pointer
   Returns: nothing, but sets the pointer to NULL
   Synopsis: this frees an smcb struct, including its frame stack
             and anything on the frame stack
 */
void PINT_smcb_free(struct PINT_smcb **smcb)
{
    int i;
    if (smcb)
    {
        if (*smcb)
        {
            gossip_debug(GOSSIP_STATE_MACHINE_DEBUG,
                     "SM free smcb %p op %d\n",*smcb,(*smcb)->op);
            for (i = 0; i < PINT_FRAME_STACK_SIZE; i++)
            {
                if ((*smcb)->frame_stack[i])
                {
                    /* DO we really want to do this??? */
                    free((*smcb)->frame_stack[i]);
                }
            }
            free(*smcb);
        }
        (*smcb) = (struct PINT_smcb *)0;
    }
}

/* Function: PINT_pop_state
   Params: pointer to an smcb pointer
   Returns: 
   Synopsis: 
 */
struct PINT_state_s *PINT_pop_state(struct PINT_smcb *smcb)
{
    assert(smcb->stackptr > 0);

    return smcb->state_stack[--smcb->stackptr];
}

/* Function: PINT_push_state
   Params: pointer to an smcb pointer
   Returns: 
   Synopsis: 
 */
void PINT_push_state(struct PINT_smcb *smcb,
				   struct PINT_state_s *p)
{
    assert(smcb->stackptr < PINT_STATE_STACK_SIZE);

    smcb->state_stack[smcb->stackptr++] = p;
}

/* Function: PINT_sm_frame
   Params: pointer to smcb, stack index
   Returns: pointer to frame
   Synopsis: returns a frame off of the frame stack
 */
void *PINT_sm_frame(struct PINT_smcb *smcb, int index)
{
    return smcb->frame_stack[smcb->framebaseptr + index];
}

/* Function: PINT_sm_push_frame
   Params: pointer to smcb, void pointer for new frame
   Returns: 
   Synopsis: pushes a new frame pointer onto the frame_stack
 */
void PINT_sm_push_frame(struct PINT_smcb *smcb, void *frame_p)
 {
    assert(smcb->framestackptr < PINT_FRAME_STACK_SIZE);

    smcb->frame_stack[smcb->framestackptr++] = frame_p;
 }

/* Function: PINT_sm_set
   Params: pointer to smcb
   Returns: 
   Synopsis: This moves the framebaseptr up to the stop of the frame_stack
 */
void PINT_sm_set_frame(struct PINT_smcb *smcb)
{
    assert(smcb->framestackptr >= 0);

    if (smcb->framestackptr == 0)
        smcb->framebaseptr = 0;
    else
        smcb->framebaseptr = smcb->framestackptr-1;
}

/* Function: PINT_sm_pop_frame
   Params: pointer to an smcb pointer
   Returns: frame pointer
   Synopsis: pops a frame pointer from the frame_stack and returns it
 */
void *PINT_sm_pop_frame(struct PINT_smcb *smcb)
{
    int index;
    void *frame;
    assert(smcb->framestackptr > smcb->framebaseptr);

    index = --smcb->framestackptr;
    frame = smcb->frame_stack[index];
    smcb->frame_stack[index + 1] = NULL; /* so we won't free on smcb_free */
    return frame;
}

/* Function: DUMMY FUNCTION
   Params: 
   Returns: 
   Synopsis: 
 */
struct PINT_state_s  *PINT_sm_something(void)
{
    return NULL;
}

/* Function: PINT_sm_start_child_frames
   Params: pointer to an smcb pointer
   Returns: 
   Synopsis: This starts all the enw child SMs based on the frame_stack
 */
void PINT_sm_start_child_frames(struct PINT_smcb *smcb)
{
    int i;
    struct PINT_smcb *new_sm;
    job_status_s *r;

    assert(smcb);

    for(i = smcb->framebaseptr; i < smcb->framebaseptr; i++)
    {
        /* allocate smcb */
        PINT_smcb_alloc(&new_sm, smcb->op, 0, NULL,
                smcb->terminate_fn, smcb->context);
        /* set parent smcb pointer */
        new_sm->parent_smcb = smcb;
        /* increment parent's counter */
        smcb->children_running++;
        /* assign frame */
        PINT_sm_push_frame(new_sm, smcb->frame_stack[i]);
        /* locate SM to run */
        new_sm->current_state = PINT_sm_something();
        /* invoke SM */
        PINT_state_machine_start(new_sm, r);
    }
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */

#endif
