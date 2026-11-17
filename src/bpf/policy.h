/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 Hungyu Lin <dennylin0707@gmail.com>
 *
 * scx_lertick scheduling policy: per-LLC queues, continuation of the running
 * task, cache-affine pick, sync wakeups and cgroup weights. Queues and CPU
 * selection follow scx_simple; cgroup weights follow scx_flatcg (Tejun Heo).
 *
 * The including file defines, before including this header:
 *   struct task_ctx with u64 runtime_start_ns, bool running, u32 rq_weight,
 *     bool cg_counted, u32 eff_weight, bool episode_valid,
 *     u64 episode_runtime_ns, u64 prediction_ns and struct lertick_model model;
 *   const volatile u32 mode and min_train_samples;
 *   NSEC_PER_MSEC, LERTICK_MAX_CPUS (a power of two) and LERTICK_MAX_DOMAINS;
 *   LERTICK_MOVE_TO_LOCAL(dsq), its kernel's scx_bpf_dsq_move_to_local();
 *   LERTICK_TASK_CGROUP(p), its kernel's scx_bpf_task_cgroup().
 */
#ifndef LERTICK_UPSTREAM
/*
 * Lertick keeps one copy of this policy: the product scheduler
 * (src/bpf/scx_lertick.bpf.c) includes it and scripts/export_scx.py copies it,
 * without this note, to upstream/scx_lertick. Change the policy here only.
 */
#endif /* !LERTICK_UPSTREAM */
#ifndef __LERTICK_POLICY_H
#define __LERTICK_POLICY_H

static u64 vtime_now;
/* Sum of runnable tasks' weights: the virtual clock advances by service / weight. */
static u64 runnable_weight;
/*
 * Sleeper credit: a woken task is placed at most this far behind the clock,
 * about one fixed slice, as EEVDF bounds lag to about one slice.
 */
#define LAG_NS (2ULL * NSEC_PER_MSEC)
/*
 * When taking the next task, prefer one that last ran on this CPU among the
 * first AFFINE_DEPTH queued tasks within AFFINE_WINDOW_NS of the head in
 * vtime.
 */
#define AFFINE_WINDOW_NS (2ULL * NSEC_PER_MSEC)
#define AFFINE_DEPTH 8
/* A sync wakeup may queue on the waker's CPU if within this much vtime of the queue head. */
#define SYNC_WINDOW_NS (2ULL * NSEC_PER_MSEC)
/* Queued tasks examined, and the longest unbroken run, when prev may keep its CPU. */
#define HEAD_SCAN 16
#define KEEP_MAX_NS (20ULL * NSEC_PER_MSEC)

/*
 * One vtime-ordered queue per last-level cache domain; DSQ id d serves domain
 * d. The loader fills cpu_domain from the cache topology; with one domain (the
 * default, or an unknown topology) this is a single shared queue. All queues
 * share one virtual clock, so vtimes compare across domains. A CPU takes work
 * from its own domain first and steals from the others when that is empty.
 */
const volatile u32	   nr_domains = 1;
const volatile u8	   cpu_domain[LERTICK_MAX_CPUS];

static __always_inline u32 domain_count(void)
{
	u32 n = nr_domains;

	if (n < 1)
		n = 1;
	return n < LERTICK_MAX_DOMAINS ? n : LERTICK_MAX_DOMAINS;
}

static __always_inline u32 domain_of(s32 cpu)
{
	u32 idx = cpu, d;

	if (idx >= LERTICK_MAX_CPUS)
		return 0;
	/* Keep the mask: clang may bound one copy of idx and index with another. */
	barrier_var(idx);
	d = cpu_domain[idx & (LERTICK_MAX_CPUS - 1)];
	return d < domain_count() ? d : 0;
}

/* Tasks queued in all domains. */
static __always_inline u64 nr_queued_all(void)
{
	u64 sum = 0;
	u32 d;

	bpf_for(d, 0, domain_count()) sum += scx_bpf_dsq_nr_queued(d);
	return sum;
}

/*
 * A sync waker is about to sleep, so its CPU is the next free one and its
 * cache holds what the wakee will read. Queue the wakee there, unless that
 * would put it well ahead of tasks already waiting in fair order.
 */
