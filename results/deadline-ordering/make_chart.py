# Draw the README charts. Run from the repository root.
#   docs/assets/lertick-scoreboard-cpu{4,8}.svg  Lertick against each scheduler
#   docs/assets/lertick-vs-eevdf-cpu{4,8}.svg    Lertick against EEVDF per test
# Percentages are positive when Lertick is better.
ROWS = [  # label, higher_is_better, kind
    ("perf messaging, processes", False, "time"),
    ("perf messaging, threads", False, "time"),
    ("perf pipe*", False, "time"),
    ("hackbench, threads", False, "time"),
    ("hackbench, 16 groups", False, "time"),
    ("schbench 1 thread, request latency", False, "lat"),
    ("schbench 1 thread, wakeup latency", False, "lat"),
    ("schbench 4 msg threads, request latency", False, "lat"),
    ("schbench 4 msg threads, wakeup latency", False, "lat"),
    ("schbench 4 msg threads, throughput", True, "rps"),
    ("Kernel compile", False, "time"),
]
# EEVDF: the deadline-ordering build, from dl_ratio.py (median over the
# sessions x4 and final5, 8 runs each). The sched_ext schedulers: the
# previous build (v2) run with them in the brawl session (brawl_ratio.py),
# since the deadline build has not run in one session with them.
# In ROWS order; positive is better for Lertick.
EEVDF = {  # median over sessions of 8 runs
    4: [-8.0, 9.5, 93.7, 29.0, 11.4, 0.6, 19.2, 6.7, 19.4, 12.6, 0.7],
    8: [-5.8, 1.7, 85.6, 12.1, 3.3, 0.0, 21.3, -2.1, 25.4, -0.2, 1.7],
}
OTHERS = {  # same session (brawl), 8 runs
    "scx_bpfland": {4: [13.2, 29.5, 69.0, 20.0, 21.2, -1.9, 11.8, 85.6, 38.2, 32.3, -3.6],
                    8: [18.7, 36.8, 8.5, 13.0, 29.9, -0.5, 11.1, 7.9, 73.8, -0.5, 1.1]},
    "scx_lavd": {4: [8.2, 16.5, 93.7, 20.5, 21.5, -1.9, 35.2, 72.1, -19522.2, 13.6, -1.8],
                 8: [-6.6, 2.2, 87.6, -3.7, 6.5, 3.4, 51.0, 2.4, 21.6, -0.7, -0.7]},
    "scx_p2dq": {4: [11.8, 14.2, 88.6, 21.1, 23.8, -3.3, -36.4, 69.7, -20.8, 32.3, 1.1],
                 8: [5.5, 9.2, 87.4, -4.8, 15.9, 0.7, 17.9, -1.1, 4.3, -0.9, 0.2]},
    "scx_flash": {4: [11.8, 32.4, 93.9, 35.7, 34.6, -0.7, 16.0, 71.9, -114.6, 29.4, -3.1],
                  8: [21.7, 44.0, 87.7, 36.9, 46.6, 2.6, 26.7, 5.3, 66.8, 0.4, -0.3]},
}
VERSIONS = {"scx_bpfland": "1.1.2", "scx_lavd": "1.1.4", "scx_p2dq": "1.1.4", "scx_flash": "1.1.4"}
SAME = 5.0  # percent; matches the held-out verdict
CAP = 40.0  # bars longer than this are cut short and marked with an arrow
LABEL_END = 400  # right edge of the longest benchmark label
FONT = "-apple-system,BlinkMacSystemFont,SF Pro Display,Segoe UI,Arial,sans-serif"
GREEN, GREY, RED = "#34c759", "#aeaeb2", "#ff3b30"
INK, MUTED, FAINT, BG = "#1d1d1f", "#6e6e73", "#86868b", "#f5f5f7"


def text(x, y, s, size, fill=INK, weight="400", anchor="start"):
    return (f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" font-family="{FONT}" '
            f'font-size="{size}" font-weight="{weight}" fill="{fill}">{s}</text>')


def describe(p, hib, kind):
    if kind == "rps":
        return f"{abs(p):.0f}% {'more' if p > 0 else 'fewer'} requests/s"
    if not hib and p <= -100:
        return f"{1 - p / 100:.1f}× {'higher' if kind == 'lat' else 'slower'}"
    word = ("faster", "slower") if kind == "time" else ("lower", "higher")
    return f"{abs(p):.0f}% {word[0] if p > 0 else word[1]}"


def tally(pcts):
    """Count tests where Lertick is better, the same or worse than the other scheduler."""
    better = sum(p > SAME for p in pcts)
    worse = sum(p < -SAME for p in pcts)
    return better, len(pcts) - better - worse, worse


