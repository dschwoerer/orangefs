/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * Changes by Acxiom Corporation to add PINT_check_mode() helper function
 * as a replacement for check_mode() in permission checking, also added
 * PINT_check_group() for supplimental group support 
 * Copyright � Acxiom Corporation, 2005.
 *
 * See COPYING in top-level directory.
 */

/* This file includes definitions of common internal utility functions */
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <assert.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>

#include "pvfs2-types.h"
#include "pint-util.h"
#include "gen-locks.h"
#include "gossip.h"
#include "pvfs2-debug.h"
#include "bmi-byteswap.h"

static int current_tag = 1;
static gen_mutex_t current_tag_lock = GEN_MUTEX_INITIALIZER;

static gen_mutex_t check_group_mutex = GEN_MUTEX_INITIALIZER;
static char* check_group_pw_buffer = NULL;
static long check_group_pw_buffer_size = 0;
static char* check_group_gr_buffer = NULL;
static long check_group_gr_buffer_size = 0;
static int PINT_check_group(uid_t uid, gid_t gid);

void PINT_time_mark(PINT_time_marker *out_marker)
{
    struct rusage usage;

    gettimeofday(&out_marker->wtime, NULL);
    getrusage(RUSAGE_SELF, &usage);
    out_marker->utime = usage.ru_utime;
    out_marker->stime = usage.ru_stime;
}

void PINT_time_diff(PINT_time_marker mark1, 
                    PINT_time_marker mark2,
                    double *out_wtime_sec,
                    double *out_utime_sec,
                    double *out_stime_sec)
{
    *out_wtime_sec = 
        ((double)mark2.wtime.tv_sec +
         (double)(mark2.wtime.tv_usec) / 1000000) -
        ((double)mark1.wtime.tv_sec +
         (double)(mark1.wtime.tv_usec) / 1000000);

    *out_stime_sec = 
        ((double)mark2.stime.tv_sec +
         (double)(mark2.stime.tv_usec) / 1000000) -
        ((double)mark1.stime.tv_sec +
         (double)(mark1.stime.tv_usec) / 1000000);

    *out_utime_sec = 
        ((double)mark2.utime.tv_sec +
         (double)(mark2.utime.tv_usec) / 1000000) -
        ((double)mark1.utime.tv_sec +
         (double)(mark1.utime.tv_usec) / 1000000);
}

PVFS_msg_tag_t PINT_util_get_next_tag(void)
{
    PVFS_msg_tag_t ret;

    gen_mutex_lock(&current_tag_lock);
    ret = current_tag;

    /* increment the tag, don't use zero */
    if (current_tag + 1 == PINT_MSG_TAG_INVALID)
    {
	current_tag = 1;
    }
    else
    {
	current_tag++;
    }
    gen_mutex_unlock(&current_tag_lock);

    return ret;
}

