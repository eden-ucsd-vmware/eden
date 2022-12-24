/**
 * fltrace.c - Memory interposition library to forward 
 * all heap allocations to UFFD-registered memory and 
 * kick off a handler thread to serve them.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>
#include <jemalloc/jemalloc.h>

#include "base/assert.h"
#include "base/atomic.h"
#include "base/init.h"
#include "base/log.h"
#include "base/mem.h"
#include "base/realmem.h"
#include "rmem/api.h"
#include "rmem/common.h"
#include "rmem/region.h"

/**
 * Defs 
 */
enum init_state {
    NOT_STARTED = 0,
    INITIALIZED = 1,
    INIT_STARTED = 2,
    INIT_FAILED = 3
};

/* State */
__thread bool __from_internal_jemalloc = false;
__thread bool __init_in_progress = false;
static atomic_t rmlib_state = ATOMIC_INIT(NOT_STARTED);
int shm_id;

/**
 * We need modified versions of logging calls that do not call 
 * malloc internally to avoid recursive behavior. 
 * We will only use these functions for logging in this file.
 */
#define ft_log(fmt, ...)                                    \
  do {                                                      \
    fprintf(stderr, "[%s][%s:%d]: " fmt "\n", __FILE__,     \
            __func__, __LINE__, ##__VA_ARGS__);             \
  } while (0)
#define ft_log_info ft_log
#define ft_log_warn ft_log
#define ft_log_err  ft_log
#ifdef DEBUG
#define ft_log_debug ft_log
#else
#define ft_log_debug(fmt, ...) do {} while (0)
#endif

/**
 * Helpers 
 */
int parse_env_settings()
{
    char *memory_limit, *evict_thr; 
    
    /* set local memory */
    memory_limit = getenv("LOCAL_MEMORY");
    /* Hack: fix a bug wheren env variable has non-printable chars at start */
    if (memory_limit != NULL)
        while (!isalnum(memory_limit[0]) && memory_limit[0] != 0)
            memory_limit++;

    if (memory_limit == NULL) {
        ft_log_err("set LOCAL_MEMORY (in bytes) env var to enable remote memory");
        return 1;
    }
    local_memory = atoll(memory_limit);

    /* set eviction threshold */
    evict_thr = getenv("EVICTION_THRESHOLD");
    if (evict_thr != NULL)
        eviction_threshold = atof(evict_thr);
    
    return 0;
}

/**
 * Some wrappers for RMem API
 */

void *rmlib_rmmap(void *addr, size_t length, int prot, 
    int flags, int fd, off_t offset)
{
    void *p = NULL;
    if (!(flags & MAP_ANONYMOUS) || !(flags & MAP_ANON) || (prot & PROT_EXEC) 
            || (flags & (MAP_STACK | MAP_FIXED | MAP_DENYWRITE))
            || (addr && !within_memory_region(addr))) {
        p = real_mmap(addr, length, prot, flags, fd, offset);
    } else {
        /* we don't support these flags */
        assertz(prot & PROT_EXEC);
        assertz(flags & MAP_STACK);
        assertz(flags & MAP_FIXED);
        assertz(flags & MAP_DENYWRITE);
        assert(fd == -1);
        assert(length);
        ft_log_debug("%s - using rmalloc", __func__);
        p = rmalloc(length);
    }
    return p;
}

int rmlib_rmunmap(void *ptr, size_t length)
{
    if (!ptr) return 0;
    if (!within_memory_region(ptr)) {
        return real_munmap(ptr, length);
    } else {
        return rmunmap(ptr, length);
    }
}

/**
 * Main Initialization - every external (i.e., coming from the
 * application) memory alloc/map/free call we interpose on in this
 * library will call this function first to initialize resources or
 * wait while someone else does it.
 *
 * We could initialize rmlib in a constructor, such as: static
 * __attribute__((constructor)) void __init__(void) but that would be
 * incorrect because the contructor is called before main, but malloc
 * can be called during other libraries initializations.
 *
 * Note on jemalloc: jemalloc relies on libc mmap, which we interpose.
 * We forward all external memory calls to jemalloc and handle its
 * allocation calls in our mmap shim.
 *
 * This whole interposition is infested with potential infinite loops
 * so tread carefully. E.g., To initialize real mmap, we use dlopen
 * which calls malloc internally -- hence the use of libc calls.
 * Similarly, printf and other logging calls during init may call
 * malloc too.
 */
static bool init(bool init_start_expected)
{
    bool ret, status;
    int r, oldval, shmid, initd;
    key_t key;

    /* inf loop; a thread came back here during init */
    if (__init_in_progress) {
        ft_log_err("ERROR! init called recursively");
        exit(1);
    }

again:
    /* check rmlib status */
    initd = atomic_read(&rmlib_state);
    switch (initd)
    {
        case NOT_STARTED:
            /* continue to init */
            if (init_start_expected) {
                ft_log_err("ERROR! init already expected");
                exit(1);
            }
            break;
        case INIT_STARTED:
            /* wait for init to finish */
            cpu_relax();
            goto again;
            break;
        case INIT_FAILED:
            return false;
        case INITIALIZED:
            return true;
        default:
            BUG();  /*unknown*/
    }

    /* claim the one to be initing */
    oldval = atomic_cmpxchg_val(&rmlib_state, NOT_STARTED, INIT_STARTED);
    ft_log_debug("CAS ret=%d", oldval);
    if (oldval != NOT_STARTED)
        /* someone else started, check again on my next action */
        goto again;

    /* i started init */
    __init_in_progress = true;

    /* check for fork'ed processes that inherit LD_PRELOAD */
    key = ftok("rmem_rmlib", 65);
    shmid = shmget(key, 1024, 0666 | IPC_CREAT | IPC_EXCL);
    ft_log_debug("shm id for key %d: %d", key, shmid);

    if (shmid < 0) {
        ft_log_warn("failed to create new shmid, some other process or parent" 
            "process may already be running with rmlib. errno: %d", errno);
        /* use libc for fork'ed processes */
        goto error;
    }

    /* just a hey! to whoever might be listening (aka debugging) */
    shm_id = shmid;
    char *str = (char *)shmat(shmid, (void *)0, 0);
    sprintf(str, "hello from pid %d", getpid());
    shmdt(str);

    /* get settings from env */
    r = parse_env_settings();
    if (r) {
        ft_log_err("failed to parse env settings");
        goto error;
    }

    /* init base library */
    ft_log_debug("calling base init");
    r = base_init();
    if (r)  goto error;

    /* init rmem (with local backend) */
    ft_log_debug("calling rmem init");
    rmem_enabled = true;
    rmbackend_type = RMEM_BACKEND_LOCAL;
    r = rmem_common_init();
    if (r)  goto error;

    /* done initializing */
    ret = atomic_cmpxchg(&rmlib_state, INIT_STARTED, INITIALIZED);
    BUG_ON(!ret);
    status = true;
    goto out;

error:
    ft_log_warn("couldn't init remote memory; reverting to libc");
    ret = atomic_cmpxchg(&rmlib_state, INIT_STARTED, INIT_FAILED);
    BUG_ON(!ret);
    status = false;
    goto out;

out:
    __init_in_progress = false;
    return status;
}

/**
 *  Interface functions
 */

void *malloc(size_t size)
{
    bool from_runtime;
    void* retptr;

    from_runtime = IN_RUNTIME();
    RUNTIME_ENTER();

    ft_log_debug("[%s], size=%lu, from-runtime=%d from-jemalloc=%d",
        __func__, size, from_runtime, __from_internal_jemalloc);

    if (from_runtime) {
        ft_log_debug("%s from runtime, using libc", __func__);
        retptr = libc_malloc(size);
        goto out;
    }

    /* rmlib status */
    if (!init(false)) {
        ft_log_debug("%s not initialized, using libc", __func__);
        retptr = libc_malloc(size);
        goto out;
    }

    /* application malloc */
    __from_internal_jemalloc = true;
    ft_log_debug("using je_malloc");
    retptr = rmlib_je_malloc(size);
    __from_internal_jemalloc = false;

out:
    ft_log_debug("[%s] return=%p", __func__, retptr);
    if (!from_runtime)
        RUNTIME_EXIT();
    return retptr;
}

void free(void *ptr)
{
    int initd;
    bool from_runtime;

    if (ptr == NULL)
        return;

    from_runtime = IN_RUNTIME();
    RUNTIME_ENTER();

    ft_log_debug("[%s] ptr=%p from-runtime=%d from-jemalloc=%d", __func__,
        ptr, from_runtime, __from_internal_jemalloc);

    if (from_runtime) {
        ft_log_debug("%s from runtime, using libc", __func__);
        libc_free(ptr);
        goto out;
    }

    /* rmlib status */
    initd = atomic_read(&rmlib_state);
    BUG_ON(initd == NOT_STARTED);
    if (initd == INIT_FAILED) {
        ft_log_debug("%s not initialized, using libc", __func__);
        libc_free(ptr);
        goto out;
    }

    /** FIXME: there may have been some non-runtime libc mallocs that occurred
     * between INIT_STARTED and INITIALIZED that we would be passing to 
     * jemalloc. We should keep track of these and use libc_free on them. */

    /* application free */
    __from_internal_jemalloc = true;
    rmlib_je_free(ptr);
    __from_internal_jemalloc = false;

out:
    ft_log_debug("[%s] return", __func__);
    if (!from_runtime)
        RUNTIME_EXIT();
}

void *realloc(void *ptr, size_t size)
{
    void *retptr;
    bool from_runtime;

    if (ptr == NULL) 
        return malloc(size);

    from_runtime = IN_RUNTIME();
    RUNTIME_ENTER();

    ft_log_debug("[%s] ptr=%p, size=%lu, from-runtime=%d from-jemalloc=%d",
        __func__, ptr, size, from_runtime, __from_internal_jemalloc);

    if (from_runtime) {
        ft_log_debug("%s from runtime, using libc", __func__);
        retptr = libc_realloc(ptr, size);
        goto out;
    }

    /* rmlib status */
    if (!init(true)) {
        ft_log_debug("%s not initialized, using libc", __func__);
        retptr = libc_realloc(ptr, size);
        goto out;
    }
    
    /* application realloc */
    __from_internal_jemalloc = true;
    retptr = rmlib_je_realloc(ptr, size);
    __from_internal_jemalloc = false;

out:
    ft_log_debug("[%s] return=%p", __func__, retptr);
    if (!from_runtime)
        RUNTIME_EXIT();
    return retptr;
}

void *calloc(size_t nitems, size_t size)
{
    void *retptr;
    bool from_runtime;

    from_runtime = IN_RUNTIME();
    RUNTIME_ENTER();

    ft_log_debug("[%s] number=%lu, size=%lu, from-runtime=%d", 
        __func__, nitems, size, from_runtime);

    if (from_runtime) {
        ft_log_debug("%s from runtime, using libc", __func__);
        retptr = libc_calloc(nitems, size);
        goto out;
    }

    /* rmlib status */
    if (!init(false)) {
        ft_log_debug("%s not initialized, using libc", __func__);
        retptr = libc_calloc(nitems, size);
        goto out;
    }

    /* application calloc */
    __from_internal_jemalloc = true;
    retptr = rmlib_je_calloc(nitems, size);
    __from_internal_jemalloc = false;

out:
    ft_log_debug("[%s] return=%p", __func__, retptr);
    if (!from_runtime)
        RUNTIME_EXIT();
    return retptr;
}

void *__internal_aligned_alloc(size_t alignment, size_t size)
{
    void *retptr;
    bool from_runtime;

    from_runtime = IN_RUNTIME();
    RUNTIME_ENTER();

    ft_log_debug("[%s] alignment=%lu, size=%lu, from-runtime=%d", 
        __func__, alignment, size, from_runtime);

    if (from_runtime) {
        ft_log_debug("%s from runtime, using libc", __func__);
        retptr = libc_memalign(alignment, size);
        goto out;
    }

    /* rmlib status */
    if (!init(false)) {
        ft_log_debug("%s not initialized, using libc", __func__);
        retptr = libc_memalign(alignment, size);
        goto out;
    }

    /* application aligned alloc */
    __from_internal_jemalloc = true;
    retptr = rmlib_je_aligned_alloc(alignment, size);
    __from_internal_jemalloc = false;

out:
    ft_log_debug("[%s] return=%p", __func__, retptr);
    if (!from_runtime)
        RUNTIME_EXIT();
    return retptr;
}

int posix_memalign(void **ptr, size_t alignment, size_t size)
{
    ft_log_debug("[%s] ptr=%p, alignment=%lu, size=%lu", 
        __func__, ptr, alignment, size);
    /* TODO: need to check alignment, check return values */
    *ptr = __internal_aligned_alloc(alignment, size);
    return 0;
}

void *memalign(size_t alignment, size_t size)
{
    ft_log_debug("[%s] alignment=%lu, size=%lu", __func__, alignment, size);
    return __internal_aligned_alloc(alignment, size);
}

void *aligned_alloc(size_t alignment, size_t size)
{
    ft_log_debug("[%s] alignment=%lu, size=%lu", __func__, alignment, size);
    return __internal_aligned_alloc(alignment, size);
}

/**
 * Memory management functions (sys/mman.h).
 */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    void *retptr;
    bool from_runtime;

    from_runtime = IN_RUNTIME();
    RUNTIME_ENTER();

    ft_log_debug("[%s] addr=%p,length=%lu,prot=%d,flags=%d,fd=%d,offset=%ld,from-"
    "runtime=%d", __func__, addr, length, prot, flags, fd, offset, from_runtime);

    /* First check for calls coming from jemalloc. These allocations are 
     * meant to be forwarded to remote memory; the tool must have been inited
     * by now as jemalloc calls are triggered by our own calls after init */
    if (__from_internal_jemalloc) {
        ft_log_debug("internal jemalloc mmap, fwd to RLib: addr=%p", addr);
        if (rmlib_state.cnt != INITIALIZED) {
            ft_log_err("ERROR! rmlib not initialized before jemalloc mmap");
            exit(1);
        }
        retptr = rmlib_rmmap(addr, length, prot, flags, fd, offset);
        goto out;
    }

    /* mmap coming directly from runtime */
    if (from_runtime) {
        ft_log_debug("%s from runtime, using real mmap", __func__);
        retptr = real_mmap(addr, length, prot, flags, fd, offset);
        goto out;
    }

    /* mmap coming from the app, initialize */
    if (!init(false)) {
        ft_log_debug("%s not initialized, using real mmap", __func__);
        retptr = real_mmap(addr, length, prot, flags, fd, offset);
        goto out;
    }

    if ((fd != -1) && (flags & MAP_ANONYMOUS)) {
        ft_log_err("ERROR! bad mmap args: fd=%d, flags=%d", fd, flags);
        exit(1);
    }

    ft_log_debug("%s directly from the app, fwd to RLib", __func__);
    retptr = rmlib_rmmap(addr, length, prot, flags, fd, offset);

out:
    ft_log_debug("[%s] return=%p", __func__, retptr);
    if (!from_runtime)
        RUNTIME_EXIT();
    return retptr;
}

