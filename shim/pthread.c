
#include <dlfcn.h>
#include <pthread.h>

#include <base/lock.h>
#include <base/log.h>
#include <rmem/common.h>
#include <runtime/sync.h>
#include <runtime/thread.h>

BUILD_ASSERT(sizeof(pthread_t) >= sizeof(uintptr_t));

struct join_handle {
	void *(*fn)(void *);
	void *args;
	void *retval;
	spinlock_t lock;
	thread_t *waiter;
	bool detached;
};

static void thread_trampoline(void *arg)
{
	struct join_handle *j = arg;

	j->retval = j->fn(j->args);
	preempt_disable();
	spin_lock(&j->lock);
	if (j->detached) {
		spin_unlock(&j->lock);
		preempt_enable();
		return;
	}
	if (j->waiter != NULL) {
		thread_ready_preempt_disabled(j->waiter);
	}
	j->waiter = thread_self();
	thread_park_and_unlock_np(&j->lock);
}

static int thread_spawn_joinable(struct join_handle **handle,
				 void *(*fn)(void *), void *arg)
{
	struct join_handle *j;

	preempt_disable();
	thread_t *t = thread_create_with_buf(thread_trampoline, (void **)&j,
					     sizeof(struct join_handle));
	if (t == NULL) {
		preempt_enable();
		return -ENOMEM;
	}

	j->fn = fn;
	j->args = arg;
	spin_lock_init(&j->lock);
	j->waiter = NULL;
	j->detached = false;

	if (handle)
		*handle = j;

	thread_ready_preempt_disabled(t);
	preempt_enable();
	return 0;
}

static int thread_detach(struct join_handle *j)
{
	preempt_disable();
	spin_lock(&j->lock);
	if (j->detached) {
		spin_unlock(&j->lock);
		preempt_enable();
		return -EINVAL;
	}
	j->detached = true;
	if (j->waiter != NULL) {
		thread_ready_preempt_disabled(j->waiter);
	}
	spin_unlock(&j->lock);
	preempt_enable();
	return 0;
}

static int thread_join(struct join_handle *j, void **retval)
{
	spin_lock_np(&j->lock);
	if (j->detached) {
		spin_unlock_np(&j->lock);
		return -EINVAL;
	}
	if (j->waiter == NULL) {
		j->waiter = thread_self();
		thread_park_and_unlock_np(&j->lock);
		spin_lock_np(&j->lock);
	}
	if (retval)
		*retval = j->retval;
	spin_unlock_np(&j->lock);
	thread_ready(j->waiter);
	return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
		   void *(*start_routine)(void *), void *arg)
{
	static int (*fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *),
			 void *);
	if (unlikely(!__self || !preempt_enabled())) {
		if (!fn)
			fn = dlsym(RTLD_NEXT, "pthread_create");
		return fn(thread, attr, start_routine, arg);
	}

	return thread_spawn_joinable((struct join_handle **)thread,
				     start_routine, arg);
}

int pthread_detach(pthread_t thread)
{
	static int (*fn)(pthread_t);
	if (unlikely(!__self || !preempt_enabled())) {
		if (!fn)
			fn = dlsym(RTLD_NEXT, "pthread_detach");
		return fn(thread);
	}

	return thread_detach((struct join_handle *)thread);
}

int pthread_join(pthread_t thread, void **retval)
{
	static int (*fn)(pthread_t, void **);
	if (unlikely(!__self || !preempt_enabled())) {
		if (!fn)
			fn = dlsym(RTLD_NEXT, "pthread_join");
		return fn(thread, retval);
	}

	return thread_join((struct join_handle *)thread, retval);
}

int pthread_yield(void)
{
	static int (*fn)(void);
	if (unlikely(!__self || !preempt_enabled())) {
		if (!fn)
			fn = dlsym(RTLD_NEXT, "pthread_yield");
		return fn();
	}

	thread_yield();
	return 0;
}

int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
	const cpu_set_t *cpuset)
{
	static int (*fn)(pthread_t, size_t, const cpu_set_t *);
	if (unlikely(!__self || !preempt_enabled())) {
		if (!fn)
			fn = dlsym(RTLD_NEXT, "pthread_setaffinity_np");
		return fn(thread, cpusetsize, cpuset);
	}

	panic("pthread_setaffinity_np not supported for shenango threads");
}