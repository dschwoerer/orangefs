#ifndef __STATE_H_
#define __STATE_H_

int NCAC_extent_read_access(NCAC_req_t *req, struct extent *page,
                        unsigned long offset, unsigned long size);
int NCAC_extent_write_access(NCAC_req_t *req, struct extent *page,
                        unsigned long offset, unsigned long size);
int NCAC_extent_first_read_access(NCAC_req_t *req, struct extent *page);
int NCAC_extent_first_write_access(NCAC_req_t *req, struct extent *page);
int NCAC_extent_read_comm_done(struct extent *page);
int NCAC_extent_write_comm_done(struct extent *page);
int NCAC_check_ioreq(struct extent *page);
int NCAC_move_inactive_to_active(struct cache_stack *cache_stack,struct extent *page);
int NCAC_extent_read_access_recheck(NCAC_req_t *req, struct extent *page,
                        unsigned int offset, unsigned int size);
int NCAC_extent_write_access_recheck(NCAC_req_t *req, struct extent *page,
                        unsigned int offset, unsigned int size);
int NCAC_extent_done_access(NCAC_req_t *ncac_req);
void mark_extent_rmw_lock(struct extent *extent, int ioreq);

#endif
