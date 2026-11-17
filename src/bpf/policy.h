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
 *     bool cg_counted and u32 eff_weight;
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
	vtime += ran * 100 / weight;
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
	return LERTICK_MOVE_TO_LOCAL(home) || steal(home);
}

#endif /* __LERTICK_POLICY_H */
