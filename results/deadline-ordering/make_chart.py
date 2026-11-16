# Draw docs/assets/lertick-vs-eevdf.svg from the held-out medians
# (heldout-cpu4.tar.gz and heldout-cpu8.tar.gz, Lertick online against EEVDF).
# Bars to the right are better than EEVDF. Run from the repository root.
rows = [  # label, (EEVDF, Lertick) on 4 CPUs, same on 8 CPUs, higher_is_better, kind
    ("perf messaging, processes", (0.619, 0.616), (0.532, 0.562), False, "time"),
    ("perf messaging, threads", (0.479, 0.475), (0.432, 0.473), False, "time"),
    ("perf pipe*", (20.38, 1.606), (21.96, 2.347), False, "time"),
    ("hackbench, threads", (1.150, 0.770), (1.178, 0.835), False, "time"),
    ("hackbench, 16 groups", (1.731, 1.718), (1.535, 1.271), False, "time"),
    ("schbench 1 thread, request", (15220, 11380), (13104, 10256), False, "lat"),
    ("schbench 1 thread, wakeup", (92, 86), (104, 71), False, "lat"),
    ("schbench 4 msg threads, request", (70270, 161500), (37056, 35904), False, "lat"),
    ("schbench 4 msg threads, wakeup", (3780, 5384), (171, 217), False, "lat"),
    ("schbench 4 msg threads, throughput", (165, 162), (354, 373), True, "rps"),
    ("Kernel compile (kernel/sched)", (2.434, 2.660), (2.815, 2.853), False, "time"),
]
SAME = 5.0  # percent
CAP = 40.0  # bars longer than this are clipped and marked with an arrow
W, top, rh, bh = 1160, 190, 64, 20
zero_x, scale = 790, 6.0  # px per percent
TAG_X = 420  # right edge of the 4/8 CPU tags
H = top + rh * len(rows) + 60
font = "-apple-system,BlinkMacSystemFont,SF Pro Display,Segoe UI,Arial,sans-serif"


def describe(linux, lt, hib, kind):
    better = (lt / linux - 1) if hib else (1 - lt / linux)
    pct = better * 100
    if abs(pct) <= SAME:
        return pct, "same"
    if kind == "rps":
        return pct, f"{abs(pct):.0f}% {'more' if pct > 0 else 'fewer'} requests/s"
    if not hib and lt / linux >= 2:
        return pct, f"{lt / linux:.1f}× higher" if kind == "lat" else f"{lt / linux:.1f}× slower"
    word = ("faster", "slower") if kind == "time" else ("lower", "higher")
    return pct, f"{abs(pct):.0f}% {word[0] if pct > 0 else word[1]}"


out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="t d">',
       '<title id="t">Lertick compared with Linux EEVDF</title>',
       '<desc id="d">Percentage difference of Lertick (learned mode) against Linux EEVDF on 11 kernel scheduler benchmarks '
       'not used for tuning, with 4 and 8 CPUs. Green is better than EEVDF, red is worse, grey is within 5%.</desc>',
       f'<rect width="{W}" height="{H}" rx="24" fill="#f5f5f7"/>',
       f'<text x="48" y="64" font-family="{font}" font-size="30" font-weight="700" fill="#1d1d1f">Lertick vs Linux default scheduler (EEVDF)</text>',
       f'<text x="48" y="100" font-family="{font}" font-size="18" fill="#6e6e73">Right is better than Linux. Upper bar 4 CPUs, lower bar 8 CPUs. Median of 5 runs.</text>',
       f'<line x1="{zero_x}" y1="{top - 18}" x2="{zero_x}" y2="{top + rh * len(rows) - 10}" stroke="#86868b" stroke-width="2"/>',
       f'<text x="{zero_x}" y="{top - 28}" text-anchor="middle" font-family="{font}" font-size="14" fill="#6e6e73">same as Linux</text>',
       f'<text x="{zero_x - 110}" y="{top - 28}" text-anchor="end" font-family="{font}" font-size="14" fill="#ff3b30">← worse</text>',
       f'<text x="{zero_x + 110}" y="{top - 28}" text-anchor="start" font-family="{font}" font-size="14" fill="#34c759">better →</text>']
for i, (label, c4, c8, hib, kind) in enumerate(rows):
    y = top + i * rh
    out.append(f'<text x="48" y="{y + 25}" font-family="{font}" font-size="18" fill="#1d1d1f">{label}</text>')
    for j, (tag, (linux, lt)) in enumerate((("4 CPUs", c4), ("8 CPUs", c8))):
        by = y + j * (bh + 6)
        pct, txt = describe(linux, lt, hib, kind)
        same = abs(pct) <= SAME
        color = "#aeaeb2" if same else "#34c759" if pct > 0 else "#ff3b30"
        clipped = abs(pct) > CAP
        w = max(min(abs(pct), CAP) * scale, 3)
        x = zero_x if pct >= 0 else zero_x - w
        out.append(f'<text x="{TAG_X}" y="{by + 15}" text-anchor="end" font-family="{font}" font-size="13" fill="#86868b">{tag}</text>')
        out.append(f'<rect x="{x:.1f}" y="{by}" width="{w:.1f}" height="{bh}" rx="5" fill="{color}"/>')
        if clipped:
            ax = x + w if pct > 0 else x
            d = 1 if pct > 0 else -1
            out.append(f'<path d="M{ax - d * 10:.1f} {by + 3} L{ax + d * 2:.1f} {by + bh / 2} L{ax - d * 10:.1f} {by + bh - 3}" fill="none" stroke="#f5f5f7" stroke-width="2.5"/>')
        weight = "400" if same else "700"
        fill = "#6e6e73" if same else color
        if pct >= 0:
            out.append(f'<text x="{x + w + 10:.1f}" y="{by + 15}" font-family="{font}" font-size="15" font-weight="{weight}" fill="{fill}">{txt}</text>')
        else:
            out.append(f'<text x="{x - 10:.1f}" y="{by + 15}" text-anchor="end" font-family="{font}" font-size="15" font-weight="{weight}" fill="{fill}">{txt}</text>')
out.append(f'<text x="48" y="{H - 28}" font-family="{font}" font-size="14" fill="#86868b">*Likely a VM artefact. perf bench sched, hackbench rt-tests 2.5, schbench 6300b8f; Linux 7.0 on an ARM64 VM. Bars past 40% are clipped.</text>')
out.append('</svg>')
open("docs/assets/lertick-vs-eevdf.svg", "w").write("\n".join(out) + "\n")
