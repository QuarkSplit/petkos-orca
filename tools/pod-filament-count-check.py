#!/usr/bin/env python3
"""Podslicer: how many filaments does this G-code actually print with?

Most of this farm's machines hold ONE spool, and their firmware rejects a file that describes a
multi-filament print outright. So "a one-colour plate slices as a single-filament print" is not a
tidiness preference, it is the difference between a file that prints and a file that does not.

It has also been asserted and been wrong before. A plate can look single-colour on screen, be
counted as single-colour by every in-app check, and still emit a file full of tool changes -
because the count the ENGINE used came from somewhere else: the length of a colour vector, a slot
list rather than a spool list, or a paint flag that vetoed the cut-down. The G-code cannot be
wrong about it, because the G-code is what gets made.

    python tools/pod-filament-count-check.py <file.gcode|file.gcode.3mf|dir> [--expect 1]
    python tools/pod-filament-count-check.py out/ --expect 1 --json

Exit code is 0 only when every file checked matches --expect (or, with no --expect, when the
three DECLARED measures agree with each other and with the tool changes in the body - a
disagreement IS the finding).
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import zipfile

_SETTING = re.compile(r"^;\s*([A-Za-z0-9_\[\] ]+?)\s*=\s*(.*?)\s*$")
# A tool change in the body. T0 on a single-tool machine is harmless and common; T1 and above is
# a second filament by definition. M620/M621 are the AMS load/unload pair.
_TOOLCHANGE = re.compile(r"^\s*T(\d+)\b")
_AMS = re.compile(r"^\s*M62[01]\b")


def _members(path: pathlib.Path):
    """(name, line-iterator) for each G-code in the target. A .gcode.3mf is a ZIP."""
    if path.name.lower().endswith(".3mf"):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.lower().endswith(".gcode")]
            if not names:
                raise SystemExit(f"{path}: a 3MF with no .gcode inside it")
            for n in names:
                with z.open(n) as fh:
                    yield f"{path}!{n}", (line.decode("utf-8", "replace") for line in fh)
    else:
        with path.open("r", encoding="utf-8", errors="replace") as fh:
            yield str(path), fh


def measure(name: str, lines) -> dict:
    settings: dict[str, str] = {}
    tools: set[int] = set()
    ams = 0
    wipe_tower_lines = 0
    for line in lines:
        if line.startswith(";"):
            m = _SETTING.match(line)
            if m:
                # Last value wins: the trailing config block is the resolved one.
                settings[m.group(1)] = m.group(2)
            if "WIPE_TOWER" in line or "wipe tower" in line.lower():
                wipe_tower_lines += 1
            continue
        m = _TOOLCHANGE.match(line)
        if m:
            tools.add(int(m.group(1)))
            continue
        if _AMS.match(line):
            ams += 1

    def n_entries(key: str):
        v = settings.get(key)
        if v is None:
            return None
        v = v.strip()
        if not v:
            return None
        # Orca writes filament-indexed config as comma- or semicolon-separated lists.
        sep = ";" if ";" in v else ","
        return len([p for p in v.split(sep) if p.strip() != ""])

    # DECLARED is the width the print was composed at: how many filaments the file says exist,
    # which is the number a single-spool firmware objects to. CONSUMED is how many actually had
    # material put through them. They are not the same measure and must not be averaged into one:
    # Orca lists only filaments that were used in the totals, so consumed is a LOWER BOUND on
    # declared and a legitimate print can have consumed < declared. What is never legitimate is
    # the declared keys disagreeing with each other, or a tool change above the declared width.
    measures = {
        "filament_settings_id": n_entries("filament_settings_id"),
        "filament_type": n_entries("filament_type"),
        "filament_colour": n_entries("filament_colour"),
    }
    # NOT "total filament used [g]" - that is a grand total, one scalar, so counting its entries
    # always said 1 and would have hidden exactly the disagreement this tool exists to find. The
    # per-filament line is "filament used [g] = 1.71, 3.03, 0.00, 0.00", and a filament that had
    # nothing put through it was not consumed.
    consumed = None
    per_filament = settings.get("filament used [g]")
    if per_filament:
        used = []
        for part in per_filament.replace(";", ",").split(","):
            part = part.strip()
            if not part:
                continue
            try:
                used.append(float(part))
            except ValueError:
                used.append(0.0)
        if used:
            consumed = sum(1 for v in used if v > 0.0)
    distinct_tools = sorted(tools)
    return {
        "file": name,
        "measures": measures,
        "consumed": consumed,
        "tool_changes": distinct_tools,
        "tools_above_zero": [t for t in distinct_tools if t > 0],
        "ams_commands": ams,
        "wipe_tower_mentions": wipe_tower_lines,
        "printer": settings.get("printer_settings_id"),
        "printer_model": settings.get("printer_model"),
    }


def verdict(r: dict, expect) -> list:
    fails: list = []
    counts = {k: v for k, v in r["measures"].items() if v is not None}
    if not counts:
        fails.append("no filament-indexed key present at all; this file does not say what it used")
        return fails

    # The declared keys are independent readings of ONE fact - the width the print was composed
    # at - so them disagreeing is exactly the class of bug this exists to catch, and it is a
    # failure even when --expect is not given.
    if len(set(counts.values())) > 1:
        fails.append(f"the declared filament width disagrees between keys: {counts}")

    n = max(counts.values())
    consumed = r.get("consumed")
    if consumed is not None and consumed > n:
        fails.append(f"{consumed} filament(s) were consumed but only {n} declared")
    if r["tools_above_zero"] and n <= 1:
        # A tool change above T0 is a second filament whatever the config block claims.
        fails.append(
            f"config says {n} filament(s) but the body changes tool to "
            f"{r['tools_above_zero']} - a single-spool machine will reject this")

    if expect is not None:
        if n != expect:
            fails.append(f"expected {expect} filament(s), found {n} ({counts})")
        if expect == 1:
            if r["tools_above_zero"]:
                fails.append(f"expected a single-filament print, found tool changes {r['tools_above_zero']}")
            if r["ams_commands"]:
                fails.append(f"expected a single-filament print, found {r['ams_commands']} AMS load/unload commands")
            if r["wipe_tower_mentions"]:
                fails.append(f"expected a single-filament print, found {r['wipe_tower_mentions']} wipe-tower lines")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", type=pathlib.Path)
    ap.add_argument("--expect", type=int, default=None,
                    help="the number of filaments this print should use; 1 for a single-spool machine")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if args.target.is_dir():
        targets = sorted(p for p in args.target.rglob("*")
                         if p.suffix.lower() == ".gcode" or p.name.lower().endswith(".gcode.3mf"))
        if not targets:
            print(f"{args.target}: no G-code in here", file=sys.stderr)
            return 2
    else:
        targets = [args.target]

    reports, ok = [], True
    for t in targets:
        for name, lines in _members(t):
            r = measure(name, lines)
            r["failures"] = verdict(r, args.expect)
            ok = ok and not r["failures"]
            reports.append(r)

    if args.json:
        print(json.dumps(reports, indent=2))
    else:
        for r in reports:
            counts = {k: v for k, v in r["measures"].items() if v is not None}
            n = max(counts.values()) if counts else "?"
            extra = []
            if r["tools_above_zero"]:
                extra.append(f"tool changes {r['tools_above_zero']}")
            if r["ams_commands"]:
                extra.append(f"{r['ams_commands']} AMS cmds")
            if r["wipe_tower_mentions"]:
                extra.append("wipe tower")
            status = "FAIL" if r["failures"] else "ok  "
            if r.get("consumed") is not None and r["consumed"] != n:
                extra.insert(0, f"{r['consumed']} consumed")
            tail = f"  [{', '.join(extra)}]" if extra else ""
            print(f"{status}  {str(n) + ' declared':16} {r['printer'] or '?'}{tail}")
            print(f"        {r['file']}")
            for f in r["failures"]:
                print(f"        ! {f}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
