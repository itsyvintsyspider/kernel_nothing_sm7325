// SPDX-License-Identifier: GPL-2.0
/*
 * Completely Fair Scheduling (CFS) Class (SCHED_NORMAL/SCHED_BATCH)
 *
 *  Copyright (C) 2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *
 *  Interactivity improvements by Mike Galbraith
 *  (C) 2007 Mike Galbraith <efault@gmx.de>
 *
 *  Various enhancements by Dmitry Adamushko.
 *  (C) 2007 Dmitry Adamushko <dmitry.adamushko@gmail.com>
 *
 *  Group scheduling enhancements by Srivatsa Vaddagiri
 *  Copyright IBM Corporation, 2007
 *  Author: Srivatsa Vaddagiri <vatsa@linux.vnet.ibm.com>
 *
 *  Scaled math optimizations by Thomas Gleixner
 *  Copyright (C) 2007, Thomas Gleixner <tglx@linutronix.de>
 *
 *  Adaptive scheduling granularity, math enhancements by Peter Zijlstra
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra
 */
#include "sched.h"

#include <trace/events/sched.h>
#include <trace/hooks/sched.h>

#include "walt/walt.h"

#ifdef CONFIG_SMP
static inline bool task_fits_max(struct task_struct *p, int cpu);
#endif /* CONFIG_SMP */
static inline unsigned long _task_util_est(struct task_struct *p)
{
	struct util_est ue = READ_ONCE(p->se.avg.util_est);

	return (max(ue.ewma, ue.enqueued) | UTIL_AVG_UNCHANGED);
}

static inline unsigned long task_util_est(struct task_struct *p)
{
#ifdef CONFIG_SCHED_WALT
	return p->wts.demand_scaled;
#endif
	return max(task_util(p), _task_util_est(p));
}

#ifdef CONFIG_UCLAMP_TASK
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
	return clamp(task_util_est(p),
		     uclamp_eff_value(p, UCLAMP_MIN),
		     uclamp_eff_value(p, UCLAMP_MAX));
}
#else
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
	return task_util_est(p);
}
#endif

static inline void util_est_enqueue(struct cfs_rq *cfs_rq,
				    struct task_struct *p)
{
	unsigned int enqueued;

	if (!sched_feat(UTIL_EST))
		return;

	/* Update root cfs_rq's estimated utilization */
	enqueued  = cfs_rq->avg.util_est.enqueued;
	enqueued += _task_util_est(p);
	WRITE_ONCE(cfs_rq->avg.util_est.enqueued, enqueued);
}

/*
 * Check if a (signed) value is within a specified (unsigned) margin,
 * based on the observation that:
 *
 *     abs(x) < y := (unsigned)(x + y - 1) < (2 * y - 1)
 *
 * NOTE: this only works when value + maring < INT_MAX.
 */
static inline bool within_margin(int value, int margin)
{
	return ((unsigned int)(value + margin - 1) < (2 * margin - 1));
}

static void
util_est_dequeue(struct cfs_rq *cfs_rq, struct task_struct *p, bool task_sleep)
{
	long last_ewma_diff;
	struct util_est ue;
	int cpu;

	if (!sched_feat(UTIL_EST))
		return;

	/* Update root cfs_rq's estimated utilization */
	ue.enqueued  = cfs_rq->avg.util_est.enqueued;
	ue.enqueued -= min_t(unsigned int, ue.enqueued, _task_util_est(p));
	WRITE_ONCE(cfs_rq->avg.util_est.enqueued, ue.enqueued);

	/*
	 * Skip update of task's estimated utilization when the task has not
	 * yet completed an activation, e.g. being migrated.
	 */
	if (!task_sleep)
		return;

	/*
	 * If the PELT values haven't changed since enqueue time,
	 * skip the util_est update.
	 */
	ue = p->se.avg.util_est;
	if (ue.enqueued & UTIL_AVG_UNCHANGED)
		return;

	/*
	 * Reset EWMA on utilization increases, the moving average is used only
	 * to smooth utilization decreases.
	 */
	ue.enqueued = (task_util(p) | UTIL_AVG_UNCHANGED);
	if (sched_feat(UTIL_EST_FASTUP)) {
		if (ue.ewma < ue.enqueued) {
			ue.ewma = ue.enqueued;
			goto done;
		}
	}

	/*
	 * Skip update of task's estimated utilization when its EWMA is
	 * already ~1% close to its last activation value.
	 */
	last_ewma_diff = ue.enqueued - ue.ewma;
	if (within_margin(last_ewma_diff, (SCHED_CAPACITY_SCALE / 100)))
		return;

	/*
	 * To avoid overestimation of actual task utilization, skip updates if
	 * we cannot grant there is idle time in this CPU.
	 */
	cpu = cpu_of(rq_of(cfs_rq));
	if (task_util(p) > capacity_orig_of(cpu))
		return;

	/*
	 * Update Task's estimated utilization
	 *
	 * When *p completes an activation we can consolidate another sample
	 * of the task size. This is done by storing the current PELT value
	 * as ue.enqueued and by using this value to update the Exponential
	 * Weighted Moving Average (EWMA):
	 *
	 *  ewma(t) = w *  task_util(p) + (1-w) * ewma(t-1)
	 *          = w *  task_util(p) +         ewma(t-1)  - w * ewma(t-1)
	 *          = w * (task_util(p) -         ewma(t-1)) +     ewma(t-1)
	 *          = w * (      last_ewma_diff            ) +     ewma(t-1)
	 *          = w * (last_ewma_diff  +  ewma(t-1) / w)
	 *
	 * Where 'w' is the weight of new samples, which is configured to be
	 * 0.25, thus making w=1/4 ( >>= UTIL_EST_WEIGHT_SHIFT)
	 */
	ue.ewma <<= UTIL_EST_WEIGHT_SHIFT;
	ue.ewma  += last_ewma_diff;
	ue.ewma >>= UTIL_EST_WEIGHT_SHIFT;
done:
	WRITE_ONCE(p->se.avg.util_est, ue);
}

static inline bool
bias_to_this_cpu(struct task_struct *p, int cpu, int start_cpu)
{
	bool base_test = cpumask_test_cpu(cpu, &p->cpus_mask) &&
						cpu_active(cpu);
	bool start_cap_test = (capacity_orig_of(cpu) >=
					capacity_orig_of(start_cpu));

	return base_test && start_cap_test;
}

static inline bool task_fits_capacity(struct task_struct *p,
					long capacity,
					int cpu)
{
	unsigned int margin;

	/*
	 * Derive upmigration/downmigrate margin wrt the src/dest CPU.
	 */
	if (capacity_orig_of(task_cpu(p)) > capacity_orig_of(cpu))
		margin = sched_capacity_margin_down[cpu];
	else
		margin = sched_capacity_margin_up[task_cpu(p)];

	return capacity * 1024 > uclamp_task_util(p) * margin;
}

static inline bool task_fits_max(struct task_struct *p, int cpu)
{
	unsigned long capacity = capacity_orig_of(cpu);
	unsigned long max_capacity = cpu_rq(cpu)->rd->max_cpu_capacity.val;
	unsigned long task_boost = per_task_boost(p);

	if (capacity == max_capacity)
		return true;

	if (is_min_capacity_cpu(cpu)) {
		if (task_boost_policy(p) == SCHED_BOOST_ON_BIG ||
				task_boost > 0 ||
				uclamp_boosted(p) ||
				walt_should_kick_upmigrate(p, cpu))
			return false;
	} else { /* mid cap cpu */
		if (task_boost > TASK_BOOST_ON_MID)
			return false;
	}

	return task_fits_capacity(p, capacity, cpu);
}

static inline bool task_demand_fits(struct task_struct *p, int cpu)
{
	unsigned long capacity = capacity_orig_of(cpu);
	unsigned long max_capacity = cpu_rq(cpu)->rd->max_cpu_capacity.val;

	if (capacity == max_capacity)
		return true;

	return task_fits_capacity(p, capacity, cpu);
}

struct find_best_target_env {
	bool is_rtg;
	int need_idle;
	bool boosted;
	int fastpath;
	int start_cpu;
	int order_index;
	int end_index;
	bool strict_max;
	int skip_cpu;
};

static inline bool prefer_spread_on_idle(int cpu, bool new_ilb)
{
#ifdef CONFIG_SCHED_WALT
	switch (sysctl_sched_prefer_spread) {
	case 1:
		return is_min_capacity_cpu(cpu);
	case 2:
		return true;
	case 3:
		return (new_ilb && is_min_capacity_cpu(cpu));
	case 4:
		return new_ilb;
	default:
		return false;
	}
#else
	return false;
#endif
}

#ifdef CONFIG_SCHED_WALT
static inline void walt_adjust_cpus_for_packing(struct task_struct *p,
				int *target_cpu, int *best_idle_cpu,
				int shallowest_idle_cstate,
				struct find_best_target_env *fbt_env)
{
	unsigned long tutil, estimated_capacity;

	if (*best_idle_cpu == -1 || *target_cpu == -1)
		return;

	if (prefer_spread_on_idle(*best_idle_cpu, false))
		fbt_env->need_idle |= 2;

	if (task_rtg_high_prio(p) && walt_nr_rtg_high_prio(*target_cpu) > 0) {
		*target_cpu = -1;
		return;
	}


	if (fbt_env->need_idle || task_placement_boost_enabled(p) ||
		fbt_env->boosted || shallowest_idle_cstate <= 0) {
		*target_cpu = -1;
		return;
	}

	if (task_in_cum_window_demand(cpu_rq(*target_cpu), p))
		tutil = 0;
	else
		tutil = task_util(p);

	estimated_capacity = cpu_util_cum(*target_cpu, tutil);
	estimated_capacity = add_capacity_margin(estimated_capacity,
							*target_cpu);

	/*
	 * If there is only one active CPU and it is already above its current
	 * capacity, avoid placing additional task on the CPU.
	 */
	if (estimated_capacity > capacity_curr_of(*target_cpu)) {
		*target_cpu = -1;
		return;
	}

	if (fbt_env->is_rtg)
		*best_idle_cpu = -1;
}
#endif

static inline void update_misfit_status(struct task_struct *p, struct rq *rq)
{
	if (!static_branch_unlikely(&sched_asym_cpucapacity))
		return;

	if (!p || p->nr_cpus_allowed == 1) {
		rq->misfit_task_load = 0;
		return;
	}

	if (task_fits_max(p, cpu_of(rq))) {
		rq->misfit_task_load = 0;
		return;
	}

	/*
	 * Make sure that misfit_task_load will not be null even if
	 * task_h_load() returns 0.
	 */
	rq->misfit_task_load = max_t(unsigned long, task_h_load(p), 1);
}

#else /* CONFIG_SMP */

#define UPDATE_TG	0x0
#define SKIP_AGE_LOAD	0x0
#define DO_ATTACH	0x0

static inline void update_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int not_used1)
{
	cfs_rq_util_change(cfs_rq, 0);
}

static inline void remove_entity_load_avg(struct sched_entity *se) {}

static inline void
attach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags) {}
static inline void
detach_entity_load_avg(struct cfs_rq *cfs_rq, struct sched_entity *se) {}

static inline int sched_balance_newidle(struct rq *rq, struct rq_flags *rf)
{
	return 0;
}

static inline void
util_est_enqueue(struct cfs_rq *cfs_rq, struct task_struct *p) {}

static inline void
util_est_dequeue(struct cfs_rq *cfs_rq, struct task_struct *p,
		 bool task_sleep) {}
static inline void update_misfit_status(struct task_struct *p, struct rq *rq) {}

#endif /* CONFIG_SMP */

static void check_spread(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
#ifdef CONFIG_SCHED_DEBUG
	s64 d = se->vruntime - cfs_rq->min_vruntime;

	if (d < 0)
		d = -d;

	if (d > 3*sysctl_sched_latency)
		schedstat_inc(cfs_rq->nr_spread_over);
#endif
}

static inline bool entity_is_long_sleeper(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq;
	u64 sleep_time;

	if (se->exec_start == 0)
		return false;

	cfs_rq = cfs_rq_of(se);

	sleep_time = rq_clock_task(rq_of(cfs_rq));

	/* Happen while migrating because of clock task divergence */
	if (sleep_time <= se->exec_start)
		return false;

	sleep_time -= se->exec_start;
	if (sleep_time > ((1ULL << 63) / scale_load_down(NICE_0_LOAD)))
		return true;

	return false;
}

static void
place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int initial)
{
	u64 vruntime = cfs_rq->min_vruntime;

	/*
	 * The 'current' period is already promised to the current tasks,
	 * however the extra weight of the new task will slow them down a
	 * little, place the new task so that it fits in the slot that
	 * stays open at the end.
	 */
	if (initial && sched_feat(START_DEBIT))
		vruntime += sched_vslice(cfs_rq, se);

	/* sleeps up to a single latency don't count. */
	if (!initial) {
		unsigned long thresh = sysctl_sched_latency;

		/*
		 * Halve their sleep time's effect, to allow
		 * for a gentler effect of sleepers:
		 */
		if (sched_feat(GENTLE_FAIR_SLEEPERS))
			thresh >>= 1;

		vruntime -= thresh;
#ifdef CONFIG_SCHED_WALT
		if (entity_is_task(se)) {
			if (per_task_boost(task_of(se)) == TASK_BOOST_STRICT_MAX) {
				vruntime -= thresh;
				vruntime -= sysctl_sched_latency;
				se->vruntime = vruntime;
				return;
			} else if (walt_binder_low_latency_task(task_of(se))) {
				vruntime -= sysctl_sched_latency;
				se->vruntime = vruntime;
				return;
			} else if (task_rtg_high_prio(task_of(se)) ||
					walt_procfs_low_latency_task(task_of(se))) {
				vruntime -= thresh;
				se->vruntime = vruntime;
				return;
			}
		}
#endif
	}

	/*
	 * Pull vruntime of the entity being placed to the base level of
	 * cfs_rq, to prevent boosting it if placed backwards.
	 * However, min_vruntime can advance much faster than real time, with
	 * the extreme being when an entity with the minimal weight always runs
	 * on the cfs_rq. If the waking entity slept for a long time, its
	 * vruntime difference from min_vruntime may overflow s64 and their
	 * comparison may get inversed, so ignore the entity's original
	 * vruntime in that case.
	 * The maximal vruntime speedup is given by the ratio of normal to
	 * minimal weight: scale_load_down(NICE_0_LOAD) / MIN_SHARES.
	 * When placing a migrated waking entity, its exec_start has been set
	 * from a different rq. In order to take into account a possible
	 * divergence between new and prev rq's clocks task because of irq and
	 * stolen time, we take an additional margin.
	 * So, cutting off on the sleep time of
	 *     2^63 / scale_load_down(NICE_0_LOAD) ~ 104 days
	 * should be safe.
	 */
	if (entity_is_long_sleeper(se))
		se->vruntime = vruntime;
	else
		se->vruntime = max_vruntime(se->vruntime, vruntime);
}

static void check_enqueue_throttle(struct cfs_rq *cfs_rq);

static inline void check_schedstat_required(void)
{
#ifdef CONFIG_SCHEDSTATS
	if (schedstat_enabled())
		return;

	/* Force schedstat enabled if a dependent tracepoint is active */
	if (trace_sched_stat_wait_enabled()    ||
			trace_sched_stat_sleep_enabled()   ||
			trace_sched_stat_iowait_enabled()  ||
			trace_sched_stat_blocked_enabled() ||
			trace_sched_stat_runtime_enabled())  {
		printk_deferred_once("Scheduler tracepoints stat_sleep, stat_iowait, "
			     "stat_blocked and stat_runtime require the "
			     "kernel parameter schedstats=enable or "
			     "kernel.sched_schedstats=1\n");
	}
#endif
}

static inline bool cfs_bandwidth_used(void);

/*
 * MIGRATION
 *
 *	dequeue
 *	  update_curr()
 *	    update_min_vruntime()
 *	  vruntime -= min_vruntime
 *
 *	enqueue
 *	  update_curr()
 *	    update_min_vruntime()
 *	  vruntime += min_vruntime
 *
 * this way the vruntime transition between RQs is done when both
 * min_vruntime are up-to-date.
 *
 * WAKEUP (remote)
 *
 *	->migrate_task_rq_fair() (p->state == TASK_WAKING)
 *	  vruntime -= min_vruntime
 *
 *	enqueue
 *	  update_curr()
 *	    update_min_vruntime()
 *	  vruntime += min_vruntime
 *
 * this way we don't have the most up-to-date min_vruntime on the originating
 * CPU and an up-to-date min_vruntime on the destination CPU.
 */

static void
enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	bool renorm = !(flags & ENQUEUE_WAKEUP) || (flags & ENQUEUE_MIGRATED);
	bool curr = cfs_rq->curr == se;

	/*
	 * If we're the current task, we must renormalise before calling
	 * update_curr().
	 */
	if (renorm && curr)
		se->vruntime += cfs_rq->min_vruntime;

	update_curr(cfs_rq);

	/*
	 * Otherwise, renormalise after, such that we're placed at the current
	 * moment in time, instead of some random moment in the past. Being
	 * placed in the past could significantly boost this task to the
	 * fairness detriment of existing tasks.
	 */
	if (renorm && !curr)
		se->vruntime += cfs_rq->min_vruntime;

	/*
	 * When enqueuing a sched_entity, we must:
	 *   - Update loads to have both entity and cfs_rq synced with now.
	 *   - Add its load to cfs_rq->runnable_avg
	 *   - For group_entity, update its weight to reflect the new share of
	 *     its group cfs_rq
	 *   - Add its new weight to cfs_rq->load.weight
	 */
	update_load_avg(cfs_rq, se, UPDATE_TG | DO_ATTACH);
	update_cfs_group(se);
	enqueue_runnable_load_avg(cfs_rq, se);
	account_entity_enqueue(cfs_rq, se);

	if (flags & ENQUEUE_WAKEUP)
		place_entity(cfs_rq, se, 0);
	/* Entity has migrated, no longer consider this task hot */
	if (flags & ENQUEUE_MIGRATED)
		se->exec_start = 0;

	check_schedstat_required();
	update_stats_enqueue(cfs_rq, se, flags);
	check_spread(cfs_rq, se);
	if (!curr)
		__enqueue_entity(cfs_rq, se);
	se->on_rq = 1;

	/*
	 * When bandwidth control is enabled, cfs might have been removed
	 * because of a parent been throttled but cfs->nr_running > 1. Try to
	 * add it unconditionnally.
	 */
	if (cfs_rq->nr_running == 1 || cfs_bandwidth_used())
		list_add_leaf_cfs_rq(cfs_rq);

	if (cfs_rq->nr_running == 1)
		check_enqueue_throttle(cfs_rq);
}

static void __clear_buddies_last(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->last != se)
			break;

		cfs_rq->last = NULL;
	}
}

static void __clear_buddies_next(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->next != se)
			break;

		cfs_rq->next = NULL;
	}
}

static void __clear_buddies_skip(struct sched_entity *se)
{
	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		if (cfs_rq->skip != se)
			break;

		cfs_rq->skip = NULL;
	}
}

static void clear_buddies(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if (cfs_rq->last == se)
		__clear_buddies_last(se);

	if (cfs_rq->next == se)
		__clear_buddies_next(se);

	if (cfs_rq->skip == se)
		__clear_buddies_skip(se);
}

static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq);

static void
dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se, int flags)
{
	/*
	 * Update run-time statistics of the 'current'.
	 */
	update_curr(cfs_rq);

	/*
	 * When dequeuing a sched_entity, we must:
	 *   - Update loads to have both entity and cfs_rq synced with now.
	 *   - Subtract its load from the cfs_rq->runnable_avg.
	 *   - Subtract its previous weight from cfs_rq->load.weight.
	 *   - For group entity, update its weight to reflect the new share
	 *     of its group cfs_rq.
	 */
	update_load_avg(cfs_rq, se, UPDATE_TG);
	dequeue_runnable_load_avg(cfs_rq, se);

	update_stats_dequeue(cfs_rq, se, flags);

	clear_buddies(cfs_rq, se);

	if (se != cfs_rq->curr)
		__dequeue_entity(cfs_rq, se);
	se->on_rq = 0;
	account_entity_dequeue(cfs_rq, se);

	/*
	 * Normalize after update_curr(); which will also have moved
	 * min_vruntime if @se is the one holding it back. But before doing
	 * update_min_vruntime() again, which will discount @se's position and
	 * can move min_vruntime forward still more.
	 */
	if (!(flags & DEQUEUE_SLEEP))
		se->vruntime -= cfs_rq->min_vruntime;

	/* return excess runtime on last dequeue */
	return_cfs_rq_runtime(cfs_rq);

	update_cfs_group(se);

	/*
	 * Now advance min_vruntime if @se was the entity holding it back,
	 * except when: DEQUEUE_SAVE && !DEQUEUE_MOVE, in this case we'll be
	 * put back on, and if we advance min_vruntime, we'll be placed back
	 * further than we started -- ie. we'll be penalized.
	 */
	if ((flags & (DEQUEUE_SAVE | DEQUEUE_MOVE)) != DEQUEUE_SAVE)
		update_min_vruntime(cfs_rq);
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void
check_preempt_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	unsigned long ideal_runtime, delta_exec;
	struct sched_entity *se;
	s64 delta;

	ideal_runtime = sched_slice(cfs_rq, curr);
	delta_exec = curr->sum_exec_runtime - curr->prev_sum_exec_runtime;
	if (delta_exec > ideal_runtime) {
		resched_curr(rq_of(cfs_rq));
		/*
		 * The current task ran long enough, ensure it doesn't get
		 * re-elected due to buddy favours.
		 */
		clear_buddies(cfs_rq, curr);
		return;
	}

	/*
	 * Ensure that a task that missed wakeup preemption by a
	 * narrow margin doesn't have to wait for a full slice.
	 * This also mitigates buddy induced latencies under load.
	 */
	if (delta_exec < sysctl_sched_min_granularity)
		return;

	se = __pick_first_entity(cfs_rq);
	delta = curr->vruntime - se->vruntime;

	if (delta < 0)
		return;

	if (delta > ideal_runtime)
		resched_curr(rq_of(cfs_rq));
}

static void
set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	/* 'current' is not kept within the tree. */
	if (se->on_rq) {
		/*
		 * Any task has to be enqueued before it get to execute on
		 * a CPU. So account for the time it spent waiting on the
		 * runqueue.
		 */
		update_stats_wait_end(cfs_rq, se);
		__dequeue_entity(cfs_rq, se);
		update_load_avg(cfs_rq, se, UPDATE_TG);
	}

	update_stats_curr_start(cfs_rq, se);
	cfs_rq->curr = se;

	/*
	 * Track our maximum slice length, if the CPU's load is at
	 * least twice that of our own weight (i.e. dont track it
	 * when there are only lesser-weight tasks around):
	 */
	if (schedstat_enabled() &&
	    rq_of(cfs_rq)->cfs.load.weight >= 2*se->load.weight) {
		schedstat_set(se->statistics.slice_max,
			max((u64)schedstat_val(se->statistics.slice_max),
			    se->sum_exec_runtime - se->prev_sum_exec_runtime));
	}

	se->prev_sum_exec_runtime = se->sum_exec_runtime;
}

static int
wakeup_preempt_entity(struct sched_entity *curr, struct sched_entity *se);

/*
 * Pick the next process, keeping these things in mind, in this order:
 * 1) keep things fair between processes/task groups
 * 2) pick the "next" process, since someone really wants that to run
 * 3) pick the "last" process, for cache locality
 * 4) do not run the "skip" process, if something else is available
 */
static struct sched_entity *
pick_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *curr)
{
	struct sched_entity *left = __pick_first_entity(cfs_rq);
	struct sched_entity *se;

	/*
	 * If curr is set we have to see if its left of the leftmost entity
	 * still in the tree, provided there was anything in the tree at all.
	 */
	if (!left || (curr && entity_before(curr, left)))
		left = curr;

	se = left; /* ideally we run the leftmost entity */

	/*
	 * Avoid running the skip buddy, if running something else can
	 * be done without getting too unfair.
	 */
	if (cfs_rq->skip == se) {
		struct sched_entity *second;

		if (se == curr) {
			second = __pick_first_entity(cfs_rq);
		} else {
			second = __pick_next_entity(se);
			if (!second || (curr && entity_before(curr, second)))
				second = curr;
		}

		if (second && wakeup_preempt_entity(second, left) < 1)
			se = second;
	}

	/*
	 * Prefer last buddy, try to return the CPU to a preempted task.
	 */
	if (cfs_rq->last && wakeup_preempt_entity(cfs_rq->last, left) < 1)
		se = cfs_rq->last;

	/*
	 * Someone really wants this to run. If it's not unfair, run it.
	 */
	if (cfs_rq->next && wakeup_preempt_entity(cfs_rq->next, left) < 1)
		se = cfs_rq->next;

	clear_buddies(cfs_rq, se);

	return se;
}

static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq);

static void put_prev_entity(struct cfs_rq *cfs_rq, struct sched_entity *prev)
{
	/*
	 * If still on the runqueue then deactivate_task()
	 * was not called and update_curr() has to be done:
	 */
	if (prev->on_rq)
		update_curr(cfs_rq);

	/* throttle cfs_rqs exceeding runtime */
	check_cfs_rq_runtime(cfs_rq);

	check_spread(cfs_rq, prev);

	if (prev->on_rq) {
		update_stats_wait_start(cfs_rq, prev);
		/* Put 'current' back into the tree. */
		__enqueue_entity(cfs_rq, prev);
		/* in !on_rq case, update occurred at dequeue */
		update_load_avg(cfs_rq, prev, 0);
	}
	cfs_rq->curr = NULL;
}

static void
entity_tick(struct cfs_rq *cfs_rq, struct sched_entity *curr, int queued)
{
	/*
	 * Update run-time statistics of the 'current'.
	 */
	update_curr(cfs_rq);

	/*
	 * Ensure that runnable average is periodically updated.
	 */
	update_load_avg(cfs_rq, curr, UPDATE_TG);
	update_cfs_group(curr);

#ifdef CONFIG_SCHED_HRTICK
	/*
	 * queued ticks are scheduled to match the slice, so don't bother
	 * validating it and just reschedule.
	 */
	if (queued) {
		resched_curr(rq_of(cfs_rq));
		return;
	}
	/*
	 * don't let the period tick interfere with the hrtick preemption
	 */
	if (!sched_feat(DOUBLE_TICK) &&
			hrtimer_active(&rq_of(cfs_rq)->hrtick_timer))
		return;
#endif

	if (cfs_rq->nr_running > 1)
		check_preempt_tick(cfs_rq, curr);
}


/**************************************************
 * CFS bandwidth control machinery
 */

#ifdef CONFIG_CFS_BANDWIDTH

#ifdef CONFIG_JUMP_LABEL
static struct static_key __cfs_bandwidth_used;

static inline bool cfs_bandwidth_used(void)
{
	return static_key_false(&__cfs_bandwidth_used);
}

void cfs_bandwidth_usage_inc(void)
{
	static_key_slow_inc_cpuslocked(&__cfs_bandwidth_used);
}

void cfs_bandwidth_usage_dec(void)
{
	static_key_slow_dec_cpuslocked(&__cfs_bandwidth_used);
}
#else /* CONFIG_JUMP_LABEL */
static bool cfs_bandwidth_used(void)
{
	return true;
}

void cfs_bandwidth_usage_inc(void) {}
void cfs_bandwidth_usage_dec(void) {}
#endif /* CONFIG_JUMP_LABEL */

/*
 * default period for cfs group bandwidth.
 * default: 0.1s, units: nanoseconds
 */
static inline u64 default_cfs_period(void)
{
	return 100000000ULL;
}

static inline u64 sched_cfs_bandwidth_slice(void)
{
	return (u64)sysctl_sched_cfs_bandwidth_slice * NSEC_PER_USEC;
}

/*
 * Replenish runtime according to assigned quota. We use sched_clock_cpu
 * directly instead of rq->clock to avoid adding additional synchronization
 * around rq->lock.
 *
 * requires cfs_b->lock
 */
void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b)
{
	if (cfs_b->quota != RUNTIME_INF)
		cfs_b->runtime = cfs_b->quota;
}

static inline struct cfs_bandwidth *tg_cfs_bandwidth(struct task_group *tg)
{
	return &tg->cfs_bandwidth;
}

/* returns 0 on failure to allocate runtime */
static int __assign_cfs_rq_runtime(struct cfs_bandwidth *cfs_b,
				   struct cfs_rq *cfs_rq, u64 target_runtime)
{
	u64 min_amount, amount = 0;

	lockdep_assert_held(&cfs_b->lock);

	/* note: this is a positive sum as runtime_remaining <= 0 */
	min_amount = target_runtime - cfs_rq->runtime_remaining;

	if (cfs_b->quota == RUNTIME_INF)
		amount = min_amount;
	else {
		start_cfs_bandwidth(cfs_b);

		if (cfs_b->runtime > 0) {
			amount = min(cfs_b->runtime, min_amount);
			cfs_b->runtime -= amount;
			cfs_b->idle = 0;
		}
	}

	cfs_rq->runtime_remaining += amount;

	return cfs_rq->runtime_remaining > 0;
}

/* returns 0 on failure to allocate runtime */
static int assign_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	int ret;

	raw_spin_lock(&cfs_b->lock);
	ret = __assign_cfs_rq_runtime(cfs_b, cfs_rq, sched_cfs_bandwidth_slice());
	raw_spin_unlock(&cfs_b->lock);

	return ret;
}

static void __account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
	/* dock delta_exec before expiring quota (as it could span periods) */
	cfs_rq->runtime_remaining -= delta_exec;

	if (likely(cfs_rq->runtime_remaining > 0))
		return;

	if (cfs_rq->throttled)
		return;
	/*
	 * if we're unable to extend our runtime we resched so that the active
	 * hierarchy can be throttled
	 */
	if (!assign_cfs_rq_runtime(cfs_rq) && likely(cfs_rq->curr))
		resched_curr(rq_of(cfs_rq));
}

static __always_inline
void account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec)
{
	if (!cfs_bandwidth_used() || !cfs_rq->runtime_enabled)
		return;

	__account_cfs_rq_runtime(cfs_rq, delta_exec);
}

static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->throttled;
}

/* check whether cfs_rq, or any parent, is throttled */
static inline int throttled_hierarchy(struct cfs_rq *cfs_rq)
{
	return cfs_bandwidth_used() && cfs_rq->throttle_count;
}

/*
 * Ensure that neither of the group entities corresponding to src_cpu or
 * dest_cpu are members of a throttled hierarchy when performing group
 * load-balance operations.
 */
static inline int throttled_lb_pair(struct task_group *tg,
				    int src_cpu, int dest_cpu)
{
	struct cfs_rq *src_cfs_rq, *dest_cfs_rq;

	src_cfs_rq = tg->cfs_rq[src_cpu];
	dest_cfs_rq = tg->cfs_rq[dest_cpu];

	return throttled_hierarchy(src_cfs_rq) ||
	       throttled_hierarchy(dest_cfs_rq);
}

static int tg_unthrottle_up(struct task_group *tg, void *data)
{
	struct rq *rq = data;
	struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

	cfs_rq->throttle_count--;
	if (!cfs_rq->throttle_count) {
		cfs_rq->throttled_clock_pelt_time += rq_clock_pelt(rq) -
					     cfs_rq->throttled_clock_pelt;

		/* Add cfs_rq with already running entity in the list */
		if (cfs_rq->nr_running >= 1)
			list_add_leaf_cfs_rq(cfs_rq);
	}

	return 0;
}

static int tg_throttle_down(struct task_group *tg, void *data)
{
	struct rq *rq = data;
	struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

	/* group is entering throttled state, stop time */
	if (!cfs_rq->throttle_count) {
		cfs_rq->throttled_clock_pelt = rq_clock_pelt(rq);
		list_del_leaf_cfs_rq(cfs_rq);
	}
	cfs_rq->throttle_count++;

	return 0;
}

static bool throttle_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	struct sched_entity *se;
	long task_delta, idle_task_delta, dequeue = 1;

	raw_spin_lock(&cfs_b->lock);
	/* This will start the period timer if necessary */
	if (__assign_cfs_rq_runtime(cfs_b, cfs_rq, 1)) {
		/*
		 * We have raced with bandwidth becoming available, and if we
		 * actually throttled the timer might not unthrottle us for an
		 * entire period. We additionally needed to make sure that any
		 * subsequent check_cfs_rq_runtime calls agree not to throttle
		 * us, as we may commit to do cfs put_prev+pick_next, so we ask
		 * for 1ns of runtime rather than just check cfs_b.
		 */
		dequeue = 0;
	} else {
		list_add_tail_rcu(&cfs_rq->throttled_list,
				  &cfs_b->throttled_cfs_rq);
	}
	raw_spin_unlock(&cfs_b->lock);

	if (!dequeue)
		return false;  /* Throttle no longer required. */

	se = cfs_rq->tg->se[cpu_of(rq_of(cfs_rq))];

	/* freeze hierarchy runnable averages while throttled */
	rcu_read_lock();
	walk_tg_tree_from(cfs_rq->tg, tg_throttle_down, tg_nop, (void *)rq);
	rcu_read_unlock();

	task_delta = cfs_rq->h_nr_running;
	idle_task_delta = cfs_rq->idle_h_nr_running;
	for_each_sched_entity(se) {
		struct cfs_rq *qcfs_rq = cfs_rq_of(se);
		/* throttled entity or throttle-on-deactivate */
		if (!se->on_rq)
			break;

		if (dequeue)
			dequeue_entity(qcfs_rq, se, DEQUEUE_SLEEP);
		qcfs_rq->h_nr_running -= task_delta;
		qcfs_rq->idle_h_nr_running -= idle_task_delta;

		if (qcfs_rq->load.weight)
			dequeue = 0;
	}

	if (!se)
		sub_nr_running(rq, task_delta);

	/*
	 * Note: distribution will already see us throttled via the
	 * throttled-list.  rq->lock protects completion.
	 */
	cfs_rq->throttled = 1;
	cfs_rq->throttled_clock = rq_clock(rq);
	return true;
}

