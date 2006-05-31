/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include "pvfs2-kernel.h"


static uint64_t next_tag_value;
static spinlock_t next_tag_value_lock = SPIN_LOCK_UNLOCKED;

/* the pvfs2 memory caches */

/* a cache for pvfs2 upcall/downcall operations */
static kmem_cache_t *op_cache = NULL;
/* a cache for device (/dev/pvfs2-req) communication */
static kmem_cache_t *dev_req_cache = NULL;
/* a cache for pvfs2-inode objects (i.e. pvfs2 inode private data) */
static kmem_cache_t *pvfs2_inode_cache = NULL;
#ifdef HAVE_AIO_VFS_SUPPORT
/* a cache for pvfs2_kiocb objects (i.e pvfs2 iocb structures ) */
static kmem_cache_t *pvfs2_kiocb_cache = NULL;
#endif

extern int debug;
extern int pvfs2_gen_credentials(
    PVFS_credentials *credentials);

int op_cache_initialize(void)
{
    op_cache = kmem_cache_create(
        "pvfs2_op_cache", sizeof(pvfs2_kernel_op_t),
        0, PVFS2_CACHE_CREATE_FLAGS, NULL, NULL);

    if (!op_cache)
    {
        pvfs2_panic("Cannot create pvfs2_op_cache\n");
        return -ENOMEM;
    }

    /* initialize our atomic tag counter */
    spin_lock(&next_tag_value_lock);
    next_tag_value = 100;
    spin_unlock(&next_tag_value_lock);
    return 0;
}

int op_cache_finalize(void)
{
    if (kmem_cache_destroy(op_cache) != 0)
    {
        pvfs2_panic("Failed to destroy pvfs2_op_cache\n");
        return -EINVAL;
    }
    return 0;
}

static pvfs2_kernel_op_t *op_alloc_common(int op_linger)
{
    pvfs2_kernel_op_t *new_op = NULL;

    new_op = kmem_cache_alloc(op_cache, PVFS2_CACHE_ALLOC_FLAGS);
    if (new_op)
    {
        memset(new_op, 0, sizeof(pvfs2_kernel_op_t));

        INIT_LIST_HEAD(&new_op->list);
        spin_lock_init(&new_op->lock);
        init_waitqueue_head(&new_op->waitq);

        init_waitqueue_head(&new_op->io_completion_waitq);
        atomic_set(&new_op->aio_ref_count, 0);

        pvfs2_op_initialize(new_op);

        /* initialize the op specific tag and upcall credentials */
        spin_lock(&next_tag_value_lock);
        new_op->tag = next_tag_value++;
        if (next_tag_value == 0)
        {
            next_tag_value = 100;
        }
        spin_unlock(&next_tag_value_lock);

        pvfs2_gen_credentials(&new_op->upcall.credentials);
        new_op->op_linger = new_op->op_linger_tmp = op_linger;
    }
    else
    {
        pvfs2_panic("op_alloc_common: kmem_cache_alloc failed!\n");
    }
    return new_op;
}

pvfs2_kernel_op_t *op_alloc()
{
    return op_alloc_common(1);
}

pvfs2_kernel_op_t *op_alloc_trailer()
{
    return op_alloc_common(2);
}

void op_release(pvfs2_kernel_op_t *pvfs2_op)
{
    if (pvfs2_op)
    {
        pvfs2_op_initialize(pvfs2_op);
        kmem_cache_free(op_cache, pvfs2_op);
    }
    else 
    {
        pvfs2_panic("NULL pointer in op_release\n");
    }
}

static void dev_req_cache_ctor(
    void *req,
    kmem_cache_t * cachep,
    unsigned long flags)
{
    if (flags & SLAB_CTOR_CONSTRUCTOR)
    {
        memset(req, 0, sizeof(MAX_ALIGNED_DEV_REQ_DOWNSIZE));
    }
    else
    {
        pvfs2_panic("WARNING!! devreq_ctor called without ctor flag\n");
    }
}

int dev_req_cache_initialize(void)
{
    dev_req_cache = kmem_cache_create(
        "pvfs2_devreqcache", MAX_ALIGNED_DEV_REQ_DOWNSIZE, 0,
        PVFS2_CACHE_CREATE_FLAGS, dev_req_cache_ctor, NULL);

    if (!dev_req_cache)
    {
        pvfs2_panic("Cannot create pvfs2_dev_req_cache\n");
        return -ENOMEM;
    }
    return 0;
}

int dev_req_cache_finalize(void)
{
    if (kmem_cache_destroy(dev_req_cache) != 0)
    {
        pvfs2_panic("Failed to destroy pvfs2_devreqcache\n");
        return -EINVAL;
    }
    return 0;
}

void *dev_req_alloc(void)
{
    void *buffer;

    buffer = kmem_cache_alloc(dev_req_cache, PVFS2_CACHE_ALLOC_FLAGS);
    if (buffer == NULL)
    {
        pvfs2_panic("Failed to allocate from dev_req_cache\n"); 
    }
    return buffer;
}

void dev_req_release(void *buffer)
{
    if (buffer)
    {
        kmem_cache_free(dev_req_cache, buffer);
    }
    else 
    {
        pvfs2_panic("NULL pointer passed to dev_req_release\n");
    }
    return;
}

