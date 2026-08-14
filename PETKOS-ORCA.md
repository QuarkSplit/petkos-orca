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

### The re-resolution mechanism (built 2026-08-12)

Three faults — the preset-rename fault above, a plate left unresolved after assignment, and a
hard nozzle-variant refusal from the model combo — were one sentence in three costumes: **a
plate's printer identity changed, and the presets that depend on it were not re-resolved against
the new printer.** The mechanism now exists:
`PresetBundle::reresolve_plate_context_for_printer`, called from both plate-printer write paths.
Its rules, which are the top-of-file rules applied to a change of identity:

- A dependent preset that still runs on the new printer is kept. It is still what the user chose.
- A process that cannot run switches to the new printer's own declared `default_print_profile` —
  the printer's own declaration, never a nearest match. A plate cleared back to "follow the
  project" re-inherits the Project pairing instead.
- A filament that cannot run is reported by name and never rewritten: filament is material
  choice, and substituting one is a silent yes with a real cost.
- Anything that still cannot resolve stays as it is and is named. Unresolved is unresolved.
- A printer this build does not have preserves the whole context verbatim — the project may be
  headed to a machine that has it.

The model combo applies the same shape to its own dependent: picking a printer MODEL re-resolves
the nozzle variant — carried over when the model has it, the model's single variant when not, a
named choice when several exist. The only refusal left is a model with no preset at all.

The crash that stalled item 2's first fix was never in the preset system: it was a dangling
`MeshRaycaster` in the scene raycaster, exposed by any per-plate bed change and triggered by the
mouse being over the canvas. Ownership is now shared (`SceneRaycasterItem` / `PickingModel` /
`GLVolume` hold `shared_ptr`), so no rebuild order can dangle a registered raycaster. Full
forensics, including where the app's own crash logs live, in the 2026-08-12 work-log entries.

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

**A message in the corner is the app asking for help. It costs the user attention, so it has
to be worth it.** The bar, in order:

- **One sentence, in the user's terms**, and a count rather than a list. A message that grows
  with the number of affected objects is a message that fails hardest exactly when it matters
  most, and re-pointing a plate at a smaller machine does that to every fit warning at once.
- **Say which problem this is.** Two faults reported as one sentence because one test answered
  both is the test's convenience, not the user's. If they are cheap to tell apart, tell them
  apart and raise two.
- **Name nothing the user cannot reach.** An object's name in a message is only useful if
  clicking it selects that object. `NotificationManager::ObjectProblemNotification` is the
  mechanism: one line, click to select, a More/Less toggle over the names and the remedy.
- **Do the work rather than describe it.** If the app can fix the thing, the message offers the
  fix as a link. If it cannot fix it honestly — anything that changes what gets made — it says
  so and leaves the choice alone, because a silent yes costs more than a silent no.
- **Severity is not a visual treatment.** `PopNotification::uses_block_render()` lets a
  notification choose its own renderer; upstream inferred a full-bleed red panel from the level
  alone, so one calm line of text arrived shouting. Red belongs on the edge, not the field.

**Nothing may gate the user out of slicing on a maybe.** The clearance tests compare inflated
2D hulls and approximate the toolhead rather than model it; their own wording says "may". A
maybe must not block a definite when the user is looking at the plate. The bar for a hard
refusal is that emitting the G-code damages the machine, or the thing asked for is genuinely
impossible. Everything else informs and gets out of the way, naming the object it is about.
Silent gates, where a button is disabled or does nothing with no message, are the worst case
and are always a bug.

**Do not optimise the GUI from reading.** The per-plate render loop is the obvious suspect and
was never the cost: 2.1 ms of an 88 ms frame at 36 plates. It is CPU-bound throughout — the buffer
swap stays at 0.6 ms while the frame triples.

