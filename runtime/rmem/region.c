/*
 * region.h - Remote memory region management helpers
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/mman.h>

#include "base/stddef.h"
#include "rmem/backend.h"
#include "rmem/pflags.h"
#include "rmem/region.h"
#include "rmem/uffd.h"

/* region data */
struct region_listhead region_list;
struct region_t* last_evicted = NULL;
struct region_t* region_cached = NULL;
int nregions = 0;
DEFINE_SPINLOCK(regions_lock);

void deregister_memory_region(struct region_t *mr) {
    int r = 0;
    log_debug("deregistering region %p", mr);
    if (mr->addr != 0) {
        uffd_unregister(userfault_fd, mr->addr, mr->size);
        r = munmap((void *)mr->addr, mr->size);
        if (r < 0) log_warn("munmap failed");
        size_t page_flags_size = 
            align_up((mr->size >> CHUNK_SHIFT), 8) * PAGE_FLAGS_NUM / 8;
        r = munmap(mr->page_flags, page_flags_size);
        if (r < 0) log_warn("munmap page_flags failed");
    }
    mr->addr = 0;
}

int register_memory_region(struct region_t *mr, int writeable) {
    void *ptr = NULL;
    size_t page_flags_size;
    int r;

    log_debug("registering region %p", mr);

    /* mmap virt addr space*/
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int prot = PROT_READ;
    if (writeable)  prot |= PROT_WRITE;
    ptr = mmap(NULL, mr->size, prot, mmap_flags, -1, 0);
    if (ptr == MAP_FAILED) {
        log_err("mmap failed");
        goto error;
    }
    mr->addr = (unsigned long)ptr;
    log_info("mmap ptr %p addr mr %p, size %ld\n", 
        ptr, (void *)mr->addr, mr->size);

    /* register it with userfaultfd */
    assert(userfault_fd >= 0);
    r = uffd_register(userfault_fd, mr->addr, mr->size, writeable);
    if (r < 0) goto error;

    /* initalize metadata */
    page_flags_size = align_up((mr->size >> CHUNK_SHIFT), 8) * PAGE_FLAGS_NUM / 8;
     mr->page_flags = (atomic_pflags_t *)mmap(NULL, page_flags_size, 
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mr->page_flags == NULL) 
        goto error;
    mr->ref_cnt = ATOMIC_VAR_INIT(0);
    mr->current_offset = ATOMIC_VAR_INIT(0);
    mr->evict_offset = ATOMIC_VAR_INIT(0);

    /* add it to the list. TODO: this should be done in rmem.c after adding 
     * a region */
    spin_lock(&regions_lock);
    CIRCLEQ_INSERT_HEAD(&region_list, mr, link);
    nregions++;
    region_cached = mr;
    BUG_ON(nregions > RMEM_MAX_REGIONS);
    spin_unlock(&regions_lock);
    return 0;
error:
    deregister_memory_region(mr);
    return 1;
}

void remove_memory_region(struct region_t *mr) {
    int ret;
    log_debug("deleting region %p", mr);
    BUG_ON(atomic_load(&mr->ref_cnt) > 0);
    
    spin_lock(&regions_lock);
    CIRCLEQ_REMOVE(&region_list, mr, link);
    nregions--;
    last_evicted = CIRCLEQ_FIRST(&region_list); /* reset */
    spin_unlock(&regions_lock);

    /* deregister */
    deregister_memory_region(mr);

    /* notify backed memory */
    assert(rmbackend != NULL);
    ret = rmbackend->remove_region(mr);
    assertz(ret);
    
    munmap(mr, sizeof(struct region_t));
}
