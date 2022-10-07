/*
 * region.h - Remote memory region management helpers
 */

#ifndef __REGION_H__
#define __REGION_H__

#include <infiniband/verbs.h>   /* TODO: get rid of this dependency */
#include <sys/queue.h>
#include <stdatomic.h>

#include "base/assert.h"
#include "base/lock.h"
#include "rmem/config.h"
#include "rmem/rdma.h"

/* Smallest native type that can support flags for each page */
typedef char pflags_t;
typedef _Atomic(pflags_t) atomic_pflags_t;
BUILD_ASSERT(sizeof(atomic_pflags_t) == sizeof(pflags_t));

/**
 * Region definition
 */
struct region_t {
    /* region metadata */
    volatile size_t size;
    unsigned long addr;
    unsigned long remote_addr;
    atomic_ullong current_offset;
    atomic_ullong evict_offset;

    /* page metadata */
    atomic_pflags_t *page_flags;

    /* RDMA-specific data. TODO: move into rdma backend */
    struct server_conn_t *server;

    atomic_int ref_cnt;
    CIRCLEQ_ENTRY(region_t) link;
} __aligned(CACHE_LINE_SIZE);

/* region data */
CIRCLEQ_HEAD(region_listhead, region_t);
extern struct region_listhead region_list;
extern struct region_t* last_evicted;
DECLARE_SPINLOCK(regions_lock);

/* functions */
int register_memory_region(struct region_t *mr, int writeable);
void remove_memory_region(struct region_t *mr);

/* 
 * Memory region utils 
 */

/* Adds a reference to region
 * For internal-use. Unsafe unless used from within the regions_lock */
static inline bool __get_mr(struct region_t *mr) 
{
    log_debug("adding ref_cnt for mr %p", mr);
    int r = atomic_fetch_add_explicit(&mr->ref_cnt, 1, memory_order_acquire);
    BUG_ON(r < 0);
    return (r > 0);
}

/* Checks if a given address falls in a region. You must already have a 
 * safe reference for mr for this to be deletion-safe */
static inline bool is_in_memory_region_unsafe(struct region_t *mr, 
    unsigned long addr) 
{
    return addr >= mr->addr && addr < mr->addr + mr->size;
}

/* Checks if given pointer falls in any of the active memory regions */
static inline bool within_memory_region(void *ptr) 
{
    if (ptr == NULL)
        return false;

    struct region_t *mr = NULL;
    spin_lock(&regions_lock);
    CIRCLEQ_FOREACH(mr, &region_list, link) {
        if (is_in_memory_region_unsafe(mr, (unsigned long)ptr)) {
            spin_unlock(&regions_lock);
            return true;
        }
    }
    spin_unlock(&regions_lock);
    return false;
}

/* Finds a region with available memory of given size. It returns a deletion-safe 
 * reference so make sure to put_mr() once done. Note however that it doesn't 
 * reserve memory so it might be gone by the time you get to it. */
static inline struct region_t *get_available_region(size_t size) 
{
    struct region_t *mr = NULL;
    spin_lock(&regions_lock);
    CIRCLEQ_FOREACH(mr, &region_list, link) {
        size_t required_space = size;
        if (mr->current_offset + required_space <= mr->size) {
            log_debug("%s:found avilable mr:%p for size:%ld", __func__, mr, size);
            __get_mr(mr);
            spin_unlock(&regions_lock);
            return mr;
        } else {
            log_debug("%s: mr:%p is out of memory. size:%ld, current offset:%lld",
                __func__, mr, mr->size, mr->current_offset);
        }
    }
    spin_unlock(&regions_lock);
    log_info("available mr does not have enough memory to serve, add new slab");
    return NULL;
}

/* Get next evictable region. It returns a deletion-safe reference so make 
 * sure to put_mr() once done. */
static inline struct region_t *get_next_evictable_region() 
{
    spin_lock(&regions_lock);
    if (CIRCLEQ_EMPTY(&region_list))
        return NULL;

    if (last_evicted == NULL) {
        last_evicted = CIRCLEQ_FIRST(&region_list);
        if (last_evicted != NULL)   __get_mr(last_evicted);
        spin_unlock(&regions_lock);
        return last_evicted;
    }

    last_evicted = CIRCLEQ_NEXT(last_evicted, link);
    if (last_evicted != NULL)   __get_mr(last_evicted);
    spin_unlock(&regions_lock);
    return last_evicted;
}

/* Get first region. It returns an unsafe reference so use it with care. */
static inline struct region_t *get_first_region_unsafe() 
{
    struct region_t* mr;
    spin_lock(&regions_lock);
    mr = CIRCLEQ_FIRST(&region_list);
    spin_unlock(&regions_lock);
    return mr;
}

static inline struct region_t* __get_region_by_addr(unsigned long addr, 
    bool add_ref) 
{
    struct region_t *mr = NULL;
    spin_lock(&regions_lock);
    CIRCLEQ_FOREACH(mr, &region_list, link) {
        if (is_in_memory_region_unsafe(mr, addr)) {
            if (add_ref)
                __get_mr(mr);
            spin_unlock(&regions_lock);
            return mr;
        }
    }
    spin_unlock(&regions_lock);
    return NULL;
}

static inline struct region_t* get_region_by_addr_unsafe(unsigned long addr) 
{
    return __get_region_by_addr(addr, false);
}

/* Finds region of a page. It increments ref_count so make sure to cache it 
 * through the operation and put_mr() after the usage is done */
static inline struct region_t* get_region_by_addr_safe(unsigned long addr) 
{
    return __get_region_by_addr(addr, true);
}

static inline void put_mr_references(struct region_t *mr, int n) 
{
    log_debug("decreasing ref_cnt for mr %p", mr);
    atomic_fetch_sub_explicit(&mr->ref_cnt, n, memory_order_consume);
}

static inline void put_mr(struct region_t *mr) 
{
    assert(mr);
    if (mr != NULL) put_mr_references(mr, 1);
}

#endif    // __REGION_H__