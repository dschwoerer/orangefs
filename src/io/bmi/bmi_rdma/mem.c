/*
 * RDMA BMI method, memory allocation and caching.
 *
 * Copyright (C) 2004-6 Pete Wyckoff <pw@osc.edu>
 * Copyright (C) 2016 David Reynolds <david@omnibond.com>
 *
 * See COPYING in top-level directory.
 */
#include <src/common/gen-locks/gen-locks.h>
#include "pvfs2-internal.h"
#include "rdma.h"

/*
 * This internal state structure is allocated when the init function
 * is called.  The device hangs onto it and gives it back to us as
 * needed.
 *
 * TODO: Use an rbtree here instead.  Also deregister refcnt==0 regions
 * when new ones come along that overlap, much like dreg, as an indication
 * that application buffers have changed.
 */
typedef struct
{
    struct qlist_head list;
    gen_mutex_t mutex;
    struct qlist_head free_chunk_list;
    int (*mem_register)(memcache_entry_t *c);
    void (*mem_deregister)(memcache_entry_t *c);
} memcache_device_t;

/*
 * Create and link a new memcache entry.  Assumes lock already held.
 * Initializes count to 1.
 */
static memcache_entry_t *memcache_add(memcache_device_t *memcache_device,
                                      void *buf,
                                      bmi_size_t len)
{
    memcache_entry_t *c;

    c = malloc(sizeof(*c));
    if (bmi_rdma_likely(c))
    {
        c->buf = buf;
        c->len = len;
        c->count = 1;
        qlist_add_tail(&c->list, &memcache_device->list);
    }

    return c;
}

/*
 * Just undo the creation of the entry, in cases where memory registration
 * fails, for instance.
 */
static void memcache_del(memcache_device_t *memcache_device __unused,
                         memcache_entry_t *c)
{
    qlist_del(&c->list);
    free(c);
}

/*
 * See if an entry exists that totally covers the request.  Assumes lock
 * already held.  These criteria apply:
 *   1. existing bounds must cover potential new one
 *   2. prefer highest refcnt (hopin for maximal reuse)
 *   3. prefer tightest bounds amoung matching refcnt
 */
static memcache_entry_t *memcache_lookup_cover(
                                        memcache_device_t *memcache_device,
                                        const void *const buf,
                                        bmi_size_t len)
{
    struct qlist_head *l;
    const char *end = (const char *) buf + len;
    memcache_entry_t *cbest = 0;

    qlist_for_each(l, &memcache_device->list)
    {
        memcache_entry_t *c = qlist_entry(l, memcache_entry_t, list);

        if (!(c->buf <= buf && end <= (const char *) c->buf + c->len))
        {
            continue;
        }

        if (!cbest)
        {
            goto take;
        }

        if (c->count < cbest->count)
        {
            continue;   /* discard lower refcnt one */
        }
        if (c->count > cbest->count)
        {
            goto take;  /* prefer higher refcnt */
        }
        /* equal refcnt, prefer tighter bounds */
        if (c->len < cbest->len)
        {
            goto take;
        }

        continue;

take:
        cbest = c;
    }

    return cbest;
}

/*
 * See if the exact entry exists.  There must never be more than one
 * of the same entry.  Used only for BMI_rdma_memfree.
 */
static memcache_entry_t *memcache_lookup_exact(
                                        memcache_device_t *memcache_device,
                                        const void *const buf,
                                        bmi_size_t len)
{
    struct qlist_head *l;

    qlist_for_each(l, &memcache_device->list)
    {
        memcache_entry_t *c = qlist_entry(l, memcache_entry_t, list);
        if (c->buf == buf && c->len == len)
        {
            return c;
        }
    }

    return 0;
}

/*
 * BMI malloc and free routines.  If the region is big enough, pin
 * it now to save time later in the actual send or recv routine.
 * These are only ever called from PVFS internal functions to allocate
 * buffers, on the server, or on the client for non-user-supplied
 * buffers.
 *
 * Standard sizes will appear frequently, thus do not free them.  Use
 * a separate list sorted by sizes than can be used to reuse one.
 */
