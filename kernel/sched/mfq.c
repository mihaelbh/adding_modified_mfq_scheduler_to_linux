#include <linux/sched/rt.h>

#define STARVATION_COUNT	50
#define STARVATION_PERIOD_MS	500

static unsigned int get_rr_interval_mfq(struct rq *rq, struct task_struct *task);

// calculate prio from
static void calculate_prio(struct task_struct *p) {
	u64 sleep = p->se.curr_sleep_time_nsec;
	u64 exec = p->se.prev_processing_time_nsec;

	if(exec > 0 && sleep > 0) {
		p->se.prio = (u32) (CONFIG_MFQ_QUEUE_NUM * exec) / (exec + sleep);
	} else {
		p->se.prio = 0;
	}

	if(p->se.prio >= CONFIG_MFQ_QUEUE_NUM) {
		p->se.prio = CONFIG_MFQ_QUEUE_NUM - 1;
	}
}

// resets timeslice and puts task in a queue, depending on prio
static void put_in_queue(struct task_struct *p, struct rq *rq) {
	if(p->se.on_rq == 1) {
		return;
	}

	p->se.time_slice = get_rr_interval_mfq(rq, p);

	list_add_tail(&p->se.node, &rq->cfs.sched_queue[p->se.prio]);
	p->se.on_rq = 1;
	add_nr_running(rq, 1);
}

// remove task from queue
static void remove_from_queue(struct task_struct *p, struct rq *rq) {
	if(p->se.on_rq == 0) {
		return;
	}

	list_del_init(&p->se.node);
	p->se.on_rq = 0;
	sub_nr_running(rq, 1);
}

// go through rq and get the first task in highest priority non-empty queue
static struct task_struct *get_highest_prio_task(struct rq *rq) {
	int i;
	for(i=0; i<CONFIG_MFQ_QUEUE_NUM; i++) {
		if(!list_empty(&rq->cfs.sched_queue[i])) {
			break;
		}
	}

	if(i >= CONFIG_MFQ_QUEUE_NUM) {
		return NULL;
	}

	struct sched_entity *se = list_first_entry(&rq->cfs.sched_queue[i], struct sched_entity, node);
	struct task_struct *p = task_of(se);
	return p;
}

// go through queues and find task that stopped executing first
static struct task_struct *get_first_stopped_task(struct rq *rq) {
	int i;
	struct sched_entity *se = list_first_entry_or_null(&rq->cfs.sched_queue[0], struct sched_entity, node);
	struct task_struct *task1;
	if(se != NULL) {
		task1 = task_of(se);
	} else {
		task1 = NULL;
	}
	struct task_struct *task2 = NULL;

	for(i=1; i<CONFIG_MFQ_QUEUE_NUM; i++) {
		se = list_first_entry_or_null(&rq->cfs.sched_queue[i], struct sched_entity, node);
		if(se != NULL) {
			task2 = task_of(se);
		} else {
			task2 = NULL;
		}

		if(task1 != NULL && task2 != NULL) {
			if(task1->se.curr_stopped_executing_nsec > task2->se.curr_stopped_executing_nsec) {
				task1 = task2;
			}
		} else if(task1 == NULL && task2 != NULL) {
			task1 = task2;
		}
	}

	return task1;
}

// helpful functions
// ==============================================================================================================
// sched_class functions

static void enqueue_task_mfq(struct rq *rq, struct task_struct *p, int flags) {
	if(rq == NULL || p == NULL) {
		return;
	}

	if(p->se.prev_gone_from_ready_nsec != 0) {
		// enqueued before
		p->se.curr_sleep_time_nsec += sched_clock() - p->se.prev_gone_from_ready_nsec;
		calculate_prio(p);
	} else {
		// enqueued for the first time
		p->se.prio = 0;
	}

	p->se.curr_become_ready_nsec = sched_clock();
	put_in_queue(p, rq);
}

static bool dequeue_task_mfq(struct rq *rq, struct task_struct *p, int flags) {
	if(rq == NULL || p == NULL) {
		return false;
	}

	remove_from_queue(p, rq);

	p->se.prev_prio = p->se.prio;
	p->se.prev_become_ready_nsec = p->se.curr_become_ready_nsec;
	p->se.prev_gone_from_ready_nsec = sched_clock();
	p->se.prev_processing_time_nsec = p->se.curr_processing_time_nsec;
	p->se.curr_processing_time_nsec = 0;

	return true;
}

static void yield_task_mfq(struct rq *rq) {
	if(rq == NULL) {
		return;
	}

	rq->curr->se.time_slice = get_rr_interval_mfq(rq, rq->curr);
	list_move_tail(&rq->curr->se.node, &rq->cfs.sched_queue[rq->curr->se.prio]);
}

// currently it doesn't yield to task, it just yields
static bool yield_to_task_mfq(struct rq *rq, struct task_struct *p) {
	if(rq == NULL) {
		return false;
	}

	yield_task_mfq(rq);

	return true;
}

