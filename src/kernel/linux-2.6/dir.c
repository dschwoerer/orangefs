/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/** \file
 *  \ingroup pvfs2linux
 *
 *  Linux VFS directory operations.
 */

#include "pvfs2-kernel.h"
#include "pvfs2-sysint.h"
#include "pvfs2-internal.h"

extern struct list_head pvfs2_request_list;
extern spinlock_t pvfs2_request_list_lock;
extern wait_queue_head_t pvfs2_request_list_waitq;
extern int debug;

/* shared file/dir operations defined in file.c */
extern int pvfs2_file_open(
    struct inode *inode,
    struct file *file);
extern int pvfs2_file_release(
    struct inode *inode,
    struct file *file);

/** Read directory entries from an instance of an open directory.
 *
 * \param filldir callback function called for each entry read.
 *
 * \retval <0 on error
 * \retval 0  when directory has been completely traversed
 * \retval >0 if we don't call filldir for all entries
 *
 * \note If the filldir call-back returns non-zero, then readdir should
 *       assume that it has had enough, and should return as well.
 */
static int pvfs2_readdir(
    struct file *file,
    void *dirent,
    filldir_t filldir)
{
    int ret = 0;
    PVFS_ds_position pos = 0;
    ino_t ino = 0;
    struct dentry *dentry = file->f_dentry;
    pvfs2_kernel_op_t *new_op = NULL;
    pvfs2_inode_t *pvfs2_inode = PVFS2_I(dentry->d_inode);

  restart_readdir:

    pos = (PVFS_ds_position)file->f_pos;
    if (pos == 0)
    {
        pvfs2_inode->readdir_token_adjustment = 0;
    }

    pvfs2_print("pvfs2_readdir called on %s (pos=%d, tadj=%d, "
                "retry=%d, v=%llu)\n", dentry->d_name.name, (int)pos,
                (int)pvfs2_inode->readdir_token_adjustment,
                (int)pvfs2_inode->num_readdir_retries,
                llu(pvfs2_inode->directory_version));

    switch (pos)
    {
	/*
	   if we're just starting, populate the "." and ".." entries
	   of the current directory; these always appear
	 */
    case 0:
        if (pvfs2_inode->directory_version == 0)
        {
            ino = dentry->d_inode->i_ino;
            if (filldir(dirent, ".", 1, pos, ino, DT_DIR) < 0)
            {
                break;
            }
        }
        file->f_pos++;
        pos++;
	/* drop through */
    case 1:
        if (pvfs2_inode->directory_version == 0)
        {
            ino = parent_ino(dentry);
            if (filldir(dirent, "..", 2, pos, ino, DT_DIR) < 0)
            {
                break;
            }
        }
        file->f_pos++;
        pos++;
	/* drop through */
    default:
	/* handle the normal cases here */
	new_op = op_alloc();
	if (!new_op)
	{
	    return -ENOMEM;
	}
	new_op->upcall.type = PVFS2_VFS_OP_READDIR;

	if (pvfs2_inode && pvfs2_inode->refn.handle &&
            pvfs2_inode->refn.fs_id)
	{
	    new_op->upcall.req.readdir.refn = pvfs2_inode->refn;
	}
	else
	{
	    new_op->upcall.req.readdir.refn.handle =
		pvfs2_ino_to_handle(dentry->d_inode->i_ino);
	    new_op->upcall.req.readdir.refn.fs_id =
		PVFS2_SB(dentry->d_inode->i_sb)->fs_id;
	}
	new_op->upcall.req.readdir.max_dirent_count = MAX_DIRENT_COUNT;

	/* NOTE:
	   the position we send to the readdir upcall is out of
	   sync with file->f_pos since pvfs2 doesn't include the
	   "." and ".." entries that we added above.

	   so the proper pvfs2 position is (pos - 2), except where
	   pos == 0.  In that case, pos is PVFS_READDIR_START.

           the token adjustment is for the case where files or
           directories are being removed between calls to readdir.
           while we're progressing through the directory, our issued
           upcall offset needs to be adjusted less the number of
           objects in this directory that were removed.  the readdir
           retries are not fast enough on the local system, so we have
           to use this in addition just to be closer to correct.
        */
	new_op->upcall.req.readdir.token =
            (pos == 2 ? PVFS_READDIR_START : (pos - 2));

        if (new_op->upcall.req.readdir.token != PVFS_READDIR_START)
        {
            new_op->upcall.req.readdir.token -=
                pvfs2_inode->readdir_token_adjustment;
            if (new_op->upcall.req.readdir.token == 0)
            {
                new_op->upcall.req.readdir.token =
                    PVFS_READDIR_START;
            }
        }

        ret = service_operation(
            new_op, "pvfs2_readdir", PVFS2_OP_RETRY_COUNT,
            get_interruptible_flag(dentry->d_inode));

	pvfs2_print("Readdir downcall status is %d (dirent_count "
		    "is %d)\n", new_op->downcall.status,
		    new_op->downcall.resp.readdir.dirent_count);

	if (new_op->downcall.status == 0)
	{
	    int i = 0, len = 0;
	    ino_t current_ino = 0;
	    char *current_entry = NULL;

            if (new_op->downcall.resp.readdir.dirent_count == 0)
            {
                goto graceful_termination_path;
            }

            if (pvfs2_inode->directory_version == 0)
            {
                pvfs2_inode->directory_version =
                    new_op->downcall.resp.readdir.directory_version;
            }

            if (pvfs2_inode->num_readdir_retries > -1)
            {
                if (pvfs2_inode->directory_version !=
                    new_op->downcall.resp.readdir.directory_version)
                {
                    /*
                      if the token adjustment is moving rapidly, do
                      not attempt any retries
                    */
                    if ((pvfs2_inode->readdir_token_adjustment) &&
                        (pvfs2_inode->readdir_token_adjustment %
                         MAX_DIRENT_COUNT) == 0)
                    {
                        pvfs2_inode->last_version_changed = 1;
                    }

                    /*
                      don't bother to restart if the directory is
                      changing too rapidly.  either someone is
                      creating or removing (as in the case of a
                      typical rm -rf operation) too quickly and we
                      have no guarantee of the listing
                    */
                    if (pvfs2_inode->last_version_changed)
                    {
                        /* once set, do not clear until finished */
                        goto continue_listing;
                    }

                    pvfs2_print("detected directory change on listing; "
                                "starting over\n");

                    file->f_pos = 0;
                    pvfs2_inode->directory_version =
                        new_op->downcall.resp.readdir.directory_version;
                    pvfs2_inode->last_version_changed = 1;

                    op_release(new_op);
                    pvfs2_inode->num_readdir_retries--;
                    goto restart_readdir;
                }
                else
                {
                    /*
                      version hasn't changed between two consecutive
                      calls; clear the flag here
                    */
                    pvfs2_inode->last_version_changed = 0;
                }
            }
            else
            {
                pvfs2_print("Giving up on readdir retries to avoid "
                            "possible livelock (%d tries attempted)\n",
                            PVFS2_NUM_READDIR_RETRIES);
            }

          continue_listing:
	    for (i = 0; i < new_op->downcall.resp.readdir.dirent_count; i++)
	    {
                len = new_op->downcall.resp.readdir.d_name_len[i];
                current_entry =
                    &new_op->downcall.resp.readdir.d_name[i][0];
                current_ino =
                    pvfs2_handle_to_ino(
                        new_op->downcall.resp.readdir.refn[i].handle);

                if (filldir(dirent, current_entry, len, pos,
                            current_ino, DT_UNKNOWN) < 0)
                {
                  graceful_termination_path:

                    pvfs2_inode->directory_version = 0;
                    pvfs2_inode->num_readdir_retries =
                        PVFS2_NUM_READDIR_RETRIES;
                    pvfs2_inode->last_version_changed = 0;

                    ret = 0;
                    break;
                }
                file->f_pos++;
                pos++;
            }
	}
        else
        {
            pvfs2_print("Failed to readdir (downcall status %d)\n",
                        new_op->downcall.status);
        }

	op_release(new_op);
	break;
    }

#ifdef HAVE_TOUCH_ATIME
    touch_atime(file->f_vfsmnt, dentry);
#else
    update_atime(dentry->d_inode);
#endif


    pvfs2_print("pvfs2_readdir returning %d\n",ret);
    return ret;
}

/** PVFS2 implementation of VFS directory operations */
struct file_operations pvfs2_dir_operations =
{
#ifdef PVFS2_LINUX_KERNEL_2_4
    read : generic_read_dir,
    readdir : pvfs2_readdir,
    open : pvfs2_file_open,
    release : pvfs2_file_release
#else
    .read = generic_read_dir,
    .readdir = pvfs2_readdir,
    .open = pvfs2_file_open,
    .release = pvfs2_file_release
#endif
};

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
