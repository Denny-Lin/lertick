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
# Median over every session that ran both schedulers, from pool_all.py
# (held-out tests; kernel compile from kbuild-long), in ROWS order.
EEVDF = {  # 7 sessions (6 for kernel compile)
    4: [-0.2, 1.6, 92.8, 28.6, 1.2, -0.3, 6.5, -142.7, -47.1, 2.2, 1.1],
    8: [-5.6, -4.6, 88.9, 11.6, 4.1, -9.5, 5.3, -3.7, -31.1, 0.2, 0.3],
}
OTHERS = {  # 2 sessions each
    "scx_bpfland": {4: [18.8, 25.8, 28.3, 19.6, 25.8, 0.9, -12.2, 60.0, 1.3, 9.8, 0.2],
                    8: [23.9, 29.2, -5.9, 13.0, 33.8, 8.2, 11.0, 10.4, 55.3, 0.2, 0.7]},
    "scx_lavd": {4: [11.0, 19.8, 92.7, 19.3, 16.3, 3.1, 34.3, 25.8, -12886.7, -0.6, -1.6],
                 8: [5.6, -6.2, 89.4, -8.2, 6.4, -9.1, 33.9, 8.3, -14.4, 0.2, 1.7]},
    "scx_p2dq": {4: [19.7, 17.5, 86.4, 19.4, 20.5, 3.1, -54.4, 21.2, -82.2, 14.0, 0.8],
                 8: [4.7, -0.6, 89.4, -9.0, 15.4, 2.6, 5.3, -1.2, -8.9, -2.5, 0.3]},
    "scx_flash": {4: [17.7, 29.0, 92.7, 38.6, 37.3, -2.4, -10.5, 25.1, -228.0, 9.3, -0.7],
                  8: [27.9, 38.9, 90.5, 38.0, 47.2, -4.8, -4.0, -2.7, 36.8, 0.3, -0.8]},
}
SAME = 5.0  # percent; matches the held-out verdict
CAP = 40.0  # bars longer than this are clipped and marked with an arrow
LABEL_END = 400  # right edge of the longest benchmark label
FONT = "-apple-system,BlinkMacSystemFont,SF Pro Display,Segoe UI,Arial,sans-serif"


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
           f'<text x="48" y="100" font-family="{FONT}" font-size="18" fill="#6e6e73">Lertick vs Linux default scheduler (EEVDF). Right is better. Median over 7 sessions.</text>',
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
    out.append(f'<text x="48" y="{H - 28}" font-family="{FONT}" font-size="14" fill="#86868b">*EEVDF and scx_lavd take about 20 µs per pipe round trip in this VM. Linux 7.0, ARM64 VM. Bars past 40% are clipped.</text>')
    out.append('</svg>')
    open(path, "w").write("\n".join(out) + "\n")


def tally(pcts):
    """Count tests where Lertick is better, the same or worse than the other scheduler."""
    better = sum(p > SAME for p in pcts)
    worse = sum(p < -SAME for p in pcts)
    return better, len(pcts) - better - worse, worse


def scoreboard(cpus, path):
    rows = [("Linux EEVDF", "default · median of 7 sessions", tally(EEVDF[cpus]))]
    rows += [(name, f"sched_ext {'1.1.2' if name == 'scx_bpfland' else '1.1.4'} · median of 2 sessions", tally(p[cpus])) for name, p in OTHERS.items()]
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
