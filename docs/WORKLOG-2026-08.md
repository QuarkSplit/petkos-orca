# Work log — August 2026

Dated build history and closed audit findings for the fork, split out of `PODSLICER.md` on
2026-08-11 so the root file states current architecture rather than its own history.

**Read this when you need provenance, not before starting work.** `PODSLICER.md` is the
authority on how the fork behaves now; anything here is a record of how it got there and may
describe states that no longer exist.

## Work log — 2026-08-13 (the lag is measured, and it is not where anyone was looking)

The report was "since plates got their own printers, the interface feels laggy" — canvas
interaction, plate switching, printer assignment. The suspect list that came with it was
written from reading the code, and it led with the obvious one: every plate is drawn every
frame, with the plate list locked, ~13 GL buffers per plate, no frustum cull.

That suspect is real and it is 2% of the problem.

### The instrument had to exist first

The headless slicing rig cannot see a GUI stall, and the in-repo Shiny profiler forces TBB to
one thread, which changes the app it is measuring. `PetkosPerf` is the smallest honest
alternative: an env-gated scoped span (`PETKOS_PERF=1`) writing into a preallocated ring, with
a CSV and a summary at exit. It is free when off — one cached bool and a predictable branch —
and it stays in the tree, because a performance claim that cannot be re-run is a story.

Samples do not go through `BOOST_LOG`. This build forces severity to `info` (the version
string is `2.5.0-dev`, and `set_logging_level` clamps for pre-release builds) and the file sink
is `auto_flush`, so a per-frame log line is a synchronous disk write large enough to be
mistaken for the bug being hunted.

Interactions carry **both halves**: the first frame that shows a result, and the moment the
event loop goes idle. That split is the whole point — it is what distinguishes an app that is
slow from an app that paints quickly and then stays busy, and the two want different fixes.

`PetkosPerfDriver` makes it repeatable. The plate board is custom-painted and the canvas is a
GL surface, so nothing external can reach either; the driver rides the app's own idle and calls
the production paths — `rotate_on_sphere`, `select_plate`, `set_plate_printer`, real
`wxMouseEvent`s posted at the board. Idle rather than a timer, because Windows' default timer
resolution is ~15.6 ms and would cap a frame-time measurement at 64 fps, hiding exactly the
range in question.

Two things it got wrong first, both now guarded in code rather than in someone's memory:

- **The app opens on Home**, where the 3D canvas is not shown and `render()` early-returns. The
  first pass reported 40 orbit frames in 15 ms and not one of them was drawn. A run that
  measures an idle app looks *fast*, which is the worst possible failure for an instrument. It
  now selects the editor first and logs an error if it cannot.
- **`create_plate` refuses past `MAX_PLATE_COUNT`**, so a loop written against the requested
  count never terminates. It now stops when the list stops growing and names the cap.

### What it measured

At 1 / 6 / 36 plates, Release, one 40 mm cube per plate:

| | 1 | 6 | 36 |
|---|---|---|---|
| frame `CanvasRender` p50 | 28.5 ms | 33.9 | **88.1 ms** |
| `_render_overlays` | **20.6** | 21.0 | 22.4 |
| `_render_objects` opaque | 3.6 | — | **70.3** |
| all plate drawing | 0.09 | 0.38 | 2.1 |
| both raycaster walks | 0.014 | 0.04 | 0.16 |
| `CanvasSwap` | 0.74 | 0.63 | 0.61 |
| plate switch → painted | — | 58.7 | 211 (settle 401) |
| printer assign → painted | 82.9 | 152 | 387 |
| board row drag → settle | — | 24.9 | 408 |

Everything scales with plate count, and it is **CPU-bound** — the swap stays flat at 0.6 ms
while the frame triples, so nothing here is the GPU's fault.

`PlateRender` does fire 35.2 times per frame at 36 plates, exactly as the audit said. Each call
costs 0.056 ms. The whole per-plate render loop, mutex included, is 2.1 ms of an 88 ms frame.
The two raycaster walks are 0.16 ms together. The frame belongs to two things nobody had named:

- **`_render_overlays` costs a flat ~21 ms every frame**, whatever the plate count. At one plate
  that is 80% of the frame, which is why an empty project runs at 39 fps.
- **`_render_objects` costs ~1.9 ms per volume** — not per plate. That is where plate count
  enters the frame, because plates carry objects.

### One model, one owner

The clicks are the plate board. A printer assignment at 36 plates cost 191 ms, of which 120 ms
was `SppBoardRefresh`; the board refresh *is* the click. `PlateBoardModel::rebuild` is O(plates)
whole-config compositions — each row asks `get_extruders()`, which composes that plate's entire
~1000-key `DynamicPrintConfig` at 1.6 ms a time — and `PlateInspector::reload` then built a
**second complete model on the stack**, read it, and discarded it, while the board's identical
and freshly-rebuilt one sat one pointer away.

The board now publishes the model it has already built. Nothing else changed: the inspector only
ever read those rows.

| at 36 plates | before | after |
|---|---|---|
| `BoardRowLayout` | 48 calls, 1931 ms | 24 calls, 1085 ms |
| `ResolvePlateContext` | 1364 calls, 2077 ms | 752 calls, 1236 ms |
| `SppBoardRefresh` p50 | 119.6 ms | 76.0 ms |
| `SetPlatePrinter` p50 | 191.1 ms | 146.8 ms |
| plate switch → painted | 211 ms | 156 ms |
| board drag → painted | 217 ms | 178 ms |

Exactly half the rebuilds, which is what the change predicts, and frame time untouched, which
is also what it predicts.

The unit cost underneath all of it is `PresetBundle::resolve_plate_slicing_config` at
**1.5–1.7 ms per call**. Its tier-1 sibling `resolve_plate_presets` is **0.003 ms**. The fork
already invented that narrowing and documented the rule for using it safely; it simply was not
applied everywhere, and the board is the biggest place it was not.

### Two crashes, read out of the app's own logs

Both were waiting in `datadir/log`, and both are one shape: a pointer trusted after its owner
had gone.

`crash_Thu_Aug_13_09_52_28` killed the app during startup, jumping to `0x762E7365` — an address
that is a fragment of a string, which is what a freed vtable reads like. The stack is
`ShowNetpluginTip` → `RunScript`, reached from a **queued** navigation event: `m_browser` was
non-null but its `wxWebView` had been destroyed in between, and the existing guard checks only
for null. `g_webviews` is already the exact set of live views, so liveness is now a question
with an answer rather than a pointer everyone has to trust.

The second is a latent repeat of one already fixed. `load_wipe_tower_preview` carries a guard
for the empty extruder palette that took the app down on 12 Aug; two lines below it the plate
lookup is unguarded, and `get_plate` answers NULL for an index it does not have. The wipe tower
arrives through an `obj_idx - 1000` encoding, which is precisely where an index outlives the
plate it named.

### What is not done, and why

**Culling is written and not landed.** The frustum test, the per-plate bounding box it uses, and
the hoisting of lazy resource generation out of the draw path — so a culled plate still
registers its raycaster and stays clickable — are all worked out. It recovers at most ~2 ms
while `_render_objects` costs 70 and `_render_overlays` 21. It goes in after those, or it is
polish sold as a fix.

**Startup is ~26 s to the first painted frame** and was not attacked. It is the largest single
number in the whole report and deserves its own pass, with a marker at "main frame shown" so
the number separates "the window appeared" from "the canvas drew".

## Work log — 2026-08-12, session 4 (the notification stops shouting and starts pointing)

The trigger was one screenshot: a solid red panel reading "Following objects are laid over the
boundary of plate or exceeds the height limit:", the mesh filename `R2.stl`, and a sentence
instructing the user to solve it. Four faults live in that one message, and each is a fault of
design rather than of wording:

- **It names two different problems and does not say which one this is.** Hanging over the plate
  edge and being taller than the machine prints have different fixes. Both are cheap to tell
  apart and upstream never bothered.
- **It names the object by the original author's filename**, which identifies nothing the user
  can see, and gives them no way to reach it. Naming a thing the user cannot select is worse
  than saying nothing, because it looks like help.
- **It asks for work the app can do itself.** "Move it totally on or off the plate" is what
  Arrange is for.
- **It does not survive its own commonest cause.** Point a plate at a smaller machine — the
  ordinary operation in this fork — and every object on it fails at once, so the paragraph grows
  without bound at exactly the moment it is least readable.

The severity treatment made all four worse: `render_notifications` chose the renderer from the
notification's LEVEL, so anything at error level got a full-bleed red panel however little it had
to say. Small text, red field, no action: attention without information.

### What was built

**`BuildVolume::object_state` / `volume_state_bbox` take `ignore_height`.** The same geometry
test, asked a second time with the Z ceiling lifted. Whatever it still rejects is a footprint
fault; whatever it now accepts was only ever a height fault. Exact rather than inferred, and
covered by `tests/libslic3r/test_build_volume.cpp` for both the bbox path and the mesh path.

**`check_outside_state` classifies as it detects.** `ObjectFilamentResults` gained
`objects_over_boundary` and `objects_over_height`; a collision that explains itself as neither is
still reported as a boundary fault, because an unexplained fault must never become a silent one.

**`NotificationManager::ObjectProblemNotification`** — the mechanism, not the instance. One
sentence with a count; clicking it selects the objects it is about; a fix the app can perform
itself sits beside it as a link; names and remedy live behind a More/Less toggle, each name
selecting its own object. Six names then "and N more".

**`PopNotification::uses_block_render()`** is the general repair underneath it: a notification
now chooses its own renderer instead of having one inferred from its level. That is what lets an
error be calm — a card with a red edge — without lying about its severity, and it is available to
every other notification that currently gets the red panel by default.

**One producer, two consumers.** `update_plate_fit_notifications()` turns a fit check into the
two notifications, and both the scene reload and `Plater::validate_current_plate` call it, so the
live scene and validation can no longer describe the same plate differently. The old path — a
file-static `get_object_clashed_text()` string built by `construct_error_string()`, pushed by
text and closed by matching that text — is gone, along with `EWarning::ObjectClashed`.

What the user now sees, for a plate re-pointed at a smaller machine: `7 objects hang over the
edge of the plate     Arrange   More`. One click fixes it; the click arranges THIS plate
(`PREPARE_STATE_MENU`), not the project.

### What was deliberately not done

The height problem is offered no one-click fix. Scaling or cutting a model changes what gets
made, and choosing it silently would be a yes the user never gave; the plate's actual limit is
stated instead and the choice stays theirs.

The slice button is still disabled while a plate does not fit. That gate is upstream's and it is
a separate fault — a disabled control with its explanation in a dismissable notification is a
silent gate waiting to happen. Not touched here; the notification is no longer dismissable in a
way that outlives a scene change, so the pairing is no worse than it was.

## Work log — 2026-08-12, session 3 (the board learns what it is for)

Petko's design review, in one sentence: the UI must make "one project, many machines, switch
anytime" legible, and instead the board had become a farm dashboard burying that sentence. The
fixes that followed are all restatements of it (commits `7cb1ff7bd7`, `bca2d008ee`):

- **A row IS the mapping**, drawn as pictures: the plate's render with its name beneath, an
  arrow (accent = assigned, dim = following the project default), the machine's wizard cover
  photo with its model name beneath. Renders self-heal at paint time — anything that moves an
  instance resets them (`notify_instance_update`), and only the Preview strip ever re-rendered.
- **Single click on the caption renames the plate** inline, through a new
  `Plater::rename_plate` that carries the undo snapshot the Plate Settings dialog's path never
  had. Plate naming is an agent surface over MCP; this is the human end of it.
- **The picker picks machines, not presets.** One row per printer model; the nozzle resolves
  silently (current variant → 0.4 → whatever exists) and NEVER asks. A changed nozzle is looked
  for at the inspector's Nozzle row, now a native variant menu. The popup scrolls (it could
  not, so brands past its height were unreachable) and dismisses when the app loses foreground,
  as does the hover preview.
- **What got deleted:** the Capacity tab (twin of Machine until estimates exist — its queue
  bars moved into Machine grouping), the filled bed glyphs that read as broken thumbnails, the
  em-dash hours column, the "N not estimated" scold on fresh projects, machine names repeated
  under machine headers, square swatches, "1 machines".
- **The board is the top of the sidebar, not the sidebar**: capped at four rows, scrolls
  internally. It had grown to push the filament and process sections — the actual slicer —
  off screen, which read as "the settings disappeared".

Left deliberately dormant: `build_variant_items`/`is_back` in the picker (the ask-step that was
built and then repealed the same hour — remove on next touch).

### Found live, not yet fixed: File-Save re-decorates a downloaded project

A save mid-testing (15:27:33, `export_3mf ... backup=0` in the 15:25 debug log) wrote the
load-time `"(Superdestroyer-BD.3mf)"` preset decorations back into the fixture, and the next
load decorated them AGAIN: `@BBL P2S(Superdestroyer-BD.3mf)(Superdestroyer-BD.3mf)`. Every save
of a downloaded project degrades its names one generation deeper. The load side copes (the
rename-completion resolves them), but the fix belongs on the SAVE side: strip the current
project's own decoration from every name the exporter writes — one helper, every name field.
The pristine fixture is gone (no other copy existed); the current file loads and slices, so it
stays. First item for the next session; also in the plans INDEX open threads.

The same live pass earned the day's quiet-down batch (`a76be888ad`): the relative-E validator
error deleted in favour of the slicer emitting `G92 E0` itself at layer change, the
invalid-values toast demoted to one calm line, and assignment now selecting the plate so the
new bed is visible the moment it is picked — the reported "first change does nothing" was
correct behaviour succeeding invisibly.

## Work log — 2026-08-12, session 2 (the mechanism, and the crash that was never about presets)

### The re-resolution mechanism exists

`PresetBundle::reresolve_plate_context_for_printer`, the "one missing mechanism" the previous
entry's table describes, is built, unit-tested and verified live. One path, called from both
plate-printer write paths (`set_plate_printer` and the batch variant): when a plate's printer
identity changes, a dependent that still runs on the new printer is kept, a process that cannot
run switches to the new printer's own declared `default_print_profile` (or back to inheritance
when the plate was just cleared to follow the Project row), filaments are REPORTED by name and
never substituted — material is user intent — and anything that still cannot resolve stays put
and says why. A printer this build does not have preserves the context verbatim, same as before.
It also re-records `printer_vendor_id`, which assignment alone left cleared.

Six new cases pin those rules in `test_preset_bundle_loading.cpp` (`[Reresolve]`). Live, on the
21-plate fixture: assigning plate 1 to the Creality K2 Pro logged
`process '0.20mm Standard @BBL P2S(Superdestroyer-BD.3mf)' cannot run on 'Creality K2 Pro 0.4
nozzle'; switched to '0.20mm Standard @Creality K2 Pro 0.4 nozzle' (the printer's own default
process)`, the board grew a second machine group with the K2's real 300×300 bed, and the
inspector read "Assigned to this plate".

The model-combo hard refusal (fault 3 in the table) is closed by the same reasoning at its own
site: picking a printer MODEL re-resolves the nozzle variant against that model — kept when it
carries over, the model's own single variant when not, a named choice when several exist, and a
refusal only when the model genuinely has no preset here.

### The assignment crash was never in the preset system

The previous entry's deferral said assigning a plate a working process "crashes the process
shortly afterwards, 2 of 2" and suspected the notification. Reproduced 3 of 3 — but only with
**the mouse over the 3D canvas**, which is why every human run died and every scripted run
(cursor parked elsewhere) survived. The network plugin, CallAfter timing and call ordering were
all red herrings.

**The stack was on disk the whole time.** The app installs its own `SetUnhandledExceptionFilter`
(`dev-utils/BaseException.h`) with a StackWalker that writes `crash_<date>_0.log` into
`datadir/log/` — including for yesterday's two "no WER event, no dump" crashes nobody could
diagnose. Check there FIRST for any future crash; WER LocalDumps never fires because the app's
own filter eats the exception.

The stack: `GLCanvas3D::render → _mouse_to_3d → SceneRaycaster::hit →
MeshRaycaster::closest_hit → AABBMesh::query_ray_hits`, access violation reading freed memory.
`PartPlate::set_shape` (reached from every bed-changing path: assignment, clearing, reflow via
`reposition`) resets and rebuilds all eight of the plate's `PickingModel`s, destroying the
`MeshRaycaster` objects that the canvas' registered `SceneRaycasterItem`s still point at. The
next repaint with the cursor over the canvas raycasts through the dangling pointer. Upstream
never hits this because upstream never changes a single plate's bed mid-session; the choreography
(`reload_scene` re-registers) covers only its own paths.

**Fixed at the ownership boundary, not with more choreography.** `SceneRaycasterItem`,
`PickingModel` and `GLVolume` now share ownership of their `MeshRaycaster`
(`std::shared_ptr`), so no rebuild order can dangle a registered raycaster — for plates, volumes
and every gizmo alike. An epoch-counter re-registration scheme was built first and deleted on
review: its ten-line justifying comment was the tell that it was choreography defending broken
ownership. The one-frame staleness the shared pointer leaves (old geometry answering hovers until
the scene refresh that the assignment path already triggers) is harmless and self-healing.

### Two modal gates became notifications

The "3MF was created by BambuStudio" info box and the startup "configuration file recreated"
notice were modal OK-dialogs carrying no choice. Both gated every scripted load and every human
one — for a farm fed on downloaded projects, the Bambu box fired on essentially every open. Both
now inform through the notification manager and get out of the way. The dialogs that remain on
the load path (`ProjectDropDialog`, network-plugin install) carry real choices, and the former
remembers its answer.

### The close path, one layer further down

Closing a project on a datadir whose AppConfig filament section was null (the state a config
rebuild leaves) threw `The Project row has no explicit filament preset` twice from
`full_fff_config` and terminated the app. Yesterday's close-path repair emptied the filament row
when nothing was installed, which only moved the throw. The row now floors at the filament
collection's own inert default preset — the same floor the machine collection already had.

### Scripted testing exists now, and its traps are recorded

- **`PETKOS_TEST_ASSIGN="<plate>:<preset>[:delay_ms]"`** (env var, dev hook at the tail of
  `Plater::priv::load_files`): drives the real plate-assignment write path after a project load,
  no synthetic input. The plate board is custom-painted, so no UIA tool can reach its rows; this
  is the scriptable route. The optional delay exists so a debugger can attach after startup —
  the Bambu network plugin executes garbage under a launch-time debugger (anti-debug), so attach
  late or sideline the plugin.
- The hook logs what it saw even when it does nothing. Its first version was silent unless it
  fired, which made "not in the binary", "env var missing" and "never reached" indistinguishable
  and cost a diagnosis round.
- **Modal dialogs are dismissed by posting `WM_LBUTTONDOWN/UP` to the button's own hwnd** —
  no cursor, no focus, works with other windows on top. Enumerate the dialog's children for the
  button text. In PowerShell, a delegate callback writes `$script:` scope; a FUNCTION reading its
  local copy of the same name reads nothing — dismissals silently no-op. Keep such code inline or
  read `$script:` on both ends.
- **`WindowFromPoint` before every synthetic click.** A run of clicks landed in a Chrome window
  covering the slicer; nothing in the click API fails when the target is buried.
- **Killing an orca instance risks corrupting `PetkosOrca.conf`** (it happened twice this
  session; the app writes the conf often). The corrupted-conf notice is now a notification, and
  the parse failure is logged as `parse app config ... error` at the top of the next run's log.
- The build's post-build step (`rm -rf python` + recopy beside the exe) half-fails while any
  instance runs, because `python312.dll` is mapped; the gutted folder then kills the next launch
  instantly with exit code -1 and no log. Restore from
  `deps/build/OrcaSlicer_dep/usr/local/libpython`, or close instances before building the exe
  target. The DLL-rename trick still covers the link itself.

### Verification

Release build clean. `libslic3r_tests` 185/185 (49277 assertions), `slic3rutils_tests` 73
passed / 0 failed / 33 skipped, including the six new `[Reresolve]` cases.

**Live, under the exact trigger condition (scripted assignment firing with the cursor parked
over the canvas), both scenarios survived:** a resolving assignment (K2 Pro 0.4 — process
switched, plate resolved, engine applied `printer 'Creality K2 Pro 0.4 nozzle', process
'0.20mm Standard @Creality K2 Pro 0.4 nozzle'`) and a deliberately unresolvable one (K2 Pro
0.2 — reported, no crash). The second scenario had crashed live minutes earlier through a
SECOND defect the first fix exposed: `load_wipe_tower_preview` (and `simple_render`, and the
real-tower variant) indexed the plate's extruder-color palette blind, and an unresolved plate's
palette is honestly empty. All three sites now treat an empty palette as "nothing to tint":
no tower preview, no per-material tint, geometry still renders. A day of interactive use by
Petko between the two builds also exercised the raycaster fix without a recurrence.

Two more modal info-gates found in that session became notifications: the two
`show_substitutions_info` overloads ("some values were not recognized"), which reported
already-made replacements behind a modal OK. Full replacement lists go to the log.

One true vendor-data finding, reported by the mechanism rather than fixed: `Creality K2 Pro
0.2 nozzle` declares `default_print_profile` = `0.16mm Optimal @Creality K2 Pro 0.2 nozzle`,
which does not exist in this datadir's process collection, so a plate assigned to it stays
unresolved with the reason named. Check whether the 29 July preset cleanup pruned it or the
vendor profile genuinely lacks it.

## Work log — 2026-08-12 (a downloaded project could not be opened at all, and why)

The 11 Aug entry recorded a reproducible crash on `Superdestroyer-BD.3mf` and attributed it to
`first_visible_idx()` promoting an uninstalled machine. That was a real defect and it is fixed
below, but it was not the reason the project failed. The actual reason is worse and much wider.

### The file loaded, and then the app threw its geometry away

Loading that project ended in a dialog reading **"The file does not contain any geometry data."**
The 3MF contains 49 objects across 21 plates. The message is a lie about the file, and the path
that produces it is this:

```
Plater::priv::load_files      try {
  load_config_model(...)        creates external presets named "<preset>(Superdestroyer-BD.3mf)"
  load_current_presets(...)
    force_print_bed_update()    sets printer_model = "bbl_empty" so the next branch always fires
    Plater::on_config_change("printer_model")
      Plater::set_bed_shape() const
        resolve_current_plate_slicing_config() == false
        throw Slic3r::RuntimeError(error)          <-- fork-introduced, commit 34e0c7b3dd
}                             catch (std::exception&) { show_error(...); continue; }
```

The `continue` abandons the rest of that input file, so the local `Slic3r::Model` holding all 49
objects is destroyed at the end of the loop iteration and `tolal_model_count` stays 0. The count is
honest; the load simply never happened. `force_print_bed_update()` poisons `printer_model` on
**every** project open precisely so that branch runs, so the throw fired for every project whose
plate context did not resolve.

`set_bed_shape()` is a `const` query reached from a config-change notification. It now reports and
leaves the bed as it is. This is the same rule as F7 and the two dialog throws closed on 11 Aug:
**a query and an action must not share a throw**, and here the throw did not merely kill a dialog,
it silently discarded a whole project and then told the user their file was empty.

### The resolve failure it rode on was itself spurious

The error was `Process preset '0.20mm Standard @BBL P2S(Superdestroyer-BD.3mf)' is incompatible
with printer 'Bambu Lab P2S 0.4 nozzle(Superdestroyer-BD.3mf)'` — two presets out of the same 3MF
declaring each other incompatible. The reason is a half-finished rename.

`PresetCollection::load_external_preset` renames every preset it creates from a project to
`"<original>(<project file>)"`, and each collection does it independently. The process preset's
`compatible_printers` list, restored from the 3MF's `print_compatible_printers` a few lines earlier
in `load_config_file_config`, still named the printer by its **undecorated** name. So the list had
exactly one entry and the printer was no longer it.

`load_config_file_config` now maps that one reference onto the preset the same load created from
it. This is not a compatibility fallback: it substitutes nothing and widens nothing, it finishes a
rename that was left half-done, and anything else the list names still has to match.

The error message also now names the reason rather than only the verdict — "it names 1 compatible
printers and this is not one of them" is what turned a day-old mystery into a five-minute fix.

### How wide this was

Measured across `E:\3D-Printing\Projects\Comic Con 2026`: **89 of 91 3MFs are BambuStudio-authored**
and 72 name a Bambu printer preset. This datadir has **no Bambu vendor profile at all** — `system/`
holds Anycubic, Creality, Custom, Elegoo, Flashforge, OrcaFilamentLibrary and Prusa, and nothing
else, after the 29 July preset cleanup. So the printer named by essentially every project in the
collection is not merely unticked, it does not exist, and every one of them took this path.

### Four more throws out of wx handlers, same class

Each of these fired only when a plate was unresolved, which is exactly the state a downloaded
project starts in, and none had a catch in any frame above it:

- `Selection::translate` — the wipe-tower branch, reached from
  `PartPlateList::set_default_wipe_tower_pos_for_plate` during load. It wanted a brim width for a
  margin; it now uses `WIPE_TOWER_MARGIN` and says so.
- `GLVolumeCollection::get_selection_support_normal_z` — the overhang tint, on the render path,
  reached from `Plater::select_plate`. Returns 0 (highlight nothing) and reports.
- `PartPlate::on_filament_added` — a void notification handler. Seeds the standard flow, which is
  the value the same function already substitutes for a Hybrid extruder.
- `Plater::get_selected_printer_name_in_combox` — every caller wants a name to show a user. Returns
  the plate's recorded printer name, which is the one thing a plate still knows when tier 1
  refuses to resolve it.

### AppConfig: two defects, one rule

The write end of the poisoning loop is closed, and it took two fixes rather than one.

- The printer guard tested `is_project_embedded` **alone**, and missed. A preset synthesised from a
  project's flat config gets `is_external` at `Preset.cpp:2747` and only picks up
  `is_project_embedded` a few lines later behind a filename test, so the two do not always travel
  together. `Bambu Lab P2S 0.4 nozzle(Superdestroyer-BD.3mf)` duly reached `PetkosOrca.conf`.
- Worse, the placeholder was written into `presets.machine` **before** the code decided not to
  persist its settings. Measured: a good `Flashforge AD5X 0.4 nozzle` became `Default Printer` with
  a null filament list, and every later launch started from nothing.

Both now answer to one rule: **AppConfig records the global startup selection, and neither a preset
that came from a file nor the built-in placeholder is one.** Process and filament rows get the same
treatment, keeping whatever was already recorded rather than overwriting it with a name known to
dangle.

`select_persisted_or_keep` was tested for **presence, not installedness** — `find_preset` ignores
`is_visible` while `select_preset_by_name_strict` requires it, so an unticked printer passed the
guard, failed the strict select, and deselected the collection exactly as an absent name did. It
now resolves once, requires `is_visible`, and passes the resolved preset's own name so
`renamed_from` resolution takes effect. Both limits recorded beside `3f2223996e` are closed.

### The uninstalled-machine promotion, which was the 11 Aug diagnosis

`first_visible_idx()` only ever matched `ORCA_FILAMENT_LIBRARY` presets, which no printer or
process can be, so every machine and process collection fell through to a branch that returned
`m_num_default_presets` **without testing `is_visible`** — whichever preset sorted first on disk,
installed or not. `select_preset()` then marked it visible. That is how `Anycubic 4Max Pro 0.4
nozzle` joined the machine list. It now honours its own contract, and falls back only to the inert
`- default -` preset when nothing is installed. `get_selected_preset()`'s two overloads no longer
index `m_presets` out of range, and the repair says what it kept.

### F7, one layer up

`Plater::resolve_plate_slicing_config` wrote `plate->update_apply_result_invalid(true)` from a
`const` query with more query callers than F7 had — the render path, the sidebar and the board all
resolve plates they are only describing. `m_apply_invalid` makes `can_slice()` false with nothing on
screen, so a briefly-unresolvable plate left the Slice button dead and silent. The flag belongs to
`Plater::priv::apply_plate_config`, which both sets it on a real failure with a notification naming
the plate and clears it on success.

### Build and tests

`OrcaSlicer_app_gui` builds clean. **All six test suites now build and pass**, including the three
that had never been built at all:

| Suite | Result |
|---|---|
| `libslic3r_tests` | 184 cases, **49240 assertions, pass** |
| `slic3rutils_tests` | 106 cases, 76 passed, 30 skipped, 935 assertions, **0 failed** |
| `fff_print_tests` | 85 cases, 769 assertions, **pass** (never built before) |
| `sla_print_tests` | 21 cases, 12906 assertions, **pass** (never built before) |
| `libnest2d_tests` | 21 cases, 638 assertions, **pass** (never built before) |
| `filament_group_tests` | 3 cases, 353 assertions, **pass** (never built before) |

184/184 confirms the two test fixes that were committed on 11 Aug and never compiled.
**`compare_analyzer` is not a C++ target** — the directory holds two Python scripts and is not in
`tests/CMakeLists.txt`. It cannot be built and never could be; drop it from the list of unbuilt
suites.

**Run the suites from a writable working directory.** Several write artefacts (`twospheres.obj`,
`Halfcone.obj`) into the CWD, and from a drive root the write fails and the process dies with
`0xC0000409` before Catch2 prints its summary — a fully green suite that reads as a hard crash.

### Live verification

`Superdestroyer-BD.3mf`, the 21-plate fixture that could not be opened at all: **loads**. Geometry
on every plate, all 21 rows naming their printer, filament swatches populated, plate inspector
resolving, **zero** incompatibility errors in the log, and the Slice button enabled. The remaining
notifications are true ones the project earns: two out-of-range values in the 3MF, and a relative
extruder addressing warning. The printer picker opens, lists the seven installed machines by
vendor with bed sizes, flags a smaller bed, and assignment commits.

### Two method notes, both of which produced a wrong reading first

**A stale Windows hard-error dialog sits on top of everything and silently swallows synthetic
clicks.** One crash at 00:55 left an "orca-slicer.exe - Application Error" dialog owned by the
hard-error host, not by the slicer, so it does not appear when enumerating the slicer's own
windows. Four consecutive runs were then misread as crashing when the app was alive and simply
waiting on a dialog it never received the click for. Sweep for any visible window titled
`Application Error` before believing a UI result.

**Make the capturing process per-monitor DPI aware, and the clicking one too.** A DPI-unaware
enumeration returned the dialog at `480,351 570x170` where it really was at `600,439 712x213`;
clicks computed from the first land in the dialog body and do nothing. This is the same trap the
11 Aug entry recorded for `PrintWindow`, and it applies identically to `SetCursorPos`.

### Closing the project killed the app too, and that is what corrupted the config

Found by closing the app at the end of the session rather than killing it, which is the only way
this path ever runs. `full_fff_config` threw `Filament preset 'Bambu PETG HF @BBL P1S 0.4
nozzle(Ciri Sword TW4 V1.3mf)' is not installed` and the process terminated.
`reset_project_embedded_presets` deletes the filament presets a project brought and left the
Project filament row still naming them; the loop that walks that row noticed, logged, and did
nothing. The rule for the load end of exactly this is already written above
`select_persisted_or_keep`; the close end had never been given it. It now repairs the row to the
collection's own installed selection and names every replacement.

**This is what corrupted `PetkosOrca.conf`.** The crash happened while the app was writing it, so
the next launch reported "The configuration file may be corrupted and cannot be parsed", rebuilt it
and lost the recorded printer selection. Two notes for whoever meets that state:

- The installed-model list survives a rebuild; only the selection is lost. Re-adding
  `presets.machine` by hand is enough, and the app rewrites its own checksum on the next clean exit.
- **A wrong MD5 trailer is not fatal.** `AppConfig::load` only logs it ("This may indicate a file
  corruption or a harmless user edit") and carries on; the hash covers everything up to and
  including the final `}`, and the line is `# MD5 checksum <32 UPPERCASE HEX>` on the following
  line. The corrupted-config dialog comes from a JSON parse failure, not from the checksum.

**A real defect found on the way, not fixed, and it blocks a first-run user.** Picking a printer
*model* from the sidebar combo matches candidates on `printer_model` **and** the currently edited
preset's `printer_variant` (`Plater.cpp` ~11010). From `Default Printer`, whose variant matches no
system preset, the search finds nothing and refuses with "No installed printer preset exactly
matches this model and the current nozzle variant" — so from a rebuilt config you cannot select any
printer from the combo at all. That is a hard refusal to a definite request, which this fork's own
rules forbid. It should fall back to the model's own variants and, where there is more than one,
name them rather than refuse.

### Deferred, stated as a deferral

**Giving a plate a process its new machine can run is written, works, and crashes — so it is not in
this build.** Assigning a plate a printer leaves the plate's process as the one the project shipped
with, which genuinely cannot run on another machine, so the plate resolves to nothing until the
process is changed by hand. A `switch_plate_process_for_printer` that takes the printer preset's own
`default_print_profile` was implemented and **verified working** — the log reads `plate 1: process
'0.20mm Standard @BBL P2S(Superdestroyer-BD.3mf)' cannot run on 'Creality K2 Pro 0.4 nozzle';
switched to '0.20mm Standard @Creality K2 Pro 0.4 nozzle' (the printer's own default process)` and
the plate then resolved with zero errors. But the assignment crashes the process shortly afterwards,
2 of 2, and neither deferring the notification through `CallAfter` nor moving the call after
`apply_printer_to_plate` fixed it. It was removed rather than shipped unverified. The next session
should reinstate it behind a proper diagnosis: there is no debugger on this machine, so start by
installing one or by narrowing with a headless test that calls `set_plate_printer` without the
picker in the frame.

## Work log — 2026-08-10 (in progress, not a final architecture record)

This entry records what was changed during the current refactor so another agent can continue
from evidence rather than infer intent from the diff. It does not mark the audit, design or
implementation as complete.

- Added `PlateSlicingContext` for the exact per-plate printer preset/vendor, process preset,
  filament preset list and physical-printer id. Empty slicing fields mean explicit inheritance
  from the Project row; a non-empty value is resolved by exact name and is never remapped.
- Added `PresetBundle::resolve_plate_slicing_config`. It composes an FDM-only effective config and
  rejects missing/incompatible printer, process and filament identities. Major slice, geometry,
  export, send, device and multi-nozzle consumers were moved to this resolver during this round.
- Stored the context in 3MF plate metadata and in the undo/redo plate state. The physical printer
  remains distinct from the slicing preset.
- Added a retained full slicing-config snapshot to each completed plate slice. The snapshot is
  written as `plater_sliced_config:<option>` metadata alongside the plate's retained G-code and is
  read strictly, with substitutions disabled.
- Changed context-only printer/filament updates to keep the previous G-code attached to the plate.
  `is_slice_result_valid()` compares the retained snapshot with the live exact config, so the old
  file is stale and cannot be dispatched under a different context but is not discarded merely by
  switching context. Ordinary model/geometry invalidation still clears slice validity.
- Replaced the active AMS synchronization path. It now resolves tray `filament_id` values exactly
  against the current plate's printer and process, rejects zero or ambiguous matches, and writes
  filament names/colours only into that plate's context/config. It no longer edits
  `PresetBundle::filament_presets`, `project_config`, the Project process, or Project selections.
- Removed the old AMS implementation that substituted Generic/type-similar/previous/random
  filaments, used index-based colour mapping, appended Project filaments, and merged global
  filament rows. A dirty edited filament now blocks sync with an explicit message instead of being
  silently reset.
- Current direct AMS sync requires the number of loaded, exactly identified trays to equal the
  plate's current filament-row count. Mapping mode changes only explicitly mapped plate rows. This
  is the current implemented boundary; it is recorded here for follow-up and is not declared the
  final UX for plate-local filament-count changes.
- Removed silent per-plate bed-type reset when the current Project printer did not support a plate
  override. Unsupported or unresolved plate state is now surfaced instead of rewritten.
- Added focused tests for exact plate-context resolution, context round-trip in 3MF, and distinct
  retained per-plate sliced configs. Verification is still in progress in this work log; consult
  the newest build/test entry before assuming these changes pass.

### Pause checkpoint — 2026-08-10 (work performed, not final or settled)

This checkpoint records the additional work performed before the user-requested pause. It does
not claim that the repository-wide audit, implementation, build, or tests are complete.

- Removed project-load coupling to the Monitor's selected machine. Opening a project no longer
  prompts for or rewrites the Project printer from the device-page focus.
- Made Project-printer multi-extruder synchronization affect only a current plate that explicitly
  inherits the Project printer, and resolve that plate's physical device by its stored id.
- Tightened Project inheritance resolution: an unresolved Project printer or process selection is
  now an error instead of permission to reuse an edited preset left in memory.
- Reworked compatibility refresh so it updates compatibility flags without replacing printer,
  process, or filament selections with a first/preferred compatible preset.
- Made Plate Settings confirmation and batch printer assignment retain the event's explicit plate
  index. Invalid members abort a batch before any assignment is made.
- Added an exact per-plate export-filename path and moved normal Bambu send, multi-machine send,
  send-to-printer, AMS-sync, thumbnail, statistics, filament/nozzle mapping, and filename consumers
  away from current/all-plate substitution. Normal send dialogs now require one explicit plate.
- Changed normal print jobs to reject negative/current/all plate sentinels. LAN verification now
  uses the explicitly selected transport and stops on failure instead of retrying through another
  transport.
- Made normal send-dialog machine operations use the dialog's exact device id rather than the
  Monitor's selected machine. The two remaining selected-machine reads in that dialog are confined
  to the SD-card/device-page entry path, where device focus is the source of the operation.
- Removed normal-path project-wide dispatch from the send and AMS dialogs. Retained plate slices
  remain the unit being inspected and sent; this work did not add parallel slice-all dispatch.

#### Verification state at this pause

- `git diff --check` still reports no whitespace errors (only the working tree's LF/CRLF warnings).
- A serial Release build before these latest edits compiled the source target and reached linking,
  then failed with `LNK1104` because `build/src/Release/OrcaSlicer.dll` could not be opened.
- A later compiler checkpoint (`m1e`) was deliberately treated as intermediate because edits had
  occurred while it was running. At the pause its processes were no longer present and both log
  files were zero bytes, so it supplies no pass/fail evidence. A fresh serial build is required.
- Focused `[PlateContext]` and `[RetainedGcode]` tests have not yet been built or run after these
  changes.

#### Audit findings recorded at the pause — all seven answered, verdicts added 2026-08-11

**This list is closed. It is kept because the findings are still the clearest statement of what was
wrong, but none of it is outstanding work.** Each verdict below was established by reading the tree,
not by trusting the commit that claimed the fix. Six were repaired; one was answered deliberately in
the opposite direction and must not be reopened.

- `Sidebar::priv::layout_printer` is passed Bambu-network capability where it expects vendor
  identity, and dual-extruder layout is restricted to Bambu. The bed-visibility check also ignores
  its `isBBL` argument and rereads the Project preset.
  → **Closed.** The machine-dependent half is now `layout_printer_machine(bool isDual, const
  DynamicPrintConfig *printer_cfg)`, which reads the plate config it is handed and returns early on
  `nullptr` rather than substituting the Project machine. Dual-extruder layout carries no vendor
  test. A **third defect of the same shape**, found on 11 Aug and not in the original wording, is
  also closed: the two network buttons were still decided from the Project printer by a second
  writer that ran after the first, so a Bambu plate in an Elegoo project showed the wrong pair after
  any preset refresh. Both `Show()` calls moved beside the bed control.
- Preview setup still sizes extruder parameters from the Project filament list instead of the
  current plate's resolved config. → **Closed.** The Project-list initialiser is gone; an
  unresolved plate now leaves the previous extruder parameters in place instead of applying the
  Project count.
- The legacy `SendJob` and the legacy send/export helpers still accept current/all sentinels after
  launch. Their UI callers should resolve the current plate once and pass its concrete index.
  → **Closed.** `SendJob::process` and `send_gcode_legacy` reject a sentinel and resolve the target
  plate's own context; the event boundary resolves the current plate once. The **declarations** kept
  their `= -1` defaults and their now-false doc comment, and `set_print_job_plate_idx` still
  performed the `PLATE_CURRENT_IDX` substitution before the reject; that residue was removed on
  11 Aug.
- `PlateSettingsDialog` is constructed before the event's target plate is retrieved, so its bed and
  vendor controls can still be based on whichever plate was current. → **Closed.** The dialog takes
  the index and builds its bed-type list from that plate. Its enable/disable decision was still
  reading the *current* plate and **threw out of the constructor** when that plate was unresolved,
  which killed the application on the way into the one editor that can repair an unresolved plate.
  Fixed 11 Aug: it resolves its own plate and reports rather than throws.
- The main Print action still chooses Bambu-network versus print-host routing from the Project
  printer, and the main button's default action can remain stale after switching to a plate with a
  different printer. → **Closed.** The stale-default half was already fixed; the routing half was
  half-converted, with a plate-derived label over a Project-derived handler, so the button could
  name one network and dispatch to the other. `on_action_print_plate` and both `MainFrame` routing
  sites now read the current plate's resolved printer.
- GL-canvas nozzle/filament compatibility still passes the Project filament-preset list even though
  the print config is plate-local. → **Closed.** It reads the plate config's own
  `filament_settings_id`; the unresolved branch clears the warnings rather than substituting.
- The normal Bambu dialog still contains permissive branches for missing printer-model data and
  timelapse-storage checks that time out or fail. → **Split, and one half deliberately refused.**
  `is_same_printer_model` no longer answers "yes" when it cannot tell, and the caller turns the
  unknown into a confirm-before-send warning. The **timelapse check keeps proceeding**, and says so
  in a notification naming the consequence. An unanswered storage query is a maybe, a maybe must not
  block a print, and the cost of being wrong is an overwritten old video rather than a damaged
  machine. The defect was the silence. Do not re-raise this as a no-fallback violation.

## Work log — 2026-08-10, session 2 (the printer panel; in progress)

Continues the entry above. The pause checkpoint left the tree uncompilable, so this session's
first job was the build, and the second was the planned sidebar UI.

### The tree did not compile, and now does

Three defects in the previous round's edits, in eight places:

- `plate->get_slice_result()->print_statistics` is a `PrintEstimatedStatistics`, which has no
  `total_weight`. The old code read weight from `get_current_fff_print().print_statistics()`, a
  different type from a different object, and the refactor merged the two reads into one. Fixed by
  `PartPlate::get_retained_print_statistics`, which takes time and weight from the plate's own
  `GCodeResult` and recomposes weight from its per-filament volumes and densities. That figure
  survives a reopen; the `Print` object's does not, because a `Print` carries whichever plate was
  sliced last.
- `DynamicPrintConfig::get_filament_type(std::string&, int)` was non-const, so a resolved plate
  context, which hands out const presets, could not name its own filament types. It only reads, so
  it is const now.
- `PartPlateList::get_plate` had no const overload, so a const caller could not name a plate. Added
  one rather than casting the constness away at the call site.

### The board

New `src/slic3r/GUI/PlateBoard.{hpp,cpp}`, kept out of `Plater.cpp` deliberately: that file is
20k lines and the fork's largest rebase surface.

- `PlateBoardModel` is a pure function of `PartPlateList` plus `PresetBundle`. It resolves bed sizes
  from the preset collection directly, never through `PartPlateList::resolve_printer_bed`, which
  bails without a wxApp instance and so cannot run headless.
- `PlateBoard` draws the rollup and one row per plate: index, proportional bed glyph, printer name,
  hours from the plate's own retained slice, part count, state. It stores no selection and routes
  every click through `Plater::select_plate`.
- `PlatePrinterPopup` is a plain list, not a `PlaterPresetComboBox` subclass. Every preset combo in
  this application mutates the global bundle and `PlaterPresetComboBox::update()` takes no argument
  saying which preset to show, so a subclass would tick the globally selected printer and one leak
  would re-slice other plates. It commits through `Plater::set_plate_printer`, the one write path.
- The picker's one bulk action, `Also assign to every unassigned plate (N)`, is a **modifier on the
  next pick** rather than an action of its own, because a bulk assignment still has to be told which
  machine. It names its count, so the blast radius is on screen before the click, and it commits
  through `set_plate_printers`, which takes one snapshot for the batch. There is deliberately no
  action that retargets already-assigned plates.
- Selection now notifies from inside `PartPlateList::select_plate`. The two construct-and-call sites
  in `Plater::select_plate` and `select_plate_by_hover_id` are deleted; the commented `wxQueueEvent`
  stays commented. Paths that previously notified nothing (init, delete_plate, select_plate_by_obj,
  add-plate, move-to-front) are now covered.
- The collapsed printer-section title comes from `Sidebar::printer_summary_text()`.

**Deviation from the plan, stated so it is not mistaken for an oversight.** The plan reparents the
project printer combo into a Project row drawn by the board. The combo stays in its existing panel,
which sits directly above the board and already is the project printer with global semantics. Every
guarantee the plan asks of the Project row holds: pinned, never scrolling away, the only door to the
project printer. What is not built is the drawing of it inside the board control.

**Also deferred, and not started:** stages 4 and 5 (the pinned inspector, multi-plate scopes,
grouping modes, drag), the bulk `Assign to every unassigned plate` footer, and the deletion of the
in-canvas plate strip. The strip is left in place: deleting it at this stage would remove the
thumbnails with only a hover preview to replace them, and that is open thread 3 in the plan.

> **Superseded.** Stages 4 and 5 landed in `907d7604a2` and `f58b26ea3f`; the hover preview and drag
> landed on 2026-08-11. The bulk footer named here **was already built** in this very session, as a
> modifier on the next pick — the paragraph two bullets above says so, and this list contradicts it.
> The strip is still in place, but for a better reason than the one given here: see the 2026-08-11
> entry.

### Bugfixes from the queued list above

- `layout_printer`'s first argument is named `has_bbl_network` and gates only the two Bambu network
  buttons. Dual-extruder layout no longer requires a Bambu vendor, which had hidden the second
  extruder from every non-Bambu dual machine in this fleet.
- `PresetBundle` gained `get_vendor_type`, `is_bbl_vendor`, `use_bbl_network` and `use_bbl_device_tab`
  overloads taking one printer config. The main Print button's routing now comes from the current
  plate's resolved printer, and re-decides when a plate switch changes the machine, not on every
  click.
- Preview extruder parameters come from the current plate's resolved filament list.
- `PlateSettingsDialog` takes the target plate index and builds its bed-type choices from that
  plate's machine. It was constructed before the event's plate was retrieved.
- GL-canvas nozzle/filament compatibility uses the plate config's own `filament_settings_id`.
- `SendJob`, `send_gcode_legacy`, `send_gcode` and `export_config_3mf` reject a sentinel index
  instead of substituting the current plate. `on_action_send_gcode` resolves the current plate at the
  event boundary.
- `SelectMachineDialog::is_same_printer_model` no longer answers "yes" when it cannot tell. All
  three early returns (no device manager, unresolvable machine, no preset bundle) returned true, so
  a missing fact read as "same model" and the mismatch warning never fired. The caller turns false
  into a confirm-before-send warning rather than a refusal, so an unknown answer now costs one
  dialog instead of letting one machine's G-code reach another.

**One queued item was answered differently from the way it was queued, deliberately.** The timelapse
storage check that times out or fails now proceeds AND says so, through a warning notification. It
does not become a hard error. An unanswered storage query is a maybe, and a maybe must not block a
print the user asked for; the cost of being wrong is an overwritten old video, not a damaged
machine. The defect was that it proceeded silently, and that is what changed.

### A startup crash that had been waiting behind the compile errors

The first successful build crashed on launch with an access violation, before the main window,
inside `Sidebar::reset_bed_type_combox_choices` → `Plater::get_curr_printer_model`.

**`Plater::p` is null for the whole of `Sidebar`'s construction.** `Plater::Plater` initialises
`p(new priv(this, main_frame))`, and `priv` constructs the `Sidebar` in its own member-init list,
so the unique_ptr is not assigned until that constructor returns. Any `Plater` method the sidebar
calls in that window that touches `p` dereferences null.

This was not introduced by the board. The previous round rewrote `get_curr_printer_model` to go
through `resolve_current_plate_slicing_config`, which reads `p->partplate_list`; it had simply
never run, because the tree had not compiled since. Guarded by `Plater::is_initialized()`, which
now gates `get_curr_printer_model`, `get_plate_printer_model`,
`resolve_current_plate_slicing_config`, `notify_plate_selection_changed`,
`Sidebar::refresh_plate_board` and `PlateBoard::reload`. Every caller already handled the failure:
a null printer model makes the bed-type combo list all types, and it is rebuilt on the first
preset update.

**The obvious spelling of that guard is wrong, and it crashed in a third place before this was
understood.** `is_initialized()` cannot be `p != nullptr`. While `p(new priv(this, main_frame))` is
being evaluated, `p` is not a null unique_ptr, it is **raw uninitialised storage**: the test reads
whatever was on the stack, can answer yes, and then hands out a dangling `priv`. That is what took
down `MenuFactory::init` → `Plater::get_filament_colors_render_info`, one frame deeper than the
sidebar and past a guard that looked correct.

The guard is therefore a `bool m_priv_ready` **declared before `p`**, so its default initialiser has
already run by the time `priv`'s constructor calls back, and set to true in Plater's constructor
body. Do not "simplify" it back to a pointer test, and do not move the declaration.

**If you add anything reachable from Plater::priv's construction — the sidebar, the menu factory,
anything they touch — it must ask `is_initialized()` first.**

### And the other half: `wxGetApp().plater()` is null through all of it

`MenuFactory::init` is called from `Plater::priv`'s constructor, and `GUI_App::plater_` is only
assigned after `new Plater(...)` returns, so every `wxGetApp().plater()->…` on that path
dereferences null.

This used to be harmless by accident. `get_filament_colors_render_info`,
`get_filament_color_render_type` and their neighbours read
`wxGetApp().preset_bundle->project_config` and touched no member of `this`, so calling them through
a null pointer never faulted. Making filament colour a property of the **current plate** made them
read `this`, and the same call started faulting at null + the offset of the first member touched.

Guarded at the call sites, which is where the wrong assumption lives:
`get_extruder_color_icons` (wxExtensions), `PlaterPresetComboBox::get_cur_color_info`, and
`MenuFactory::append_menu_items_flush_options`. Nothing is lost by returning empty: the filament
action menu rebuilds its icons on every open, and the combos repaint on the first update.

**The general shape, worth checking for whenever a Plater method is made plate-aware:** a method
that only read globals could be called on a null plater and nobody noticed. The moment it reads
`this`, every such call site becomes a crash. Grep the call sites, not just the method.

### The structural repair, instead of one guard per crash

`menus.init(main_frame)` was moved out of `Plater::priv`'s constructor into `Plater`'s constructor
body, which now also assigns `wxGetApp().plater_ = this` before calling it. That is where the menu
factory belongs: it reads the current plate, so it needs a plater that exists, is registered with
the app, and is past its own construction. Nothing between the old call site and the end of priv's
constructor uses the menus. Guards remain at the individual call sites as well, because the sidebar
is still built from inside priv.

### `ConfigManipulation`'s printer config was captured before the bundle existed

Sixth startup crash, past MainFrame this time: `GUI_App::load_current_presets` → `TabPrint::update`
→ `ConfigManipulation::update_print_fff_config` → `DynamicConfig::opt_float`.

`Tab::get_config_manipulation()` calls `set_printer_config(&m_preset_bundle->printers.…)`, and it
runs at line 176 of `Tab`'s **constructor** while `m_preset_bundle` is assigned at line 212, inside
`create_preset_tab()`. The stored pointer was therefore derived from an unset bundle, and the first
read through it faulted. It now reads `wxGetApp().preset_bundle`, which exists before any tab.

The refactor replaced a by-value `auto gpreset = …get_edited_preset()` read with a pointer captured
once, which is the right direction but moved the read earlier than the data.

**Note for anyone debugging a similar fault:** `DynamicConfig::opt_float(key, idx) const` ends in
`return 0;` from a function returning `const double&`, i.e. a reference to a temporary. Upstream
bug. Any read of a key that might be absent is therefore a crash rather than a zero, so check the
option pointer instead of trusting the accessor.

### The second startup landmine, same window, different shape

With the null dereference guarded, the next launch died on
`Unhandled standard exception of type "class Slic3r::RuntimeError" with message "No plate is
selected for flushing-volume editing"; terminating the application.`

`Sidebar`'s constructor builds the Flushing-volumes button and immediately calls
`is_flush_config_modified()` to decide whether it wears a "modified" badge. That query shared
`current_plate_config_or_throw()` with the flush **editing** path, so during construction, where
there is no current plate, asking whether a badge was needed killed the application.

**A query and an action must not share a throw.** Editing flush volumes with no plate is a real
error. Wondering whether they differ from default is not: the honest answer is "not as far as
anything knows yet", and there are already four later call sites that re-apply the badge. Only
`is_flush_config_modified()` changed; the editing entry points still throw.

The three plate-dependent calls in `Sidebar`'s constructor are `reset_bed_type_combox_choices`,
`is_flush_config_modified` and `update_all_preset_comboboxes`. All three are now safe at that
point. Anything added there is a fourth.

### Verification state

**Superseded on 2026-08-11. The two statements below that are now false are the last two bullets:
the board HAS been seen working on a multi-plate project, and the focused tests HAVE been built and
run. `BUILD_TESTS` is `ON`. The bullets are left standing because this log records the sequence of
evidence rather than its conclusion; read the 2026-08-11 entry for the current state.**

- Release build of `libslic3r` and `libslic3r_gui`: **clean, 0 errors.**
- **The application starts.** Launched with no project: main window up, responding, no crash log.
  The only errors it logs are two pre-existing `PartPlate::calc_exclude_triangles: Unable to create
  exclude triangles` lines.
- Six startup crashes were fixed to get there, in this order. Each was hidden behind the one before
  it, and none came from the new UI:
  1. `Sidebar::reset_bed_type_combox_choices` → `get_curr_printer_model` reading `p`
  2. `is_flush_config_modified`, a query sharing a throw with the flush editing path
  3. the first `is_initialized()` spelling, `p != nullptr`, reading uninitialised storage
  4. `MenuFactory::init` → `get_filament_colors_render_info` on a null `wxGetApp().plater()`
  5. `Sidebar::should_show_SEMM_buttons`, same null plater
  6. `ConfigManipulation`'s printer config captured before `Tab::m_preset_bundle` was assigned
- **Not yet verified: opening a multi-plate project, and the board rendering.** The board is hidden
  at one plate by design, so a single-plate launch proves startup and nothing about the panel.
- Focused `[PlateContext]` / `[RetainedGcode]` tests: **still not run.** The build directory has
  `BUILD_TESTS:BOOL=OFF`, which is why the previous round could not run them either. Catch2 is
  vendored at `tests/catch2`, so turning it on needs no new dependency.

## Work log — 2026-08-10, session 3 (stage 4, and an audit that killed its own suggestions)

Commit `907d7604a2`. Nine agents audited the tree and continued the UI. Thirteen findings,
**seven real and six refuted.**

### Stage 4 of the printer panel

- The pinned inspector, with its `PLATE nn` / `N PLATES` / `PROJECT` scopes stated in a badge rather
  than inferred from what was last clicked.
- `Sidebar::m_scoped_plates`, with its invariant — the current plate is always a member and the set
  is never empty — enforced in **one** function. Two enforcement sites is how a scope set comes to
  disagree with `PartPlateList` about what is selected.
- Ctrl/shift-click to extend the scope, and upward resolution from an object selection.
- Per-plate bed type, using the dialog's verbatim inheritance strings. Inventing a second vocabulary
  for the same fact is how two controls come to describe one state differently.
- Read-only nozzle, filament-usage and mapping rows: none of those three is per-plate storage, and
  an editable-looking control over storage that does not exist is a lie the moment it is clicked.
- `More plate settings` routes through `EVT_OPEN_PLATESETTINGSDIALOG` rather than constructing the
  dialog, so the dialog keeps the sixty lines of syncing `Plater` already does for it.

### Six findings refuted, recorded as refuted

Four of the six were raised against the `PartPlate` filament checks (`check_tpu_printable_status`,
`check_filament_printable`, `check_mixture_of_pla_and_petg`,
`check_compatible_of_nozzle_and_filament`) and all four assumed those checks could be reached with a
partially populated config. They cannot: every caller passes a config produced by
`construct_full_config`, whose first statement applies `FullPrintConfig::defaults()`, so the keys
the findings assumed absent are always present. The premise held only of the old background-`Print`
path, which F5 removed in the same commit. **Re-checked at HEAD on 11 Aug and still refuted.**

Those four were the items proposed *before* the audit ran. The adversarial pass earned its keep by
killing the coordinator's own suggestions, which is the outcome to expect from one rather than the
surprising one.

### The seven that were real

- **F7 — a `const` query wrote state.** `resolve_plate_context` set `m_apply_invalid`, one writer
  and no clearer, and the query runs over non-current plates from the render loop. So a plate that
  was briefly unresolvable kept a sticky failure and a `SLICE_FAILED` thumbnail long after the cause
  was gone. `render_logo` had the same write. Both now only report, and the flag is owned by the
  action path that can clear it. A per-frame error log went with them.
- **F8 — retained-slice validity compared the wrong two things:** composed config against
  engine-transformed config. The engine rewrites 135 real user settings, so excusing those keys as
  noise would have meant a process-preset edit stopped invalidating a retained slice. Now composed
  against composed. It also fixes the CLI, which tested the snapshot before the headless early-out
  and was therefore told every plate it had just sliced was invalid.
- **F5 — `validate_current_plate` read the background `Print`'s config**, which belongs to whichever
  plate the background process happens to hold and can be a different plate entirely.
- **The retained slice never survived a reopen.** `_load_model_from_file` parses plate metadata into
  `m_plater_data` and copies about 25 fields out to the caller; `sliced_config` was not one of them,
  so every reopened plate got an empty snapshot and read as stale. Two further exactness defects on
  the same path: values were escaped with `xml_escape`, so newline, tab and CR normalised to spaces
  on read, and reading through `set_deserialize` ran preset migration over a machine-written
  snapshot.
- **A retained-slice key this build cannot parse** now drops that plate's snapshot, records the
  reason and keeps the project, instead of failing the whole 3MF load.
- **Opening a project poisoned the app config.** `export_selections` persisted the transient
  project-embedded preset names, closing the project deleted those presets, and the next launch threw
  from `full_fff_config` during `MainFrame::init_tabpanel`. A project-embedded printer is no longer
  written to AppConfig.
- The AppConfig keeper, which became its own commit and is recorded below.

**The focused tests ran here for the first time**: `[PlateContext]` and `[RetainedGcode]`, 60
assertions in 3 cases, green. `BUILD_TESTS` had been `OFF` in the build directory, which is why the
two previous rounds could not run them and why their work logs could not say whether they passed.

### `3f2223996e` — a stale name must not deselect a valid preset

`select_preset_by_name_strict` does not leave a failed selection alone: on a miss it sets
`m_idx_selected` to `(size_t)-1`, deselecting the collection. A stale name in AppConfig therefore
did not merely fail to be honoured, it **destroyed the valid selection `load_presets` had already
made**, and the resulting empty preset name reached `full_fff_config` and killed the application
about 48 seconds into launch. A persisted name is now selected only when it names an installed
preset; otherwise the collection keeps what it has and says which selection it kept.

Not a fallback: there is no valid name being substituted for, and what stays is the collection's own
installed selection rather than a guess at what the user meant.

**Two limits found on 11 Aug, recorded beside the fix so they are not mistaken for new defects in
it.** Both are open.

- The guard tests **presence, not installedness.** `find_preset` matches on canonical name and
  ignores `is_visible`; `select_preset_by_name_strict` requires `is_visible`. Every system preset
  from a loaded vendor profile stays in the collection merely marked invisible, so a printer the user
  unticked in the Configuration Wizard passes the guard, fails the strict select, and deselects the
  collection exactly as before. Fix: resolve the preset once, require `p->is_visible`, and pass
  `p->name` so the `renamed_from` resolution actually takes effect.
- What it keeps for a poisoned config is `Default Printer` / `Default Setting`, the internal
  placeholders. The app reaches its main window, which is what the commit claims and all it claims.
  But every plate that inherits then inherits a placeholder, and the poisoning loop is still live at
  the other end: see the AppConfig finding in the 11 Aug entry.

## Work log — 2026-08-10, session 4 (stage 5, the render path, and paste that arranges)

Commit `f58b26ea3f`. Five agents on disjoint files: two explorations that edited nothing, three
deliverables.

### Stage 5

All four grouping modes with sticky collapsible headers, proportional queue bars in the by-capacity
headers, compact 22 px rows inside groups over eight plates, and the n>8 default switch held as
**"not explicit"** so a project that grows past eight regroups itself. Grouping state is per project
and resets on a project change, rather than leaving a large project's grouping on a small one.

### F6 — the render path was composing a full print config per plate per frame

The 3D editor reached `plate_uses_dual_bbl` through `render_icons`, which the original finding
missed and which is the largest site: **36 config compositions per frame** at the plate bound, each
roughly a thousand heap allocations. That is now a printer-preset lookup and one option read,
short-circuited on vendor first, so the editor composes none. `render_logo` and `get_real_print_seq`
likewise. In the plate toolbar, three validity calls per sliced plate per frame became one, hoisted
out of the loop, and `is_slice_result_valid`'s two-way `diff()` became `equals()` — identical
semantics, because `ConfigBase::diff` and `equals` both iterate only the intersection of the key
sets, and no key vector is allocated.

The short circuit was re-derived from scratch on 11 Aug and is value-identical to the composed
answer: `nozzle_diameter` and `print_sequence` appear in none of the option lists
`construct_full_config` writes, so the composed value always equals the preset's.

### Paste arranges instead of stacking

A copy lands on the **current** plate, packed into the free space around the objects already there,
which are handed to the nester as fixed obstacles and never moved: the user placed those
deliberately. The bed and exclusion areas come from that plate, not the project, because in this
fork every plate has its own. Final alignment is disabled when the plate is occupied, or libnest2d
translates the preloaded items too and the copies land shifted off the very things they were packed
around. Anything that does not fit is laid out in the row in front of the plate and named in one
notification: nothing is refused and nothing is dropped.

Those three claims were verified independently on 11 Aug at the libnest2d level: fixed items are
`markAsFixedInBin` and routed to `fixed_bins` rather than `store_`, so no existing `ModelInstance` is
written at all; `do_final_align` is genuinely off and the `USER_DEFINED` override cannot re-arm it;
and every leftover increments `unplaced` and inserts its object name *before* the contour test, so a
part with no usable footprint is still named.

### The debt this commit declared, and what it actually was

The commit body says the plate-inheritance rule now exists in three places. A read on 11 Aug found
**four**: `PlateBoardModel::build` writes it inline as well, and had diverged from the other three
rather than merely duplicating them — it reads the *saved* selected preset instead of the edited
one, applies neither the ptFFF gate nor the recorded-vendor check, and therefore renders a blank
machine name in every inherited row where the other three raise an explicit error.

**Declared debt is not measured debt.** Three of the four sites are folded below; the fourth is
deferred with a reason.

## Work log — 2026-08-11 (the fold, the board finished, and the first build all three agents met in)

Six agents: two read-only audits, three implementers on disjoint files, one build. Everything below
was verified against the tree rather than taken from the commit that claimed it, which is how the
four-not-three finding above was made.

### The plate-inheritance rule has one home

New tier-1 entry point on `PresetBundle`, declared above the composing resolver:

```cpp
struct ResolvedPlatePresets { const Preset *printer, *print; std::string printer_vendor_id; bool is_bbl_printer; };
bool resolve_plate_presets(const PlateSlicingContext&, ResolvedPlatePresets&, std::string &error) const;
const ConfigOption *plate_process_option(const PlateSlicingContext&, const DynamicPrintConfig *plate_overrides,
                                         const std::string &opt_key) const;
```

Tier 1 owns the whole rule and nothing else: an empty field is explicit inheritance from the Project
row, a named preset resolves by exact name and is never remapped, the printer must be ptFFF with a
non-empty `nozzle_diameter`, and a recorded vendor id must still match. It composes nothing,
allocates no preset copy, writes no state and throws nothing. `resolve_plate_slicing_config` now
calls it and keeps only compatibility, the filament list and `construct_full_config`; the two
`PartPlate.cpp` statics are gone, replaced by one thin GUI adapter that adds only the no-wxApp
availability check; and `Plater::get_plate_process_option` forwards to `plate_process_option`.

**Why this had to happen rather than being logged as tidiness:** the copies had already diverged in
behaviour. Two checked neither the nozzle definition nor the recorded vendor, and the two process
copies checked no printer at all, so a plate the slicer calls unresolved still handed a process value
back to the sidebar. Four answers to one question is four chances to disagree, and they were already
taking them.

**The fold widened a latent defect, and writing the test for it is what found it.**
`PartPlate::get_real_print_seq` returned `ByDefault` on an unresolved context, discarding the plate's
**own** `print_sequence` override, which needs no preset to read. That was latent before, because the
old copy resolved only the process preset; the fold requires the printer to resolve too, so every
plate whose printer preset is not installed hit it — and the picker deliberately supports exactly
that state. `ArrangeJob` turns this value into `params.is_seq_print`, so a plate the user had set to
ByObject would have been packed without extruder clearance, silently. It now reads the plate's own
value first and falls through to the process preset only when the plate said nothing, matching how
`plate_process_option` treats a plate override.

### The board's two unfinished affordances

- **Hover thumbnail on a row, and the refresh owner that made it possible.**
  `GLCanvas3D::_update_imgui_select_plate_toolbar` was the **only** consumer of
  `Plater::is_plate_toolbar_image_dirty()` — that is, the in-canvas strip was the only thing that
  ever refreshed `PartPlate::thumbnail_data`. Nine producers mark the flag and without the strip
  nobody clears it. New `GLCanvas3D::refresh_plate_thumbnail(int)` carries the guarantee the board
  cannot make for itself: `render_thumbnail` issues GL directly and is correct only while a context
  is current, and the board asks from a wx timer where nothing has just rendered. `_set_current` and
  `_is_shown_on_screen` are private, so this belongs in the canvas rather than at the call site. It
  renders one image, not the strip's two per plate, and does **not** consume the dirty flag, which
  also drives the strip's own item rebuild.
- **Drag to a group header, finished.** Autoscroll was motion-driven, so holding the pointer against
  the edge produced nothing and a group scrolled out of view was **unreachable while the button was
  held** — a drag that cannot be completed. It is now clock-driven and recomputes the drop target
  each tick, because no motion event is coming. The drag pill was clipped to the scroll region, so it
  vanished exactly when the pointer was furthest from a target; it is now drawn after
  `DestroyClippingRegion` and clamped into the control. A press in Plate-order or By-material is
  recorded and, past the slop, **answers with where the gesture works**, once per grouping mode:
  previously it did nothing and said nothing, which is this fork's definition of a silent gate.

Two smaller repairs of the same character. The board's notifications moved from `BBLPlateInfo` to
`CustomNotification`, because `NotificationManager::set_in_preview` hides every `BBLPlateInfo` while
the Preview tab is up and the board lives in the sidebar, which is up on both tabs — a message that
vanishes on one tab is the silence it was written to replace. And the hover preview now hides on
`wxEVT_SHOW`, because the sidebar hides the board below two plates and a hidden parent does not hide
a top-level popup.

**The rollup counted kinds of assignment, not machines.** `machines` was
`assigned_machines.size() + (any_inherited ? 1 : 0)`, and the picker lists the project printer as an
ordinary entry, so explicitly assigning one plate to the preset the Project row already names — one
click — counted a single physical machine twice. It is now a distinct-name set, and `summary_text`
uses `_L_PLURAL` so one machine no longer reads "1 machines".

### The last of the queued audit list, and two throws that killed the application

Two uncaught `Slic3r::RuntimeError`s were thrown out of wx event handlers.
`generic_exception_handle` rethrows `std::exception`, so `OnExceptionInMainLoop` returns false and
the process terminates. Both fired **exactly when a plate's context was unresolved**, which is to say
in the two places a user goes to diagnose or repair one:

- `SelectMachineDialog::prepare`, reached by pressing Print. It now returns `bool`, shows the error
  and returns false; `SendToPrinterDialog` and `SyncAmsInfoDialog` took the same treatment, and the
  callers guard the `ShowModal` exactly as `SendMultiMachinePage` already did.
- `PlateSettingsDialog`, whose bed-type gate resolved the *current* plate rather than the dialog's
  own and threw from the constructor. Opening plate 3's settings while plate 1 was current therefore
  greyed the control according to plate 1's vendor, and an unresolved current plate killed the app on
  the way into the only editor that can fix an unresolved plate.

This is the flush-volume lesson recurring in a new place: **a query and an action must not share a
throw.** It is also a harder gate than the disabled button the rules already call a bug.

Also closed here: the half-converted Print routing (a plate-derived label over a Project-derived
handler, so the button could name one network and dispatch to the other), the Preview extruder-count
fallback to the Project filament list, `layout_printer`'s network argument, and the sentinel residue
in `Plater.hpp` and `set_print_job_plate_idx`. **`can_paste_from_clipboard` still held both gates
`Selection::paste_from_clipboard` had been changed to drop**, so the previous commit's paste fix was
unreachable and the Paste menu item was greyed with no message; both conditions are gone. And
`place_instances_on_plate` called a resolver that **throws**, straight from the Ctrl+V handler, with
no catch in any frame above it — so pasting onto a plate whose printer preset is not installed killed
the process, after the clipboard objects had already been added to the `Model`. It now routes those
copies to the leftovers row and names the plate.

The PROJECT-scope destination was completed rather than reparented: `combo_printer` itself now binds
`wxEVT_LEFT_DOWN` to `Sidebar::set_project_scope` and skips to the combo's own handler, because a
child's mouse event does not travel up to the panel and the combo covers most of the row's width.

### Build

`cmake --build build --config Release --target OrcaSlicer_app_gui libslic3r_tests slic3rutils_tests`
— **zero errors**, three targets linked, no new warning. One source fix was needed and it was a
missing declaration: `refresh_plate_thumbnail` had a definition and a call site but no entry in
`GLCanvas3D.hpp`, because the agent that wrote the definition did not own the header.

`slic3rutils_tests` was added to the target list deliberately. `PlateBoardModel` lives in
`libslic3r_gui`, which `libslic3r_tests` does not link, so the board's only headless coverage is
compiled by that target alone, and the build line the recon phase produced would have passed while
silently skipping every case in it.

The staging pair under `build/OrcaSlicer/` was 72 minutes and 12 days stale respectively and has been
refreshed. Nothing in the normal loop reads it, which is exactly why it rots unnoticed. **Do not
treat anything under `build/OrcaSlicer/` as evidence of what the current code does.**

### Tests

| Suite | Filter | Result |
|---|---|---|
| `libslic3r_tests` | `[PlateContext],[RetainedGcode]` | **214 assertions in 7 cases, pass** (was 60 in 3) |
| `slic3rutils_tests` | `[PlateBoard],[PlateContext]` | **367 assertions in 7 cases, pass** (new suite) |
| `libslic3r_tests` | full | 184 cases, **1 failed** |
| `slic3rutils_tests` | full | 106 cases, **1 failed**, 33 skipped |

Both wider failures were **stale tests rather than new breakage**, and both test sources are fixed.

- `Printer extruder count tolerates missing nozzle diameter` asserted a fallback this fork repealed.
  Commit `34e0c7b3dd` changed `get_printer_extruder_count` from warn-and-return-1 to
  error-and-return-0 for a missing or empty `nozzle_diameter` — a deliberate no-fallback edit that
  **the commit body never named**, which is why nothing noticed the upstream test still asserting the
  old behaviour. The test now asserts 0 and is renamed to say so.
- `test_plugin_audit` hardcoded `orcaslicer.conf` where every sibling assertion uses
  `SLIC3R_APP_KEY`, and `29291cec5d` renamed the key to `PetkosOrca` on 29 July. The security check
  itself is intact: `is_denied_filename` uses `istarts_with`, and the uppercase-token assertion in
  the same section passed. The test now case-flips the key at runtime so the trap cannot recur on the
  next rename.

**Neither test fix has been compiled or run.** They were written after the build; they are
expected-value and string-construction changes only, and both identifiers were checked to exist — but
that is an argument, not a measurement. The next build settles it, and it should read 184/184 and
73 passed / 0 failed / 33 skipped.

`fff_print`, `sla_print`, `libnest2d`, `filament_group` and `compare_analyzer` are **not built** and
their state is unknown. The 33 skips are all "the interpreter is already running", a one-process
harness limit, pre-existing.

### Live verification, and the crash it found

Verified on `DC-17M Watergun.3mf`, 7 plates over 5 installed machines, against the 3MF's own
metadata: every row names the printer the file names, part counts match, the bed glyphs are
measurably proportional (Prusa CORE One drew 30×26 px for a 250×220 bed, ratio 1.154 against 1.136),
the swatches carry the real library colours **without the board having parsed the plate names**, the
inspector resolves per plate, and the log confirms the engine agrees:
`apply_plate_config: plate 1 slicing with printer 'Elegoo Centauri Carbon 0.4 nozzle'`. All four
grouping modes and collapse/expand work.

**Not verified, for want of a fixture:** hours and weight from a plate's own retained slice, the
rollup totals, and the capacity queue bar. No 3MF on either drive carries retained G-code, so every
hours cell is an em-dash and the rollup honestly reads "7 not estimated". Producing this needs a live
slice, save and reopen.

**A reproducible crash, 2 of 2, on the 21-plate fixture.** `Superdestroyer-BD.3mf` dies after its
four load dialogs, with no WER event, no dump and no "unhandled exception" line. It rides on a live
no-fallback violation that is visible in the log:

```
[error] PresetBundle::update_compatible: Project printer selection is unresolved
[info]  PresetCollection::select_preset: machine try to select preset 1
[info]  machine set Anycubic 4Max Pro 0.4 nozzle, idx 1 to visible
```

`Anycubic 4Max Pro` is not one of the seven installed models. `reset_project_embedded_presets`
correctly sets `m_idx_selected = -1`; `delete_current_preset` then passes that through both of its
range guards, neither of which is true for `(size_t)-1`, into `select_preset((size_t)-1)`, which
falls to `first_visible_idx()`, which can only ever match a preset whose `get_printer_id()` is
`ORCA_FILAMENT_LIBRARY` — no printer can — and so returns `m_num_default_presets`, index 1, the
alphabetically first system printer on disk whether installed or not. `select_preset` then **marks
that invisible preset visible**, which is the substitution becoming permanent and unannounced. The
`delete_current_preset` step is inferred; everything from `first_visible_idx()` onward is proved by
the log above. Open, and it outranks the remaining panel work.

**The Project row also poisons its own AppConfig, and the loop is still live.** The combo reads
`(DC-17M Watergun.3mf)` — a project-embedded preset with an empty base name — and
`export_selections` writes that string into `PetkosOrca.conf`. On the next launch `3f2223996e`'s
keeper fires and the app survives, keeping `Default Printer` / `Default Setting`, the internal
placeholders. So every inheriting plate inherits a placeholder, and opening the next project restocks
the poison.

**Four modal dialogs gate every downloaded project load**, and until the last is dismissed the
sidebar still shows the pre-load state. One of them declares a process preset incompatible with the
printer it shipped in the same 3MF with. For a farm whose entire input stream is MakerWorld projects,
that is the workflow rather than an edge case.

Two method notes for the next live pass, both of which produced a wrong reading before they were
understood. **Make the process per-monitor DPI aware first**, or `GetWindowRect` comes back scaled by
1/1.25 and `PrintWindow` renders into an undersized DC, silently invalidating every coordinate. And
**posted clicks queue behind the app's own work**: on a large project the auto-backup saturates the
UI thread for minutes, so a click and the capture after it can be a whole tool call out of step.
Three apparent grouping bugs dissolved under single-click retests with a stability check.

`PartPlate::calc_exclude_triangles: Unable to create exclude triangles` logs **once per plate**, not
twice per launch. The earlier note that "two are expected" holds only for a single-plate startup.

### Deferred, stated as deferrals

- **The in-canvas plate strip is still there, and plan §8.2 is wrong about it.** The strip is
  **Preview-only** — `GUI_Preview.cpp` disables it in View3D and enables it in Preview — so it has
  never competed with the board in the 3D editor. It is also not just thumbnails: its All-Plates tile
  is the only canvas door to Slice All, the only switch for the G-code viewer's all-plates statistics
  page, and the source of the flag `MainFrame` reads to gate the Print button, while its per-plate
  button **starts a slice** rather than selecting one. Deleting it today takes three working features
  with it. Rewrite §8.2 before anyone tries again.
- **The fourth copy of the inheritance rule, in `PlateBoardModel::build`, is not folded.** It needs a
  decision the fold does not make: the row must display an uninstalled preset's *name*, which tier 1
  correctly refuses to resolve. Its own change.
- **`sliced_config_dropped_reason` has no reader.** A plate whose retained snapshot was dropped
  renders as never sliced, collapsing two states the design deliberately separates, with nothing on
  screen saying a snapshot was discarded. Blocked until `PartPlate` carries the reason off
  `PlateData`.
- **`Plater::resolve_plate_slicing_config` writes `plate->update_apply_result_invalid(true)` from a
  `const` query.** This is F7 one layer up, and it now has more query callers than it did. Since
  `m_apply_invalid` makes `can_slice()` false with only a log line, it is a latent silent gate on the
  Slice button. It needs its own pass with a build behind it; moving the slice gate blind is worse
  than leaving it.
- **`get_printer_extruder_count` returns 0 as a silent sentinel** at roughly twenty call sites,
  several of which size loops by it, so the no-fallback repair turns into an empty loop rather than
  an error. Only reachable via a preset with no `nozzle_diameter`, so it is defensive rather than
  live, but sentinel-versus-error is a real choice nobody has made.
- **No per-plate thumbnail cache across hovers.** There is no per-plate generation counter to key one
  on, only a project-wide bool that the strip clears and only while Preview is up. So in the 3D
  editor each dwell re-renders one plate: one 512×512 offscreen render per 380 ms of deliberate
  pointing, with the scale-down done once per show rather than per paint. A real fix is an epoch
  counter on `Plater`.
- **No >8-plate mixed-printer fixture exists.** Every project on either drive with more than eight
  plates names an uninstalled Bambu project printer and carries zero per-plate assignments. One has
  to be produced by hand from `Superdestroyer-BD.3mf`, which currently crashes on load.

### Closed as won't-do, so it is not re-attempted

**Reparenting `combo_printer` into a board-drawn Project row** (plan §2.3). Every guarantee §2.3 asks
for already holds on the existing panel, which sits directly above the board and is stated as the
Project row in three places in the source. Reparenting a live wx combo into a custom-painted row
means hit-testing around a child window inside `PlateBoard::on_paint`/`on_mouse` and buys no
guarantee that is not already held. What was actually missing was the PROJECT-scope *destination*,
which is ten lines and is now built.

**A board-level `Assign to every unassigned plate` footer.** Both footers already exist, on the
picker where they belong, they are mutually exclusive, and the batch takes one snapshot. A
board-level version would additionally have no machine to assign, which is why it is a modifier on
the next pick rather than an action of its own.

## Work log — 2026-08-13 (the frame, and the 974-option config it was composing 47 times)

The GUI lag is fixed. At 36 plates the frame went **86.6 ms → 5.8 ms**, p99 **119.8 → 13.4 ms**,
startup **25 s → 12 s**, plate switch **153 ms → 45 ms** to first paint. A whole scripted run that
took ~100 s takes 23 s.

### It was one mechanism, and reading it as two problems was the error

`Plater::get_extruders_colors()` composes a complete **~974-option `DynamicPrintConfig`** — the
printer, the process and every filament preset deep-copied, then five whole-config passes — **in
order to read `filament_colour`**. 1.49 ms a call. The frame called it **47 times**:

| where | per frame | cost |
|---|---|---|
| `GLVolumeCollection::render` | 36, one per volume, inside the draw loop | ~53 ms |
| `GLGizmosManager::get_selectable_idxs` | 11, via the four toolbar renders | ~16 ms |

The earlier entry recorded these as two findings — "a flat ~21 ms of overlays" and "~1.9 ms per
volume in objects". They are the same call. The overlay path reaches it through
`GLGizmoMmuSegmentation::on_is_selectable()`, which answers *"is there more than one filament?"* by
composing the whole config and taking `.size()` on the colour list.

**What identified it: a cost that ignores the scene is not about the scene.** ~21 ms of overlays
whatever the plate count, whatever the object count, is the signature of work the scene does not
touch. That is the transferable rule, not the particular function.

### The fix, two shapes of one idea (`16615db75d`)

**Hoist, don't cache** for the 36. The palette is identical for every volume in a frame and the
loop lives in the caller, so `GLVolumeCollection::render` fetches once and passes it down through
new `render(colors)` / `render_with_outline(size, colors)` overloads. It is still fetched fresh
every frame, so a filament colour change lands on the next repaint and there is no invalidation to
get wrong. Overloads rather than replacements, so the four callers outside a loop keep working and
the diff against upstream stays small.

`GLWipeTowerVolume` needed the overload too, or wipe towers would have silently taken the base path
instead of their own per-filament colours — and this is not a compile error, it is a wrong picture.
It is the **only** `GLVolume` subclass in the tree, which is what makes that exhaustive.

**Memo per frame** for the 11. `GLCanvas3D::s_frame_id` bumps once per rendered frame and
`get_selectable_idxs()` keys a memo on it. The answer depends only on filament count and app mode,
neither of which can change mid-frame, and anything that changes either has dirtied the canvas, so
the next frame always arrives. Between frames it returns what the last painted frame used — which
is what hit-testing wants, since the click regions then agree with the icons actually on screen.

**Rejected: reading `filament_colour` narrowly off the project config.** It *is* a project option,
but composition applies the filament preset **after** the project, so a narrow read is not provably
equal. It wins nothing the hoist does not.

### An instrument with a hole in it is worse than none (`4581afd026`)

`ResolvePlateContext` wrapped only `PartPlate.cpp`'s file-static `resolve_plate_context` helper.
That helper genuinely does not fire during a frame — so the instrument reported **zero compositions
per frame**, and that reading was believed for most of a day. The app was doing 47, through
`Plater::resolve_plate_slicing_config`, the second door into the same composition. Probing it turned
0 into 47 and the hunt ended the same hour.

**When a span reads zero, prove it can fire at all before concluding the path is cold.**

### An allocation count is a tax on everything, not just its caller

Paths that never read the palette got faster with it: bed 0.106 → 0.022 ms, picking 0.149 → 0.099,
plate drawing 0.153 → 0.045, and startup halved. 34,586 copies of a 974-option config per run was
thrashing the allocator for the whole application. This is why the win (86.6 → 5.8) is larger than
the arithmetic of the removed calls (~70 ms) predicts.

### Verified

Two identical 36-plate runs, `perf-runs/hoist-36` and `hoist2-36`, against `quad-36`. Frame 6.37
and 5.84 against 86.58; overlays 1.397 and 1.384 against 19.417; objects 1.187 and 1.158 against
30.449; compositions 2,349 and 2,289 against 34,586. Two runs rather than one because the claim
includes the variance collapsing, and one run cannot say that.

### The next target, named with evidence rather than deferred vaguely

**The same mechanism has a third costume, in the board.** `PartPlate::get_extruders(true)` calls
`resolve_plate_context` — one whole-config composition — to read ten `opt_int` values, and
`PlateBoardModel::rebuild` calls it **once per plate**. At 36 plates that is ~36 ms of the 84 ms
`SppBoardRefresh`, which is the largest phase of the `SetPlatePrinter` click, itself inside the
`PrinterAssign` first paint — still the worst interaction in the app at roughly 300 ms.

That figure needs its caveat stated, because a single run misled this entry once already.
`PrinterAssign` samples **twice per run**, and after the fix it reads 461, 184, 305 and 250 ms
across four runs against a tight 401, 425, 418 before it. So it did improve, by something like a
quarter, and the spread is far too wide to quote a number to three digits. `PlateSwitch` samples
eleven times a run and is unambiguous: 153, 164, 170 before against 57, 45, 72, 66 after.
**Interaction spans with n=2 are for direction, not magnitude.**

It is deliberately not fixed in this pass. The frame fix was safe because it caches nothing across
frames; the board fix is a cache with a real invalidation design, and the value being cached feeds
**slicing**, where a stale config is a wrong G-code rather than a slow list. The right shape is the
epoch counter on `Plater` already named in the 2026-08-11 deferrals for per-plate thumbnails — one
counter would retire both — and it wants its own pass with a build behind it.

### The toolbar quad, and a claim that had to be built out to be checked

`GLTexture::render_sub_texture` draws every toolbar button, gizmo icon and all nine slices of every
toolbar border. It used to build a `GLModel` per quad — a VAO and two buffers created, uploaded,
drawn and destroyed, about twenty GL calls and six GPU object lifetimes per square — around a
hundred times a frame. It now keeps one quad for the life of the GL context and rewrites its four
vertices per draw (`GLModel::update_vertices`, one 64-byte `glBufferSubData`), released from
`OpenGLManager`'s destructor because a GL object freed after its context is a crash at exit rather
than a leak.

**Its comment claimed 22.6 ms a frame and no run supported that.** With the change in, the frame
measured 86.6 ms against 88.6 without — inside an 86–101 ms run-to-run spread. The claim was
inherited from the flat overlay cost, which turned out to be the config composition above.

Settled by building it out and back in, now that nothing dwarfs it: **11.1 and 8.7 ms without,
6.4 and 5.8 ms with.** Worth 3–5 ms, roughly a third of the remaining frame, of which only ~1.5 ms
is the overlay draw itself (`RenderOverlays` ~2.9 → 1.39). The rest lands on work that draws no
textures at all — `ResolvePlateContext` 1.42 → 1.05 ms, `BoardRowLayout` 39.5 → 25.8 — which is the
allocator tax again, from a different source.

Two method notes from that check. **The first out-build measured 11.1 ms straight off a
seventeen-minute all-core compile; repeating it on a settled machine gave 8.7.** A measurement taken
immediately after a long build is a measurement of a hot machine. And **reverting a change that
touches a widely-included header costs the wide rebuild twice**, once out and once back — worth it
to replace a guess with a number, but budget for it.

### Tooling

`tools/petkos-dev-build.ps1` (`ff3629d0dc`) builds the DLL and exe only, cores-minus-two, below
normal priority: minutes rather than the ~20 of `build_release_vs2022.bat`, which also builds six
test binaries, runs gettext and installs ~8,000 resource files that no code change touches.
**Polling a build log in a loop is what fills a session's context** — wait on one blocking call.
Touching a widely-included header such as `GLModel.hpp` still costs a wide rebuild.

## Work log — 2026-08-13, second half (the click, and two faults that were forging the numbers)

The frame was the morning's work. The afternoon's is the printer-assign click, which is the
interaction this fork exists to make good: upstream's is worse, and a fleet slicer whose
"put this plate on that machine" costs half a second is arguing against itself.

### Two measurement faults, found in the middle of it

**A stray `find / -name brush.h` from the previous session had been running since 09:55.** Seven
hours, most of a core, through every measurement taken today. Killed. Nothing in the harness would
have shown it: the perf runs report the app's own spans, not what else the machine is doing.

**And the room.** London heatwave, 38 C outside, and the laptop is thermally limited: the CPU sat
at 133% of base during runs against a 180% ceiling, and the same binary measured 11.1 ms straight
off a long build and 8.7 ms settled.

Together those make **every cross-run comparison taken hours apart worthless on this machine.**
What survives, and what this entry quotes:

- **within-run ratios** - two spans measured in the same process, which share whatever clock the
  machine happened to be running at
- **back-to-back A/B runs** - build out, measure, build in, measure, nothing else in between

Quoting a number to three digits across a session boundary is now a stated error here, not a
stylistic preference.

### The board was O(plates) for an O(1) change (`0acf25f8c5`)

Assigning a printer to one plate rebuilt all 36 rows, and every row composes a whole config to
read which extruders its plate uses.

The model now separates **reading** a row from **deriving** what follows from the row set. The
rollup totals, the glyph reference and the grouping are pure functions of `m_rows`, so deriving
them instead of accumulating them inside the read loop is what lets one row change and every total
stay correct. `refresh_plate()` re-reads one row and derives the rest.

It **declines rather than guesses**: it patches the model in place, so it only does so while the
model is provably still describing this plate list, and otherwise returns false and the caller does
a full rebuild. An optimisation that can decline is not a second source of truth that can drift.

  BoardRowRefresh  1.5-1.7 ms  against BoardRowLayout 29-34 ms, same run
  SppBoardRefresh  64% of the click before, 9% after

The three probes added alongside settle what the rest of a board reload costs: item rebuild
0.006 ms, sizer pass 0.016 ms, inspector 2.5 ms. The old 83 ms was almost entirely the model.

### One composition per context, for as long as one operation lasts (`a05d8fbcb6`)

The board fix removed the O(plates) rebuild from *one* click. `PresetBundle::ComposeScope` removes
the O(plates) composition from every path that still has one, which is the mechanism rather than
the instance.

Inside one operation the preset collections and the project config cannot change, so a plate's
slicing context plus its two filament maps is the complete key, and plates sharing a context share
one composition.

**It is a scope, not a cache with a lifetime.** It exists between a constructor and a destructor
and nowhere else, and the owner clears it on the way out. That is deliberate: the value being
shared is the one slicing reads, so rather than write an invalidation rule and hope it covers every
mutation site, nothing is permitted to outlive the operation it was safe to share within.

**It is per-thread, and that was nearly a crash.** The bundle is a global, and ArrangeJob,
FillBedJob, OrientJob and SendJob all compose against it from their own threads. A cache stored on
the bundle would have had a background job pushing into the very vector a frame was walking - a
data race on the container, so a crash rather than a stale answer. Caught by asking who else calls
the function, before the first build of it finished. A thread with no scope of its own composes
exactly as it always did, and the owning bundle is recorded so a scope opened on one bundle cannot
answer for another.

Cache hits still copy the resolved config rather than return a reference, because several callers
apply that plate's own overrides straight onto what they are handed, and a reference would land one
plate's overrides on the next plate's config. One whole-config copy against six is the win.

  composition, per call   1.50 ms -> 0.39 ms
  full board rebuild      34.2 ms -> 7.1 ms
  plate switch, painted   75.8 ms -> 25.5 ms
  frame                   9.75 ms -> 7.48 ms

### What the click is made of now, and where the next work is

`SetPlatePrinter` is ~51 ms of work followed by a ~198 ms wait for the first paint. Neither half is
presets any more.

**`PartPlateList::reflow_layout` is 22.6 ms mean and 79 ms at worst**, and the mechanism is named:
`PartPlate::reposition()` calls `set_shape()`, which regenerates the plate's render data, and
reflow calls it for every plate. `set_shape` does early-out on an unchanged shape, but only after
translating and allocating three point vectors, and a plate that genuinely moved pays the full
~2.4 ms rebuild.

The fault underneath is that **a plate's position is baked into its geometry rather than being a
transform.** `m_shape_local` exists because somebody already met this and stopped halfway: the
untranslated profile is kept, but `m_shape` is still world-space and every move rewrites it. Moving
a plate should be a matrix, not a regeneration. That is the fix, and it reaches the plate's render
path, its picking and its `contains()` tests, so it wants its own pass.

**The ~198 ms to first paint is not the driver.** The perf driver runs on `wxEVT_IDLE` with
`RequestMore()`, and idle runs after paints, so the gap is real event-loop and paint latency rather
than harness pacing. Inside it sits the board's thumbnail heal: a 512x512 offscreen GL render
issued from **inside the paint handler**, 44 ms, and an assign invalidates the thumbnail of the
plate it changed, so the click pays it every time. A paint should never block on a render it could
do afterwards.

### Startup logs at info, and it is mostly presets

A measured run writes a 1.9 MB log: 7,986 info lines, of which **7,718 are preset loading** -
`PresetCollection::set_printer_hold_alias` 5,573 times and `Preset::set_visible_from_appconfig`
2,145. That is startup, not the frame, and startup is 12 s. It is also the category of thing that
rides through a perf run unnoticed, so it is written down here rather than left to be rediscovered.

## Work log — 2026-08-14 (the project printer was never a printer)

### What it actually was

`PlateSlicingContext` had five fields and a rule: an empty field means the plate follows the
project. That rule is where the singular engine lived. Not in a missing feature — the data layer
had held a per-plate printer since `4cf3af2bda` — but in what *absence* was defined to mean. As
long as "empty" resolved to the globally selected preset, every plate that had not been explicitly
told otherwise had one machine standing behind it, and the resolver read that machine on every
slice, every frame and every board rebuild.

The project printer is not a printer. It is the settings tabs' **cursor** — which preset is
currently being edited — and the application had been reading a cursor as a fact about the project.

So the deletion is not "remove a field". It is: **empty stops meaning anything.** A plate names its
own printer, its own process and its own materials, or it is unresolved, which is an error to fix at
its source. The global selection survives as what it always was, an editing cursor, and it now
follows the current plate instead of standing behind every plate.

The difference between a state and a default is how many times it is read.
`PresetBundle::complete_plate_context` is the only remaining read of the global selection on a
plate's behalf, and it runs **once per plate, at the moment the plate comes into being** — a plate
being created, or a project written before per-plate machines being loaded.

### What that deleted

| Gone | It was |
|---|---|
| the inheritance branches in `resolve_plate_presets` and `compose_plate_slicing_config` | the mechanism itself: an empty name read the edited preset |
| `PartPlate::has_slicing_context_assignment()` | "is this plate explicitly configured", a question with one answer now |
| `apply_printer_to_plate`'s `inherited` branch and `PlateBoardModel::build_project_row` | a second bed source and a row describing it |
| the board's inherited group, `m_all_inherited`, `PlateBoardRow::assigned`, the dim third state in the row painter | "changeable default, not a choice" — there are no defaults left to describe |
| the picker's "Same as Global" first entry | the one-click way back to the state being deleted |
| `Sidebar::set_project_scope`, `m_scope_project`, `is_project_scope`, `printer_panel_project_scope`, the inspector's whole PROJECT branch and its Source row | a scope whose only content was the project printer |
| `ArrangeJob`'s pool/sticky split and its whole single-project-bed path | two arrange algorithms because plates came in two kinds |
| `reresolve_plate_context_for_printer`'s `process_now_inherits` rule | a process re-inheriting a pairing that no longer exists |

`PLATE_BOARD_PROJECT_ROW` became `PLATE_BOARD_NO_PLATE`. The name was the lie: -1 never meant the
project, it meant "the scope is not exactly one plate".

### Every plate-context field now has a user-facing write path

One idiom throughout: **click the value, get a menu of what will actually resolve here.** A menu
that lists what cannot resolve is a menu that turns half its entries into an error message after the
click, so each list is filtered by the same compatibility calls the composer makes afterwards — and
a stored name this installation does not have is always offered back verbatim at the top, because
opening a picker must never be the thing that discards it.

| Field | Where it is written |
|---|---|
| `printer_preset_name` | the row chip, the inspector's Printer row, a drag onto a machine group, the sidebar printer combo |
| `printer_vendor_id` | never by hand: recorded by the re-resolution mechanism from the resolved printer |
| `print_preset_name` | **new** — the inspector's Process row, and the sidebar process combo |
| `filament_preset_names` | **new** — click a swatch, pick that slot's material; and the sidebar filament combos |
| `physical_printer_id` | **new** — the inspector's "Prints on" row, alongside the send dialog that already wrote it |

The swatch strip is where filament is changed because the swatch is the thing showing the value; a
separate materials row would be a second place displaying the same fact. It writes **one slot at a
time**, because writing a whole new list to alter one slot is how the other slots get quietly reset.
And it is writable only when the badge names one plate.

### The cursor follows the plate (`Plater::follow_plate_presets`)

Selecting a plate points the settings tabs at that plate's presets. Only fields that actually
differ are moved, so in a single-machine project the cursor never moves and Orca's unsaved-changes
dialog never appears. When they do differ the dialog is exactly right — a plate on another machine
genuinely is a different thing to be editing — and if the user declines it, the tabs stay where they
were and say so, rather than silently describing one plate while the badge names another.

The other direction is the same idea: the sidebar's printer, process and filament combos now write
to the current plate. They used to edit "the project", with the untold plates following along.

### The consequences that were not obvious

**A new plate is a copy of the last one.** That is what `complete_plate_contexts` does, and it also
answers three questions that used to be answered by "the project": what shape a plate that does not
exist yet has (`ArrangeJob`'s future beds), what an arrange overflow lands on, and what a plate
imported without a machine becomes.

**Arrange overflow keeps its machine.** An item that will not fit goes onto one of arrange's extra
beds *of the same shape*, which is the same machine by construction; `finalize` gives the plate it
creates the context of the plate it overflowed from. Without that, an object would change machine by
failing to fit — a silent yes with a cost in the physical world.

**A homeless item joins the current plate.** Items with no source plate used to fall into the pool
of plates sharing the project bed. The current plate is the one the user is looking at and where the
object visually already is; anything else would be the app deciding which machine prints it.

**The plate label says its machine only when that distinguishes.** Every plate has a printer now, so
an unconditional suffix would repeat one string on all thirty-six plates of a single-machine project.
Crossing between one machine and several restyles every label, not just the one that changed, because
the rule is a property of the set.

**Swatches read the plate's own colours.** `PartPlate::config()` already carried `filament_colour`
(the AMS sync writes it there), and the board was reading the project library regardless — so the
plates whose materials had been set deliberately were exactly the ones showing the wrong colour.

**CLI keeps a second bed source, named rather than hidden.** There is no wxApp and no preset bundle
in CLI; a plate resolves against the project config the 3MF carried, which arrives through
`set_shapes`. `PartPlateList::plate_beds_come_from_presets()` is where that difference is stated.
CLI's global arrange pool became "the plates that share the first plate's machine" — a bed is the
property the pool ever really needed — and the legacy bed translation still runs, because a project
written before per-plate machines still names no printer on any plate when CLI reads it.

### The trap this walked into, which had already been sprung once today

`e0039df8bf` ("A plate is never incomplete") is the same design, from this morning, and it was
reverted in `1e6a972028` because it silently tripled print times. This session rebuilt the design
and reproduced the bug exactly before finding the plan note that diagnosed it.

The fault is timing, not the seed. `PartPlateList::load_from_3mf_structure` runs BEFORE
`load_config_model` and `load_project_embedded_presets` put the project's own presets into the
bundle. Completing the plates there samples whatever was selected before the file was opened -
`Default Setting`, `Default Filament` - and writes them onto every plate as concrete names, after
which they outrank the real presets the 3MF was carrying. `Default Filament` caps
`filament_max_volumetric_speed` at 2, about six times below the real value:

    working    filament_settings_id = "Generic PETG(...)"   max_volumetric_speed 12  ->  6h45m, 174.7 g
    populated  filament_settings_id = "Default Filament"    max_volumetric_speed 2   -> 21h03m,   0.00 g

**Every in-app check passed on that build.** The scope check passed, the save round-tripped, the
plates were all complete. Only reading the emitted G-code caught it, which is the general lesson:
a change can be correct in the data model and wrong in the output, and "complete" is not the same
as "right".

The first fix was a timing rule - refuse to complete while a project is loading - and the tell that
it was a patch was the length of the comment defending it. A rule enforced by a flag breaks again
the next time somebody adds a call site, and the same file already says that a workaround needing a
paragraph to justify it is the code asking to be fixed.

**The repair is that the seed is an argument.** `complete_plate_context(context, seed)` takes the
seed by reference and reads no global state at all, so a caller has to name where the values come
from. That is the difference between a value somebody chose and whatever was selected when the code
happened to run, and a signature that cannot reach a global cannot get it wrong from any call site.

- A plate in an existing session is completed from the last complete plate; only with no plate at
  all does the bundle's selection get used, and there it is a remembered choice.
- A loaded project is completed from what the FILE declared. `load_files` reads
  `printer_settings_id`, `print_settings_id` and `filament_settings_id` out of the 3MF's own config
  before that config is moved into the bundle, and passes them in. Correct whenever it runs.

And the driver now asserts that no plate names a preset that `is_default`. A placeholder is a
complete context, so the completeness check could never have caught this on its own.

### The acceptance check

`PetkosPerfDriver`'s new Context phase, on by default, asks three questions of the code slicing
itself reads, in rising strength:

1. Does every plate carry a complete context of its own?
2. Does an **empty** context refuse to resolve? That refusal *is* the deletion.
3. **Does moving the global selection change what a plate slices with?** The cursor is moved out
   from under plate 1 and the plate must not notice; the selection is restored afterwards. This is
   the one a well-behaved caller cannot fake.

### Podslicer, and the pass that renamed it (2026-08-14, afternoon)

Petko named the fork: a pack of orcas is a pod. `SLIC3R_APP_FULL_NAME` -> `Podslicer` (display only,
109 uses, all dialog captions), `SLIC3R_APP_KEY` -> `Podslicer` (config file, wx app name, 3MF
`Application` metadata). `SLIC3R_APP_NAME` stays `OrcaSlicer` because it goes into the G-code header
and firmware parsers sniff that prefix - `version.inc` already carried that warning.

Renaming the app key renames the config file, which is how the 2026-07-29 rename left an orphan.
`AppConfig::loading_path()` now adopts the config written under any previous key rather than opening
factory-fresh beside it.

`PETKOS-ORCA.md` -> `PODSLICER.md`.

### What the parallel pass found, and it was mostly about this session's own work

Four agents ran against a read-only tree while the compiler had it. Two of the findings were fatal
and both were mine.

**The 21-hour fault, re-entered through a different door.** `create_plate()` completed EVERY plate in
the list, and `load_from_3mf_structure` calls it once per plate. On iteration N it topped up plates
0..N-1 - which the file had just written as empty - with whatever the bundle held before the project
was opened. Only the current iteration's plate was corrected afterwards. Completion only fills empty
fields, so nothing later corrected the rest.

The rule this establishes, and it is the same rule the seed-as-an-argument change was reaching for:
**a plate is seeded by whoever ASKED for it.** `on_action_add_plate` seeds from the plate the user is
on; the load path seeds from what the file declared. `create_plate` seeds nothing.

**And the migration was in the wrong branch.** `if (type_3mf)` runs 7387-7982 and `else` runs
7983-8125; the capture landed at 8117. The whole mechanism was dead for every project the app
actually opens. A brace-depth check said "inside" because `} else {` never returns to depth zero -
worth remembering, because it is a plausible-looking verification that proves nothing.

**A hard stop nobody had hit yet.** `compose_plate_slicing_config` refuses a context whose filament
is incompatible, and re-resolution deliberately never rewrote filament - so pointing a downloaded
Bambu plate at a machine this farm owns left the plate unable to SLICE. Six plates by four slots is
54 manual repairs before anything runs. `translate_filament_to_printer` fixes it, and it sharpens
rather than breaks the rule it amends: "never substitute a filament" protects a MATERIAL choice, and
PLA re-expressed as the target machine's PLA is not a substitution. One string decides it.

**A preset-name defect visible in the live config.** `load_external_preset` builds the project suffix
from a name that already carries one, so it accretes on every save/reopen:
`PolyTerra PLA @BBL A1M(helldiver-colored.3mf)(helldiver-colored.3mf)(helldiver-colored.3mf)`, found
in a real sliced file on disk, and doubled on all three collections in `PetkosOrca.conf`. The
previous author met this and left a commented-out `//TODO` regex three lines above. It is not
cosmetic here: plate contexts STORE these names, so a name that grows every round trip eventually
stops matching what the bundle installs.

**The board evicts the inspector.** There is no scrollbar anywhere in `PlateBoard.cpp`; the board
asks for four rows, the sidebar gives about 2.5, and the shortfall lands on the last child - so the
plate inspector is not visible in the running app at all. Both screenshots show rows bisected top and
bottom on a three-plate project.

**And `board_soft` (#323A3D, a TEXT colour) was the brush for both empty picture cells**, which made a
near-black square the loudest object on a panel about which machine a plate goes to. Dark mode
lightens it, so it was a light-mode-only fault that never showed up in testing. The empty cells now
draw the plate's bed in plan, scaled against the project's largest - which is also what the glyph
animation has been interpolating for nothing since the row became pictures.

### The demand evidence for all of this

Upstream's canonical request, **#7238**, was closed *not planned* **by a stale bot: zero reactions,
two comments, both the bot, no human reply.** The demand does not show up as reactions. It shows up
as **the same request independently refiled at least twelve times in three years** - #1277, #1309,
#3593, #3942, #7221, #7238, #8420, #8596, #10551, #11445, plus discussions #6357 and #7506 - at least
six auto-closed unanswered. Fifteen are closed by this tree.

Do not claim the high-reaction neighbours that turn up in the same searches: toolchangers (#2050, 67
reactions), per-feature filament (#7106, 37), custom bed surfaces (#836, 36), filament-scope
overrides (#12401, 21). Podslicer does none of those.

### The instrument that would have caught the 21-hour slice

`tools/petkos-gcode-check.py` reads what a slice ACTUALLY used out of the emitted G-code - the only
artefact that cannot be wrong about what gets made. It knows the incident by signature: a placeholder
preset, `filament_max_volumetric_speed` below 4, or `0.00 g`. It found the suffix-growth bug on its
first real file. `tools/petkos-verify.ps1` now slices and reads it as a fourth question, and
`tools/petkos-import-check.ps1` runs the whole cross-printer workflow end to end: open a real
Bambu-authored 3MF, retarget it to a machine this farm owns, slice, and check the project's own
wall/infill/pattern choices are still in the G-code.

# Work log — 2026-08-15, overnight (the queries all report, and the app stops narrating its own boot)

The overnight half of the ship sprint, run unattended. Petko's live session (an unsaved
KATANAPainted project) stayed open on the live datadir the whole night; everything below ran
beside it on the instrument's own datadir clone, which is what the harness learned to do first.

### The sweep: no GUI query throws on an unresolved plate any more

The 01:24 crash log was the night's opening evidence, and it was the sweep's own mechanism one
build behind: `Plater::set_bed_shape` reported an unresolved plate through a notification during
`on_init_inner`, before ImGui's first frame, and `CalcTextSizeA` dereferenced a null font. The
guard (`PopNotification::init` defers until ImGui can measure text) had landed in `1cf04da958`
at 02:09; the crash was the 56ffcafa5a binary's last gasp. Reading the crash log first is the
house rule, and this time it closed the item rather than opening one.

The sweep itself: 16 throw sites in 12 files became reporting queries on the IMSlider model —
log, mark, degrade to honest defaults, never substitute. The judgment calls, per class:

- **ConfigManipulation (4 sites)**: a validation with no printer context skips — "no check
  performed" is an answer. The spiral-mode dialog keeps working and only loses its I3 note.
- **Render-path gizmos (BrimEars ×2, FuzzySkin)**: the crash class. They draw against
  `FullPrintConfig::defaults()` plus the plate's own overrides — the honest absence of a preset.
- **Value queries (DragDropPanel, FilamentMapPanel ×4, FilamentMapDialog ×2, EditGCodeDialog,
  GLGizmoRotate, AmsMappingPopup, GUI_ObjectList)**: empty/zero/skip, each matching what its
  callers already do on the sibling failure path.
- **WipeTowerDialog**: the front door (`open_flushing_dialog`) now refuses with a message before
  the dialog's own reads can throw behind it. A button that does nothing silently is always a bug.

Left throwing, deliberately: Jobs (Arrange/FillBed/Orient/Rotoptimize) — commands refusing
loudly through `e.eptr` → finalize or the floor, both of which report; PostProcessor and
BackgroundSlicingProcess (their own catch/report channels); genuine invariants
(PartPlate::preprocess_exclude_areas index check, GUI_App::post_init).

### Startup: the boot was narrating itself, 7,892 lines per boot

Measured from the live session's own log (no launch needed): cold boot is 16.3–19.0 s to first
window, ~20–22 s interactive. Three sinks: network plugin DLL + agent 7.24 s (36%), main-window
construction 6.18 s (31%, incl. four WebView creations and a 1.8 s unlogged gap), preset load
3.68 s — of which 2.14 s is TWO log lines: `set_printer_hold_alias` (5,573×, one per
preset×printer, from inside the compatible-printers loop) and `set_visible_from_appconfig`
(2,319×), through a sink that flushes every record (`utils.cpp` `auto_flush=true`), from six
vendor-load threads at once. 96.4% of the startup log's bytes were these two lines, identical
counts every boot. Both demoted to `trace`. [A/B: startup log after = ___ lines, alias window = ___ s]

The network-DLL and WebView sinks are named for daylight: `restart_networking()` already proves
a full late re-init works, so deferring `on_init_network` past first paint is feasible — but it
reorders against `PluginManager::initialize()` and the agent callbacks, not a 4 AM change.

### Plate switch: the click was five tab rebuilds wearing one cursor move

Live-log decomposition of six real clicks: `SelectPlate` itself is 81–93 ms; then
`follow_plate_presets` runs from its CallAfter and costs 433–670 ms when the printer changes —
78–87% of every such click. Inside it: `Tab::select_preset(printer)` 272–474 ms (which cascades
`load_current_preset` for machine + process + filament), then `select_preset(process)` 76–105 ms
(two more loads) — **five full tab rebuilds per click**, each with `update_visibility()`'s
page-tree teardown. A same-printer click costs 20 ms. The harness had a 225 ms hole here:
`Tab.cpp` had zero probes. It now has them (`TabSelectPreset`, `TabLoadCurrentPreset`,
`TabUpdateVisibility`, plus `FollowPlatePresets`, `PlaterSetBedShape`, `BundleFullConfig`,
`LoadBedtypeTextures`), so the dedup (5 loads → the 2 the click needs) is a measured daylight
fix instead of blind surgery in the region whose dialog fixes Petko confirmed hours earlier.

### The black flash, and every plate now wears its own bed

The mechanism, confirmed in code: `PartPlateList` held ONE logo texture for the whole list.
Changing the selected plate's printer reset it and started an async reload —
`load_from_svg_file(compress=true)` allocates the GPU texture EMPTY (`glTexImage2D` with null
data) and fills it frames later from the compressor thread. Between reset and fill, the bed
bound an empty texture: black. And unselected plates drew no texture at all, which on a
mixed-printer project made every switch a black-then-appear.

The repair is Petko's own design plus a cache:

- `PlateBed` gained `bed_texture`, resolved in `resolve_printer_bed` exactly as `bed_model` is —
  so a plate's texture is a fact about ITS printer, not about the selection.
- `PartPlateList::logo_texture_for()` — one texture per filename, loaded once, never evicted,
  and not handed out until `all_compressed_data_sent_to_gpu()`. Until then the plate keeps
  drawing the texture it last drew (`m_logo_texture_shown`). No frame can bind an empty texture.
- **Every plate renders its texture now** — the selected one full-strength, the rest at 0.35
  alpha through a new `opacity` uniform on the printbed shader (both GLSL 110 and 140; the other
  three users of that shader pin it to 1.0).

### The harness runs beside an open session, and the first attempt found out why it never had

`pod run`/`pod slice` refused to run while ANY slicer was open. The invariant it protects is
exclusive use of a datadir, so the clone route petkos-verify.ps1 already described became the
shared mechanism. But the clone had never actually been USED: the "both suites green" runs at
~02:09 predate Petko's 02:14 launch and ran on the live datadir. The first real clone run hung
for its whole 600 s timeout with a healthy event loop and zero driver lines, and the mechanism
is a trap worth naming: **the clone excludes plugins/ but its conf still claimed
`installed_networking: true`**, so `on_init_network` set `m_networking_need_update`, and
`post_init` answered that with `NetworkPluginDownloadDialog::ShowModal()` — a modal an
unattended run can never click, sitting exactly BEFORE the line that starts the perf driver.
The log signature: post_init reaches "end load_gl_resources" and never prints
"finished post_init", while Backup timers tick happily forever.

The clone is now made by ONE implementation, `pod clone` (the three ps1 harnesses call it):
copy minus plugins/ and log/, then falsify the conf's networking claim honestly — this datadir
really has no plugin — and recompute the MD5 trailer. The app then boots it as a clean
no-networking install, straight through to the driver. The rerun went 600 s hang → 31 s green.

Also in the same pass: `pod status` names the running instance's pid and WHICH datadir it
holds; `petkos-dev-build.ps1` learned that the post-build event's `rm -rf Release/python`
fails while the app maps `python312.dll` from it (the whole directory moves aside now, same
rename trick as the exe); `PETKOS_TEST_ASSIGN` learned `;`-separated multi-assignment, which
is what built the fixture below.

### Verify, on the night's build (05:26)

All five questions PASS on the datadir clone: scope isolation (plate 1 wall_loops=5, plate 2
untouched), context ownership, per-plate printers on disk, override surviving save, and the
G-code gate — the emitted file names `Anycubic Kobra S1 0.4 nozzle`, its real process and
filament, wall_loops 5, 27.57 g. The new Tab probes came back alive in the same run:
`TabLoadCurrentPreset` ×8 for two plate switches (166 ms mean, 410 ms p95),
`FollowPlatePresets` 594 ms p50 — the 225 ms hole in SelectPlate is now named spans.

### Wave 2c, stage 0.1 and the three device-layer lies (the pre-authorized set only)

- `SelectMachine.cpp resolve_plate_slicing_context`'s success path returned `false` — every
  caller read a resolved plate as a failure, and the whole Bambu send path was dead by one
  keyword. It returns `true`.
- `DeviceManager::subscribe_device_list` cleared `subscribe_list_cache` and then iterated it
  to build the unsubscribe list — always empty, so `del_subscribe` never fired and stale
  subscriptions accumulated per ever-selected machine. Collect first, clear after.
- `DeviceManagerRefresher::on_timer` returned when no machine was selected, which starved the
  account-level `check_pushing`/`refresh_connection` for the whole fleet — and a board full of
  plates naming machines has no "selection" at all. Only the per-machine cert install is gated
  on the selection now.
- `MoonrakerPrinterAgent` inherited `IPrinterAgent::add_subscribe`'s do-nothing
  `BAMBU_NETWORK_SUCCESS`, telling DeviceManager a list of machines was being watched when a
  Moonraker websocket watches exactly the one it is connected to. It now answers honestly:
  the connected device is subscribed by construction, anything else is a visible refusal.
  (CrealityPrintAgent inherits the same silent default — named here, not fixed tonight.)

### Import-check, on the same build (05:47)

VERIFIED. The Bambu-authored Superdestroyer project retargeted to
`Elegoo Centauri Carbon 0.4 nozzle` and the emitted G-code holds the five numbers:
max_volumetric_speed 12 (not the placeholder's 2), wall_loops 2, density 15%, pattern grid,
280.40 g of a real PLA. Both suites green on the complete night's edit set - the gate binary
import-check ran includes the device-layer fixes.

### Endgame results

- **Both suites green on the final binary.** `pod verify`: five PASSes including the G-code
  gate (Kobra S1 preset, wall_loops 5 override in the emitted file, 27.57 g).
  `pod import-check`: VERIFIED - Bambu→Elegoo holds 12 / 2 / 15% / grid / 280.40 g.
- **Perf regression 1/6/36 (tag `overnight`)**: CanvasRender p50 at 36 plates **5.365 ms** vs
  the 13 Aug baseline ~5.8 ms - no frame regression, WITH every plate now rendering its pale
  texture. PlateSwitch settle p50 92 ms (n=11). The new probes put numbers on the daylight
  fix: `FollowPlatePresets` 1.51 s for one cursor move at 36 plates, `TabSelectPreset`
  1.11 s p50 - the tab-rebuild cost GROWS with plate count.
- **Six test suites, from a scratch cwd.** The fork's own surfaces are green and stable:
  slic3rutils 72 passed / 34 skipped / 0 failed, libnest2d and filament_group all pass, and
  a full-pass libslic3r run (187/187) once its stale tests were modernised. Three test
  archaeology finds, all the same shape - tests asserting the pre-14-Aug world:
  `process_now_inherits`, the board's `project_row`/`assigned`, an all-empty context
  resolving by inheritance, and the `enable_filament_dynamic_map` lossy pin (which fired
  exactly as its own comment promised, once full plate persistence made the key survive).
  One REAL code fix fell out: the generic `plater_plate_config` 3MF channel deserialized
  `filament_volume_map` verbatim while the legacy attribute channel clamps it - the
  unvalidated channel always won. They agree now; whether the clamp should spare TPU High
  Flow (which the filament-map panel can emit) is flagged for daylight.
- **Intermittents, named and not chased**: test_marchingsquares, test_voronoi,
  test_hollowing (libslic3r), test_print:382 + test_skirt_brim:281 (fff_print), and
  sla_print_tests:226 fail in roughly half of runs and pass in the others, on upstream
  geometry surfaces no commit of this branch touches.
- **The fixture**: `perf-runs/fixture-superdestroyer-mixed.3mf` - 21 plates, ALL owning
  their printer, seven distinct machines across plates 1-10, a verified slice attached to
  plate 1, and board screenshots (`perf-runs/fixture-board-*.png`) showing the machine-
  grouped board with per-plate inspectors and honest unresolved flags for parts that
  genuinely do not fit their new beds. Building it exposed and fixed one more instrument
  gap: the driver's `save=` only executed inside the scope phase; it is phase-independent
  now.

- Watch item: the live conf holds a double-decorated preset name
  (`Bambu Lab A1 0.4 nozzle(KATANAPainted-Optimized.3mf)(KATANAPainted-Optimized.3mf)`) written
  by a pre-fix binary; if it recurs on a post-fix conf write, the suffix stripper has a hole on
  the orca_presets remember path.
