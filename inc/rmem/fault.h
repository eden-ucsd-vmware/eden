/*
 * fault.h - definitions for fault requests
 */

#ifndef __FAULT_H__
#define __FAULT_H__

#include <stdio.h>
#include <sys/queue.h>

#include "base/assert.h"
#include "base/lock.h"
#include "base/tcache.h"
#include "base/thread.h"
#include "base/types.h"
#include "runtime/thread.h"
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
    uint32_t reserved2;

    unsigned long page;
    struct region_t* region;
    thread_t* thread;
    uint64_t pad[2];

    TAILQ_ENTRY(fault) link;
} fault_t;
BUILD_ASSERT(sizeof(fault_t) % CACHE_LINE_SIZE == 0);

/* fault object as readable string - for debug tracking */
#define __FAULT_STR_LEN 100
extern __thread char fstr[__FAULT_STR_LEN];
static inline char* fault_to_str(fault_t* f) {
    snprintf(fstr, __FAULT_STR_LEN, "F[%s:%s:%lx]", 
        f->from_kernel ? "kern" : "user",
        f->is_read ? "r" : (f->is_write ? "w" : "wp"),
        f->page);
    return fstr;
}
#define FSTR(f) fault_to_str(f)

/*
 * Fault object tcache support
 */
DECLARE_PERTHREAD(struct tcache_perthread, fault_pt);

/* inits */
int fault_tcache_init(); 
int fault_tcache_init_thread();

/* fault_alloc - allocates a fault object */
static inline fault_t *fault_alloc(void) {
	return tcache_alloc(&perthread_get(fault_pt));
}

/* fault_free - frees a fault */
static inline void fault_free(struct fault *f) {
	tcache_free(&perthread_get(fault_pt), (void *)f);
}

/*
 * Fault request utils
 */
static inline void fault_upgrade_to_write(fault_t* f) {
    f->is_read = f->is_wrprotect = false;
    f->is_write = true;
    log_debug("%s - upgraded to WRITE as no WP_ON_READ", FSTR(f));
}

/*
 * Per-thread zero page support
 */
extern __thread void* zero_page;

static inline void zero_page_init_thread() {
  zero_page = aligned_alloc(CHUNK_SIZE, CHUNK_SIZE);
  assert(zero_page);
  memset(zero_page, 0, CHUNK_SIZE);
}

static inline void zero_page_free_thread() {
  assert(zero_page);
  free(zero_page);
}

#endif  // __FAULT_H__