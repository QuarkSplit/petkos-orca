#!/usr/bin/env python3
"""Petko's Orca: turn perf CSVs into the one table the question actually needs.

A summary per run answers "how long did this take". The question here is different: does
cost track the plate count? So this puts 1 / 6 / 36 side by side, and for each probe prints
BOTH the per-call time and the calls per frame - because a probe can look cheap per call and
still own the frame by being called once per plate.

    python tools/petkos-perf-table.py perf-runs/baseline-1.csv perf-runs/baseline-6.csv perf-runs/baseline-36.csv
    python tools/petkos-perf-table.py --against perf-runs/after-36.csv perf-runs/baseline-36.csv
"""
import argparse
import collections
import csv
import os
import sys


def load(path):
    """-> (spans, interactions, marks); spans[name] = list of ms."""
    spans = collections.defaultdict(list)
    inter = collections.defaultdict(lambda: {"paint": [], "settle": []})
    marks = []
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        for row in csv.DictReader(fh):
            kind, name = row["kind"], row["name"]
            try:
                dur, aux = float(row["dur_ms"]), float(row["aux"])
            except (TypeError, ValueError):
                continue
            if kind == "span":
                spans[name].append(dur)
            elif kind == "interaction":
                if dur >= 0:
                    inter[name]["paint"].append(dur)
                inter[name]["settle"].append(aux)
            elif kind == "mark":
                marks.append((name, aux, float(row["start_ms"])))
    return spans, inter, marks


def pct(values, p):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(p * len(ordered)))]


def label(path):
    return os.path.splitext(os.path.basename(path))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--against", help="baseline CSV to show a delta against")
    args = ap.parse_args()

    runs = [(label(p), load(p)) for p in args.csv]
    base = load(args.against) if args.against else None

    # Frames per run come from the whole-frame probe, so "per frame" is honest rather than
    # a guess from the sample count of whichever probe happens to be listed first.
    frames = {}
    for name, (spans, _, _) in runs:
        frames[name] = max(1, len(spans.get("CanvasRender", [])))

    names = sorted({k for _, (s, _, _) in runs for k in s})
    width = max(len(n) for n in names) + 2

    print("\nSPANS  -  p50 ms  (calls/frame)   frames: " +
          ", ".join(f"{n}={frames[n]}" for n, _ in runs))
    print("-" * (width + 22 * len(runs)))
    print("probe".ljust(width) + "".join(n.rjust(22) for n, _ in runs))
    for probe in names:
        line = probe.ljust(width)
        for name, (spans, _, _) in runs:
            v = spans.get(probe, [])
            if not v:
                line += "-".rjust(22)
                continue
            line += f"{pct(v, .5):8.3f} ({len(v)/frames[name]:6.2f})".rjust(22)
        print(line)

    print("\nTOTAL ms spent per probe (sum of all calls)")
    print("-" * (width + 22 * len(runs)))
    for probe in names:
        line = probe.ljust(width)
        for name, (spans, _, _) in runs:
            v = spans.get(probe, [])
            line += (f"{sum(v):12.1f}" if v else "-").rjust(22)
        print(line)

    print("\nINTERACTIONS  -  first-paint p50 / settle p50 / settle max, ms")
    print("-" * (width + 30 * len(runs)))
    ikeys = sorted({k for _, (_, i, _) in runs for k in i})
    for k in ikeys:
        line = k.ljust(width)
        for name, (_, inter, _) in runs:
            d = inter.get(k)
            if not d or not d["settle"]:
                line += "-".rjust(30)
                continue
            line += (f"{pct(d['paint'], .5):7.1f} /{pct(d['settle'], .5):7.1f} /"
                     f"{max(d['settle']):7.1f}").rjust(30)
        print(line)

    if base is not None:
        print("\nDELTA vs " + label(args.against) + "  (negative is faster)")
        print("-" * (width + 22 * len(runs)))
        bspans = base[0]
        for probe in names:
            b = bspans.get(probe, [])
            if not b:
                continue
            line = probe.ljust(width)
            for name, (spans, _, _) in runs:
                v = spans.get(probe, [])
                if not v:
                    line += "-".rjust(22)
                    continue
                d = sum(v) - sum(b)
                line += f"{d:+12.1f} ms".rjust(22)
            print(line)

    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