static __always_inline bool sync_local(struct task_struct *p, s32 *cpu)
{
	struct bpf_iter_scx_dsq it;
	struct task_struct     *head;
	s32 this   = bpf_get_smp_processor_id();
	u64  vtime = p->scx.dsq_vtime;
	bool ok	   = true;

	if (!bpf_cpumask_test_cpu(this, p->cpus_ptr))
		return false;
	if (time_before(vtime, vtime_now - LAG_NS))
		vtime = vtime_now - LAG_NS;
	if (bpf_iter_scx_dsq_new(&it, domain_of(this), 0) == 0) {
		head = bpf_iter_scx_dsq_next(&it);
		ok   = !head ||
		     !time_before(head->scx.dsq_vtime + SYNC_WINDOW_NS, vtime);
	}
	bpf_iter_scx_dsq_destroy(&it);
	if (!ok)
		return false;
	p->scx.dsq_vtime = vtime;
	*cpu		 = this;
	return true;
}

/*
 * cgroup CPU weights (cpu.weight, systemd CPUWeight=). The hierarchy is
 * flattened as in the sched_ext scx_flatcg example (Tejun Heo, GPL-2.0): a
 * cgroup's hierarchical share (hweight) is the product, at each level, of
 * its weight over the weights of its active siblings. Lertick keeps one
 * virtual clock for all queues, so the share becomes a per-task effective weight:
 * the task's part of its cgroup's runnable weight times the cgroup's share
 * of all runnable weight. With every cgroup at the default weight, and
 * runnable tasks in one cgroup, this equals the task weight.
 */
#define HWEIGHT_ONE (1ULL << 16)

struct cgrp_ctx {
	u32 nr_active; /* this cgroup or a descendant has runnable tasks */
	u32 nr_runnable; /* runnable tasks directly in this cgroup */
	u32 weight;
	u32 hweight; /* share of the whole machine, HWEIGHT_ONE = all */
	u64 child_weight_sum; /* weights of active children */
	u64 hweight_gen;
	u64 task_weight_sum; /* weights of runnable tasks directly in this cgroup */
};

struct {
	__uint(type, BPF_MAP_TYPE_CGRP_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct cgrp_ctx);
} cgrp_ctx_stor SEC(".maps");

private(LERTICK_CGRP) struct bpf_spin_lock cgrp_lock;
static u64				hweight_gen = 1;

static __always_inline struct cgrp_ctx *cgrp_ctx(struct cgroup *cgrp)
{
	return bpf_cgrp_storage_get(&cgrp_ctx_stor, cgrp, 0, 0);
}

static struct cgrp_ctx *cgrp_ancestor_ctx(struct cgroup *cgrp, int level)
{
	struct cgrp_ctx *cgc;

	cgrp = bpf_cgroup_ancestor(cgrp, level);
	if (!cgrp)
		return NULL;
	cgc = cgrp_ctx(cgrp);
	bpf_cgroup_release(cgrp);
	return cgc;
}

static void cgrp_refresh_hweight(struct cgroup *cgrp, struct cgrp_ctx *cgc)
{
	int level;

	if (!cgc->nr_active || cgc->hweight_gen == hweight_gen)
		return;
	bpf_for(level, 0, cgrp->level + 1)
	{
		struct cgrp_ctx *c, *pc;
		bool		 active;

		c = cgrp_ancestor_ctx(cgrp, level);
		if (!c)
			break;
		if (!level) {
			c->hweight     = HWEIGHT_ONE;
			c->hweight_gen = hweight_gen;
			continue;
		}
		pc = cgrp_ancestor_ctx(cgrp, level - 1);
		if (!pc)
			break;
		bpf_spin_lock(&cgrp_lock);
		active = c->nr_active;
		if (active && pc->child_weight_sum) {
			c->hweight_gen = pc->hweight_gen;
			c->hweight     = (u64)pc->hweight * c->weight /
				     pc->child_weight_sum;
			if (!c->hweight)
				c->hweight = 1;
		}
		bpf_spin_unlock(&cgrp_lock);
		if (!active)
			break;
	}
}

/* A cgroup gains or loses its first or last runnable task: update active sums upward. */
static void cgrp_update_active(struct cgroup *cgrp, struct cgrp_ctx *cgc,
			       bool runnable)
{
	bool updated = false;
	int  idx;

	if (runnable) {
		if (__sync_fetch_and_add(&cgc->nr_runnable, 1))
			return;
	} else {
		if (__sync_sub_and_fetch(&cgc->nr_runnable, 1))
			return;
		cgrp_refresh_hweight(cgrp, cgc);
	}
	bpf_for(idx, 0, cgrp->level)
	{
		int		 level = cgrp->level - idx;
		struct cgrp_ctx *c, *pc = NULL;
		bool		 propagate = false;

		c			   = cgrp_ancestor_ctx(cgrp, level);
		if (!c)
			break;
		if (level) {
			pc = cgrp_ancestor_ctx(cgrp, level - 1);
			if (!pc)
				break;
		}
		bpf_spin_lock(&cgrp_lock);
		if (runnable) {
			if (!c->nr_active++) {
				updated = true;
				if (pc) {
					propagate = true;
					pc->child_weight_sum += c->weight;
				}
			}
		} else if (c->nr_active) {
			if (!--c->nr_active) {
				updated = true;
				if (pc) {
					propagate = true;
					pc->child_weight_sum -= c->weight;
				}
			}
		}
		bpf_spin_unlock(&cgrp_lock);
		if (!propagate)
			break;
	}
	if (updated)
		__sync_fetch_and_add(&hweight_gen, 1);
	if (runnable)
		cgrp_refresh_hweight(cgrp, cgc);
}

static void cgrp_account(struct cgroup *cgrp, struct task_ctx *tctx,
			 bool runnable)
{
	struct cgrp_ctx *cgc = cgrp_ctx(cgrp);

	if (!cgc)
		return;
	if (runnable)
		__sync_fetch_and_add(&cgc->task_weight_sum, tctx->rq_weight);
	else
		__sync_fetch_and_sub(&cgc->task_weight_sum, tctx->rq_weight);
	cgrp_update_active(cgrp, cgc, runnable);
	tctx->cg_counted = runnable;
}

static void cgrp_task_runnable(struct task_struct *p, struct task_ctx *tctx,
			       bool runnable)
{
	struct cgroup *cgrp;

	if (runnable == tctx->cg_counted)
		return;
	cgrp = LERTICK_TASK_CGROUP(p);
	if (!cgrp)
		return;
	cgrp_account(cgrp, tctx, runnable);
	bpf_cgroup_release(cgrp);
}

/* Weight to charge service against: the task's share of its cgroup's share. */
static u32 cgrp_effective_weight(struct task_struct *p, struct task_ctx *tctx,
				 u32 weight)
{
	struct cgrp_ctx *cgc;
	struct cgroup	*cgrp;
	u64		 eff, tws, rw = runnable_weight;

	if (!tctx || !tctx->cg_counted)
		return weight;
	cgrp = LERTICK_TASK_CGROUP(p);
	if (!cgrp)
		return weight;
	cgc = cgrp_ctx(cgrp);
	if (!cgc) {
		bpf_cgroup_release(cgrp);
		return weight;
	}
	cgrp_refresh_hweight(cgrp, cgc);
	tws = cgc->task_weight_sum;
	eff = (u64)weight * cgc->hweight / HWEIGHT_ONE;
	bpf_cgroup_release(cgrp);
	if (!tws || !rw)
		return weight;
	eff = eff * rw / tws;
	if (eff < 1)
		eff = 1;
	if (eff > 1000000)
		eff = 1000000;
	return eff;
}

/*
 * Experimental switches, off by default; screening builds turn them on.
 * LERTICK_OVERRUN_YIELD: a task past its predicted burst gets the minimum
 * slice and loses the continuation of keep_prev.
 * LERTICK_WAKE_PREEMPT: a woken task that finds no idle CPU preempts a CPU
 * whose task has run past its predicted burst.
 */
#ifndef LERTICK_OVERRUN_YIELD
#define LERTICK_OVERRUN_YIELD 0
#endif
#ifndef LERTICK_WAKE_PREEMPT
#define LERTICK_WAKE_PREEMPT 0
#endif
/*
 * LERTICK_CPU_REQUEUE: a task preempted in the middle of a burst waits on a
 * queue of its own CPU instead of its LLC's, so it resumes where its cache
 * (and any lock it holds) is; woken tasks still balance through the LLC
 * queue. A CPU runs its own queue's head unless the LLC head is more than
 * CPU_WINDOW_NS earlier; idle CPUs steal from other CPUs' queues.
 */
#ifndef LERTICK_CPU_REQUEUE
#define LERTICK_CPU_REQUEUE 0
#endif
#define CPU_DSQ(cpu)	((u64)LERTICK_MAX_DOMAINS + (u32)(cpu))
#define CPU_WINDOW_NS	(2ULL * NSEC_PER_MSEC)

static __always_inline bool dsq_head(u64 dsq, u64 *vtime)
{
	struct task_struct *q;

	bpf_for_each(scx_dsq, q, dsq, 0) {
		*vtime = q->scx.dsq_vtime;
		return true;
	}
	return false;
}

/* The queue for an enqueued task: its CPU's own after a preemption, else its LLC's. */
static __always_inline u64 enqueue_dsq(struct task_struct *p,
				       struct task_ctx *tctx, u64 enq_flags)
{
	s32 cpu = scx_bpf_task_cpu(p);