int PINT_copy_object_attr(PVFS_object_attr *dest, PVFS_object_attr *src)
{
    int ret = -PVFS_ENOMEM;

    if (dest && src)
    {
	if (src->mask & PVFS_ATTR_COMMON_UID)
        {
            dest->owner = src->owner;
        }
	if (src->mask & PVFS_ATTR_COMMON_GID)
        {
            dest->group = src->group;
        }
	if (src->mask & PVFS_ATTR_COMMON_PERM)
        {
            dest->perms = src->perms;
        }
	if (src->mask & PVFS_ATTR_COMMON_ATIME)
        {
            dest->atime = src->atime;
        }
	if (src->mask & PVFS_ATTR_COMMON_CTIME)
        {
            dest->ctime = src->ctime;
        }
        if (src->mask & PVFS_ATTR_COMMON_MTIME)
        {
            dest->mtime = src->mtime;
        }
	if (src->mask & PVFS_ATTR_COMMON_TYPE)
        {
            dest->objtype = src->objtype;
        }

        if (src->mask & PVFS_ATTR_DIR_DIRENT_COUNT)
        {
            dest->u.dir.dirent_count = 
                src->u.dir.dirent_count;
        }

        if (src->mask & PVFS_ATTR_DIR_HINT)
        {
            dest->u.dir.hint.dfile_count = 
                src->u.dir.hint.dfile_count;
            dest->u.dir.hint.dist_name_len =
                src->u.dir.hint.dist_name_len;
            if (dest->u.dir.hint.dist_name_len > 0)
            {
                dest->u.dir.hint.dist_name = strdup(src->u.dir.hint.dist_name);
                if (dest->u.dir.hint.dist_name == NULL)
                {
                    return ret;
                }
            }
            dest->u.dir.hint.dist_params_len =
                src->u.dir.hint.dist_params_len;
            if (dest->u.dir.hint.dist_params_len > 0)
            {
                dest->u.dir.hint.dist_params = strdup(src->u.dir.hint.dist_params);
                if (dest->u.dir.hint.dist_params == NULL)
                {
                    free(dest->u.dir.hint.dist_name);
                    return ret;
                }
            }
        }

        /*
          NOTE:
          we only copy the size out if we're actually a
          datafile object.  sometimes the size field is
          valid when the objtype is a metafile because
          of different uses of the acache.  In this case
          (namely, getattr), the size is stored in the
          acache before this deep copy, so it's okay
          that we're not copying here even though the
          size mask bit is set.

          if we don't do this trick, the metafile that
          caches the size will have it's union data
          overwritten with a bunk size.
        */
        if ((src->mask & PVFS_ATTR_DATA_SIZE) &&
            (src->mask & PVFS_ATTR_COMMON_TYPE) &&
            (src->objtype == PVFS_TYPE_DATAFILE))
        {
            dest->u.data.size = src->u.data.size;
        }

	if ((src->mask & PVFS_ATTR_COMMON_TYPE) &&
            (src->objtype == PVFS_TYPE_METAFILE))
        {      
            if(src->mask & PVFS_ATTR_META_DFILES)
            {
                PVFS_size df_array_size = src->u.meta.dfile_count *
                    sizeof(PVFS_handle);

                if (df_array_size)
                {
                    if ((dest->mask & PVFS_ATTR_META_DFILES) &&
                        dest->u.meta.dfile_count > 0)
                    {
                        if (dest->u.meta.dfile_array)
                        {
                            free(dest->u.meta.dfile_array);
                        }
                    }
                    dest->u.meta.dfile_array = malloc(df_array_size);
                    if (!dest->u.meta.dfile_array)
                    {
                        return ret;
                    }
                    memcpy(dest->u.meta.dfile_array,
                           src->u.meta.dfile_array, df_array_size);
                }
                else
                {
                    dest->u.meta.dfile_array = NULL;
                }
                dest->u.meta.dfile_count = src->u.meta.dfile_count;
            }

            if(src->mask & PVFS_ATTR_META_DIST)
            {
                assert(src->u.meta.dist_size > 0);

                if ((dest->mask & PVFS_ATTR_META_DIST))
                {
                    PINT_dist_free(dest->u.meta.dist);
                }
                dest->u.meta.dist = PINT_dist_copy(src->u.meta.dist);
                if (dest->u.meta.dist == NULL)
                {
                    return ret;
                }
                dest->u.meta.dist_size = src->u.meta.dist_size;
            }
        }

        if (src->mask & PVFS_ATTR_SYMLNK_TARGET)
        {
            dest->u.sym.target_path_len = src->u.sym.target_path_len;
            dest->u.sym.target_path = strdup(src->u.sym.target_path);
            if (dest->u.sym.target_path == NULL)
            {
                return ret;
            }
        }

	dest->mask = src->mask;
        ret = 0;
    }
    return ret;
}

void PINT_free_object_attr(PVFS_object_attr *attr)
{
    if (attr)
    {
        if (attr->objtype == PVFS_TYPE_METAFILE)
        {
            if (attr->mask & PVFS_ATTR_META_DFILES)
            {
                if (attr->u.meta.dfile_array)
                {
                    free(attr->u.meta.dfile_array);
                }
            }
            if (attr->mask & PVFS_ATTR_META_DIST)
            {
                if (attr->u.meta.dist)
                {
                    PINT_dist_free(attr->u.meta.dist);
                }
            }
        }
        else if (attr->objtype == PVFS_TYPE_SYMLINK)
        {
            if (attr->mask & PVFS_ATTR_SYMLNK_TARGET)
            {
                if ((attr->u.sym.target_path_len > 0) &&
                    attr->u.sym.target_path)
                {
                    free(attr->u.sym.target_path);
                }
            }
        }
        else if (attr->objtype == PVFS_TYPE_DIRECTORY)
        {
            if ((attr->mask & PVFS_ATTR_DIR_HINT) || (attr->mask & PVFS_ATTR_DIR_DIRENT_COUNT))
            {
                if (attr->u.dir.hint.dist_name)
                {
                    free(attr->u.dir.hint.dist_name);
                }
                if (attr->u.dir.hint.dist_params)
                {
                    free(attr->u.dir.hint.dist_params);
                }
            }
        }
    }
}

