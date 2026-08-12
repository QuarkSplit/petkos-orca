# Petko's Orca — what this fork is for

`AGENTS.md` describes upstream OrcaSlicer. This file describes why this fork exists, and it
outranks upstream convention wherever the two disagree.

## The objective, and it is the whole objective

**This slicer is single-printer all the way down. It must become multi-printer all the way
down.**

Petko's Orca is FDM-only. Do not retain a project-wide FDM/SLA switch as a reason for keeping
printer state global.

Not "supports assigning a printer to a plate". Every system that consumes a printer must
accept that a project holds several, at once, and behave correctly. Anything less produces
the worst possible outcome: an interface that describes a fleet sitting above an engine that
knows one machine, which is a convincing lie rather than a missing feature.

This is the acceptance test for any change here. A change that adds per-plate data without
making its consumers plural is not progress; it widens the gap between what the app says and
what it does.

There are no fallbacks. A missing or incompatible plate context is unresolved and must be fixed
at its source; it is never permission to substitute the project printer, another preset, or a
nearest match. The same rule applies to workarounds: if a long comment is needed to defend one,
the code is wrong and the real boundary must be repaired.

The slicing identity and physical device identity are separate plate properties. Multiple
physical printers may share one slicing preset, and one physical printer may use different nozzle
variants over time.

Each plate owns its completed slice. Switching plates, slicing another plate, closing the app, or
reopening the project must retain every slice whose complete plate context is unchanged. Batch
dispatch must consume retained results and must not slice all plates in parallel.

Preset portability for downloaded MakerWorld and Printables projects is a real upstream Orca
problem, but it is separate. Do not bury compatibility fallbacks in the per-plate architecture;
make the context model exact first, then solve portable reconciliation independently.

**That deferral was withdrawn on 2026-08-10 and this paragraph is now historical.** The context
model is exact enough; cross-printer import is first-class work. The problem it names is not an
edge case but a routine workflow: the well-plated, coloured, split projects worth downloading are
overwhelmingly authored for Bambu machines, and pushing one onto a different printer produces a
flood of "unavailable" settings, many of which exist on the target under another name, and
sometimes a crash. The per-plate context is an asset here rather than a complication, because a
project's authored printer identity can be preserved per plate instead of flattened onto one
global printer.

**The second scope decision withdrawn the same day: per-plate PROCESS settings are wanted.** The
UI plan put them out of scope because the live control is `ParamsPanel::get_top_panel()` driven by
the Tab's own preset combo, so per-plate process means touching the Tab system rather than the
sidebar. That cost is now worth paying. The case that settles it: a project with one plate needing
supports and one not. Today that is five to ten per-object toggles on one plate and the inverse on
the other, which multiplies the work far past what the task is. Upstream's global/per-object split
is not a constraint to design around; it is one of the things this fork may replace.

**What a cross-printer import does now, as of 2026-08-12.** A project authored for a machine this
build does not have opens: geometry, plates, per-plate printer names and filaments all arrive, and
the plate picker retargets them. Two things had to be true for that, and both are load-bearing:

- **Nothing on the load path throws on an unresolved plate.** `Plater::set_bed_shape()` did, from
  inside `load_files`' own try block, whose catch abandons the input file — so the app discarded 49
  loaded objects and reported "The file does not contain any geometry data" about a file that was
  full of it. Four more throws of the same shape are gone with it. A plate that cannot resolve is a
  thing to report, never a reason to drop a project.
- **Renaming a preset means renaming what points at it.** Loading a project decorates each of its
  presets with `(<project file>)`, per collection, and the process preset's `compatible_printers`
  kept naming the printer's old name — so a project's own two presets declared each other
  incompatible and every plate read as unresolved. `load_config_file_config` completes that rename.

### These are not three bugs. They are one missing mechanism.

The preset-rename fault above, the plate-process fault below, and the nozzle-variant refusal are the
same sentence in three costumes: **a plate's printer identity changed, and the presets that depend on
it were not re-resolved against the new printer.**

| | What changed | What failed to follow | How it surfaced |
|---|---|---|---|
| 1 | the printer's decorated name | `compatible_printers` on the process | every plate silently unresolved (fixed) |
| 2 | the plate's assigned printer | the process | plate stays unresolved until set by hand |
| 3 | the selected printer model | the nozzle variant | hard refusal from the sidebar combo |

They fail in three different ways precisely because there is no re-resolution mechanism: each
dependency is handled ad hoc at its own call site, so each one invents its own failure. Patching
them individually keeps the list three items long.

