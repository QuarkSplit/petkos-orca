#!/usr/bin/env python3
"""Turn a sliced Orca project into the slice-report.json the farm pipeline expects.

`E:\\3D-Printing\\Projects\\<project>\\_PIPELINE.md` says a model reaches `04-sliced\\` when it has
"`.gcode` + `slice-report.json` with real time / grams / colours". This produces that report by
reading the saved project, so the numbers come from the slice rather than from an estimate.

    python tools/petkos-slice-report.py <project.3mf> [-o slice-report.json]

Every figure is read out of the file or left null. Nothing here fills a gap with a guess: a
fabricated weight or print time reaches a pricing decision and a machine booking, and a null that
says "this was not measured" is worth more than a number that cannot be trusted.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import zipfile
import xml.etree.ElementTree as ET


def _items(node) -> dict:
    """Collect <metadata key= value=/> and <*_item key= value=/> children into a dict."""
    out = {}
    for child in node:
        key = child.get("key")
        if key is not None:
            out[key] = child.get("value")
    return out


def read_project(path: pathlib.Path) -> dict:
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())

        # Per-plate identity: which machine, which materials, which overrides. This is the part the
        # per-plate work exists to make true, so the report states it explicitly rather than
        # implying one machine for the project.
        plates: list[dict] = []
        if "Metadata/model_settings.config" in names:
            root = ET.fromstring(z.read("Metadata/model_settings.config").decode("utf-8", "replace"))
            for plate_node in root.findall("plate"):
                meta = _items(plate_node)
                overrides = {
                    k.split(":", 1)[1]: v
                    for k, v in meta.items()
                    if k.startswith("plater_plate_config:")
                }
                plates.append({
                    "index": int(meta.get("plater_id", "0") or 0),
                    "name": meta.get("plater_name") or None,
                    "printer": meta.get("plater_printer_preset") or None,   # null = follows the project
                    "printerVendor": meta.get("plater_printer_vendor") or None,
                    "process": meta.get("plater_print_preset") or None,
                    "filaments": (meta.get("plater_filament_presets") or None),
                    "objectCount": len(plate_node.findall("model_instance")),
                    "overrides": overrides or None,
                    "printTimeSeconds": None,
                    "weightGrams": None,
                })

        # The measured figures, if the project carries a slice.
        if "Metadata/slice_info.config" in names:
            root = ET.fromstring(z.read("Metadata/slice_info.config").decode("utf-8", "replace"))
            for plate_node in root.findall("plate"):
                meta = _items(plate_node)
                try:
                    idx = int(meta.get("index", "0") or 0)
                except ValueError:
                    continue
                target = next((p for p in plates if p["index"] == idx), None)
                if target is None:
                    continue
                # An attribute can be PRESENT and empty - a plate whose slice recorded no figure.
                # That is a null, not a zero and not a crash: reporting 0 g would be a measurement
                # nobody took, and it reaches a price and a machine booking.
                def _num(key):
                    raw = (meta.get(key) or "").strip()
                    try:
                        return float(raw)
                    except ValueError:
                        return None
                if (v := _num("prediction")) is not None:
                    target["printTimeSeconds"] = int(v)
                if (v := _num("weight")) is not None:
                    target["weightGrams"] = round(v, 1)
                filaments = [_items(f) for f in plate_node.findall("filament")]
                if filaments:
                    target["filamentsUsed"] = filaments

        # The project's own filament slots: colour and, now, finish. A silver-and-yellow job is two
        # slots, and the report should say which is which without anyone opening the file.
        colours: list[str] = []
        finishes: list[str] = []
        if "Metadata/project_settings.config" in names:
            cfg = json.loads(z.read("Metadata/project_settings.config").decode("utf-8", "replace"))
            colours = cfg.get("filament_colour") or []
            finishes = cfg.get("filament_finish") or []

        gcodes = sorted(n for n in names if n.lower().endswith(".gcode"))

        # Read the settings back out of the G-CODE, not out of the app or the project config.
        # Orca appends its whole effective config as "; key = value" at the end of every file, so
        # this is what the machine will actually be told to do. It is the only reading that cannot
        # be a label disagreeing with the job: a per-plate override that fails to reach here has
        # not worked, whatever the UI said.
        WANTED = ("printer_model", "printer_settings_id", "print_settings_id", "bed_shape",
                  "wall_loops", "sparse_infill_density", "sparse_infill_pattern",
                  "layer_height", "filament_colour", "filament_finish")
        for entry in gcodes:
            raw = z.read(entry)
            # The config block is appended at the END of the file; a 35 MB G-code does not need
            # decoding in full to read it.
            tail = raw[-200000:].decode("utf-8", "replace")
            found = {}
            for line in tail.splitlines():
                if not line.startswith("; "):
                    continue
                m = re.match(r"; ([a-z_]+) = (.*)$", line)
                if m and m.group(1) in WANTED and m.group(1) not in found:
                    found[m.group(1)] = m.group(2).strip()
            # Metadata/plate_N.gcode -> plate index N
            m = re.search(r"plate_(\d+)\.gcode$", entry)
            if m:
                idx = int(m.group(1))
                target = next((p for p in plates if p["index"] == idx), None)
                if target is not None:
                    target["gcode"] = {"file": entry, "bytes": len(raw), "settings": found}

    slots = [
        {"slot": i + 1,
         "colour": colours[i] if i < len(colours) else None,
         "finish": finishes[i] if i < len(finishes) else None}
        for i in range(max(len(colours), len(finishes)))
    ]

    sliced = [p for p in plates if p["printTimeSeconds"] is not None]
    machines = sorted({p["printer"] for p in plates if p["printer"]})

    return {
        "project": path.name,
        "plates": plates,
        "filamentSlots": slots,
        "gcodeFiles": gcodes,
        "summary": {
            "plateCount": len(plates),
            "platesSliced": len(sliced),
            "distinctMachines": len(machines),
            "machines": machines,
            # Sums over what was actually measured. If a plate has no slice it contributes nothing
            # and platesSliced says so, rather than a total that quietly means "some of it".
            "totalPrintTimeSeconds": sum(p["printTimeSeconds"] for p in sliced) or None,
            "totalWeightGrams": (round(sum(p["weightGrams"] for p in sliced if p["weightGrams"]), 1)
                                 if any(p["weightGrams"] for p in sliced) else None),
        },
        # The claim worth making, stated as data: which settings actually differ between plates,
        # read from the G-code. "Two machines from one project" was always true; two different
        # PROCESS answers from one project is the thing that was not.
        "perPlateDifferences": _differences(plates),
    }


def _differences(plates: list[dict]) -> dict:
    """Which G-code settings differ across plates, and what each plate got."""
    with_gcode = [p for p in plates if p.get("gcode")]
    if len(with_gcode) < 2:
        return {}
    keys = set().union(*(p["gcode"]["settings"].keys() for p in with_gcode))
    out = {}
    for k in sorted(keys):
        values = [p["gcode"]["settings"].get(k) for p in with_gcode]
        if len(set(values)) > 1:
            out[k] = {f"plate{p['index']}": p["gcode"]["settings"].get(k) for p in with_gcode}
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project", type=pathlib.Path)
    ap.add_argument("-o", "--out", type=pathlib.Path)
    args = ap.parse_args()

    if not args.project.exists():
        print(f"no such project: {args.project}", file=sys.stderr)
        return 1

    report = read_project(args.project)
    out = args.out or args.project.with_name("slice-report.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=1, ensure_ascii=False), encoding="utf-8")

    s = report["summary"]
    print(f"wrote {out}")
    print(f"  {s['plateCount']} plate(s), {s['platesSliced']} sliced, "
          f"{s['distinctMachines']} machine(s): {', '.join(s['machines']) or '(none named)'}")
    for slot in report["filamentSlots"]:
        print(f"  slot {slot['slot']}: colour={slot['colour']} finish={slot['finish']}")
    diffs = report.get("perPlateDifferences") or {}
    if diffs:
        print("  settings that DIFFER between plates, read from the G-code:")
        for k, v in diffs.items():
            print(f"    {k}: " + ", ".join(f"{plate}={val}" for plate, val in v.items()))
    else:
        print("  no per-plate differences found in the G-code")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