int munmap(void *ptr, size_t length)
{
    int ret;
    bool from_runtime;
    enum init_state initd;

    if (!ptr) 
        return 0;
    
    from_runtime = IN_RUNTIME();
    RUNTIME_ENTER();

    ft_log_debug("[%s] ptr=%p, length=%lu, from-runtime=%d", __func__, ptr, 
        length, from_runtime);

    /* check init status */
    initd = atomic_read(&rmlib_state);

    /* First check for calls coming from jemalloc. These deallocations are 
     * meant to be forwarded to remote memory; the tool must have been init'd 
     * by now as jemalloc calls are triggered by our own calls after init */
    if (__from_internal_jemalloc) {
        ft_log_debug("internal jemalloc munmap, fwd to RLib: addr=%p", ptr);
        if (initd != INITIALIZED) {
            ft_log_err("ERROR! rmlib not initialized before jemalloc munmap");
            exit(1);
        }
        ret = rmlib_rmunmap(ptr, length);
        goto out;
    }

    if (from_runtime) {
        ft_log_debug("%s from runtime, using real munmap", __func__);
        ret = real_munmap(ptr, length);
        goto out;
    }

    /* directly from the app */
    if (initd == NOT_STARTED) {
        ft_log_err("ERROR! rmlib init not started before app munmap");
        exit(1);
    }

    if (initd == INIT_FAILED) {
        ret = real_munmap(ptr, length);
        ft_log_debug("child process; return=%d", ret);
        goto out;
    }
    ft_log_debug("munmap directly from the app, fwd to RLib: addr=%p", ptr);
    ret = rmlib_rmunmap(ptr, length);

out:
    ft_log_debug("[%s] return=%d", __func__, ret);
    if (!from_runtime)
        RUNTIME_EXIT();
    return ret;
}

