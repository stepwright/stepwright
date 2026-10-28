#!/usr/bin/env python3
"""Draw docs/assets/benchmarks.svg from published benchmark results.

Usage: scripts/bench_chart.py <macos-results-dir> <linux-results-dir> -o docs/assets/benchmarks.svg

Each panel compares a stepwright operation with a familiar alternative, using
the same median ns/item values as the tables bench_report.py writes. Panels have
independent scales because the cases differ by orders of magnitude. The SVG has
no external fonts or scripts and adapts to light and dark page themes.
"""
import argparse
import os
import sys
from html import escape

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bench_report import load_run, ns_per_item  # noqa: E402  (same numbers as the tables)

# (title, rows); a row is (label, benchmark run name, is_stepwright).
PANELS = [
    ("Run the next step", [
        ("stepwright then()", "BM_then_chain/1000", True),
        ("function-pointer call", "BM_baseline_fnptr_chain/1000", False),
        ("std::function call", "BM_baseline_std_function_chain/1000", False),
    ]),
    ("Await a child that completes at once", [
        ("stepwright after()", "BM_after_sync_child/1000", True),
        ("coroutine, visible child*", "BM_coro_after_sync_child/1000", False),
        ("coroutine, opaque child", "BM_coro_after_opaque_sync_child/1000", False),
    ]),
    ("Lock and unlock, uncontended", [
        ("stepwright async_mutex", "BM_mutex_uncontended", True),
        ("std::mutex", "BM_baseline_std_mutex_uncontended", False),
    ]),
    ("Send and receive on one thread", [
        ("stepwright channel", "BM_channel_send_pull_same_thread/1000", True),
        ("std::mutex + std::deque", "BM_baseline_mutex_deque_same_thread/1000", False),
    ]),
    ("Repost work from eight workers", [
        ("stepwright thread_pool", "BM_pool_post_from_worker/8/real_time", True),
        ("mutex + deque + std::function pool", "BM_baseline_pool_post_from_worker/8/real_time", False),
    ]),
]

WIDTH = 960
LABEL_W = 270
BAR_X = 300
BAR_MAX = 560
BAR_H = 11
ROW_GAP = 14
PANEL_HEAD = 34
PANEL_GAP = 22
TOP = 92


def medians(path):
    run = load_run(path)
    values = {}
    for entries in run["groups"].values():
        for name, entry in entries.items():
            if "median" in entry:
                values[name] = ns_per_item(entry["median"])
    return run["context"], values


def fmt(ns):
    if ns >= 100:
        return f"{ns:,.0f} ns"
    if ns >= 10:
        return f"{ns:.1f} ns"
    return f"{ns:.2f} ns"