static void pvfs2_inode_cache_ctor(
    void *new_pvfs2_inode,
    kmem_cache_t * cachep,
    unsigned long flags)
{
    pvfs2_inode_t *pvfs2_inode = (pvfs2_inode_t *)new_pvfs2_inode;

    if (flags & SLAB_CTOR_CONSTRUCTOR)
    {
        memset(pvfs2_inode, 0, sizeof(pvfs2_inode_t));

        pvfs2_inode_initialize(pvfs2_inode);

#ifndef PVFS2_LINUX_KERNEL_2_4
        /*
           inode_init_once is from 2.6.x's inode.c; it's normally run
           when an inode is allocated by the system's inode slab
           allocator.  we call it here since we're overloading the
           system's inode allocation with this routine, thus we have
           to init vfs inodes manually
        */
        inode_init_once(&pvfs2_inode->vfs_inode);
        pvfs2_inode->vfs_inode.i_version = 1;
#endif
        /* Initialize the reader/writer semaphore */
        init_rwsem(&pvfs2_inode->xattr_sem);
    }
    else
    {
        pvfs2_panic("WARNING!! inode_ctor called without ctor flag\n");
    }
}

static void pvfs2_inode_cache_dtor(
    void *old_pvfs2_inode,
    kmem_cache_t * cachep,
    unsigned long flags)
{
    pvfs2_inode_t *pvfs2_inode = (pvfs2_inode_t *)old_pvfs2_inode;

    if (pvfs2_inode && pvfs2_inode->link_target)
    {
        kfree(pvfs2_inode->link_target);
        pvfs2_inode->link_target = NULL;
    }
}

int pvfs2_inode_cache_initialize(void)
{
    pvfs2_inode_cache = kmem_cache_create(
        "pvfs2_inode_cache", sizeof(pvfs2_inode_t), 0,
        PVFS2_CACHE_CREATE_FLAGS, pvfs2_inode_cache_ctor,
        pvfs2_inode_cache_dtor);

    if (!pvfs2_inode_cache)
    {
        pvfs2_panic("Cannot create pvfs2_inode_cache\n");
        return -ENOMEM;
    }
    return 0;
}

int pvfs2_inode_cache_finalize(void)
{
    if (kmem_cache_destroy(pvfs2_inode_cache) != 0)
    {
        pvfs2_panic("Failed to destroy pvfs2_inode_cache\n");
        return -EINVAL;
    }
    return 0;
}

pvfs2_inode_t* pvfs2_inode_alloc(void)
{
    pvfs2_inode_t *pvfs2_inode = NULL;
    /*
        this allocator has an associated constructor that fills in the
        internal vfs inode structure.  this initialization is extremely
        important and is required since we're allocating the inodes
        ourselves (rather than letting the system inode allocator
        initialize them for us); see inode.c/inode_init_once()
    */
    pvfs2_inode = kmem_cache_alloc(pvfs2_inode_cache,
                                   PVFS2_CACHE_ALLOC_FLAGS);
    if (pvfs2_inode == NULL) 
    {
        pvfs2_panic("Failed to allocate pvfs2_inode\n");
    }
    return pvfs2_inode;
}

void pvfs2_inode_release(pvfs2_inode_t *pinode)
{
    if (pinode)
    {
        kmem_cache_free(pvfs2_inode_cache, pinode);
    }
    else
    {
        pvfs2_panic("NULL pointer in pvfs2_inode_release\n");
    }
}

#ifdef HAVE_AIO_VFS_SUPPORT

static void kiocb_ctor(
    void *req,
    kmem_cache_t * cachep,
    unsigned long flags)
{
    if (flags & SLAB_CTOR_CONSTRUCTOR)
    {
        memset(req, 0, sizeof(pvfs2_kiocb));
    }
    else
    {
        pvfs2_panic("WARNING!! kiocb_ctor called without ctor flag\n");
    }
}


int kiocb_cache_initialize(void)
{
    pvfs2_kiocb_cache = kmem_cache_create(
        "pvfs2_kiocbcache", sizeof(pvfs2_kiocb), 0,
        PVFS2_CACHE_CREATE_FLAGS, kiocb_ctor, NULL);

    if (!pvfs2_kiocb_cache)
    {
        pvfs2_panic("Cannot create pvfs2_kiocb_cache!\n");
        return -ENOMEM;
    }
    return 0;
}

int kiocb_cache_finalize(void)
{
    if (kmem_cache_destroy(pvfs2_kiocb_cache) != 0)
    {
        pvfs2_panic("Failed to destroy pvfs2_devreqcache\n");
        return -EINVAL;
    }
    return 0;
}

pvfs2_kiocb* kiocb_alloc(void)
{
    pvfs2_kiocb *x = NULL;

    x = kmem_cache_alloc(pvfs2_kiocb_cache, PVFS2_CACHE_ALLOC_FLAGS);
    if (x == NULL)
    {
        pvfs2_panic("kiocb_alloc: kmem_cache_alloc failed!\n");
    }
    return x;
}

void kiocb_release(pvfs2_kiocb *x)
{
    if (x)
    {
        kmem_cache_free(pvfs2_kiocb_cache, x);
    }
    else 
    {
        pvfs2_panic("kiocb_release: kmem_cache_free NULL pointer!\n");
    }
}

#endif

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