**The frame was one mechanism wearing two costumes, and reading them as two problems was the
error.** A flat ~21 ms in `_render_overlays` and ~1.9 ms per volume in `_render_objects` were both
`Plater::get_extruders_colors()`, which composes a complete ~974-option `DynamicPrintConfig` — the
printer, the process and every filament preset, deep-copied, then five whole-config passes — in
order to read `filament_colour`. 1.49 ms a call, 47 calls a frame: 36 inside the volume loop and
11 through `GLGizmosManager::get_selectable_idxs`, where `GLGizmoMmuSegmentation` answers "is there
more than one filament?" by composing the config and taking `.size()`.

Hoisting the palette out of the volume loop and memoising the selectability query on a per-frame
counter took the frame from **86.6 ms to 5.8 ms** and startup from 25 s to 12 s. Paths that never
read the palette got 2-5× faster alongside it, because 34,586 copies of a 974-option config per run
was thrashing the allocator for the whole app. **When a cost ignores the scene, it is not about the
scene** — that is what identified it, and it is the general rule.

**What is left is not the frame.** At 36 plates the remaining costs are plate switching
(`SelectPlate` ~71 ms), the board (`BoardRowLayout` ~35 ms, `SppBoardRefresh` ~84 ms) and
thumbnail rendering (~44 ms). Measure before touching any of them.

**The instrument is in the tree.** `PETKOS_PERF=1` turns on scoped spans; `PETKOS_PERF_SCRIPT`
drives a scripted run through the production paths; `pwsh -Command "& tools/petkos-perf-run.ps1"`
takes 1, 6 and 36 plates and `tools/petkos-perf-table.py` puts them side by side. Free when off.
Five traps it already knows about: the app opens on Home where the canvas does not draw, so a run
that forgets to select the editor measures an idle app and calls it fast; `pwsh -File` flattens
`-Plates 1,6,36` into 1636; `create_plate` refuses past `MAX_PLATE_COUNT`; a killed process
writes no CSV, because the flush is in `GUI_App::OnExit`; and **a probe can be attached to the
wrong door**. `ResolvePlateContext` originally wrapped only `PartPlate.cpp`'s file-static compose
helper, which genuinely never fires during a frame — so the instrument reported zero compositions
per frame while the app was doing 47 through `Plater::resolve_plate_slicing_config`. An instrument
with a hole in it is worse than none, because it is believed. When a span reads zero, confirm it
can fire at all before concluding the path is cold. Never measure while a build runs.

**Launch only via `run-petkos-orca.bat`.** It passes an isolated `--datadir`; an un-isolated
launch has previously run the setup wizard and clobbered the installed Orca's shared config.

**The app holds `build/src/Release/OrcaSlicer.dll` open while running**, so a link fails with
`LNK1104` if it is not closed first. Copy the rebuilt DLL over `build/OrcaSlicer/OrcaSlicer.dll`
afterwards, or that staging copy silently goes stale beside a current one.

**A full rebuild must not be a child of the shell that starts it.** A rebuild here is 30-35
minutes, and a long-running background shell is reliably stopped before then. When it goes the
top-level `cmake` goes with it, but MSBuild's worker nodes keep compiling for another minute or
two — so the tree fills with fresh, correct object files and then falls silent, having never
linked. Nothing reports an error, because the process that would have reported it is the one that
was killed, and from the outside it is indistinguishable from a finished build. The only tell is
the timestamp on `build/src/Release/OrcaSlicer.dll`. Use `tools/petkos-build-detached.ps1`, which
runs it as a one-shot Task Scheduler task and signals completion with a FILE containing the exit
code — a handle belongs to a session, a file does not. `tools/petkos-dev-build.ps1` is still the
right thing for a quick incremental build you will sit through.

**A newer `.obj` timestamp is not evidence that its contents are current.** Editing a source file
while a build is running produces an object stamped with the finish time and compiled from the text
as it was at the start. The build is green and the change is simply absent. Never edit source
during a build, and confirm a change reached the binary by looking for a marker string in it:
`strings -a build/src/Release/OrcaSlicer.dll | grep -c "<some literal you added>"`. Same family as
the probe attached to the wrong door below.

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