	if (LERTICK_CPU_REQUEUE && !(enq_flags & SCX_ENQ_WAKEUP) && tctx &&
	    tctx->episode_active && tctx->episode_runtime_ns && cpu >= 0 &&
	    cpu < LERTICK_MAX_CPUS && bpf_cpumask_test_cpu(cpu, p->cpus_ptr))
		return CPU_DSQ(cpu);
	return domain_of(cpu);
}

/* Create the LLC queues and, with LERTICK_CPU_REQUEUE, one queue per CPU. */
static __always_inline s32 policy_init(void)
{
	u32 d, nr = scx_bpf_nr_cpu_ids();
	s32 ret;

	bpf_for(d, 0, domain_count()) {
		ret = scx_bpf_create_dsq(d, -1);
		if (ret)
			return ret;
	}
	if (!LERTICK_CPU_REQUEUE)
		return 0;
	bpf_for(d, 0, nr) {
		if (d >= LERTICK_MAX_CPUS)
			break;
		ret = scx_bpf_create_dsq(CPU_DSQ(d), -1);
		if (ret)
			return ret;
	}
	return 0;
}

/* With every LLC queue empty, take a task waiting on another CPU's queue. */
static __always_inline bool steal_cpu_queue(s32 cpu)
{
	u32 nr = scx_bpf_nr_cpu_ids(), i;

	if (!LERTICK_CPU_REQUEUE)
		return false;
	bpf_for(i, 0, nr) {
		if (i >= LERTICK_MAX_CPUS)
			break;
		if (i != (u32)cpu && LERTICK_MOVE_TO_LOCAL(CPU_DSQ(i)))
			return true;
	}
	return false;
}

/*
 * A learned burst that has run past its prediction, for example a thread
 * spinning on a lock whose holder waits in the queue.
 */
static __always_inline bool predicted(struct task_ctx *tctx)
{
	return tctx && mode == LERTICK_MODE_ONLINE && tctx->episode_valid &&
	       tctx->model.updates >= min_train_samples && tctx->prediction_ns;
}

static __always_inline bool overran(struct task_ctx *tctx, u64 consumed)
{
	return predicted(tctx) && consumed >= tctx->prediction_ns;
}

/* When the task now running on each CPU passes its prediction; 0 if never. */
struct cpu_run {
	u64 overrun_at;
};

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, LERTICK_MAX_CPUS);
	__type(key, u32);
	__type(value, struct cpu_run);
} cpu_run_map SEC(".maps");

static __always_inline void policy_running(struct task_struct *p,
					   struct task_ctx *tctx)
{
	u32 key = scx_bpf_task_cpu(p);
	struct cpu_run *r;
	u64 now, at = 0;

	if (!LERTICK_WAKE_PREEMPT)
		return;
	r = bpf_map_lookup_elem(&cpu_run_map, &key);
	if (!r)
		return;
	now = bpf_ktime_get_ns();
	if (predicted(tctx)) {
		if (tctx->prediction_ns > tctx->episode_runtime_ns)
			at = now + tctx->prediction_ns -
			     tctx->episode_runtime_ns;
		else
			at = now;
	}
	r->overrun_at = at;
}

static __always_inline void policy_stopping(struct task_struct *p)
{
	u32 key = scx_bpf_task_cpu(p);
	struct cpu_run *r;

	if (!LERTICK_WAKE_PREEMPT)
		return;
	r = bpf_map_lookup_elem(&cpu_run_map, &key);
	if (r)
		r->overrun_at = 0;
}

/*
 * A woken task queued for lack of an idle CPU preempts the first of its
 * previous CPU and two random CPUs whose task has overrun its prediction.
 */
static __always_inline void wake_preempt(struct task_struct *p)
{
	u32 nr = scx_bpf_nr_cpu_ids();
	u64 now = bpf_ktime_get_ns();
	s32 cand[3];
	int i;

	if (!LERTICK_WAKE_PREEMPT || !nr)
		return;
	cand[0] = scx_bpf_task_cpu(p);
	cand[1] = bpf_get_prandom_u32() % nr;
	cand[2] = bpf_get_prandom_u32() % nr;
	bpf_for(i, 0, 3) {
		u32 key;
		struct cpu_run *r;
		s32 cpu = cand[i & 3];

		if (cpu < 0 || !bpf_cpumask_test_cpu(cpu, p->cpus_ptr))
			continue;
		key = cpu;
		r = bpf_map_lookup_elem(&cpu_run_map, &key);
		if (r && r->overrun_at && !time_before(now, r->overrun_at)) {
			r->overrun_at = 0;
			scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
			return;
		}
	}
}