def draw(cpus, path):
    W, top, rh, bh = 1120, 170, 50, 26
    zero_x, scale = 700, 6.0  # px per percent
    H = top + rh * len(ROWS) + 76
    pcts = EEVDF[cpus]
    b, s, w = tally(pcts)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="t d">',
           f'<title id="t">Lertick compared with Linux EEVDF on {cpus} CPUs, per benchmark</title>',
           f'<desc id="d">Difference between Lertick and Linux EEVDF on {cpus} CPUs, median of 2 sessions of 8 runs: '
           f'Lertick is better on {b}, within 5% on {s} and worse on {w} of {len(ROWS)} benchmarks. '
           'Green is better than EEVDF, red is worse.</desc>',
           f'<rect width="{W}" height="{H}" rx="24" fill="{BG}"/>',
           text(48, 64, f"{cpus} CPUs: each benchmark against Linux EEVDF", 30, weight="700"),
           text(48, 100, "Right of the line is better for Lertick. Median of 2 sessions of 8 runs.", 18, MUTED),
           f'<line x1="{zero_x}" y1="{top - 18}" x2="{zero_x}" y2="{top + rh * len(ROWS) - 10}" stroke="{FAINT}" stroke-width="2"/>',
           text(zero_x, top - 28, "same as EEVDF", 14, MUTED, anchor="middle"),
           text(zero_x - 110, top - 28, "← worse", 14, RED, anchor="end"),
           text(zero_x + 110, top - 28, "better →", 14, GREEN)]
    for i, ((label, hib, kind), p) in enumerate(zip(ROWS, pcts)):
        y = top + i * rh
        out.append(text(48, y + 19, label, 18))
        if abs(p) <= SAME:  # no bar: the difference is noise at this threshold
            out.append(text(zero_x + 12, y + 19, "within 5%", 16, FAINT))
            continue
        txt = describe(p, hib, kind)
        color = GREEN if p > 0 else RED
        bw = min(abs(p), CAP) * scale
        x = zero_x if p > 0 else zero_x - bw
        out.append(f'<rect x="{x:.1f}" y="{y}" width="{bw:.1f}" height="{bh}" rx="6" fill="{color}"/>')
        if abs(p) > CAP:
            ax, d = (x + bw, 1) if p > 0 else (x, -1)
            out.append(f'<path d="M{ax - d * 11:.1f} {y + 4} L{ax + d * 2:.1f} {y + bh / 2} L{ax - d * 11:.1f} {y + bh - 4}" fill="none" stroke="{BG}" stroke-width="2.5"/>')
        if p > 0:
            out.append(text(x + bw + 10, y + 19, txt, 16, color, "700"))
        elif x - 10 - 9 * len(txt) < LABEL_END:  # no room left of the bar: write inside it
            out.append(text(x + 16, y + 19, txt, 16, "#ffffff", "700"))
        else:
            out.append(text(x - 10, y + 19, txt, 16, color, "700", "end"))
    out.append(text(48, H - 48, "*In this VM a pipe round trip takes about 20 µs under EEVDF and 1.2 to 4.3 µs under Lertick.", 14, FAINT))
    out.append(text(48, H - 26, "Bars past 40% are cut short. Linux 7.0, ARM64 VM.", 14, FAINT))
    out.append('</svg>')
    open(path, "w").write("\n".join(out) + "\n")


def scoreboard(cpus, path):
    n = len(ROWS)
    eevdf = ("Linux EEVDF", "kernel default", tally(EEVDF[cpus]))
    others = [(name, f"version {VERSIONS[name]}", tally(p[cpus])) for name, p in OTHERS.items()]
    W, bh, rh = 1120, 40, 72
    x0, x1 = 240, W - 48
    unit = (x1 - x0) / n
    first = 176  # top of the EEVDF row
    group = first + bh + 24  # divider above the sched_ext rows
    rest = group + 56  # top of the first sched_ext row
    H = rest + rh * (len(others) - 1) + bh + 80

    def row(y, name, sub, counts):
        parts = [text(48, y + 18, name, 20, weight="600"), text(48, y + 40, sub, 15, FAINT)]
        x = x0
        for count, color in zip(counts, (GREEN, GREY, RED)):
            if count:
                seg = unit * count
                parts.append(f'<rect x="{x + 1:.1f}" y="{y}" width="{seg - 2:.1f}" height="{bh}" rx="6" fill="{color}"/>')
                parts.append(text(x + seg / 2, y + 27, str(count), 18, "#ffffff", "700", "middle"))
                x += seg
        return parts

    def says(name, counts):
        return f"{name}: better on {counts[0]}, the same on {counts[1]}, worse on {counts[2]}"

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="t d">',
           f'<title id="t">Lertick against other schedulers on {cpus} CPUs</title>',
           f'<desc id="d">On {cpus} CPUs, out of {n} benchmarks, Lertick against {says(eevdf[0], eevdf[2])}. '
           'The previous Lertick version against sched_ext schedulers: '
           + '; '.join(says(name, c) for name, _, c in others) + '.</desc>',
           f'<rect width="{W}" height="{H}" rx="24" fill="{BG}"/>',
           text(48, 64, f"{cpus} CPUs: Lertick against other schedulers", 30, weight="700"),
           text(48, 100, f"How many of {n} benchmarks Lertick is better on, the same on, or worse on.", 18, MUTED)]
    lx = 48
    for label, color in (("Better", GREEN), ("Same (within 5%)", GREY), ("Worse", RED)):
        out.append(f'<rect x="{lx}" y="125" width="14" height="14" rx="3" fill="{color}"/>')
        out.append(text(lx + 22, 137, label, 15, MUTED))
        lx += 22 + 8.2 * len(label) + 28
    out += row(first, *eevdf)
    out.append(f'<line x1="48" y1="{group}" x2="{W - 48}" y2="{group}" stroke="#d2d2d7" stroke-width="1"/>')
    out.append(text(48, group + 30, "sched_ext schedulers, compared with the previous Lertick version", 15, MUTED, "600"))
    for i, (name, sub, counts) in enumerate(others):
        out += row(rest + i * rh, name, sub, counts)
    out.append(text(48, H - 30, "Linux 7.0, ARM64 VM. EEVDF: median of 2 sessions of 8 runs. sched_ext schedulers: one session of 8 runs.", 14, FAINT))
    out.append('</svg>')
    open(path, "w").write("\n".join(out) + "\n")


import sys
for cpus in [int(c) for c in sys.argv[1:]] or (4, 8):
    scoreboard(cpus, f"docs/assets/lertick-scoreboard-cpu{cpus}.svg")
    draw(cpus, f"docs/assets/lertick-vs-eevdf-cpu{cpus}.svg")
