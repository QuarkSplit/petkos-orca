#!/usr/bin/env python3
"""Podslicer: the fork's own laws, checked against the source.

Three of the bugs fixed on 2026-09-14 were the same sentence in different costumes - a place that
still believed in a project printer, or still asked a question the app had enough information to
answer. Fixing the instances does not stop the next one: the laws live in prose, in a document
nobody compiles.

So they live here too. Each rule below is one law, stated as something that must not appear in the
source. A rule that cannot be checked precisely is not in this file - a lint that cries wolf is a
lint that gets suppressed, which is worse than no lint.

    python tools/pod-lint.py            # check
    python tools/pod-lint.py --list     # what is being enforced, and why

Exit code 0 when every law holds.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SRC = REPO / "src"


def function_body(text: str, signature: str):
    """The text of the function whose definition line contains `signature`.

    The end is the next line that is exactly `}` at column 0. That is a formatting convention
    rather than a parse, and it is stated as one - but it is the convention this whole tree
    follows, and it does not lie. Counting braces DOES lie: a '{' inside a string literal, a char
    literal or a comment is not a scope, and the first attempt at this rule counted one, ran off
    the end of load_files, and reported 71 breaches in functions it had never entered. A check
    that over-reports is worse than no check, because it teaches people to ignore it."""
    i = text.find(signature)
    if i < 0:
        return None
    start_line = text[:i].count("\n") + 1
    lines = text.splitlines()
    body = []
    for n in range(start_line, len(lines)):
        if lines[n] == "}":
            return start_line, "\n".join(body)
        body.append(lines[n])
    return None


# THE ONE EXEMPTION, named rather than left as a failing line nobody reads.
#
# The law is "the app decides what it has enough information to decide". Its exception is not
# "this dialog is old" or "this dialog is hard to remove" - it is that the USER HAS ASKED TO BE
# ASKED. StepMeshDialog is the STEP tessellation picker, and it only opens when the preference
# "enable_step_mesh_setting" is on. With it off, the import silently uses the stored linear and
# angle deflection, which is the decision the law demands; with it on, the user has said "let me
# set the deflection per file", and honouring that is not ceremony, it is the feature.
#
# It is also not on the project-open path at all: it is reachable only for .stp/.step files, and
# a 3MF never enters that branch. If the preference is ever removed, this exemption goes with it.
MODAL_EXEMPT_IN_LOAD_FILES = ("mesh_dlg.ShowModal",)


def rule_no_modal_in_load_files(findings):
    """LAW: opening a project never asks.

    A downloaded project arrives authored for a machine the farm does not own; retargeting it is
    something the app can decide and report. A modal raised from inside load_files also stacks
    under the progress dialog and holds the whole load hostage, and the printer-offer dialog that
    lived here crashed when accepted, because it acted on half-constructed state."""
    p = SRC / "slic3r/GUI/Plater.cpp"
    text = p.read_text(encoding="utf-8", errors="replace")
    found = function_body(text, "std::vector<size_t> Plater::priv::load_files(")
    if found is None:
        findings.append((p, 0, "cannot find Plater::priv::load_files - this rule has gone blind"))
        return
    start, body = found
    body_lines = body.splitlines()
    for n, line_text in enumerate(body_lines):
        # Commented-out code is not a call. Several of these dialogs were already disabled by
        # being commented out rather than deleted, and counting them as live made the rule report
        # work that was done years ago.
        stripped = line_text.strip()
        if stripped.startswith("//") or stripped.startswith("*"):
            continue
        code = line_text.split("//", 1)[0]
        if not re.search(r"\bShowModal\s*\(", code):
            continue
        if any(exempt in code for exempt in MODAL_EXEMPT_IN_LOAD_FILES):
            continue
        findings.append((p, start + n, "ShowModal inside load_files - opening a project must not ask"))


def rule_no_project_printer(findings):
    """LAW: there is no project printer.

    A plate names its own machine or it is unresolved. An empty field is an error, not
    inheritance. Any surface offering 'same as the project printer' is offering a state that does
    not exist, and the picker that did it wrote an empty string straight through to the plate."""
    for p in SRC.rglob("*.cpp"):
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in re.finditer(r'_L\(\s*"([^"]*[Pp]roject [Pp]rinter[^"]*)"', text):
            said = m.group(1)
            # A string that DENIES the project printer is the rule being explained to the user,
            # not a breach of it. Only an OFFER of one is a breach.
            if re.search(r"(?i)\bno project printer\b", said):
                continue
            if not re.search(r"(?i)same as|follows?\b|defaults? to|inherit", said):
                continue
            line = text[: m.start()].count("\n") + 1
            findings.append((p, line, f"user-facing string offers a project printer: {said!r}"))


def rule_plate_never_resolves_through_the_cursor(findings):
    """LAW: nothing resolves a plate through the globally selected preset.

    The preset dropdowns are an editing cursor - which preset is being edited - not a fact about
    the project. While a plate could resolve by reading the cursor, every plate had that machine
    standing behind it, and moving the cursor changed what an untouched plate would slice."""
    targets = ["slic3r/GUI/PartPlate.cpp", "libslic3r/PlateSlicingContext.hpp"]
    for rel in targets:
        p = SRC / rel
        if not p.exists():
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r"printers\s*\.\s*get_edited_preset\s*\(", text):
            line = text[: m.start()].count("\n") + 1
            # A line that is a comment about the rule is not a breach of it.
            line_text = text.splitlines()[line - 1].strip()
            if line_text.startswith("//") or line_text.startswith("*"):
                continue
            findings.append((p, line, "a plate resolved through the edited printer preset"))


RULES = [
    rule_no_modal_in_load_files,
    rule_no_project_printer,
    rule_plate_never_resolves_through_the_cursor,
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="print the laws and what each is for")
    args = ap.parse_args()

    if args.list:
        for r in RULES:
            print(f"== {r.__name__}")
            print((r.__doc__ or "").rstrip() + "\n")
        return 0

    findings: list = []
    for r in RULES:
        r(findings)

    if not findings:
        print(f"pod-lint: {len(RULES)} laws hold")
        return 0
    for p, line, msg in findings:
        rel = p.relative_to(REPO).as_posix()
        print(f"{rel}:{line}: {msg}")
    print(f"\npod-lint: {len(findings)} breach(es) of {len(RULES)} laws")
    return 1


if __name__ == "__main__":
    sys.exit(main())
