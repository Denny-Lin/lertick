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
    ("Kernel compile (kernel/)", False, "time"),
]
# Against EEVDF: median over the six sessions pooled by pool.py (held-out) and
# over the five kbuild-long sessions (kernel compile), in ROWS order.
EEVDF = {
    4: [-1.3, 1.2, 92.8, 28.7, 1.0, -7.3, 4.9, -136.3, -47.2, 3.5, 1.9],
    8: [-6.7, -3.0, 88.5, 10.9, 4.4, -11.6, 4.6, -6.0, -32.4, 0.5, 0.6],
}
# Against other schedulers: one session each, compared within that session
# (h2h.py for p2dq and flash; the medians below for bpfland and lavd).
FIRST = {  # Lertick in the first held-out sessions (heldout-cpu4/cpu8, kbuild-long)
    4: [0.616, 0.475, 1.606, 0.770, 1.718, 11380, 86, 161500, 5384, 162, 28.41],
    8: [0.562, 0.473, 2.347, 0.835, 1.271, 10256, 71, 35904, 217, 373, 25.68],
}
BPFLAND = {  # scx_bpfland 1.1.2 in the same sessions
    4: [0.735, 0.601, 1.854, 0.908, 2.381, 11280, 71, 385500, 5336, 155, 28.95],
    8: [0.691, 0.628, 2.858, 0.940, 2.057, 12208, 89, 41664, 498, 375, 26.44],
}
LAVD = {  # (Lertick, scx_lavd 1.1.4) in the lavd sessions
    4: list(zip([0.617, 0.429, 1.450, 0.727, 1.733, 9648, 59, 130432, 5544, 199, 28.24],
                [0.668, 0.516, 20.774, 0.884, 2.015, 9936, 95, 177920, 66, 197, 28.06])),
    8: list(zip([0.466, 0.392, 2.647, 0.801, 1.274, 11824, 68, 27104, 119, 422, 22.91],
                [0.504, 0.375, 20.091, 0.732, 1.385, 9808, 103, 32160, 151, 419, 23.33])),
}
P2DQ = {  # h2h.py, p2dq/flash sessions, fallback repetitions dropped
    4: [13.0, 10.9, 86.6, 18.0, 21.9, 4.2, -51.4, 24.4, -86.0, 11.5, 3.2],
    8: [-7.3, -0.5, 87.0, -7.5, 18.6, 3.8, 9.0, -0.2, -1.7, -5.2, 1.0],
}
FLASH = {
    4: [14.7, 26.3, 93.0, 39.3, 39.0, -3.8, 5.1, 27.5, -234.9, 8.4, 1.7],
    8: [21.6, 35.4, 88.9, 38.0, 47.9, -10.1, -4.4, -12.0, 25.7, -4.3, 0.0],
}
SAME = 5.0  # percent; matches the held-out verdict
CAP = 40.0  # bars longer than this are clipped and marked with an arrow
LABEL_END = 400  # right edge of the longest benchmark label
FONT = "-apple-system,BlinkMacSystemFont,SF Pro Display,Segoe UI,Arial,sans-serif"


def pct(lertick, other, hib):
    return ((lertick / other - 1) if hib else (1 - lertick / other)) * 100


def describe(p, hib, kind):
    if abs(p) <= SAME:
        return "same"
    if kind == "rps":
        return f"{abs(p):.0f}% {'more' if p > 0 else 'fewer'} requests/s"
    if not hib and p <= -100:
        return f"{1 - p / 100:.1f}× {'higher' if kind == 'lat' else 'slower'}"
    word = ("faster", "slower") if kind == "time" else ("lower", "higher")
    return f"{abs(p):.0f}% {word[0] if p > 0 else word[1]}"


