/*
 * RDMA BMI handy utilities that are not really core functions.
 *
 * Copyright (C) 2003-6 Pete Wyckoff <pw@osc.edu>
 * Copyright (C) 2016 David Reynolds <david@omnibond.com>
 *
 * See COPYING in top-level directory.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

#define __util_c
#include "rdma.h"
#include "pvfs2-internal.h"

/*
 * TODO: Should error calls exit? This was changed in revision 10276.
 *       I believe the exit calls may have been commented out because
 *       exiting from here was causing memory leaks. But if it is an error
 *       shouldn't it at least provide a way to cleanup and then exit?
 */

/*
 * Utility functions.
 */
void __hidden
error (const char *fmt, ...)
{
    char s[2048];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(s, fmt, ap);
    va_end(ap);
    gossip_err("Error: %s.\n", s);
    gossip_backtrace();
    /*exit(1);*/
}

void __hidden
error_errno(const char *fmt, ...)
{
    char s[2048];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(s, fmt, ap);
    va_end(ap);
    gossip_err("Error: %s: %s.\n", s, strerror(errno));
    /*exit(1);*/
}

void __hidden
error_xerrno(int errnum, const char *fmt, ...)
{
    char s[2048];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(s, fmt, ap);
    va_end(ap);
    gossip_err("Error: %s: %s.\n", s, strerror(errnum));
    /* exit(1);*/
}

void __attribute__((format(printf,1,2))) __hidden
warning(const char *fmt, ...)
{
    char s[2048];
    va_list ap;

    va_start(ap, fmt);
    vsprintf(s, fmt, ap);
    va_end(ap);
    gossip_err("Warning: %s.\n", s);
}

/* removed "malloc" attribute */
void * __hidden
bmi_rdma_malloc(unsigned long n)
{
    char *x;

    if (n == 0)
    {
        error("%s: alloc 0 bytes", __func__);
        return NULL;
    }
    x = malloc(n);
    if (!x)
    {
        error("%s: malloc %ld bytes failed", __func__, n);
    }
    return x;
}

/*
 * Grab the first item and delete it from the list.
 */
void * __hidden
qlist_del_head(struct qlist_head *list)
{
    struct qlist_head *h;

    if (qlist_empty(list))
    {
        error("%s: empty list %p", __func__, list);
        return NULL;
    }
    h = list->next;
    qlist_del(h);
    return h;
}

void * __hidden
qlist_try_del_head(struct qlist_head *list)
{
    struct qlist_head *h;

    if (qlist_empty(list))
    {
        return 0;
    }
    h = list->next;
    qlist_del(h);
    return h;
}

/*
 * Debugging printf for sendq and recvq state names.
 */
static const char *
name_lookup(name_t *a, int num)
{
    while (a->num)
    {
        if (a->num == num)
        {
            return a->name;
        }
        ++a;
    }
    return "(unknown)";
}

const char *
sq_state_name(sq_state_t num)
{
    return name_lookup(sq_state_names, (int) num);
}

const char *
rq_state_name(rq_state_t num)
{
    return name_lookup(rq_state_names, (int) num);
}

const char *
msg_type_name(msg_type_t num)
{
    return name_lookup(msg_type_names, (int) num);
}

/*
 * Walk buflist, copying one way or the other, but no more than len
 * even if buflist could handle it.
 */
void
memcpy_to_buflist(rdma_buflist_t *buflist, const void *buf, bmi_size_t len)
{
    int i;
    const char *cp = buf;

    for (i = 0; i < buflist->num && len > 0; i++)
    {
        bmi_size_t bytes = buflist->len[i];
        if (bytes > len)
        {
            bytes = len;
        }
        memcpy(buflist->buf.recv[i], cp, bytes);
        cp += bytes;
        len -= bytes;
    }
}

void
memcpy_from_buflist(rdma_buflist_t *buflist, void *buf)
{
    int i;
    char *cp = buf;

    for (i = 0; i < buflist->num; i++)
    {
        memcpy(cp, buflist->buf.send[i], (size_t) buflist->len[i]);
        cp += buflist->len[i];
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