def render(mac_ctx, mac, linux_ctx, linux, source):
    out = []
    y = TOP
    for title, rows in PANELS:
        for _, name, _ in rows:
            for label, values in (("macOS", mac), ("Linux", linux)):
                if name not in values:
                    sys.exit(f"bench_chart: {name} missing from the {label} results")
        scale = max(max(mac[n], linux[n]) for _, n, _ in rows)
        out.append(f'<text class="panel" x="24" y="{y + 18}">{escape(title)}</text>')
        out.append(f'<text class="note" x="{WIDTH - 24}" y="{y + 18}" text-anchor="end">'
                   f'own scale · lower is better</text>')
        y += PANEL_HEAD
        for label, name, ours in rows:
            out.append(f'<text class="{"label ours" if ours else "label"}" x="{LABEL_W}" '
                       f'y="{y + BAR_H + 4}" text-anchor="end">{escape(label)}</text>')
            for i, (values, cls) in enumerate(((mac, "mac"), (linux, "linux"))):
                value = values[name]
                w = max(2.0, BAR_MAX * value / scale)
                by = y + i * (BAR_H + 3)
                kind = "ours" if ours else "base"
                out.append(f'<rect class="bar {kind} {cls}" x="{BAR_X}" y="{by}" width="{w:.1f}" '
                           f'height="{BAR_H}" rx="3"/>')
                out.append(f'<text class="value" x="{BAR_X + w + 8:.1f}" y="{by + BAR_H - 1}">'
                           f'{fmt(value)}</text>')
            y += 2 * BAR_H + 3 + ROW_GAP
        y += PANEL_GAP
    height = y + 58
    foot = (f"Median ns per item over ten repetitions · macOS: {mac_ctx.get('label', '')} · "
            f"Linux: {linux_ctx.get('label', '')}")
    data = f"Commit {mac_ctx.get('commit', '?')} · raw results in {source}"
    note = ("* With AppleClang the visible child's coroutine frame is elided; "
            "GCC allocates it. See docs/benchmarks.md.")
    head = f"""<svg xmlns="http://www.w3.org/2000/svg" width="{WIDTH}" height="{height}" viewBox="0 0 {WIDTH} {height}" role="img" aria-label="stepwright benchmark comparison">
<style>
  text {{ font-family: Inter, -apple-system, "Segoe UI", Helvetica, Arial, sans-serif; fill: #1F2937; }}
  .title {{ font-size: 20px; font-weight: 700; }}
  .sub, .note, .foot {{ font-size: 12.5px; fill: #64748B; }}
  .panel {{ font-size: 15px; font-weight: 600; }}
  .label {{ font-size: 13.5px; fill: #475569; }}
  .label.ours {{ fill: #0F766E; font-weight: 600; }}
  .value {{ font-size: 12px; fill: #475569; font-variant-numeric: tabular-nums; }}
  .bar.ours.mac {{ fill: url(#ours); }}
  .bar.ours.linux {{ fill: url(#ours); opacity: 0.55; }}
  .bar.base.mac {{ fill: #94A3B8; }}
  .bar.base.linux {{ fill: #94A3B8; opacity: 0.5; }}
  .rule {{ stroke: #E2E8F0; }}
  @media (prefers-color-scheme: dark) {{
    text {{ fill: #E2E8F0; }}
    .sub, .note, .foot {{ fill: #94A3B8; }}
    .label {{ fill: #CBD5E1; }}
    .label.ours {{ fill: #5EEAD4; }}
    .value {{ fill: #CBD5E1; }}
    .bar.base.mac {{ fill: #64748B; }}
    .bar.base.linux {{ fill: #64748B; }}
    .rule {{ stroke: #1E293B; }}
  }}
</style>
<defs><linearGradient id="ours" x1="0" x2="1"><stop offset="0" stop-color="#14B8A6"/><stop offset="1" stop-color="#818CF8"/></linearGradient></defs>
<text class="title" x="24" y="34">stepwright next to familiar alternatives</text>
<text class="sub" x="24" y="56">Each pair of bars: macOS first, Linux second (lighter). Coloured: stepwright; grey: the alternative.</text>
<line class="rule" x1="24" x2="{WIDTH - 24}" y1="74" y2="74"/>
"""
    tail = f"""<line class="rule" x1="24" x2="{WIDTH - 24}" y1="{height - 70}" y2="{height - 70}"/>
<text class="foot" x="24" y="{height - 50}">{escape(foot)}</text>
<text class="foot" x="24" y="{height - 32}">{escape(data)}</text>
<text class="foot" x="24" y="{height - 14}">{escape(note)}</text>
</svg>
"""
    return head + "\n".join(out) + "\n" + tail


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("macos")
    parser.add_argument("linux")
    parser.add_argument("-o", "--output", required=True)
    args = parser.parse_args()
    mac_ctx, mac = medians(args.macos)
    linux_ctx, linux = medians(args.linux)
    source = os.path.dirname(os.path.normpath(args.macos))
    svg = render(mac_ctx, mac, linux_ctx, linux, source)
    with open(args.output, "w", encoding="utf-8") as handle:
        handle.write(svg)


if __name__ == "__main__":
    main()