void unthrottle_cfs_rq(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	struct sched_entity *se;
	long task_delta, idle_task_delta;

	se = cfs_rq->tg->se[cpu_of(rq)];

	cfs_rq->throttled = 0;

	update_rq_clock(rq);

	raw_spin_lock(&cfs_b->lock);
	cfs_b->throttled_time += rq_clock(rq) - cfs_rq->throttled_clock;
	list_del_rcu(&cfs_rq->throttled_list);
	raw_spin_unlock(&cfs_b->lock);

	/* update hierarchical throttle state */
	walk_tg_tree_from(cfs_rq->tg, tg_nop, tg_unthrottle_up, (void *)rq);

	if (!cfs_rq->load.weight)
		return;

	task_delta = cfs_rq->h_nr_running;
	idle_task_delta = cfs_rq->idle_h_nr_running;
	for_each_sched_entity(se) {
		if (se->on_rq)
			break;
		cfs_rq = cfs_rq_of(se);
		enqueue_entity(cfs_rq, se, ENQUEUE_WAKEUP);

		cfs_rq->h_nr_running += task_delta;
		cfs_rq->idle_h_nr_running += idle_task_delta;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto unthrottle_throttle;
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		cfs_rq->h_nr_running += task_delta;
		cfs_rq->idle_h_nr_running += idle_task_delta;


		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto unthrottle_throttle;

		/*
		 * One parent has been throttled and cfs_rq removed from the
		 * list. Add it back to not break the leaf list.
		 */
		if (throttled_hierarchy(cfs_rq))
			list_add_leaf_cfs_rq(cfs_rq);
	}

	/* At this point se is NULL and we are at root level*/
	add_nr_running(rq, task_delta);

unthrottle_throttle:
	/*
	 * The cfs_rq_throttled() breaks in the above iteration can result in
	 * incomplete leaf list maintenance, resulting in triggering the
	 * assertion below.
	 */
	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		if (list_add_leaf_cfs_rq(cfs_rq))
			break;
	}

	assert_list_leaf_cfs_rq(rq);

	/* Determine whether we need to wake up potentially idle CPU: */
	if (rq->curr == rq->idle && rq->cfs.nr_running)
		resched_curr(rq);
}

static u64 distribute_cfs_runtime(struct cfs_bandwidth *cfs_b, u64 remaining)
{
	struct cfs_rq *cfs_rq;
	u64 runtime;
	u64 starting_runtime = remaining;

	rcu_read_lock();
	list_for_each_entry_rcu(cfs_rq, &cfs_b->throttled_cfs_rq,
				throttled_list) {
		struct rq *rq = rq_of(cfs_rq);
		struct rq_flags rf;

		rq_lock_irqsave(rq, &rf);
		if (!cfs_rq_throttled(cfs_rq))
			goto next;

		/* By the above check, this should never be true */
		SCHED_WARN_ON(cfs_rq->runtime_remaining > 0);

		runtime = -cfs_rq->runtime_remaining + 1;
		if (runtime > remaining)
			runtime = remaining;
		remaining -= runtime;

		cfs_rq->runtime_remaining += runtime;

		/* we check whether we're throttled above */
		if (cfs_rq->runtime_remaining > 0)
			unthrottle_cfs_rq(cfs_rq);

next:
		rq_unlock_irqrestore(rq, &rf);

		if (!remaining)
			break;
	}
	rcu_read_unlock();

	return starting_runtime - remaining;
}

/*
 * Responsible for refilling a task_group's bandwidth and unthrottling its
 * cfs_rqs as appropriate. If there has been no activity within the last
 * period the timer is deactivated until scheduling resumes; cfs_b->idle is
 * used to track this state.
 */
static int do_sched_cfs_period_timer(struct cfs_bandwidth *cfs_b, int overrun, unsigned long flags)
{
	u64 runtime;
	int throttled;

	/* no need to continue the timer with no bandwidth constraint */
	if (cfs_b->quota == RUNTIME_INF)
		goto out_deactivate;

	throttled = !list_empty(&cfs_b->throttled_cfs_rq);
	cfs_b->nr_periods += overrun;

	/*
	 * idle depends on !throttled (for the case of a large deficit), and if
	 * we're going inactive then everything else can be deferred
	 */
	if (cfs_b->idle && !throttled)
		goto out_deactivate;

	__refill_cfs_bandwidth_runtime(cfs_b);

	if (!throttled) {
		/* mark as potentially idle for the upcoming period */
		cfs_b->idle = 1;
		return 0;
	}

	/* account preceding periods in which throttling occurred */
	cfs_b->nr_throttled += overrun;

	/*
	 * This check is repeated as we are holding onto the new bandwidth while
	 * we unthrottle. This can potentially race with an unthrottled group
	 * trying to acquire new bandwidth from the global pool. This can result
	 * in us over-using our runtime if it is all used during this loop, but
	 * only by limited amounts in that extreme case.
	 */
	while (throttled && cfs_b->runtime > 0 && !cfs_b->distribute_running) {
		runtime = cfs_b->runtime;
		cfs_b->distribute_running = 1;
		raw_spin_unlock_irqrestore(&cfs_b->lock, flags);
		/* we can't nest cfs_b->lock while distributing bandwidth */
		runtime = distribute_cfs_runtime(cfs_b, runtime);
		raw_spin_lock_irqsave(&cfs_b->lock, flags);

		cfs_b->distribute_running = 0;
		throttled = !list_empty(&cfs_b->throttled_cfs_rq);

		lsub_positive(&cfs_b->runtime, runtime);
	}

	/*
	 * While we are ensured activity in the period following an
	 * unthrottle, this also covers the case in which the new bandwidth is
	 * insufficient to cover the existing bandwidth deficit.  (Forcing the
	 * timer to remain active while there are any throttled entities.)
	 */
	cfs_b->idle = 0;

	return 0;

out_deactivate:
	return 1;
}

/* a cfs_rq won't donate quota below this amount */
static const u64 min_cfs_rq_runtime = 1 * NSEC_PER_MSEC;
/* minimum remaining period time to redistribute slack quota */
static const u64 min_bandwidth_expiration = 2 * NSEC_PER_MSEC;
/* how long we wait to gather additional slack before distributing */
static const u64 cfs_bandwidth_slack_period = 5 * NSEC_PER_MSEC;

/*
 * Are we near the end of the current quota period?
 *
 * Requires cfs_b->lock for hrtimer_expires_remaining to be safe against the
 * hrtimer base being cleared by hrtimer_start. In the case of
 * migrate_hrtimers, base is never cleared, so we are fine.
 */
static int runtime_refresh_within(struct cfs_bandwidth *cfs_b, u64 min_expire)
{
	struct hrtimer *refresh_timer = &cfs_b->period_timer;
	s64 remaining;

	/* if the call-back is running a quota refresh is already occurring */
	if (hrtimer_callback_running(refresh_timer))
		return 1;

	/* is a quota refresh about to occur? */
	remaining = ktime_to_ns(hrtimer_expires_remaining(refresh_timer));
	if (remaining < (s64)min_expire)
		return 1;

	return 0;
}

static void start_cfs_slack_bandwidth(struct cfs_bandwidth *cfs_b)
{
	u64 min_left = cfs_bandwidth_slack_period + min_bandwidth_expiration;

	/* if there's a quota refresh soon don't bother with slack */
	if (runtime_refresh_within(cfs_b, min_left))
		return;

	/* don't push forwards an existing deferred unthrottle */
	if (cfs_b->slack_started)
		return;
	cfs_b->slack_started = true;

	hrtimer_start(&cfs_b->slack_timer,
			ns_to_ktime(cfs_bandwidth_slack_period),
			HRTIMER_MODE_REL);
}

/* we know any runtime found here is valid as update_curr() precedes return */
static void __return_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	struct cfs_bandwidth *cfs_b = tg_cfs_bandwidth(cfs_rq->tg);
	s64 slack_runtime = cfs_rq->runtime_remaining - min_cfs_rq_runtime;

	if (slack_runtime <= 0)
		return;

	raw_spin_lock(&cfs_b->lock);
	if (cfs_b->quota != RUNTIME_INF) {
		cfs_b->runtime += slack_runtime;

		/* we are under rq->lock, defer unthrottling using a timer */
		if (cfs_b->runtime > sched_cfs_bandwidth_slice() &&
		    !list_empty(&cfs_b->throttled_cfs_rq))
			start_cfs_slack_bandwidth(cfs_b);
	}
	raw_spin_unlock(&cfs_b->lock);

	/* even if it's not valid for return we don't want to try again */
	cfs_rq->runtime_remaining -= slack_runtime;
}

static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return;

	if (!cfs_rq->runtime_enabled || cfs_rq->nr_running)
		return;

	__return_cfs_rq_runtime(cfs_rq);
}

/*
 * This is done with a timer (instead of inline with bandwidth return) since
 * it's necessary to juggle rq->locks to unthrottle their respective cfs_rqs.
 */
static void do_sched_cfs_slack_timer(struct cfs_bandwidth *cfs_b)
{
	u64 runtime = 0, slice = sched_cfs_bandwidth_slice();
	unsigned long flags;

	/* confirm we're still not at a refresh boundary */
	raw_spin_lock_irqsave(&cfs_b->lock, flags);
	cfs_b->slack_started = false;
	if (cfs_b->distribute_running) {
		raw_spin_unlock_irqrestore(&cfs_b->lock, flags);
		return;
	}

	if (runtime_refresh_within(cfs_b, min_bandwidth_expiration)) {
		raw_spin_unlock_irqrestore(&cfs_b->lock, flags);
		return;
	}

	if (cfs_b->quota != RUNTIME_INF && cfs_b->runtime > slice)
		runtime = cfs_b->runtime;

	if (runtime)
		cfs_b->distribute_running = 1;

	raw_spin_unlock_irqrestore(&cfs_b->lock, flags);

	if (!runtime)
		return;

	runtime = distribute_cfs_runtime(cfs_b, runtime);

	raw_spin_lock_irqsave(&cfs_b->lock, flags);
	lsub_positive(&cfs_b->runtime, runtime);
	cfs_b->distribute_running = 0;
	raw_spin_unlock_irqrestore(&cfs_b->lock, flags);
}

/*
 * When a group wakes up we want to make sure that its quota is not already
 * expired/exceeded, otherwise it may be allowed to steal additional ticks of
 * runtime as update_curr() throttling can not not trigger until it's on-rq.
 */
static void check_enqueue_throttle(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return;

	/* an active group must be handled by the update_curr()->put() path */
	if (!cfs_rq->runtime_enabled || cfs_rq->curr)
		return;

	/* ensure the group is not already throttled */
	if (cfs_rq_throttled(cfs_rq))
		return;

	/* update runtime allocation */
	account_cfs_rq_runtime(cfs_rq, 0);
	if (cfs_rq->runtime_remaining <= 0)
		throttle_cfs_rq(cfs_rq);
}

static void sync_throttle(struct task_group *tg, int cpu)
{
	struct cfs_rq *pcfs_rq, *cfs_rq;

	if (!cfs_bandwidth_used())
		return;

	if (!tg->parent)
		return;

	cfs_rq = tg->cfs_rq[cpu];
	pcfs_rq = tg->parent->cfs_rq[cpu];

	cfs_rq->throttle_count = pcfs_rq->throttle_count;
	cfs_rq->throttled_clock_pelt = rq_clock_pelt(cpu_rq(cpu));
}

/* conditionally throttle active cfs_rq's from put_prev_entity() */
static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	if (!cfs_bandwidth_used())
		return false;

	if (likely(!cfs_rq->runtime_enabled || cfs_rq->runtime_remaining > 0))
		return false;

	/*
	 * it's possible for a throttled entity to be forced into a running
	 * state (e.g. set_curr_task), in this case we're finished.
	 */
	if (cfs_rq_throttled(cfs_rq))
		return true;

	return throttle_cfs_rq(cfs_rq);
}

static enum hrtimer_restart sched_cfs_slack_timer(struct hrtimer *timer)
{
	struct cfs_bandwidth *cfs_b =
		container_of(timer, struct cfs_bandwidth, slack_timer);

	do_sched_cfs_slack_timer(cfs_b);

	return HRTIMER_NORESTART;
}

extern const u64 max_cfs_quota_period;

static enum hrtimer_restart sched_cfs_period_timer(struct hrtimer *timer)
{
	struct cfs_bandwidth *cfs_b =
		container_of(timer, struct cfs_bandwidth, period_timer);
	unsigned long flags;
	int overrun;
	int idle = 0;
	int count = 0;

	raw_spin_lock_irqsave(&cfs_b->lock, flags);
	for (;;) {
		overrun = hrtimer_forward_now(timer, cfs_b->period);
		if (!overrun)
			break;

		idle = do_sched_cfs_period_timer(cfs_b, overrun, flags);

		if (++count > 3) {
			u64 new, old = ktime_to_ns(cfs_b->period);

			/*
			 * Grow period by a factor of 2 to avoid losing precision.
			 * Precision loss in the quota/period ratio can cause __cfs_schedulable
			 * to fail.
			 */
			new = old * 2;
			if (new < max_cfs_quota_period) {
				cfs_b->period = ns_to_ktime(new);
				cfs_b->quota *= 2;

				pr_warn_ratelimited(
	"cfs_period_timer[cpu%d]: period too short, scaling up (new cfs_period_us = %lld, cfs_quota_us = %lld)\n",
					smp_processor_id(),
					div_u64(new, NSEC_PER_USEC),
					div_u64(cfs_b->quota, NSEC_PER_USEC));
			} else {
				pr_warn_ratelimited(
	"cfs_period_timer[cpu%d]: period too short, but cannot scale up without losing precision (cfs_period_us = %lld, cfs_quota_us = %lld)\n",
					smp_processor_id(),
					div_u64(old, NSEC_PER_USEC),
					div_u64(cfs_b->quota, NSEC_PER_USEC));
			}

			/* reset count so we don't come right back in here */
			count = 0;
		}
	}
	if (idle)
		cfs_b->period_active = 0;
	raw_spin_unlock_irqrestore(&cfs_b->lock, flags);

	return idle ? HRTIMER_NORESTART : HRTIMER_RESTART;
}

void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	raw_spin_lock_init(&cfs_b->lock);
	cfs_b->runtime = 0;
	cfs_b->quota = RUNTIME_INF;
	cfs_b->period = ns_to_ktime(default_cfs_period());

	INIT_LIST_HEAD(&cfs_b->throttled_cfs_rq);
	hrtimer_init(&cfs_b->period_timer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS_PINNED);
	cfs_b->period_timer.function = sched_cfs_period_timer;
	hrtimer_init(&cfs_b->slack_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	cfs_b->slack_timer.function = sched_cfs_slack_timer;
	cfs_b->distribute_running = 0;
	cfs_b->slack_started = false;
}

static void init_cfs_rq_runtime(struct cfs_rq *cfs_rq)
{
	cfs_rq->runtime_enabled = 0;
	INIT_LIST_HEAD(&cfs_rq->throttled_list);
}

void start_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	lockdep_assert_held(&cfs_b->lock);

	if (cfs_b->period_active)
		return;

	cfs_b->period_active = 1;
	hrtimer_forward_now(&cfs_b->period_timer, cfs_b->period);
	hrtimer_start_expires(&cfs_b->period_timer, HRTIMER_MODE_ABS_PINNED);
}

static void destroy_cfs_bandwidth(struct cfs_bandwidth *cfs_b)
{
	/* init_cfs_bandwidth() was not called */
	if (!cfs_b->throttled_cfs_rq.next)
		return;

	hrtimer_cancel(&cfs_b->period_timer);
	hrtimer_cancel(&cfs_b->slack_timer);
}

/*
 * Both these CPU hotplug callbacks race against unregister_fair_sched_group()
 *
 * The race is harmless, since modifying bandwidth settings of unhooked group
 * bits doesn't do much.
 */

/* cpu online calback */
static void __maybe_unused update_runtime_enabled(struct rq *rq)
{
	struct task_group *tg;

	lockdep_assert_held(&rq->lock);

	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
		struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

		raw_spin_lock(&cfs_b->lock);
		cfs_rq->runtime_enabled = cfs_b->quota != RUNTIME_INF;
		raw_spin_unlock(&cfs_b->lock);
	}
	rcu_read_unlock();
}

/* cpu offline callback */
static void __maybe_unused unthrottle_offline_cfs_rqs(struct rq *rq)
{
	struct task_group *tg;

	lockdep_assert_held(&rq->lock);

	rcu_read_lock();
	list_for_each_entry_rcu(tg, &task_groups, list) {
		struct cfs_rq *cfs_rq = tg->cfs_rq[cpu_of(rq)];

		if (!cfs_rq->runtime_enabled)
			continue;

		/*
		 * clock_task is not advancing so we just need to make sure
		 * there's some valid quota amount
		 */
		cfs_rq->runtime_remaining = 1;
		/*
		 * Offline rq is schedulable till CPU is completely disabled
		 * in take_cpu_down(), so we prevent new cfs throttling here.
		 */
		cfs_rq->runtime_enabled = 0;

		if (cfs_rq_throttled(cfs_rq))
			unthrottle_cfs_rq(cfs_rq);
	}
	rcu_read_unlock();
}

#else /* CONFIG_CFS_BANDWIDTH */

static inline bool cfs_bandwidth_used(void)
{
	return false;
}

static void account_cfs_rq_runtime(struct cfs_rq *cfs_rq, u64 delta_exec) {}
static bool check_cfs_rq_runtime(struct cfs_rq *cfs_rq) { return false; }
static void check_enqueue_throttle(struct cfs_rq *cfs_rq) {}
static inline void sync_throttle(struct task_group *tg, int cpu) {}
static __always_inline void return_cfs_rq_runtime(struct cfs_rq *cfs_rq) {}

static inline int cfs_rq_throttled(struct cfs_rq *cfs_rq)
{
	return 0;
}

static inline int throttled_hierarchy(struct cfs_rq *cfs_rq)
{
	return 0;
}

static inline int throttled_lb_pair(struct task_group *tg,
				    int src_cpu, int dest_cpu)
{
	return 0;
}

void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b) {}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void init_cfs_rq_runtime(struct cfs_rq *cfs_rq) {}
#endif

static inline struct cfs_bandwidth *tg_cfs_bandwidth(struct task_group *tg)
{
	return NULL;
}
static inline void destroy_cfs_bandwidth(struct cfs_bandwidth *cfs_b) {}
static inline void update_runtime_enabled(struct rq *rq) {}
static inline void unthrottle_offline_cfs_rqs(struct rq *rq) {}

#endif /* CONFIG_CFS_BANDWIDTH */

/**************************************************
 * CFS operations on tasks:
 */

#ifdef CONFIG_SCHED_HRTICK
static void hrtick_start_fair(struct rq *rq, struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	SCHED_WARN_ON(task_rq(p) != rq);

	if (rq->cfs.h_nr_running > 1) {
		u64 slice = sched_slice(cfs_rq, se);
		u64 ran = se->sum_exec_runtime - se->prev_sum_exec_runtime;
		s64 delta = slice - ran;

		if (delta < 0) {
			if (rq->curr == p)
				resched_curr(rq);
			return;
		}
		hrtick_start(rq, delta);
	}
}

/*
 * called from enqueue/dequeue and updates the hrtick when the
 * current task is from our class and nr_running is low enough
 * to matter.
 */
static void hrtick_update(struct rq *rq)
{
	struct task_struct *curr = rq->curr;

	if (!hrtick_enabled(rq) || curr->sched_class != &fair_sched_class)
		return;

	if (cfs_rq_of(&curr->se)->nr_running < sched_nr_latency)
		hrtick_start_fair(rq, curr);
}
#else /* !CONFIG_SCHED_HRTICK */
static inline void
hrtick_start_fair(struct rq *rq, struct task_struct *p)
{
}

static inline void hrtick_update(struct rq *rq)
{
}
#endif

#ifdef CONFIG_SMP
bool __cpu_overutilized(int cpu, int delta)
{
	return (capacity_orig_of(cpu) * 1024) <
		((cpu_util(cpu) + delta) * sched_capacity_margin_up[cpu]);
}

bool cpu_overutilized(int cpu)
{
	return __cpu_overutilized(cpu, 0);
}

#ifdef CONFIG_SCHED_WALT
static bool sd_overutilized(struct sched_domain *sd)
{
	return sd->shared->overutilized;
}

static void set_sd_overutilized(struct sched_domain *sd)
{
	trace_sched_overutilized(sd, sd->shared->overutilized, true);
	sd->shared->overutilized = true;
}

static void clear_sd_overutilized(struct sched_domain *sd)
{
	trace_sched_overutilized(sd, sd->shared->overutilized, false);
	sd->shared->overutilized = false;
}
#endif

static inline void update_overutilized_status(struct rq *rq)
{
#ifdef CONFIG_SCHED_WALT
	struct sched_domain *sd;

	rcu_read_lock();
	sd = rcu_dereference(rq->sd);
	if (sd && !sd_overutilized(sd) &&
	    cpu_overutilized(rq->cpu))
		set_sd_overutilized(sd);
	rcu_read_unlock();
#else
	if (!READ_ONCE(rq->rd->overutilized) && cpu_overutilized(rq->cpu)) {
		WRITE_ONCE(rq->rd->overutilized, SG_OVERUTILIZED);
		trace_sched_overutilized_tp(rq->rd, SG_OVERUTILIZED);
	}
#endif
}
#else
static inline void update_overutilized_status(struct rq *rq) { }
#endif

/*
 * The enqueue_task method is called before nr_running is
 * increased. Here we update the fair scheduling stats and
 * then put the task into the rbtree:
 */
static void
enqueue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;
	int idle_h_nr_running = task_has_idle_policy(p);
	int task_new = !(flags & ENQUEUE_WAKEUP);

	/*
	 * The code below (indirectly) updates schedutil which looks at
	 * the cfs_rq utilization to select a frequency.
	 * Let's add the task's estimated utilization to the cfs_rq's
	 * estimated utilization, before we update schedutil.
	 */
	util_est_enqueue(&rq->cfs, p);

	/*
	 * If in_iowait is set, the code below may not trigger any cpufreq
	 * utilization updates, so do it here explicitly with the IOWAIT flag
	 * passed.
	 */
	if (p->in_iowait)
		cpufreq_update_util(rq, SCHED_CPUFREQ_IOWAIT);

	for_each_sched_entity(se) {
		if (se->on_rq)
			break;
		cfs_rq = cfs_rq_of(se);
		enqueue_entity(cfs_rq, se, flags);

		cfs_rq->h_nr_running++;
		cfs_rq->idle_h_nr_running += idle_h_nr_running;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto enqueue_throttle;

		flags = ENQUEUE_WAKEUP;
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		update_load_avg(cfs_rq, se, UPDATE_TG);
		update_cfs_group(se);

		cfs_rq->h_nr_running++;
		cfs_rq->idle_h_nr_running += idle_h_nr_running;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto enqueue_throttle;

               /*
                * One parent has been throttled and cfs_rq removed from the
                * list. Add it back to not break the leaf list.
                */
               if (throttled_hierarchy(cfs_rq))
                       list_add_leaf_cfs_rq(cfs_rq);
	}

enqueue_throttle:
	if (!se) {
		add_nr_running(rq, 1);
#ifdef CONFIG_SCHED_WALT
		p->wts.misfit = !task_fits_max(p, rq->cpu);
#endif
		inc_rq_walt_stats(rq, p);
		/*
		 * Since new tasks are assigned an initial util_avg equal to
		 * half of the spare capacity of their CPU, tiny tasks have the
		 * ability to cross the overutilized threshold, which will
		 * result in the load balancer ruining all the task placement
		 * done by EAS. As a way to mitigate that effect, do not account
		 * for the first enqueue operation of new tasks during the
		 * overutilized flag detection.
		 *
		 * A better way of solving this problem would be to wait for
		 * the PELT signals of tasks to converge before taking them
		 * into account, but that is not straightforward to implement,
		 * and the following generally works well enough in practice.
		 */
		if (!task_new)
			update_overutilized_status(rq);

	}

	if (cfs_bandwidth_used()) {
		/*
		 * When bandwidth control is enabled; the cfs_rq_throttled()
		 * breaks in the above iteration can result in incomplete
		 * leaf list maintenance, resulting in triggering the assertion
		 * below.
		 */
		for_each_sched_entity(se) {
			cfs_rq = cfs_rq_of(se);

			if (list_add_leaf_cfs_rq(cfs_rq))
				break;
		}
	}

	assert_list_leaf_cfs_rq(rq);

	hrtick_update(rq);
}

static void set_next_buddy(struct sched_entity *se);

/*
 * The dequeue_task method is called before nr_running is
 * decreased. We remove the task from the rbtree and
 * update the fair scheduling stats:
 */
static void dequeue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se;
	int task_sleep = flags & DEQUEUE_SLEEP;
	int idle_h_nr_running = task_has_idle_policy(p);

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		dequeue_entity(cfs_rq, se, flags);

		cfs_rq->h_nr_running--;
		cfs_rq->idle_h_nr_running -= idle_h_nr_running;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto dequeue_throttle;

		/* Don't dequeue parent if it has other entities besides us */
		if (cfs_rq->load.weight) {
			/* Avoid re-evaluating load for this entity: */
			se = parent_entity(se);
			/*
			 * Bias pick_next to pick a task from this cfs_rq, as
			 * p is sleeping when it is within its sched_slice.
			 */
			if (task_sleep && se && !throttled_hierarchy(cfs_rq))
				set_next_buddy(se);
			break;
		}
		flags |= DEQUEUE_SLEEP;
	}

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		update_load_avg(cfs_rq, se, UPDATE_TG);
		update_cfs_group(se);

		cfs_rq->h_nr_running--;
		cfs_rq->idle_h_nr_running -= idle_h_nr_running;

		/* end evaluation on encountering a throttled cfs_rq */
		if (cfs_rq_throttled(cfs_rq))
			goto dequeue_throttle;

	}

dequeue_throttle:
	if (!se) {
		sub_nr_running(rq, 1);
		dec_rq_walt_stats(rq, p);
	}

	util_est_dequeue(&rq->cfs, p, task_sleep);
	hrtick_update(rq);
}

#ifdef CONFIG_SMP

/* Working cpumask for: load_balance, load_balance_newidle. */
DEFINE_PER_CPU(cpumask_var_t, load_balance_mask);
DEFINE_PER_CPU(cpumask_var_t, select_idle_mask);

#ifdef CONFIG_NO_HZ_COMMON

static struct {
	cpumask_var_t idle_cpus_mask;
	atomic_t nr_cpus;
	int has_blocked;		/* Idle CPUS has blocked load */
	unsigned long next_balance;     /* in jiffy units */
	unsigned long next_blocked;	/* Next update of blocked load in jiffies */
} nohz ____cacheline_aligned;

#endif /* CONFIG_NO_HZ_COMMON */

/* CPU only has SCHED_IDLE tasks enqueued */
static int sched_idle_cpu(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	return unlikely(rq->nr_running == rq->cfs.idle_h_nr_running &&
			rq->nr_running);
}

static unsigned long cpu_runnable_load(struct rq *rq)
{
	return cfs_rq_runnable_load_avg(&rq->cfs);
}

static unsigned long cpu_avg_load_per_task(int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long nr_running = READ_ONCE(rq->cfs.h_nr_running);
	unsigned long load_avg = cpu_runnable_load(rq);

	if (nr_running)
		return load_avg / nr_running;

	return 0;
}

static void record_wakee(struct task_struct *p)
{
	/*
	 * Only decay a single time; tasks that have less then 1 wakeup per
	 * jiffy will not have built up many flips.
	 */
	if (time_after(jiffies, current->wakee_flip_decay_ts + HZ)) {
		current->wakee_flips >>= 1;
		current->wakee_flip_decay_ts = jiffies;
	}

	if (current->last_wakee != p) {
		current->last_wakee = p;
		current->wakee_flips++;
	}
}

/*
 * Detect M:N waker/wakee relationships via a switching-frequency heuristic.
 *
 * A waker of many should wake a different task than the one last awakened
 * at a frequency roughly N times higher than one of its wakees.
 *
 * In order to determine whether we should let the load spread vs consolidating
 * to shared cache, we look for a minimum 'flip' frequency of llc_size in one
 * partner, and a factor of lls_size higher frequency in the other.
 *
 * With both conditions met, we can be relatively sure that the relationship is
 * non-monogamous, with partner count exceeding socket size.
 *
 * Waker/wakee being client/server, worker/dispatcher, interrupt source or
 * whatever is irrelevant, spread criteria is apparent partner count exceeds
 * socket size.
 */
static int wake_wide(struct task_struct *p, int sibling_count_hint)
{
	unsigned int master = current->wakee_flips;
	unsigned int slave = p->wakee_flips;
	int llc_size = this_cpu_read(sd_llc_size);

	if (sibling_count_hint >= llc_size)
		return 1;

	if (master < slave)
		swap(master, slave);
	if (slave < llc_size || master < slave * llc_size)
		return 0;
	return 1;
}

/*
 * The purpose of wake_affine() is to quickly determine on which CPU we can run
 * soonest. For the purpose of speed we only consider the waking and previous
 * CPU.
 *
 * wake_affine_idle() - only considers 'now', it check if the waking CPU is
 *			cache-affine and is (or	will be) idle.
 *
 * wake_affine_weight() - considers the weight to reflect the average
 *			  scheduling latency of the CPUs. This seems to work
 *			  for the overloaded case.
 */
static int
wake_affine_idle(int this_cpu, int prev_cpu, int sync)
{
	/*
	 * If this_cpu is idle, it implies the wakeup is from interrupt
	 * context. Only allow the move if cache is shared. Otherwise an
	 * interrupt intensive workload could force all tasks onto one
	 * node depending on the IO topology or IRQ affinity settings.
	 *
	 * If the prev_cpu is idle and cache affine then avoid a migration.
	 * There is no guarantee that the cache hot data from an interrupt
	 * is more important than cache hot data on the prev_cpu and from
	 * a cpufreq perspective, it's better to have higher utilisation
	 * on one CPU.
	 */
	if (available_idle_cpu(this_cpu) && cpus_share_cache(this_cpu, prev_cpu))
		return available_idle_cpu(prev_cpu) ? prev_cpu : this_cpu;

	if (sync && cpu_rq(this_cpu)->nr_running == 1)
		return this_cpu;

	return nr_cpumask_bits;
}

static int
wake_affine_weight(struct sched_domain *sd, struct task_struct *p,
		   int this_cpu, int prev_cpu, int sync)
{
	s64 this_eff_load, prev_eff_load;
	unsigned long task_load;

	this_eff_load = cpu_runnable_load(cpu_rq(this_cpu));

	if (sync) {
		unsigned long current_load = task_h_load(current);

		if (current_load > this_eff_load)
			return this_cpu;

		this_eff_load -= current_load;
	}

	task_load = task_h_load(p);

	this_eff_load += task_load;
	if (sched_feat(WA_BIAS))
		this_eff_load *= 100;
	this_eff_load *= capacity_of(prev_cpu);

	prev_eff_load = cpu_runnable_load(cpu_rq(prev_cpu));
	prev_eff_load -= task_load;
	if (sched_feat(WA_BIAS))
		prev_eff_load *= 100 + (sd->imbalance_pct - 100) / 2;
	prev_eff_load *= capacity_of(this_cpu);

	/*
	 * If sync, adjust the weight of prev_eff_load such that if
	 * prev_eff == this_eff that select_idle_sibling() will consider
	 * stacking the wakee on top of the waker if no other CPU is
	 * idle.
	 */
	if (sync)
		prev_eff_load += 1;

	return this_eff_load < prev_eff_load ? this_cpu : nr_cpumask_bits;
}

static int wake_affine(struct sched_domain *sd, struct task_struct *p,
		       int this_cpu, int prev_cpu, int sync)
{
	int target = nr_cpumask_bits;

	if (sched_feat(WA_IDLE))
		target = wake_affine_idle(this_cpu, prev_cpu, sync);

	if (sched_feat(WA_WEIGHT) && target == nr_cpumask_bits)
		target = wake_affine_weight(sd, p, this_cpu, prev_cpu, sync);

	schedstat_inc(p->se.statistics.nr_wakeups_affine_attempts);
	if (target == nr_cpumask_bits)
		return prev_cpu;

	schedstat_inc(sd->ttwu_move_affine);
	schedstat_inc(p->se.statistics.nr_wakeups_affine);
	return target;
}

static unsigned long cpu_util_without(int cpu, struct task_struct *p);

static unsigned long capacity_spare_without(int cpu, struct task_struct *p)
{
	return max_t(long, capacity_of(cpu) - cpu_util_without(cpu, p), 0);
}

/*
 * find_idlest_group finds and returns the least busy CPU group within the
 * domain.
 *
 * Assumes p is allowed on at least one CPU in sd.
 */
