/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

/* PVFS directory cache implementation */

#include <assert.h>
#include "ncache.h"

typedef struct
{
    PVFS_object_ref parent;       /* parent directory */
    char name[PVFS_SEGMENT_MAX];  /* segment name */
    PVFS_object_ref entry;        /* entry in parent */
    struct timeval tstamp_valid;  /* timestamp of validity period */
    int abs_resolved;             /* if this entry is a symlink, is it
                                     the resolved object, or the
                                     symlink object itself? */
} ncache_entry;

struct ncache_t
{
    ncache_entry dentry;
    int prev;
    int next;
    int status;        /*whether the entry is in use or not*/
};

/* Cache Management structure */
typedef struct
{
    struct ncache_t element[PINT_NCACHE_MAX_ENTRIES];
    int count;
    int top;
    int bottom;
    gen_mutex_t *mt_lock;
} ncache;

#define BAD_LINK -1
#define STATUS_UNUSED 0
#define STATUS_USED 1

/* uncomment to enable ncache */
/* #define ENABLE_NCACHE 1 */

#if ENABLE_NCACHE
static void ncache_remove_dentry(int item);
static void ncache_rotate_dentry(int item);
static int ncache_get_lru(void);
static int ncache_update_dentry_timestamp(
    ncache_entry* entry); 
static int ncache_add_dentry(
    char *name,
    int abs_resolved,
    PVFS_object_ref parent,
    PVFS_object_ref entry);

static ncache *cache = NULL;
#endif

static int s_pint_ncache_timeout_ms = PINT_NCACHE_TIMEOUT_MS;

/* compare
 *
 * compares a ncache entry to the search key
 *
 * returns 1 on an equal comparison (match), 0 otherwise
 */
static inline int compare(
    struct ncache_t element,
    char *name,
    int abs_resolved,
    PVFS_object_ref refn)
{
    int ret = 0, len = 0;

    if ((element.dentry.parent.handle == refn.handle) &&
        (element.dentry.parent.fs_id == refn.fs_id) &&
        (element.dentry.abs_resolved == abs_resolved))
    {
        int len1 = strlen(name);
        int len2 = strlen(element.dentry.name);

        if (len1 == len2)
        {
            len = ((len1 < len2) ? len1 : len2);
            ret = (strncmp(name, element.dentry.name, len) == 0);
        }
    }
    return ret;
}

/* check_dentry_expiry
 *
 * need to validate the dentry against the timestamp
 *
 * returns 0 if dentry timestamp is valid, -PVFS_errno if dentry is
 * expired
 */
static inline int check_dentry_expiry(struct timeval time_stamp)
{
    int ret = 0;
    struct timeval cur_time;

    ret = gettimeofday(&cur_time,NULL);
    if (ret == 0)
    {
        /* does timestamp exceed current time?  if so, entry is valid */
        ret = (((time_stamp.tv_sec > cur_time.tv_sec) ||
                ((time_stamp.tv_sec == cur_time.tv_sec) &&
                 (time_stamp.tv_usec > cur_time.tv_usec))) ? 0 : -1);
    }
    return ret;
}

/* ncache_lookup
 *
 * search PVFS directory cache for specific entry
 *
 * returns 0 on success, -PVFS_errno on failure,
 * -PVFS_ENOENT if entry is not present.
 */
int PINT_ncache_lookup(
    char *name,
    int want_resolved,
    PVFS_object_ref parent,
    PVFS_object_ref *entry)
{
#if ENABLE_NCACHE
    int ret = -PVFS_EINVAL, i = 0;

    if (!name || !entry)
    {
        return ret;
    }

    gossip_debug(GOSSIP_NCACHE_DEBUG, "PINT_ncache_lookup called on "
                 "segment %s\n\tunder %Lu,%d [%d]\n", name,
                 Lu(parent.handle), parent.fs_id, want_resolved);

    gen_mutex_lock(cache->mt_lock);
    entry->handle = PINT_NCACHE_HANDLE_INVALID;

    for(i = cache->top; i != BAD_LINK; i = cache->element[i].next)
    {
        if (compare(cache->element[i],name,want_resolved,parent))
        {
            /* match found; check to see if it is still valid */
            ret = check_dentry_expiry(
                cache->element[i].dentry.tstamp_valid);

            if (ret < 0)
            {
                /* entry is stale, remove from cache */
                gossip_debug(GOSSIP_NCACHE_DEBUG,
                             "ncache entry expired.\n");
                ncache_remove_dentry(i);

                /* we never have more than one entry for the same
                 * object in the cache, so we can assume we have no
                 * up-to-date one at this point.
                 */
                gen_mutex_unlock(cache->mt_lock);
                return -PVFS_ENOENT;
            }
            gossip_debug(GOSSIP_NCACHE_DEBUG, "ncache entry valid.\n");

            /*
              update links so that this dentry is at the top of our
              list; update the time stamp on the ncache entry
            */
            ncache_rotate_dentry(i);
            ret = ncache_update_dentry_timestamp(
                &cache->element[i].dentry);
            if (ret < 0)
            {
                gen_mutex_unlock(cache->mt_lock);
                return -PVFS_ENOENT;
            }

            entry->handle = cache->element[i].dentry.entry.handle;
            entry->fs_id = cache->element[i].dentry.entry.fs_id;        
            gen_mutex_unlock(cache->mt_lock);
            return 0;
        }
    }

    /* passed through entire cache with no matches */
    gen_mutex_unlock(cache->mt_lock);
    return -PVFS_ENOENT;
#else
    return -PVFS_ENOENT;
#endif
}

/* ncache_rotate_dentry()
 *
 * moves the specified item to the top of the ncache linked list to prevent it
 * from being identified as the least recently used item in the cache.
 *
 * no return value
 */
#if ENABLE_NCACHE
static void ncache_rotate_dentry(int item)
{
    int prev = 0, next = 0, new_bottom;

    if (cache->top != cache->bottom) 
    {
        if (cache->top != item)
        {
            /*
              only move links if there's more than one thing in the
              list
            */
            if (cache->bottom == item)
            {
                new_bottom = cache->element[cache->bottom].prev;

                cache->element[new_bottom].next = BAD_LINK;
                cache->bottom = new_bottom;
            }
            else
            {
                /*somewhere in the middle*/
                next = cache->element[item].next;
                prev = cache->element[item].prev;

                cache->element[prev].next = next;
                cache->element[next].prev = prev;
            }

            cache->element[cache->top].prev = item;

            cache->element[item].next = cache->top;
            cache->element[item].prev = BAD_LINK;
            cache->top = item;
        }
    }
}
#endif

/* ncache_insert
 *
 * insert an entry into PVFS directory cache
 *
 * returns 0 on success, -1 on failure
 */
int PINT_ncache_insert(
    char *name,
    int abs_resolved,
    PVFS_object_ref entry,
    PVFS_object_ref parent)
{
#if ENABLE_NCACHE
    int i = 0, index = 0, ret = 0;
    unsigned char entry_found = 0;
        
    gen_mutex_lock(cache->mt_lock);

    gossip_debug(
        GOSSIP_NCACHE_DEBUG, "PINT_ncache_insert: inserting segment "
        "%s\n\t(%Lu,%d) under parent (%Lu,%d) [%d]\n", name,
        Lu(entry.handle), entry.fs_id, Lu(parent.handle),
        parent.fs_id, abs_resolved);

    for (i = cache->top; i != BAD_LINK; i = cache->element[i].next)
    {
        if (compare(cache->element[i],name,abs_resolved,parent))
        {
            entry_found = 1;
            index = i;
            break;
        }
    }
        
    /* add/merge element to the cache */
    if (entry_found == 0)
    {
        /* Element not in cache, add it */
        ncache_add_dentry(name,abs_resolved,parent,entry);
    }
    else
    {
        /* move entry to the top of the list and update its timestamp */
        gossip_debug(GOSSIP_NCACHE_DEBUG, "ncache: inserting entry "
                     "already present; timestamp update.\n");

        ncache_rotate_dentry(index);
        ret = ncache_update_dentry_timestamp(
            &cache->element[index].dentry); 
        if (ret < 0)
        {
            gen_mutex_unlock(cache->mt_lock);
            return(ret);
        }
    }
    gen_mutex_unlock(cache->mt_lock);
#endif
    return 0;
}

/* ncache_remove
 *
 * remove a particular entry from the PVFS directory cache
 *
 * returns 0 on success, -1 on failure
 */
int PINT_ncache_remove(
    char *name,
    int abs_resolved,
    PVFS_object_ref parent,
    int *item_found)
{
#if ENABLE_NCACHE
    int i = 0;

    if (!name)
    {
        return -PVFS_EINVAL;
    }

    if (item_found)
    {
        *item_found = 0;
    }

    gen_mutex_lock(cache->mt_lock);
    for(i = cache->top; i != BAD_LINK; i = cache->element[i].next)
    {
        if (compare(cache->element[i],name,abs_resolved,parent))
        {
            ncache_remove_dentry(i);

            if (item_found)
            {
                *item_found = 1;
            }
            break;
        }
    }
    gen_mutex_unlock(cache->mt_lock);
#endif
    return 0;
}

/* ncache_flush
 * 
 * remove all entries from the PVFS directory cache
 *
 * returns 0 on success, -1 on failure
 */
int PINT_ncache_flush(void)
{
    return -PVFS_ENOSYS;
}

/* pint_ncache_initialize
 *
 * initialize the PVFS directory cache
 *
 * returns 0 on success, -1 on failure
 */
int PINT_ncache_initialize(void)
{
#if ENABLE_NCACHE
    int ret = 0, i = 0;

    if (cache == NULL)
    {
        cache = (ncache*)malloc(sizeof(ncache));
        if (cache)
        {
            cache->mt_lock = gen_mutex_build();
            cache->top = BAD_LINK;
            cache->bottom = 0;
            cache->count = 0;

            for(i = 0;i < PINT_NCACHE_MAX_ENTRIES; i++)
            {
                cache->element[i].prev = BAD_LINK;
                cache->element[i].next = BAD_LINK;
                cache->element[i].status = STATUS_UNUSED;
            }
        }
        ret = (cache ? 0 : -PVFS_ENOMEM);
    }

    gossip_debug(GOSSIP_NCACHE_DEBUG,
                 "PINT_ncache_initialize returning %d\n", ret);
    return ret;
#else
    return 0;
#endif
}

/* pint_ncache_finalize
 *
 * close down the PVFS directory cache framework
 *
 * returns 0
 */
int PINT_ncache_finalize(void)
{
#if ENABLE_NCACHE

    if (cache)
    {
        gen_mutex_destroy(cache->mt_lock);
        free(cache);
        cache = NULL;
    }

    gossip_debug(GOSSIP_NCACHE_DEBUG, "PINT_ncache_finalize complete\n");
#endif
    return 0;
}

int PINT_ncache_get_timeout(void)
{
    return s_pint_ncache_timeout_ms;
}

void PINT_ncache_set_timeout(int max_timeout_ms)
{
    s_pint_ncache_timeout_ms = max_timeout_ms;
}


/* ncache_add_dentry
 *
 * add a dentry to the ncache
 *
 * returns 0 on success, -PVFS_errno on failure
 */
#if ENABLE_NCACHE
static int ncache_add_dentry(
    char *name,
    int abs_resolved,
    PVFS_object_ref parent,
    PVFS_object_ref entry)
{
    int new = 0, ret = 0;
    int size = strlen(name) + 1; /* size includes null terminator*/

    new = ncache_get_lru();

    /* add element to cache */
    cache->element[new].status = STATUS_USED;
    cache->element[new].dentry.parent = parent;
    cache->element[new].dentry.entry = entry;
    cache->element[new].dentry.abs_resolved = abs_resolved;
    memcpy(cache->element[new].dentry.name,name,size);

    /* set timestamp */
    ret = ncache_update_dentry_timestamp(
        &cache->element[new].dentry);

    if (ret < 0)
    {
        return ret;
    }
    cache->element[new].prev = BAD_LINK;
    cache->element[new].next = cache->top;

    /* make previous element point to new entry */
    if (cache->top != BAD_LINK)
    {
        cache->element[cache->top].prev = new;
    }
    cache->top = new;
    return 0;
}

/* ncache_get_lru
 *
 * this function gets the least recently used cache entry (assuming a
 * full cache) or searches through the cache for the first unused slot
 * (if there are some free slots)
 *
 * returns 0 on success, -PVFS_errno on failure
 */
static int ncache_get_lru(void)
{
    int new = 0, i = 0;

    if (cache->count == PINT_NCACHE_MAX_ENTRIES)
    {
        new = cache->bottom;
        cache->bottom = cache->element[new].prev;
        cache->element[cache->bottom].next = BAD_LINK;
        return new;
    }
    else
    {
        for(i = 0; i < PINT_NCACHE_MAX_ENTRIES; i++)
        {
            if (cache->element[i].status == STATUS_UNUSED)
            {
                cache->count++;
                return i;
            }
        }
    }

    gossip_debug(GOSSIP_NCACHE_DEBUG,
                  "error getting least recently used dentry.\n");
    gossip_debug(GOSSIP_NCACHE_DEBUG,
                  "cache->count = %d max_entries = %d.\n",
                  cache->count, PINT_NCACHE_MAX_ENTRIES);

    return -PVFS_ENOENT;
}

/* ncache_remove_dentry
 *
 * Handles the actual manipulation of the cache to handle removal
 *
 * returns nothing
 */
static void ncache_remove_dentry(int item)
{
    int prev = 0,next = 0;

    cache->element[item].status = STATUS_UNUSED;
    memset(&cache->element[item].dentry.name, 0, PVFS_SEGMENT_MAX);
    cache->count--;

    /* if there's exactly one item in the list, just get rid of it*/
    if (cache->top == cache->bottom)
    {
        cache->top = 0;
        cache->bottom = 0;
        cache->element[item].prev = BAD_LINK;
        cache->element[item].next = BAD_LINK;
        return;
    }

    /* depending on where the dentry is in the list, we have to do
     * different things if its the first, last, or somewhere in the
     * middle.
     */
    if (item == cache->top)
    {
        /* Adjust top */
        cache->top = cache->element[item].next;
        cache->element[cache->top].prev = BAD_LINK;
    }
    else if (item == cache->bottom)
    {
        /* Adjust bottom */
        cache->bottom = cache->element[item].prev;
        cache->element[cache->bottom].next = -1;
    }
    else
    {
        /* Item in the middle */
        prev = cache->element[item].prev;
        next = cache->element[item].next;
        cache->element[prev].next = next;
        cache->element[next].prev = prev;
    }
}

/* ncache_update_dentry_timestamp
 *
 * updates the timestamp of the ncache entry
 *
 * returns 0 on success, -PVFS_errno on failure
 */
static int ncache_update_dentry_timestamp(ncache_entry* entry) 
{
    int ret = 0;

    ret = gettimeofday(&entry->tstamp_valid,NULL);
    if (ret == 0)
    {
        entry->tstamp_valid.tv_sec +=
            (int)(s_pint_ncache_timeout_ms / 1000);
        entry->tstamp_valid.tv_usec +=
            (int)((s_pint_ncache_timeout_ms % 1000) * 1000);
    }
    return ret;
}
#endif /* ENABLE_NCACHE */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
