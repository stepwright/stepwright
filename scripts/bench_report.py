#!/usr/bin/env python3
"""Turn Google Benchmark JSON output into Markdown tables.

usage: scripts/bench_report.py RUN [RUN ...] [-o FILE | --update FILE]

Each RUN is a directory of bench_*.json files written by `make bench
BENCH_OUT=<dir>` (or `make docker-bench BENCH_OUT=<dir>`), or a single JSON
file. scripts/ci.sh also writes <dir>/context.json (label, CPU, OS, compiler,
flags, commit), which is merged into the context Google Benchmark records.
Every run becomes one column pair (ns/item and coefficient of variation),
labelled with its label; benchmarks are grouped by the executable (benchmark
file) that produced them.

Values: the median over the repetitions of the time per processed item
(1e9 / items_per_second), which Google Benchmark computes from real time for
benchmarks registered with UseRealTime() and from CPU time otherwise. CV is the
coefficient of variation of that rate over the repetitions.

Without -o/--update the Markdown goes to stdout. --update FILE replaces the text
between the markers
    <!-- bench-report:begin -->
    <!-- bench-report:end -->
in FILE, leaving the hand-written text around them untouched.
"""

import argparse
import json
import os
import sys
from collections import OrderedDict

BEGIN = "<!-- bench-report:begin -->"
END = "<!-- bench-report:end -->"


def load_run(path):
    files = []
    extra_context = {}
    if os.path.isdir(path):
        files = sorted(
            os.path.join(path, f) for f in os.listdir(path)
            if f.endswith(".json") and f != "context.json")
        side = os.path.join(path, "context.json")
        if os.path.exists(side):
            with open(side, encoding="utf-8") as handle:
                extra_context = json.load(handle)
    else:
        files = [path]
    if not files:
        sys.exit(f"bench_report: no JSON files in {path}")
    run = {"context": None, "groups": OrderedDict()}
    for f in files:
        with open(f, encoding="utf-8") as handle:
            text = handle.read()
        if not text.strip():
            continue  # an executable whose benchmarks were all filtered out
        data = json.loads(text)
        context = data.get("context", {})
        if run["context"] is None:
            run["context"] = dict(context, **extra_context)
        group = os.path.basename(context.get("executable", f))
        entries = run["groups"].setdefault(group, OrderedDict())
        for b in data.get("benchmarks", []):
            name = b.get("run_name", b["name"])
            entry = entries.setdefault(name, {})
            kind = b.get("aggregate_name")
            if kind is None:
                # Single run without aggregates: treat it as the median.
                if b.get("run_type") == "iteration" and "median" not in entry:
                    kind = "median"
                else:
                    continue
            entry[kind] = b
    if run["context"] is None:
        sys.exit(f"bench_report: no results in {path}")
    return run


def ns_per_item(b):
    rate = b.get("items_per_second")
    if rate:
        return 1e9 / rate
    return b.get("real_time" if b["run_name"].endswith("real_time") else "cpu_time")


def cv_percent(entry):
    b = entry.get("cv")
    if not b:
        return None
    value = b.get("items_per_second")
    if value is None:
        value = b.get("real_time" if b["run_name"].endswith("real_time") else "cpu_time")
    return 100.0 * value


def extra_counters(b):
    skip = {"name", "family_index", "per_family_instance_index", "run_name", "run_type",
            "repetitions", "repetition_index", "threads", "aggregate_name",
            "aggregate_unit", "iterations", "real_time", "cpu_time", "time_unit",
            "items_per_second", "bytes_per_second", "label", "error_occurred",
            "error_message"}
    return {k: v for k, v in b.items() if k not in skip and isinstance(v, (int, float))}


def fmt_ns(value):
    if value is None:
        return "–"
    if value >= 100000:
        return f"{value:,.0f}"
    if value >= 100:
        return f"{value:.0f}"
    if value >= 10:
        return f"{value:.1f}"
    return f"{value:.2f}"


def fmt_cv(value):
    return "–" if value is None else f"{value:.1f} %"


def display_name(name):
    return name[:-len("/real_time")] + " (real)" if name.endswith("/real_time") else name


def render(runs):
    labels = []
    for i, run in enumerate(runs):
        label = run["context"].get("label") or f"run {i + 1}"
        labels.append(label)
    out = []
    out.append("### Platforms")
    out.append("")
    out.append("| | " + " | ".join(labels) + " |")
    out.append("|---|" + "---|" * len(runs))
    rows = [("CPU", "cpu"), ("OS", "os"), ("Compiler", "compiler"), ("Flags", "flags"),
            ("Commit", "commit"), ("Date", "date"), ("CPUs", "num_cpus"),
            ("Load average", "load_avg"), ("Arguments", "bench_args")]
    for title, key in rows:
        values = []
        for run in runs:
            value = run["context"].get(key, "–")
            if key == "load_avg" and isinstance(value, list):
                value = " / ".join(f"{v:.1f}" for v in value)
            if key in ("flags", "bench_args"):
                value = f"`{value}`"
            values.append(str(value))
        out.append(f"| {title} | " + " | ".join(values) + " |")
    out.append("")
    groups = OrderedDict()
    for run in runs:
        for group in run["groups"]:
            groups.setdefault(group, None)
    for group in groups:
        out.append(f"### `{group}`")
        out.append("")
        header = "| Benchmark | " + " | ".join(f"{l} ns/item | CV" for l in labels) + " | Notes |"
        out.append(header)
        out.append("|---|" + "---:|---:|" * len(runs) + "---|")
        names = OrderedDict()
        for run in runs:
            for name in run["groups"].get(group, {}):
                names.setdefault(name, None)
        for name in names:
            cells = []
            notes = OrderedDict()
            for run in runs:
                entry = run["groups"].get(group, {}).get(name, {})
                median = entry.get("median")
                cells.append(fmt_ns(ns_per_item(median)) if median else "–")
                cells.append(fmt_cv(cv_percent(entry)))
                if median:
                    for key, value in extra_counters(median).items():
                        notes.setdefault(key, []).append(f"{value:.3g}")
            note = "; ".join(f"{k} = {' / '.join(v)}" for k, v in notes.items())
            out.append(f"| `{display_name(name)}` | " + " | ".join(cells) + f" | {note} |")
        out.append("")
    return "\n".join(out).rstrip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("runs", nargs="+", help="result directory or JSON file per platform")
    target = parser.add_mutually_exclusive_group()
    target.add_argument("-o", "--output", help="write the Markdown to this file")
    target.add_argument("--update", help="replace the marked region of this file")
    args = parser.parse_args()
    text = render([load_run(path) for path in args.runs])
    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(text)
    elif args.update:
        with open(args.update, encoding="utf-8") as handle:
            document = handle.read()
        start = document.find(BEGIN)
        stop = document.find(END)
        if start < 0 or stop < start:
            sys.exit(f"bench_report: {args.update} has no {BEGIN} ... {END} region")
        document = (document[:start + len(BEGIN)] + "\n" + text + document[stop:])
        with open(args.update, "w", encoding="utf-8") as handle:
            handle.write(document)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
