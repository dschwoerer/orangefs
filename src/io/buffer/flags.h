/*
 * Macros for manipulating and testing extent->flags
 */

#ifndef _FLAGS_H
#define _FLAGS_H

#include <asm/asm-i386/bitops.h>

static inline int my_test_bit(int nr, unsigned long * addr)
{
        int oldbit;

	oldbit = ((unsigned long)*addr) & ( 1UL << nr );

        return oldbit;
}

#define PG_locked	 	 0	/* Page is locked. Don't touch. */
#define PG_error		 1
#define PG_clean		 2	/* Clean page */
#define PG_dirty	 	 3 

#define PG_uptodate		 4 
#define PG_lru			 5
#define PG_active		 6
#define PG_rmw			 7   /* read is on this extent for rmw */	

#define PG_readcomm		8	/* Read communication */
#define PG_readpending		9	/* Read op pending */
#define PG_writecomm		10	/* Write communication */
#define PG_writepending		11	/* Write op pending */

#define PG_referenced		12 
#define PG_blank		13	/* Blank page */




/*
 * Manipulation of state flags
 */
#define PageLocked(page)		\
		my_test_bit(PG_locked, &(page)->flags)
#define SetPageLocked(page)		\
		set_bit(PG_locked, &(page)->flags)
#define TestSetPageLocked(page)		\
		test_and_set_bit(PG_locked, &(page)->flags)
#define ClearPageLocked(page)		\
		clear_bit(PG_locked, &(page)->flags)
#define TestClearPageLocked(page)	\
		test_and_clear_bit(PG_locked, &(page)->flags)

#define PageError(page)		my_test_bit(PG_error, &(page)->flags)
#define SetPageError(page)	set_bit(PG_error, &(page)->flags)
#define ClearPageError(page)	clear_bit(PG_error, &(page)->flags)

#define PageDirty(page)		my_test_bit(PG_dirty, &(page)->flags)
#define SetPageDirty(page)	set_bit(PG_dirty, &(page)->flags)
#define TestSetPageDirty(page)	test_and_set_bit(PG_dirty, &(page)->flags)
#define ClearPageDirty(page)	clear_bit(PG_dirty, &(page)->flags)
#define TestClearPageDirty(page) test_and_clear_bit(PG_dirty, &(page)->flags)

#define SetPageLRU(page)	set_bit(PG_lru, &(page)->flags)
#define PageLRU(page)		my_test_bit(PG_lru, &(page)->flags)
#define TestSetPageLRU(page)	test_and_set_bit(PG_lru, &(page)->flags)
#define TestClearPageLRU(page)	test_and_clear_bit(PG_lru, &(page)->flags)

#define PageActive(page)	my_test_bit(PG_active, &(page)->flags)
#define SetPageActive(page)	set_bit(PG_active, &(page)->flags)
#define ClearPageActive(page)	clear_bit(PG_active, &(page)->flags)
#define TestClearPageActive(page) test_and_clear_bit(PG_active, &(page)->flags)
#define TestSetPageActive(page) test_and_set_bit(PG_active, &(page)->flags)

#define PageReferenced(page)    my_test_bit(PG_referenced, &(page)->flags)
#define SetPageReferenced(page) set_bit(PG_referenced, &(page)->flags)
#define ClearPageReferenced(page)   clear_bit(PG_referenced, &(page)->flags)
#define TestClearPageReferenced(page) test_and_clear_bit(PG_referenced, &(page)->flags)

/* new stuff */
#define PageRcomm(page)		my_test_bit(PG_readcomm, &(page)->flags)
#define SetPageRcomm(page)	set_bit(PG_readcomm, &(page)->flags)
#define ClearPageRcomm(page)	clear_bit(PG_readcomm, &(page)->flags)

#define PageWcomm(page)		my_test_bit(PG_writecomm, &(page)->flags)
#define SetPageWcomm(page)	set_bit(PG_writecomm, &(page)->flags)
#define ClearPageWcomm(page)	clear_bit(PG_writecomm, &(page)->flags)

#define PageReadPending(page)	 my_test_bit(PG_readpending, &(page)->flags)
#define SetPageReadPending(page) set_bit(PG_readpending, &(page)->flags)
#define ClearPageReadPending(page) clear_bit(PG_readpending, &(page)->flags)

#define PageWritePending(page)	 my_test_bit(PG_writepending, &(page)->flags)
#define SetPageWritePending(page) set_bit(PG_writepending, &(page)->flags)
#define ClearPageWritePending(page) clear_bit(PG_writepending, &(page)->flags)

#define PageClean(page)	 my_test_bit(PG_clean, &(page)->flags)
#define SetPageClean(page) set_bit(PG_clean, &(page)->flags)
#define ClearPageClean(page) clear_bit(PG_clean, &(page)->flags)

#define PageBlank(page)	 my_test_bit(PG_blank, &(page)->flags)
#define SetPageBlank(page) set_bit(PG_blank, &(page)->flags)
#define ClearPageBlank(page) clear_bit(PG_blank, &(page)->flags)

#define PageRMW(page)	 my_test_bit(PG_rmw, &(page)->flags)
#define SetPageRMW(page) set_bit(PG_rmw, &(page)->flags)
#define ClearPageRMW(page) clear_bit(PG_rmw, &(page)->flags)

#define PageWriteCount(page) ((page)->writes)
#define IncWriteCount(page) ((page)->writes++);
#define DecWriteCount(page) ((page)->writes--);

#define PageReadCount(page) ((page)->reads)
#define IncReadCount(page)  ((page)->reads++);
#define DecReadCount(page)  ((page)->reads--);

#define ClearPageFlags(page) (page)->flags =0;

#define extent_ref_release(p) 
#define extent_ref_get(p)

#endif	/* _FLAGS_H */
