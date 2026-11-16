# Draw docs/assets/lertick-vs-eevdf.svg from the release confirmation medians.
# Positive = Lertick better than EEVDF. Run from the repository root.
rows = [  # label, EEVDF, Lertick, higher_is_better
    ("hackbench · pipes", 0.8255, 0.6990, False),
    ("hackbench · sockets", 1.6250, 1.4990, False),
    ("schbench busy · wakeup latency", 120.5, 47, False),
    ("schbench busy · request latency", 18240, 17152, False),
    ("schbench light · request latency", 11760, 11712, False),
    ("schbench overloaded · wakeup latency", 8944, 8128, False),
    ("schbench overloaded · request latency", 379392, 371712, False),
    ("schbench overloaded · throughput", 124.5, 119, True),
]
W, top, rh, zero_x, scale = 1120, 160, 52, 560, 7.0  # px per percent
H = top + rh * len(rows) + 70
font = "-apple-system,BlinkMacSystemFont,SF Pro Display,Segoe UI,Arial,sans-serif"
out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}" role="img" aria-labelledby="t d">',
       '<title id="t">Lertick compared with Linux EEVDF</title>',
       '<desc id="d">Percentage difference of Lertick (learned mode) against Linux EEVDF on hackbench and schbench, '
       'medians of 10 runs on one ARM64 VM. Green bars are better than EEVDF, red is worse, grey is within 2%.</desc>',
       f'<rect width="{W}" height="{H}" rx="24" fill="#f5f5f7"/>',
       f'<text x="48" y="64" font-family="{font}" font-size="30" font-weight="700" fill="#1d1d1f">Lertick vs Linux default scheduler (EEVDF)</text>',
       f'<text x="48" y="100" font-family="{font}" font-size="18" fill="#6e6e73">Bars to the right are better than Linux. Median of 10 runs, ARM64 VM, 4 CPUs.</text>',
       f'<line x1="{zero_x}" y1="{top - 18}" x2="{zero_x}" y2="{top + rh * len(rows) - 10}" stroke="#86868b" stroke-width="2"/>',
       f'<text x="{zero_x}" y="{top - 26}" text-anchor="middle" font-family="{font}" font-size="14" fill="#6e6e73">same as Linux</text>']
for i, (label, linux, lt, hib) in enumerate(rows):
    better = (lt / linux - 1) if hib else (1 - lt / linux)
    pct = better * 100
    y = top + i * rh
    color = "#34c759" if pct > 2 else "#ff3b30" if pct < -2 else "#aeaeb2"
    w = max(abs(pct) * scale, 3)
    x = zero_x if pct >= 0 else zero_x - w
    if abs(pct) <= 2:
        txt = "same"
    elif hib:
        txt = f"{abs(pct):.0f}% {'more' if pct > 0 else 'fewer'} requests/s"
    elif label.startswith("hackbench"):
        txt = f"{abs(pct):.0f}% {'faster' if pct > 0 else 'slower'}"
    else:
        txt = f"{abs(pct):.0f}% {'lower' if pct > 0 else 'higher'}"
    out.append(f'<text x="48" y="{y + 23}" font-family="{font}" font-size="18" fill="#1d1d1f">{label}</text>')
    out.append(f'<rect x="{x:.1f}" y="{y + 4}" width="{w:.1f}" height="26" rx="6" fill="{color}"/>')
    tx = (x + w + 10) if pct >= 0 else (zero_x + 10)
    anchor = "start"
    weight = "700" if abs(pct) > 2 else "400"
    out.append(f'<text x="{tx:.1f}" y="{y + 23}" text-anchor="{anchor}" font-family="{font}" font-size="17" font-weight="{weight}" fill="{color if abs(pct) > 2 else "#6e6e73"}">{txt}</text>')
out.append(f'<text x="48" y="{H - 30}" font-family="{font}" font-size="14" fill="#86868b">schbench 6300b8f, hackbench rt-tests 2.5, Linux 7.0.0-38-generic on ARM64 QEMU/HVF. Details: results/deadline-ordering/README.md</text>')
out.append('</svg>')
open("docs/assets/lertick-vs-eevdf.svg", "w").write("\n".join(out) + "\n")