/* PINT_check_mode()
 *
 * checks to see if the type of access described by "access_type" is permitted 
 * for user "uid" of group "gid" on the object with attributes "attr"
 *
 * returns 0 on success, -PVFS_EACCES if permission is not granted
 */
int PINT_check_mode(
    PVFS_object_attr *attr,
    PVFS_uid uid, PVFS_gid gid,
    enum PINT_access_type access_type)
{
    int in_group_flag = 0;
    int ret = 0;
    uint32_t perm_mask = (PVFS_ATTR_COMMON_UID |
                          PVFS_ATTR_COMMON_GID |
                          PVFS_ATTR_COMMON_PERM);

    /* if we don't have masks for the permission information that we
     * need, then the system is broken
     */
    assert((attr->mask & perm_mask) == perm_mask);

    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - check_mode called --- "
                 "(uid=%d,gid=%d,access_type=%d)\n", uid, gid, access_type);
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - object attributes --- "
                 "(uid=%d,gid=%d,mode=%d)\n", attr->owner, attr->group,
                 attr->perms);

    /* give root permission, no matter what */
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG,
                 " - checking if uid (%d) is root ...\n", uid);
    if (uid == 0)
    {
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
        return 0;
    }
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - no\n");

    /* see if uid matches object owner */
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - checking if owner (%d) "
        "matches uid (%d)...\n", attr->owner, uid);
    if(attr->owner == uid)
    {
        /* see if object user permissions match access type */
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - checking if permissions "
            "(%d) allows access type (%d) for user...\n", attr->perms, access_type);
        if(access_type == PINT_ACCESS_READABLE && (attr->perms &
            PVFS_U_READ))
        {
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
            return(0);
        }
        if(access_type == PINT_ACCESS_WRITABLE && (attr->perms &
            PVFS_U_WRITE))
        {
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
            return(0);
        }
        if(access_type == PINT_ACCESS_EXECUTABLE && (attr->perms &
            PVFS_U_EXECUTE))
        {
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
            return(0);
        }
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - no\n");
    }
    else
    {
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - no\n");
    }

    /* see if other bits allow access */
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - checking if permissions "
        "(%d) allows access type (%d) by others...\n", attr->perms, access_type);
    if(access_type == PINT_ACCESS_READABLE && (attr->perms &
        PVFS_O_READ))
    {
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
        return(0);
    }
    if(access_type == PINT_ACCESS_WRITABLE && (attr->perms &
        PVFS_O_WRITE))
    {
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
        return(0);
    }
    if(access_type == PINT_ACCESS_EXECUTABLE && (attr->perms &
        PVFS_O_EXECUTE))
    {
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
        return(0);
    }
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - no\n");

    /* see if gid matches object group */
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - checking if group (%d) "
        "matches gid (%d)...\n", attr->group, gid);
    if(attr->group == gid)
    {
        /* default group match */
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
        in_group_flag = 1;
    }
    else
    {
        /* no default group match, check supplementary groups */
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - no\n");
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - checking for"
            " supplementary group match...\n");
        ret = PINT_check_group(uid, attr->group);
        if(ret == 0)
        {
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
            in_group_flag = 1;
        }
        else
        {
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - no\n");
            if(ret != -PVFS_ENOENT)
            {
                /* system error; not just failed match */
                return(ret);
            }
        }
    }

    if(in_group_flag)
    {
        /* see if object group permissions match access type */
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - checking if permissions "
            "(%d) allows access type (%d) for group...\n", attr->perms, access_type);
        if(access_type == PINT_ACCESS_READABLE && (attr->perms &
            PVFS_G_READ))
        {
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
            return(0);
        }
        if(access_type == PINT_ACCESS_WRITABLE && (attr->perms &
            PVFS_G_WRITE))
        {
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
            return(0);
        }
        if(access_type == PINT_ACCESS_EXECUTABLE && (attr->perms &
            PVFS_G_EXECUTE))
        {
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - yes\n");
            return(0);
        }
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, " - no\n");
    }
  
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "******PINT_check_mode: denying access\n");
    /* default case: access denied */
    return -PVFS_EACCES;
}

/* PINT_check_group()
 *
 * checks to see if uid is a member of gid
 * 
 * returns 0 on success, -PVFS_ENOENT if not a member, other PVFS error codes
 * on system failure
 */
