# Prior art and verified references

> Archived Phase 0 background research, superseded by Lertick's CPU-only scope.
> Original names (ScxSprout, scx-sprout, and linux-resource-learning), proposed
> phases, and statements below are preserved as historical reasoning. Memory
> allocation, reclaim, DAMON/DAMOS, NUMA memory placement, and joint CPU/memory
> management are background research only, neither active nor backup development
> directions. See the [archive index](README.md), [current Phase 1 results](../PHASE1_RESULTS.md),
> [decision record](../DECISIONS.md), and [current licensing](../../THIRD_PARTY.md).
> Phase 2 implementation has not been approved.

Status: **Phase 0 survey; no implementation or benchmark replication.**

Sources checked: **2026-10-09**. ScxSprout (`scx-sprout`) is a proposed direction awaiting review and approval.

**Verified fact** below means the linked primary source documents the behavior. **Engineering implication** is our interpretation. Reported measurements belong to their authors and do not predict this project's performance. This is a targeted survey, not an exhaustive novelty determination.

## CPU scheduling and integration

| Work | Verified fact | Engineering implication / verification limit |
| --- | --- | --- |
| [Linux sched_ext](https://docs.kernel.org/scheduler/sched-ext.html) | BPF scheduling callbacks and dispatch queues support dynamic policies and detected-error/stall fallback. Interfaces can change between kernels. | Upstream CPU experimentation substrate; pin kernels, verify configuration, and test recovery. Not a learned policy by itself. |
| [scx and tooling](https://github.com/sched-ext/scx) | Provides scheduler implementations, documentation, loader/configuration, and support utilities. | Existing community and serious comparators; do not duplicate the ecosystem unnecessarily. We did not build or benchmark it. |
| [scx_simple and upstream examples](https://github.com/torvalds/linux/blob/master/tools/sched_ext/README.md) | `scx_simple` implements a minimal weighted-vtime/FIFO example. | Necessary baseline. Its small policy is not evidence of performance on complex topologies. |
| [scx C examples](https://github.com/sched-ext/scx-c-examples) | Current separate home of illustrative C examples; README points to upstream `tools/sched_ext` for definitive versions. | Record exact source revisions when implementation begins; old scx paths may be stale. |
| [scx_lavd](https://github.com/sched-ext/scx/blob/main/scheds/rust/scx_lavd/README.md) | Uses measured latency criticality in deadline/slice decisions; BPF makes scheduling decisions while Rust supplies topology/loading/reporting. | Strong adaptive comparator. Its README does not identify an online-trained ML model; adaptation alone is not proof of ML. |
| [scx_rusty](https://github.com/sched-ext/scx/blob/main/scheds/rust/scx_rusty/README.md) | Multi-domain BPF/userspace hybrid with userspace load balancing and tunable domain policy. | Hybrid architecture is established; all-in-BPF operation must earn its complexity through measurements. |
| [scx_layered](https://sched-ext.com/docs/scheds/rust/scx_layered) | Configures task layers and distinct resource/scheduling policies. | Workload classification and CPU isolation are existing features. Learned adaptation must be evaluated against strong configured policies. |
| [Meta yield predictor](https://github.com/sched-ext/scx/blob/main/OVERVIEW.md) | Overview reports prediction of near-term yielding for placement/cache locality, a neural model with one 16-unit hidden layer, syscall-history features, 87% inference accuracy, and 15% Nginx throughput improvement in that experiment. | Direct overlap with learned task behavior. The passage does not verify production deployment, training location, or reproducibility of the ML experiment. Do not reuse its numbers as expected gains. |
| [ML load-balancing experiment, OSS NA 2025](https://static.sched.com/hosted_files/ossna2025/d2/Improve-Load-Balancing-With-Machine-Learning-Techniques-based-on-sched_ext.pdf) | Ching-Chun (Jim) Huang with I Hsin Cheng, Cheng-Yang Chou, and Po-Ying Chiu, June 23, 2025: slides describe BPF feature collection/inference, Rust retraining, and updated weights via maps. | Continuous hybrid learning is prior art. [Referenced branch](https://github.com/vax-r/scx/tree/scx_rusty_MLLB) exists; full code correctness and end-to-end learning were not audited here. |
| [scx_forge](https://github.com/sched-ext/scx/blob/main/scheds/rust/scx_forge/README.md) | AI-oriented base scheduler intended for agent-driven customization and optimization. | Development-time AI is distinct from a runtime online-trained model. No claim about its benchmark performance follows from the README. |
| [scx_rlfifo](https://github.com/sched-ext/scx/blob/main/scheds/experimental/scx_rlfifo/README.md) | Simple userspace round-robin/FIFO template. | **Not reinforcement learning.** Its name must not be used as evidence of an RL scheduler. |

## In-kernel ML and memory systems

| Work | Verified fact | Engineering implication / verification limit |
| --- | --- | --- |
| [KML](https://github.com/sbu-fsl/kernel-ml) | Framework supports OS/storage ML, with readahead and NFS read-size studies. Repository installation requires modified Linux sources and describes a 4.19.51+ kernel. | Fully in-kernel ML predates this project; avoid a generic “first” claim. Upstream-only integration could reduce a concrete installation burden. |
| [ML IDS in eBPF](https://github.com/CN-TU/machine-learning-in-ebpf) | Decision-tree inference and flow-state tracking run in BPF; training is performed separately. README documents Linux 5.3+ loop support and older BCC/Python dependencies. | Demonstrates inference feasibility, not online BPF training or scheduler-hot-path feasibility. |
| [DAMON/DAMOS](https://docs.kernel.org/mm/damon/design.html) | Access monitoring and schemes apply actions matching access patterns with controls including quotas and watermarks; current design also provides monitoring-interval adaptation. | A memory project should build on these controls where possible. Do not describe it as static or incapable of adaptation. |
| [damo](https://github.com/damonitor/damo) | Userspace control and reporting tool for DAMON. | Useful optional observation tool; adds a dependency if bundled into a minimal C-only project. |
| [MGLRU](https://docs.kernel.org/mm/multigen_lru.html) | Recency generations and refault feedback guide reclaim and self-correction. | Mandatory memory-policy baseline; Linux already learns from history through statistical feedback without an ML predictor. |
| [TMO / Senpai](https://www.cs.cmu.edu/~dskarlat/publications/tmo_asplos22.pdf) | Userspace Senpai uses PSI feedback to drive proactive cgroup reclaim. The paper describes stateless `memory.reclaim` and leaves automated/online parameter tuning as future work. | Direct overlap with the proposed memory backup. A learned budget needs comparison with a Senpai-style feedback controller, not just with reclaim disabled. |
| [LearnedCache](https://arxiv.org/html/2605.26168v2) | Offline per-workload training predicts bounded-horizon page reuse; BPF policies use quantized weights. [Installation](https://github.com/JayAndJef/cache_ext_lc) requires custom Linux `6.6.8-cache-ext+`. | Direct learned-page-cache prior art. No-patch deployment and online adaptation are candidate differences to assess, not established novelty. Access hooks provide a sample, not every memory access. |
| [LLAMA](https://research.google/pubs/learning-based-memory-allocation-for-c-server-workloads/) | Predicts C++ object lifetime classes from symbolized calling contexts and arranges huge-page allocation by lifetime. | Userspace heap allocation rather than Linux page allocation. Application context/instrumentation would expand a small BPF MVP. |

## Academic bibliography and distinctions

### CPU scheduling, placement, and policy selection

1. **Jingde Chen, Subho S. Banerjee, Zbigniew T. Kalbarczyk, Ravishankar K. Iyer (2020). _Machine Learning for Load Balancing in the Linux Kernel._ APSys '20, 67–74. DOI: 10.1145/3409963.3410492.** [Author paper](https://ssbanerje.github.io/publications/apsys2020/Paper.pdf), [university record](https://experts.illinois.edu/en/publications/machine-learning-for-load-balancing-in-the-linux-kernel/). **Verified:** in-kernel MLP inference imitates CFS migration decisions. Reported agreement with labels is not proof of better scheduling; resource counters and RL were future directions.

2. **Hyeonmyeong Lee, Sungmin Jung, Heeseung Jo (2022). _STUN: Reinforcement-Learning-Based Optimization of Kernel Scheduler Parameters for Static Workload Performance._ Applied Sciences 12(14), 7072. DOI: 10.3390/app12147072.** [Publisher](https://www.mdpi.com/2076-3417/12/14/7072). **Verified from indexed publisher metadata/abstract:** RL tunes Linux scheduler policies and parameters. **Limit:** static-workload optimization does not establish stable online control under arbitrary workload transitions; full results were not reproduced.

3. **Georgios C. Chasparis, Michael Rossbory (2016). _Efficient Dynamic Pinning of Parallelized Applications by Distributed Reinforcement Learning._** [Author preprint](https://arxiv.org/abs/1606.08156). **Verified:** an external manager recursively changes thread CPU affinities using performance feedback and is evaluated on Linux. **Implication:** learned CPU placement has substantial prior art and is distinct from kernel dispatch policy.

4. **Xinbo Wang, Shian Jia, Ziyang Huang, Jing Cao, Mingli Song (2025). _Mixture-of-Schedulers: An Adaptive Scheduling Agent as a Learned Router for Expert Policies._** [arXiv preprint](https://arxiv.org/abs/2511.11628). **Verified:** offline workload-recognition training, runtime voting, and a machine-specific mapping choose sched_ext experts. **Distinction:** runtime selection is not incremental in-BPF model training. Reported benchmark success is source-specific and unreplicated here.

### Kernel learning and inference

5. **Ibrahim Umit Akgun, Ali Selman Aydin, Andrew Burford, Michael McNeill, Michael Arkhangelskiy, Aadil Shaikh, Lukas Velikov, Erez Zadok (2021). _KML: Using Machine Learning to Improve Storage Systems._** [arXiv](https://arxiv.org/abs/2111.11554), [author technical report FSL-21-02](https://www.fsl.cs.stonybrook.edu/docs/kml/kml-tr-fsl-21-02.pdf). **Verified:** kernel training/inference and offline userspace training/kernel inference are separate supported modes. The report covers readahead and NFS parameters. **Limit:** kernel integration and published overheads were not reproduced.

6. **Maximilian Bachl, Joachim Fabini, Tanja Zseby (2021 preprint, revised 2022). _A flow-based IDS using Machine Learning in eBPF._** [Paper](https://arxiv.org/abs/2102.09980). **Verified:** decision-tree classification uses flow history inside BPF. **Distinction:** separately trained inference in a network hook is not online kernel training or CPU scheduling.

7. **Prabhpreet Singh Sodhi, Georgios Liargkovas, Kostis Kaffes (2025). _Empowering machine-learning assisted kernel decisions with eBPFML._ 3rd Workshop on eBPF and Kernel Extensions. DOI: 10.1145/3748355.3748363.** [Author paper](https://www.cs.columbia.edu/~kkaffes/papers/empowering-ebpf25.pdf). **Verified:** proposes matrix primitives and CPU accelerator integration as a design vision, explicitly not a complete upstream-ready runtime. **Limit:** do not treat proposed helpers as available kernel capabilities. Its statement that the verifier forbids loops is stale; the loop-based IDS source above is a counterexample.

### Memory monitoring, reclaim, and allocation

8. **Johannes Weiner, Niket Agarwal, Dan Schatzberg, Leon Yang, Hao Wang, Blaise Sanouillet, Bikash Sharma, Tejun Heo, Mayank Jain, Chunqiang Tang, Dimitrios Skarlatos (2022). _TMO: Transparent Memory Offloading in Datacenters._ ASPLOS '22. DOI: 10.1145/3503222.3507731.** [Author paper](https://www.cs.cmu.edu/~dskarlat/publications/tmo_asplos22.pdf), [author-group bibliography](https://www.cs.cmu.edu/~caos/publications.html), [Meta engineering account](https://engineering.fb.com/2022/06/20/data-infrastructure/transparent-memory-offloading-more-memory-at-a-fraction-of-the-cost-and-power/). **Verified:** PSI, proactive reclaim, and userspace control support deployed offloading. **Distinction:** existing adaptive feedback control is not automatically ML, but is a strong baseline for it.

9. **Martin Maas, David G. Andersen, Michael Isard, Mohammad Mahdi Javanmard, Kathryn S. McKinley, Colin Raffel (2020). _Learning-based Memory Allocation for C++ Server Workloads._ ASPLOS '20.** [Google publication](https://research.google/pubs/learning-based-memory-allocation-for-c-server-workloads/), [author paper](https://www.cs.utexas.edu/~mckinley/papers/llama-asplos-2020.pdf). **Verified:** LLAMA learns lifetimes from previous executions and combines prediction with a huge-page heap layout. **Limit:** reported fragmentation improvements do not imply better Linux reclaim or online BPF feasibility.

10. **David A. Cohn, Satinder P. Singh (NIPS 1996). _Predicting Lifetimes in Dynamically Allocated Memory._ Advances in Neural Information Processing Systems 9.** [Proceedings metadata](https://papers.neurips.cc/paper_files/paper/1996/hash/a9078e8653368c9c291ae2f8b74012e7-Abstract.html), [proceedings paper](https://proceedings.neurips.cc/paper_files/paper/1996/file/a9078e8653368c9c291ae2f8b74012e7-Paper.pdf). **Verified:** decision trees predict allocation lifetimes. Learned allocation is decades-old prior art; conference-year metadata is 1996 even though some bibliographies use the proceedings publication year 1997.

11. **Konstantinos Mores, Stratos Psomadakis, Georgios Goumas (2024). _eBPF-mm: Userspace-guided memory management in Linux with eBPF._ ACM SRC @ MICRO '24 / arXiv preprint.** [Paper](https://arxiv.org/abs/2409.11220). **Verified:** introduces a new page-fault hook for profiles that select page sizes; DAMON identifies hot regions. **Limit:** this paper is not proof of an upstream, unpatched memory-policy hook.

12. **Tal Zussman, Ioannis Zarkadas, Jeremy Carin, Andrew Cheng, Hubertus Franke, Jonas Pfefferle, Asaf Cidon (2025). _cache_ext: Customizing the Page Cache with eBPF._ SOSP '25, 462–478. DOI: 10.1145/3731569.3764820.** [DOI](https://doi.org/10.1145/3731569.3764820), [artifact bibliography and installation](https://github.com/JayAndJef/cache_ext_lc). **Verified:** page-cache policy extension framework used by LearnedCache. **Verification limit:** metadata and the derivative artifact were inspected; this survey did not establish current upstream acceptance.

13. **Zejia Qi (2026). _LearnedCache: eBPF-Integrated Perceptron-Based Eviction Policies for the Linux Page Cache._** [arXiv metadata](https://arxiv.org/abs/2605.26168), [full paper, v2 July 30, 2026](https://arxiv.org/html/2605.26168v2), [models](https://github.com/JayAndJef/learnedcache). **Verified:** offline workload-specific training, quantized BPF inference, and protection/ranking policies. **Limit:** preprint results were not reproduced, and the project requires custom hooks rather than proving upstream-only feasibility.

## What the proposed project can claim now

**Verified project status:** research documents and a proposed design exist; no running policy or measured gain exists.

**Engineering estimate:** a narrow online linear model adjusting bounded slices under explicit weighted-vtime accounting may fit a small C/eBPF codebase. A userspace online reclaim-budget controller using existing cgroup interfaces is a backup. Both have directly relevant prior art.

**Untested hypotheses:** online updating could handle workload drift better than EWMA or frozen models; a bounded decision could retain acceptable fairness; training inside BPF could justify its cost. All require comparison with strong baselines and measurements.

Potential differentiation is a documented, compact, reproducible experiment with upstream-only installation, clear labels, arithmetic bounds, drift tests, overhead accounting, and recovery verification. The survey does not prove these features are unique. Never advertise this project as the first AI scheduler, first kernel learner, or faster than Linux without further evidence.

## Remaining verification work

- Pin exact kernel, scx, and comparator source revisions before implementation.
- Audit and reproduce the `scx_rusty_MLLB` learning path rather than relying solely on slides.
- Review the full STUN and cache_ext papers and their artifacts; indexed metadata/abstracts are narrower evidence.
- Extend the literature search for online burst prediction, timeslice adaptation, contextual bandits, and memory controllers; this survey cannot establish absence.
- Verify the target Ubuntu guest's actual kernel configuration and cgroup/PSI/DAMON features. No guest was inspected here.
- Recheck emerging 2026 projects before a release or any novelty statement. Current repository URLs and preprints can change.
