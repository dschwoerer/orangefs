/* 
 * (C) 2009 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

#ifndef _SECURITY_UTIL_H_
#define _SECURITY_UTIL_H_




void PINT_null_capability(PVFS_capability *cap);
int PINT_capability_is_null(const PVFS_capability *cap);

PVFS_capability *PINT_dup_capability(const PVFS_capability *cap);
int PINT_copy_capability(const PVFS_capability *src, PVFS_capability *dest);
void PINT_cleanup_capability(PVFS_capability *cap);

PVFS_credential *PINT_dup_credential(const PVFS_credential *cred);
int PINT_copy_credential(const PVFS_credential *src, PVFS_credential *dest);
void PINT_cleanup_credential(PVFS_credential *cred);


#endif /* _SECURITY_UTIL_H_ */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
