/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* Remove Implementation */

#include <assert.h>

#include "pinode-helper.h"
#include "pvfs2-sysint.h"
#include "pint-sysint.h"
#include "pint-dcache.h"
#include "pint-servreq.h"
#include "pint-dcache.h"
#include "pint-bucket.h"
#include "pcache.h"
#include "PINT-reqproto-encode.h"

#define REQ_ENC_FORMAT 0

/* PVFS_sys_remove()
 *
 * Remove a file with specified attributes 
 *
 * returns 0 on success, -errno on failure
 */
int PVFS_sys_remove(PVFS_sysreq_remove *req)
{
	struct PVFS_server_req_s req_p;		    /* server request */
	struct PVFS_server_resp_s *ack_p = NULL;    /* server response */
	int ret = -1, ioserv_count = 0;
	pinode *pinode_ptr = NULL, *item_ptr = NULL;
	bmi_addr_t serv_addr;	/* PVFS address type structure */
	int name_sz = 0, attr_mask = 0;
	pinode_reference entry;
	int items_found = 0, i = 0;
	struct PINT_decoded_msg decoded;
	bmi_size_t max_msg_sz;

	enum {
	    NONE_FAILURE = 0,
	    GET_PINODE_FAILURE,
	    SERVER_LOOKUP_FAILURE,
	    SEND_REQ_FAILURE,
	    RECV_REQ_FAILURE,
	    REMOVE_CACHE_FAILURE,
	} failure = NONE_FAILURE;

	/* lookup meta file */
	attr_mask = ATTR_BASIC | ATTR_META;

	ret = PINT_do_lookup(req->entry_name, req->parent_refn, attr_mask,
				req->credentials, &entry);
	if (ret < 0)
	{
	    failure = GET_PINODE_FAILURE;
	    goto return_error;
	}

	/* get the pinode for the thing we're deleting */
	ret = phelper_get_pinode(entry, &pinode_ptr,
			attr_mask, req->credentials );
	if (ret < 0)
	{
	    failure = GET_PINODE_FAILURE;
	    goto return_error;
	}

	/* are we allowed to delete this file? */
	ret = check_perms(pinode_ptr->attr, req->credentials.perms,
				req->credentials.uid, req->credentials.gid);
	if (ret < 0)
	{
	    ret = (-EPERM);
	    failure = GET_PINODE_FAILURE;
	    goto return_error;
	}

	ret = PINT_bucket_map_to_server(&serv_addr, entry.handle, entry.fs_id);
	if (ret < 0)
	{
	    failure = SERVER_LOOKUP_FAILURE;
	    goto return_error;
	}

	/* send remove message to the meta file */

	max_msg_sz = sizeof(struct PVFS_server_resp_s);
	req_p.op = PVFS_SERV_REMOVE;
	req_p.rsize = sizeof(struct PVFS_server_req_s);
	req_p.credentials = req->credentials;
	req_p.u.remove.handle = pinode_ptr->pinode_ref.handle;
	req_p.u.remove.fs_id = req->parent_refn.fs_id;

	/* dead man walking */
	ret = PINT_server_send_req(serv_addr, &req_p, max_msg_sz, &decoded);
	if (ret < 0)
	{
	    failure = SEND_REQ_FAILURE;
	    goto return_error;
	}

	ack_p = (struct PVFS_server_resp_s *) decoded.buffer;

	if (ack_p->status < 0 )
	{
	    ret = ack_p->status;
	    failure = RECV_REQ_FAILURE;
	    goto return_error;
	}

	PINT_decode_release(&decoded, PINT_DECODE_RESP, REQ_ENC_FORMAT);

	/* rmdirent the dir entry */
	ret = PINT_bucket_map_to_server(&serv_addr, req->parent_refn.handle, req->parent_refn.fs_id);
	if (ret < 0)
	{
	    failure = SERVER_LOOKUP_FAILURE;
	    goto return_error;
	}

	name_sz = strlen(req->entry_name) + 1; /*include null terminator*/

	req_p.op = PVFS_SERV_RMDIRENT;
	req_p.rsize = sizeof(struct PVFS_server_req_s) + name_sz;
	req_p.credentials = req->credentials;
	req_p.u.rmdirent.entry = req->entry_name;
	req_p.u.rmdirent.parent_handle = req->parent_refn.handle;
	req_p.u.rmdirent.fs_id = req->parent_refn.fs_id;

	/* dead man walking */
	ret = PINT_server_send_req(serv_addr, &req_p, max_msg_sz, &decoded);
	if (ret < 0)
	{
	    failure = SEND_REQ_FAILURE;
	    goto return_error;
	}

	ack_p = (struct PVFS_server_resp_s *) decoded.buffer;

	if (ack_p->status < 0 )
	{
	    ret = ack_p->status;
	    failure = RECV_REQ_FAILURE;
	    goto return_error;
	}

	/* sanity check:
	 * rmdirent returns a handle to the meta file for the dirent that was 
	 * removed. if this isn't equal to what we passed in, we need to figure 
	 * out what we deleted and figure out why the server had the wrong link.
	 */

	assert(ack_p->u.rmdirent.entry_handle == pinode_ptr->pinode_ref.handle);

	PINT_decode_release(&decoded, PINT_DECODE_RESP, REQ_ENC_FORMAT);

	/* send remove messages to each of the data file servers */
	

	/* none of this stuff changes, so we don't need to set it in a loop */
	max_msg_sz = sizeof(struct PVFS_server_resp_s);
	req_p.op = PVFS_SERV_REMOVE;
	req_p.rsize = sizeof(struct PVFS_server_req_s);
	req_p.credentials = req->credentials;
	req_p.u.remove.fs_id = req->parent_refn.fs_id;

	ioserv_count = pinode_ptr->attr.u.meta.nr_datafiles;

	/* TODO: come back and unserialize this */
	for(i = 0; i < ioserv_count; i++)
	{
	    /* each of the data files could be on different servers, so we need
	     * to get the correct server from the bucket table interface
	     */
	    req_p.u.remove.handle = pinode_ptr->attr.u.meta.dfh[i];
	    ret = PINT_bucket_map_to_server(&serv_addr, req_p.u.remove.handle, req->parent_refn.fs_id);
	    if (ret < 0)
	    {
		failure = SERVER_LOOKUP_FAILURE;
		goto return_error;
	    }

	    ret = PINT_server_send_req(serv_addr, &req_p, max_msg_sz, &decoded);
	    if (ret < 0)
	    {
		failure = SEND_REQ_FAILURE;
		goto return_error;
	    }

	    ack_p = (struct PVFS_server_resp_s *) decoded.buffer;

	    if (ack_p->status < 0 )
	    {
		ret = ack_p->status;
		failure = RECV_REQ_FAILURE;
		goto return_error;
	    }

	    PINT_decode_release(&decoded, PINT_DECODE_RESP, REQ_ENC_FORMAT);
	}

	/* Remove the dentry from the dcache */
	ret = PINT_dcache_remove(req->entry_name,req->parent_refn,&items_found);
	if (ret < 0)
	{
	    failure = REMOVE_CACHE_FAILURE;
	    goto return_error;
	}

	/* Remove from pinode cache */
	ret = PINT_pcache_remove(entry,&item_ptr);
	if (ret < 0)
	{
	    failure = REMOVE_CACHE_FAILURE;
	    goto return_error;
	}

	/* free the pinode that we removed from cache */
	PINT_pcache_pinode_dealloc(item_ptr);

	return(0);
return_error:

    /* TODO: what exactly (if anything) do we want to roll back in case
     * something gets fubar'ed while we're removing data/meta/dirent files?
     */

    switch(failure)
    {
	case RECV_REQ_FAILURE:
	    PINT_decode_release(&decoded, PINT_DECODE_RESP, REQ_ENC_FORMAT);
	case SEND_REQ_FAILURE:
	case SERVER_LOOKUP_FAILURE:
	case GET_PINODE_FAILURE:
	case REMOVE_CACHE_FAILURE:
	case NONE_FAILURE:
	    break;
    }
    return(ret);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 noexpandtab
 */
