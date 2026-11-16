# Draw the README charts from the held-out medians (heldout-cpu4/cpu8.tar.gz)
# and the longer kernel compile (kbuild-long.tar.gz). Run from the repository root.
#   docs/assets/lertick-scoreboard-cpu{4,8}.svg  Lertick against each scheduler
#   docs/assets/lertick-vs-eevdf-cpu{4,8}.svg    Lertick against EEVDF per test
ROWS = [  # label, (EEVDF, Lertick) on 4 CPUs, same on 8 CPUs, higher_is_better, kind
    ("perf messaging, processes", (0.619, 0.616), (0.532, 0.562), False, "time"),
    ("perf messaging, threads", (0.479, 0.475), (0.432, 0.473), False, "time"),
    ("perf pipe*", (20.38, 1.606), (21.96, 2.347), False, "time"),
    ("hackbench, threads", (1.150, 0.770), (1.178, 0.835), False, "time"),
    ("hackbench, 16 groups", (1.731, 1.718), (1.535, 1.271), False, "time"),
    ("schbench 1 thread, request latency", (15220, 11380), (13104, 10256), False, "lat"),
    ("schbench 1 thread, wakeup latency", (92, 86), (104, 71), False, "lat"),
    ("schbench 4 msg threads, request latency", (70270, 161500), (37056, 35904), False, "lat"),
    ("schbench 4 msg threads, wakeup latency", (3780, 5384), (171, 217), False, "lat"),
    ("schbench 4 msg threads, throughput", (165, 162), (354, 373), True, "rps"),
    ("Kernel compile (kernel/)", (30.12, 28.41), (25.69, 25.68), False, "time"),
]
# scx_bpfland 1.1.2 medians from the same sessions, in ROWS order.
BPFLAND = {
    4: [0.735, 0.601, 1.854, 0.908, 2.381, 11280, 71, 385500, 5336, 155, 28.95],
    8: [0.691, 0.628, 2.858, 0.940, 2.057, 12208, 89, 41664, 498, 375, 26.44],
}
# scx_lavd 1.1.4 (LLVM 20 build) ran in its own sessions with its own Lertick arm
# (heldout cpu4-lavd/cpu8-lavd, kbuild-long cpu4-lavd/cpu8-lavd): (Lertick, lavd) in ROWS order.
LAVD = {
    4: list(zip([0.617, 0.429, 1.450, 0.727, 1.733, 9648, 59, 130432, 5544, 199, 28.24],
                [0.668, 0.516, 20.774, 0.884, 2.015, 9936, 95, 177920, 66, 197, 28.06])),
    8: list(zip([0.466, 0.392, 2.647, 0.801, 1.274, 11824, 68, 27104, 119, 422, 22.91],
                [0.504, 0.375, 20.091, 0.732, 1.385, 9808, 103, 32160, 151, 419, 23.33])),
}
SAME = 5.0  # percent; matches the held-out verdict
CAP = 40.0  # bars longer than this are clipped and marked with an arrow
LABEL_END = 400  # right edge of the longest benchmark label
FONT = "-apple-system,BlinkMacSystemFont,SF Pro Display,Segoe UI,Arial,sans-serif"


def describe(linux, lt, hib, kind):
    pct = ((lt / linux - 1) if hib else (1 - lt / linux)) * 100
    if abs(pct) <= SAME:
        return pct, "same"
    if kind == "rps":
        return pct, f"{abs(pct):.0f}% {'more' if pct > 0 else 'fewer'} requests/s"
    if not hib and lt / linux >= 2:
        return pct, f"{lt / linux:.1f}× {'higher' if kind == 'lat' else 'slower'}"
    word = ("faster", "slower") if kind == "time" else ("lower", "higher")
    return pct, f"{abs(pct):.0f}% {word[0] if pct > 0 else word[1]}"