static struct sched_group *
find_idlest_group(struct sched_domain *sd, struct task_struct *p,
		  int this_cpu, int sd_flag)
{
	struct sched_group *idlest = NULL, *group = sd->groups;
	struct sched_group *most_spare_sg = NULL;
	unsigned long min_runnable_load = ULONG_MAX;
	unsigned long this_runnable_load = ULONG_MAX;
	unsigned long min_avg_load = ULONG_MAX, this_avg_load = ULONG_MAX;
	unsigned long most_spare = 0, this_spare = 0;
	int imbalance_scale = 100 + (sd->imbalance_pct-100)/2;
	unsigned long imbalance = scale_load_down(NICE_0_LOAD) *
				(sd->imbalance_pct-100) / 100;

	do {
		unsigned long load, avg_load, runnable_load;
		unsigned long spare_cap, max_spare_cap;
		int local_group;
		int i;

		/* Skip over this group if it has no CPUs allowed */
		if (!cpumask_intersects(sched_group_span(group),
					p->cpus_ptr))
			continue;

		local_group = cpumask_test_cpu(this_cpu,
					       sched_group_span(group));

		/*
		 * Tally up the load of all CPUs in the group and find
		 * the group containing the CPU with most spare capacity.
		 */
		avg_load = 0;
		runnable_load = 0;
		max_spare_cap = 0;

		for_each_cpu(i, sched_group_span(group)) {
			load = cpu_runnable_load(cpu_rq(i));
			runnable_load += load;

			avg_load += cfs_rq_load_avg(&cpu_rq(i)->cfs);

			spare_cap = capacity_spare_without(i, p);

			if (spare_cap > max_spare_cap)
				max_spare_cap = spare_cap;
		}

		/* Adjust by relative CPU capacity of the group */
		avg_load = (avg_load * SCHED_CAPACITY_SCALE) /
					group->sgc->capacity;
		runnable_load = (runnable_load * SCHED_CAPACITY_SCALE) /
					group->sgc->capacity;

		if (local_group) {
			this_runnable_load = runnable_load;
			this_avg_load = avg_load;
			this_spare = max_spare_cap;
		} else {
			if (min_runnable_load > (runnable_load + imbalance)) {
				/*
				 * The runnable load is significantly smaller
				 * so we can pick this new CPU:
				 */
				min_runnable_load = runnable_load;
				min_avg_load = avg_load;
				idlest = group;
			} else if ((runnable_load < (min_runnable_load + imbalance)) &&
				   (100*min_avg_load > imbalance_scale*avg_load)) {
				/*
				 * The runnable loads are close so take the
				 * blocked load into account through avg_load:
				 */
				min_avg_load = avg_load;
				idlest = group;
			}

			if (most_spare < max_spare_cap) {
				most_spare = max_spare_cap;
				most_spare_sg = group;
			}
		}
	} while (group = group->next, group != sd->groups);

	/*
	 * The cross-over point between using spare capacity or least load
	 * is too conservative for high utilization tasks on partially
	 * utilized systems if we require spare_capacity > task_util(p),
	 * so we allow for some task stuffing by using
	 * spare_capacity > task_util(p)/2.
	 *
	 * Spare capacity can't be used for fork because the utilization has
	 * not been set yet, we must first select a rq to compute the initial
	 * utilization.
	 */
	if (sd_flag & SD_BALANCE_FORK)
		goto skip_spare;

	if (this_spare > task_util(p) / 2 &&
	    imbalance_scale*this_spare > 100*most_spare)
		return NULL;

	if (most_spare > task_util(p) / 2)
		return most_spare_sg;

skip_spare:
	if (!idlest)
		return NULL;

	/*
	 * When comparing groups across NUMA domains, it's possible for the
	 * local domain to be very lightly loaded relative to the remote
	 * domains but "imbalance" skews the comparison making remote CPUs
	 * look much more favourable. When considering cross-domain, add
	 * imbalance to the runnable load on the remote node and consider
	 * staying local.
	 */
	if ((sd->flags & SD_NUMA) &&
	    min_runnable_load + imbalance >= this_runnable_load)
		return NULL;

	if (min_runnable_load > (this_runnable_load + imbalance))
		return NULL;

	if ((this_runnable_load < (min_runnable_load + imbalance)) &&
	     (100*this_avg_load < imbalance_scale*min_avg_load))
		return NULL;

	return idlest;
}

/*
 * find_idlest_group_cpu - find the idlest CPU among the CPUs in the group.
 */
static int
find_idlest_group_cpu(struct sched_group *group, struct task_struct *p, int this_cpu)
{
	unsigned long load, min_load = ULONG_MAX;
	unsigned int min_exit_latency = UINT_MAX;
	u64 latest_idle_timestamp = 0;
	int least_loaded_cpu = this_cpu;
	int shallowest_idle_cpu = -1, si_cpu = -1;
	int i;

	/* Check if we have any choice: */
	if (group->group_weight == 1)
		return cpumask_first(sched_group_span(group));

	/* Traverse only the allowed CPUs */
	for_each_cpu_and(i, sched_group_span(group), p->cpus_ptr) {
		if (available_idle_cpu(i)) {
			struct rq *rq = cpu_rq(i);
			struct cpuidle_state *idle = idle_get_state(rq);
			if (idle && idle->exit_latency < min_exit_latency) {
				/*
				 * We give priority to a CPU whose idle state
				 * has the smallest exit latency irrespective
				 * of any idle timestamp.
				 */
				min_exit_latency = idle->exit_latency;
				latest_idle_timestamp = rq->idle_stamp;
				shallowest_idle_cpu = i;
			} else if ((!idle || idle->exit_latency == min_exit_latency) &&
				   rq->idle_stamp > latest_idle_timestamp) {
				/*
				 * If equal or no active idle state, then
				 * the most recently idled CPU might have
				 * a warmer cache.
				 */
				latest_idle_timestamp = rq->idle_stamp;
				shallowest_idle_cpu = i;
			}
		} else if (shallowest_idle_cpu == -1 && si_cpu == -1) {
			if (sched_idle_cpu(i)) {
				si_cpu = i;
				continue;
			}

			load = cpu_runnable_load(cpu_rq(i));
			if (load < min_load) {
				min_load = load;
				least_loaded_cpu = i;
			}
		}
	}

	if (shallowest_idle_cpu != -1)
		return shallowest_idle_cpu;
	if (si_cpu != -1)
		return si_cpu;
	return least_loaded_cpu;
}

static inline int find_idlest_cpu(struct sched_domain *sd, struct task_struct *p,
				  int cpu, int prev_cpu, int sd_flag)
{
	int new_cpu = cpu;

	if (!cpumask_intersects(sched_domain_span(sd), p->cpus_ptr))
		return prev_cpu;

	/*
	 * We need task's util for capacity_spare_without, sync it up to
	 * prev_cpu's last_update_time.
	 */
	if (!(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	while (sd) {
		struct sched_group *group;
		struct sched_domain *tmp;
		int weight;

		if (!(sd->flags & sd_flag)) {
			sd = sd->child;
			continue;
		}

		group = find_idlest_group(sd, p, cpu, sd_flag);
		if (!group) {
			sd = sd->child;
			continue;
		}

		new_cpu = find_idlest_group_cpu(group, p, cpu);
		if (new_cpu == cpu) {
			/* Now try balancing at a lower domain level of 'cpu': */
			sd = sd->child;
			continue;
		}

		/* Now try balancing at a lower domain level of 'new_cpu': */
		cpu = new_cpu;
		weight = sd->span_weight;
		sd = NULL;
		for_each_domain(cpu, tmp) {
			if (weight <= tmp->span_weight)
				break;
			if (tmp->flags & sd_flag)
				sd = tmp;
		}
	}

	return new_cpu;
}

#ifdef CONFIG_SCHED_SMT
DEFINE_STATIC_KEY_FALSE(sched_smt_present);
EXPORT_SYMBOL_GPL(sched_smt_present);

static inline void set_idle_cores(int cpu, int val)
{
	struct sched_domain_shared *sds;

	sds = rcu_dereference(per_cpu(sd_llc_shared, cpu));
	if (sds)
		WRITE_ONCE(sds->has_idle_cores, val);
}

static inline bool test_idle_cores(int cpu, bool def)
{
	struct sched_domain_shared *sds;

	sds = rcu_dereference(per_cpu(sd_llc_shared, cpu));
	if (sds)
		return READ_ONCE(sds->has_idle_cores);

	return def;
}

/*
 * Scans the local SMT mask to see if the entire core is idle, and records this
 * information in sd_llc_shared->has_idle_cores.
 *
 * Since SMT siblings share all cache levels, inspecting this limited remote
 * state should be fairly cheap.
 */
void __update_idle_core(struct rq *rq)
{
	int core = cpu_of(rq);
	int cpu;

	rcu_read_lock();
	if (test_idle_cores(core, true))
		goto unlock;

	for_each_cpu(cpu, cpu_smt_mask(core)) {
		if (cpu == core)
			continue;

		if (!available_idle_cpu(cpu))
			goto unlock;
	}

	set_idle_cores(core, 1);
unlock:
	rcu_read_unlock();
}

/*
 * Scan the entire LLC domain for idle cores; this dynamically switches off if
 * there are no idle cores left in the system; tracked through
 * sd_llc->shared->has_idle_cores and enabled through update_idle_core() above.
 */
static int select_idle_core(struct task_struct *p, struct sched_domain *sd, int target)
{
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(select_idle_mask);
	int core, cpu;

	if (!static_branch_likely(&sched_smt_present))
		return -1;

	if (!test_idle_cores(target, false))
		return -1;

	cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr);

	for_each_cpu_wrap(core, cpus, target) {
		bool idle = true;

		for_each_cpu(cpu, cpu_smt_mask(core)) {
			__cpumask_clear_cpu(cpu, cpus);
			if (!available_idle_cpu(cpu))
				idle = false;
		}

		if (idle)
			return core;
	}

	/*
	 * Failed to find an idle core; stop looking for one.
	 */
	set_idle_cores(target, 0);

	return -1;
}

/*
 * Scan the local SMT mask for idle CPUs.
 */
static int select_idle_smt(struct task_struct *p, struct sched_domain *sd, int target)
{
	int cpu, si_cpu = -1;

	if (!static_branch_likely(&sched_smt_present))
		return -1;

	for_each_cpu(cpu, cpu_smt_mask(target)) {
		if (!cpumask_test_cpu(cpu, p->cpus_ptr) ||
		    !cpumask_test_cpu(cpu, sched_domain_span(sd)))
			continue;
		if (available_idle_cpu(cpu))
			return cpu;
		if (si_cpu == -1 && sched_idle_cpu(cpu))
			si_cpu = cpu;
	}

	return si_cpu;
}

#else /* CONFIG_SCHED_SMT */

static inline int select_idle_core(struct task_struct *p, struct sched_domain *sd, int target)
{
	return -1;
}

static inline int select_idle_smt(struct task_struct *p, struct sched_domain *sd, int target)
{
	return -1;
}

#endif /* CONFIG_SCHED_SMT */

/*
 * Scan the LLC domain for idle CPUs; this is dynamically regulated by
 * comparing the average scan cost (tracked in sd->avg_scan_cost) against the
 * average idle time for this rq (as found in rq->avg_idle).
 */
static int select_idle_cpu(struct task_struct *p, struct sched_domain *sd, int target)
{
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(select_idle_mask);
	struct sched_domain *this_sd;
	u64 avg_cost, avg_idle;
	u64 time, cost;
	s64 delta;
	int this = smp_processor_id();
	int cpu, nr = INT_MAX, si_cpu = -1;

	this_sd = rcu_dereference(*this_cpu_ptr(&sd_llc));
	if (!this_sd)
		return -1;

	/*
	 * Due to large variance we need a large fuzz factor; hackbench in
	 * particularly is sensitive here.
	 */
	avg_idle = this_rq()->avg_idle / 512;
	avg_cost = this_sd->avg_scan_cost + 1;

	if (sched_feat(SIS_AVG_CPU) && avg_idle < avg_cost)
		return -1;

	if (sched_feat(SIS_PROP)) {
		u64 span_avg = sd->span_weight * avg_idle;
		if (span_avg > 4*avg_cost)
			nr = div_u64(span_avg, avg_cost);
		else
			nr = 4;
	}

	time = cpu_clock(this);

	cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr);

	for_each_cpu_wrap(cpu, cpus, target) {
		if (!--nr)
			return si_cpu;
		if (cpu_isolated(cpu))
			continue;
		if (available_idle_cpu(cpu))
			break;
		if (si_cpu == -1 && sched_idle_cpu(cpu))
			si_cpu = cpu;
	}

	time = cpu_clock(this) - time;
	cost = this_sd->avg_scan_cost;
	delta = (s64)(time - cost) / 8;
	this_sd->avg_scan_cost += delta;

	return cpu;
}

/*
 * Scan the asym_capacity domain for idle CPUs; pick the first idle one on which
 * the task fits. If no CPU is big enough, but there are idle ones, try to
 * maximize capacity.
 */
static int
select_idle_capacity(struct task_struct *p, struct sched_domain *sd, int target)
{
	unsigned long task_util, best_cap = 0;
	int cpu, best_cpu = -1;
	struct cpumask *cpus;

	cpus = this_cpu_cpumask_var_ptr(select_idle_mask);
	cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr);

	task_util = uclamp_task_util(p);

	for_each_cpu_wrap(cpu, cpus, target) {
		unsigned long cpu_cap = capacity_of(cpu);

		if ((!available_idle_cpu(cpu) && !sched_idle_cpu(cpu)) ||
				cpu_isolated(target))
			continue;
		if (fits_capacity(task_util, cpu_cap))
			return cpu;

		if (cpu_cap > best_cap) {
			best_cap = cpu_cap;
			best_cpu = cpu;
		}
	}

	return best_cpu;
}

static inline bool asym_fits_capacity(int task_util, int cpu)
{
	if (static_branch_unlikely(&sched_asym_cpucapacity))
		return fits_capacity(task_util, capacity_of(cpu));

	return true;
}

/*
 * Try and locate an idle core/thread in the LLC cache domain.
 */
static int select_idle_sibling(struct task_struct *p, int prev, int target)
{
	struct sched_domain *sd;
	unsigned long task_util;
	int i, recent_used_cpu;

	/*
	 * On asymmetric system, update task utilization because we will check
	 * that the task fits with cpu's capacity.
	 */
	if (static_branch_unlikely(&sched_asym_cpucapacity)) {
		sync_entity_load_avg(&p->se);
		task_util = uclamp_task_util(p);
	}

	if ((available_idle_cpu(target) || sched_idle_cpu(target)) &&
	    asym_fits_capacity(task_util, target))
		return target;

	/*
	 * If the previous CPU is cache affine and idle, don't be stupid:
	 */
	if (prev != target && cpus_share_cache(prev, target) &&
	    (available_idle_cpu(prev) || sched_idle_cpu(prev)) &&
	    asym_fits_capacity(task_util, prev))
		return prev;

	/* Check a recently used CPU as a potential idle candidate: */
	recent_used_cpu = p->recent_used_cpu;
	if (recent_used_cpu != prev &&
	    recent_used_cpu != target &&
	    cpus_share_cache(recent_used_cpu, target) &&
	    (available_idle_cpu(recent_used_cpu) || sched_idle_cpu(recent_used_cpu)) &&
	    cpumask_test_cpu(p->recent_used_cpu, p->cpus_ptr) &&
	    asym_fits_capacity(task_util, recent_used_cpu)) {
		/*
		 * Replace recent_used_cpu with prev as it is a potential
		 * candidate for the next wake:
		 */
		p->recent_used_cpu = prev;
		return recent_used_cpu;
	}

	/*
	 * For asymmetric CPU capacity systems, our domain of interest is
	 * sd_asym_cpucapacity rather than sd_llc.
	 */
	if (static_branch_unlikely(&sched_asym_cpucapacity)) {
		sd = rcu_dereference(per_cpu(sd_asym_cpucapacity, target));
		/*
		 * On an asymmetric CPU capacity system where an exclusive
		 * cpuset defines a symmetric island (i.e. one unique
		 * capacity_orig value through the cpuset), the key will be set
		 * but the CPUs within that cpuset will not have a domain with
		 * SD_ASYM_CPUCAPACITY. These should follow the usual symmetric
		 * capacity path.
		 */
		if (sd) {
			i = select_idle_capacity(p, sd, target);
			return ((unsigned)i < nr_cpumask_bits) ? i : target;
		}
	}

	sd = rcu_dereference(per_cpu(sd_llc, target));
	if (!sd)
		return target;

	i = select_idle_core(p, sd, target);
	if ((unsigned)i < nr_cpumask_bits)
		return i;

	i = select_idle_cpu(p, sd, target);
	if ((unsigned)i < nr_cpumask_bits)
		return i;

	i = select_idle_smt(p, sd, target);
	if ((unsigned)i < nr_cpumask_bits)
		return i;

	return target;
}

/*
 * cpu_util_without: compute cpu utilization without any contributions from *p
 * @cpu: the CPU which utilization is requested
 * @p: the task which utilization should be discounted
 *
 * The utilization of a CPU is defined by the utilization of tasks currently
 * enqueued on that CPU as well as tasks which are currently sleeping after an
 * execution on that CPU.
 *
 * This method returns the utilization of the specified CPU by discounting the
 * utilization of the specified task, whenever the task is currently
 * contributing to the CPU utilization.
 */
