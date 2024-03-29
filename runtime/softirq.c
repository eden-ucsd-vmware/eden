/*
 * softirq.c - handles high priority events (timers, ingress packets, etc.)
 */

#include <base/stddef.h>
#include <base/log.h>
#include <runtime/thread.h>
#include <runtime/pgfault.h>

#include "defs.h"
#include "net/defs.h"

struct softirq_work {
	unsigned int recv_cnt, compl_cnt, join_cnt, timer_budget;
	struct kthread *k;
	struct rx_net_hdr *recv_reqs[SOFTIRQ_MAX_BUDGET];
	struct mbuf *compl_reqs[SOFTIRQ_MAX_BUDGET];
	struct kthread *join_reqs[SOFTIRQ_MAX_BUDGET];
};

static void softirq_fn(void *arg)
{
	struct softirq_work *w = arg;
	int i;

	/* complete TX requests and free packets */
	for (i = 0; i < w->compl_cnt; i++)
		mbuf_free(w->compl_reqs[i]);

	/* deliver new RX packets to the runtime */
	net_rx_softirq(w->recv_reqs, w->recv_cnt);

	/* handle any pending timeouts */
	if (timer_needed(w->k))
		timer_softirq(w->k, w->timer_budget);

	/* join parked kthreads */
	for (i = 0; i < w->join_cnt; i++)
		join_kthread(w->join_reqs[i]);
}

static void softirq_gather_work(struct softirq_work *w, struct kthread *k,
				unsigned int budget)
{
	unsigned int recv_cnt = 0, compl_cnt = 0, join_cnt = 0;
	int budget_left, local_budget_left;

	budget_left = MIN(budget, SOFTIRQ_MAX_BUDGET);
	local_budget_left = SOFTIRQ_MAX_BUDGET;

	if (rmem_enabled) {
		/* REMOTE MEMORY TODO: Is this really needed? */
		/* supress new work when it gets congested. NOTE(TODO): we assume that 
		* supressing RX means supressing new work, which is not always true. Also, 
		* we're supressing other events for IO core like TX completions due to the 
		* shared events queue, which may cause trouble in some cases. */
		// int active_threads = (k->rq_head - k->rq_tail) + k->rq_overflow_len; 
		// local_budget_left = CONGESTION_THRESHOLD - (active_threads + k->pf_pending);
	}

	while (budget_left > 0 && local_budget_left > 0) {
		budget_left--; local_budget_left--;

		uint64_t cmd;
		unsigned long payload;

		if (!lrpc_recv(&k->rxq, &cmd, &payload))
			break;

		switch (cmd) {
		case RX_NET_RECV:
			w->recv_reqs[recv_cnt] = shmptr_to_ptr(
				&netcfg.rx_region,
				(shmptr_t)payload, MBUF_DEFAULT_LEN);
			BUG_ON(w->recv_reqs[recv_cnt] == NULL);
			recv_cnt++;
			break;

		case RX_NET_COMPLETE:
			w->compl_reqs[compl_cnt++] = (struct mbuf *)payload;
			break;

		case RX_JOIN:
			w->join_reqs[join_cnt++] = (struct kthread *)payload;
			break;

		default:
			log_err_ratelimited("net: invalid RXQ cmd '%ld'", cmd);
		}
	}

	w->k = k;
	w->recv_cnt = recv_cnt;
	w->compl_cnt = compl_cnt;
	w->join_cnt = join_cnt;
	w->timer_budget = budget_left;
}


/**
 * softirq_ready - checks if there are any softirqs ready to be handled
 * @k: the kthread from which to take RX queue commands
 */
static inline bool softirq_ready(struct kthread* k) 
{
	return timer_needed(k) || !lrpc_empty(&k->rxq);
}

/**
 * softirq_run_thread - creates a closure for softirq handling
 * @k: the kthread from which to take RX queue commands
 * @budget: the maximum number of events to process
 *
 * Returns a thread that handles receive processing when executed or
 * NULL if no receive processing work is available.
 */
thread_t *softirq_run_thread(struct kthread *k, unsigned int budget)
{
	thread_t *th;
	struct softirq_work *w;

	assert_spin_lock_held(&k->lock);

	/* check if there's any work available */
	if (!softirq_ready(k))
		return NULL;

	th = thread_create_with_buf(softirq_fn, (void **)&w, sizeof(*w));
	if (unlikely(!th))
		return NULL;

	softirq_gather_work(w, k, budget);
	th->state = THREAD_STATE_RUNNABLE;
	return th;
}

/**
 * softirq_run - handles softirq processing in the current thread
 * @budget: the maximum number of events to process
 */
void softirq_run(unsigned int budget)
{
	struct kthread *k;
	struct softirq_work w;

	/* check if there's any work available */
	k = getk();
	if (!softirq_ready(k)){
		putk();
		return;
	}

	spin_lock(&k->lock);
	softirq_gather_work(&w, k, budget);
	spin_unlock(&k->lock);
	putk();

	softirq_fn(&w);
}