static void wakeup_preempt_mfq(struct rq *rq, struct task_struct *p, int wake_flags) {
	if(rq == NULL || p == NULL) {
		return;
	}

	if(p->sched_class != &fair_sched_class) {
		return;
	}

	if(rq->curr->sched_class == &idle_sched_class) {
		resched_curr(rq);
	}

	if(rq->curr->se.prio > p->se.prio) {
		resched_curr(rq);
	}
}

static struct task_struct *pick_task_mfq(struct rq *rq, struct rq_flags *rf) {
	if(rq == NULL) {
		return NULL;
	}

	struct task_struct *highest_prio = get_highest_prio_task(rq);

	if(rq->cfs.starvation_counter >= STARVATION_COUNT && highest_prio != NULL) {
		rq->cfs.starvation_counter = 0;

		struct task_struct *starved_task = get_first_stopped_task(rq);

		if((starved_task != NULL) && (starved_task != rq->curr) && (starved_task->se.curr_stopped_executing_nsec + (STARVATION_PERIOD_MS * 1000000) <= sched_clock())) {
			highest_prio = starved_task;
		}
	} else {
		rq->cfs.starvation_counter++;
	}

	return highest_prio;
}

static struct task_struct *pick_next_task_mfq(struct rq *rq, struct task_struct *prev, struct rq_flags *rf) {
	return pick_task_mfq(rq, rf);
}

// called when task stops executing
static void put_prev_task_mfq(struct rq *rq, struct task_struct *prev, struct task_struct *next) {
	prev->se.curr_stopped_executing_nsec = sched_clock();
	prev->se.curr_processing_time_nsec += prev->se.curr_stopped_executing_nsec - prev->se.curr_started_executing_nsec;
}

// called before task starts executing
static void set_next_task_mfq(struct rq *rq, struct task_struct *p, bool first) {
	p->se.curr_started_executing_nsec = sched_clock();
}

static int select_task_rq_mfq(struct task_struct *p, int prev_cpu, int wake_flags) {
	if(cpumask_test_cpu(prev_cpu, p->cpus_ptr)) {
		// if prev_cpu is in the cpumask then return prev_cpu
		return prev_cpu;
	}

	// else return first cpu in cpumask
	return cpumask_first(p->cpus_ptr);
}

static void migrate_task_rq_mfq(struct task_struct *p, int new_cpu) {}

static void rq_online_mfq(struct rq *rq) {}

static void rq_offline_mfq(struct rq *rq) {}

static void task_dead_mfq(struct task_struct *p) {}

static void set_cpus_allowed_mfq(struct task_struct *p, struct affinity_context *ctx) {
	set_cpus_allowed_common(p, ctx);
}

static void task_tick_mfq(struct rq *rq, struct task_struct *curr, int queued) {
	curr->se.time_slice--;
	if(curr->se.time_slice > 0) {
		return;
	}

	if(curr->se.prio < (CONFIG_MFQ_QUEUE_NUM - 1)) {
		curr->se.prio++;
	}

	curr->se.time_slice = get_rr_interval_mfq(rq, curr);
	list_move_tail(&curr->se.node, &rq->cfs.sched_queue[curr->se.prio]);

	resched_curr(rq);
}

static void task_fork_mfq(struct task_struct *p) {}

static void reweight_task_mfq(struct rq *rq, struct task_struct *p, const struct load_weight *lw) {}

static void prio_changed_mfq(struct rq *rq, struct task_struct *p, u64 oldprio) {
	if(p == rq->curr) {
		return;
	}

	if(p->prio < rq->curr->prio) {
		resched_curr(rq);
	}
}

static void switching_from_mfq(struct rq *rq, struct task_struct *p) {}

static void switched_from_mfq(struct rq *rq, struct task_struct *p) {}

static void switched_to_mfq(struct rq *rq, struct task_struct *p) {}

// calculate timeslice
// RR_timeslice is 10ms
//
// | prio	| timeslice	|
// |------------|---------------|
// | 0		| 10ms		|
// | 1		| 20ms		|
// | 2		| 30ms		|
// | ...	| ...		|
static unsigned int get_rr_interval_mfq(struct rq *rq, struct task_struct *task) {
	return (RR_TIMESLICE / 10) * (task->se.prio + 1);
}

static void update_curr_mfq(struct rq *rq) {
	struct task_struct *p = get_highest_prio_task(rq);

	if(p == NULL || rq->curr == NULL) {
		return;
	}

	if(rq->curr->sched_class == &idle_sched_class) {
		resched_curr(rq);
	}

	if(p->se.prio < rq->curr->se.prio) {
		resched_curr(rq);
	}
}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void task_change_group_mfq(struct task_struct *p) {}
#endif // CONFIG_FAIR_GROUP_SCHED

#ifdef CONFIG_SCHED_CORE
static int task_is_throttled_mfq(struct task_struct *p, int cpu) {
	return 0;
}
#endif // CONFIG_SCHED_CORE