static int PINT_check_group(uid_t uid, gid_t gid)
{
    struct passwd pwd;
    struct passwd* pwd_p = NULL;
    struct group grp;
    struct group* grp_p = NULL;
    int i = 0;
    int ret = -1;

    /* Explanation: 
     *
     * We use the _r variants of getpwuid and getgrgid in order to insure
     * thread safety; particularly if this function ever gets called in a
     * client side situation in which we can't prevent the application from
     * making conflicting calls.
     *
     * These _r functions require that a buffer be supplied for the user and
     * group information, however.  These buffers may be unconfortably large
     * for the stack, so we malloc them on a static pointer and then mutex
     * lock this function so that it can still be reentrant.
     */

    gen_mutex_lock(&check_group_mutex);

    if(!check_group_pw_buffer)
    {
        /* need to create a buffer for pw and grp entries */
#if defined(_SC_GETGR_R_SIZE_MAX) && defined(_SC_GETPW_R_SIZE_MAX)
        /* newish posix systems can tell us what the max buffer size is */
        check_group_gr_buffer_size = sysconf(_SC_GETGR_R_SIZE_MAX);
        check_group_pw_buffer_size = sysconf(_SC_GETPW_R_SIZE_MAX);
#else
        /* fall back for older systems */
        check_group_pw_buffer_size = 1024;
        check_group_gr_buffer_size = 1024;
#endif
        check_group_pw_buffer = (char*)malloc(check_group_pw_buffer_size);
        check_group_gr_buffer = (char*)malloc(check_group_gr_buffer_size);
        if(!check_group_pw_buffer || !check_group_gr_buffer)
        {
            if(check_group_pw_buffer)
            {
                free(check_group_pw_buffer);
                check_group_pw_buffer = NULL;
            }
            if(check_group_gr_buffer)
            {
                free(check_group_gr_buffer);
                check_group_gr_buffer = NULL;
            }
            gen_mutex_unlock(&check_group_mutex);
            return(-PVFS_ENOMEM);
        }
    }

    /* get user information */
    ret = getpwuid_r(uid, &pwd, check_group_pw_buffer,
        check_group_pw_buffer_size,
        &pwd_p);
    if(ret != 0)
    {
        gen_mutex_unlock(&check_group_mutex);
        return(-PVFS_ENOENT);
    }

    /* check primary group */
    if(pwd.pw_gid == gid) return 0;

    /* get other group information */
    ret = getgrgid_r(gid, &grp, check_group_gr_buffer,
        check_group_gr_buffer_size,
        &grp_p);
    if(ret != 0)
    {
        gen_mutex_unlock(&check_group_mutex);
        return(-PVFS_ENOENT);
    }

    gen_mutex_unlock(&check_group_mutex);


    for(i = 0; grp.gr_mem[i] != NULL; i++)
    {
        if(0 == strcmp(pwd.pw_name, grp.gr_mem[i]) )
        {
            gen_mutex_unlock(&check_group_mutex);
            return 0;
        } 
    }

    gen_mutex_unlock(&check_group_mutex);
    return(-PVFS_ENOENT);
}

/* Checks if a given user is part of any groups that matches the file gid */
static int in_group_p(PVFS_uid uid, PVFS_gid gid, PVFS_gid attr_group)
{
    if (attr_group == gid)
        return 1;
    if (PINT_check_group(uid, attr_group) == 0)
        return 1;
    return 0;
}

/*
 * Return 0 if requesting clients is granted want access to the object
 * by the acl. Returns -PVFS_E... otherwise.
 */