static unsigned long cpu_util_without(int cpu, struct task_struct *p)
{
#ifndef CONFIG_SCHED_WALT
	struct cfs_rq *cfs_rq;
#endif
	unsigned int util;

#ifdef CONFIG_SCHED_WALT
	/*
	 * WALT does not decay idle tasks in the same manner
	 * as PELT, so it makes little sense to subtract task
	 * utilization from cpu utilization. Instead just use
	 * cpu_util for this case.
	 */
	if (likely(p->state == TASK_WAKING))
		return cpu_util(cpu);
#endif

	/* Task has no contribution or is new */
	if (cpu != task_cpu(p) || !READ_ONCE(p->se.avg.last_update_time))
		return cpu_util(cpu);

#ifdef CONFIG_SCHED_WALT
	util = max_t(long, cpu_util(cpu) - task_util(p), 0);
#else
	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	/* Discount task's util from CPU's util */
	lsub_positive(&util, task_util(p));

	/*
	 * Covered cases:
	 *
	 * a) if *p is the only task sleeping on this CPU, then:
	 *      cpu_util (== task_util) > util_est (== 0)
	 *    and thus we return:
	 *      cpu_util_without = (cpu_util - task_util) = 0
	 *
	 * b) if other tasks are SLEEPING on this CPU, which is now exiting
	 *    IDLE, then:
	 *      cpu_util >= task_util
	 *      cpu_util > util_est (== 0)
	 *    and thus we discount *p's blocked utilization to return:
	 *      cpu_util_without = (cpu_util - task_util) >= 0
	 *
	 * c) if other tasks are RUNNABLE on that CPU and
	 *      util_est > cpu_util
	 *    then we use util_est since it returns a more restrictive
	 *    estimation of the spare capacity on that CPU, by just
	 *    considering the expected utilization of tasks already
	 *    runnable on that CPU.
	 *
	 * Cases a) and b) are covered by the above code, while case c) is
	 * covered by the following code when estimated utilization is
	 * enabled.
	 */
	if (sched_feat(UTIL_EST)) {
		unsigned int estimated =
			READ_ONCE(cfs_rq->avg.util_est.enqueued);

		/*
		 * Despite the following checks we still have a small window
		 * for a possible race, when an execl's select_task_rq_fair()
		 * races with LB's detach_task():
		 *
		 *   detach_task()
		 *     p->on_rq = TASK_ON_RQ_MIGRATING;
		 *     ---------------------------------- A
		 *     deactivate_task()                   \
		 *       dequeue_task()                     + RaceTime
		 *         util_est_dequeue()              /
		 *     ---------------------------------- B
		 *
		 * The additional check on "current == p" it's required to
		 * properly fix the execl regression and it helps in further
		 * reducing the chances for the above race.
		 */
		if (unlikely(task_on_rq_queued(p) || current == p))
			lsub_positive(&estimated, _task_util_est(p));

		util = max(util, estimated);
	}
#endif

	/*
	 * Utilization (estimated) can exceed the CPU capacity, thus let's
	 * clamp to the maximum CPU capacity to ensure consistency with
	 * the cpu_util call.
	 */
	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

/*
 * Returns the current capacity of cpu after applying both
 * cpu and freq scaling.
 */
unsigned long capacity_curr_of(int cpu)
{
	unsigned long max_cap = cpu_rq(cpu)->cpu_capacity_orig;
	unsigned long scale_freq = arch_scale_freq_capacity(cpu);

	return cap_scale(max_cap, scale_freq);
}

#ifdef CONFIG_SCHED_WALT
static inline bool walt_get_rtg_status(struct task_struct *p)
{
	struct walt_related_thread_group *grp;
	bool ret = false;

	rcu_read_lock();

	grp = task_related_thread_group(p);
	if (grp)
		ret = grp->skip_min;

	rcu_read_unlock();

	return ret;
}

static inline bool walt_task_skip_min_cpu(struct task_struct *p)
{
	return sched_boost() != CONSERVATIVE_BOOST &&
		walt_get_rtg_status(p) && p->wts.unfilter;
}

static inline bool walt_is_many_wakeup(int sibling_count_hint)
{
	return sibling_count_hint >= sysctl_sched_many_wakeup_threshold;
}

static inline bool walt_target_ok(int target_cpu, int order_index)
{
	return !((order_index != num_sched_clusters - 1) &&
		 (cpumask_weight(&cpu_array[order_index][0]) == 1) &&
		 (target_cpu == cpumask_first(&cpu_array[order_index][0])));
}

static void walt_get_indicies(struct task_struct *p, int *order_index,
		int *end_index, int task_boost, bool boosted)
{
	int i = 0;

	*order_index = 0;
	*end_index = 0;

	if (num_sched_clusters <= 1)
		return;

	if (task_boost > TASK_BOOST_ON_MID) {
		*order_index = num_sched_clusters - 1;
		return;
	}

	if (is_full_throttle_boost()) {
		*order_index = num_sched_clusters - 1;
		if ((*order_index > 1) && task_demand_fits(p,
			cpumask_first(&cpu_array[*order_index][1])))
			*end_index = 1;
		return;
	}

	if (boosted || task_boost_policy(p) == SCHED_BOOST_ON_BIG ||
		walt_task_skip_min_cpu(p))
		*order_index = 1;

	for (i = *order_index ; i < num_sched_clusters - 1; i++) {
		if (task_demand_fits(p, cpumask_first(&cpu_array[i][0])))
			break;
	}

	*order_index = i;
}

enum fastpaths {
	NONE = 0,
	SYNC_WAKEUP,
	PREV_CPU_FASTPATH,
};

static void walt_find_best_target(struct sched_domain *sd, cpumask_t *cpus,
					struct task_struct *p,
					struct find_best_target_env *fbt_env)
{
	unsigned long min_util = uclamp_task_util(p);
	long target_max_spare_cap = 0;
	unsigned long best_idle_cuml_util = ULONG_MAX;
	/* Initialise with deepest possible cstate (INT_MAX) */
	int shallowest_idle_cstate = INT_MAX;
	int best_idle_cpu = -1;
	int target_cpu = -1;
	int i, start_cpu;
	long spare_wake_cap, most_spare_wake_cap = 0;
	int most_spare_cap_cpu = -1;
	int prev_cpu = task_cpu(p);
	int unisolated_candidate = -1;
	int order_index = fbt_env->order_index, end_index = fbt_env->end_index;
	int stop_index = INT_MAX;
	int cluster;
	unsigned int target_nr_rtg_high_prio = UINT_MAX;
	bool rtg_high_prio_task = task_rtg_high_prio(p);
	cpumask_t visit_cpus;

	/* Find start CPU based on boost value */
	start_cpu = fbt_env->start_cpu;

	/*
	 * For higher capacity worth I/O tasks, stop the search
	 * at the end of higher capacity cluster(s).
	 */
	if (order_index > 0 && p->wts.iowaited) {
		stop_index = num_sched_clusters - 2;
		most_spare_wake_cap = LONG_MIN;
	}

	if (fbt_env->strict_max) {
		stop_index = 0;
		most_spare_wake_cap = LONG_MIN;
	}

	if (p->state == TASK_RUNNING)
		most_spare_wake_cap = ULONG_MAX;

	/* fast path for prev_cpu */
	if (((capacity_orig_of(prev_cpu) == capacity_orig_of(start_cpu)) ||
		asym_cap_siblings(prev_cpu, start_cpu)) &&
		!cpu_isolated(prev_cpu) && cpu_online(prev_cpu) &&
		idle_cpu(prev_cpu)) {

		if (idle_get_state_idx(cpu_rq(prev_cpu)) <= 1) {
			target_cpu = prev_cpu;

			fbt_env->fastpath = PREV_CPU_FASTPATH;
			cpumask_set_cpu(target_cpu, cpus);
			goto out;
		}
	}
	for (cluster = 0; cluster < num_sched_clusters; cluster++) {
		cpumask_and(&visit_cpus, &p->cpus_mask,
				&cpu_array[order_index][cluster]);
		for_each_cpu(i, &visit_cpus) {
			unsigned long capacity_orig = capacity_orig_of(i);
			unsigned long wake_util, new_util, new_util_cuml;
			long spare_cap;
			int idle_idx = INT_MAX;

			trace_sched_cpu_util(i);

			if (!cpu_active(i) || cpu_isolated(i))
				continue;

			if (unisolated_candidate == -1)
				unisolated_candidate = i;

			/*
			 * This CPU is the target of an active migration that's
			 * yet to complete. Avoid placing another task on it.
			 * See check_for_migration()
			 */
			if (is_reserved(i))
				continue;

			if (sched_cpu_high_irqload(i))
				continue;

			if (fbt_env->skip_cpu == i)
				continue;

			/*
			 * p's blocked utilization is still accounted for on prev_cpu
			 * so prev_cpu will receive a negative bias due to the double
			 * accounting. However, the blocked utilization may be zero.
			 */
			wake_util = cpu_util_without(i, p);
			new_util = wake_util + uclamp_task_util(p);
			spare_wake_cap = capacity_orig - wake_util;

			if (spare_wake_cap > most_spare_wake_cap) {
				most_spare_wake_cap = spare_wake_cap;
				most_spare_cap_cpu = i;
			}

			if (per_task_boost(cpu_rq(i)->curr) ==
					TASK_BOOST_STRICT_MAX)
				continue;
			/*
			 * Cumulative demand may already be accounting for the
			 * task. If so, add just the boost-utilization to
			 * the cumulative demand of the cpu.
			 */
			if (task_in_cum_window_demand(cpu_rq(i), p))
				new_util_cuml = cpu_util_cum(i, 0) +
						min_util - task_util(p);
			else
				new_util_cuml = cpu_util_cum(i, 0) + min_util;

			/*
			 * Ensure minimum capacity to grant the required boost.
			 * The target CPU can be already at a capacity level higher
			 * than the one required to boost the task.
			 */
			new_util = max(min_util, new_util);
			if (new_util > capacity_orig)
				continue;

			/*
			 * Pre-compute the maximum possible capacity we expect
			 * to have available on this CPU once the task is
			 * enqueued here.
			 */
			spare_cap = capacity_orig - new_util;

			if (idle_cpu(i))
				idle_idx = idle_get_state_idx(cpu_rq(i));

			/*
			 * Find an optimal backup IDLE CPU for non latency
			 * sensitive tasks.
			 *
			 * Looking for:
			 * - favoring shallowest idle states
			 *   i.e. avoid to wakeup deep-idle CPUs
			 *
			 * The following code path is used by non latency
			 * sensitive tasks if IDLE CPUs are available. If at
			 * least one of such CPUs are available it sets the
			 * best_idle_cpu to the most suitable idle CPU to be
			 * selected.
			 *
			 * If idle CPUs are available, favour these CPUs to
			 * improve performances by spreading tasks.
			 * Indeed, the energy_diff() computed by the caller
			 * will take care to ensure the minimization of energy
			 * consumptions without affecting performance.
			 */
			if (idle_cpu(i)) {
				/*
				 * Prefer shallowest over deeper idle state cpu,
				 * of same capacity cpus.
				 */
				if (idle_idx > shallowest_idle_cstate)
					continue;
				if (shallowest_idle_cstate == idle_idx &&
					(best_idle_cpu == prev_cpu ||
					(i != prev_cpu &&
					new_util_cuml > best_idle_cuml_util)))
					continue;

				shallowest_idle_cstate = idle_idx;
				best_idle_cuml_util = new_util_cuml;
				best_idle_cpu = i;
				continue;
			}

			/*
			 * Consider only idle CPUs for active migration.
			 */
			if (p->state == TASK_RUNNING)
				continue;

			/*
			 * Try to spread the rtg high prio tasks so that they
			 * don't preempt each other. This is a optimisitc
			 * check assuming rtg high prio can actually preempt
			 * the current running task with the given vruntime
			 * boost.
			 */
			if (rtg_high_prio_task)  {
				if (walt_nr_rtg_high_prio(i) > target_nr_rtg_high_prio)
					continue;

				/* Favor CPUs with maximum spare capacity */
				if (walt_nr_rtg_high_prio(i) == target_nr_rtg_high_prio &&
						spare_cap < target_max_spare_cap)
					continue;

			} else {
				/* Favor CPUs with maximum spare capacity */
				if (spare_cap < target_max_spare_cap)
					continue;
			}

			target_max_spare_cap = spare_cap;
			target_nr_rtg_high_prio = walt_nr_rtg_high_prio(i);
			target_cpu = i;
		}

		if (best_idle_cpu != -1)
			break;

		if ((cluster >= end_index) && (target_cpu != -1) &&
			walt_target_ok(target_cpu, order_index))
			break;

		if (most_spare_cap_cpu != -1 && cluster >= stop_index)
			break;
	}

	walt_adjust_cpus_for_packing(p, &target_cpu, &best_idle_cpu,
				shallowest_idle_cstate, fbt_env);

	/*
	 * We set both idle and target as long as they are valid CPUs.
	 * If we don't find either, then we fallback to most_spare_cap,
	 * If we don't find most spare cap, we fallback to prev_cpu,
	 * provided that the prev_cpu is not isolated.
	 * If the prev_cpu is isolated, we fallback to unisolated_candidate.
	 */

	if (unlikely(target_cpu == -1)) {
		if (best_idle_cpu != -1)
			target_cpu = best_idle_cpu;
		else if (most_spare_cap_cpu != -1)
			target_cpu = most_spare_cap_cpu;
		else if (cpu_isolated(prev_cpu))
			target_cpu = unisolated_candidate;
	}

	if (target_cpu != -1)
		cpumask_set_cpu(target_cpu, cpus);
	if (best_idle_cpu != -1 && target_cpu != best_idle_cpu)
		cpumask_set_cpu(best_idle_cpu, cpus);
out:
	trace_sched_find_best_target(p, min_util, start_cpu,
			     best_idle_cpu, most_spare_cap_cpu,
			     target_cpu, order_index, end_index,
			     fbt_env->skip_cpu, p->state == TASK_RUNNING);
}

static inline unsigned long
cpu_util_next_walt(int cpu, struct task_struct *p, int dst_cpu)
{
	unsigned long util =
		cpu_rq(cpu)->wrq.walt_stats.cumulative_runnable_avg_scaled;
	bool queued = task_on_rq_queued(p);

	/*
	 * When task is queued,
	 * (a) The evaluating CPU (cpu) is task's current CPU. If the
	 * task is migrating, discount the task contribution from the
	 * evaluation cpu.
	 * (b) The evaluating CPU (cpu) is task's current CPU. If the
	 * task is NOT migrating, nothing to do. The contribution is
	 * already present on the evaluation CPU.
	 * (c) The evaluating CPU (cpu) is not task's current CPU. But
	 * the task is migrating to the evaluating CPU. So add the
	 * task contribution to it.
	 * (d) The evaluating CPU (cpu) is neither the current CPU nor
	 * the destination CPU. don't care.
	 *
	 * When task is NOT queued i.e waking. Task contribution is not
	 * present on any CPU.
	 *
	 * (a) If the evaluating CPU is the destination CPU, add the task
	 * contribution.
	 * (b) The evaluation CPU is not the destination CPU, don't care.
	 */
	if (unlikely(queued)) {
		if (task_cpu(p) == cpu) {
			if (dst_cpu != cpu)
				util = max_t(long, util - task_util(p), 0);
		} else if (dst_cpu == cpu) {
			util += task_util(p);
		}
	} else if (dst_cpu == cpu) {
		util += task_util(p);
	}

	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

#else
/*
 * Predicts what cpu_util(@cpu) would return if @p was migrated (and enqueued)
 * to @dst_cpu.
 */
static unsigned long cpu_util_next(int cpu, struct task_struct *p, int dst_cpu)
{
	struct cfs_rq *cfs_rq = &cpu_rq(cpu)->cfs;
	unsigned long util_est, util = READ_ONCE(cfs_rq->avg.util_avg);

	/*
	 * If @p migrates from @cpu to another, remove its contribution. Or,
	 * if @p migrates from another CPU to @cpu, add its contribution. In
	 * the other cases, @cpu is not impacted by the migration, so the
	 * util_avg should already be correct.
	 */
	if (task_cpu(p) == cpu && dst_cpu != cpu)
		sub_positive(&util, task_util(p));
	else if (task_cpu(p) != cpu && dst_cpu == cpu)
		util += task_util(p);

	if (sched_feat(UTIL_EST)) {
		util_est = READ_ONCE(cfs_rq->avg.util_est.enqueued);

		/*
		 * During wake-up, the task isn't enqueued yet and doesn't
		 * appear in the cfs_rq->avg.util_est.enqueued of any rq,
		 * so just add it (if needed) to "simulate" what will be
		 * cpu_util() after the task has been enqueued.
		 */
		if (dst_cpu == cpu)
			util_est += _task_util_est(p);

		util = max(util, util_est);
	}

	return min(util, capacity_orig_of(cpu));
}
#endif

/*
 * compute_energy(): Estimates the energy that @pd would consume if @p was
 * migrated to @dst_cpu. compute_energy() predicts what will be the utilization
 * landscape of @pd's CPUs after the task migration, and uses the Energy Model
 * to compute what would be the energy if we decided to actually migrate that
 * task.
 */
static long
compute_energy(struct task_struct *p, int dst_cpu, struct perf_domain *pd)
{
	struct cpumask *pd_mask = perf_domain_span(pd);
#ifndef CONFIG_SCHED_WALT
	unsigned long cpu_cap = arch_scale_cpu_capacity(cpumask_first(pd_mask));
#endif
	unsigned long max_util = 0, sum_util = 0;
	unsigned long energy = 0;
	int cpu;
	unsigned long cpu_util;

	/*
	 * The capacity state of CPUs of the current rd can be driven by CPUs
	 * of another rd if they belong to the same pd. So, account for the
	 * utilization of these CPUs too by masking pd with cpu_online_mask
	 * instead of the rd span.
	 *
	 * If an entire pd is outside of the current rd, it will not appear in
	 * its pd list and will not be accounted by compute_energy().
	 */
	for_each_cpu_and(cpu, pd_mask, cpu_online_mask) {
#ifdef CONFIG_SCHED_WALT
		cpu_util = cpu_util_next_walt(cpu, p, dst_cpu);
		sum_util += cpu_util;
#else
		unsigned long util_cfs = cpu_util_next(cpu, p, dst_cpu);
		struct task_struct *tsk = cpu == dst_cpu ? p : NULL;

		/*
		 * Busy time computation: utilization clamping is not
		 * required since the ratio (sum_util / cpu_capacity)
		 * is already enough to scale the EM reported power
		 * consumption at the (eventually clamped) cpu_capacity.
		 */
		sum_util += schedutil_cpu_util(cpu, util_cfs, cpu_cap,
					       ENERGY_UTIL, NULL);

		/*
		 * Performance domain frequency: utilization clamping
		 * must be considered since it affects the selection
		 * of the performance domain frequency.
		 * NOTE: in case RT tasks are running, by default the
		 * FREQUENCY_UTIL's utilization can be max OPP.
		 */
		cpu_util = schedutil_cpu_util(cpu, util_cfs, cpu_cap,
					      FREQUENCY_UTIL, tsk);
#endif
		max_util = max(max_util, cpu_util);
	}

	trace_android_vh_em_pd_energy(pd->em_pd, max_util, sum_util, &energy);
	if (!energy)
		energy = em_pd_energy(pd->em_pd, max_util, sum_util);

	return energy;
}

#ifdef CONFIG_SCHED_WALT
static inline int wake_to_idle(struct task_struct *p)
{
	return (current->wts.wake_up_idle || p->wts.wake_up_idle);
}
#else
static inline int wake_to_idle(struct task_struct *p)
{
	return 0;
}
#endif

/* return true if cpu should be chosen over best_energy_cpu */
static inline bool select_cpu_same_energy(int cpu, int best_cpu, int prev_cpu)
{
	if (capacity_orig_of(cpu) < capacity_orig_of(best_cpu))
		return true;

	if (best_cpu == prev_cpu)
		return false;

	if (idle_cpu(best_cpu) && idle_get_state_idx(cpu_rq(best_cpu)) <= 0)
		return false; /* best_cpu is idle wfi or shallower */

	if (idle_cpu(cpu) && idle_get_state_idx(cpu_rq(cpu)) <= 0)
		return true; /* new cpu is idle wfi or shallower */

	/*
	 * If we are this far this must be a tie between a busy and deep idle,
	 * pick the busy.
	 */
	return idle_cpu(best_cpu);
}

/*
 * find_energy_efficient_cpu(): Find most energy-efficient target CPU for the
 * waking task. find_energy_efficient_cpu() looks for the CPU with maximum
 * spare capacity in each performance domain and uses it as a potential
 * candidate to execute the task. Then, it uses the Energy Model to figure
 * out which of the CPU candidates is the most energy-efficient.
 *
 * The rationale for this heuristic is as follows. In a performance domain,
 * all the most energy efficient CPU candidates (according to the Energy
 * Model) are those for which we'll request a low frequency. When there are
 * several CPUs for which the frequency request will be the same, we don't
 * have enough data to break the tie between them, because the Energy Model
 * only includes active power costs. With this model, if we assume that
 * frequency requests follow utilization (e.g. using schedutil), the CPU with
 * the maximum spare capacity in a performance domain is guaranteed to be among
 * the best candidates of the performance domain.
 *
 * In practice, it could be preferable from an energy standpoint to pack
 * small tasks on a CPU in order to let other CPUs go in deeper idle states,
 * but that could also hurt our chances to go cluster idle, and we have no
 * ways to tell with the current Energy Model if this is actually a good
 * idea or not. So, find_energy_efficient_cpu() basically favors
 * cluster-packing, and spreading inside a cluster. That should at least be
 * a good thing for latency, and this is consistent with the idea that most
 * of the energy savings of EAS come from the asymmetry of the system, and
 * not so much from breaking the tie between identical CPUs. That's also the
 * reason why EAS is enabled in the topology code only for systems where
 * SD_ASYM_CPUCAPACITY is set.
 *
 * NOTE: Forkees are not accepted in the energy-aware wake-up path because
 * they don't have any useful utilization data yet and it's not possible to
 * forecast their impact on energy consumption. Consequently, they will be
 * placed by find_idlest_cpu() on the least loaded CPU, which might turn out
 * to be energy-inefficient in some use-cases. The alternative would be to
 * bias new tasks towards specific types of CPUs first, or to try to infer
 * their util_avg from the parent task, but those heuristics could hurt
 * other use-cases too. So, until someone finds a better way to solve this,
 * let's keep things simple by re-using the existing slow path.
 */
#ifdef CONFIG_SCHED_WALT
static DEFINE_PER_CPU(cpumask_t, energy_cpus);
int find_energy_efficient_cpu(struct task_struct *p, int prev_cpu,
				     int sync, int sibling_count_hint)
{
	unsigned long prev_delta = ULONG_MAX, best_delta = ULONG_MAX;
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;
	int weight, cpu = smp_processor_id(), best_energy_cpu = prev_cpu;
	struct perf_domain *pd;
	unsigned long cur_energy;
	cpumask_t *candidates;
	bool is_rtg, curr_is_rtg;
	struct find_best_target_env fbt_env;
	bool need_idle = wake_to_idle(p) || uclamp_latency_sensitive(p);
	u64 start_t = 0;
	int delta = 0;
	int task_boost = per_task_boost(p);
	bool is_uclamp_boosted = uclamp_boosted(p);
	bool boosted = is_uclamp_boosted || (task_boost > 0);
	int start_cpu, order_index, end_index;

	if (walt_is_many_wakeup(sibling_count_hint) && prev_cpu != cpu &&
			cpumask_test_cpu(prev_cpu, &p->cpus_mask))
		return prev_cpu;

	if (unlikely(!cpu_array))
		goto eas_not_ready;

	walt_get_indicies(p, &order_index, &end_index, task_boost, boosted);
	start_cpu = cpumask_first(&cpu_array[order_index][0]);

	is_rtg = task_in_related_thread_group(p);
	curr_is_rtg = task_in_related_thread_group(cpu_rq(cpu)->curr);

	fbt_env.fastpath = 0;
	fbt_env.need_idle = need_idle;

	if (trace_sched_task_util_enabled())
		start_t = sched_clock();

	/* Pre-select a set of candidate CPUs. */
	candidates = this_cpu_ptr(&energy_cpus);
	cpumask_clear(candidates);

	if (sync && (need_idle || (is_rtg && curr_is_rtg)))
		sync = 0;

	if (sysctl_sched_sync_hint_enable && sync
			&& bias_to_this_cpu(p, cpu, start_cpu)) {
		best_energy_cpu = cpu;
		fbt_env.fastpath = SYNC_WAKEUP;
		goto done;
	}

	rcu_read_lock();
	pd = rcu_dereference(rd->pd);
	if (!pd)
		goto fail;

	fbt_env.is_rtg = is_rtg;
	fbt_env.start_cpu = start_cpu;
	fbt_env.order_index = order_index;
	fbt_env.end_index = end_index;
	fbt_env.boosted = boosted;
	fbt_env.strict_max = is_rtg &&
		(task_boost == TASK_BOOST_STRICT_MAX);
	fbt_env.skip_cpu = walt_is_many_wakeup(sibling_count_hint) ?
			   cpu : -1;

	walt_find_best_target(NULL, candidates, p, &fbt_env);

	/* Bail out if no candidate was found. */
	weight = cpumask_weight(candidates);
	if (!weight)
		goto unlock;

	/* If there is only one sensible candidate, select it now. */
	cpu = cpumask_first(candidates);
	if (weight == 1 && (idle_cpu(cpu) || cpu == prev_cpu)) {
		best_energy_cpu = cpu;
		goto unlock;
	}

	if (p->state == TASK_WAKING)
		delta = task_util(p);

	if (task_placement_boost_enabled(p) || fbt_env.need_idle ||
	    boosted || is_rtg || __cpu_overutilized(prev_cpu, delta) ||
	    !task_fits_max(p, prev_cpu) || cpu_isolated(prev_cpu)) {
		best_energy_cpu = cpu;
		goto unlock;
	}

	if (cpumask_test_cpu(prev_cpu, &p->cpus_mask))
		prev_delta = best_delta =
				compute_energy(p, prev_cpu, pd);
	else
		prev_delta = best_delta = ULONG_MAX;

	/* Select the best candidate energy-wise. */
	for_each_cpu(cpu, candidates) {
		if (cpu == prev_cpu)
			continue;

		cur_energy = compute_energy(p, cpu, pd);
		trace_sched_compute_energy(p, cpu, cur_energy,
			prev_delta, best_delta, best_energy_cpu);

		if (cur_energy < best_delta) {
			best_delta = cur_energy;
			best_energy_cpu = cpu;
		} else if (cur_energy == best_delta) {
			if (select_cpu_same_energy(cpu, best_energy_cpu,
							prev_cpu)) {
				best_delta = cur_energy;
				best_energy_cpu = cpu;
			}
		}
	}

unlock:
	rcu_read_unlock();

	/*
	 * Pick the prev CPU, if best energy CPU can't saves at least 6% of
	 * the energy used by prev_cpu.
	 */
	if (!(idle_cpu(best_energy_cpu) &&
	    idle_get_state_idx(cpu_rq(best_energy_cpu)) <= 0) &&
	    (prev_delta != ULONG_MAX) && (best_energy_cpu != prev_cpu)  &&
	    ((prev_delta - best_delta) <= prev_delta >> 4) &&
	    (capacity_orig_of(prev_cpu) <= capacity_orig_of(start_cpu)))
		best_energy_cpu = prev_cpu;

done:
	trace_sched_task_util(p, cpumask_bits(candidates)[0], best_energy_cpu,
			sync, fbt_env.need_idle, fbt_env.fastpath,
			task_boost_policy(p), start_t, boosted, is_rtg,
			walt_get_rtg_status(p), start_cpu);

	return best_energy_cpu;

fail:
	rcu_read_unlock();

eas_not_ready:
	return -EPERM;
}
#else
int find_energy_efficient_cpu(struct task_struct *p, int prev_cpu,
		int sync, __attribute__((unused))int sibling_count_hint)
{
	unsigned long prev_delta = ULONG_MAX, best_delta = ULONG_MAX;
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;
	int max_spare_cap_cpu_ls = prev_cpu, best_idle_cpu = -1;
	unsigned long max_spare_cap_ls = 0, target_cap;
	unsigned long cpu_cap, util, base_energy = 0;
	bool boosted, latency_sensitive = false;
	unsigned int min_exit_lat = UINT_MAX;
	int cpu, best_energy_cpu = prev_cpu;
	struct cpuidle_state *idle;
	struct sched_domain *sd;
	struct perf_domain *pd;

	rcu_read_lock();
	pd = rcu_dereference(rd->pd);
	if (!pd || READ_ONCE(rd->overutilized))
		goto fail;
	cpu = smp_processor_id();
	if (sync && cpu_rq(cpu)->nr_running == 1 &&
			cpumask_test_cpu(cpu, p->cpus_ptr)) {
		rcu_read_unlock();
		return cpu;
	}
	/*
	 * Energy-aware wake-up happens on the lowest sched_domain starting
	 * from sd_asym_cpucapacity spanning over this_cpu and prev_cpu.
	 */
	sd = rcu_dereference(*this_cpu_ptr(&sd_asym_cpucapacity));
	while (sd && !cpumask_test_cpu(prev_cpu, sched_domain_span(sd)))
		sd = sd->parent;
	if (!sd)
		goto fail;
	sync_entity_load_avg(&p->se);
	if (!task_util_est(p))
		goto unlock;
	latency_sensitive = uclamp_latency_sensitive(p);
	boosted = uclamp_boosted(p);
	target_cap = boosted ? 0 : ULONG_MAX;
	for (; pd; pd = pd->next) {
		unsigned long cur_delta, spare_cap, max_spare_cap = 0;
		unsigned long base_energy_pd;
		int max_spare_cap_cpu = -1;
		/* Compute the 'base' energy of the pd, without @p */
		base_energy_pd = compute_energy(p, -1, pd);
		base_energy += base_energy_pd;
		for_each_cpu_and(cpu, perf_domain_span(pd),
				 sched_domain_span(sd)) {
			if (!cpumask_test_cpu(cpu, p->cpus_ptr))
				continue;
			util = cpu_util_next(cpu, p, cpu);
			cpu_cap = capacity_of(cpu);
			spare_cap = cpu_cap;
			lsub_positive(&spare_cap, util);

			/*
			 * Skip CPUs that cannot satisfy the capacity request.
			 * IOW, placing the task there would make the CPU
			 * overutilized. Take uclamp into account to see how
			 * much capacity we can get out of the CPU; this is
			 * aligned with schedutil_cpu_util().
			 */
			util = uclamp_rq_util_with(cpu_rq(cpu), util, p);
			if (!fits_capacity(util, cpu_cap))
				continue;
			/* Always use prev_cpu as a candidate. */
			if (!latency_sensitive && cpu == prev_cpu) {
				prev_delta = compute_energy(p, prev_cpu, pd);
				prev_delta -= base_energy_pd;
				best_delta = min(best_delta, prev_delta);
			}
			/*
			 * Find the CPU with the maximum spare capacity in
			 * the performance domain
			 */
			if (spare_cap > max_spare_cap) {
				max_spare_cap = spare_cap;
				max_spare_cap_cpu = cpu;
			}
			if (!latency_sensitive)
				continue;
			if (idle_cpu(cpu)) {
				cpu_cap = capacity_orig_of(cpu);
				if (boosted && cpu_cap < target_cap)
					continue;
				if (!boosted && cpu_cap > target_cap)
					continue;
				idle = idle_get_state(cpu_rq(cpu));
				if (idle && idle->exit_latency > min_exit_lat &&
						cpu_cap == target_cap)
					continue;
				if (idle)
					min_exit_lat = idle->exit_latency;
				target_cap = cpu_cap;
				best_idle_cpu = cpu;
			} else if (spare_cap > max_spare_cap_ls) {
				max_spare_cap_ls = spare_cap;
				max_spare_cap_cpu_ls = cpu;
			}
		}
		/* Evaluate the energy impact of using this CPU. */
		if (!latency_sensitive && max_spare_cap_cpu >= 0 &&
				max_spare_cap_cpu != prev_cpu) {
			cur_delta = compute_energy(p, max_spare_cap_cpu, pd);
			cur_delta -= base_energy_pd;
			if (cur_delta < best_delta) {
				best_delta = cur_delta;
				best_energy_cpu = max_spare_cap_cpu;
			}
		}
	}
unlock:
	rcu_read_unlock();
	if (latency_sensitive)
		return best_idle_cpu >= 0 ?
			best_idle_cpu : max_spare_cap_cpu_ls;
	/*
	 * Pick the best CPU if prev_cpu cannot be used, or if it saves at
	 * least 6% of the energy used by prev_cpu.
	 */
	if (prev_delta == ULONG_MAX)
		return best_energy_cpu;
	if ((prev_delta - best_delta) > ((prev_delta + base_energy) >> 4))
		return best_energy_cpu;
	return prev_cpu;
fail:
	rcu_read_unlock();
	return -EPERM;
}
#endif
/*
 * select_task_rq_fair: Select target runqueue for the waking task in domains
 * that have the 'sd_flag' flag set. In practice, this is SD_BALANCE_WAKE,
 * SD_BALANCE_FORK, or SD_BALANCE_EXEC.
 *
 * Balances load by selecting the idlest CPU in the idlest group, or under
 * certain conditions an idle sibling CPU if the domain has SD_WAKE_AFFINE set.
 *
 * Returns the target CPU number.
 *
 * preempt must be disabled.
 */
static int
#ifdef CONFIG_SCHED_WALT
select_task_rq_fair(struct task_struct *p, int prev_cpu, int sd_flag, int wake_flags,
		    int sibling_count_hint)
#else
select_task_rq_fair(struct task_struct *p, int prev_cpu, int sd_flag, int wake_flags)
#endif
{
	struct sched_domain *tmp, *sd = NULL;
	int cpu = smp_processor_id();
	int new_cpu = prev_cpu;
	int want_affine = 0;
	int sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	int target_cpu = -1;

	trace_android_rvh_select_task_rq_fair(p, prev_cpu, sd_flag,
			wake_flags, &target_cpu);
	if (target_cpu >= 0)
		return target_cpu;

	if (sched_energy_enabled()) {
		rcu_read_lock();
#ifdef CONFIG_SCHED_WALT
		new_cpu = find_energy_efficient_cpu(p, prev_cpu, sync,
						    sibling_count_hint);
#else
		new_cpu = find_energy_efficient_cpu(p, prev_cpu, sync, 1);
#endif
		if (unlikely(new_cpu < 0))
			new_cpu = prev_cpu;
		rcu_read_unlock();
		return new_cpu;
	}

	if (sd_flag & SD_BALANCE_WAKE) {
		record_wakee(p);

		if (sched_energy_enabled()) {
#ifdef CONFIG_SCHED_WALT
			new_cpu = find_energy_efficient_cpu(p, prev_cpu, sync,
							    sibling_count_hint);
#else
			new_cpu = find_energy_efficient_cpu(p, prev_cpu, sync, 1);
#endif
			if (new_cpu >= 0)
				return new_cpu;
			new_cpu = prev_cpu;
		}

#ifdef CONFIG_SCHED_WALT
		want_affine = !wake_wide(p, sibling_count_hint) &&
			      cpumask_test_cpu(cpu, p->cpus_ptr);
#else
		want_affine = !wake_wide(p, 1) &&
			      cpumask_test_cpu(cpu, p->cpus_ptr);
#endif
	}

	rcu_read_lock();
	for_each_domain(cpu, tmp) {
		if (!(tmp->flags & SD_LOAD_BALANCE))
			break;

		/*
		 * If both 'cpu' and 'prev_cpu' are part of this domain,
		 * cpu is a valid SD_WAKE_AFFINE target.
		 */
		if (want_affine && (tmp->flags & SD_WAKE_AFFINE) &&
		    cpumask_test_cpu(prev_cpu, sched_domain_span(tmp))) {
			if (cpu != prev_cpu)
				new_cpu = wake_affine(tmp, p, cpu, prev_cpu, sync);

			sd = NULL; /* Prefer wake_affine over balance flags */
			break;
		}

		if (tmp->flags & sd_flag)
			sd = tmp;
		else if (!want_affine)
			break;
	}

	if (unlikely(sd)) {
		/* Slow path */
		new_cpu = find_idlest_cpu(sd, p, cpu, prev_cpu, sd_flag);
	} else if (sd_flag & SD_BALANCE_WAKE) { /* XXX always ? */
		/* Fast path */

		new_cpu = select_idle_sibling(p, prev_cpu, new_cpu);

		if (want_affine)
			current->recent_used_cpu = cpu;
	}
	rcu_read_unlock();

	return new_cpu;
}

static void detach_entity_cfs_rq(struct sched_entity *se);

/*
 * Called immediately before a task is migrated to a new CPU; task_cpu(p) and
 * cfs_rq_of(p) references at time of call are still valid and identify the
 * previous CPU. The caller guarantees p->pi_lock or task_rq(p)->lock is held.
 */
static void migrate_task_rq_fair(struct task_struct *p, int new_cpu)
{
	/*
	 * As blocked tasks retain absolute vruntime the migration needs to
	 * deal with this by subtracting the old and adding the new
	 * min_vruntime -- the latter is done by enqueue_entity() when placing
	 * the task on the new runqueue.
	 */
	if (p->state == TASK_WAKING) {
		struct sched_entity *se = &p->se;
		struct cfs_rq *cfs_rq = cfs_rq_of(se);
		u64 min_vruntime;

#ifndef CONFIG_64BIT
		u64 min_vruntime_copy;

		do {
			min_vruntime_copy = cfs_rq->min_vruntime_copy;
			smp_rmb();
			min_vruntime = cfs_rq->min_vruntime;
		} while (min_vruntime != min_vruntime_copy);
#else
		min_vruntime = cfs_rq->min_vruntime;
#endif

		se->vruntime -= min_vruntime;
	}

	if (p->on_rq == TASK_ON_RQ_MIGRATING) {
		/*
		 * In case of TASK_ON_RQ_MIGRATING we in fact hold the 'old'
		 * rq->lock and can modify state directly.
		 */
		lockdep_assert_held(&task_rq(p)->lock);
		detach_entity_cfs_rq(&p->se);

	} else {
		/*
		 * We are supposed to update the task to "current" time, then
		 * its up to date and ready to go to new CPU/cfs_rq. But we
		 * have difficulty in getting what current time is, so simply
		 * throw away the out-of-date time. This will result in the
		 * wakee task is less decayed, but giving the wakee more load
		 * sounds not bad.
		 */
		remove_entity_load_avg(&p->se);
	}

	/* Tell new CPU we are migrated */
	p->se.avg.last_update_time = 0;

	update_scan_period(p, new_cpu);
}

static void task_dead_fair(struct task_struct *p)
{
	remove_entity_load_avg(&p->se);
}

static int
balance_fair(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	if (rq->nr_running)
		return 1;

	return sched_balance_newidle(rq, rf) != 0;
}
#endif /* CONFIG_SMP */

static unsigned long wakeup_gran(struct sched_entity *se)
{
	unsigned long gran = sysctl_sched_wakeup_granularity;

	/*
	 * Since its curr running now, convert the gran from real-time
	 * to virtual-time in his units.
	 *
	 * By using 'se' instead of 'curr' we penalize light tasks, so
	 * they get preempted easier. That is, if 'se' < 'curr' then
	 * the resulting gran will be larger, therefore penalizing the
	 * lighter, if otoh 'se' > 'curr' then the resulting gran will
	 * be smaller, again penalizing the lighter task.
	 *
	 * This is especially important for buddies when the leftmost
	 * task is higher priority than the buddy.
	 */
	return calc_delta_fair(gran, se);
}

/*
 * Should 'se' preempt 'curr'.
 *
 *             |s1
 *        |s2
 *   |s3
 *         g
 *      |<--->|c
 *
 *  w(c, s1) = -1
 *  w(c, s2) =  0
 *  w(c, s3) =  1
 *
 */
static int
wakeup_preempt_entity(struct sched_entity *curr, struct sched_entity *se)
{
	s64 gran, vdiff = curr->vruntime - se->vruntime;

	if (vdiff <= 0)
		return -1;

	gran = wakeup_gran(se);
	if (vdiff > gran)
		return 1;

	return 0;
}

static void set_last_buddy(struct sched_entity *se)
{
	if (entity_is_task(se) && unlikely(task_has_idle_policy(task_of(se))))
		return;

	for_each_sched_entity(se) {
		if (SCHED_WARN_ON(!se->on_rq))
			return;
		cfs_rq_of(se)->last = se;
	}
}

static void set_next_buddy(struct sched_entity *se)
{
	if (entity_is_task(se) && unlikely(task_has_idle_policy(task_of(se))))
		return;

	for_each_sched_entity(se) {
		if (SCHED_WARN_ON(!se->on_rq))
			return;
		cfs_rq_of(se)->next = se;
	}
}

static void set_skip_buddy(struct sched_entity *se)
{
	for_each_sched_entity(se)
		cfs_rq_of(se)->skip = se;
}

/*
 * Preempt the current task with a newly woken task if needed:
 */
static void check_preempt_wakeup(struct rq *rq, struct task_struct *p, int wake_flags)
{
	struct task_struct *curr = rq->curr;
	struct sched_entity *se = &curr->se, *pse = &p->se;
	struct cfs_rq *cfs_rq = task_cfs_rq(curr);
	int scale = cfs_rq->nr_running >= sched_nr_latency;
	int next_buddy_marked = 0;

	if (unlikely(se == pse))
		return;

	/*
	 * This is possible from callers such as attach_tasks(), in which we
	 * unconditionally check_prempt_curr() after an enqueue (which may have
	 * lead to a throttle).  This both saves work and prevents false
	 * next-buddy nomination below.
	 */
	if (unlikely(throttled_hierarchy(cfs_rq_of(pse))))
		return;

	if (sched_feat(NEXT_BUDDY) && scale && !(wake_flags & WF_FORK)) {
		set_next_buddy(pse);
		next_buddy_marked = 1;
	}

	/*
	 * We can come here with TIF_NEED_RESCHED already set from new task
	 * wake up path.
	 *
	 * Note: this also catches the edge-case of curr being in a throttled
	 * group (e.g. via set_curr_task), since update_curr() (in the
	 * enqueue of curr) will have resulted in resched being set.  This
	 * prevents us from potentially nominating it as a false LAST_BUDDY
	 * below.
	 */
	if (test_tsk_need_resched(curr))
		return;

	/* Idle tasks are by definition preempted by non-idle tasks. */
	if (unlikely(task_has_idle_policy(curr)) &&
	    likely(!task_has_idle_policy(p)))
		goto preempt;

	/*
	 * Batch and idle tasks do not preempt non-idle tasks (their preemption
	 * is driven by the tick):
	 */
	if (unlikely(p->policy != SCHED_NORMAL) || !sched_feat(WAKEUP_PREEMPTION))
		return;

	find_matching_se(&se, &pse);
	update_curr(cfs_rq_of(se));
	BUG_ON(!pse);
	if (wakeup_preempt_entity(se, pse) == 1) {
		/*
		 * Bias pick_next to pick the sched entity that is
		 * triggering this preemption.
		 */
		if (!next_buddy_marked)
			set_next_buddy(pse);
		goto preempt;
	}

	return;

preempt:
	resched_curr(rq);
	/*
	 * Only set the backward buddy when the current task is still
	 * on the rq. This can happen when a wakeup gets interleaved
	 * with schedule on the ->pre_schedule() or idle_balance()
	 * point, either of which can * drop the rq lock.
	 *
	 * Also, during early boot the idle thread is in the fair class,
	 * for obvious reasons its a bad idea to schedule back to it.
	 */
	if (unlikely(!se->on_rq || curr == rq->idle))
		return;

	if (sched_feat(LAST_BUDDY) && scale && entity_is_task(se))
		set_last_buddy(se);
}

static struct task_struct *
pick_next_task_fair(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct cfs_rq *cfs_rq = &rq->cfs;
	struct sched_entity *se;
	struct task_struct *p;
	int new_tasks;

again:
	if (!sched_fair_runnable(rq))
		goto idle;

#ifdef CONFIG_FAIR_GROUP_SCHED
	if (!prev || prev->sched_class != &fair_sched_class)
		goto simple;

	/*
	 * Because of the set_next_buddy() in dequeue_task_fair() it is rather
	 * likely that a next task is from the same cgroup as the current.
	 *
	 * Therefore attempt to avoid putting and setting the entire cgroup
	 * hierarchy, only change the part that actually changes.
	 */

	do {
		struct sched_entity *curr = cfs_rq->curr;

		/*
		 * Since we got here without doing put_prev_entity() we also
		 * have to consider cfs_rq->curr. If it is still a runnable
		 * entity, update_curr() will update its vruntime, otherwise
		 * forget we've ever seen it.
		 */
		if (curr) {
			if (curr->on_rq)
				update_curr(cfs_rq);
			else
				curr = NULL;

			/*
			 * This call to check_cfs_rq_runtime() will do the
			 * throttle and dequeue its entity in the parent(s).
			 * Therefore the nr_running test will indeed
			 * be correct.
			 */
			if (unlikely(check_cfs_rq_runtime(cfs_rq))) {
				cfs_rq = &rq->cfs;

				if (!cfs_rq->nr_running)
					goto idle;

				goto simple;
			}
		}

		se = pick_next_entity(cfs_rq, curr);
		cfs_rq = group_cfs_rq(se);
	} while (cfs_rq);

	p = task_of(se);

	/*
	 * Since we haven't yet done put_prev_entity and if the selected task
	 * is a different task than we started out with, try and touch the
	 * least amount of cfs_rqs.
	 */
	if (prev != p) {
		struct sched_entity *pse = &prev->se;

		while (!(cfs_rq = is_same_group(se, pse))) {
			int se_depth = se->depth;
			int pse_depth = pse->depth;

			if (se_depth <= pse_depth) {
				put_prev_entity(cfs_rq_of(pse), pse);
				pse = parent_entity(pse);
			}
			if (se_depth >= pse_depth) {
				set_next_entity(cfs_rq_of(se), se);
				se = parent_entity(se);
			}
		}

		put_prev_entity(cfs_rq, pse);
		set_next_entity(cfs_rq, se);
	}

	goto done;
simple:
#endif
	if (prev)
		put_prev_task(rq, prev);

	do {
		se = pick_next_entity(cfs_rq, NULL);
		set_next_entity(cfs_rq, se);
		cfs_rq = group_cfs_rq(se);
	} while (cfs_rq);

	p = task_of(se);

done: __maybe_unused;
#ifdef CONFIG_SMP
	/*
	 * Move the next running task to the front of
	 * the list, so our cfs_tasks list becomes MRU
	 * one.
	 */
	list_move(&p->se.group_node, &rq->cfs_tasks);
#endif

	if (hrtick_enabled(rq))
		hrtick_start_fair(rq, p);

	update_misfit_status(p, rq);

	return p;

idle:
	if (rf) {
		new_tasks = sched_balance_newidle(rq, rf);

		/*
		 * Because sched_balance_newidle() releases (and re-acquires)
		 * rq->lock, it is possible for any higher priority task to
		 * appear. In that case we must re-start the pick_next_entity()
		 * loop.
		 */
		if (new_tasks < 0)
			return RETRY_TASK;

		if (new_tasks > 0)
			goto again;
	}

	/*
	 * rq is about to be idle, check if we need to update the
	 * lost_idle_time of clock_pelt
	 */
	update_idle_rq_clock_pelt(rq);

	return NULL;
}

/*
 * Account for a descheduled task:
 */
static void put_prev_task_fair(struct rq *rq, struct task_struct *prev)
{
	struct sched_entity *se = &prev->se;
	struct cfs_rq *cfs_rq;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		put_prev_entity(cfs_rq, se);
	}
}

/*
 * sched_yield() is very simple
 *
 * The magic of dealing with the ->skip buddy is in pick_next_entity.
 */
static void yield_task_fair(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct cfs_rq *cfs_rq = task_cfs_rq(curr);
	struct sched_entity *se = &curr->se;

	/*
	 * Are we the only task in the tree?
	 */
	if (unlikely(rq->nr_running == 1))
		return;

	clear_buddies(cfs_rq, se);

	if (curr->policy != SCHED_BATCH) {
		update_rq_clock(rq);
		/*
		 * Update run-time statistics of the 'current'.
		 */
		update_curr(cfs_rq);
		/*
		 * Tell update_rq_clock() that we've just updated,
		 * so we don't do microscopic update in schedule()
		 * and double the fastpath cost.
		 */
		rq_clock_skip_update(rq);
	}

	set_skip_buddy(se);
}

static bool yield_to_task_fair(struct rq *rq, struct task_struct *p, bool preempt)
{
	struct sched_entity *se = &p->se;

	/* throttled hierarchies are not runnable */
	if (!se->on_rq || throttled_hierarchy(cfs_rq_of(se)))
		return false;

	/* Tell the scheduler that we'd really like pse to run next. */
	set_next_buddy(se);

	yield_task_fair(rq);

	return true;
}

#ifdef CONFIG_SMP
/**************************************************
 * Fair scheduling class load-balancing methods.
 *
 * BASICS
 *
 * The purpose of load-balancing is to achieve the same basic fairness the
 * per-CPU scheduler provides, namely provide a proportional amount of compute
 * time to each task. This is expressed in the following equation:
 *
 *   W_i,n/P_i == W_j,n/P_j for all i,j                               (1)
 *
 * Where W_i,n is the n-th weight average for CPU i. The instantaneous weight
 * W_i,0 is defined as:
 *
 *   W_i,0 = \Sum_j w_i,j                                             (2)
 *
 * Where w_i,j is the weight of the j-th runnable task on CPU i. This weight
 * is derived from the nice value as per sched_prio_to_weight[].
 *
 * The weight average is an exponential decay average of the instantaneous
 * weight:
 *
 *   W'_i,n = (2^n - 1) / 2^n * W_i,n + 1 / 2^n * W_i,0               (3)
 *
 * C_i is the compute capacity of CPU i, typically it is the
 * fraction of 'recent' time available for SCHED_OTHER task execution. But it
 * can also include other factors [XXX].
 *
 * To achieve this balance we define a measure of imbalance which follows
 * directly from (1):
 *
 *   imb_i,j = max{ avg(W/C), W_i/C_i } - min{ avg(W/C), W_j/C_j }    (4)
 *
 * We them move tasks around to minimize the imbalance. In the continuous
 * function space it is obvious this converges, in the discrete case we get
 * a few fun cases generally called infeasible weight scenarios.
 *
 * [XXX expand on:
 *     - infeasible weights;
 *     - local vs global optima in the discrete case. ]
 *
 *
 * SCHED DOMAINS
 *
 * In order to solve the imbalance equation (4), and avoid the obvious O(n^2)
 * for all i,j solution, we create a tree of CPUs that follows the hardware
 * topology where each level pairs two lower groups (or better). This results
 * in O(log n) layers. Furthermore we reduce the number of CPUs going up the
 * tree to only the first of the previous level and we decrease the frequency
 * of load-balance at each level inv. proportional to the number of CPUs in
 * the groups.
 *
 * This yields:
 *
 *     log_2 n     1     n
 *   \Sum       { --- * --- * 2^i } = O(n)                            (5)
 *     i = 0      2^i   2^i
 *                               `- size of each group
 *         |         |     `- number of CPUs doing load-balance
 *         |         `- freq
 *         `- sum over all levels
 *
 * Coupled with a limit on how many tasks we can migrate every balance pass,
 * this makes (5) the runtime complexity of the balancer.
 *
 * An important property here is that each CPU is still (indirectly) connected
 * to every other CPU in at most O(log n) steps:
 *
 * The adjacency matrix of the resulting graph is given by:
 *
 *             log_2 n
 *   A_i,j = \Union     (i % 2^k == 0) && i / 2^(k+1) == j / 2^(k+1)  (6)
 *             k = 0
 *
 * And you'll find that:
 *
 *   A^(log_2 n)_i,j != 0  for all i,j                                (7)
 *
 * Showing there's indeed a path between every CPU in at most O(log n) steps.
 * The task movement gives a factor of O(m), giving a convergence complexity
 * of:
 *
 *   O(nm log n),  n := nr_cpus, m := nr_tasks                        (8)
 *
 *
 * WORK CONSERVING
 *
 * In order to avoid CPUs going idle while there's still work to do, new idle
 * balancing is more aggressive and has the newly idle CPU iterate up the domain
 * tree itself instead of relying on other CPUs to bring it work.
 *
 * This adds some complexity to both (5) and (8) but it reduces the total idle
 * time.
 *
 * [XXX more?]
 *
 *
 * CGROUPS
 *
 * Cgroups make a horror show out of (2), instead of a simple sum we get:
 *
 *                                s_k,i
 *   W_i,0 = \Sum_j \Prod_k w_k * -----                               (9)
 *                                 S_k
 *
 * Where
 *
 *   s_k,i = \Sum_j w_i,j,k  and  S_k = \Sum_i s_k,i                 (10)
 *
 * w_i,j,k is the weight of the j-th runnable task in the k-th cgroup on CPU i.
 *
 * The big problem is S_k, its a global sum needed to compute a local (W_i)
 * property.
 *
 * [XXX write more on how we solve this.. _after_ merging pjt's patches that
 *      rewrite all of this once again.]
 */

static unsigned long __read_mostly max_load_balance_interval = HZ/10;

enum fbq_type { regular, remote, all };

enum group_type {
	group_other = 0,
	group_misfit_task,
	group_imbalanced,
	group_overloaded,
};

#define LBF_ALL_PINNED	0x01
#define LBF_NEED_BREAK	0x02
#define LBF_DST_PINNED  0x04
#define LBF_SOME_PINNED	0x08
#define LBF_NOHZ_STATS	0x10
#define LBF_NOHZ_AGAIN	0x20
#define LBF_IGNORE_BIG_TASKS 0x100
#define LBF_IGNORE_PREFERRED_CLUSTER_TASKS 0x200

struct lb_env {
	struct sched_domain	*sd;

	struct rq		*src_rq;
	int			src_cpu;

	int			dst_cpu;
	struct rq		*dst_rq;

	struct cpumask		*dst_grpmask;
	int			new_dst_cpu;
	enum cpu_idle_type	idle;
	long			imbalance;
	unsigned int		src_grp_nr_running;
	/* The set of CPUs under consideration for load-balancing */
	struct cpumask		*cpus;

	unsigned int		flags;

	unsigned int		loop;
	unsigned int		loop_break;
	unsigned int		loop_max;
	bool			prefer_spread;

	enum fbq_type		fbq_type;
	enum group_type		src_grp_type;
	struct list_head	tasks;
};

/*
 * Is this task likely cache-hot:
 */
static int task_hot(struct task_struct *p, struct lb_env *env)
{
	s64 delta;

	lockdep_assert_held(&env->src_rq->lock);

	if (p->sched_class != &fair_sched_class)
		return 0;

	if (unlikely(task_has_idle_policy(p)))
		return 0;

	/*
	 * Buddy candidates are cache hot:
	 */
	if (sched_feat(CACHE_HOT_BUDDY) && env->dst_rq->nr_running &&
			(&p->se == cfs_rq_of(&p->se)->next ||
			 &p->se == cfs_rq_of(&p->se)->last))
		return 1;

	if (sysctl_sched_migration_cost == -1)
		return 1;
	if (sysctl_sched_migration_cost == 0)
		return 0;

	delta = rq_clock_task(env->src_rq) - p->se.exec_start;

	return delta < (s64)sysctl_sched_migration_cost;
}

#ifdef CONFIG_NUMA_BALANCING
/*
 * Returns 1, if task migration degrades locality
 * Returns 0, if task migration improves locality i.e migration preferred.
 * Returns -1, if task migration is not affected by locality.
 */
static int migrate_degrades_locality(struct task_struct *p, struct lb_env *env)
{
	struct numa_group *numa_group = rcu_dereference(p->numa_group);
	unsigned long src_weight, dst_weight;
	int src_nid, dst_nid, dist;

	if (!static_branch_likely(&sched_numa_balancing))
		return -1;

	if (!p->numa_faults || !(env->sd->flags & SD_NUMA))
		return -1;

	src_nid = cpu_to_node(env->src_cpu);
	dst_nid = cpu_to_node(env->dst_cpu);

	if (src_nid == dst_nid)
		return -1;

	/* Migrating away from the preferred node is always bad. */
	if (src_nid == p->numa_preferred_nid) {
		if (env->src_rq->nr_running > env->src_rq->nr_preferred_running)
			return 1;
		else
			return -1;
	}

	/* Encourage migration to the preferred node. */
	if (dst_nid == p->numa_preferred_nid)
		return 0;

	/* Leaving a core idle is often worse than degrading locality. */
	if (env->idle == CPU_IDLE)
		return -1;

	dist = node_distance(src_nid, dst_nid);
	if (numa_group) {
		src_weight = group_weight(p, src_nid, dist);
		dst_weight = group_weight(p, dst_nid, dist);
	} else {
		src_weight = task_weight(p, src_nid, dist);
		dst_weight = task_weight(p, dst_nid, dist);
	}

	return dst_weight < src_weight;
}

#else
static inline int migrate_degrades_locality(struct task_struct *p,
					     struct lb_env *env)
{
	return -1;
}
#endif

static inline bool can_migrate_boosted_task(struct task_struct *p,
			int src_cpu, int dst_cpu)
{
	if (per_task_boost(p) == TASK_BOOST_STRICT_MAX &&
		task_in_related_thread_group(p) &&
		(capacity_orig_of(dst_cpu) < capacity_orig_of(src_cpu)))
		return false;
	return true;
}

/*
 * can_migrate_task - may task p from runqueue rq be migrated to this_cpu?
 */
static
int can_migrate_task(struct task_struct *p, struct lb_env *env)
{
	int tsk_cache_hot;
	int can_migrate = 1;

	lockdep_assert_held(&env->src_rq->lock);

	trace_android_rvh_can_migrate_task(p, env->dst_cpu, &can_migrate);
	if (!can_migrate)
		return 0;

	/*
	 * We do not migrate tasks that are:
	 * 1) throttled_lb_pair, or
	 * 2) cannot be migrated to this CPU due to cpus_ptr, or
	 * 3) running (obviously), or
	 * 4) are cache-hot on their current CPU.
	 */
	if (throttled_lb_pair(task_group(p), env->src_cpu, env->dst_cpu))
		return 0;

	/*
	 * don't allow pull boost task to smaller cores.
	 */
	if (!can_migrate_boosted_task(p, env->src_cpu, env->dst_cpu))
		return 0;

#ifdef CONFIG_SCHED_WALT
	if (p->wts.iowaited && is_min_capacity_cpu(env->dst_cpu) &&
			!is_min_capacity_cpu(env->src_cpu))
		return 0;
#endif

	/* Disregard pcpu kthreads; they are where they need to be. */
	if (kthread_is_per_cpu(p))
		return 0;

	if (!cpumask_test_cpu(env->dst_cpu, p->cpus_ptr)) {
		int cpu;

		schedstat_inc(p->se.statistics.nr_failed_migrations_affine);

		env->flags |= LBF_SOME_PINNED;

		/*
		 * Remember if this task can be migrated to any other CPU in
		 * our sched_group. We may want to revisit it if we couldn't
		 * meet load balance goals by pulling other tasks on src_cpu.
		 *
		 * Avoid computing new_dst_cpu for NEWLY_IDLE or if we have
		 * already computed one in current iteration.
		 */
		if (env->idle == CPU_NEWLY_IDLE || (env->flags & LBF_DST_PINNED))
			return 0;

		/* Prevent to re-select dst_cpu via env's CPUs: */
		for_each_cpu_and(cpu, env->dst_grpmask, env->cpus) {
			if (cpumask_test_cpu(cpu, p->cpus_ptr)) {
				env->flags |= LBF_DST_PINNED;
				env->new_dst_cpu = cpu;
				break;
			}
		}

		return 0;
	}

	/* Record that we found atleast one task that could run on dst_cpu */
	env->flags &= ~LBF_ALL_PINNED;

#ifdef CONFIG_SCHED_WALT
	if (static_branch_unlikely(&sched_energy_present)) {
		struct root_domain *rd = env->dst_rq->rd;

		if ((rcu_dereference(rd->pd) && !sd_overutilized(env->sd)) &&
		    env->idle == CPU_NEWLY_IDLE && !env->prefer_spread &&
		    !task_in_related_thread_group(p)) {
			long util_cum_dst, util_cum_src;
			unsigned long demand;

			demand = task_util(p);
			util_cum_dst = cpu_util_cum(env->dst_cpu, 0) + demand;
			util_cum_src = cpu_util_cum(env->src_cpu, 0) - demand;

			if (util_cum_dst > util_cum_src)
				return 0;
		}
	}

	if (env->flags & LBF_IGNORE_PREFERRED_CLUSTER_TASKS &&
			 !preferred_cluster(
				cpu_rq(env->dst_cpu)->wrq.cluster, p))
		return 0;

	/* Don't detach task if it doesn't fit on the destination */
	if (env->flags & LBF_IGNORE_BIG_TASKS &&
		!task_fits_max(p, env->dst_cpu))
		return 0;

	/* Don't detach task if it is under active migration */
	if (env->src_rq->wrq.push_task == p)
		return 0;
#endif

	if (task_running(env->src_rq, p)) {
		schedstat_inc(p->se.statistics.nr_failed_migrations_running);
		return 0;
	}

#ifdef CONFIG_SCHED_WALT
	if ((env->idle == CPU_NEWLY_IDLE) &&
		is_min_capacity_cpu(env->dst_cpu) &&
		!is_min_capacity_cpu(env->src_cpu) &&
		walt_get_rtg_status(p)) {
		bool pull_to_silver_allowed = false;
		unsigned int cpu;

		for_each_cpu(cpu, env->cpus) {
			if (!is_min_capacity_cpu(cpu) &&
				cpu_overutilized(cpu)) {
				pull_to_silver_allowed = true;
				break;
			}
		}

		if (!pull_to_silver_allowed)
			return 0;
	}
#endif

	/*
	 * Aggressive migration if:
	 * 1) IDLE or NEWLY_IDLE balance.
	 * 2) destination numa is preferred
	 * 3) task is cache cold, or
	 * 4) too many balance attempts have failed.
	 */
	tsk_cache_hot = migrate_degrades_locality(p, env);
	if (tsk_cache_hot == -1)
		tsk_cache_hot = task_hot(p, env);

	if (env->idle != CPU_NOT_IDLE || tsk_cache_hot <= 0 ||
	    env->sd->nr_balance_failed > env->sd->cache_nice_tries) {
		if (tsk_cache_hot == 1) {
			schedstat_inc(env->sd->lb_hot_gained[env->idle]);
			schedstat_inc(p->se.statistics.nr_forced_migrations);
		}
		return 1;
	}

	schedstat_inc(p->se.statistics.nr_failed_migrations_hot);
	return 0;
}

/*
 * detach_task() -- detach the task for the migration specified in env
 */
static void detach_task(struct task_struct *p, struct lb_env *env)
{
	lockdep_assert_held(&env->src_rq->lock);

	deactivate_task(env->src_rq, p, DEQUEUE_NOCLOCK);
	lockdep_off();
	double_lock_balance(env->src_rq, env->dst_rq);
	if (!(env->src_rq->clock_update_flags & RQCF_UPDATED))
		update_rq_clock(env->src_rq);
	set_task_cpu(p, env->dst_cpu);
	double_unlock_balance(env->src_rq, env->dst_rq);
	lockdep_on();
}

/*
 * detach_one_task() -- tries to dequeue exactly one task from env->src_rq, as
 * part of active balancing operations within "domain".
 *
 * Returns a task if successful and NULL otherwise.
 */
static struct task_struct *detach_one_task(struct lb_env *env)
{
	struct task_struct *p;

	lockdep_assert_held(&env->src_rq->lock);

	list_for_each_entry_reverse(p,
			&env->src_rq->cfs_tasks, se.group_node) {
		if (!can_migrate_task(p, env))
			continue;

		detach_task(p, env);

		/*
		 * Right now, this is only the second place where
		 * lb_gained[env->idle] is updated (other is detach_tasks)
		 * so we can safely collect stats here rather than
		 * inside detach_tasks().
		 */
		schedstat_inc(env->sd->lb_gained[env->idle]);
		return p;
	}
	return NULL;
}

static const unsigned int sched_nr_migrate_break = 32;

/*
 * detach_tasks() -- tries to detach up to imbalance runnable load from
 * busiest_rq, as part of a balancing operation within domain "sd".
 *
 * Returns number of detached tasks if successful and 0 otherwise.
 */
static int detach_tasks(struct lb_env *env)
{
	struct list_head *tasks = &env->src_rq->cfs_tasks;
	struct task_struct *p;
	unsigned long load = 0;
	int detached = 0;
	int orig_loop = env->loop;

	lockdep_assert_held(&env->src_rq->lock);

	if (env->imbalance <= 0)
		return 0;

	if (!same_cluster(env->dst_cpu, env->src_cpu))
		env->flags |= LBF_IGNORE_PREFERRED_CLUSTER_TASKS;

	if (capacity_orig_of(env->dst_cpu) < capacity_orig_of(env->src_cpu))
		env->flags |= LBF_IGNORE_BIG_TASKS;

redo:
	while (!list_empty(tasks)) {
		/*
		 * We don't want to steal all, otherwise we may be treated likewise,
		 * which could at worst lead to a livelock crash.
		 */
		if (env->idle != CPU_NOT_IDLE && env->src_rq->nr_running <= 1)
			break;

		p = list_last_entry(tasks, struct task_struct, se.group_node);

		env->loop++;
		/* We've more or less seen every task there is, call it quits */
		if (env->loop > env->loop_max)
			break;

		/* take a breather every nr_migrate tasks */
		if (env->loop > env->loop_break) {
			env->loop_break += sched_nr_migrate_break;
			env->flags |= LBF_NEED_BREAK;
			break;
		}

		if (!can_migrate_task(p, env))
			goto next;

		/*
		 * Depending of the number of CPUs and tasks and the
		 * cgroup hierarchy, task_h_load() can return a null
		 * value. Make sure that env->imbalance decreases
		 * otherwise detach_tasks() will stop only after
		 * detaching up to loop_max tasks.
		 */
		load = max_t(unsigned long, task_h_load(p), 1);


		if (sched_feat(LB_MIN) && load < 16 && !env->sd->nr_balance_failed)
			goto next;

		/*
		 * p is not running task when we goes until here, so if p is one
		 * of the 2 task in src cpu rq and not the running one,
		 * that means it is the only task that can be balanced.
		 * So only when there is other tasks can be balanced or
		 * there is situation to ignore big task, it is needed
		 * to skip the task load bigger than 2*imbalance.
		 *
		 * And load based checks are skipped for prefer_spread in
		 * finding busiest group, ignore the task's h_load.
		 */
		if (!env->prefer_spread &&
			((cpu_rq(env->src_cpu)->nr_running > 2) ||
			(env->flags & LBF_IGNORE_BIG_TASKS)) &&
			((load / 2) > env->imbalance))
			goto next;

		detach_task(p, env);
		list_add(&p->se.group_node, &env->tasks);

		detached++;
		env->imbalance -= load;

#ifdef CONFIG_PREEMPTION
		/*
		 * NEWIDLE balancing is a source of latency, so preemptible
		 * kernels will stop after the first task is detached to minimize
		 * the critical section.
		 */
		if (env->idle == CPU_NEWLY_IDLE)
			break;
#endif

		/*
		 * We only want to steal up to the prescribed amount of
		 * runnable load.
		 */
		if (env->imbalance <= 0)
			break;

		continue;
next:
#ifdef CONFIG_SCHED_WALT
		trace_sched_load_balance_skip_tasks(env->src_cpu, env->dst_cpu,
				env->src_grp_type, p->pid, load, task_util(p),
				cpumask_bits(&p->cpus_mask)[0]);
#endif
		list_move(&p->se.group_node, tasks);
	}

	if (env->flags & (LBF_IGNORE_BIG_TASKS |
			LBF_IGNORE_PREFERRED_CLUSTER_TASKS) && !detached) {
		tasks = &env->src_rq->cfs_tasks;
		env->flags &= ~(LBF_IGNORE_BIG_TASKS |
				LBF_IGNORE_PREFERRED_CLUSTER_TASKS);
		env->loop = orig_loop;
		goto redo;
	}

	/*
	 * Right now, this is one of only two places we collect this stat
	 * so we can safely collect detach_one_task() stats here rather
	 * than inside detach_one_task().
	 */
	schedstat_add(env->sd->lb_gained[env->idle], detached);

	return detached;
}

/*
 * attach_task() -- attach the task detached by detach_task() to its new rq.
 */
static void attach_task(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_held(&rq->lock);

	BUG_ON(task_rq(p) != rq);
	activate_task(rq, p, ENQUEUE_NOCLOCK);
	check_preempt_curr(rq, p, 0);
}

/*
 * attach_one_task() -- attaches the task returned from detach_one_task() to
 * its new rq.
 */
static void attach_one_task(struct rq *rq, struct task_struct *p)
{
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	attach_task(rq, p);
	update_overutilized_status(rq);
	rq_unlock(rq, &rf);
}

/*
 * attach_tasks() -- attaches all tasks detached by detach_tasks() to their
 * new rq.
 */
static void attach_tasks(struct lb_env *env)
{
	struct list_head *tasks = &env->tasks;
	struct task_struct *p;
	struct rq_flags rf;

	rq_lock(env->dst_rq, &rf);
	update_rq_clock(env->dst_rq);

	while (!list_empty(tasks)) {
		p = list_first_entry(tasks, struct task_struct, se.group_node);
		list_del_init(&p->se.group_node);

		attach_task(env->dst_rq, p);
	}

	/*
	 * The enqueue_task_fair only updates the overutilized status
	 * for the waking tasks. Since multiple tasks may get migrated
	 * from load balancer, instead of doing it there, update the
	 * overutilized status here at the end.
	 */
	update_overutilized_status(env->dst_rq);
	rq_unlock(env->dst_rq, &rf);
}

#ifdef CONFIG_NO_HZ_COMMON
static inline bool cfs_rq_has_blocked(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->avg.load_avg)
		return true;

	if (cfs_rq->avg.util_avg)
		return true;

	return false;
}