def draw(cpus, path):
    W, top, rh, bh = 1120, 170, 50, 26
    zero_x, scale = 700, 6.0  # px per percent
    H = top + rh * len(ROWS) + 60
    pcts = EEVDF[cpus]
    wins = sum(p > SAME for p in pcts)
    losses = sum(p < -SAME for p in pcts)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="t d">',
           f'<title id="t">Lertick compared with Linux EEVDF on {cpus} CPUs</title>',
           f'<desc id="d">Percentage difference of Lertick (learned mode) against Linux EEVDF on {cpus} CPUs, median over several sessions: '
           f'better on {wins}, the same on {len(ROWS) - wins - losses}, worse on {losses} of {len(ROWS)} benchmarks not used for tuning. '
           'Green is better than EEVDF, red is worse, grey is within 5%.</desc>',
           f'<rect width="{W}" height="{H}" rx="24" fill="#f5f5f7"/>',
           f'<text x="48" y="64" font-family="{FONT}" font-size="30" font-weight="700" fill="#1d1d1f">{cpus} CPUs: better on {wins}, worse on {losses}</text>',
           f'<text x="48" y="100" font-family="{FONT}" font-size="18" fill="#6e6e73">Lertick vs Linux default scheduler (EEVDF). Right is better. Median over 5–6 sessions.</text>',
           f'<line x1="{zero_x}" y1="{top - 18}" x2="{zero_x}" y2="{top + rh * len(ROWS) - 10}" stroke="#86868b" stroke-width="2"/>',
           f'<text x="{zero_x}" y="{top - 28}" text-anchor="middle" font-family="{FONT}" font-size="14" fill="#6e6e73">same as Linux</text>',
           f'<text x="{zero_x - 110}" y="{top - 28}" text-anchor="end" font-family="{FONT}" font-size="14" fill="#ff3b30">← worse</text>',
           f'<text x="{zero_x + 110}" y="{top - 28}" font-family="{FONT}" font-size="14" fill="#34c759">better →</text>']
    for i, ((label, hib, kind), p) in enumerate(zip(ROWS, pcts)):
        y = top + i * rh
        txt = describe(p, hib, kind)
        same = abs(p) <= SAME
        color = "#aeaeb2" if same else "#34c759" if p > 0 else "#ff3b30"
        w = max(min(abs(p), CAP) * scale, 3)
        x = zero_x if p >= 0 else zero_x - w
        out.append(f'<text x="48" y="{y + 19}" font-family="{FONT}" font-size="18" fill="#1d1d1f">{label}</text>')
        out.append(f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="{bh}" rx="6" fill="{color}"/>')
        if abs(p) > CAP:
            ax, d = (x + w, 1) if p > 0 else (x, -1)
            out.append(f'<path d="M{ax - d * 11:.1f} {y + 4} L{ax + d * 2:.1f} {y + bh / 2} L{ax - d * 11:.1f} {y + bh - 4}" fill="none" stroke="#f5f5f7" stroke-width="2.5"/>')
        weight, fill = ("400", "#6e6e73") if same else ("700", color)
        if p >= 0:
            out.append(f'<text x="{x + w + 10:.1f}" y="{y + 19}" font-family="{FONT}" font-size="16" font-weight="{weight}" fill="{fill}">{txt}</text>')
        elif x - 10 - 9 * len(txt) < LABEL_END:  # no room left of the bar: write inside it
            out.append(f'<text x="{x + 16:.1f}" y="{y + 19}" font-family="{FONT}" font-size="16" font-weight="700" fill="#ffffff">{txt}</text>')
        else:
            out.append(f'<text x="{x - 10:.1f}" y="{y + 19}" text-anchor="end" font-family="{FONT}" font-size="16" font-weight="{weight}" fill="{fill}">{txt}</text>')
    out.append(f'<text x="48" y="{H - 28}" font-family="{FONT}" font-size="14" fill="#86868b">*In this VM, EEVDF and scx_lavd take about 20 µs per pipe round trip. perf bench sched, hackbench rt-tests 2.5, schbench 6300b8f; Linux 7.0 on an ARM64 VM. Bars past 40% are clipped.</text>')
    out.append('</svg>')
    open(path, "w").write("\n".join(out) + "\n")


def tally(pcts):
    """Count tests where Lertick is better, the same or worse than the other scheduler."""
    better = sum(p > SAME for p in pcts)
    worse = sum(p < -SAME for p in pcts)
    return better, len(pcts) - better - worse, worse


def scoreboard(cpus, path):
    hib = [r[1] for r in ROWS]
    rows = [
        ("Linux EEVDF", "default · median of 5–6 sessions", tally(EEVDF[cpus])),
        ("scx_bpfland", "sched_ext 1.1.2 · one session", tally([pct(l, o, h) for l, o, h in zip(FIRST[cpus], BPFLAND[cpus], hib)])),
        ("scx_lavd", "sched_ext 1.1.4 · one session", tally([pct(l, o, h) for (l, o), h in zip(LAVD[cpus], hib)])),
        ("scx_p2dq", "sched_ext 1.1.4 · one session", tally(P2DQ[cpus])),
        ("scx_flash", "sched_ext 1.1.4 · one session", tally(FLASH[cpus])),
    ]
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
    draw(cpus, f"docs/assets/lertick-vs-eevdf-cpu{cpus}.svg")