int PINT_check_acls(void *acl_buf, size_t acl_size, 
    PVFS_object_attr *attr,
    PVFS_uid uid, PVFS_gid gid, int want)
{
    pvfs2_acl_entry pe, *pa;
    int i = 0, found = 0, count = 0;
    uint32_t perm_mask = (PVFS_ATTR_COMMON_UID |
                          PVFS_ATTR_COMMON_GID |
                          PVFS_ATTR_COMMON_PERM);

    if (acl_size == 0)
    {
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "no acl's present.. denying access\n");
        return -PVFS_EACCES;
    }

    /* keyval for ACLs includes a \0. so subtract the thingie */
    acl_size--;
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "PINT_check_acls: read keyval size "
    " %d (%d acl entries)\n",
        (int) acl_size, 
        (int) (acl_size / sizeof(pvfs2_acl_entry)));
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "uid = %d, gid = %d, want = %d\n",
        uid, gid, want);

    assert((attr->mask & perm_mask) == perm_mask);
    assert(acl_buf);
    assert(acl_size % sizeof(pvfs2_acl_entry) == 0);
    count = acl_size / sizeof(pvfs2_acl_entry);

    for (i = 0; i < count; i++)
    {
        pa = (pvfs2_acl_entry *) acl_buf + i;
        /* 
           NOTE: Remember that keyval is encoded as lebf, so convert it 
           to host representation 
        */
        pe.p_tag  = bmitoh32(pa->p_tag);
        pe.p_perm = bmitoh32(pa->p_perm);
        pe.p_id   = bmitoh32(pa->p_id);
        pa = &pe;
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "Decoded ACL entry %d "
            "(p_tag %d, p_perm %d, p_id %d)\n",
            i, pa->p_tag, pa->p_perm, pa->p_id);
        switch(pa->p_tag) 
        {
            case PVFS2_ACL_USER_OBJ:
                /* (May have been checked already) */
                if (attr->owner == uid)
                    goto check_perm;
                break;
            case PVFS2_ACL_USER:
                if (pa->p_id == uid)
                    goto mask;
                break;
            case PVFS2_ACL_GROUP_OBJ:
                if (in_group_p(uid, gid, attr->group)) 
                {
                    found = 1;
                    if ((pa->p_perm & want) == want)
                        goto mask;
                }
                break;
            case PVFS2_ACL_GROUP:
                if (in_group_p(uid, gid, pa->p_id)) {
                    found = 1;
                    if ((pa->p_perm & want) == want)
                        goto mask;
                }
                break;
            case PVFS2_ACL_MASK:
                break;
            case PVFS2_ACL_OTHER:
                if (found)
                {
                    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "(1) PINT_check_acls:"
                        "returning access denied\n");
                    return -PVFS_EACCES;
                }
                else
                    goto check_perm;
            default:
                gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "(2) PINT_check_acls: "
                        "returning EIO\n");
                return -PVFS_EIO;
        }
    }
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "(3) PINT_check_acls: returning EIO\n");
    return -PVFS_EIO;
mask:
    /* search the remaining entries */
    i = i + 1;
    for (; i < count; i++)
    {
        pvfs2_acl_entry me, *mask_obj = (pvfs2_acl_entry *) acl_buf + i;
        
        /* 
          NOTE: Again, since pvfs2_acl_entry is in lebf, we need to
          convert it to host endian format
         */
        me.p_tag  = bmitoh32(mask_obj->p_tag);
        me.p_perm = bmitoh32(mask_obj->p_perm);
        me.p_id   = bmitoh32(mask_obj->p_id);
        mask_obj = &me;
        gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "Decoded (mask) ACL entry %d "
            "(p_tag %d, p_perm %d, p_id %d)\n",
            i, mask_obj->p_tag, mask_obj->p_perm, mask_obj->p_id);
        if (mask_obj->p_tag == PVFS2_ACL_MASK) 
        {
            if ((pa->p_perm & mask_obj->p_perm & want) == want)
                return 0;
            gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "(4) PINT_check_acls:"
                "returning access denied (mask)\n");
            return -PVFS_EACCES;
        }
    }

check_perm:
    if ((pa->p_perm & want) == want)
        return 0;
    gossip_debug(GOSSIP_PERMISSIONS_DEBUG, "(5) PINT_check_acls: returning"
            "access denied\n");
    return -PVFS_EACCES;
}

char *PINT_util_get_object_type(int objtype)
{
    static char *obj_types[] =
    {
         "NONE", "METAFILE", "DATAFILE",
         "DIRECTORY", "SYMLINK", "DIRDATA", "UNKNOWN"
    };
    switch(objtype)
    {
    case PVFS_TYPE_NONE:
         return obj_types[0];
    case PVFS_TYPE_METAFILE:
         return obj_types[1];
    case PVFS_TYPE_DATAFILE:
         return obj_types[2];
    case PVFS_TYPE_DIRECTORY:
         return obj_types[3];
    case PVFS_TYPE_SYMLINK:
         return obj_types[4];
    case PVFS_TYPE_DIRDATA:
         return obj_types[5];
    }
    return obj_types[6];
}

PVFS_time PINT_util_get_current_time(void)
{
    struct timeval t = {0,0};
    PVFS_time current_time = 0;

    gettimeofday(&t, NULL);
    current_time = (PVFS_time)t.tv_sec;
    return current_time;
}

PVFS_time PINT_util_mktime_version(PVFS_time time)
{
    struct timeval t = {0,0};
    PVFS_time version = (time << 32);

    gettimeofday(&t, NULL);
    version |= (PVFS_time)t.tv_usec;
    return version;
}

PVFS_time PINT_util_mkversion_time(PVFS_time version)
{
    return (PVFS_time)(version >> 32);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