static inline bool others_have_blocked(struct rq *rq)
{
	if (READ_ONCE(rq->avg_rt.util_avg))
		return true;

	if (READ_ONCE(rq->avg_dl.util_avg))
		return true;

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	if (READ_ONCE(rq->avg_irq.util_avg))
		return true;
#endif

	return false;
}

static inline void update_blocked_load_status(struct rq *rq, bool has_blocked)
{
	rq->last_blocked_load_update_tick = jiffies;

	if (!has_blocked)
		rq->has_blocked_load = 0;
}
#else
static inline bool cfs_rq_has_blocked(struct cfs_rq *cfs_rq) { return false; }
static inline bool others_have_blocked(struct rq *rq) { return false; }
static inline void update_blocked_load_status(struct rq *rq, bool has_blocked) {}
#endif

static bool __update_blocked_others(struct rq *rq, bool *done)
{
	const struct sched_class *curr_class;
	u64 now = rq_clock_pelt(rq);
	bool decayed;

	/*
	 * update_load_avg() can call cpufreq_update_util(). Make sure that RT,
	 * DL and IRQ signals have been updated before updating CFS.
	 */
	curr_class = rq->curr->sched_class;

	decayed = update_rt_rq_load_avg(now, rq, curr_class == &rt_sched_class) |
		  update_dl_rq_load_avg(now, rq, curr_class == &dl_sched_class) |
		  update_irq_load_avg(rq, 0);

	if (others_have_blocked(rq))
		*done = false;

	return decayed;
}

#ifdef CONFIG_FAIR_GROUP_SCHED

static inline bool cfs_rq_is_decayed(struct cfs_rq *cfs_rq)
{
	if (cfs_rq->load.weight)
		return false;

	if (cfs_rq->avg.load_sum)
		return false;

	if (cfs_rq->avg.util_sum)
		return false;

	if (cfs_rq->avg.runnable_load_sum)
		return false;

	return true;
}

static bool __update_blocked_fair(struct rq *rq, bool *done)
{
	struct cfs_rq *cfs_rq, *pos;
	bool decayed = false;
	int cpu = cpu_of(rq);

	/*
	 * Iterates the task_group tree in a bottom up fashion, see
	 * list_add_leaf_cfs_rq() for details.
	 */
	for_each_leaf_cfs_rq_safe(rq, cfs_rq, pos) {
		struct sched_entity *se;

		if (update_cfs_rq_load_avg(cfs_rq_clock_pelt(cfs_rq), cfs_rq)) {
			update_tg_load_avg(cfs_rq, 0);

			if (cfs_rq == &rq->cfs)
				decayed = true;
		}

		/* Propagate pending load changes to the parent, if any: */
		se = cfs_rq->tg->se[cpu];
		if (se && !skip_blocked_update(se))
			update_load_avg(cfs_rq_of(se), se, UPDATE_TG);

		/*
		 * There can be a lot of idle CPU cgroups.  Don't let fully
		 * decayed cfs_rqs linger on the list.
		 */
		if (cfs_rq_is_decayed(cfs_rq))
			list_del_leaf_cfs_rq(cfs_rq);

		/* Don't need periodic decay once load/util_avg are null */
		if (cfs_rq_has_blocked(cfs_rq))
			*done = false;
	}

	return decayed;
}

/*
 * Compute the hierarchical load factor for cfs_rq and all its ascendants.
 * This needs to be done in a top-down fashion because the load of a child
 * group is a fraction of its parents load.
 */
static void update_cfs_rq_h_load(struct cfs_rq *cfs_rq)
{
	struct rq *rq = rq_of(cfs_rq);
	struct sched_entity *se = cfs_rq->tg->se[cpu_of(rq)];
	unsigned long now = jiffies;
	unsigned long load;

	if (cfs_rq->last_h_load_update == now)
		return;

	WRITE_ONCE(cfs_rq->h_load_next, NULL);
	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		WRITE_ONCE(cfs_rq->h_load_next, se);
		if (cfs_rq->last_h_load_update == now)
			break;
	}

	if (!se) {
		cfs_rq->h_load = cfs_rq_load_avg(cfs_rq);
		cfs_rq->last_h_load_update = now;
	}

	while ((se = READ_ONCE(cfs_rq->h_load_next)) != NULL) {
		load = cfs_rq->h_load;
		load = div64_ul(load * se->avg.load_avg,
			cfs_rq_load_avg(cfs_rq) + 1);
		cfs_rq = group_cfs_rq(se);
		cfs_rq->h_load = load;
		cfs_rq->last_h_load_update = now;
	}
}

static unsigned long task_h_load(struct task_struct *p)
{
	struct cfs_rq *cfs_rq = task_cfs_rq(p);

	update_cfs_rq_h_load(cfs_rq);
	return div64_ul(p->se.avg.load_avg * cfs_rq->h_load,
			cfs_rq_load_avg(cfs_rq) + 1);
}
#else
static bool __update_blocked_fair(struct rq *rq, bool *done)
{
	struct cfs_rq *cfs_rq = &rq->cfs;
	bool decayed;

	decayed = update_cfs_rq_load_avg(cfs_rq_clock_pelt(cfs_rq), cfs_rq);
	if (cfs_rq_has_blocked(cfs_rq))
		*done = false;

	return decayed;
}

static unsigned long task_h_load(struct task_struct *p)
{
	return p->se.avg.load_avg;
}
#endif

static void update_blocked_averages(int cpu)
{
	bool decayed = false, done = true;
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	update_rq_clock(rq);

	decayed |= __update_blocked_others(rq, &done);
	decayed |= __update_blocked_fair(rq, &done);

	update_blocked_load_status(rq, !done);
	if (decayed)
		cpufreq_update_util(rq, 0);
	rq_unlock_irqrestore(rq, &rf);
}

/********** Helpers for find_busiest_group ************************/

/*
 * sg_lb_stats - stats of a sched_group required for load_balancing
 */
struct sg_lb_stats {
	unsigned long avg_load; /*Avg load across the CPUs of the group */
	unsigned long group_load; /* Total load over the CPUs of the group */
	unsigned long load_per_task;
	unsigned long group_capacity;
	unsigned long group_util; /* Total utilization of the group */
	unsigned int sum_nr_running; /* Nr tasks running in the group */
	unsigned int idle_cpus;
	unsigned int group_weight;
	enum group_type group_type;
	int group_no_capacity;
	unsigned long group_misfit_task_load; /* A CPU has a task too big for its capacity */
#ifdef CONFIG_NUMA_BALANCING
	unsigned int nr_numa_running;
	unsigned int nr_preferred_running;
#endif
};

/*
 * sd_lb_stats - Structure to store the statistics of a sched_domain
 *		 during load balancing.
 */
struct sd_lb_stats {
	struct sched_group *busiest;	/* Busiest group in this sd */
	struct sched_group *local;	/* Local group in this sd */
	unsigned long total_running;
	unsigned long total_load;	/* Total load of all groups in sd */
	unsigned long total_capacity;	/* Total capacity of all groups in sd */
	unsigned long total_util;	/* Total util of all groups in sd */
	unsigned long avg_load;	/* Average load across all groups in sd */

	struct sg_lb_stats busiest_stat;/* Statistics of the busiest group */
	struct sg_lb_stats local_stat;	/* Statistics of the local group */
};

static inline void init_sd_lb_stats(struct sd_lb_stats *sds)
{
	/*
	 * Skimp on the clearing to avoid duplicate work. We can avoid clearing
	 * local_stat because update_sg_lb_stats() does a full clear/assignment.
	 * We must however clear busiest_stat::avg_load because
	 * update_sd_pick_busiest() reads this before assignment.
	 */
	*sds = (struct sd_lb_stats){
		.busiest = NULL,
		.local = NULL,
		.total_running = 0UL,
		.total_load = 0UL,
		.total_capacity = 0UL,
		.total_util = 0UL,
		.busiest_stat = {
			.avg_load = 0UL,
			.sum_nr_running = 0,
			.group_type = group_other,
		},
	};
}

static unsigned long scale_rt_capacity(int cpu, unsigned long max)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long used, free;
	unsigned long irq;

	irq = cpu_util_irq(rq);

	if (unlikely(irq >= max))
		return 1;

	used = READ_ONCE(rq->avg_rt.util_avg);
	used += READ_ONCE(rq->avg_dl.util_avg);

	if (unlikely(used >= max))
		return 1;

	free = max - used;

	return scale_irq_capacity(free, irq, max);
}

void init_max_cpu_capacity(struct max_cpu_capacity *mcc) {
	raw_spin_lock_init(&mcc->lock);
	mcc->val = 0;
	mcc->cpu = -1;
}

static void update_cpu_capacity(struct sched_domain *sd, int cpu)
{
	unsigned long capacity = arch_scale_cpu_capacity(cpu);
	struct sched_group *sdg = sd->groups;

	capacity *= arch_scale_max_freq_capacity(sd, cpu);
	capacity >>= SCHED_CAPACITY_SHIFT;

	capacity = min(capacity, thermal_cap(cpu));
	cpu_rq(cpu)->cpu_capacity_orig = capacity;

	capacity = scale_rt_capacity(cpu, capacity);

	if (!capacity)
		capacity = 1;

	cpu_rq(cpu)->cpu_capacity = capacity;
	sdg->sgc->capacity = capacity;
	sdg->sgc->min_capacity = capacity;
	sdg->sgc->max_capacity = capacity;
}

void update_group_capacity(struct sched_domain *sd, int cpu)
{
	struct sched_domain *child = sd->child;
	struct sched_group *group, *sdg = sd->groups;
	unsigned long capacity, min_capacity, max_capacity;
	unsigned long interval;

	interval = msecs_to_jiffies(sd->balance_interval);
	interval = clamp(interval, 1UL, max_load_balance_interval);
	sdg->sgc->next_update = jiffies + interval;

	if (!child) {
		update_cpu_capacity(sd, cpu);
		return;
	}

	capacity = 0;
	min_capacity = ULONG_MAX;
	max_capacity = 0;

	if (child->flags & SD_OVERLAP) {
		/*
		 * SD_OVERLAP domains cannot assume that child groups
		 * span the current group.
		 */

		for_each_cpu(cpu, sched_group_span(sdg)) {
			struct sched_group_capacity *sgc;
			struct rq *rq = cpu_rq(cpu);

			if (cpu_isolated(cpu))
				continue;

			/*
			 * build_sched_domains() -> init_sched_groups_capacity()
			 * gets here before we've attached the domains to the
			 * runqueues.
			 *
			 * Use capacity_of(), which is set irrespective of domains
			 * in update_cpu_capacity().
			 *
			 * This avoids capacity from being 0 and
			 * causing divide-by-zero issues on boot.
			 */
			if (unlikely(!rq->sd)) {
				capacity += capacity_of(cpu);
			} else {
				sgc = rq->sd->groups->sgc;
				capacity += sgc->capacity;
			}

			min_capacity = min(capacity, min_capacity);
			max_capacity = max(capacity, max_capacity);
		}
	} else  {
		/*
		 * !SD_OVERLAP domains can assume that child groups
		 * span the current group.
		 */

		group = child->groups;
		do {
			struct sched_group_capacity *sgc = group->sgc;
			__maybe_unused cpumask_t *cpus =
					sched_group_span(group);

			if (!cpu_isolated(cpumask_first(cpus))) {
				capacity += sgc->capacity;
				min_capacity = min(sgc->min_capacity,
							min_capacity);
				max_capacity = max(sgc->max_capacity,
							max_capacity);
			}
			group = group->next;
		} while (group != child->groups);
	}

	sdg->sgc->capacity = capacity;
	sdg->sgc->min_capacity = min_capacity;
	sdg->sgc->max_capacity = max_capacity;
}

/*
 * Check whether the capacity of the rq has been noticeably reduced by side
 * activity. The imbalance_pct is used for the threshold.
 * Return true is the capacity is reduced
 */
static inline int
check_cpu_capacity(struct rq *rq, struct sched_domain *sd)
{
	return ((rq->cpu_capacity * sd->imbalance_pct) <
				(rq->cpu_capacity_orig * 100));
}

/*
 * Check whether a rq has a misfit task and if it looks like we can actually
 * help that task: we can migrate the task to a CPU of higher capacity, or
 * the task's current CPU is heavily pressured.
 */
static inline int check_misfit_status(struct rq *rq, struct sched_domain *sd)
{
	return rq->misfit_task_load &&
		(rq->cpu_capacity_orig < rq->rd->max_cpu_capacity.val ||
		 check_cpu_capacity(rq, sd));
}

/*
 * Group imbalance indicates (and tries to solve) the problem where balancing
 * groups is inadequate due to ->cpus_ptr constraints.
 *
 * Imagine a situation of two groups of 4 CPUs each and 4 tasks each with a
 * cpumask covering 1 CPU of the first group and 3 CPUs of the second group.
 * Something like:
 *
 *	{ 0 1 2 3 } { 4 5 6 7 }
 *	        *     * * *
 *
 * If we were to balance group-wise we'd place two tasks in the first group and
 * two tasks in the second group. Clearly this is undesired as it will overload
 * cpu 3 and leave one of the CPUs in the second group unused.
 *
 * The current solution to this issue is detecting the skew in the first group
 * by noticing the lower domain failed to reach balance and had difficulty
 * moving tasks due to affinity constraints.
 *
 * When this is so detected; this group becomes a candidate for busiest; see
 * update_sd_pick_busiest(). And calculate_imbalance() and
 * find_busiest_group() avoid some of the usual balance conditions to allow it
 * to create an effective group imbalance.
 *
 * This is a somewhat tricky proposition since the next run might not find the
 * group imbalance and decide the groups need to be balanced again. A most
 * subtle and fragile situation.
 */

static inline int sg_imbalanced(struct sched_group *group)
{
	return group->sgc->imbalance;
}

/*
 * group_has_capacity returns true if the group has spare capacity that could
 * be used by some tasks.
 * We consider that a group has spare capacity if the  * number of task is
 * smaller than the number of CPUs or if the utilization is lower than the
 * available capacity for CFS tasks.
 * For the latter, we use a threshold to stabilize the state, to take into
 * account the variance of the tasks' load and to return true if the available
 * capacity in meaningful for the load balancer.
 * As an example, an available capacity of 1% can appear but it doesn't make
 * any benefit for the load balance.
 */