def draw(column, cpus, path):
    W, top, rh, bh = 1120, 170, 50, 26
    zero_x, scale = 700, 6.0  # px per percent
    H = top + rh * len(ROWS) + 60
    results = [describe(*r[column], r[3], r[4]) for r in ROWS]
    wins = sum(p > SAME for p, _ in results)
    losses = sum(p < -SAME for p, _ in results)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="t d">',
           f'<title id="t">Lertick compared with Linux EEVDF on {cpus} CPUs</title>',
           f'<desc id="d">Percentage difference of Lertick (learned mode) against Linux EEVDF on {cpus} CPUs: '
           f'better on {wins}, the same on {len(ROWS) - wins - losses}, worse on {losses} of {len(ROWS)} benchmarks not used for tuning. '
           'Green is better than EEVDF, red is worse, grey is within 5%.</desc>',
           f'<rect width="{W}" height="{H}" rx="24" fill="#f5f5f7"/>',
           f'<text x="48" y="64" font-family="{FONT}" font-size="30" font-weight="700" fill="#1d1d1f">{cpus} CPUs: better on {wins}, worse on {losses}</text>',
           f'<text x="48" y="100" font-family="{FONT}" font-size="18" fill="#6e6e73">Lertick vs Linux default scheduler (EEVDF). Right is better. Median of 5 runs.</text>',
           f'<line x1="{zero_x}" y1="{top - 18}" x2="{zero_x}" y2="{top + rh * len(ROWS) - 10}" stroke="#86868b" stroke-width="2"/>',
           f'<text x="{zero_x}" y="{top - 28}" text-anchor="middle" font-family="{FONT}" font-size="14" fill="#6e6e73">same as Linux</text>',
           f'<text x="{zero_x - 110}" y="{top - 28}" text-anchor="end" font-family="{FONT}" font-size="14" fill="#ff3b30">← worse</text>',
           f'<text x="{zero_x + 110}" y="{top - 28}" font-family="{FONT}" font-size="14" fill="#34c759">better →</text>']
    for i, (row, (pct, txt)) in enumerate(zip(ROWS, results)):
        y = top + i * rh
        same = abs(pct) <= SAME
        color = "#aeaeb2" if same else "#34c759" if pct > 0 else "#ff3b30"
        w = max(min(abs(pct), CAP) * scale, 3)
        x = zero_x if pct >= 0 else zero_x - w
        out.append(f'<text x="48" y="{y + 19}" font-family="{FONT}" font-size="18" fill="#1d1d1f">{row[0]}</text>')
        out.append(f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="{bh}" rx="6" fill="{color}"/>')
        if abs(pct) > CAP:
            ax, d = (x + w, 1) if pct > 0 else (x, -1)
            out.append(f'<path d="M{ax - d * 11:.1f} {y + 4} L{ax + d * 2:.1f} {y + bh / 2} L{ax - d * 11:.1f} {y + bh - 4}" fill="none" stroke="#f5f5f7" stroke-width="2.5"/>')
        weight, fill = ("400", "#6e6e73") if same else ("700", color)
        if pct >= 0:
            out.append(f'<text x="{x + w + 10:.1f}" y="{y + 19}" font-family="{FONT}" font-size="16" font-weight="{weight}" fill="{fill}">{txt}</text>')
        elif x - 10 - 9 * len(txt) < LABEL_END:  # no room left of the bar: write inside it
            out.append(f'<text x="{x + 16:.1f}" y="{y + 19}" font-family="{FONT}" font-size="16" font-weight="700" fill="#ffffff">{txt}</text>')
        else:
            out.append(f'<text x="{x - 10:.1f}" y="{y + 19}" text-anchor="end" font-family="{FONT}" font-size="16" font-weight="{weight}" fill="{fill}">{txt}</text>')
    out.append(f'<text x="48" y="{H - 28}" font-family="{FONT}" font-size="14" fill="#86868b">*In this VM, EEVDF and scx_lavd take about 20 µs per pipe round trip. perf bench sched, hackbench rt-tests 2.5, schbench 6300b8f; Linux 7.0 on an ARM64 VM. Bars past 40% are clipped.</text>')
    out.append('</svg>')
    open(path, "w").write("\n".join(out) + "\n")


def tally(pairs):
    """Count tests where Lertick is better, the same or worse than the other scheduler."""
    pcts = [describe(other, lt, hib, kind)[0] for other, lt, hib, kind in pairs]
    better = sum(p > SAME for p in pcts)
    worse = sum(p < -SAME for p in pcts)
    return better, len(pcts) - better - worse, worse


def scoreboard(cpus, path):
    column = 1 if cpus == 4 else 2
    rows = [("Linux EEVDF", "default scheduler", tally([(r[column][0], r[column][1], r[3], r[4]) for r in ROWS]))]
    rows.append(("scx_bpfland", "sched_ext 1.1.2", tally([(b, r[column][1], r[3], r[4]) for r, b in zip(ROWS, BPFLAND[cpus])])))
    rows.append(("scx_lavd", "sched_ext 1.1.4", tally([(o, lt, r[3], r[4]) for r, (lt, o) in zip(ROWS, LAVD[cpus])])))
    W, top, rh, bh = 1120, 150, 84, 40
    x0, bar_w = 330, 520
    H = top + rh * len(rows) + 40
    n = len(ROWS)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="t d">',
           f'<title id="t">Lertick against other schedulers on {cpus} CPUs</title>',
           f'<desc id="d">On {cpus} CPUs, out of {n} benchmarks: ' + '; '.join(
               f'against {name}, Lertick is better on {b}, the same on {s_}, worse on {w}' for name, _, (b, s_, w) in rows) + '.</desc>',
           f'<rect width="{W}" height="{H}" rx="24" fill="#f5f5f7"/>',
           f'<text x="48" y="64" font-family="{FONT}" font-size="30" font-weight="700" fill="#1d1d1f">{cpus} CPUs: Lertick against other schedulers</text>',
           f'<text x="48" y="100" font-family="{FONT}" font-size="18" fill="#6e6e73">Out of {n} kernel scheduler benchmarks, how many Lertick is better, the same or worse on.</text>']
    for i, (name, sub, (b, s_, w)) in enumerate(rows):
        y = top + i * rh
        out.append(f'<text x="48" y="{y + 20}" font-family="{FONT}" font-size="20" font-weight="600" fill="#1d1d1f">vs {name}</text>')
        out.append(f'<text x="48" y="{y + 44}" font-family="{FONT}" font-size="15" fill="#86868b">{sub}</text>')
        x = x0
        for count, color in ((b, "#34c759"), (s_, "#aeaeb2"), (w, "#ff3b30")):
            if count:
                seg = bar_w * count / n
                out.append(f'<rect x="{x + 1:.1f}" y="{y}" width="{seg - 2:.1f}" height="{bh}" rx="6" fill="{color}"/>')
                out.append(f'<text x="{x + seg / 2:.1f}" y="{y + 27}" text-anchor="middle" font-family="{FONT}" font-size="18" font-weight="700" fill="#ffffff">{count}</text>')
                x += seg
        out.append(f'<text x="{x0 + bar_w + 24}" y="{y + 27}" font-family="{FONT}" font-size="18" fill="#1d1d1f">'
                   f'<tspan fill="#248a3d" font-weight="700">{b} better</tspan> · {s_} same · <tspan fill="#d70015" font-weight="700">{w} worse</tspan></text>')
    out.append('</svg>')
    open(path, "w").write("\n".join(out) + "\n")


for cpus in (4, 8):
    scoreboard(cpus, f"docs/assets/lertick-scoreboard-cpu{cpus}.svg")
draw(1, 4, "docs/assets/lertick-vs-eevdf-cpu4.svg")
draw(2, 8, "docs/assets/lertick-vs-eevdf-cpu8.svg")
