#include <linux/sched/rt.h>

#define STARVATION_COUNT	50
#define STARVATION_PERIOD_MS	500
#define MIN_TIMESLICE		5

static unsigned int get_rr_interval_mfq(struct rq *rq, struct task_struct *task);
static void update_curr_mfq(struct rq *rq);
static struct task_struct *pick_next_task_mfq(struct rq *rq, struct task_struct *prev, struct rq_flags *rf);

#ifdef CONFIG_SCHED_CORE
static int task_is_throttled_mfq(struct task_struct *p, int cpu);
#endif // CONFIG_SCHED_CORE

// calculate prio
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

	list_add_tail(&p->se.node, &rq->cfs.sched_queue[p->se.prio]);
	rq->cfs.num_enqueued[p->se.prio]++;
	p->se.on_rq = 1;
	add_nr_running(rq, 1);
}

// remove task from queue
static void remove_from_queue(struct task_struct *p, struct rq *rq) {
	if(p->se.on_rq == 0) {
		return;
	}

	list_del_init(&p->se.node);
	rq->cfs.num_enqueued[p->se.prio]--;
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
// used for finding task that is most likely starved
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

// enqueues task
static void enqueue_task_mfq(struct rq *rq, struct task_struct *p, int flags) {
	if(rq == NULL || p == NULL) {
		return;
	}

	if(p->se.prev_gone_from_ready_nsec != 0) {
		// enqueued before
		p->se.curr_sleep_time_nsec = sched_clock() - p->se.prev_gone_from_ready_nsec;
		calculate_prio(p);
	} else {
		// enqueued for the first time
		p->se.prio = 0;
	}

	p->se.curr_become_ready_nsec = sched_clock();
	put_in_queue(p, rq);
}

// dequeues task
static bool dequeue_task_mfq(struct rq *rq, struct task_struct *p, int flags) {
	if(rq == NULL || p == NULL) {
		return false;
	}

	remove_from_queue(p, rq);

	p->se.prev_gone_from_ready_nsec = sched_clock();
	p->se.prev_processing_time_nsec = p->se.curr_processing_time_nsec;
	p->se.curr_processing_time_nsec = 0;

	return true;
}

// yields task
// puts current task to the end of the queue
// called from do_sched_yield() which calls schedule()
static void yield_task_mfq(struct rq *rq) {
	if(rq == NULL) {
		return;
	}

	rq->curr->se.timeslice = get_rr_interval_mfq(rq, rq->curr);
	list_move_tail(&rq->curr->se.node, &rq->cfs.sched_queue[rq->curr->se.prio]);
}

// yields to specific task
// first yields current task then sets the specified task as next
// called form yield_to() function, which calls resched_curr()
static bool yield_to_task_mfq(struct rq *rq, struct task_struct *p) {
	if(rq == NULL) {
		return false;
	}

	yield_task_mfq(rq);

	struct task_struct *next = get_highest_prio_task(rq);
	if(next == p) {
		// if the next task is the task we yield to just return
		return true;
	}

	// else move task in front of the next task
	rq->cfs.num_enqueued[p->se.prio]--;
	p->se.prio = next->se.prio;
	list_move(&p->se.node, &rq->cfs.sched_queue[p->se.prio]);
	rq->cfs.num_enqueued[p->se.prio]++;

	return true;
}

// called when a task wakes up
// check if preempt is needed
static void wakeup_preempt_mfq(struct rq *rq, struct task_struct *p, int wake_flags) {
	if(rq == NULL || p == NULL) {
		return;
	}

	if(rq->curr->se.prio > p->se.prio) {
		resched_curr(rq);
	}
}

// picks next task for execution
// used by pick_next_task_mfq()
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

// picks next task for execution
// returns result of pick_task_mfq()
static struct task_struct *pick_next_task_mfq(struct rq *rq, struct task_struct *prev, struct rq_flags *rf) {
	return pick_task_mfq(rq, rf);
}

// called when task stops executing
// remember when it stopped executing and for how long did it execute
static void put_prev_task_mfq(struct rq *rq, struct task_struct *prev, struct task_struct *next) {
	prev->se.curr_stopped_executing_nsec = sched_clock();
	prev->se.curr_processing_time_nsec += prev->se.curr_stopped_executing_nsec - prev->se.curr_started_executing_nsec;
}

// called before task starts executing
// we set the timeslice here so the execution can be more fair
// if it is set during enqueue then it could be unfair because other tasks enqueued after
// because timeslice depends on number of tasks enqueued
static void set_next_task_mfq(struct rq *rq, struct task_struct *p, bool first) {
	p->se.curr_started_executing_nsec = sched_clock();
	if(p->se.timeslice <= 0) {
		// reset timeslice only if task spent all of the previous timeslice
		p->se.timeslice = get_rr_interval_mfq(rq, p);
	}
}

// select cpu to execute the task on
// searches for cpu with lowest nr_running
// happens on wakeup/fork
// depens on rq->nr_running that also contains rr and deadline tasks, not just normal
static int select_task_rq_mfq(struct task_struct *p, int prev_cpu, int wake_flags) {
	int new_cpu = prev_cpu;
	if(!cpumask_test_cpu(prev_cpu, p->cpus_ptr)) {
		// if prev_cpu isn't in the allowed cpus, take the first allowed
		new_cpu = cpumask_first(p->cpus_ptr);
	}

	struct rq *rq = cpu_rq(new_cpu);
	int min_nr_running = rq->nr_running;

	int cpu;
	for_each_cpu(cpu, p->cpus_ptr) {

		if(!cpu_active(cpu)) {
			// skip inactive cpu
			continue;
		}

		rq = cpu_rq(cpu);

		if(rq->nr_running == 0) {
			// if cpu has no tasks select it immediately
			return cpu;
		}

		if(rq->nr_running < min_nr_running) {
			min_nr_running = rq->nr_running;
			new_cpu = cpu;
		}
	}

	return new_cpu;
}

static void migrate_task_rq_mfq(struct task_struct *p, int new_cpu) {}

static void rq_online_mfq(struct rq *rq) {}

static void rq_offline_mfq(struct rq *rq) {}

static void task_dead_mfq(struct task_struct *p) {}

// decides on which cpus the task is allowed to run
static void set_cpus_allowed_mfq(struct task_struct *p, struct affinity_context *ctx) {
	set_cpus_allowed_common(p, ctx);
}

// caled periodically with HZ frequency
// lowers timeslice
// if timeslice is 0, increase prio by 1 and requeue task and preempt
static void task_tick_mfq(struct rq *rq, struct task_struct *curr, int queued) {
	curr->se.timeslice--;
	update_curr_mfq(rq);
	if(curr->se.timeslice > 0) {
		return;
	}

	rq->cfs.num_enqueued[curr->se.prio]--;
	if(curr->se.prio < (CONFIG_MFQ_QUEUE_NUM - 1)) {
		curr->se.prio++;
	}
	list_move_tail(&curr->se.node, &rq->cfs.sched_queue[curr->se.prio]);
	rq->cfs.num_enqueued[curr->se.prio]++;

	resched_curr(rq);
}

static void task_fork_mfq(struct task_struct *p) {}

static void reweight_task_mfq(struct rq *rq, struct task_struct *p, const struct load_weight *lw) {}

// if prio is changed (p->prio that uses SCHED_RR SCHED_FIFO and SCHED_NORMAL, not p->se.prio taht uses this scheduler)
// check if preemption is needed
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
// each queue gets predefined timeslice 20ms, 40ms, 80ms, 160ms ...
// that timeslice is then equally divided between the tasks in that queue
// if the timeslice is too small set it to minimal timeslice
static unsigned int get_rr_interval_mfq(struct rq *rq, struct task_struct *task) {
	unsigned int timeslice = ((2 << task->se.prio) * 10) * HZ / 1000;
	unsigned int num_tasks = rq->cfs.num_enqueued[task->se.prio];

	if(num_tasks > 1) {
		timeslice = timeslice / num_tasks;
	}

	unsigned int min_timeslice = MIN_TIMESLICE * HZ / 1000;
	if(timeslice < min_timeslice) {
		timeslice = min_timeslice;
	}

	return timeslice;
}

// called periodically from task_tick_mfq()
// checks if timeslice of current task needs to be readjusted
// checks if preemption is needed
static void update_curr_mfq(struct rq *rq) {
	struct task_struct *p = get_highest_prio_task(rq);

	if(p == NULL || rq->curr == NULL) {
		return;
	}

	// if a bunch of tasks were enqueued since the task started executing
	// check if timeslice needs to be readjusted
	unsigned int new_timeslice = get_rr_interval_mfq(rq, rq->curr);
	if(rq->curr->se.timeslice > new_timeslice) {
		rq->curr->se.timeslice = new_timeslice;
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

#ifdef CONFIG_MFQ_SCHED

DEFINE_SCHED_CLASS(fair) = {
	.enqueue_task		= enqueue_task_mfq,
	.dequeue_task		= dequeue_task_mfq,
	.yield_task		= yield_task_mfq,
	.yield_to_task		= yield_to_task_mfq,

	.wakeup_preempt		= wakeup_preempt_mfq,

	.pick_task		= pick_task_mfq,
	.pick_next_task		= pick_next_task_mfq,
	.put_prev_task		= put_prev_task_mfq,
	.set_next_task          = set_next_task_mfq,

	.select_task_rq		= select_task_rq_mfq,
	.migrate_task_rq	= migrate_task_rq_mfq,

	.rq_online		= rq_online_mfq,
	.rq_offline		= rq_offline_mfq,

	.task_dead		= task_dead_mfq,
	.set_cpus_allowed	= set_cpus_allowed_mfq,

	.task_tick		= task_tick_mfq,
	.task_fork		= task_fork_mfq,

	.reweight_task		= reweight_task_mfq,
	.prio_changed		= prio_changed_mfq,
	.switching_from		= switching_from_mfq,
	.switched_from		= switched_from_mfq,
	.switched_to		= switched_to_mfq,

	.get_rr_interval	= get_rr_interval_mfq,

	.update_curr		= update_curr_mfq,

#ifdef CONFIG_FAIR_GROUP_SCHED
	.task_change_group	= task_change_group_mfq,
#endif

#ifdef CONFIG_SCHED_CORE
	.task_is_throttled	= task_is_throttled_mfq,
#endif

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};

#endif // CONFIG_MFQ_SCHED
