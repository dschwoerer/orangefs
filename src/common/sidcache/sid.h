/*
 * (C) 2012 Clemson University
 *
 * See COPYING in top-level directory.
*/

#ifndef SID_H
#define SID_H 1

#include "pvfs-sid.h"

extern int SID_initialize(void);
extern int SID_load(const char *path);
extern int SID_loadbuffer(const char *buffer, int size);
extern int SID_save(const char *path);
extern int SID_savelist(char *buffer, int size, SID_server_list_t *slist);
extern int SID_add(const PVFS_SID *sid,
                   PVFS_BMI_addr_t bmi_addr,
                   const char *url);
extern int SID_delete(const PVFS_SID *sid);
extern int SID_finalize(void);

#endif /* SID_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */