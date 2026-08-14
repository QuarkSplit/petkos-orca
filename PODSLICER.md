# Podslicer — what this fork is for

A pack of orcas is a pod. This fork took OrcaSlicer, which knows one machine, and made it know
a fleet; on 2026-08-14 it took its own name to say so.

`AGENTS.md` describes upstream OrcaSlicer. This file describes why this fork exists, and it
outranks upstream convention wherever the two disagree.

## What it is, in three sentences

**Podslicer is OrcaSlicer with the single-printer assumption removed.** A plate carries its own
printer, process, filament slots and target machine, saved in the project and restored with it,
so one file can hold an ABS plate for one machine, a PLA plate for another and a TPU plate for a
third - each drawn at its real bed size, each arranged against its own bed, each keeping its own
finished slice until something about that plate actually changes.

**Moving a plate to another machine is a translation, not a reset.** Wall count, infill, print
order and every other choice you made travel to the new printer as that plate's own settings; the
same material is re-expressed for the new machine; anything the target genuinely cannot do is
named rather than silently dropped. That is why a project downloaded for a Bambu opens and prints
on a farm that owns no Bambu.

**There is no project printer to get wrong.** The preset dropdowns are an editing cursor that
follows whichever plate you are looking at, so no plate can quietly be slicing against a machine
that was selected an hour ago for something else.

## The demand for this, and what it actually looks like

Upstream closed the canonical request, **#7238**, as *not planned* - by a stale bot, with zero
reactions and two comments, both of them the bot. No human ever replied.

The demand does not show up as reaction counts. It shows up as **the same request independently
refiled at least twelve times in three years** - #1277, #1309, #3593, #3942, #7221, #7238, #8420,
#8596, #10551, #11445, and discussions #6357 and #7506 - at least six of them auto-closed without
a human reply. Nobody piles onto an issue they never find; they file a new one.

Fifteen of those are closed by what is in this tree. The neighbouring high-reaction issues that
turn up in the same searches - multi-extruder toolchangers (#2050, 67 reactions), per-feature
filament (#7106, 37), custom bed surfaces (#836, 36), filament-scope overrides (#12401, 21) - are
things this fork does NOT do, and nothing here should claim them.

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
at its source; it is never permission to substitute another preset or a nearest match. There is
no project printer left to substitute either — see below. The same rule applies to workarounds:
if a long comment is needed to defend one, the code is wrong and the real boundary must be
repaired.

### The project printer was deleted as a state (2026-08-14)

**A plate owns its context. An empty field is not inheritance; it is an error.**

The singular engine did not live in a missing feature — the data layer had held a per-plate
printer since `4cf3af2bda`. It lived in what *absence* was defined to mean. While an empty field
in `PlateSlicingContext` resolved to the globally selected preset, every plate that had not been
explicitly told otherwise had one machine standing behind it, and the resolver read that machine
on every slice, every frame and every board rebuild.

The project printer is not a printer. It is the settings tabs' **cursor** — which preset is
currently being edited — and the application was reading a cursor as a fact about the project.

So the rule now is: a plate names its own printer, its own process and its own materials, or it
is unresolved. The global selection survives as what it always was, an editing cursor, and it
**follows the current plate** (`Plater::follow_plate_presets`) instead of standing behind every
plate. The sidebar's printer, process and filament combos write to the current plate.

`PresetBundle::complete_plate_context` is the ONE remaining read of the global selection on a
plate's behalf, and it runs once per plate, at the two moments a plate can exist without a
context: a plate being created, and a project written before per-plate machines being loaded. A
new plate is seeded from the plates that already exist, which is also what answers "what shape is
a plate that does not exist yet" for arrange, and "what does an overflow land on".

**WHEN it runs is as load-bearing as what it does, and getting it wrong has already shipped
once.** `e0039df8bf` completed the plates inside `PartPlateList::load_from_3mf_structure`, which
runs BEFORE `load_config_model` and `load_project_embedded_presets` put the project's own presets
into the bundle. So every plate was filled in with whatever was selected before the file was
opened - `Default Setting`, `Default Filament` - as CONCRETE names, which then permanently
outranked the real presets the 3MF was carrying. `Default Filament` caps
`filament_max_volumetric_speed` at 2, so a 6h45m plate became 21h03m and reported 0.00 g. For a
print farm those are the two numbers that reach a price and a machine booking. Every in-app check
passed on that build; only reading the emitted G-code caught it, and the commit was reverted in
`1e6a972028`.

The first repair was a timing rule: refuse to complete while a project is loading. That is a patch,
and the tell was the size of the comment defending it - a rule enforced by a flag breaks again the
next time somebody adds a call site. **The real repair is that the seed is an argument.**

`PresetBundle::complete_plate_context(context, seed)` takes the seed by reference and reads no
global state at all. A caller has to name where the values come from, which is exactly the
difference between a value somebody chose and whatever happened to be selected when the code ran.
A signature that cannot reach a global cannot make this mistake, wherever it is called from.

The two callers, and what each names as the holder:

- `complete_plate_contexts()` - a plate in an existing session. Each plate is completed from the
  last complete one, so a new plate lands on the machine the user is already on. Only when no plate
  has a context is the bundle's selection used, and there it is a remembered choice rather than a
  placeholder.
- `complete_plate_contexts(declared)` - a loaded project. `Plater::priv::load_files` reads
  `printer_settings_id`, `print_settings_id` and `filament_settings_id` **out of the 3MF's own
  config**, before that config is moved into the bundle, and passes them in. That is correct
  whenever it runs and whatever the bundle holds, because it came from the file.

The driver's Context phase also asserts that no plate names a preset that `is_default`, which is
the cheap in-app half of the check. The other half is reading `filament_settings_id` and
`filament_max_volumetric_speed` out of the G-code, and it is the half that actually caught it.

The difference between a state and a default is how many times it is read.

Consequences that are load-bearing and easy to undo by accident:

- **Nothing may resolve a plate through `printers.get_edited_preset()`.** An empty context is an
  error with a message naming what is missing. The Context phase of `PetkosPerfDriver` asserts
  exactly this, and asserts that moving the global selection does not change what a plate slices
  with.
- **Arrange overflow keeps its machine.** An item that will not fit goes onto one of arrange's
  extra beds of the same shape — the same machine by construction — and the plate `finalize`
  creates takes the context of the plate it overflowed from. An object must never change machine
  by failing to fit.
- **CLI has a second bed source and it is named, not hidden.** There is no wxApp and no preset
  bundle there; a plate resolves against the project config the 3MF carried.
  `PartPlateList::plate_beds_come_from_presets()` is where that difference is stated. Do not
  "unify" it into a fallback.

Every plate-context field has a user-facing write path, and they share one idiom: click the value,
get a menu of what will actually resolve here, filtered by the same compatibility calls the
composer makes afterwards. A stored name this installation does not have is always offered back
verbatim, because opening a picker must never be what discards it. Filament is written one slot
at a time, from the swatch that shows it.

### A change of machine is a TRANSLATION, never a discard (2026-08-14)

Every FDM printer speaks the same language. A 3MF is always resliced, whatever it was downloaded
for, so a project authored for a Bambu landing on a farm that owns no Bambu must arrive with
everything it intended, expressed on whatever machine will actually print it. There is almost
nothing a Bambu can physically do that an Elegoo cannot.

**So a value that was chosen for another printer is still a value somebody chose.** It is a
translation problem. It is never a reason to substitute a stock default, and never a reason to
drop the value on the floor. Wall count, infill pattern and density, wipe, print order, per-machine
calibration: these are the same decision on any of these machines, and failing to carry them is
inexcusable rather than merely imperfect.

That is why `PresetBundle::carry_process_intent` exists. Switching a plate to a machine whose
process it cannot run changes the process NAME to the machine's own default - a name means nothing
across machines - and carries the chosen VALUES onto the plate as its own overrides. A preset is a
base plus a deviation: the base is a vendor's tuning for one machine, the deviation is what a
person decided, and only the deviation crosses. When the source has no parent profile in this
installation - an embedded preset out of another slicer, the normal case for a download - nothing
distinguishes tuning from intent, so all of it is treated as chosen and carried, and the result
says that is what happened.

**Colour information is material information.** A project routinely wants several materials at once
- PETG for the support interface, PLA everywhere else - and the surface where that is managed is
the one that today presents itself as filament colour. Per-plate `filament_preset_names` is the
field; the swatch strip is its control, one slot at a time. Anything that treats a slot as a colour
rather than as a material is describing half the fact.

**The hard cases, named rather than deferred vaguely.** These are genuine work, not edge cases, and
none of them may be allowed to trip project import or a printer swap - which is the base, and the
base has to keep working while they are built:

- **Negative space and modifier geometry.** Blocking a region with negative geometry to make a
  hole, a magnet pocket or a no-support zone. Bambu Studio and PrusaSlicer express these
  differently in the process/modifier layer, and there are many slicer-printer pairings a 3MF
  import has to reconcile. Expect heavy work here.
- **Print by object.** A by-object project needs toolhead collision resolution translated from one
  machine's kinematics to another's, not merely copied.
- **Dual nozzle.** A project authored for an H2D genuinely has to be refactored for a
  single-nozzle machine; the G-code is a different shape. This is the one case where "translate"
  is not enough.
- **Filament and tool swaps are a SLICING step.** Different machines handle a swap differently.
  That difference belongs at slice time and must not reach import or printer-swap code at all.

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
  the printer's own declaration, never a nearest match. The VALUES that were chosen come with
  it as plate overrides; see `carry_process_intent` and the translation rule above.
- A filament that cannot run is TRANSLATED when the same material exists for the new printer,
  and reported by name when it does not. **Amended 2026-08-14, and the amendment matters more
  than the rule it replaces.** "Never rewrite a filament" was written to protect a MATERIAL
  choice, and it still does: PLA turned into PETG is a different thing coming out of the
  nozzle and stays refused. But PLA re-expressed as the target machine's PLA is not a
  substitution at all - a filament preset is a base plus a deviation exactly as a process
  preset is, the base is one vendor's tuning of one material for one machine, and none of
  that tuning means anything elsewhere. The discriminator is one string, `filament_type`.
  Without this, pointing a downloaded Bambu plate at a machine you own left the plate unable
  to SLICE, because the composer refuses a context with an incompatible filament - 54 clicks
  of hand repair on a six-plate four-colour project before anything would run. See
  `PresetBundle::translate_filament_to_printer` for the preference order, every step of which
  is a declaration rather than a guess.
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

Of that table, the Presets, Process/filament/nozzle and Reassignment rows are closed. The Devices
row is the one still standing: a plate carries a `physical_printer_id` and can be given one from
the inspector, but the monitor and sync layers still hold one selected machine.

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

