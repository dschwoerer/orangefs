/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* PVFS Types
 */
#ifndef __PVFS2_TYPES_H
#define __PVFS2_TYPES_H

#include <stdint.h>
#include <time.h>

typedef char    *PVFS_string;
typedef uint8_t  PVFS_boolean;
typedef int64_t  PVFS_offset;
typedef int64_t  PVFS_size;
typedef uint32_t PVFS_bitfield;
typedef int32_t  PVFS_flag;
typedef int32_t  PVFS_count32;
typedef int64_t  PVFS_count64;
typedef uint64_t PVFS_volume_id;
typedef uint64_t PVFS_flow_id;
typedef int64_t  PVFS_handle;
typedef int16_t  PVFS_type;
typedef int32_t  PVFS_fs_id;
typedef int32_t  PVFS_error;
typedef int32_t  PVFS_magic;
typedef uint32_t PVFS_uid;
typedef uint32_t PVFS_gid;
typedef uint32_t PVFS_permissions;
typedef int64_t  PVFS_token;
typedef time_t   PVFS_time; /* TODO: is this really a good idea? */
typedef int32_t  PVFS_msg_tag_t;
typedef int32_t  PVFS_context_id;

#define PVFS_NAME_MAX    256 /* Max length of PVFS filename */
#define PVFS_TOKEN_START 0 /* Token value for readdir */
#define PVFS_TOKEN_END   1
#define MAX_STRING_SIZE  1000

/* Pinode Number */
typedef struct
{
    int64_t handle;		/* unique identifier per PVFS2 file */
    PVFS_fs_id fs_id;		/* Filesystem ID */
} pinode_reference;

/* PVFS directory entry */
struct PVFS_dirent_s {
    /*pinode_number pinode_no;*/
    char d_name[PVFS_NAME_MAX + 1];
     PVFS_handle handle;
    /* something about how to get to the next one ? */
};
typedef struct PVFS_dirent_s PVFS_dirent;

/* PVFS_credentials structure */
struct PVFS_credentials_s {
    PVFS_uid uid;
    PVFS_gid gid;
    PVFS_permissions perms;
};
typedef struct PVFS_credentials_s PVFS_credentials;

struct extent {
    int64_t first;
    int64_t last;
};
typedef struct extent PVFS_extent;
/* PVFS2 errors
 *
 * Errors are made up of a code to indicate the error type and a class
 * that indicates where the error came from.  These are |'d together.
 */

/* TODO: this should probably be prototyped somewhere else? */
void PVFS_perror(char* text, int retcode);

/* 7 bits are used for the error code */
#define PVFS_ERROR_CODE(__error)  ((__error) &   (int32_t) 0x7f)
#define PVFS_ERROR_CLASS(__error) ((__error) & ~((int32_t) 0x7f))

#define PVFS_ERROR_BMI    (1 << 7) /* BMI-specific error (e.g. socket got closed ) */
#define PVFS_ERROR_TROVE  (2 << 7) /* Trove-specific error (e.g. no space on device) */
#define PVFS_ERROR_FLOW   (3 << 7)
#define PVFS_ERROR_SM     (4 << 7) /* state machine specific error */
#define PVFS_ERROR_SCHED  (5 << 7)
#define PVFS_ERROR_CLIENT (6 << 7)
#define PVFS_ERROR_FS     (7 << 7) /* general file system semantics error (e.g. no such file) */

/* PVFS_ERROR_TO_ERRNO - macro for mapping from a PVFS error value
 * to a local UNIX errno value
 */
extern int32_t PINT_errno_mapping[];
#define PVFS_ERROR_TO_ERRNO(__error) PINT_errno_mapping[PVFS_ERROR_CODE(__error)]