int madvise(void *addr, size_t length, int advice)
{
    int ret;
    bool from_runtime, initd;

    from_runtime = IN_RUNTIME();
    RUNTIME_ENTER();

    ft_log_debug("[%s] addr=%p, size=%lu, advice=%d, from-runtime=%d from-je=%d", 
        __func__, addr, length, advice, from_runtime, __from_internal_jemalloc);
    if (advice == MADV_DONTNEED)    ft_log_debug("MADV_DONTNEED flag");
    if (advice == MADV_HUGEPAGE)    ft_log_debug("MADV_HUGEPAGE flag");
    if (advice == MADV_FREE)        ft_log_debug("MADV_FREE flag");

    if (from_runtime) {
        ft_log_debug("%s from runtime, using real madvise", __func__);
        ret = real_madvise(addr, length, advice);
        ft_log_debug("real madvise; return=%d", ret);
        goto out;
    }
    
    /* rmlib status */
    initd = init(false);
    if (initd && within_memory_region(addr))
        ret = rmadvise(addr, length, advice);
    else
        ret = real_madvise(addr, length, advice);

out:
    ft_log_debug("[%s] return=%d", __func__, ret);
    if (!from_runtime)
        RUNTIME_EXIT();
    return ret;
}

#if 0
/* others? */
void *mremap(void *old_addr, size_t old_size, size_t new_size, int flags,
                         ... /* void *new_address */) {
    ft_log_debug("addr=%p,old_size=%lu,new_size=%lu,flags=%d,from-runtime=%d",
       old_addr, old_size, new_size, flags, from_runtime);
    return 0;
}
#endif

static __attribute__((constructor)) void __init__(void)
{
    ft_log_debug("ftrace constructor!");
}

static __attribute__((destructor)) void finish(void)
{
    ft_log_debug("ftrace destructor!");
    /* NOTE: ideally we should free all remote memory resources
     * with rmem_common_destroy() here but since we assume that 
     * the program begins in "application" mode rather than in 
     * "runtime" mode, we send all initial (before main()) 
     * allocations to remote memory; hence we need them running 
     * even during destroy. */
    shmctl(shm_id, IPC_RMID, NULL);
}