void *memcache_memalloc(void *md,
                        bmi_size_t len,
                        int eager_limit)
{
    memcache_device_t *memcache_device = md;
    void *buf;

    debug(4, "%s: len %lld limit %d", __func__, lld(len), eager_limit);

    /* search in size cache first */
    if (len > eager_limit)
    {
        memcache_entry_t *c;
        gen_mutex_lock(&memcache_device->mutex);
        qlist_for_each_entry(c, &memcache_device->free_chunk_list, list)
        {
            if (c->len == len)
            {
                debug(4, "%s: recycle free chunk, buf %p", __func__, c->buf);
                qlist_del(&c->list);
                qlist_add_tail(&c->list, &memcache_device->list);
                ++c->count;
                if (c->count == 1)
                {
                    memcache_device->mem_register(c);
                }
                buf = c->buf;
                gen_mutex_unlock(&memcache_device->mutex);
                goto out;
            }
        }
        gen_mutex_unlock(&memcache_device->mutex);
    }

    buf = malloc(len);

    if (bmi_rdma_unlikely(!buf))
    {
        goto out;
    }

    if (len > eager_limit)
    {
        memcache_entry_t *c;

        gen_mutex_lock(&memcache_device->mutex);
        /* could be recycled buffer */
        c = memcache_lookup_cover(memcache_device, buf, len);
        if (c)
        {
            ++c->count;
            if (c->count == 1)
            {
                memcache_device->mem_register(c);
            }
            debug(4, "%s: reuse reg, buf %p, count %d",
                  __func__, c->buf, c->count);
        }
        else
        {
            c = memcache_add(memcache_device, buf, len);
            if (bmi_rdma_unlikely(!c))
            {
                free(buf);
                buf = NULL;
            }
            else
            {
                int ret = memcache_device->mem_register(c);
                if (ret)
                {
                    memcache_del(memcache_device, c);
                    free(buf);
                    buf = NULL;
                }
                debug(4, "%s: new reg, buf %p", __func__, c->buf);
            }
        }

        gen_mutex_unlock(&memcache_device->mutex);
    }

out:
    return buf;
}

int memcache_memfree(void *md,
                  void *buf,
                  bmi_size_t len)
{
    memcache_device_t *memcache_device = md;
    memcache_entry_t *c;

    gen_mutex_lock(&memcache_device->mutex);
    /* okay if not found, just not cached; perhaps an eager-size buffer */
    c = memcache_lookup_exact(memcache_device, buf, len);
    if (c)
    {
        debug(4, "%s: cache free buf %p len %lld",
              __func__, c->buf, lld(c->len));

        if (c->count != 1)
        {
            error("%s: buf %p len %lld count %d, expected 1",
                  __func__, c->buf, lld(c->len), c->count);
            gen_mutex_unlock(&memcache_device->mutex);
            return EINVAL;
        }

        /* cache it */
        --c->count;
        if (c->count == 0)
        {
            memcache_device->mem_deregister(c);
        }

        qlist_del(&c->list);
        qlist_add(&c->list, &memcache_device->free_chunk_list);
        gen_mutex_unlock(&memcache_device->mutex);

        return 0;
    }

    gen_mutex_unlock(&memcache_device->mutex);

    free(buf);
    return 0;
}

/*
 * Interface for bmi_rdma send and recv routines in rdma.c.  Takes a buflist
 * and looks up each entry in the memcache, adding it if not yet pinned.
 */
void memcache_register(void *md,
                       rdma_buflist_t *buflist)
{
    int i, ret;
    memcache_device_t *memcache_device = md;

    buflist->memcache = bmi_rdma_malloc(buflist->num *
                                        sizeof(*buflist->memcache));
    if (!buflist->memcache)
    {
        error("%s: bmi_rdma_malloc failed for %lld bytes",
              __func__, buflist->num * sizeof(*buflist->memcache));
        return;
    }

    gen_mutex_lock(&memcache_device->mutex);

    for (i = 0; i < buflist->num; i++)
    {
        memcache_entry_t *c;
        c = memcache_lookup_cover(memcache_device,
                                  buflist->buf.send[i],
                                  buflist->len[i]);
        if (c)
        {
            ++c->count;
            if (c->count == 1)
            {
                memcache_device->mem_register(c);
            }

            debug(2, "%s: hit [%d] %p len %lld (via %p len %lld) refcnt now %d",
                  __func__,
                  i,
                  buflist->buf.send[i],
                  lld(buflist->len[i]),
                  c->buf,
                  lld(c->len),
                  c->count);
        }
        else
        {
            debug(2, "%s: miss [%d] %p len %lld",
                  __func__,
                  i,
                  buflist->buf.send[i],
                  lld(buflist->len[i]));

            c = memcache_add(memcache_device,
                             buflist->buf.recv[i],
                             buflist->len[i]);
            /* XXX: replace error with return values, let caller deal */
            if (!c)
            {
                error("%s: no memory for cache entry", __func__);
                goto out;
            }

            ret = memcache_device->mem_register(c);
            if (ret)
            {
                memcache_del(memcache_device, c);
                error("%s: could not register memory", __func__);
                goto out;
            }
        }

        buflist->memcache[i] = c;
    }

out:
    gen_mutex_unlock(&memcache_device->mutex);
}