static inline bool
group_has_capacity(struct lb_env *env, struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running < sgs->group_weight)
		return true;

	if ((sgs->group_capacity * 100) >
			(sgs->group_util * env->sd->imbalance_pct))
		return true;

	return false;
}

/*
 *  group_is_overloaded returns true if the group has more tasks than it can
 *  handle.
 *  group_is_overloaded is not equals to !group_has_capacity because a group
 *  with the exact right number of tasks, has no more spare capacity but is not
 *  overloaded so both group_has_capacity and group_is_overloaded return
 *  false.
 */
static inline bool
group_is_overloaded(struct lb_env *env, struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running <= sgs->group_weight)
		return false;

#ifdef CONFIG_SCHED_WALT
	if (env->idle != CPU_NOT_IDLE && walt_rotation_enabled)
		return true;
#endif

	if ((sgs->group_capacity * 100) <
			(sgs->group_util * env->sd->imbalance_pct))
		return true;

	return false;
}

/*
 * group_smaller_min_cpu_capacity: Returns true if sched_group sg has smaller
 * per-CPU capacity than sched_group ref.
 */
static inline bool
group_smaller_min_cpu_capacity(struct sched_group *sg, struct sched_group *ref)
{
	return sg->sgc->min_capacity *
				sched_capacity_margin_up[group_first_cpu(sg)] <
						ref->sgc->min_capacity * 1024;
}

/*
 * group_smaller_max_cpu_capacity: Returns true if sched_group sg has smaller
 * per-CPU capacity_orig than sched_group ref.
 */
static inline bool
group_smaller_max_cpu_capacity(struct sched_group *sg, struct sched_group *ref)
{
	return sg->sgc->max_capacity *
				sched_capacity_margin_up[group_first_cpu(sg)] <
						ref->sgc->max_capacity * 1024;
}

/*
 * group_similar_cpu_capacity: Returns true if the minimum capacity of the
 * compared groups differ by less than 12.5%.
 */
static inline bool
group_similar_cpu_capacity(struct sched_group *sg, struct sched_group *ref)
{
	long diff = sg->sgc->min_capacity - ref->sgc->min_capacity;
	long max = max(sg->sgc->min_capacity, ref->sgc->min_capacity);

	return ((abs(diff) < max >> 3) ||
		asym_cap_siblings(group_first_cpu(sg), group_first_cpu(ref)));
}

static inline enum
group_type group_classify(struct sched_group *group,
			  struct sg_lb_stats *sgs)
{
	if (sgs->group_no_capacity)
		return group_overloaded;

	if (sg_imbalanced(group))
		return group_imbalanced;

	if (sgs->group_misfit_task_load)
		return group_misfit_task;

	return group_other;
}

static bool update_nohz_stats(struct rq *rq, bool force)
{
#ifdef CONFIG_NO_HZ_COMMON
	unsigned int cpu = rq->cpu;

	if (!rq->has_blocked_load)
		return false;

	if (!cpumask_test_cpu(cpu, nohz.idle_cpus_mask))
		return false;

	if (!force && !time_after(jiffies, rq->last_blocked_load_update_tick))
		return true;

	update_blocked_averages(cpu);

	return rq->has_blocked_load;
#else
	return false;
#endif
}

/**
 * update_sg_lb_stats - Update sched_group's statistics for load balancing.
 * @env: The load balancing environment.
 * @group: sched_group whose statistics are to be updated.
 * @sgs: variable to hold the statistics for this group.
 * @sg_status: Holds flag indicating the status of the sched_group
 */
static inline void update_sg_lb_stats(struct lb_env *env,
				      struct sched_group *group,
				      struct sg_lb_stats *sgs,
				      int *sg_status)
{
	int i, nr_running;

	memset(sgs, 0, sizeof(*sgs));

	for_each_cpu_and(i, sched_group_span(group), env->cpus) {
		struct rq *rq = cpu_rq(i);

		if (cpu_isolated(i))
			continue;

		if ((env->flags & LBF_NOHZ_STATS) && update_nohz_stats(rq, false))
			env->flags |= LBF_NOHZ_AGAIN;

		sgs->group_load += cpu_runnable_load(rq);
		sgs->group_util += cpu_util(i);

		nr_running = rq->nr_running;
		sgs->sum_nr_running += nr_running;

		if (nr_running > 1)
			*sg_status |= SG_OVERLOAD;

		if (cpu_overutilized(i)) {
			*sg_status |= SG_OVERUTILIZED;

			if (rq->misfit_task_load)
				*sg_status |= SG_HAS_MISFIT_TASK;
		}

#ifdef CONFIG_NUMA_BALANCING
		sgs->nr_numa_running += rq->nr_numa_running;
		sgs->nr_preferred_running += rq->nr_preferred_running;
#endif
		/*
		 * No need to call idle_cpu() if nr_running is not 0
		 */
		if (!nr_running && idle_cpu(i))
			sgs->idle_cpus++;

		if (env->sd->flags & SD_ASYM_CPUCAPACITY &&
		    sgs->group_misfit_task_load < rq->misfit_task_load) {
			sgs->group_misfit_task_load = rq->misfit_task_load;
			*sg_status |= SG_OVERLOAD;
		}
	}

	/* Isolated CPU has no weight */
	if (!group->group_weight) {
		sgs->group_capacity = 0;
		sgs->avg_load = 0;
		sgs->group_no_capacity = 1;
		sgs->group_type = group_other;
		sgs->group_weight = group->group_weight;
	} else {
		/* Adjust by relative CPU capacity of the group */
		sgs->group_capacity = group->sgc->capacity;
		sgs->avg_load = (sgs->group_load*SCHED_CAPACITY_SCALE) /
							sgs->group_capacity;

		sgs->group_weight = group->group_weight;

		sgs->group_no_capacity = group_is_overloaded(env, sgs);
		sgs->group_type = group_classify(group, sgs);
	}

	if (sgs->sum_nr_running)
		sgs->load_per_task = sgs->group_load / sgs->sum_nr_running;
}

/**
 * update_sd_pick_busiest - return 1 on busiest group
 * @env: The load balancing environment.
 * @sds: sched_domain statistics
 * @sg: sched_group candidate to be checked for being the busiest
 * @sgs: sched_group statistics
 *
 * Determine if @sg is a busier group than the previously selected
 * busiest group.
 *
 * Return: %true if @sg is a busier group than the previously selected
 * busiest group. %false otherwise.
 */
static bool update_sd_pick_busiest(struct lb_env *env,
				   struct sd_lb_stats *sds,
				   struct sched_group *sg,
				   struct sg_lb_stats *sgs)
{
	struct sg_lb_stats *busiest = &sds->busiest_stat;

	/*
	 * Don't try to pull misfit tasks we can't help.
	 * We can use max_capacity here as reduction in capacity on some
	 * CPUs in the group should either be possible to resolve
	 * internally or be covered by avg_load imbalance (eventually).
	 */
	if (sgs->group_type == group_misfit_task &&
	    (!group_smaller_max_cpu_capacity(sg, sds->local) ||
	     !group_has_capacity(env, &sds->local_stat)))
		return false;

	if (sgs->group_type > busiest->group_type)
		return true;

	if (sgs->group_type < busiest->group_type)
		return false;

	/*
	 * This sg and busiest are classified as same. when prefer_spread
	 * is true, we want to maximize the chance of pulling taks, so
	 * prefer to pick sg with more runnable tasks and break the ties
	 * with utilization.
	 */
	if (env->prefer_spread) {
		if (sgs->sum_nr_running < busiest->sum_nr_running)
			return false;
		if (sgs->sum_nr_running > busiest->sum_nr_running)
			return true;
		return sgs->group_util > busiest->group_util;
	}

	if (sgs->avg_load <= busiest->avg_load)
		return false;

	if (!(env->sd->flags & SD_ASYM_CPUCAPACITY))
		goto asym_packing;

	/*
	 * Candidate sg has no more than one task per CPU and
	 * has higher per-CPU capacity. Migrating tasks to less
	 * capable CPUs may harm throughput. Maximize throughput,
	 * power/energy consequences are not considered.
	 */
	if (sgs->sum_nr_running <= sgs->group_weight &&
	    group_smaller_min_cpu_capacity(sds->local, sg))
		return false;

	/*
	 * Candidate sg doesn't face any severe imbalance issues so
	 * don't disturb unless the groups are of similar capacity
	 * where balancing is more harmless.
	 */
	if (sgs->group_type == group_other &&
		!group_similar_cpu_capacity(sds->local, sg))
		return false;

	/*
	 * If we have more than one misfit sg go with the biggest misfit.
	 */
	if (sgs->group_type == group_misfit_task &&
	    sgs->group_misfit_task_load < busiest->group_misfit_task_load)
		return false;

asym_packing:

	/* This is the busiest node in its class. */
	if (!(env->sd->flags & SD_ASYM_PACKING))
		return true;

	/* No ASYM_PACKING if target CPU is already busy */
	if (env->idle == CPU_NOT_IDLE)
		return true;
	/*
	 * ASYM_PACKING needs to move all the work to the highest
	 * prority CPUs in the group, therefore mark all groups
	 * of lower priority than ourself as busy.
	 */
	if (sgs->sum_nr_running &&
	    sched_asym_prefer(env->dst_cpu, sg->asym_prefer_cpu)) {
		if (!sds->busiest)
			return true;

		/* Prefer to move from lowest priority CPU's work */
		if (sched_asym_prefer(sds->busiest->asym_prefer_cpu,
				      sg->asym_prefer_cpu))
			return true;
	}

	return false;
}

#ifdef CONFIG_NUMA_BALANCING
static inline enum fbq_type fbq_classify_group(struct sg_lb_stats *sgs)
{
	if (sgs->sum_nr_running > sgs->nr_numa_running)
		return regular;
	if (sgs->sum_nr_running > sgs->nr_preferred_running)
		return remote;
	return all;
}

static inline enum fbq_type fbq_classify_rq(struct rq *rq)
{
	if (rq->nr_running > rq->nr_numa_running)
		return regular;
	if (rq->nr_running > rq->nr_preferred_running)
		return remote;
	return all;
}
#else
static inline enum fbq_type fbq_classify_group(struct sg_lb_stats *sgs)
{
	return all;
}

static inline enum fbq_type fbq_classify_rq(struct rq *rq)
{
	return regular;
}
#endif /* CONFIG_NUMA_BALANCING */

/**
 * update_sd_lb_stats - Update sched_domain's statistics for load balancing.
 * @env: The load balancing environment.
 * @sds: variable to hold the statistics for this sched_domain.
 */
static inline void update_sd_lb_stats(struct lb_env *env, struct sd_lb_stats *sds)
{
	struct sched_domain *child = env->sd->child;
	struct sched_group *sg = env->sd->groups;
	struct sg_lb_stats *local = &sds->local_stat;
	struct sg_lb_stats tmp_sgs;
	bool prefer_sibling = child && child->flags & SD_PREFER_SIBLING;
	int sg_status = 0;

#ifdef CONFIG_NO_HZ_COMMON
	if (env->idle == CPU_NEWLY_IDLE && READ_ONCE(nohz.has_blocked))
		env->flags |= LBF_NOHZ_STATS;
#endif

	do {
		struct sg_lb_stats *sgs = &tmp_sgs;
		int local_group;

		local_group = cpumask_test_cpu(env->dst_cpu, sched_group_span(sg));
		if (local_group) {
			sds->local = sg;
			sgs = local;

			if (env->idle != CPU_NEWLY_IDLE ||
			    time_after_eq(jiffies, sg->sgc->next_update))
				update_group_capacity(env->sd, env->dst_cpu);
		}

		update_sg_lb_stats(env, sg, sgs, &sg_status);

		if (local_group)
			goto next_group;

		/*
		 * In case the child domain prefers tasks go to siblings
		 * first, lower the sg capacity so that we'll try
		 * and move all the excess tasks away. We lower the capacity
		 * of a group only if the local group has the capacity to fit
		 * these excess tasks. The extra check prevents the case where
		 * you always pull from the heaviest group when it is already
		 * under-utilized (possible with a large weight task outweighs
		 * the tasks on the system).
		 */
		if (prefer_sibling && sds->local &&
		    group_has_capacity(env, local) &&
		    (sgs->sum_nr_running > local->sum_nr_running + 1)) {
			sgs->group_no_capacity = 1;
			sgs->group_type = group_classify(sg, sgs);
		}

		/*
		 * Disallow moving tasks from asym cap sibling CPUs to other
		 * CPUs (lower capacity) unless the asym cap sibling group has
		 * no capacity to manage the current load.
		 */
		if ((env->sd->flags & SD_ASYM_CPUCAPACITY) &&
			sgs->group_no_capacity &&
			asym_cap_sibling_group_has_capacity(env->dst_cpu,
						env->sd->imbalance_pct)) {
			sgs->group_no_capacity = 0;
			sgs->group_type = group_classify(sg, sgs);
		}

		if (update_sd_pick_busiest(env, sds, sg, sgs)) {
			sds->busiest = sg;
			sds->busiest_stat = *sgs;
		}

next_group:
		/* Now, start updating sd_lb_stats */
		sds->total_running += sgs->sum_nr_running;
		sds->total_load += sgs->group_load;
		sds->total_capacity += sgs->group_capacity;
		sds->total_util += sgs->group_util;

		trace_sched_load_balance_sg_stats(sg->cpumask[0],
				sgs->group_type, sgs->idle_cpus,
				sgs->sum_nr_running, sgs->group_load,
				sgs->group_capacity, sgs->group_util,
				sgs->group_no_capacity,	sgs->load_per_task,
				sgs->group_misfit_task_load,
				sds->busiest ? sds->busiest->cpumask[0] : 0);

		sg = sg->next;
	} while (sg != env->sd->groups);

#ifdef CONFIG_NO_HZ_COMMON
	if ((env->flags & LBF_NOHZ_AGAIN) &&
	    cpumask_subset(nohz.idle_cpus_mask, sched_domain_span(env->sd))) {

		WRITE_ONCE(nohz.next_blocked,
			   jiffies + msecs_to_jiffies(LOAD_AVG_PERIOD));
	}
#endif

	if (env->sd->flags & SD_NUMA)
		env->fbq_type = fbq_classify_group(&sds->busiest_stat);

	env->src_grp_nr_running = sds->busiest_stat.sum_nr_running;

	if (!env->sd->parent) {
		struct root_domain *rd = env->dst_rq->rd;

		/* update overload indicator if we are at root domain */
		WRITE_ONCE(rd->overload, sg_status & SG_OVERLOAD);

#ifndef CONFIG_SCHED_WALT
		/* Update over-utilization (tipping point, U >= 0) indicator */
		WRITE_ONCE(rd->overutilized, sg_status & SG_OVERUTILIZED);
		trace_sched_overutilized_tp(rd, sg_status & SG_OVERUTILIZED);
	} else if (sg_status & SG_OVERUTILIZED) {
		struct root_domain *rd = env->dst_rq->rd;

		WRITE_ONCE(rd->overutilized, SG_OVERUTILIZED);
		trace_sched_overutilized_tp(rd, SG_OVERUTILIZED);
#endif
	}

#ifdef CONFIG_SCHED_WALT
	if (sg_status & SG_OVERUTILIZED)
		set_sd_overutilized(env->sd);
	else
		clear_sd_overutilized(env->sd);

	/*
	 * If there is a misfit task in one cpu in this sched_domain
	 * it is likely that the imbalance cannot be sorted out among
	 * the cpu's in this sched_domain. In this case set the
	 * overutilized flag at the parent sched_domain.
	 */
	if (sg_status & SG_HAS_MISFIT_TASK) {
		struct sched_domain *sd = env->sd->parent;

		/*
		 * In case of a misfit task, load balance at the parent
		 * sched domain level will make sense only if the the cpus
		 * have a different capacity. If cpus at a domain level have
		 * the same capacity, the misfit task cannot be well
		 * accomodated	in any of the cpus and there in no point in
		 * trying a load balance at this level
		 */
		while (sd) {
			if (sd->flags & SD_ASYM_CPUCAPACITY) {
				set_sd_overutilized(sd);
				break;
			}
			sd = sd->parent;
		}
	}

	/*
	 * If the domain util is greater that domain capacity, load balancing
	 * needs to be done at the next sched domain level as well.
	 */
	if (env->sd->parent &&
	    sds->total_capacity * 1024 < sds->total_util *
			 sched_capacity_margin_up[group_first_cpu(sds->local)])
		set_sd_overutilized(env->sd->parent);
#endif
}

/**
 * check_asym_packing - Check to see if the group is packed into the
 *			sched domain.
 *
 * This is primarily intended to used at the sibling level.  Some
 * cores like POWER7 prefer to use lower numbered SMT threads.  In the
 * case of POWER7, it can move to lower SMT modes only when higher
 * threads are idle.  When in lower SMT modes, the threads will
 * perform better since they share less core resources.  Hence when we
 * have idle threads, we want them to be the higher ones.
 *
 * This packing function is run on idle threads.  It checks to see if
 * the busiest CPU in this domain (core in the P7 case) has a higher
 * CPU number than the packing function is being run on.  Here we are
 * assuming lower CPU number will be equivalent to lower a SMT thread
 * number.
 *
 * Return: 1 when packing is required and a task should be moved to
 * this CPU.  The amount of the imbalance is returned in env->imbalance.
 *
 * @env: The load balancing environment.
 * @sds: Statistics of the sched_domain which is to be packed
 */
static int check_asym_packing(struct lb_env *env, struct sd_lb_stats *sds)
{
	int busiest_cpu;

	if (!(env->sd->flags & SD_ASYM_PACKING))
		return 0;

	if (env->idle == CPU_NOT_IDLE)
		return 0;

	if (!sds->busiest)
		return 0;

	busiest_cpu = sds->busiest->asym_prefer_cpu;
	if (sched_asym_prefer(busiest_cpu, env->dst_cpu))
		return 0;

	env->imbalance = sds->busiest_stat.group_load;

	return 1;
}

/**
 * fix_small_imbalance - Calculate the minor imbalance that exists
 *			amongst the groups of a sched_domain, during
 *			load balancing.
 * @env: The load balancing environment.
 * @sds: Statistics of the sched_domain whose imbalance is to be calculated.
 */
static inline
void fix_small_imbalance(struct lb_env *env, struct sd_lb_stats *sds)
{
	unsigned long tmp, capa_now = 0, capa_move = 0;
	unsigned int imbn = 2;
	unsigned long scaled_busy_load_per_task;
	struct sg_lb_stats *local, *busiest;

	local = &sds->local_stat;
	busiest = &sds->busiest_stat;

	if (!local->sum_nr_running)
		local->load_per_task = cpu_avg_load_per_task(env->dst_cpu);
	else if (busiest->load_per_task > local->load_per_task)
		imbn = 1;

	scaled_busy_load_per_task =
		(busiest->load_per_task * SCHED_CAPACITY_SCALE) /
		busiest->group_capacity;

	if (busiest->avg_load + scaled_busy_load_per_task >=
	    local->avg_load + (scaled_busy_load_per_task * imbn)) {
		env->imbalance = busiest->load_per_task;
		return;
	}

	/*
	 * OK, we don't have enough imbalance to justify moving tasks,
	 * however we may be able to increase total CPU capacity used by
	 * moving them.
	 */

	capa_now += busiest->group_capacity *
			min(busiest->load_per_task, busiest->avg_load);
	capa_now += local->group_capacity *
			min(local->load_per_task, local->avg_load);
	capa_now /= SCHED_CAPACITY_SCALE;

	/* Amount of load we'd subtract */
	if (busiest->avg_load > scaled_busy_load_per_task) {
		capa_move += busiest->group_capacity *
			    min(busiest->load_per_task,
				busiest->avg_load - scaled_busy_load_per_task);
	}

	/* Amount of load we'd add */
	if (busiest->avg_load * busiest->group_capacity <
	    busiest->load_per_task * SCHED_CAPACITY_SCALE) {
		tmp = (busiest->avg_load * busiest->group_capacity) /
		      local->group_capacity;
	} else {
		tmp = (busiest->load_per_task * SCHED_CAPACITY_SCALE) /
		      local->group_capacity;
	}
	capa_move += local->group_capacity *
		    min(local->load_per_task, local->avg_load + tmp);
	capa_move /= SCHED_CAPACITY_SCALE;

	/* Move if we gain throughput */
	if (capa_move > capa_now) {
		env->imbalance = busiest->load_per_task;
		return;
	}

	/* We can't see throughput improvement with the load-based
	 * method, but it is possible depending upon group size and
	 * capacity range that there might still be an underutilized
	 * cpu available in an asymmetric capacity system. Do one last
	 * check just in case.
	 */
	if (env->sd->flags & SD_ASYM_CPUCAPACITY &&
		busiest->group_type == group_overloaded &&
		busiest->sum_nr_running > busiest->group_weight &&
		local->sum_nr_running < local->group_weight &&
		local->group_capacity < busiest->group_capacity)
		env->imbalance = busiest->load_per_task;
}

/**
 * calculate_imbalance - Calculate the amount of imbalance present within the
 *			 groups of a given sched_domain during load balance.
 * @env: load balance environment
 * @sds: statistics of the sched_domain whose imbalance is to be calculated.
 */
static inline void calculate_imbalance(struct lb_env *env, struct sd_lb_stats *sds)
{
	unsigned long max_pull, load_above_capacity = ~0UL;
	struct sg_lb_stats *local, *busiest;
	bool no_imbalance = false;

	local = &sds->local_stat;
	busiest = &sds->busiest_stat;

	if (busiest->group_type == group_imbalanced) {
		/*
		 * In the group_imb case we cannot rely on group-wide averages
		 * to ensure CPU-load equilibrium, look at wider averages. XXX
		 */
		busiest->load_per_task =
			min(busiest->load_per_task, sds->avg_load);
	}

	/*
	 * Avg load of busiest sg can be less and avg load of local sg can
	 * be greater than avg load across all sgs of sd because avg load
	 * factors in sg capacity and sgs with smaller group_type are
	 * skipped when updating the busiest sg:
	 */
	if (busiest->avg_load <= sds->avg_load ||
	     local->avg_load >= sds->avg_load)
		no_imbalance = true;

	if (busiest->group_type != group_misfit_task && no_imbalance) {
		env->imbalance = 0;
		if (busiest->group_type == group_overloaded &&
				local->group_type <= group_misfit_task) {
			env->imbalance = busiest->load_per_task;
			return;
		}
		return fix_small_imbalance(env, sds);
	}

	/*
	 * If there aren't any idle CPUs, avoid creating some.
	 */
	if (busiest->group_type == group_overloaded &&
	    local->group_type   == group_overloaded) {
		load_above_capacity = busiest->sum_nr_running * SCHED_CAPACITY_SCALE;
		if (load_above_capacity > busiest->group_capacity) {
			load_above_capacity -= busiest->group_capacity;
			load_above_capacity *= scale_load_down(NICE_0_LOAD);
			load_above_capacity /= busiest->group_capacity;
		} else
			load_above_capacity = ~0UL;
	}

	/*
	 * In case of a misfit task, independent of avg loads we do load balance
	 * at the parent sched domain level for B.L systems, so it is possible
	 * that busiest group avg load can be less than sd avg load.
	 * So skip calculating load based imbalance between groups.
	 */
	if (!no_imbalance) {
		/*
		 * We're trying to get all the cpus to the average_load,
		 * so we don't want to push ourselves above the average load,
		 * nor do we wish to reduce the max loaded cpu below the average
		 * load. At the same time, we also don't want to reduce the
		 * group load below the group capacity.
		 * Thus we look for the minimum possible imbalance.
		 */
		max_pull = min(busiest->avg_load - sds->avg_load,
						load_above_capacity);

		/* How much load to actually move to equalise the imbalance */
		env->imbalance = min(max_pull * busiest->group_capacity,
					(sds->avg_load - local->avg_load) *
					local->group_capacity) /
					SCHED_CAPACITY_SCALE;
	} else {
		/*
		 * Skipped load based imbalance calculations, but let's find
		 * imbalance based on busiest group type or fix small imbalance.
		 */
		env->imbalance = 0;
	}

	/* Boost imbalance to allow misfit task to be balanced.
	 * Always do this if we are doing a NEWLY_IDLE balance
	 * on the assumption that any tasks we have must not be
	 * long-running (and hence we cannot rely upon load).
	 * However if we are not idle, we should assume the tasks
	 * we have are longer running and not override load-based
	 * calculations above unless we are sure that the local
	 * group is underutilized.
	 */
	if (busiest->group_type == group_misfit_task &&
		(env->idle == CPU_NEWLY_IDLE ||
		local->sum_nr_running < local->group_weight)) {
		env->imbalance = max_t(long, env->imbalance,
				       busiest->group_misfit_task_load);
	}

	/*
	 * if *imbalance is less than the average load per runnable task
	 * there is no guarantee that any tasks will be moved so we'll have
	 * a think about bumping its value to force at least one task to be
	 * moved
	 */
	if (env->imbalance < busiest->load_per_task) {
		/*
		 * The busiest group is overloaded so it could use help
		 * from the other groups. If the local group has idle CPUs
		 * and it is not overloaded and has no imbalance with in
		 * the group, allow the load balance by bumping the
		 * imbalance.
		 */
		if (busiest->group_type == group_overloaded &&
			local->group_type <= group_misfit_task &&
			env->idle != CPU_NOT_IDLE) {
			env->imbalance = busiest->load_per_task;
			return;
		}

		return fix_small_imbalance(env, sds);
	}
}

/******* find_busiest_group() helpers end here *********************/

/**
 * find_busiest_group - Returns the busiest group within the sched_domain
 * if there is an imbalance.
 *
 * Also calculates the amount of runnable load which should be moved
 * to restore balance.
 *
 * @env: The load balancing environment.
 *
 * Return:	- The busiest group if imbalance exists.
 */
static struct sched_group *find_busiest_group(struct lb_env *env)
{
	struct sg_lb_stats *local, *busiest;
	struct sd_lb_stats sds;

	init_sd_lb_stats(&sds);

	/*
	 * Compute the various statistics relavent for load balancing at
	 * this level.
	 */
	update_sd_lb_stats(env, &sds);

	if (sched_energy_enabled()) {
		struct root_domain *rd = env->dst_rq->rd;
		int out_balance = 1;

#ifdef CONFIG_SCHED_WALT
		if (rcu_dereference(rd->pd) && !sd_overutilized(env->sd)) {
#else
		trace_android_rvh_find_busiest_group(sds.busiest, env->dst_rq,
					&out_balance);
		if (rcu_dereference(rd->pd) && !READ_ONCE(rd->overutilized)
					&& out_balance) {
#endif
			int cpu_local, cpu_busiest;
			unsigned long capacity_local, capacity_busiest;

			if (env->idle != CPU_NEWLY_IDLE && !env->prefer_spread)
				goto out_balanced;

			if (!sds.local || !sds.busiest)
				goto out_balanced;

			cpu_local = group_first_cpu(sds.local);
			cpu_busiest = group_first_cpu(sds.busiest);

			/* TODO:don't assume same cap cpus are in same domain */
			capacity_local = capacity_orig_of(cpu_local);
			capacity_busiest = capacity_orig_of(cpu_busiest);
			if ((sds.busiest->group_weight > 1) &&
					capacity_local > capacity_busiest) {
				goto out_balanced;
			} else if (capacity_local == capacity_busiest ||
				   asym_cap_siblings(cpu_local, cpu_busiest)) {
				if (cpu_rq(cpu_busiest)->nr_running < 2)
					goto out_balanced;
			}
		}
	}

	local = &sds.local_stat;
	busiest = &sds.busiest_stat;

	/* ASYM feature bypasses nice load balance check */
	if (check_asym_packing(env, &sds))
		return sds.busiest;

	/* There is no busy sibling group to pull tasks from */
	if (!sds.busiest || busiest->sum_nr_running == 0)
		goto out_balanced;

	/* XXX broken for overlapping NUMA groups */
	sds.avg_load = (SCHED_CAPACITY_SCALE * sds.total_load)
						/ sds.total_capacity;

	/*
	 * If the busiest group is imbalanced the below checks don't
	 * work because they assume all things are equal, which typically
	 * isn't true due to cpus_ptr constraints and the like.
	 */
	if (busiest->group_type == group_imbalanced)
		goto force_balance;

	/*
	 * When dst_cpu is idle, prevent SMP nice and/or asymmetric group
	 * capacities from resulting in underutilization due to avg_load.
	 *
	 * When prefer_spread is enabled, force the balance even when
	 * busiest group has some capacity but loaded with more than 1
	 * task.
	 */
	if (env->idle != CPU_NOT_IDLE && group_has_capacity(env, local) &&
	    (busiest->group_no_capacity || env->prefer_spread))
		goto force_balance;

	/* Misfit tasks should be dealt with regardless of the avg load */
	if (busiest->group_type == group_misfit_task)
		goto force_balance;

	/*
	 * If the local group is busier than the selected busiest group
	 * don't try and pull any tasks.
	 */
	if (local->avg_load >= busiest->avg_load)
		goto out_balanced;

	/*
	 * Don't pull any tasks if this group is already above the domain
	 * average load.
	 */
	if (local->avg_load >= sds.avg_load)
		goto out_balanced;

	if (env->idle == CPU_IDLE) {
		/*
		 * This CPU is idle. If the busiest group is not overloaded
		 * and there is no imbalance between this and busiest group
		 * wrt idle CPUs, it is balanced. The imbalance becomes
		 * significant if the diff is greater than 1 otherwise we
		 * might end up to just move the imbalance on another group
		 */
		if ((busiest->group_type != group_overloaded) &&
				(local->idle_cpus <= (busiest->idle_cpus + 1)))
			goto out_balanced;
	} else {
		/*
		 * In the CPU_NEWLY_IDLE, CPU_NOT_IDLE cases, use
		 * imbalance_pct to be conservative.
		 */
		if (100 * busiest->avg_load <=
				env->sd->imbalance_pct * local->avg_load)
			goto out_balanced;
	}

force_balance:
	/* Looks like there is an imbalance. Compute it */
	env->src_grp_type = busiest->group_type;
	calculate_imbalance(env, &sds);

	/*
	 * If we couldn't find any imbalance, then boost the imbalance
	 * based on the group util.
	 */
	if (!env->imbalance && env->prefer_spread)
		env->imbalance = (busiest->group_util >> 1);

	trace_sched_load_balance_stats(sds.busiest->cpumask[0],
				busiest->group_type, busiest->avg_load,
				busiest->load_per_task,	sds.local->cpumask[0],
				local->group_type, local->avg_load,
				local->load_per_task,
				sds.avg_load, env->imbalance);
	return env->imbalance ? sds.busiest : NULL;

out_balanced:
	env->imbalance = 0;
	return NULL;
}

/*
 * find_busiest_queue - find the busiest runqueue among the CPUs in the group.
 */
static struct rq *find_busiest_queue(struct lb_env *env,
				     struct sched_group *group)
{
	struct rq *busiest = NULL, *rq;
	unsigned long busiest_load = 0, busiest_capacity = 1;
	int i;

	for_each_cpu_and(i, sched_group_span(group), env->cpus) {
		unsigned long capacity, load;
		enum fbq_type rt;

		rq = cpu_rq(i);
		rt = fbq_classify_rq(rq);

		/*
		 * We classify groups/runqueues into three groups:
		 *  - regular: there are !numa tasks
		 *  - remote:  there are numa tasks that run on the 'wrong' node
		 *  - all:     there is no distinction
		 *
		 * In order to avoid migrating ideally placed numa tasks,
		 * ignore those when there's better options.
		 *
		 * If we ignore the actual busiest queue to migrate another
		 * task, the next balance pass can still reduce the busiest
		 * queue by moving tasks around inside the node.
		 *
		 * If we cannot move enough load due to this classification
		 * the next pass will adjust the group classification and
		 * allow migration of more tasks.
		 *
		 * Both cases only affect the total convergence complexity.
		 */
		if (rt > env->fbq_type)
			continue;

		/*
		 * For ASYM_CPUCAPACITY domains with misfit tasks we simply
		 * seek the "biggest" misfit task.
		 */
		if (env->src_grp_type == group_misfit_task) {
			if (rq->misfit_task_load > busiest_load) {
				busiest_load = rq->misfit_task_load;
				busiest = rq;
			}

			continue;
		}

		/*
		 * Ignore cpu, which is undergoing active_balance and doesn't
		 * have more than 2 tasks.
		 */
		if (rq->active_balance && rq->nr_running <= 2)
			continue;

		capacity = capacity_of(i);

		/*
		 * For ASYM_CPUCAPACITY domains, don't pick a CPU that could
		 * eventually lead to active_balancing high->low capacity.
		 * Higher per-CPU capacity is considered better than balancing
		 * average load.
		 */
		if (env->sd->flags & SD_ASYM_CPUCAPACITY &&
		    capacity_of(env->dst_cpu) < capacity &&
		    (rq->nr_running == 1 ||
				(rq->nr_running == 2 && task_util(rq->curr) <
				sched_small_task_threshold)))
			continue;

		load = cpu_runnable_load(rq);

		/*
		 * When comparing with imbalance, use cpu_runnable_load()
		 * which is not scaled with the CPU capacity.
		 */

		if (rq->nr_running == 1 && load > env->imbalance &&
		    !check_cpu_capacity(rq, env->sd))
			continue;

		/*
		 * For the load comparisons with the other CPU's, consider
		 * the cpu_runnable_load() scaled with the CPU capacity, so
		 * that the load can be moved away from the CPU that is
		 * potentially running at a lower capacity.
		 *
		 * Thus we're looking for max(load_i / capacity_i), crosswise
		 * multiplication to rid ourselves of the division works out
		 * to: load_i * capacity_j > load_j * capacity_i;  where j is
		 * our previous maximum.
		 */
		if (load * busiest_capacity >= busiest_load * capacity) {
			busiest_load = load;
			busiest_capacity = capacity;
			busiest = rq;
		}
	}