/* PVFS2 error codes, compliments of asm/errno.h */
#define PVFS_EPERM		 1	/* Operation not permitted */
#define PVFS_ENOENT		 2	/* No such file or directory */
#define PVFS_EINTR		 3	/* Interrupted system call */
#define PVFS_EIO		 4	/* I/O error */
#define PVFS_ENXIO		 5	/* No such device or address */
#define PVFS_EBADF		 6	/* Bad file number */
#define PVFS_EAGAIN		 7	/* Try again */
#define PVFS_ENOMEM		 8	/* Out of memory */
#define PVFS_EFAULT		 9	/* Bad address */
#define PVFS_EBUSY		10	/* Device or resource busy */
#define PVFS_EEXIST		11	/* File exists */
#define PVFS_ENODEV		12	/* No such device */
#define PVFS_ENOTDIR		13	/* Not a directory */
#define PVFS_EISDIR		14	/* Is a directory */
#define PVFS_EINVAL		15	/* Invalid argument */
#define PVFS_EMFILE		16	/* Too many open files */
#define PVFS_EFBIG		17	/* File too large */
#define PVFS_ENOSPC		18	/* No space left on device */
#define PVFS_EROFS		19	/* Read-only file system */
#define PVFS_EMLINK		20	/* Too many links */
#define PVFS_EPIPE		21	/* Broken pipe */
#define PVFS_EDEADLK		22	/* Resource deadlock would occur */
#define PVFS_ENAMETOOLONG	23	/* File name too long */
#define PVFS_ENOLCK		24	/* No record locks available */
#define PVFS_ENOSYS		25	/* Function not implemented */
#define PVFS_ENOTEMPTY	        26	/* Directory not empty */
#define PVFS_ELOOP		27	/* Too many symbolic links encountered */
#define PVFS_EWOULDBLOCK	28	/* Operation would block */
#define PVFS_ENOMSG		29	/* No message of desired type */
#define PVFS_EUNATCH		30	/* Protocol driver not attached */
#define PVFS_EBADR		31	/* Invalid request descriptor */
#define PVFS_EDEADLOCK	        32
#define PVFS_ENODATA		33	/* No data available */
#define PVFS_ETIME		34	/* Timer expired */
#define PVFS_ENONET		35	/* Machine is not on the network */
#define PVFS_EREMOTE		36	/* Object is remote */
#define PVFS_ECOMM		37	/* Communication error on send */
#define PVFS_EPROTO		38	/* Protocol error */
#define PVFS_EBADMSG		39	/* Not a data message */
#define PVFS_EOVERFLOW	        40	/* Value too large for defined data type */
#define PVFS_ERESTART	        41	/* Interrupted system call should be restarted */
#define PVFS_EMSGSIZE	        42	/* Message too long */
#define PVFS_EPROTOTYPE	        43	/* Protocol wrong type for socket */
#define PVFS_ENOPROTOOPT	44	/* Protocol not available */
#define PVFS_EPROTONOSUPPORT	45	/* Protocol not supported */
#define PVFS_EOPNOTSUPP	        46	/* Operation not supported on transport endpoint */
#define PVFS_EADDRINUSE	        47	/* Address already in use */
#define PVFS_EADDRNOTAVAIL	48	/* Cannot assign requested address */
#define PVFS_ENETDOWN	        49	/* Network is down */
#define PVFS_ENETUNREACH	50	/* Network is unreachable */
#define PVFS_ENETRESET	        51	/* Network dropped connection because of reset */
#define PVFS_ENOBUFS		52	/* No buffer space available */
#define PVFS_ETIMEDOUT	        53	/* Connection timed out */
#define PVFS_ECONNREFUSED	54	/* Connection refused */
#define PVFS_EHOSTDOWN	        55	/* Host is down */
#define PVFS_EHOSTUNREACH	56	/* No route to host */
#define PVFS_EALREADY	        57	/* Operation already in progress */
#define PVFS_EREMOTEIO	        58	/* Remote I/O error */
#define PVFS_ENOMEDIUM	        59	/* No medium found */
#define PVFS_EMEDIUMTYPE	60	/* Wrong medium type */

/* NOTE: PLEASE DO NOT ARBITRARILY ADD NEW ERROR CODES!
 *
 * IF YOU CHOOSE TO ADD A NEW ERROR CODE (DESPITE OUR PLEA),
 * YOU ALSO NEED TO INCREMENT PVFS_ERRNO MAX (BELOW) AND ADD
 * A MAPPING TO A UNIX ERRNO VALUE IN src/common/misc/errno-mapping.c
 */

#define PVFS_ERRNO_MAX          60

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */

#endif /* __PVFS2_TYPES_H */