/*
 * Similar to the normal register call, but does not use a buflist,
 * just adds an entry to the cache for use by later registration.
 * Also does not add a refcnt on any entry.
 */
void memcache_preregister(void *md __unused,
                          const void *buf __unused,
                          bmi_size_t len __unused,
                          enum PVFS_io_type rw __unused)
{
    /* TODO: this doesn't work anymore, remove it? */
    return;
}

void memcache_deregister(void *md,
                         rdma_buflist_t *buflist)
{
    int i;
    memcache_device_t *memcache_device = md;

    gen_mutex_lock(&memcache_device->mutex);
    for (i = 0; i < buflist->num; i++)
    {
        memcache_entry_t *c = buflist->memcache[i];
        --c->count;
        if (c->count == 0)
        {
            memcache_device->mem_deregister(c);
        }
        debug(2,
              "%s: dec refcount [%d] %p len %lld "
              "(via %p len %lld) refcnt now %d",
              __func__,
              i,
              buflist->buf.send[i],
              lld(buflist->len[i]),
              c->buf,
              lld(c->len),
              c->count);
        /* let garbage collection do rdma_mem_deregister(c) for refcnt == 0 */
    }

    free(buflist->memcache);
    gen_mutex_unlock(&memcache_device->mutex);
}

/*
 * Initialize.
 */
void *memcache_init(int (*mem_register)(memcache_entry_t *),
                    void (*mem_deregister)(memcache_entry_t *))
{
    memcache_device_t *memcache_device;

    memcache_device = bmi_rdma_malloc(sizeof(*memcache_device));
    INIT_QLIST_HEAD(&memcache_device->list);
    gen_mutex_init(&memcache_device->mutex);
    INIT_QLIST_HEAD(&memcache_device->free_chunk_list);
    memcache_device->mem_register = mem_register;
    memcache_device->mem_deregister = mem_deregister;
    return memcache_device;
}

/*
 * Remove all mappings in preparation for closing the pd.
 */
void memcache_shutdown(void *md)
{
    memcache_device_t *memcache_device = md;
    memcache_entry_t *c, *cn;

    gen_mutex_lock(&memcache_device->mutex);

    qlist_for_each_entry_safe(c, cn, &memcache_device->list, list)
    {
        if (c->count > 0)
        {
            memcache_device->mem_deregister(c);
        }
        qlist_del(&c->list);
        free(c);
    }

    qlist_for_each_entry_safe(c, cn, &memcache_device->free_chunk_list, list)
    {
        if (c->count > 0)
        {
            memcache_device->mem_deregister(c);
        }
        qlist_del(&c->list);
        free(c->buf);
        free(c);
    }

    gen_mutex_unlock(&memcache_device->mutex);
    free(memcache_device);
}

/*
 * Used to flush the cache when a NIC returns -ENOMEM on mem_reg.  Must
 * hold the device lock on entry here.
 */
void memcache_cache_flush(void *md)
{
    memcache_device_t *memcache_device = md;
    memcache_entry_t *c, *cn;

    debug(4, "%s", __func__);
    qlist_for_each_entry_safe(c, cn, &memcache_device->list, list);
    {
        debug(4, "%s: list c->count %x c->buf %p",
              __func__, c->count, c->buf);
        if (c->count == 0)
        {
            qlist_del(&c->list);
            free(c);
        }
    }
    qlist_for_each_entry_safe(c, cn, &memcache_device->free_chunk_list, list)
    {
        debug(4, "%s: free list c->count %x c->buf %p",
              __func__, c->count, c->buf);
        if (c->count == 0)
        {
            qlist_del(&c->list);
            free(c->buf);
            free(c);
        }
    }
}

/* Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