/*
 * Whether prev should keep its CPU instead of taking the next queued task.
 * A task whose slice ends otherwise always yields, so two tasks sharing a
 * CPU alternate and get equal time whatever their weights. prev stays while
 * it is ahead in vtime (its current run included) of the first queued task
 * that can run here. It yields after KEEP_MAX_NS without a break, and when
 * no such task is within the first HEAD_SCAN queued: a task that can
 * run only here must not wait behind prev indefinitely. The own domain is
 * scanned; when it holds nothing for this CPU, the head of each other domain
 * is compared too, since this CPU would steal from it.
 */
static __always_inline bool other_head_before(u32 home, s32 cpu, u64 vtime)
{
	struct task_struct *q;
	u32		    n = domain_count(), i;

	bpf_for(i, 1, n)
	{
		u32 d = home + i;

		if (d >= n)
			d -= n;
		bpf_for_each(scx_dsq, q, d, 0) {
			if (bpf_cpumask_test_cpu(cpu, q->cpus_ptr) &&
			    time_before(q->scx.dsq_vtime, vtime))
				return true;
			break;
		}
	}
	return false;
}

static __always_inline bool keep_prev(struct task_struct *prev,
				      struct task_ctx *tctx, s32 cpu)
{
	struct task_struct *q;
	u64		    vtime = prev->scx.dsq_vtime, ran = 0;
	u32 weight = tctx && tctx->eff_weight ? tctx->eff_weight :
		     prev->scx.weight	      ? prev->scx.weight :
						1;
	u32 home   = domain_of(cpu);
	int n	   = 0;

	if (tctx && tctx->running &&
	    prev->se.sum_exec_runtime > tctx->runtime_start_ns)
		ran = prev->se.sum_exec_runtime - tctx->runtime_start_ns;
	if (ran >= KEEP_MAX_NS)
		return false;
	if (LERTICK_OVERRUN_YIELD && tctx &&
	    overran(tctx, tctx->episode_runtime_ns + ran))
		return false;
	vtime += ran * 100 / weight;
	if (LERTICK_CPU_REQUEUE) {
		u64 hv;

		if (dsq_head(CPU_DSQ(cpu), &hv) && time_before(hv, vtime))
			return false;
	}
	bpf_for_each(scx_dsq, q, home, 0) {
		if (n++ >= HEAD_SCAN)
			return false;
		if (bpf_cpumask_test_cpu(cpu, q->cpus_ptr))
			return !time_before(q->scx.dsq_vtime, vtime);
	}
	return !other_head_before(home, cpu, vtime);
}

/*
 * Take the next task for this CPU. Among the first few queued tasks whose
 * vtime is within a small window of the head, prefer one that last ran here
 * and may still have a warm cache; fair order moves by at most the window.
 * With the own domain empty, steal the head of the next domain that has work.
 */
static __always_inline bool steal(u32 home)
{
	u32 n = domain_count(), i;

	bpf_for(i, 1, n)
	{
		u32 d = home + i;

		if (d >= n)
			d -= n;
		if (LERTICK_MOVE_TO_LOCAL(d))
			return true;
	}
	return false;
}

static __always_inline bool take_next(s32 cpu)
{
	struct task_struct *q;
	u32		    home = domain_of(cpu);
	u64		    head = 0;
	int		    n	 = 0;

	if (LERTICK_CPU_REQUEUE) {
		u64 hv, dv;

		if (dsq_head(CPU_DSQ(cpu), &hv) &&
		    (!dsq_head(home, &dv) ||
		     !time_before(dv + CPU_WINDOW_NS, hv)) &&
		    LERTICK_MOVE_TO_LOCAL(CPU_DSQ(cpu)))
			return true;
	}
	bpf_for_each(scx_dsq, q, home, 0) {
		if (!n)
			head = q->scx.dsq_vtime;
		if (n >= AFFINE_DEPTH ||
		    time_before(head + AFFINE_WINDOW_NS, q->scx.dsq_vtime))
			break;
		if (scx_bpf_task_cpu(q) == cpu &&
		    scx_bpf_dsq_move(BPF_FOR_EACH_ITER, q, SCX_DSQ_LOCAL, 0))
			return true;
		n++;
	}
	return LERTICK_MOVE_TO_LOCAL(home) || steal(home) ||
	       (LERTICK_CPU_REQUEUE && LERTICK_MOVE_TO_LOCAL(CPU_DSQ(cpu))) ||
	       steal_cpu_queue(cpu);
}

#endif /* __LERTICK_POLICY_H */
