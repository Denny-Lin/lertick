# Writing the README and results pages

[← Documentation](README.md)

How to write and update the product README, the results pages and the
charts.

## Style

- Plain words and short sentences. Say what Lertick does and how it
  compares, not how it was built.
- Say each thing once. A count shown in a chart is not repeated in the text
  beside it, and a chart does not repeat its own numbers.
- No invented labels such as "Ledger", "Now" or "Verdict", no sentences
  inside table cells and no bold sentences. Tables are for numbers.
- Every number comes from raw data in `results/` and the script that
  produced it. Give the measured range ("6 to 8%"), not a stronger word.
- Name the limits: the machine, what is untested, and any comparison that
  used an older build.

## Product README

Keep this order: title and one-line description, Performance, Get started,
then the rest. The Performance section has four parts:

1. One paragraph of three or four sentences: where Lertick was tested, what
   it does better than EEVDF, where it is worse and by how much, and what is
   untested. No win and loss counts; the charts carry them.
2. The scoreboard charts for 4 and 8 CPUs.
3. The per-benchmark charts, inside a `<details>` block.
4. One `<sub>` line: the benchmarks, that they were also used to choose
   between variants, the settings used, and links to how it was measured
   and to all runs.

Each chart's alt text states the chart's counts in words.

The copy of the README for sched-ext/scx (`upstream/scx_lertick/README.md`)
follows that project's template instead: Overview, Typical Use Case and
Production Ready?.

## Charts

- A script in the results folder draws them (now
  `results/deadline-ordering/make_chart.py`). Do not edit the SVGs by hand.
- Scoreboard: one row per scheduler, a better / same / worse bar with the
  count inside each part, one legend, and no text right of the bars. Rows
  measured with another build sit under a note that says so.
- Per benchmark: one bar per test, right is better. Results within 5% get no
  bar, only "within 5%".
- Labels under names stay short (a version, "kernel default"), so no text
  runs into a bar.
- Before pushing, render every chart to PNG and look at it, for example
  `sips -s format png chart.svg --out chart.png` on macOS.

## Results pages

A results folder has a short `README.md` and a full `RECORD.md`.

`README.md` has these sections, wrapped at 80 columns except links:

1. A first paragraph: what the folder covers and which code it matches.
2. `## Status`: the numbers against EEVDF, the open losses and what is known
   about them, and what is untested.
3. `## Changes tried`: newest first, one item each: date, kept or dropped,
   the change, one sentence of result, links.
4. `## How results are measured`: machine, benchmarks, run order and
   repetitions, the 5% rule, safety checks.
5. `## Files`: the record, experiment branches, scripts and raw data.

`RECORD.md` keeps every hypothesis, result and dropped variant, oldest
first. New entries go at the end. Old entries are not edited; a correction
is a new entry.

## When numbers change

1. Update the chart script's data from the ratio script and redraw.
2. Change the Performance paragraph only if a strength, a loss or a limit
   changed.
3. Update the alt texts, `## Status` and `## Changes tried`, and add the
   full entry to `RECORD.md`.
4. Run `python3 tests/check_docs.py`.
