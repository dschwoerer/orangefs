/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/*
 * tcp specific host addressing information
 */

#ifndef __BMI_TCP_ADDRESSING_H
#define __BMI_TCP_ADDRESSING_H

#include "bmi-types.h"

/*****************************************************************
 * Information specific to tcp/ip
 */

/* this contains TCP/IP addressing information- it is filled in as
 * connections are made */
struct tcp_addr
{
    method_addr_p map;		/* points back to generic address */
    /* stores error code for addresses that are broken for some reason */
    int addr_error;		
    char *hostname;
    char *ipaddr;
    int port;
    int socket;
    /* flag that indicates this address represents a
     * server port on which connections may be accepted */
    int server_port;
    /* reference count of pending send operations to this address */
    int write_ref_count;
    /* is the socket connected yet? */
    int not_connected;
    /* socket collection link */
    struct qlist_head sc_link;
    int sc_index;
};

/*****************************************************************
 * function prototypes
 */

void tcp_forget_addr(method_addr_p map,
		     int dealloc_flag,
		     int error_code);
method_addr_p alloc_tcp_method_addr(void);

#endif /* __BMI_TCP_ADDRESSING_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
