/*
 * stats.h - remote memory stats
 */

#ifndef __RMEM_STATS_H__
#define __RMEM_STATS_H__

/*
 * Remote memory stat counters. 
 * Don't use these enums directly. Instead, use the RSTAT() macro in defs.h
 */
enum {
    RSTAT_FAULTS = 0,
    RSTAT_FAULTS_R,
    RSTAT_FAULTS_W,
    RSTAT_FAULTS_WP,
    RSTAT_WP_UPGRADES,
    RSTAT_FAULTS_ZP,
    RSTAT_FAULTS_DONE,
    RSTAT_UFFD_NOTIF,
    RSTAT_UFFD_RETRIES,
    RSTAT_RDAHEADS,
    RSTAT_RDAHEAD_PAGES,

    RSTAT_EVICTS,
    RSTAT_EVICT_PAGES,
    RSTAT_EVICT_RETRIES,
    RSTAT_EVICT_WBACK,
    RSTAT_EVICT_WP_RETRIES,
    RSTAT_EVICT_MADV,
    RSTAT_EVICT_DONE,
    RSTAT_EVICT_PAGES_DONE,

    RSTAT_NET_READ,
    RSTAT_NET_WRITE,

    RSTAT_READY_STEALS,
    RSTAT_WAIT_STEALS,
    RSTAT_WAIT_RETRIES,

    RSTAT_MALLOC_SIZE,
    RSTAT_MUNMAP_SIZE,
    RSTAT_MADV_SIZE,

    RSTAT_NR,   /* total number of counters */
};

#endif  // __RMEM_STATS_H__