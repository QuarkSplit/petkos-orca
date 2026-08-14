#!/usr/bin/env python3
"""Petko's Orca: read what a slice ACTUALLY used, out of the G-code it emitted.

This exists because of one incident. On 2026-08-14 a change populated every plate's context
at import and did it a moment too early, so each plate was written with the placeholder
presets that happened to be selected before the file was opened. Every in-app check passed:
the plates were complete, the per-plate override reached its plate, the project round-tripped
through a save. The app then quoted 21h03m for a plate that takes 6h45m, and reported 0.00 g,
because `Default Filament` caps filament_max_volumetric_speed at 2 instead of 12. For a print
farm those two numbers are the ones that reach a price and a machine booking.

The G-code is the only artefact that cannot be wrong about what was used, because it IS what
gets made. So this reads the emitted file and reports the identity a slice actually ran with.

    python tools/petkos-gcode-check.py <file.gcode|file.gcode.3mf|dir> [--expect-printer NAME]
                                       [--expect-filament NAME] [--min-flow 4] [--json]

Exit code is 0 only when every file checked passes every expectation given. With no
expectations it prints what it found and exits 0, which is the useful "what did this run
actually do" mode.

Two traps it knows about:
  - A .gcode.3mf is a ZIP; the G-code is inside it, usually Metadata/plate_N.gcode.
  - The header block is emitted as `; key = value` and the same keys also appear in the
    trailing config block. The trailing block is the one that reflects the FULL resolved
    config, so it wins where they disagree, and a disagreement is itself worth reporting.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import zipfile

# The keys that answer "what was this actually sliced with". Anything whose wrongness would
# reach a price, a machine booking or a physical result.
IDENTITY_KEYS = [
    "printer_settings_id",
    "print_settings_id",
    "filament_settings_id",
    "printer_model",
    "printer_variant",
    "nozzle_diameter",
    "filament_type",
    "filament_max_volumetric_speed",
    "wall_loops",
    "sparse_infill_density",
    "sparse_infill_pattern",
    "layer_height",
    "curr_bed_type",
]

# What the slice claims it produced. A missing or zero value here is the tell that something
# upstream refused to compute, which is exactly what the 0.00 g incident looked like.
RESULT_KEYS = [
    "total filament used [g]",
    "total filament used [mm]",
    "estimated printing time (normal mode)",
    "total layer number",
]

_SETTING = re.compile(r"^;\s*([A-Za-z0-9_\[\] ]+?)\s*=\s*(.*?)\s*$")


def read_text(path: pathlib.Path) -> list[tuple[str, str]]:
    """Every `; key = value` line in the file, in order. A .gcode.3mf is unzipped first."""
    if path.suffix.lower() == ".3mf" or path.name.lower().endswith(".gcode.3mf"):
        out: list[tuple[str, str]] = []
        with zipfile.ZipFile(path) as z:
            members = [n for n in z.namelist() if n.lower().endswith(".gcode")]
            if not members:
                raise SystemExit(f"{path}: a 3MF with no .gcode inside it")
            for member in members:
                with z.open(member) as fh:
                    out.extend(_scan(line.decode("utf-8", "replace") for line in fh))
        return out
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        return _scan(fh)


def _scan(lines) -> list[tuple[str, str]]:
    found = []
    for line in lines:
        if not line.startswith(";"):
            continue
        m = _SETTING.match(line)
        if m:
            found.append((m.group(1), m.group(2)))
    return found


def collapse(pairs: list[tuple[str, str]]) -> tuple[dict, dict]:
    """Last value wins, because the trailing config block is the resolved one. Also returns
    the keys whose header value and trailing value DISAGREE, which is a finding in itself:
    it means something rewrote the config between the header and the end of the run."""
    first: dict[str, str] = {}
    last: dict[str, str] = {}
    for key, value in pairs:
        if key not in first:
            first[key] = value
        last[key] = value
    disagreed = {k: (first[k], last[k]) for k in last if first.get(k) != last[k]}
    return last, disagreed


def first_number(value: str) -> float | None:
    m = re.search(r"-?\d+(?:\.\d+)?", value or "")
    return float(m.group(0)) if m else None


def check(path: pathlib.Path, args) -> tuple[bool, dict]:
    values, disagreed = collapse(read_text(path))
    report = {
        "file": str(path),
        "identity": {k: values.get(k) for k in IDENTITY_KEYS if k in values},
        "result": {k: values.get(k) for k in RESULT_KEYS if k in values},
        "disagreed": disagreed,
        "failures": [],
    }

    def fail(msg: str) -> None:
        report["failures"].append(msg)

    for key in ("printer_settings_id", "print_settings_id", "filament_settings_id"):
        if key not in values:
            fail(f"{key} is absent, so the file does not say what it was sliced with")

    # The incident's own signature, checked by name. "Default" presets are placeholders that
    # exist so the app can start, not settings anybody chose.
    for key in ("printer_settings_id", "print_settings_id", "filament_settings_id"):
        got = values.get(key, "")
        if re.search(r"\bDefault (Setting|Filament|Printer)\b", got):
            fail(f"{key} = {got!r} — that is a placeholder preset, not a choice")

    flow = first_number(values.get("filament_max_volumetric_speed", ""))
    if flow is not None and flow < args.min_flow:
        fail(
            f"filament_max_volumetric_speed = {flow} (below {args.min_flow}) — this is the "
            f"value that turned a 6h45m plate into 21h03m; a real filament preset is 8-24"
        )

    grams = first_number(values.get("total filament used [g]", ""))
    if grams is not None and grams == 0.0:
        fail("total filament used [g] = 0.00 — a slice that made nothing, or a config that refused to compute")

    if args.expect_printer and args.expect_printer.lower() not in values.get("printer_settings_id", "").lower():
        fail(f"printer_settings_id = {values.get('printer_settings_id')!r}, expected to contain {args.expect_printer!r}")

    if args.expect_filament and args.expect_filament.lower() not in values.get("filament_settings_id", "").lower():
        fail(f"filament_settings_id = {values.get('filament_settings_id')!r}, expected to contain {args.expect_filament!r}")

    return not report["failures"], report


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", help="a .gcode, a .gcode.3mf, or a directory of them")
    ap.add_argument("--expect-printer", default="", help="substring the printer preset must contain")
    ap.add_argument("--expect-filament", default="", help="substring the filament preset must contain")
    ap.add_argument("--min-flow", type=float, default=4.0,
                    help="fail below this filament_max_volumetric_speed (default 4; the placeholder is 2)")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    root = pathlib.Path(args.target)
    if root.is_dir():
        files = sorted(p for p in root.rglob("*") if p.suffix.lower() in (".gcode", ".3mf"))
    else:
        files = [root]
    if not files:
        print(f"no G-code found at {root}", file=sys.stderr)
        return 2

    reports = []
    ok_all = True
    for path in files:
        try:
            ok, report = check(path, args)
        except Exception as exc:  # a file we cannot read is a failure, not a skip
            ok, report = False, {"file": str(path), "failures": [f"could not read: {exc}"]}
        ok_all &= ok
        reports.append(report)

    if args.json:
        print(json.dumps(reports, indent=2))
        return 0 if ok_all else 1

    for report in reports:
        print(f"\n=== {report['file']}")
        for key, value in report.get("identity", {}).items():
            print(f"    {key:32} {value}")
        for key, value in report.get("result", {}).items():
            print(f"    {key:32} {value}")
        for key, (head, tail) in report.get("disagreed", {}).items():
            print(f"    ! {key} changed between the header and the trailing block: {head!r} -> {tail!r}")
        for failure in report["failures"]:
            print(f"    FAIL  {failure}")
        if not report["failures"]:
            print("    PASS  the slice names real presets and produced real figures")

    print()
    print("VERIFIED" if ok_all else "NOT VERIFIED - see the failures above")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
