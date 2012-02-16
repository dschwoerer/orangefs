/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include "bmi.h"
#include "pvfs2-req-proto.h"
#include "gossip.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "PINT-reqproto-encode.h"
#include "PINT-reqproto-module.h"
#include <assert.h>

#include "pint-distribution.h"

int do_decode_resp(
    void *input_buffer,
    int input_size,
    struct PINT_decoded_msg *target_msg,
    PVFS_BMI_addr_t target_addr)
{

    struct PVFS_server_resp *response = input_buffer;
    struct PVFS_server_resp *decoded_response = NULL;
    int payload_size = input_size;

    assert(input_size > 0);

    target_msg->buffer = malloc(payload_size);
    memcpy(target_msg->buffer, response, payload_size);

    decoded_response = (struct PVFS_server_resp *) target_msg->buffer;

    switch (response->op)
    {

    case PVFS_SERV_GETCONFIG:
	((struct PVFS_server_resp *) target_msg->buffer)->u.getconfig.
	    fs_config_buf =
	    (char *) (target_msg->buffer + sizeof(struct PVFS_server_resp));
	((struct PVFS_server_resp *) target_msg->buffer)->u.getconfig.
	    server_config_buf =
	    (char *) (target_msg->buffer + sizeof(struct PVFS_server_resp) +
		      response->u.getconfig.fs_config_buf_size);
	return 0;
    case PVFS_SERV_LOOKUP_PATH:
	((struct PVFS_server_resp *) target_msg->buffer)->u.lookup_path.
	    handle_array =
	    (PVFS_handle *) ((char *) target_msg->buffer +
			     sizeof(struct PVFS_server_resp));
	((struct PVFS_server_resp *) target_msg->buffer)->u.lookup_path.
	    attr_array =
	    (PVFS_object_attr *) ((char *) target_msg->buffer +
				  sizeof(struct PVFS_server_resp) +
				  sizeof(PVFS_handle) *
				  response->u.lookup_path.handle_count);
	return 0;

    case PVFS_SERV_READDIR:
	((struct PVFS_server_resp *) target_msg->buffer)->u.readdir.
	    dirent_array =
	    (PVFS_dirent *) (target_msg->buffer +
			     sizeof(struct PVFS_server_resp));
	return 0;
    case PVFS_SERV_MGMT_PERF_MON:
	((struct PVFS_server_resp *) target_msg->buffer)->u.mgmt_perf_mon.
	    perf_array = 
	    (struct PVFS_mgmt_perf_stat*)(target_msg->buffer +
					  sizeof(struct PVFS_server_resp));
	return(0);
    case PVFS_SERV_MGMT_EVENT_MON:
	((struct PVFS_server_resp *) target_msg->buffer)->u.mgmt_event_mon.
	    event_array = 
	    (struct PVFS_mgmt_event*)(target_msg->buffer +
					  sizeof(struct PVFS_server_resp));
	return(0);
    case PVFS_SERV_MGMT_ITERATE_HANDLES:
	((struct PVFS_server_resp *) target_msg->buffer)->u.mgmt_iterate_handles.
	    handle_array = 
	    (PVFS_handle*)(target_msg->buffer + sizeof(struct PVFS_server_resp));
	return(0);
    case PVFS_SERV_MGMT_DSPACE_INFO_LIST:
	((struct PVFS_server_resp *) target_msg->buffer)->u.mgmt_dspace_info_list.
	    dspace_info_array = 
	    (struct PVFS_mgmt_dspace_info*)(target_msg->buffer + sizeof(struct PVFS_server_resp));
	return(0);


    case PVFS_SERV_GETATTR:
	if (decoded_response->u.getattr.attr.objtype == PVFS_TYPE_METAFILE)
	{
            if (decoded_response->u.getattr.attr.mask & PVFS_ATTR_META_DFILES)
            {
                decoded_response->u.getattr.attr.u.meta.dfile_array =
                    (PVFS_handle *) (((char *) decoded_response)
                                     + sizeof(struct PVFS_server_resp));
                decoded_response->u.getattr.attr.u.meta.dist =
                    (PINT_dist *) (((char *) decoded_response)
                                   + sizeof(struct PVFS_server_resp)
                                   +
                                   (decoded_response->u.getattr.attr.u.meta.
                                    dfile_count * sizeof(PVFS_handle)));
            }
            if (decoded_response->u.getattr.attr.mask & PVFS_ATTR_META_DIST)
            {
                PINT_dist_decode(decoded_response->u.getattr.attr.u.meta.dist,
                                 NULL);
            }
	}
        else if (decoded_response->u.getattr.attr.objtype == PVFS_TYPE_SYMLINK)
        {
            if (decoded_response->u.getattr.attr.mask & PVFS_ATTR_SYMLNK_TARGET)
            {
                decoded_response->u.getattr.attr.u.sym.target_path_len =
                    *((uint32_t *)(target_msg->buffer +
                                   sizeof(struct PVFS_server_resp)));
                decoded_response->u.getattr.attr.u.sym.target_path =
                    (char *)(target_msg->buffer + sizeof(uint32_t) +
                             sizeof(struct PVFS_server_resp));
            }
        }
	return 0;
    case PVFS_SERV_CREATE:
    case PVFS_SERV_SETATTR:
    case PVFS_SERV_REMOVE:
    case PVFS_SERV_CRDIRENT:
    case PVFS_SERV_MKDIR:
    case PVFS_SERV_RMDIRENT:
    case PVFS_SERV_CHDIRENT:
    case PVFS_SERV_IO:
    case PVFS_SERV_WRITE_COMPLETION:
    case PVFS_SERV_FLUSH:
    case PVFS_SERV_MGMT_SETPARAM:
    case PVFS_SERV_TRUNCATE:
    case PVFS_SERV_MGMT_NOOP:
    case PVFS_SERV_STATFS:
    case PVFS_SERV_PROTO_ERROR:
	return 0;

    /* invalid response types: */
    case PVFS_SERV_INVALID:
    case PVFS_SERV_PERF_UPDATE:
    case PVFS_SERV_JOB_TIMER:
	assert(0);
	gossip_lerr("Error: response type %d is invalid.\n", 
	    (int)response->op);
	return(-ENOSYS);
    }
    return -1;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */