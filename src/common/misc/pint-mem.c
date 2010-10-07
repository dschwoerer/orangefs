/* 
 * (C) 2001 Clemson University and The University of Chicago 
 *
 * See COPYING in top-level directory.
 */

#define _XOPEN_SOURCE 600
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#ifdef WIN32
#include "wincommon.h"
#endif

/* prototype definitions */
inline void* PINT_mem_aligned_alloc(size_t size, size_t alignment);
inline void PINT_mem_aligned_free(void *ptr);

/* PINT_mem_aligned_alloc()
 *
 * allocates a memory region of the specified size and returns a 
 * pointer to the region.  The address of the memory will be evenly
 * divisible by alignment.
 *
 * returns pointer to memory on success, NULL on failure
 */
inline void* PINT_mem_aligned_alloc(size_t size, size_t alignment)
{
    int ret;
    void *ptr;

#ifdef WIN32    
    ret = 0;
    ptr = _aligned_malloc(size, alignment);
    if (ptr == NULL)
    {
        ret = ENOMEM;
    }
#else
    ret = posix_memalign(&ptr, alignment, size);
#endif
    if(ret != 0)
    {
        errno = ret;
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

/* PINT_mem_aligned_free()
 *
 * frees memory region previously allocated with
 * PINT_mem_aligned_alloc()
 *
 * no return value
 */
inline void PINT_mem_aligned_free(void *ptr)
{
    _aligned_free(ptr);
    return;
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */


