/*
 * (C) 2012 Clemson University
 *
 * See COPYING in top-level directory.
*/

#ifndef SIDCACHEVAL_H
#define SIDCACHEVAL_H 1

#include "policy.h"

typedef int64_t BMI_addr; /* equivalent to PVFS_BMI_adddr_t */

typedef struct SID_cacheval_s
{
    int attr[SID_NUM_ATTR];
    BMI_addr bmi_addr;
    char url[0];
    int server_type;
} SID_cacheval_t;

/* server type file if cacheval holds an OR of these flags indicating
 * what the server does
 */

enum {
    SID_SERVER_NULL =    0,
    SID_SERVER_META =    1,
    SID_SERVER_DATA =    2,
    SID_SERVER_DIRDATA = 4,
    SID_SERVER_PRIME =   8,
    SID_SERVER_ROOT =   16
};

/* these are defined in policyeval.c */
/* they depend on SID_NUM_ATTR and thus they are here */

extern DB *SID_attr_index[SID_NUM_ATTR];

extern DBC *SID_attr_cursor[SID_NUM_ATTR];

#endif
/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
