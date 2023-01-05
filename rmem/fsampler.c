/*
 * fsampler.c - fault sampling support for the handler threads
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "base/sampler.h"
#include "rmem/common.h"
#include "rmem/config.h"
#include "rmem/handler.h"

#define FAULT_TRACE_BUF_SIZE    (FAULT_TRACE_STEPS*15)
#define MAX_FAULT_SAMPLE_LEN    1000

/* defs */
struct fsample {
    volatile int valid;     /* backtrace done */
    volatile int busy;      /* backtrace in progress */
    unsigned long tstamp_tsc;
    unsigned long ip;
    unsigned long addr;
    int kind;
    int tid;
    void* bktrace[FAULT_TRACE_STEPS];
    int trace_size;
} __aligned(CACHE_LINE_SIZE);

/* sampler global state */
struct sampler fsamplers[MAX_FAULT_SAMPLERS];
struct fsample fsample_data[MAX_FAULT_SAMPLERS] = {0};
atomic_t nsamplers;
int sigid;
unsigned long sampler_start_tsc;

/**
 * Ops for base sampler support
 */
void fault_add_sample(void* buffer, void* sample)
{
    /* note: the shallow copy */
    assert(buffer && sample);
    memcpy(buffer, sample, sizeof(struct fsample));
}

void fault_sample_to_str(void* sample, char* sbuf, int max_len)
{
    int n, i;
    struct fsample* fs;
    char trace[FAULT_TRACE_BUF_SIZE] = {'\0'};

    assert(sbuf && sample);
    fs = (struct fsample*) sample;

    /* convert the backtrace to text */
    for(i = 0; i < fs->trace_size && 
        strlen(trace) < (FAULT_TRACE_BUF_SIZE-1); i++)
            snprintf(&trace[strlen(trace)], 
                (FAULT_TRACE_BUF_SIZE - 1 - strlen(trace)), "%p|", fs->bktrace[i]);

    /* write to string buf */
    n = snprintf(sbuf, max_len, "%lu,%lx,%d,%lx,%d,%s", 
            (fs->tstamp_tsc - sampler_start_tsc) / cycles_per_us,
            fs->ip, fs->kind, fs->addr, fs->tid, trace);
    BUG_ON(n >= max_len);   /* truncated */
}

/* base sampler ops */
struct sampler_ops fault_sampler_ops = {
    .add_sample = fault_add_sample,
    .sample_to_str = fault_sample_to_str,
};

/**
 * Returns the next available sampler (id)
 */
int fsampler_get_sampler()
{
    int fsid;
    char fsname[100];

    /* atomically g et a sampler id */
    do {
        fsid = atomic_read(&nsamplers);
        BUG_ON(fsid > MAX_FAULT_SAMPLERS);
        if (fsid == MAX_FAULT_SAMPLERS) {
            log_warn("out of fault samplers!");
            return -1;
        }
    } while(!atomic_cmpxchg(&nsamplers, fsid, fsid + 1));
    log_debug("sampler %d taken, num samplers: %d",
        fsid, atomic_read(&nsamplers));

    /* initialize sampler */
    sprintf(fsname, "fault_samples%d", 1 + fsid);
    sampler_init(&fsamplers[fsid], fsname, SAMPLER_TYPE_POISSON,
        &fault_sampler_ops, sizeof(struct fsample), 1000, 1000, 1);
    log_info("initialized fault sampler %d", 1 + fsid);

    return fsid;
}

/**
 * Record the fault sample with a backtrace of the faulting thread
 * @fsid: the id of the sampler to use for recording
 * @kind: the kind of fault
 * @addr: the faulting address
 * @tid: the faulting thread id
 */
void fsampler_add_fault_sample(int fsid, int kind, unsigned long addr, pid_t tid)
{
    int ret;
    struct sampler* sampler;
    struct fsample* sample;
    unsigned long now_tsc;
    siginfo_t sginfo;

    assert(fsid >= 0 && fsid < MAX_FAULT_SAMPLERS);
    sampler = &fsamplers[fsid];
    sample = &fsample_data[fsid];

    now_tsc = rdtsc();
    if (!sampler_is_time(sampler, now_tsc))
        return;

    /* wait for previous backtrace to finish */
    log_debug("waiting for previous backtrace to finish");

    while (load_acquire(&sample->busy))
        cpu_relax();

    /* record the previous backtrace */
    if (sample->valid)
        sampler_add_provide_tsc(sampler, sample, sample->tstamp_tsc);

    /* prepare for next sample */
    sample->tstamp_tsc = now_tsc;
    sample->kind = kind;
    sample->addr = addr;
    sample->tid = tid;
    sample->trace_size = 0;
    sample->valid = 1;
    store_release(&sample->busy, 1);

    /* send a signal to the thread to get the new backtrace */
    log_debug("sampler %d sending sig %d to tid: %d\n", fsid, sigid, tid);
    assert(tid);
    sginfo.si_signo = sigid;
    sginfo.si_code = SI_QUEUE;
    sginfo.si_value.sival_int = fsid;
    ret = syscall(SYS_rt_tgsigqueueinfo, getpid(), tid, sigid, &sginfo);
    if (ret)
        log_warn("failed to send signal to tid: %d, errno: %d", tid, errno);
    assertz(ret);
}

/**
 * Dump any recorded samples of a sampler
 * @fsid: the sampler id
 */
void fsampler_dump(int fsid)
{
    struct sampler* sampler;

    assert(fsid >= 0 && fsid < MAX_FAULT_SAMPLERS);
    sampler = &fsamplers[fsid];
    sampler_dump(sampler, MAX_FAULT_SAMPLE_LEN);
}

void save_stacktrace(int signum, siginfo_t *siginfo, void *context)
{
    int fsid;

    fsid = siginfo->si_value.sival_int;
    log_debug("received signal %d for sampler %d", signum, fsid);
    assert(fsid >= 0 && fsid < MAX_FAULT_SAMPLERS);
    assert(fsample_data[fsid].valid);
    assert(fsample_data[fsid].busy);

    /* backtrace */
    fsample_data[fsid].trace_size = 
        backtrace(fsample_data[fsid].bktrace, FAULT_TRACE_STEPS);

    /* set done */
    store_release(&fsample_data[fsid].busy, 0);
    log_debug("backtrace done for sampler %d", fsid);
}

/**
 * sampler_init - initializes samplers for handler threads
 */
int fsampler_init(void)
{
    int i, ret;
    struct sigaction act, oldact;

    /* find a signal that hasn't been registered */
    for (i = SIGRTMIN; i < SIGRTMAX; i++) {
        ret = sigaction(i, NULL, &oldact);
        BUG_ON(ret);
        if (oldact.sa_sigaction == NULL && oldact.sa_handler == NULL)
            break;
    }
    if (i == SIGRTMAX) {
        log_err("no free signal for fault sampler");
        return 1;
    }

    /* register a signal handler that saves stack-trace of the 
     * handling thread */
    sigid = i;
    act.sa_sigaction = save_stacktrace;
    act.sa_flags = SA_SIGINFO;
    sigemptyset(&act.sa_mask);
    ret = sigaction(sigid, &act, NULL);
    assertz(ret);
    log_info("registered signal %d for fault sampler", sigid);

    /* start timestamp */
    sampler_start_tsc = rdtsc();
    return 0;
}


/**
 * fsampler_destroy - destroys samplers
 */
int fsampler_destroy(void)
{
    int i;
    for (i = 0; i < MAX_FAULT_SAMPLERS; i++)
        sampler_destroy(&fsamplers[i]);
    return 0;
}