
#ifdef MFQ_SCHED

const struct sched_class fair_sched_class;

// TODO implement functions in sched_class + init_cfs_rq()

// copied from kernel/sched/fair.c
DEFINE_SCHED_CLASS(fair) = {
	.enqueue_task		= ,
	.dequeue_task		= ,
	.yield_task		= ,
	.yield_to_task		= ,

	.wakeup_preempt		= ,

	.pick_task		= ,
	.pick_next_task		= ,
	.put_prev_task		= ,
	.set_next_task          = ,

	.select_task_rq		= ,
	.migrate_task_rq	= ,

	.rq_online		= ,
	.rq_offline		= ,

	.task_dead		= ,
	.set_cpus_allowed	= ,

	.task_tick		= ,
	.task_fork		= ,

	.reweight_task		= ,
	.prio_changed		= ,
	.switching_from		= ,
	.switched_from		= ,
	.switched_to		= ,

	.get_rr_interval	= ,

	.update_curr		= ,

#ifdef CONFIG_FAIR_GROUP_SCHED
	.task_change_group	= ,
#endif

#ifdef CONFIG_SCHED_CORE
	.task_is_throttled	= ,
#endif

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};

#endif
