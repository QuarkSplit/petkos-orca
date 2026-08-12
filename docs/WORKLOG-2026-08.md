# Work log — August 2026

Dated build history and closed audit findings for the fork, split out of `PETKOS-ORCA.md` on
2026-08-11 so the root file states current architecture rather than its own history.

**Read this when you need provenance, not before starting work.** `PETKOS-ORCA.md` is the
authority on how the fork behaves now; anything here is a record of how it got there and may
describe states that no longer exist.

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
