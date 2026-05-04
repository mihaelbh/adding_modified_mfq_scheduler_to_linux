#include <linux/sched/rt.h>

// TODO implement functions in sched_class

// resets timeslice and puts task in a queue depending on prio
static void put_in_queue(struct task_struct *p, struct rq *rq) {
	p->se.time_slice = RR_TIMESLICE;
	list_add_tail(&p->se.node, &rq->cfs_rq.sched_queue[p->se.prio]);
}

// remove task from queue
static void remove_from_queue(struct task_struct *p) {
	list_del_init(&p->se.node);
}

static void enqueue_task_mfq(struct rq *rq, struct task_struct *p, int flags) {
	if(rq == NULL || p == NULL) {
		return;
	}

	p->se.prio = 0;
	put_in_queue(p, rq);
}

static void dequeue_task_mfq(struct rq *rq, struct task_struct *p, int flags) {
	if(rq == NULL || p == NULL) {
		return;
	}

	remove_from_queue(p);
}

static void yield_task_mfq(struct rq *rq) {
	if(rq == NULL) {
		return;
	}

	remove_from_queue(rq->curr);
	put_in_queue(rq->curr, rq);
}

static bool yield_to_task_mfq(struct rq *rq, struct task_struct *p) {
	if(rq == NULL) {
		return;
	}

	yield_task_mfq(rq);

	if(p == NULL) {
		return;
	}

	// push the task that we are yielding to to the front of the queue
	// (but if there is a task in higher priority queue he will start first)
	list_del_init(&p->se.node);
	list_add(&p->se.node, &rq->cfs_rq.sched_queue[p->se.prio]);

	return true;
}

static void wakeup_preempt_mfq(struct rq *rq, struct task_struct *p, int wake_flags) {
	if(rq == NULL || p == NULL) {
		return;
	}

	if(p->policy != SCHED_NORMAL) {
		return;
	}

	resched_curr(rq);
}

static struct task_struct *pick_task_mfq(struct rq *rq, struct rq_flags *rf) {
	if(rq == NULL) {
		return NULL;
	}

	int i;
	for(i=0; i<4; i++) {
		if(!list_empty(&rq->cfs_rq.sched_queue[i])) {
			break;
		}
	}

	if(i > 3) {
		return NULL;
	}

	struct sched_entity *se = list_entry(rq->cfs_rq.sched_queue[i].next, struct sched_entity, node);
	struct task_struct *p = task_of(se);
}

struct task_struct *pick_next_task_mfq(struct rq *rq, struct task_struct *prev, struct rq_flags *rf) {
	return pick_task_mfq(rq, rf);
}

static void put_prev_task_mfq(struct rq *rq, struct task_struct *prev, struct task_struct *next) {}

static void set_next_task_mfq(struct rq *rq, struct task_struct *p, bool first) {}

static int select_task_rq_mfq(struct task_struct *p, int prev_cpu, int wake_flags) {
	return prev_cpu;
}

static void migrate_task_rq_mfq(struct task_struct *p, int new_cpu) {}

static void rq_online_mfq(struct rq *rq) {}

static void rq_offline_mfq(struct rq *rq) {}

static void task_dead_mfq(struct task_struct *p) {}

static void set_cpus_allowed_mfq(struct task_struct *p, struct affinity_context *ctx) {}

static void task_tick_mfq(struct rq *rq, struct task_struct *curr, int queued) {}

static void task_fork_mfq(struct task_struct *p) {}

static void reweight_task_mfq(struct rq *rq, struct task_struct *p, const struct load_weight *lw) {}

static void prio_changed_mfq(struct rq *rq, struct task_struct *p, u64 oldprio) {}

static void switching_from_mfq(struct rq *rq, struct task_struct *p) {}

static void switched_from_mfq(struct rq *rq, struct task_struct *p) {}

static void switched_to_mfq(struct rq *rq, struct task_struct *p) {}

static unsigned int get_rr_interval_mfq(struct rq *rq, struct task_struct *task) {
	return RR_TIMESLICE;
}

static void update_curr_mfq(struct rq *rq) {}

static void task_change_group_mfq(struct task_struct *p) {}

static int task_is_throttled_mfq(struct task_struct *p, int cpu) {
	return 0;
}
