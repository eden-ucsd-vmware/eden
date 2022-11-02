/*
 * fault.h - definitions for fault requests
 */

#ifndef __FAULT_H__
#define __FAULT_H__

#include <stdio.h>
#include <stdint.h>

#include "base/assert.h"
#include "base/lock.h"
#include "base/tcache.h"
#include "base/thread.h"
#include "base/types.h"
#include "runtime/thread.h"
#include "rmem/backend.h"
#include "rmem/config.h"

/*
 * Fault object 
 */

typedef struct fault {
    /* flags */
    uint8_t is_read;
    uint8_t is_write;
    uint8_t is_wrprotect;
    uint8_t from_kernel;
    uint8_t single_use;
    uint8_t rdahead_max;        /* suggested max read-ahead */
    uint8_t rdahead;            /* actual read-ahead locked for this fault */
    int8_t posted_chan_id;

    /* associated resources */
    unsigned long page;
    struct region_t* mr;
    thread_t* thread;
    void* bkend_buf;
    uint64_t pad[1];

	struct list_node link;
} fault_t;
BUILD_ASSERT(sizeof(fault_t) % CACHE_LINE_SIZE == 0);
BUILD_ASSERT(FAULT_MAX_RDAHEAD_SIZE < UINT8_MAX);   /* due to rdahead */

/* fault object as readable string - for debug tracking */
#define __FAULT_STR_LEN 100
extern __thread char fstr[__FAULT_STR_LEN];
static inline char* fault_to_str(fault_t* f) {
    snprintf(fstr, __FAULT_STR_LEN, "F[%s:%s:%lx:%d]", 
        f->from_kernel ? "kern" : "user",
        f->is_read ? "r" : (f->is_write ? "w" : "wp"),
        f->page, f->rdahead);
    return fstr;
}
#define FSTR(f) fault_to_str(f)

/*
 * Fault object tcache support
 */
DECLARE_PERTHREAD(struct tcache_perthread, fault_pt);

/* inits */
int fault_tcache_init(); 
void fault_tcache_init_thread();

/* fault_alloc - allocates a fault object */
static inline fault_t *fault_alloc(void)
{
    return tcache_alloc(&perthread_get(fault_pt));
}

/* fault_free - frees a fault */
static inline void fault_free(struct fault *f)
{
    tcache_free(&perthread_get(fault_pt), (void *)f);
}

/*
 * Fault request utils
 */
static inline void fault_upgrade_to_write(fault_t* f, const char* reason)
{
    f->is_read = f->is_wrprotect = false;
    f->is_write = true;
    log_debug("%s - upgraded to WRITE. Reason: %s", FSTR(f), reason);
}

/**
 * Per-thread zero page support
 */
extern __thread void* zero_page;
void zero_page_init_thread();
void zero_page_free_thread();

/**
 * Fault handling
 */
enum fault_status {
    FAULT_DONE = 0,
    FAULT_IN_PROGRESS,
    FAULT_READ_POSTED
};

struct bkend_completion_cbs;
enum fault_status handle_page_fault(int chan_id, fault_t* fault, int* nevicts, 
    struct bkend_completion_cbs* cbs);
int fault_read_done(fault_t* f);
void fault_done(fault_t* fault);

#endif    // __FAULT_H__