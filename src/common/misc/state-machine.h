/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#ifndef __STATE_MACHINE_H
#define __STATE_MACHINE_H

#include <state-machine-values.h>
#include "job.h"
#include "msgpairarray.h"

/* STATE-MACHINE.H
 *
 * This file sets up all the definitions necessary for our state machine
 * implementation.  This is set up in this (somewhat obscure) way because
 * we want:
 * - a very fast state machine implementation (few dereferences)
 * - easy access to the state data for the implementation
 * - reuse across client and server (different state data)
 *
 * The important thing to note about this file is that it requires that
 * PINT_OP_STATE be defined.  This must be a typedef to the structure
 * that holds the necessary state information for a given state machine.
 * There are four fields that must exist and are used by the state machine
 * implementation:
 * - int op;
 * - int stackptr;
 * - PINT_state_array_values *current_state;
 * - PINT_state_array_values *state_stack[PINT_STATE_STACK_SIZE];
 *
 * Also, PINT_STATE_STACK_SIZE must be defined or enum'd before that
 * declaration.
 *
 * The file state-machine-fns.h defines a set of functions for use in
 * interacting with the state machine.  There are also a couple of other
 * functions that must be defined, in particular some sort of initialization
 * function.  See src/server/server-state-machine.c for examples.
 */

/* State machine control block - one per running instance of a state
 * machine
 */
typedef struct PINT_smcb
{
    /* state machine execution variables */
    int stackptr;
    int framebaseptr;
    int framestackptr;
    struct PINT_state_s *current_state;
    struct PINT_state_s *state_stack[PINT_STATE_STACK_SIZE];
    void *frame_stack[PINT_FRAME_STACK_SIZE];
    /* usage specific routinet to look up SM from OP */
    struct PINT_state_machine_s *(*op_get_state_machine)(int);
    /* state machine context and control variables */
    int op; /* this field externally indicates type of state machine */
    PVFS_id_gen_t op_id; /* unique ID for this operation */
    struct PINT_smcb *parent_smcb; /* points to parent smcb or NULL */
    int op_terminate; /* indicates SM is ready to terminate */
    int op_cancelled; /* indicates SM operation was cancelled */
    int children_running; /* the number of child SMs running */
    job_context_id context; /* job context when waiting for children */
    int (*terminate_fn)(struct PINT_smcb *, job_status_s *);
    void *user_ptr; /* external user pointer */
} PINT_smcb;

#define PINT_SET_OP_COMPLETE do{PINT_smcb_set_complete(smcb);} while (0)

struct PINT_state_machine_s
{
    const char *name;
    struct PINT_state_s *state_machine;
};

struct PINT_state_s
{
    const char *state_name;
    struct PINT_state_machine_s *parent_machine;
    enum PINT_state_code flag;
    union
    {
        int (*func)(struct PINT_smcb *, job_status_s *);
        struct PINT_state_machine_s *nested;
    } action;
    struct PINT_pjmp_tbl_s *pjtbl;
    struct PINT_tran_tbl_s *trtbl;
};

struct PINT_pjmp_tbl_s
{
    int return_value;
    enum PINT_state_code flag;
    struct PINT_state_machine_s *state_machine;
};

struct PINT_tran_tbl_s
{
    int return_value;
    enum PINT_state_code flag;
    struct PINT_state_s *next_state;
};

#if 0
/* this union defines the possibles values for each word of the
   the state machine memory.  Properly formed state machine programs
   consist of these values in a particular order as documented
   externally
 */
union PINT_state_array_values
{
    const char *state_name;
    struct PINT_state_machine_s *parent_machine;
    int flag;
    int (*state_action)(struct PINT_smcb *, job_status_s *);
    struct PINT_state_machine_s *nested_machine;
    int return_value;
    union PINT_state_array_values *next_state;
};

struct PINT_state_machine_s
{
    const char *name;
    union PINT_state_array_values *state_machine;
};

struct PINT_state_s
{
    const char *state_name;
    struct PINT_state_machine_s *parent_machine;
};
#endif

/* All state action functions return this type which controls state
 * machine action
 */
typedef enum {
    SM_ACTION_TERMINATE = 2,
    SM_ACTION_COMPLETE = 1,
    SM_ACTION_DEFERRED = 0,
    SM_ERROR = -1             /* this is a catastrophic error */
} PINT_sm_action;

#define SM_ACTION_ISERR(ret) ((ret)<0)

/* what is this type? */
enum {
    JMP_NOT_READY = 99,
    DEFAULT_ERROR = -1,
};

#define ENCODE_TYPE 0
#define SM_STATE_RETURN -1
#define SM_NESTED_STATE 1

/* Prototypes for functions provided by user */
/* int PINT_state_machine_start(void *, job_status_s *ret); */
int PINT_state_machine_complete(void *);

/* This macro returns the state machine string of the current machine.
 * We assume the first 6 characters of every state machine name are "pvfs2_".
 */
#define PINT_state_machine_current_machine_name(smcb) \
    (((smcb)->current_state->parent_machine->name) + 6)

/* This macro returns the current state invoked */
#define PINT_state_machine_current_state_name(smcb) \
    ((smcb)->current_state->state_name)

#if 0
#define PINT_state_machine_current_machine_name(smcb) \
    (((smcb)->current_state - 2)->parent_machine->name + 6)

#endif

/* Prototypes for functions defined in by state machine code */
int PINT_state_machine_halt(void);
int PINT_state_machine_next(struct PINT_smcb *,job_status_s *);
int PINT_state_machine_invoke(struct PINT_smcb *, job_status_s *);
int PINT_state_machine_start(struct PINT_smcb *, job_status_s *);
int PINT_state_machine_locate(struct PINT_smcb *) __attribute__((used));
int PINT_smcb_set_op(struct PINT_smcb *smcb, int op);
int PINT_smcb_op(struct PINT_smcb *smcb);
void PINT_smcb_set_complete(struct PINT_smcb *smcb);
int PINT_smcb_complete(struct PINT_smcb *smcb);
void PINT_smcb_set_cancelled(struct PINT_smcb *smcb);
int PINT_smcb_cancelled(struct PINT_smcb *smcb);
int PINT_smcb_alloc(struct PINT_smcb **, int, int,
        struct PINT_state_machine_s *(*getmach)(int),
        int (*term_fn)(struct PINT_smcb *, job_status_s *),
        job_context_id context_id);
void PINT_smcb_free(struct PINT_smcb **);
struct PINT_state_s *PINT_pop_state(struct PINT_smcb *);
void PINT_push_state(struct PINT_smcb *, struct PINT_state_s *);
void *PINT_sm_frame(struct PINT_smcb *, int);
void PINT_sm_push_frame(struct PINT_smcb *smcb, void *frame_p);
void PINT_sm_set_frame(struct PINT_smcb *smcb);
void *PINT_sm_pop_frame(struct PINT_smcb *smcb);
void PINT_sm_start_child_frames(struct PINT_smcb *smcb);

/* This macro is used in calls to PINT_sm_fram() */
#define PINT_FRAME_CURRENT 0

struct PINT_state_machine_s pvfs2_void_sm;

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */

#endif
