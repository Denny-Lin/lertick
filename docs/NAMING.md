# Naming research and decision

Checked on **2026-10-09**, before publication. The owner requested an independent
choice among 10–15 short, credible names after reconsidering SchedMind.
**Lertick**, pronounced **LER-tick**, is the selected winner: learning + tick.
Repository: `Denny-Lin/lertick`. Executable and sched_ext registration:
`scx_lertick`. C/build identifiers use `lertick` / `LERTICK`.

## Method

Candidates were checked with exact quoted and targeted search-engine queries,
GitHub repository-name searches, and primary registry RDAP records using the
[IANA bootstrap](https://data.iana.org/rdap/dns.json). GitHub name searches match
substrings, so exact repository basenames were reviewed separately. Scores are
editorial judgments about brand strength and memorability, not measurements.
The [machine-readable evidence](../results/naming/search-evidence.json) records
queries, repository matches, sources, registry endpoints, and HTTP responses.

Names already associated with established software, companies, or products
were rejected. Unrelated surname/OCR hits, single code identifiers, and tiny
substring-only projects are disclosed rather than treated as proof of a
significant brand conflict. Search absence establishes no identified conflict
in these checks, not global exclusivity.

## Twelve finalists

Repository names below are proposed slugs under `Denny-Lin/`. Brand and memory
scores are out of ten; all names are 4–10 characters.

| Rank | Name and pronunciation | Intended meaning | GitHub repository | Executable | Existing naming conflicts | Brand / memory |
| --- | --- | --- | --- | --- | --- | --- |
| **1** | **Lertick — LER-tick** | Learning + scheduler tick | `lertick` | `scx_lertick` | No matching repository or established product/company identified; incidental OCR/name hits | **9 / 9** |
| **2** | **Rundapt — RUN-dapt** | Runtime that adapts | `rundapt` | `scx_rundapt` | No matching repository or established brand identified; `.com` is registered | **9 / 8** |
| **3** | **Aftick — AF-tick** | Past execution informs the next tick | `aftick` | `scx_aftick` | Only unrelated [360-AFTickets](https://github.com/guidobucett/360-AFTickets) substring, zero stars; `.com` registered | **8 / 9** |
| **4** | **Burstune — BURSS-toon** | Tune scheduling from CPU bursts | `burstune` | `scx_burstune` | No matching repository or established brand identified; incidental misspelled names | **8 / 8** |
| **5** | **Kertick — KER-tick** | Kernel scheduling tick | `kertick` | `scx_kertick` | No matching repository or established brand identified; surname/personal handles exist | **8 / 8** |
| 6 | Lertune — LER-toon | Learn and tune | `lertune` | `scx_lertune` | No matching repository or established brand identified; incidental OCR | 8 / 8 |
| 7 | Causched — CAW-shed | Causal prediction + scheduling | `causched` | `scx_causched` | No exact project; old unrelated [CAUScheduler](https://github.com/ZeroPage/CAUScheduler) substring, zero stars | 8 / 7 |
| 8 | Runhest — RUN-hest | Runtime history and estimation | `runhest` | `scx_runhest` | No matching repository or established brand identified | 7 / 7 |
| 9 | Cadturn — CAD-turn | Cadence + a task's CPU turn | `cadturn` | `scx_cadturn` | No matching repository or established brand identified; CAD can suggest design software | 7 / 8 |
| 10 | Cavrun — CAV-run | Causal, adaptive virtual runtime | `cavrun` | `scx_cavrun` | No matching repository or established brand; [CAVRUN scientific-code routine](https://www.kns.org/files/pre_paper/16/344%EB%B0%95%EC%84%A0%ED%9D%AC.pdf) exists | 7 / 7 |
| 11 | Evotick — EE-vo-tick | Evolving scheduling decisions | `evotick` | `scx_evotick` | No exact repo basename; [java-evotick](https://github.com/Sleepy4k/java-evotick), zero stars, and EvoTicket substrings; `.com` registered | 7 / 8 |
| 12 | Aevtick — EVE-tick | Coinage connecting time and tick | `aevtick` | `scx_aevtick` | No matching repository or established brand identified | 7 / 6 |

## Top five and winner

1. **Lertick** best balances meaning, pronunciation, brevity, distinctiveness,
   and clean search/domain results. It signals learning and time without an
   AI-marketing label or an unsupported speed claim. `scx_lertick` is natural
   in the sched_ext ecosystem and stays within kernel naming limits.
2. **Rundapt** expresses adaptation directly and could scale into a recognizable
   systems brand. Its word blend is slightly less natural to pronounce, and its
   registered `.com` is a practical disadvantage.
3. **Aftick** is the shortest strong option and feels appropriate for a kernel
   tool. The history association is less immediately clear than Lertick's
   learning association; its `.com` is also registered.
4. **Burstune** has the clearest CPU-burst connection. Its burst-specific meaning
   is narrower if CPU scheduling decisions expand beyond slice prediction.
5. **Kertick** is easy to pronounce and technically restrained. It emphasizes
   the kernel and time but communicates learning less clearly.

**Decision: use Lertick.** It remains appropriate if widely adopted because it
is a compact systems name, not a particular model, algorithm, or performance
promise. The subtitle is **A Self-Learning CPU Scheduler for Linux**.

## Domains

Primary `.org` and `.dev` checks returned **404 / no registration record** for
all twelve names. Some later repeat `.dev` requests were rate-limited (429);
the evidence retains the earlier successful 404 observations. Primary `.com`
results were:

| Name | `.com` | `.org` | `.dev` |
| --- | --- | --- | --- |
| Lertick | No record | No record | No record |
| Rundapt | Registered | No record | No record |
| Aftick | Registered | No record | No record |
| Burstune | No record | No record | No record |
| Kertick | No record | No record | No record |
| Lertune | No record | No record | No record |
| Causched | No record | No record | No record |
| Runhest | No record | No record | No record |
| Cadturn | No record | No record | No record |
| Cavrun | No record | No record | No record |
| Evotick | Registered | No record | No record |
| Aevtick | No record | No record | No record |

Winner checks: [lertick.com](https://rdap.verisign.com/com/v1/domain/lertick.com),
[lertick.org](https://rdap.publicinterestregistry.org/rdap/domain/lertick.org),
[lertick.dev](https://pubapi.registry.google/rdap/domain/lertick.dev).
No RDAP record does **not** confirm registrar availability, reservation status,
or price. Registered domains do not by themselves establish a product conflict.
No domain was purchased or registered.

## Rejected earlier candidates

- **SchedMind:** exact [SaaS/agent documentation repository](https://github.com/tanzeela-mahmood/schedmind)
  and [timetable application](https://github.com/Harsha754-ml/SchedMind-AI);
  the owner preferred a more distinctive identity.
- **Runora:** existing [developer terminal product](https://runora.dev/) and
  [AI workflow company](https://www.runora.ai/).
- **Runetic:** existing [running coach](https://www.runeticapp.com/),
  [retailer](https://www.runetic.com.co/about), and
  [genetic-algorithm project](https://github.com/ChadyG/runetic).
- **Runtiq:** existing [cloud infrastructure platform](https://runtiq.com/).
- **Learq:** existing [architecture business](https://www.learq.com/) and exact repositories.
- **Aftune:** existing [AI research framework](https://github.com/hengvt/AFTUNE).
- **Runvex:** existing [Houdini function/product name](https://www.sidefx.com/docs/houdini/nodes/apex/RunVex.html)
  and [software automation company](https://runvex.tech/en).
- **Runlith:** existing [AI engineering company/product](https://runlith.app/terms).
- **Histune:** existing [music game](https://www.histune.com/).
- **Burstem:** existing [npm toolbox](https://www.skypack.dev/view/burstem).
- **Vartick:** existing [learning Pomodoro timer](https://github.com/shanyueel/vartick),
  small but adjacent in concept.
- **Cauriq:** no exact brand identified, but pronunciation is close to
  [NXP QorIQ](https://www.nxp.com/products/processors-and-microcontrollers/power-architecture/qoriq-communication-processors:QORIQ_HOME),
  an established computing identity.

## Historical identifiers

The original repository was `linux-resource-learning`; Phase 1 was called
ScxSprout and used `scx_sprout` / `sprout_*`. Original commit `1d2c39e` and every
`results/phase1/` file remain unchanged. Old names in that evidence, the
[background archive](planning/README.md), and explicitly historical passages
are provenance, not current CLI instructions.

Current source, scripts, examples, Actions, repository metadata, and release
READMEs use Lertick. [Rebranding regression](REBRAND_VALIDATION.md) checks the
renamed implementation. No Phase 2 code, release, or tag is included.
