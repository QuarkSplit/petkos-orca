#!/usr/bin/env python3
"""Podslicer: find vendor profiles whose own declared default is incompatible with them.

A printer profile names the process it wants by default, in `default_print_profile`. This fork
reads that declaration whenever a plate's printer identity changes: the plate's process is
re-resolved against the new machine, and a process that cannot run on it is replaced by the
machine's OWN declaration rather than by a nearest match, because a declaration is a decision
somebody made and a nearest match is a guess.

That only works while the declaration is true. `Flashforge Creator 5 0.4 nozzle` inherited its
`default_print_profile` from its Adventurer 5M Pro parent and never overrode it, so it declared a
process whose `compatible_printers` lists only Adventurer machines - the printer's own default was
incompatible with the printer. Re-resolution had nowhere to land and the plate stayed unresolved,
which is what the user saw as a red error on a machine they had just chosen.

This walks every vendor's machine profiles, resolves the declaration through the `inherits` chain
on both sides, and reports every machine whose declared default cannot run on it.

    python tools/petkos-vendor-audit.py [--profiles DIR] [--vendor NAME] [--json] [--quiet]

Exit code is 0 when nothing is self-incompatible, 1 when something is, 2 when the profile tree
could not be read. `--vendor` may be repeated.

Two things this deliberately does not do. It does not evaluate
`compatible_printers_condition`, which is a slicer expression rather than data - a profile that
carries only a condition is reported as unchecked, and counted separately, so an empty findings
list never means "everything was checked". And it does not edit anything: a vendor's data is
theirs, and knowing which line is wrong is the part a script can be trusted with.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Dict, List, Optional, Tuple

# A profile inherits from a profile that inherits from a profile. A cycle in vendor data is
# possible and must not hang the audit, so every walk is bounded.
MAX_INHERIT_DEPTH = 32


def read_json(path: str) -> Optional[dict]:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError) as exc:
        print(f"  ! could not read {path}: {exc}", file=sys.stderr)
        return None


def load_collection(vendor_dir: str, kind: str) -> Dict[str, dict]:
    """Every profile of one kind for one vendor, keyed by its declared name.

    The name in the file wins over the filename: `inherits` and `default_print_profile` both name
    profiles, never files, and the two disagree often enough in vendor data to matter.
    """
    out: Dict[str, dict] = {}
    folder = os.path.join(vendor_dir, kind)
    if not os.path.isdir(folder):
        return out
    for entry in sorted(os.listdir(folder)):
        if not entry.lower().endswith(".json"):
            continue
        data = read_json(os.path.join(folder, entry))
        if data is None:
            continue
        name = data.get("name") or os.path.splitext(entry)[0]
        data["__file__"] = os.path.join(folder, entry)
        out[name] = data
    return out


def resolve_key(collection: Dict[str, dict], name: str, key: str) -> Tuple[Optional[object], Optional[str]]:
    """The value of one key for one profile, taken from the nearest ancestor that carries it.

    Returns the value and the name of the profile it came from, so a report can say where a wrong
    declaration actually lives - which for the Creator 5 was a different file from the one that
    declared it.
    """
    seen = set()
    current = name
    for _ in range(MAX_INHERIT_DEPTH):
        profile = collection.get(current)
        if profile is None or current in seen:
            return None, None
        seen.add(current)
        if key in profile:
            return profile[key], current
        parent = profile.get("inherits")
        if not parent:
            return None, None
        current = parent
    return None, None


def audit_vendor(vendor_dir: str) -> Tuple[List[dict], List[dict], int]:
    """(self-incompatible findings, unchecked declarations, machines examined) for one vendor."""
    machines = load_collection(vendor_dir, "machine")
    processes = load_collection(vendor_dir, "process")
    findings: List[dict] = []
    unchecked: List[dict] = []
    examined = 0

    for machine_name, machine in sorted(machines.items()):
        # `instantiation: false` marks a base profile that exists to be inherited from and can never
        # be selected, so no plate can ever resolve against it. Auditing those reports one fault per
        # family tree as well as per machine, which is how a first run of this counted 278.
        if str(machine.get("instantiation", "true")).lower() != "true":
            continue
        # A printer_model file describes a model rather than a sliceable variant: it names no
        # nozzle and declares no process, and nothing resolves a plate against it.
        if "printer_variant" not in machine and "nozzle_diameter" not in machine:
            continue
        declared, declared_by = resolve_key(machines, machine_name, "default_print_profile")
        if not declared:
            continue
        examined += 1

        if declared not in processes:
            findings.append({
                "vendor": os.path.basename(vendor_dir),
                "machine": machine_name,
                "file": machine["__file__"],
                "declared": declared,
                "declared_by": declared_by,
                "reason": "the declared process does not exist in this vendor's profiles",
            })
            continue

        listed, listed_by = resolve_key(processes, declared, "compatible_printers")
        if listed is None:
            condition, _ = resolve_key(processes, declared, "compatible_printers_condition")
            unchecked.append({
                "vendor": os.path.basename(vendor_dir),
                "machine": machine_name,
                "declared": declared,
                "condition": condition or "",
            })
            continue

        if isinstance(listed, str):
            listed = [listed]
        if machine_name not in listed:
            findings.append({
                "vendor": os.path.basename(vendor_dir),
                "machine": machine_name,
                "file": machine["__file__"],
                "declared": declared,
                "declared_by": declared_by,
                "reason": "the declared process lists %d compatible printers and this is not one of them"
                          % len(listed),
                "listed": sorted(listed),
                "listed_by": listed_by,
            })

    return findings, unchecked, examined


def main() -> int:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profiles", default=os.path.join(here, "resources", "profiles"),
                        help="the profiles directory to audit (default: this repo's)")
    parser.add_argument("--vendor", action="append", default=[],
                        help="audit only this vendor folder; may be repeated")
    parser.add_argument("--json", action="store_true", help="emit the findings as JSON")
    parser.add_argument("--quiet", action="store_true", help="print findings only, no summary")
    args = parser.parse_args()

    if not os.path.isdir(args.profiles):
        print(f"No profiles directory at {args.profiles}", file=sys.stderr)
        return 2

    vendors = args.vendor or sorted(
        name for name in os.listdir(args.profiles)
        if os.path.isdir(os.path.join(args.profiles, name)))

    all_findings: List[dict] = []
    all_unchecked: List[dict] = []
    examined = 0
    for vendor in vendors:
        vendor_dir = os.path.join(args.profiles, vendor)
        if not os.path.isdir(os.path.join(vendor_dir, "machine")):
            continue
        findings, unchecked, count = audit_vendor(vendor_dir)
        all_findings.extend(findings)
        all_unchecked.extend(unchecked)
        examined += count

    if args.json:
        print(json.dumps({"findings": all_findings, "unchecked": all_unchecked,
                          "machines_examined": examined}, indent=2))
        return 1 if all_findings else 0

    current_vendor = None
    for finding in all_findings:
        if finding["vendor"] != current_vendor:
            current_vendor = finding["vendor"]
            print(f"\n{current_vendor}")
        print(f"  {finding['machine']}")
        print(f"    declares  {finding['declared']}")
        if finding.get("declared_by") and finding["declared_by"] != finding["machine"]:
            print(f"    inherited from  {finding['declared_by']}")
        print(f"    {finding['reason']}")
        if finding.get("listed"):
            print(f"    it lists  {', '.join(finding['listed'])}")

    if not args.quiet:
        print(f"\n{len(all_findings)} self-incompatible declaration(s) across {examined} machine profile(s)"
              f" in {len(vendors)} vendor folder(s).")
        if all_unchecked:
            print(f"{len(all_unchecked)} declaration(s) not checked: the process states a"
                  f" compatible_printers_condition rather than a list.")

    return 1 if all_findings else 0


if __name__ == "__main__":
    sys.exit(main())
