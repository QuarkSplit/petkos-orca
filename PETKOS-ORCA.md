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

#### Audit findings recorded but not changed before the pause

- `Sidebar::priv::layout_printer` is passed Bambu-network capability where it expects vendor
  identity, and dual-extruder layout is restricted to Bambu. The bed-visibility check also ignores
  its `isBBL` argument and rereads the Project preset.
- Preview setup still sizes extruder parameters from the Project filament list instead of the
  current plate's resolved config.
- The legacy `SendJob` and the legacy send/export helpers still accept current/all sentinels after
  launch. Their UI callers should resolve the current plate once and pass its concrete index.
- `PlateSettingsDialog` is constructed before the event's target plate is retrieved, so its bed and
  vendor controls can still be based on whichever plate was current.
- The main Print action still chooses Bambu-network versus print-host routing from the Project
  printer, and the main button's default action can remain stale after switching to a plate with a
  different printer.
- GL-canvas nozzle/filament compatibility still passes the Project filament-preset list even though
  the print config is plate-local. The exact config already carries `filament_settings_id` and
  should be the source.
- The normal Bambu dialog still contains permissive branches for missing printer-model data and
  timelapse-storage checks that time out or fail. These currently proceed instead of remaining an
  explicit error and therefore need removal under the no-fallback rule.

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