	return busiest;
}

/*
 * Max backoff if we encounter pinned tasks. Pretty arbitrary value, but
 * so long as it is large enough.
 */
#define MAX_PINNED_INTERVAL	512
#define NEED_ACTIVE_BALANCE_THRESHOLD 10

static inline bool
asym_active_balance(struct lb_env *env)
{
	/*
	 * ASYM_PACKING needs to force migrate tasks from busy but
	 * lower priority CPUs in order to pack all tasks in the
	 * highest priority CPUs.
	 */
	return env->idle != CPU_NOT_IDLE && (env->sd->flags & SD_ASYM_PACKING) &&
	       sched_asym_prefer(env->dst_cpu, env->src_cpu);
}

static inline bool
voluntary_active_balance(struct lb_env *env)
{
	struct sched_domain *sd = env->sd;

	if (asym_active_balance(env))
		return 1;

	/*
	 * The dst_cpu is idle and the src_cpu CPU has only 1 CFS task.
	 * It's worth migrating the task if the src_cpu's capacity is reduced
	 * because of other sched_class or IRQs if more capacity stays
	 * available on dst_cpu.
	 */
	if ((env->idle != CPU_NOT_IDLE) &&
	    (env->src_rq->cfs.h_nr_running == 1)) {
		if ((check_cpu_capacity(env->src_rq, sd)) &&
		    (capacity_of(env->src_cpu)*sd->imbalance_pct < capacity_of(env->dst_cpu)*100))
			return 1;
	}

	if (env->idle != CPU_NOT_IDLE &&
			env->src_grp_type == group_misfit_task)
		return 1;

	return 0;
}

static int need_active_balance(struct lb_env *env)
{
	struct sched_domain *sd = env->sd;

	if (voluntary_active_balance(env))
		return 1;

	if ((env->idle != CPU_NOT_IDLE) &&
		(capacity_of(env->src_cpu) < capacity_of(env->dst_cpu)) &&
		((capacity_orig_of(env->src_cpu) <
				capacity_orig_of(env->dst_cpu))) &&
				env->src_rq->cfs.h_nr_running == 1 &&
				cpu_overutilized(env->src_cpu) &&
				!cpu_overutilized(env->dst_cpu)) {
		return 1;
	}

	if (env->src_grp_type == group_overloaded && env->src_rq->misfit_task_load)
		return 1;

	return unlikely(sd->nr_balance_failed > sd->cache_nice_tries+2);
}

static int should_we_balance(struct lb_env *env)
{
	struct sched_group *sg = env->sd->groups;
	int cpu, balance_cpu = -1;

	/*
	 * Ensure the balancing environment is consistent; can happen
	 * when the softirq triggers 'during' hotplug.
	 */
	if (!cpumask_test_cpu(env->dst_cpu, env->cpus))
		return 0;

	/*
	 * In the newly idle case, we will allow all the CPUs
	 * to do the newly idle load balance.
	 */
	if (env->idle == CPU_NEWLY_IDLE)
		return 1;

	/* Try to find first idle CPU */
	for_each_cpu_and(cpu, group_balance_mask(sg), env->cpus) {
		if (!idle_cpu(cpu) || cpu_isolated(cpu))
			continue;

		balance_cpu = cpu;
		break;
	}

	if (balance_cpu == -1)
		balance_cpu = group_balance_cpu_not_isolated(sg);

	/*
	 * First idle CPU or the first CPU(busiest) in this sched group
	 * is eligible for doing load balancing at this and above domains.
	 */
	return balance_cpu == env->dst_cpu;
}

/*
 * Check this_cpu to ensure it is balanced within domain. Attempt to move
 * tasks if there is an imbalance.
 */
static int load_balance(int this_cpu, struct rq *this_rq,
			struct sched_domain *sd, enum cpu_idle_type idle,
			int *continue_balancing)
{
	int ld_moved = 0, cur_ld_moved, active_balance = 0;
	struct sched_domain *sd_parent = sd->parent;
	struct sched_group *group = NULL;
	struct rq *busiest = NULL;
	struct rq_flags rf;
	struct cpumask *cpus = this_cpu_cpumask_var_ptr(load_balance_mask);

	struct lb_env env = {
		.sd		= sd,
		.dst_cpu	= this_cpu,
		.dst_rq		= this_rq,
		.dst_grpmask    = group_balance_mask(sd->groups),
		.idle		= idle,
		.loop_break	= sched_nr_migrate_break,
		.cpus		= cpus,
		.fbq_type	= all,
		.tasks		= LIST_HEAD_INIT(env.tasks),
		.imbalance	= 0,
		.flags		= 0,
		.loop		= 0,
	};

#ifdef CONFIG_SCHED_WALT
	env.prefer_spread = (idle != CPU_NOT_IDLE &&
				prefer_spread_on_idle(this_cpu,
				idle == CPU_NEWLY_IDLE) &&
				!((sd->flags & SD_ASYM_CPUCAPACITY) &&
				 !cpumask_test_cpu(this_cpu,
						 &asym_cap_sibling_cpus)));
#else
	env.prefer_spread = false;
#endif

	cpumask_and(cpus, sched_domain_span(sd), cpu_active_mask);

	schedstat_inc(sd->lb_count[idle]);

redo:
	if (!should_we_balance(&env)) {
		*continue_balancing = 0;
		goto out_balanced;
	}

	group = find_busiest_group(&env);
	if (!group) {
		schedstat_inc(sd->lb_nobusyg[idle]);
		goto out_balanced;
	}

	busiest = find_busiest_queue(&env, group);
	if (!busiest) {
		schedstat_inc(sd->lb_nobusyq[idle]);
		goto out_balanced;
	}

	BUG_ON(busiest == env.dst_rq);

	schedstat_add(sd->lb_imbalance[idle], env.imbalance);

	env.src_cpu = busiest->cpu;
	env.src_rq = busiest;

	ld_moved = 0;
	if (busiest->nr_running > 1) {
		/*
		 * Attempt to move tasks. If find_busiest_group has found
		 * an imbalance but busiest->nr_running <= 1, the group is
		 * still unbalanced. ld_moved simply stays zero, so it is
		 * correctly treated as an imbalance.
		 */
		env.flags |= LBF_ALL_PINNED;
		env.loop_max  = min(sysctl_sched_nr_migrate, busiest->nr_running);

more_balance:
		rq_lock_irqsave(busiest, &rf);

		/*
		 * The world might have changed. Validate assumptions.
		 * And also, if the busiest cpu is undergoing active_balance,
		 * it doesn't need help if it has less than 2 tasks on it.
		 */
		if (busiest->nr_running <= 1 ||
		    (busiest->active_balance && busiest->nr_running <= 2)) {
			rq_unlock_irqrestore(busiest, &rf);
			env.flags &= ~LBF_ALL_PINNED;
			goto no_move;
		}

		update_rq_clock(busiest);

		/*
		 * cur_ld_moved - load moved in current iteration
		 * ld_moved     - cumulative load moved across iterations
		 */
		cur_ld_moved = detach_tasks(&env);

		/*
		 * We've detached some tasks from busiest_rq. Every
		 * task is masked "TASK_ON_RQ_MIGRATING", so we can safely
		 * unlock busiest->lock, and we are able to be sure
		 * that nobody can manipulate the tasks in parallel.
		 * See task_rq_lock() family for the details.
		 */

		rq_unlock(busiest, &rf);

		if (cur_ld_moved) {
			attach_tasks(&env);
			ld_moved += cur_ld_moved;
		}

		local_irq_restore(rf.flags);

		if (env.flags & LBF_NEED_BREAK) {
			env.flags &= ~LBF_NEED_BREAK;
			goto more_balance;
		}

		/*
		 * Revisit (affine) tasks on src_cpu that couldn't be moved to
		 * us and move them to an alternate dst_cpu in our sched_group
		 * where they can run. The upper limit on how many times we
		 * iterate on same src_cpu is dependent on number of CPUs in our
		 * sched_group.
		 *
		 * This changes load balance semantics a bit on who can move
		 * load to a given_cpu. In addition to the given_cpu itself
		 * (or a ilb_cpu acting on its behalf where given_cpu is
		 * nohz-idle), we now have balance_cpu in a position to move
		 * load to given_cpu. In rare situations, this may cause
		 * conflicts (balance_cpu and given_cpu/ilb_cpu deciding
		 * _independently_ and at _same_ time to move some load to
		 * given_cpu) causing exceess load to be moved to given_cpu.
		 * This however should not happen so much in practice and
		 * moreover subsequent load balance cycles should correct the
		 * excess load moved.
		 */
		if ((env.flags & LBF_DST_PINNED) && env.imbalance > 0) {

			/* Prevent to re-select dst_cpu via env's CPUs */
			__cpumask_clear_cpu(env.dst_cpu, env.cpus);

			env.dst_rq	 = cpu_rq(env.new_dst_cpu);
			env.dst_cpu	 = env.new_dst_cpu;
			env.flags	&= ~LBF_DST_PINNED;
			env.loop	 = 0;
			env.loop_break	 = sched_nr_migrate_break;

			/*
			 * Go back to "more_balance" rather than "redo" since we
			 * need to continue with same src_cpu.
			 */
			goto more_balance;
		}

		/*
		 * We failed to reach balance because of affinity.
		 */
		if (sd_parent) {
			int *group_imbalance = &sd_parent->groups->sgc->imbalance;

			if ((env.flags & LBF_SOME_PINNED) && env.imbalance > 0)
				*group_imbalance = 1;
		}

		/* All tasks on this runqueue were pinned by CPU affinity */
		if (unlikely(env.flags & LBF_ALL_PINNED)) {
			__cpumask_clear_cpu(cpu_of(busiest), cpus);
			/*
			 * Attempting to continue load balancing at the current
			 * sched_domain level only makes sense if there are
			 * active CPUs remaining as possible busiest CPUs to
			 * pull load from which are not contained within the
			 * destination group that is receiving any migrated
			 * load.
			 */
			if (!cpumask_subset(cpus, env.dst_grpmask)) {
				env.loop = 0;
				env.loop_break = sched_nr_migrate_break;
				goto redo;
			}
			goto out_all_pinned;
		}
	}

no_move:
	if (!ld_moved) {
		/*
		 * Increment the failure counter only on periodic balance.
		 * We do not want newidle balance, which can be very
		 * frequent, pollute the failure counter causing
		 * excessive cache_hot migrations and active balances.
		 */
		if (idle != CPU_NEWLY_IDLE) {
			if (env.src_grp_nr_running > 1)
				sd->nr_balance_failed++;
		}

		if (need_active_balance(&env)) {
			unsigned long flags;

			raw_spin_lock_irqsave(&busiest->lock, flags);

			/*
			 * The CPUs are marked as reserved if tasks
			 * are pushed/pulled from other CPUs. In that case,
			 * bail out from the load balancer.
			 */
			if (is_reserved(this_cpu) ||
					is_reserved(cpu_of(busiest))) {
				raw_spin_unlock_irqrestore(&busiest->lock,
								flags);
				*continue_balancing = 0;
				goto out;
			}

			/*
			 * Don't kick the active_load_balance_cpu_stop,
			 * if the curr task on busiest CPU can't be
			 * moved to this_cpu:
			 */
			if (!cpumask_test_cpu(this_cpu,
						busiest->curr->cpus_ptr) ||
				!can_migrate_boosted_task(busiest->curr,
						cpu_of(busiest), this_cpu)) {
				raw_spin_unlock_irqrestore(&busiest->lock,
							    flags);
				env.flags |= LBF_ALL_PINNED;
				goto out_one_pinned;
			}

			/*
			 * ->active_balance synchronizes accesses to
			 * ->active_balance_work.  Once set, it's cleared
			 * only after active load balance is finished.
			 */
			if (!busiest->active_balance &&
			    !cpu_isolated(cpu_of(busiest))) {
				busiest->active_balance = 1;
				busiest->push_cpu = this_cpu;
				active_balance = 1;
				mark_reserved(this_cpu);
			}
			raw_spin_unlock_irqrestore(&busiest->lock, flags);

			if (active_balance) {
				int ret;

				ret = stop_one_cpu_nowait(cpu_of(busiest),
					active_load_balance_cpu_stop, busiest,
					&busiest->active_balance_work);
				if (!ret) {
					clear_reserved(this_cpu);
					busiest->active_balance = 0;
					active_balance = 0;
				}
				*continue_balancing = 0;
			}

			/* We've kicked active balancing, force task migration. */
			sd->nr_balance_failed = sd->cache_nice_tries +
					NEED_ACTIVE_BALANCE_THRESHOLD - 1;
		}
	} else
		sd->nr_balance_failed = 0;

	if (likely(!active_balance) || voluntary_active_balance(&env)) {
		/* We were unbalanced, so reset the balancing interval */
		sd->balance_interval = sd->min_interval;
	} else {
		/*
		 * If we've begun active balancing, start to back off. This
		 * case may not be covered by the all_pinned logic if there
		 * is only 1 task on the busy runqueue (because we don't call
		 * detach_tasks).
		 */
		if (sd->balance_interval < sd->max_interval)
			sd->balance_interval *= 2;
	}

	goto out;

out_balanced:
	/*
	 * We reach balance although we may have faced some affinity
	 * constraints. Clear the imbalance flag only if other tasks got
	 * a chance to move and fix the imbalance.
	 */
	if (sd_parent && !(env.flags & LBF_ALL_PINNED)) {
		int *group_imbalance = &sd_parent->groups->sgc->imbalance;

		if (*group_imbalance)
			*group_imbalance = 0;
	}

out_all_pinned:
	/*
	 * We reach balance because all tasks are pinned at this level so
	 * we can't migrate them. Let the imbalance flag set so parent level
	 * can try to migrate them.
	 */
	schedstat_inc(sd->lb_balanced[idle]);

	sd->nr_balance_failed = 0;

out_one_pinned:
	ld_moved = 0;

	/*
	 * sched_balance_newidle() disregards balance intervals, so we could
	 * repeatedly reach this code, which would lead to balance_interval
	 * skyrocketting in a short amount of time. Skip the balance_interval
	 * increase logic to avoid that.
	 */
	if (env.idle == CPU_NEWLY_IDLE)
		goto out;

	/* tune up the balancing interval */
	if ((env.flags & LBF_ALL_PINNED &&
	     sd->balance_interval < MAX_PINNED_INTERVAL) ||
	    sd->balance_interval < sd->max_interval)
		sd->balance_interval *= 2;
out:
	trace_sched_load_balance(this_cpu, idle, *continue_balancing,
				 group ? group->cpumask[0] : 0,
				 busiest ? busiest->nr_running : 0,
				 env.imbalance, env.flags, ld_moved,
				 sd->balance_interval, active_balance,
#ifdef CONFIG_SCHED_WALT
				 sd_overutilized(sd),
#else
				 READ_ONCE(this_rq->rd->overutilized),
#endif
				 env.prefer_spread);
	return ld_moved;
}

static inline unsigned long
get_sd_balance_interval(struct sched_domain *sd, int cpu_busy)
{
	unsigned long interval = sd->balance_interval;
#ifdef CONFIG_SCHED_WALT
	unsigned int cpu;
#endif

	if (cpu_busy)
		interval *= sd->busy_factor;

	/* scale ms to jiffies */
	interval = msecs_to_jiffies(interval);
	interval = clamp(interval, 1UL, max_load_balance_interval);

	/*
	 * check if sched domain is marked as overutilized
	 * we ought to only do this on systems which have SD_ASYMCAPACITY
	 * but we want to do it for all sched domains in those systems
	 * So for now, just check if overutilized as a proxy.
	 */
	/*
	 * If we are overutilized and we have a misfit task, then
	 * we want to balance as soon as practically possible, so
	 * we return an interval of zero, except for busy balance.
	 */
#ifdef CONFIG_SCHED_WALT
	if (sd_overutilized(sd) && !cpu_busy) {
		/* we know the root is overutilized, let's check for a misfit task */
		for_each_cpu(cpu, sched_domain_span(sd)) {
			if (cpu_rq(cpu)->misfit_task_load)
				return 1;
		}
	}
#endif
	return interval;
}

static inline void
update_next_balance(struct sched_domain *sd, unsigned long *next_balance)
{
	unsigned long interval, next;

	/* used by idle balance, so cpu_busy = 0 */
	interval = get_sd_balance_interval(sd, 0);
	next = sd->last_balance + interval;

	if (time_after(*next_balance, next))
		*next_balance = next;
}

/*
 * active_load_balance_cpu_stop is run by the CPU stopper. It pushes
 * running tasks off the busiest CPU onto idle CPUs. It requires at
 * least 1 task to be running on each physical CPU where possible, and
 * avoids physical / logical imbalances.
 */
int active_load_balance_cpu_stop(void *data)
{
	struct rq *busiest_rq = data;
	int busiest_cpu = cpu_of(busiest_rq);
	int target_cpu = busiest_rq->push_cpu;
	struct rq *target_rq = cpu_rq(target_cpu);
	struct sched_domain *sd = NULL;
	struct task_struct *p = NULL;
	struct rq_flags rf;
#ifdef CONFIG_SCHED_WALT
	struct task_struct *push_task;
	int push_task_detached = 0;
	struct lb_env env = {
		.sd                     = sd,
		.dst_cpu                = target_cpu,
		.dst_rq                 = target_rq,
		.src_cpu                = busiest_rq->cpu,
		.src_rq                 = busiest_rq,
		.idle                   = CPU_IDLE,
		.flags                  = 0,
		.loop                   = 0,
	};
#endif

	rq_lock_irq(busiest_rq, &rf);
	/*
	 * Between queueing the stop-work and running it is a hole in which
	 * CPUs can become inactive. We should not move tasks from or to
	 * inactive CPUs.
	 */
	if (!cpu_active(busiest_cpu) || !cpu_active(target_cpu))
		goto out_unlock;

	/* Make sure the requested CPU hasn't gone down in the meantime: */
	if (unlikely(busiest_cpu != smp_processor_id() ||
		     !busiest_rq->active_balance))
		goto out_unlock;

	/* Is there any task to move? */
	if (busiest_rq->nr_running <= 1)
		goto out_unlock;

	/*
	 * This condition is "impossible", if it occurs
	 * we need to fix it. Originally reported by
	 * Bjorn Helgaas on a 128-CPU setup.
	 */
	BUG_ON(busiest_rq == target_rq);

#ifdef CONFIG_SCHED_WALT
	push_task = busiest_rq->wrq.push_task;
	target_cpu = busiest_rq->push_cpu;
	if (push_task) {
		if (task_on_rq_queued(push_task) &&
			push_task->state == TASK_RUNNING &&
			task_cpu(push_task) == busiest_cpu &&
					cpu_online(target_cpu)) {
			update_rq_clock(busiest_rq);
			detach_task(push_task, &env);
			push_task_detached = 1;
		}
		goto out_unlock;
	}
#endif

	/* Search for an sd spanning us and the target CPU. */
	rcu_read_lock();
	for_each_domain(target_cpu, sd) {
		if ((sd->flags & SD_LOAD_BALANCE) &&
		    cpumask_test_cpu(busiest_cpu, sched_domain_span(sd)))
				break;
	}

	if (likely(sd)) {
		struct lb_env env = {
			.sd		= sd,
			.dst_cpu	= target_cpu,
			.dst_rq		= target_rq,
			.src_cpu	= busiest_rq->cpu,
			.src_rq		= busiest_rq,
			.idle		= CPU_IDLE,
			/*
			 * can_migrate_task() doesn't need to compute new_dst_cpu
			 * for active balancing. Since we have CPU_IDLE, but no
			 * @dst_grpmask we need to make that test go away with lying
			 * about DST_PINNED.
			 */
			.flags		= LBF_DST_PINNED,
		};

		schedstat_inc(sd->alb_count);
		update_rq_clock(busiest_rq);

		p = detach_one_task(&env);
		if (p) {
			schedstat_inc(sd->alb_pushed);
			/* Active balancing done, reset the failure counter. */
			sd->nr_balance_failed = 0;
		} else {
			schedstat_inc(sd->alb_failed);
		}
	}
	rcu_read_unlock();
out_unlock:
	busiest_rq->active_balance = 0;
#ifdef CONFIG_SCHED_WALT
	push_task = busiest_rq->wrq.push_task;
#endif
	target_cpu = busiest_rq->push_cpu;
	clear_reserved(target_cpu);

#ifdef CONFIG_SCHED_WALT
	if (push_task)
		busiest_rq->wrq.push_task = NULL;
#endif

	rq_unlock(busiest_rq, &rf);

#ifdef CONFIG_SCHED_WALT
	if (push_task) {
		if (push_task_detached)
			attach_one_task(target_rq, push_task);
		put_task_struct(push_task);
	}
#endif

	if (p)
		attach_one_task(target_rq, p);

	local_irq_enable();

	return 0;
}

static DEFINE_SPINLOCK(balancing);

/*
 * Scale the max load_balance interval with the number of CPUs in the system.
 * This trades load-balance latency on larger machines for less cross talk.
 */
void update_max_interval(void)
{
	unsigned int available_cpus;
#ifdef CONFIG_SCHED_WALT
	cpumask_t avail_mask;

	cpumask_andnot(&avail_mask, cpu_online_mask, cpu_isolated_mask);
	available_cpus = cpumask_weight(&avail_mask);
#else
	available_cpus = num_online_cpus();
#endif

	max_load_balance_interval = HZ*available_cpus/10;
}

/*
 * It checks each scheduling domain to see if it is due to be balanced,
 * and initiates a balancing operation if so.
 *
 * Balancing parameters are set up in init_sched_domains.
 */
static void rebalance_domains(struct rq *rq, enum cpu_idle_type idle)
{
	int continue_balancing = 1;
	int cpu = rq->cpu;
	unsigned long interval;
	struct sched_domain *sd;
	/* Earliest time when we have to do rebalance again */
	unsigned long next_balance = jiffies + 60*HZ;
	int update_next_balance = 0;
	int need_serialize, need_decay = 0;
	u64 max_cost = 0;

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		/*
		 * Decay the newidle max times here because this is a regular
		 * visit to all the domains. Decay ~1% per second.
		 */
		if (time_after(jiffies, sd->next_decay_max_lb_cost)) {
			sd->max_newidle_lb_cost =
				(sd->max_newidle_lb_cost * 253) / 256;
			sd->next_decay_max_lb_cost = jiffies + HZ;
			need_decay = 1;
		}
		max_cost += sd->max_newidle_lb_cost;

#ifdef CONFIG_SCHED_WALT
		if (!sd_overutilized(sd) && !prefer_spread_on_idle(cpu,
					idle == CPU_NEWLY_IDLE))
			continue;
#endif

		if (!(sd->flags & SD_LOAD_BALANCE))
			continue;

		/*
		 * Stop the load balance at this level. There is another
		 * CPU in our sched group which is doing load balancing more
		 * actively.
		 */
		if (!continue_balancing) {
			if (need_decay)
				continue;
			break;
		}

		interval = get_sd_balance_interval(sd, idle != CPU_IDLE);

		need_serialize = sd->flags & SD_SERIALIZE;
		if (need_serialize) {
			if (!spin_trylock(&balancing))
				goto out;
		}

		if (time_after_eq(jiffies, sd->last_balance + interval)) {
			if (load_balance(cpu, rq, sd, idle, &continue_balancing)) {
				/*
				 * The LBF_DST_PINNED logic could have changed
				 * env->dst_cpu, so we can't know our idle
				 * state even if we migrated tasks. Update it.
				 */
				idle = idle_cpu(cpu) ? CPU_IDLE : CPU_NOT_IDLE;
			}
			sd->last_balance = jiffies;
			interval = get_sd_balance_interval(sd, idle != CPU_IDLE);
		}
		if (need_serialize)
			spin_unlock(&balancing);
out:
		if (time_after(next_balance, sd->last_balance + interval)) {
			next_balance = sd->last_balance + interval;
			update_next_balance = 1;
		}
	}
	if (need_decay) {
		/*
		 * Ensure the rq-wide value also decays but keep it at a
		 * reasonable floor to avoid funnies with rq->avg_idle.
		 */
		rq->max_idle_balance_cost =
			max((u64)sysctl_sched_migration_cost, max_cost);
	}
	rcu_read_unlock();

	/*
	 * next_balance will be updated only when there is a need.
	 * When the cpu is attached to null domain for ex, it will not be
	 * updated.
	 */
	if (likely(update_next_balance)) {
		rq->next_balance = next_balance;

#ifdef CONFIG_NO_HZ_COMMON
		/*
		 * If this CPU has been elected to perform the nohz idle
		 * balance. Other idle CPUs have already rebalanced with
		 * nohz_idle_balance() and nohz.next_balance has been
		 * updated accordingly. This CPU is now running the idle load
		 * balance for itself and we need to update the
		 * nohz.next_balance accordingly.
		 */
		if ((idle == CPU_IDLE) && time_after(nohz.next_balance, rq->next_balance))
			nohz.next_balance = rq->next_balance;
#endif
	}
}

static inline int on_null_domain(struct rq *rq)
{
	return unlikely(!rcu_dereference_sched(rq->sd));
}

#ifdef CONFIG_NO_HZ_COMMON
static inline int find_energy_aware_new_ilb(void)
{
	int ilb = nr_cpu_ids;
	struct sched_domain *sd;
	int cpu = raw_smp_processor_id();
	cpumask_t idle_cpus, tmp_cpus;
	struct sched_group *sg;
	unsigned long ref_cap = capacity_orig_of(cpu);
	unsigned long best_cap = 0, best_cap_cpu = -1;

	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_asym_cpucapacity, cpu));
	if (!sd)
		goto out;

	cpumask_and(&idle_cpus, nohz.idle_cpus_mask,
			housekeeping_cpumask(HK_FLAG_MISC));
#ifdef CONFIG_SCHED_WALT
	cpumask_andnot(&idle_cpus, &idle_cpus, cpu_isolated_mask);
#endif

	sg = sd->groups;
	do {
		int i;
		unsigned long cap;

		cpumask_and(&tmp_cpus, &idle_cpus, sched_group_span(sg));
		i = cpumask_first(&tmp_cpus);

		/* This sg did not have any idle CPUs */
		if (i >= nr_cpu_ids)
			continue;

		cap = capacity_orig_of(i);

		/* The first preference is for the same capacity CPU */
		if (cap == ref_cap) {
			ilb = i;
			break;
		}

		/*
		 * When there are no idle CPUs in the same capacity group,
		 * we find the next best capacity CPU.
		 */
		if (best_cap > ref_cap) {
			if (cap > ref_cap && cap < best_cap) {
				best_cap = cap;
				best_cap_cpu = i;
			}
			continue;
		}

		if (cap > best_cap) {
			best_cap = cap;
			best_cap_cpu = i;
		}

	} while (sg = sg->next, sg != sd->groups);

	if (best_cap_cpu != -1)
		ilb = best_cap_cpu;
out:
	rcu_read_unlock();
	return ilb;
}

/*
 * idle load balancing details
 * - When one of the busy CPUs notice that there may be an idle rebalancing
 *   needed, they will kick the idle load balancer, which then does idle
 *   load balancing for all the idle CPUs.
 * - HK_FLAG_MISC CPUs are used for this task, because HK_FLAG_SCHED not set
 *   anywhere yet.
 */

static inline int find_new_ilb(void)
{
	int ilb;

	if (static_branch_likely(&sched_asym_cpucapacity))
		return find_energy_aware_new_ilb();

	for_each_cpu_and(ilb, nohz.idle_cpus_mask,
			      housekeeping_cpumask(HK_FLAG_MISC)) {
#ifdef CONFIG_SCHED_WALT
		if (cpu_isolated(ilb))
			continue;
#endif
		if (idle_cpu(ilb))
			return ilb;
	}

	return nr_cpu_ids;
}

/*
 * Kick a CPU to do the nohz balancing, if it is time for it. We pick any
 * idle CPU in the HK_FLAG_MISC housekeeping set (if there is one).
 */
static void kick_ilb(unsigned int flags)
{
	int ilb_cpu;

	/*
	 * Increase nohz.next_balance only when if full ilb is triggered but
	 * not if we only update stats.
	 */
	if (flags & NOHZ_BALANCE_KICK)
		nohz.next_balance = jiffies+1;

	ilb_cpu = find_new_ilb();

	if (ilb_cpu >= nr_cpu_ids)
		return;

	flags = atomic_fetch_or(flags, nohz_flags(ilb_cpu));
	if (flags & NOHZ_KICK_MASK)
		return;

	/*
	 * Use smp_send_reschedule() instead of resched_cpu().
	 * This way we generate a sched IPI on the target CPU which
	 * is idle. And the softirq performing nohz idle load balance
	 * will be run before returning from the IPI.
	 */
	trace_sched_load_balance_nohz_kick(smp_processor_id(), ilb_cpu);
	smp_send_reschedule(ilb_cpu);
}

/*
 * Current decision point for kicking the idle load balancer in the presence
 * of idle CPUs in the system.
 */
static void nohz_balancer_kick(struct rq *rq)
{
	unsigned long now = jiffies;
	struct sched_domain_shared *sds;
	struct sched_domain *sd;
	int nr_busy, i, cpu = rq->cpu;
	unsigned int flags = 0;
	cpumask_t cpumask;

	if (unlikely(rq->idle_balance))
		return;

	/*
	 * We may be recently in ticked or tickless idle mode. At the first
	 * busy tick after returning from idle, we will update the busy stats.
	 */
	nohz_balance_exit_idle(rq);

	/*
	 * None are in tickless mode and hence no need for NOHZ idle load
	 * balancing.
	 */
#ifdef CONFIG_SCHED_WALT
	cpumask_andnot(&cpumask, nohz.idle_cpus_mask, cpu_isolated_mask);
	if (cpumask_empty(&cpumask))
		return;
#else
	cpumask_copy(&cpumask, nohz.idle_cpus_mask);
	if (likely(!atomic_read(&nohz.nr_cpus)))
		return;
#endif

	if (READ_ONCE(nohz.has_blocked) &&
	    time_after(now, READ_ONCE(nohz.next_blocked)))
		flags = NOHZ_STATS_KICK;

	if (time_before(now, nohz.next_balance))
		goto out;

	/*
	 * With EAS, no-hz idle balance is allowed only when the CPU
	 * is overutilized and has 2 tasks. The misfit task migration
	 * happens from the tickpath.
	 */
	if (sched_energy_enabled()) {
		if (rq->nr_running >= 2 && (cpu_overutilized(cpu) ||
					prefer_spread_on_idle(cpu, false)))
			flags = NOHZ_KICK_MASK;
		goto out;
	}

	if (rq->nr_running >= 2) {
		flags = NOHZ_KICK_MASK;
		goto out;
	}

	rcu_read_lock();

	sd = rcu_dereference(rq->sd);
	if (sd) {
		/*
		 * If there's a CFS task and the current CPU has reduced
		 * capacity; kick the ILB to see if there's a better CPU to run
		 * on.
		 */
		if (rq->cfs.h_nr_running >= 1 && check_cpu_capacity(rq, sd)) {
			flags = NOHZ_KICK_MASK;
			goto unlock;
		}
	}

	sd = rcu_dereference(per_cpu(sd_asym_packing, cpu));
	if (sd) {
		/*
		 * When ASYM_PACKING; see if there's a more preferred CPU
		 * currently idle; in which case, kick the ILB to move tasks
		 * around.
		 */
		for_each_cpu_and(i, sched_domain_span(sd), &cpumask) {
			if (sched_asym_prefer(i, cpu)) {
				flags = NOHZ_KICK_MASK;
				goto unlock;
			}
		}
	}

	sd = rcu_dereference(per_cpu(sd_asym_cpucapacity, cpu));
	if (sd) {
		/*
		 * When ASYM_CPUCAPACITY; see if there's a higher capacity CPU
		 * to run the misfit task on.
		 */
		if (check_misfit_status(rq, sd)) {
			flags = NOHZ_KICK_MASK;
			goto unlock;
		}

		/*
		 * For asymmetric systems, we do not want to nicely balance
		 * cache use, instead we want to embrace asymmetry and only
		 * ensure tasks have enough CPU capacity.
		 *
		 * Skip the LLC logic because it's not relevant in that case.
		 */
		goto unlock;
	}

	sds = rcu_dereference(per_cpu(sd_llc_shared, cpu));
	if (sds) {
		/*
		 * If there is an imbalance between LLC domains (IOW we could
		 * increase the overall cache use), we need some less-loaded LLC
		 * domain to pull some load. Likewise, we may need to spread
		 * load within the current LLC domain (e.g. packed SMT cores but
		 * other CPUs are idle). We can't really know from here how busy
		 * the others are - so just get a nohz balance going if it looks
		 * like this LLC domain has tasks we could move.
		 */
		nr_busy = atomic_read(&sds->nr_busy_cpus);
		if (nr_busy > 1) {
			flags = NOHZ_KICK_MASK;
			goto unlock;
		}
	}
unlock:
	rcu_read_unlock();
out:
	if (flags)
		kick_ilb(flags);
}

static void set_cpu_sd_state_busy(int cpu)
{
	struct sched_domain *sd;

	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_llc, cpu));

	if (!sd || !sd->nohz_idle)
		goto unlock;
	sd->nohz_idle = 0;

	atomic_inc(&sd->shared->nr_busy_cpus);
unlock:
	rcu_read_unlock();
}

