/*
 * uffd.h - uffd helper methods
 */

#ifndef __UFFD_H__
#define __UFFD_H__

int userfaultfd(int flags);
int uffd_init(void);
int uffd_register(int fd, unsigned long addr, size_t size, int writeable);
int uffd_unregister(int fd, unsigned long addr, size_t size);
int uffd_copy(int fd, unsigned long dst, unsigned long src, size_t size, 
    bool wrprotect, bool no_wake, bool retry, int *n_retries);
int uffd_wp(int fd, unsigned long addr, size_t size, bool wrprotect, 
    bool no_wake, bool retry, int *n_retries);
int uffd_wp_add(int fd, unsigned long fault_addr, size_t size, bool no_wake, 
    bool retry, int *n_retries);
int uffd_wp_remove(int fd, unsigned long fault_addr, size_t size, bool no_wake, 
    bool retry, int *n_retries);
int uffd_zero(int fd, unsigned long addr, size_t size, bool no_wake, 
    bool retry, int *n_retries);
int uffd_wake(int fd, unsigned long addr, size_t size);

/* uffd state */
extern int userfault_fd;

#endif  // __UFFD_H__