**The mechanism to build:** when a plate's printer identity changes, every dependent preset is
re-resolved against the new printer's capabilities, through one path, with a single defined rule for
no-match. That rule is already written at the top of this file — no fallbacks, unresolved is
unresolved, fix it at the source — and it belongs in one place rather than at each call site.

Note that this file already classifies two of the three as defects rather than gaps. Item 2 is "an
operation that technically succeeds but leaves the user manual work the app could obviously have
done". Item 3 is a hard refusal to a definite request, which is the silent-gate failure the
objective exists to prevent. Neither is housekeeping.

Item 2's fix is written and was seen working, and it crashes on assignment; see the 2026-08-12
work-log entry. The crash is worth understanding rather than reverting around, because it is
probably the same missing mechanism objecting to being bolted on at one call site.

**This fork has free rein.** Upstream compatibility is not a goal in itself. Rebase cost is real
but secondary, and worth paying where the current design is genuinely wrong rather than merely
different. Quality-of-life gaps count as wrong: an operation that technically succeeds but leaves
the user manual work the app could obviously have done is a defect, not a missing luxury. The
worked example is paste, which drops a copy directly on top of its original and stacks every
subsequent paste in the same spot, leaving the user to drag each one out.

## Historical baseline before the 2026-08-10 refactor

This section records the state that prompted the current work. It is not a description of the
present working tree; use the dated work-log entries below for the changes made since that audit.

Commit `4cf3af2bda` gave a plate its own printer preset (`plater_printer_preset`, persisted in
`Metadata/model_settings.config`) and made the bed draw at that machine's real size. That is
the data layer and the geometry, and it works.

Nothing else was made plural. Three independent reviews of the sidebar each hit the same wall
from a different direction:

| Layer | What is still singular | Where |
|---|---|---|
| Slicing | `reslice()` slices against the project printer and only warns that the plate wanted another | `Plater.cpp` ~16898 |
| Presets | every printer combo is a view onto the ONE edited preset; selection routes through `Tab::select_preset`, which loads globally | `PresetComboBoxes.cpp` 392, 1124 |
| Process, filament, nozzle | not per-plate at any level, only the printer name and bed shape are | `PartPlate.hpp` |
| Devices | one selected machine, one sync target, so live state can be shown for at most one printer | `get_selected_machine()`, `update_sync_status(const MachineObject*)` |
| Reassignment | changing a plate's printer invalidates no slice state and re-checks no bed boundary | `Plater.cpp` ~19129 |

The sidebar showing a project filename where a printer name belongs is a symptom of this, not
the problem. Fixing the label without fixing the layers underneath makes the lie better dressed.

## How to work on it

**Start from the consumer, not the surface.** A plural UI over a singular engine is worse than
the singular UI that at least told the truth. Each stage should leave the app shippable and
should make one more consumer plural.

**Every fork change is a permanent rebase cost against upstream**, which closed the per-plate
request as not planned. Keep diffs small, and comment each site with why it is carried, in
terms of what breaks otherwise.

**Nothing may gate the user out of slicing on a maybe.** The clearance tests compare inflated
2D hulls and approximate the toolhead rather than model it; their own wording says "may". A
maybe must not block a definite when the user is looking at the plate. The bar for a hard
refusal is that emitting the G-code damages the machine, or the thing asked for is genuinely
impossible. Everything else informs and gets out of the way, naming the object it is about.
Silent gates, where a button is disabled or does nothing with no message, are the worst case
and are always a bug.

**Launch only via `run-petkos-orca.bat`.** It passes an isolated `--datadir`; an un-isolated
launch has previously run the setup wizard and clobbered the installed Orca's shared config.

**The app holds `build/src/Release/OrcaSlicer.dll` open while running**, so a link fails with
`LNK1104` if it is not closed first. Copy the rebuilt DLL over `build/OrcaSlicer/OrcaSlicer.dll`
afterwards, or that staging copy silently goes stale beside a current one.

**You do not have to close the app to link.** Windows refuses to delete or overwrite a mapped
image but will happily **rename** one within the same volume. Move `orca-slicer.exe` and
`OrcaSlicer.dll` aside to `*.inuse-<date>.*` and the linker writes fresh files while the running
instance keeps its old mapping. Killing a running slicer to unblock a build risks a project the
user has not saved; renaming risks nothing. Delete the `.inuse-*` files once nothing is holding
them.

## Dated build history

Work logs, session-by-session narrative and closed audit findings live in
[`docs/WORKLOG-2026-08.md`](docs/WORKLOG-2026-08.md). Read it when you need provenance for a
decision, not before starting work. The sections above are the current state of the fork.