void nohz_balance_exit_idle(struct rq *rq)
{
	SCHED_WARN_ON(rq != this_rq());

	if (likely(!rq->nohz_tick_stopped))
		return;

	rq->nohz_tick_stopped = 0;
	cpumask_clear_cpu(rq->cpu, nohz.idle_cpus_mask);
	atomic_dec(&nohz.nr_cpus);

	set_cpu_sd_state_busy(rq->cpu);
}

static void set_cpu_sd_state_idle(int cpu)
{
	struct sched_domain *sd;

	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_llc, cpu));

	if (!sd || sd->nohz_idle)
		goto unlock;
	sd->nohz_idle = 1;

	atomic_dec(&sd->shared->nr_busy_cpus);
unlock:
	rcu_read_unlock();
}

/*
 * This routine will record that the CPU is going idle with tick stopped.
 * This info will be used in performing idle load balancing in the future.
 */
void nohz_balance_enter_idle(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	SCHED_WARN_ON(cpu != smp_processor_id());

	/* If this CPU is going down, then nothing needs to be done: */
	if (!cpu_active(cpu))
		return;

	/* Spare idle load balancing on CPUs that don't want to be disturbed: */
	if (!housekeeping_cpu(cpu, HK_FLAG_SCHED))
		return;

	/*
	 * Can be set safely without rq->lock held
	 * If a clear happens, it will have evaluated last additions because
	 * rq->lock is held during the check and the clear
	 */
	rq->has_blocked_load = 1;

	/*
	 * The tick is still stopped but load could have been added in the
	 * meantime. We set the nohz.has_blocked flag to trig a check of the
	 * *_avg. The CPU is already part of nohz.idle_cpus_mask so the clear
	 * of nohz.has_blocked can only happen after checking the new load
	 */
	if (rq->nohz_tick_stopped)
		goto out;

	/* If we're a completely isolated CPU, we don't play: */
	if (on_null_domain(rq))
		return;

	rq->nohz_tick_stopped = 1;

	cpumask_set_cpu(cpu, nohz.idle_cpus_mask);
	atomic_inc(&nohz.nr_cpus);

	/*
	 * Ensures that if nohz_idle_balance() fails to observe our
	 * @idle_cpus_mask store, it must observe the @has_blocked
	 * store.
	 */
	smp_mb__after_atomic();

	set_cpu_sd_state_idle(cpu);

out:
	/*
	 * Each time a cpu enter idle, we assume that it has blocked load and
	 * enable the periodic update of the load of idle cpus
	 */
	WRITE_ONCE(nohz.has_blocked, 1);
}

/*
 * Internal function that runs load balance for all idle cpus. The load balance
 * can be a simple update of blocked load or a complete load balance with
 * tasks movement depending of flags.
 * The function returns false if the loop has stopped before running
 * through all idle CPUs.
 */
static bool _nohz_idle_balance(struct rq *this_rq, unsigned int flags,
			       enum cpu_idle_type idle)
{
	/* Earliest time when we have to do rebalance again */
	unsigned long now = jiffies;
	unsigned long next_balance = now + 60*HZ;
	bool has_blocked_load = false;
	int update_next_balance = 0;
	int this_cpu = this_rq->cpu;
	int balance_cpu;
	int ret = false;
	struct rq *rq;
	cpumask_t cpus;

	SCHED_WARN_ON((flags & NOHZ_KICK_MASK) == NOHZ_BALANCE_KICK);

	/*
	 * We assume there will be no idle load after this update and clear
	 * the has_blocked flag. If a cpu enters idle in the mean time, it will
	 * set the has_blocked flag and trig another update of idle load.
	 * Because a cpu that becomes idle, is added to idle_cpus_mask before
	 * setting the flag, we are sure to not clear the state and not
	 * check the load of an idle cpu.
	 */
	WRITE_ONCE(nohz.has_blocked, 0);

	/*
	 * Ensures that if we miss the CPU, we must see the has_blocked
	 * store from nohz_balance_enter_idle().
	 */
	smp_mb();

#ifdef CONFIG_SCHED_WALT
	cpumask_andnot(&cpus, nohz.idle_cpus_mask, cpu_isolated_mask);
#else
	cpumask_copy(&cpus, nohz.idle_cpus_mask);
#endif

	for_each_cpu(balance_cpu, &cpus) {
		if (balance_cpu == this_cpu || !idle_cpu(balance_cpu))
			continue;

		/*
		 * If this CPU gets work to do, stop the load balancing
		 * work being done for other CPUs. Next load
		 * balancing owner will pick it up.
		 */
		if (need_resched()) {
			has_blocked_load = true;
			goto abort;
		}

		rq = cpu_rq(balance_cpu);

		has_blocked_load |= update_nohz_stats(rq, true);

		/*
		 * If time for next balance is due,
		 * do the balance.
		 */
		if (time_after_eq(jiffies, rq->next_balance)) {
			struct rq_flags rf;

			rq_lock_irqsave(rq, &rf);
			update_rq_clock(rq);
			rq_unlock_irqrestore(rq, &rf);

			if (flags & NOHZ_BALANCE_KICK)
				rebalance_domains(rq, CPU_IDLE);
		}

		if (time_after(next_balance, rq->next_balance)) {
			next_balance = rq->next_balance;
			update_next_balance = 1;
		}
	}

	/*
	 * next_balance will be updated only when there is a need.
	 * When the CPU is attached to null domain for ex, it will not be
	 * updated.
	 */
	if (likely(update_next_balance))
		nohz.next_balance = next_balance;

	/* Newly idle CPU doesn't need an update */
	if (idle != CPU_NEWLY_IDLE) {
		update_blocked_averages(this_cpu);
		has_blocked_load |= this_rq->has_blocked_load;
	}

	if (flags & NOHZ_BALANCE_KICK)
		rebalance_domains(this_rq, CPU_IDLE);

	WRITE_ONCE(nohz.next_blocked,
		now + msecs_to_jiffies(LOAD_AVG_PERIOD));

	/* The full idle balance loop has been done */
	ret = true;

abort:
	/* There is still blocked load, enable periodic update */
	if (has_blocked_load)
		WRITE_ONCE(nohz.has_blocked, 1);

	return ret;
}

/*
 * In CONFIG_NO_HZ_COMMON case, the idle balance kickee will do the
 * rebalancing for all the cpus for whom scheduler ticks are stopped.
 */
static bool nohz_idle_balance(struct rq *this_rq, enum cpu_idle_type idle)
{
	int this_cpu = this_rq->cpu;
	unsigned int flags;

	if (!(atomic_read(nohz_flags(this_cpu)) & NOHZ_KICK_MASK))
		return false;

	if (idle != CPU_IDLE) {
		atomic_andnot(NOHZ_KICK_MASK, nohz_flags(this_cpu));
		return false;
	}

	/* could be _relaxed() */
	flags = atomic_fetch_andnot(NOHZ_KICK_MASK, nohz_flags(this_cpu));
	if (!(flags & NOHZ_KICK_MASK))
		return false;

	_nohz_idle_balance(this_rq, flags, idle);

	return true;
}

static void nohz_newidle_balance(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu;

	/*
	 * This CPU doesn't want to be disturbed by scheduler
	 * housekeeping
	 */
	if (!housekeeping_cpu(this_cpu, HK_FLAG_SCHED))
		return;

	/* Will wake up very soon. No time for doing anything else*/
	if (this_rq->avg_idle < sysctl_sched_migration_cost)
		return;

	/* Don't need to update blocked load of idle CPUs*/
	if (!READ_ONCE(nohz.has_blocked) ||
	    time_before(jiffies, READ_ONCE(nohz.next_blocked)))
		return;

	raw_spin_unlock(&this_rq->lock);
	/*
	 * This CPU is going to be idle and blocked load of idle CPUs
	 * need to be updated. Run the ilb locally as it is a good
	 * candidate for ilb instead of waking up another idle CPU.
	 * Kick an normal ilb if we failed to do the update.
	 */
	if (!_nohz_idle_balance(this_rq, NOHZ_STATS_KICK, CPU_NEWLY_IDLE))
		kick_ilb(NOHZ_STATS_KICK);
	raw_spin_lock(&this_rq->lock);
}

#else /* !CONFIG_NO_HZ_COMMON */
static inline void nohz_balancer_kick(struct rq *rq) { }

static inline bool nohz_idle_balance(struct rq *this_rq, enum cpu_idle_type idle)
{
	return false;
}

static inline void nohz_newidle_balance(struct rq *this_rq) { }
#endif /* CONFIG_NO_HZ_COMMON */

#ifdef CONFIG_SCHED_WALT
static bool silver_has_big_tasks(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (!is_min_capacity_cpu(cpu))
			break;
		if (cpu_rq(cpu)->wrq.walt_stats.nr_big_tasks)
			return true;
	}

	return false;
}
#else
static inline bool silver_has_big_tasks(void)
{
	return false;
}
#endif

/*
 * sched_balance_newidle is called by schedule() if this_cpu is about to become
 * idle. Attempts to pull tasks from other CPUs.
 */
static int sched_balance_newidle(struct rq *this_rq, struct rq_flags *rf)
{
	unsigned long next_balance = jiffies + HZ;
	int this_cpu = this_rq->cpu;
	struct sched_domain *sd;
	int pulled_task = 0;
	u64 curr_cost = 0;
	u64 avg_idle = this_rq->avg_idle;
	bool prefer_spread = prefer_spread_on_idle(this_cpu, true);
	bool force_lb = (!is_min_capacity_cpu(this_cpu) &&
				silver_has_big_tasks() &&
				sysctl_sched_force_lb_enable &&
				(atomic_read(&this_rq->nr_iowait) == 0));


	if (cpu_isolated(this_cpu))
		return 0;

	update_misfit_status(NULL, this_rq);
	/*
	 * We must set idle_stamp _before_ calling idle_balance(), such that we
	 * measure the duration of idle_balance() as idle time.
	 */
	this_rq->idle_stamp = rq_clock(this_rq);

	/*
	 * Do not pull tasks towards !active CPUs...
	 */
	if (!cpu_active(this_cpu))
		return 0;

	if (force_lb || prefer_spread)
		avg_idle = ULLONG_MAX;
	/*
	 * This is OK, because current is on_cpu, which avoids it being picked
	 * for load-balance and preemption/IRQs are still disabled avoiding
	 * further scheduler activity on it and we're being very careful to
	 * re-start the picking loop.
	 */
	rq_unpin_lock(this_rq, rf);

	if (avg_idle < sysctl_sched_migration_cost ||
	    !READ_ONCE(this_rq->rd->overload)) {

		rcu_read_lock();
		sd = rcu_dereference_check_sched_domain(this_rq->sd);
		if (sd)
			update_next_balance(sd, &next_balance);
		rcu_read_unlock();

		nohz_newidle_balance(this_rq);

		goto out;
	}

	raw_spin_unlock(&this_rq->lock);

	update_blocked_averages(this_cpu);
	rcu_read_lock();
	for_each_domain(this_cpu, sd) {
		int continue_balancing = 1;
		u64 t0, domain_cost;

		if (!(sd->flags & SD_LOAD_BALANCE))
			continue;

#ifdef CONFIG_SCHED_WALT
		if (prefer_spread && !force_lb &&
			(sd->flags & SD_ASYM_CPUCAPACITY) &&
			!(cpumask_test_cpu(this_cpu, &asym_cap_sibling_cpus)))
			avg_idle = this_rq->avg_idle;
#endif

		if (avg_idle < curr_cost + sd->max_newidle_lb_cost) {
			update_next_balance(sd, &next_balance);
			break;
		}

		if (sd->flags & SD_BALANCE_NEWIDLE) {
			t0 = sched_clock_cpu(this_cpu);

			pulled_task = load_balance(this_cpu, this_rq,
						   sd, CPU_NEWLY_IDLE,
						   &continue_balancing);

			domain_cost = sched_clock_cpu(this_cpu) - t0;
			if (domain_cost > sd->max_newidle_lb_cost)
				sd->max_newidle_lb_cost = domain_cost;

			curr_cost += domain_cost;
		}

		update_next_balance(sd, &next_balance);

		/*
		 * Stop searching for tasks to pull if there are now runnable
		 * tasks on this rq or if active migration kicked in.
		 */
		if (pulled_task || this_rq->nr_running > 0 ||
						!continue_balancing)
			break;
	}
	rcu_read_unlock();

	raw_spin_lock(&this_rq->lock);

	if (curr_cost > this_rq->max_idle_balance_cost)
		this_rq->max_idle_balance_cost = curr_cost;

out:
	/*
	 * While browsing the domains, we released the rq lock, a task could
	 * have been enqueued in the meantime. Since we're not going idle,
	 * pretend we pulled a task.
	 */
	if (this_rq->cfs.h_nr_running && !pulled_task)
		pulled_task = 1;

	/* Move the next balance forward */
	if (time_after(this_rq->next_balance, next_balance))
		this_rq->next_balance = next_balance;

	/* Is there a task of a high priority class? */
	if (this_rq->nr_running != this_rq->cfs.h_nr_running)
		pulled_task = -1;

	if (pulled_task)
		this_rq->idle_stamp = 0;

	rq_repin_lock(this_rq, rf);

	return pulled_task;
}

/*
 * run_rebalance_domains is triggered when needed from the scheduler tick.
 * Also triggered for nohz idle balancing (with nohz_balancing_kick set).
 */
static __latent_entropy void run_rebalance_domains(struct softirq_action *h)
{
	struct rq *this_rq = this_rq();
	enum cpu_idle_type idle = this_rq->idle_balance ?
						CPU_IDLE : CPU_NOT_IDLE;

	/*
	 * Since core isolation doesn't update nohz.idle_cpus_mask, there
	 * is a possibility this nohz kicked cpu could be isolated. Hence
	 * return if the cpu is isolated.
	 */
	if (cpu_isolated(this_rq->cpu))
		return;

	/*
	 * If this CPU has a pending nohz_balance_kick, then do the
	 * balancing on behalf of the other idle CPUs whose ticks are
	 * stopped. Do nohz_idle_balance *before* rebalance_domains to
	 * give the idle CPUs a chance to load balance. Else we may
	 * load balance only within the local sched_domain hierarchy
	 * and abort nohz_idle_balance altogether if we pull some load.
	 */
	if (nohz_idle_balance(this_rq, idle))
		return;

	/* normal load balance */
	update_blocked_averages(this_rq->cpu);
	rebalance_domains(this_rq, idle);
}

/*
 * Trigger the SCHED_SOFTIRQ if it is time to do periodic load balancing.
 */
void trigger_load_balance(struct rq *rq)
{
	/* Don't need to rebalance while attached to NULL domain or
	 * cpu is isolated.
	 */
	if (unlikely(on_null_domain(rq)) || cpu_isolated(cpu_of(rq)))
		return;

	if (time_after_eq(jiffies, rq->next_balance))
		raise_softirq(SCHED_SOFTIRQ);

	nohz_balancer_kick(rq);
}

static void rq_online_fair(struct rq *rq)
{
	update_sysctl();

	update_runtime_enabled(rq);
}

static void rq_offline_fair(struct rq *rq)
{
	update_sysctl();

	/* Ensure any throttled groups are reachable by pick_next_task */
	unthrottle_offline_cfs_rqs(rq);
}

#endif /* CONFIG_SMP */

/*
 * scheduler tick hitting a task of our scheduling class.
 *
 * NOTE: This function can be called remotely by the tick offload that
 * goes along full dynticks. Therefore no local assumption can be made
 * and everything must be accessed through the @rq and @curr passed in
 * parameters.
 */
static void task_tick_fair(struct rq *rq, struct task_struct *curr, int queued)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &curr->se;
#ifdef CONFIG_SCHED_WALT
	bool old_misfit = curr->wts.misfit;
	bool misfit;
#endif

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);
		entity_tick(cfs_rq, se, queued);
	}

	if (static_branch_unlikely(&sched_numa_balancing))
		task_tick_numa(rq, curr);

	update_misfit_status(curr, rq);

#ifdef CONFIG_SCHED_WALT
	misfit = rq->misfit_task_load;

	if (old_misfit != misfit) {
		walt_adjust_nr_big_tasks(rq, 1, misfit);
		curr->wts.misfit = misfit;
	}
#endif

	update_overutilized_status(task_rq(curr));
}

/*
 * called on fork with the child task as argument from the parent's context
 *  - child not yet on the tasklist
 *  - preemption disabled
 */
static void task_fork_fair(struct task_struct *p)
{
	struct cfs_rq *cfs_rq;
	struct sched_entity *se = &p->se, *curr;
	struct rq *rq = this_rq();
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);

	cfs_rq = task_cfs_rq(current);
	curr = cfs_rq->curr;
	if (curr) {
		update_curr(cfs_rq);
		se->vruntime = curr->vruntime;
	}
	place_entity(cfs_rq, se, 1);

	if (sysctl_sched_child_runs_first && curr && entity_before(curr, se)) {
		/*
		 * Upon rescheduling, sched_class::put_prev_task() will place
		 * 'current' within the tree based on its new key value.
		 */
		swap(curr->vruntime, se->vruntime);
		resched_curr(rq);
	}

	se->vruntime -= cfs_rq->min_vruntime;
	rq_unlock(rq, &rf);
}

/*
 * Priority of the task has changed. Check to see if we preempt
 * the current task.
 */
static void
prio_changed_fair(struct rq *rq, struct task_struct *p, int oldprio)
{
	if (!task_on_rq_queued(p))
		return;

	/*
	 * Reschedule if we are currently running on this runqueue and
	 * our priority decreased, or if we are not currently running on
	 * this runqueue and our priority is higher than the current's
	 */
	if (rq->curr == p) {
		if (p->prio > oldprio)
			resched_curr(rq);
	} else
		check_preempt_curr(rq, p, 0);
}

static inline bool vruntime_normalized(struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	/*
	 * In both the TASK_ON_RQ_QUEUED and TASK_ON_RQ_MIGRATING cases,
	 * the dequeue_entity(.flags=0) will already have normalized the
	 * vruntime.
	 */
	if (p->on_rq)
		return true;

	/*
	 * When !on_rq, vruntime of the task has usually NOT been normalized.
	 * But there are some cases where it has already been normalized:
	 *
	 * - A forked child which is waiting for being woken up by
	 *   wake_up_new_task().
	 * - A task which has been woken up by try_to_wake_up() and
	 *   waiting for actually being woken up by sched_ttwu_pending().
	 */
	if (!se->sum_exec_runtime ||
	    (p->state == TASK_WAKING && p->sched_remote_wakeup))
		return true;

	return false;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/*
 * Propagate the changes of the sched_entity across the tg tree to make it
 * visible to the root
 */
static void propagate_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq;

	list_add_leaf_cfs_rq(cfs_rq_of(se));

	/* Start to propagate at parent */
	se = se->parent;

	for_each_sched_entity(se) {
		cfs_rq = cfs_rq_of(se);

		if (!cfs_rq_throttled(cfs_rq)){
			update_load_avg(cfs_rq, se, UPDATE_TG);
			list_add_leaf_cfs_rq(cfs_rq);
			continue;
		}

		if (list_add_leaf_cfs_rq(cfs_rq))
			break;
	}
}
#else
static void propagate_entity_cfs_rq(struct sched_entity *se) { }
#endif

static void detach_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	/* Catch up with the cfs_rq and remove our load when we leave */
	update_load_avg(cfs_rq, se, 0);
	detach_entity_load_avg(cfs_rq, se);
	update_tg_load_avg(cfs_rq, false);
	propagate_entity_cfs_rq(se);
}

static void attach_entity_cfs_rq(struct sched_entity *se)
{
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

#ifdef CONFIG_FAIR_GROUP_SCHED
	/*
	 * Since the real-depth could have been changed (only FAIR
	 * class maintain depth value), reset depth properly.
	 */
	se->depth = se->parent ? se->parent->depth + 1 : 0;
#endif

	/* Synchronize entity with its cfs_rq */
	update_load_avg(cfs_rq, se, sched_feat(ATTACH_AGE_LOAD) ? 0 : SKIP_AGE_LOAD);
	attach_entity_load_avg(cfs_rq, se, 0);
	update_tg_load_avg(cfs_rq, false);
	propagate_entity_cfs_rq(se);
}

static void detach_task_cfs_rq(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	if (!vruntime_normalized(p)) {
		/*
		 * Fix up our vruntime so that the current sleep doesn't
		 * cause 'unlimited' sleep bonus.
		 */
		place_entity(cfs_rq, se, 0);
		se->vruntime -= cfs_rq->min_vruntime;
	}

	detach_entity_cfs_rq(se);
}

static void attach_task_cfs_rq(struct task_struct *p)
{
	struct sched_entity *se = &p->se;
	struct cfs_rq *cfs_rq = cfs_rq_of(se);

	attach_entity_cfs_rq(se);

	if (!vruntime_normalized(p))
		se->vruntime += cfs_rq->min_vruntime;
}

static void switched_from_fair(struct rq *rq, struct task_struct *p)
{
	detach_task_cfs_rq(p);
}

static void switched_to_fair(struct rq *rq, struct task_struct *p)
{
	attach_task_cfs_rq(p);

	if (task_on_rq_queued(p)) {
		/*
		 * We were most likely switched from sched_rt, so
		 * kick off the schedule if running, otherwise just see
		 * if we can still preempt the current task.
		 */
		if (rq->curr == p)
			resched_curr(rq);
		else
			check_preempt_curr(rq, p, 0);
	}
}

/* Account for a task changing its policy or group.
 *
 * This routine is mostly called to set cfs_rq->curr field when a task
 * migrates between groups/classes.
 */
static void set_next_task_fair(struct rq *rq, struct task_struct *p, bool first)
{
	struct sched_entity *se = &p->se;

#ifdef CONFIG_SMP
	if (task_on_rq_queued(p)) {
		/*
		 * Move the next running task to the front of the list, so our
		 * cfs_tasks list becomes MRU one.
		 */
		list_move(&se->group_node, &rq->cfs_tasks);
	}
#endif

	for_each_sched_entity(se) {
		struct cfs_rq *cfs_rq = cfs_rq_of(se);

		set_next_entity(cfs_rq, se);
		/* ensure bandwidth has been allocated on our new cfs_rq */
		account_cfs_rq_runtime(cfs_rq, 0);
	}
}

void init_cfs_rq(struct cfs_rq *cfs_rq)
{
	cfs_rq->tasks_timeline = RB_ROOT_CACHED;
	cfs_rq->min_vruntime = (u64)(-(1LL << 20));
#ifndef CONFIG_64BIT
	cfs_rq->min_vruntime_copy = cfs_rq->min_vruntime;
#endif
#ifdef CONFIG_SMP
	raw_spin_lock_init(&cfs_rq->removed.lock);
#endif
}

#ifdef CONFIG_FAIR_GROUP_SCHED
static void task_set_group_fair(struct task_struct *p)
{
	struct sched_entity *se = &p->se;

	set_task_rq(p, task_cpu(p));
	se->depth = se->parent ? se->parent->depth + 1 : 0;
}

static void task_move_group_fair(struct task_struct *p)
{
	detach_task_cfs_rq(p);
	set_task_rq(p, task_cpu(p));

#ifdef CONFIG_SMP
	/* Tell se's cfs_rq has been changed -- migrated */
	p->se.avg.last_update_time = 0;
#endif
	attach_task_cfs_rq(p);
}

static void task_change_group_fair(struct task_struct *p, int type)
{
	switch (type) {
	case TASK_SET_GROUP:
		task_set_group_fair(p);
		break;

	case TASK_MOVE_GROUP:
		task_move_group_fair(p);
		break;
	}
}

void free_fair_sched_group(struct task_group *tg)
{
	int i;

	destroy_cfs_bandwidth(tg_cfs_bandwidth(tg));

	for_each_possible_cpu(i) {
		if (tg->cfs_rq)
			kfree(tg->cfs_rq[i]);
		if (tg->se)
			kfree(tg->se[i]);
	}

	kfree(tg->cfs_rq);
	kfree(tg->se);
}

int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent)
{
	struct sched_entity *se;
	struct cfs_rq *cfs_rq;
	int i;

	tg->cfs_rq = kcalloc(nr_cpu_ids, sizeof(cfs_rq), GFP_KERNEL);
	if (!tg->cfs_rq)
		goto err;
	tg->se = kcalloc(nr_cpu_ids, sizeof(se), GFP_KERNEL);
	if (!tg->se)
		goto err;

	tg->shares = NICE_0_LOAD;

	init_cfs_bandwidth(tg_cfs_bandwidth(tg));

	for_each_possible_cpu(i) {
		cfs_rq = kzalloc_node(sizeof(struct cfs_rq),
				      GFP_KERNEL, cpu_to_node(i));
		if (!cfs_rq)
			goto err;

		se = kzalloc_node(sizeof(struct sched_entity),
				  GFP_KERNEL, cpu_to_node(i));
		if (!se)
			goto err_free_rq;

		init_cfs_rq(cfs_rq);
		init_tg_cfs_entry(tg, cfs_rq, se, i, parent->se[i]);
		init_entity_runnable_average(se);
	}

	return 1;

err_free_rq:
	kfree(cfs_rq);
err:
	return 0;
}

void online_fair_sched_group(struct task_group *tg)
{
	struct sched_entity *se;
	struct rq_flags rf;
	struct rq *rq;
	int i;

	for_each_possible_cpu(i) {
		rq = cpu_rq(i);
		se = tg->se[i];
		rq_lock_irq(rq, &rf);
		update_rq_clock(rq);
		attach_entity_cfs_rq(se);
		sync_throttle(tg, i);
		rq_unlock_irq(rq, &rf);
	}
}

void unregister_fair_sched_group(struct task_group *tg)
{
	unsigned long flags;
	struct rq *rq;
	int cpu;

	for_each_possible_cpu(cpu) {
		if (tg->se[cpu])
			remove_entity_load_avg(tg->se[cpu]);

		/*
		 * Only empty task groups can be destroyed; so we can speculatively
		 * check on_list without danger of it being re-added.
		 */
		if (!tg->cfs_rq[cpu]->on_list)
			continue;

		rq = cpu_rq(cpu);

		raw_spin_lock_irqsave(&rq->lock, flags);
		list_del_leaf_cfs_rq(tg->cfs_rq[cpu]);
		raw_spin_unlock_irqrestore(&rq->lock, flags);
	}
}

void init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
			struct sched_entity *se, int cpu,
			struct sched_entity *parent)
{
	struct rq *rq = cpu_rq(cpu);

	cfs_rq->tg = tg;
	cfs_rq->rq = rq;
	init_cfs_rq_runtime(cfs_rq);

	tg->cfs_rq[cpu] = cfs_rq;
	tg->se[cpu] = se;

	/* se could be NULL for root_task_group */
	if (!se)
		return;

	if (!parent) {
		se->cfs_rq = &rq->cfs;
		se->depth = 0;
	} else {
		se->cfs_rq = parent->my_q;
		se->depth = parent->depth + 1;
	}

	se->my_q = cfs_rq;
	/* guarantee group entities always have weight */
	update_load_set(&se->load, NICE_0_LOAD);
	se->parent = parent;
}

static DEFINE_MUTEX(shares_mutex);

int sched_group_set_shares(struct task_group *tg, unsigned long shares)
{
	int i;

	/*
	 * We can't change the weight of the root cgroup.
	 */
	if (!tg->se[0])
		return -EINVAL;

	shares = clamp(shares, scale_load(MIN_SHARES), scale_load(MAX_SHARES));

	mutex_lock(&shares_mutex);
	if (tg->shares == shares)
		goto done;

	tg->shares = shares;
	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);
		struct sched_entity *se = tg->se[i];
		struct rq_flags rf;

		/* Propagate contribution to hierarchy */
		rq_lock_irqsave(rq, &rf);
		update_rq_clock(rq);
		for_each_sched_entity(se) {
			update_load_avg(cfs_rq_of(se), se, UPDATE_TG);
			update_cfs_group(se);
		}
		rq_unlock_irqrestore(rq, &rf);
	}

done:
	mutex_unlock(&shares_mutex);
	return 0;
}
#else /* CONFIG_FAIR_GROUP_SCHED */

void free_fair_sched_group(struct task_group *tg) { }

int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent)
{
	return 1;
}

void online_fair_sched_group(struct task_group *tg) { }

void unregister_fair_sched_group(struct task_group *tg) { }

#endif /* CONFIG_FAIR_GROUP_SCHED */


static unsigned int get_rr_interval_fair(struct rq *rq, struct task_struct *task)
{
	struct sched_entity *se = &task->se;
	unsigned int rr_interval = 0;

	/*
	 * Time slice is 0 for SCHED_OTHER tasks that are on an otherwise
	 * idle runqueue:
	 */
	if (rq->cfs.load.weight)
		rr_interval = NS_TO_JIFFIES(sched_slice(cfs_rq_of(se), se));

	return rr_interval;
}

/*
 * All the scheduling class methods:
 */
const struct sched_class fair_sched_class = {
	.next			= &idle_sched_class,
	.enqueue_task		= enqueue_task_fair,
	.dequeue_task		= dequeue_task_fair,
	.yield_task		= yield_task_fair,
	.yield_to_task		= yield_to_task_fair,

	.check_preempt_curr	= check_preempt_wakeup,

	.pick_next_task		= pick_next_task_fair,
	.put_prev_task		= put_prev_task_fair,
	.set_next_task          = set_next_task_fair,

#ifdef CONFIG_SMP
	.balance		= balance_fair,
	.select_task_rq		= select_task_rq_fair,
	.migrate_task_rq	= migrate_task_rq_fair,

	.rq_online		= rq_online_fair,
	.rq_offline		= rq_offline_fair,

	.task_dead		= task_dead_fair,
	.set_cpus_allowed	= set_cpus_allowed_common,
#endif

	.task_tick		= task_tick_fair,
	.task_fork		= task_fork_fair,

	.prio_changed		= prio_changed_fair,
	.switched_from		= switched_from_fair,
	.switched_to		= switched_to_fair,

	.get_rr_interval	= get_rr_interval_fair,

	.update_curr		= update_curr_fair,

#ifdef CONFIG_FAIR_GROUP_SCHED
	.task_change_group	= task_change_group_fair,
#endif

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 1,
#endif
};

#ifdef CONFIG_SCHED_DEBUG
void print_cfs_stats(struct seq_file *m, int cpu)
{
	struct cfs_rq *cfs_rq, *pos;

	rcu_read_lock();
	for_each_leaf_cfs_rq_safe(cpu_rq(cpu), cfs_rq, pos)
		print_cfs_rq(m, cpu, cfs_rq);
	rcu_read_unlock();
}

#ifdef CONFIG_NUMA_BALANCING
void show_numa_stats(struct task_struct *p, struct seq_file *m)
{
	int node;
	unsigned long tsf = 0, tpf = 0, gsf = 0, gpf = 0;
	struct numa_group *ng;

	rcu_read_lock();
	ng = rcu_dereference(p->numa_group);
	for_each_online_node(node) {
		if (p->numa_faults) {
			tsf = p->numa_faults[task_faults_idx(NUMA_MEM, node, 0)];
			tpf = p->numa_faults[task_faults_idx(NUMA_MEM, node, 1)];
		}
		if (ng) {
			gsf = ng->faults[task_faults_idx(NUMA_MEM, node, 0)],
			gpf = ng->faults[task_faults_idx(NUMA_MEM, node, 1)];
		}
		print_numa_stats(m, node, tsf, tpf, gsf, gpf);
	}
	rcu_read_unlock();
}
#endif /* CONFIG_NUMA_BALANCING */
#endif /* CONFIG_SCHED_DEBUG */

__init void init_sched_fair_class(void)
{
#ifdef CONFIG_SMP
	open_softirq(SCHED_SOFTIRQ, run_rebalance_domains);

#ifdef CONFIG_NO_HZ_COMMON
	nohz.next_balance = jiffies;
	nohz.next_blocked = jiffies;
	zalloc_cpumask_var(&nohz.idle_cpus_mask, GFP_NOWAIT);
#endif
#endif /* SMP */

}

/*
 * Helper functions to facilitate extracting info from tracepoints.
 */

const struct sched_avg *sched_trace_cfs_rq_avg(struct cfs_rq *cfs_rq)
{
#ifdef CONFIG_SMP
	return cfs_rq ? &cfs_rq->avg : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_cfs_rq_avg);

char *sched_trace_cfs_rq_path(struct cfs_rq *cfs_rq, char *str, int len)
{
	if (!cfs_rq) {
		if (str)
			strlcpy(str, "(null)", len);
		else
			return NULL;
	}

	cfs_rq_tg_path(cfs_rq, str, len);
	return str;
}
EXPORT_SYMBOL_GPL(sched_trace_cfs_rq_path);

int sched_trace_cfs_rq_cpu(struct cfs_rq *cfs_rq)
{
	return cfs_rq ? cpu_of(rq_of(cfs_rq)) : -1;
}
EXPORT_SYMBOL_GPL(sched_trace_cfs_rq_cpu);

const struct sched_avg *sched_trace_rq_avg_rt(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq ? &rq->avg_rt : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_rq_avg_rt);

const struct sched_avg *sched_trace_rq_avg_dl(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq ? &rq->avg_dl : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_rq_avg_dl);

const struct sched_avg *sched_trace_rq_avg_irq(struct rq *rq)
{
#if defined(CONFIG_SMP) && defined(CONFIG_HAVE_SCHED_AVG_IRQ)
	return rq ? &rq->avg_irq : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_rq_avg_irq);

int sched_trace_rq_cpu(struct rq *rq)
{
	return rq ? cpu_of(rq) : -1;
}
EXPORT_SYMBOL_GPL(sched_trace_rq_cpu);

const struct cpumask *sched_trace_rd_span(struct root_domain *rd)
{
#ifdef CONFIG_SMP
	return rd ? rd->span : NULL;
#else
	return NULL;
#endif
}
EXPORT_SYMBOL_GPL(sched_trace_rd_span);
