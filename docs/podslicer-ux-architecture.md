# Podslicer UX architecture — the implementable interaction model

Status: normative. This is the interaction architecture the GUI work implements. Where the
current tree contradicts it, §7 names the file and function that changes. Read
`PODSLICER.md` for why the fork exists; read this for what the surfaces do.

Vocabulary used throughout:

- **Vendor default** — the shipped tuning for one (printer model + nozzle) or one
  (material + machine): `default_print_profile` / `default_filament_profile` as declared by
  the printer's own profile. Never edited, never named by the user.
- **Deviation** — a set of key→value pairs a person chose, stored against a scope (machine,
  project, plate, object). A deviation is data, not a preset: it has no name, no file of its
  own, no save ceremony.
- **Plate context** — `PlateSlicingContext`: printer, process name, filament slots, colours,
  appearance, physical printer id. Owned by the plate (`PartPlate` members), serialized in
  the 3MF.
- **Plate deviation** — the plate's own process overrides: `PartPlate::m_config` minus the
  plate-owned keys (see `is_plate_owned_process_setting`, `PlateSlicingContext.hpp`).
- **Pool / inventory** — the spools the user owns: material + colour + appearance,
  independent of any printer and any project (§5).

---

## 1. The settings model — where a value can live

There are no user presets. A process value resolves through exactly four layers; the last
layer that speaks wins:

```
L0  VENDOR DEFAULT   printer's own declared process for this model+nozzle
L1  PROJECT          project-wide deviation (PresetBundle::project_config, applied by
                     apply_project_overrides) — applies to every plate in the file
L2  PLATE            plate deviation (PartPlate::m_config, non-plate-owned keys)
L3  OBJECT / PART    per-object and per-part ModelConfig overrides
```

This is already the composition order in `PresetBundle::compose_plate_slicing_config`
("preset < project < plate < object"). What changes is that L0 stops being "whatever named
preset is selected" and becomes an anchored fact: **the plate's named process is always
resolvable to the vendor default for its printer**, and everything a person changed lives in
L1–L3 as deviations. The `print_preset_name` field survives as the identifier of the L0 base
(vendor layer-height variants like "0.20 Standard" are different L0 bases, not user
choices).

Machine facts get one deviation layer of their own, **app-level, not project-level**: a
start-G-code fix or a bed-size correction for the K1 Max is true in every project, so it is
stored once per printer profile in the app (the existing edited-printer mechanism, renamed
in the UI from "preset" to "machine settings", with no save-as / naming ceremony — edits
apply to the machine, full stop). The same holds for filament tuning: a per-(material,
machine) deviation ("my black PETG runs 5 °C hotter on the K1 Max") is app-level. Neither is
stored per project; the 3MF snapshot of resolved profiles (already exported) is the
portability mechanism.

### Display rules (the "at a glance" contract)

Every settings control in the Process tab (which IS the plate inspector — edits write to the
current plate via `write_plate_process_options`, `Tab.cpp` ~4024–4129) carries a layer tick
at its left edge:

| Tick | Meaning | Hover | Click |
|---|---|---|---|
| none | value is the vendor default (L0) | — | — |
| filled dot, accent | plate deviation (L2) | "Changed for this plate — default: *X*" | revert to inherited |
| ring, accent | project deviation (L1) visible on this plate | "Changed for this project — default: *X*" | revert |
| filled dot, violet | object override (L3), shown only in the object settings panel | "Overridden for this object — plate value: *X*" | revert |

Rules:

- The hover always shows **the value one layer down**, so "what it was" is one glance, and
  revert is one click. Revert removes the key from that layer's deviation; it never writes
  the old value back as a new deviation.
- **An explicit false is a deviation** until reverted (already the rule in
  `PlateProcessSettings.hpp`); the tick renders for it like any other value.
- The plate header (board row and inspector title) shows the deviation count:
  "0.20 Standard · **12 changes**". Clicking the count lists the twelve keys with their
  values and inherited values, each with its own revert, plus "Revert all"
  (`PartPlate::clear_process_overrides` — user-invoked only, exactly as today).
- Values that arrived by translation (carried intent, §3) are ordinary L2 deviations and
  display identically. Provenance ("carried from *Bambu P1S* profile") appears only in the
  reception report (§6), not as a per-control badge — a plate does not wear its history.

### What is deleted

- `Plater::save_plate_process_as_preset` and its menu entry. A plate's deviation is already
  persistent, already travels in the 3MF, already duplicates with the plate. Cross-project
  reuse is plate-level: duplicate the plate, or import the plate from the old project
  (§3, §6). No name dialog, ever.
- The Tab's save/dirty ceremony for process and filament. There is no dirty state: an edit
  IS the plate's state the moment it is made (this is already true through
  `write_plate_process_options`; the remaining preset-save affordances come off the
  surface). `UnsavedChangesDialog` remains only where a machine settings edit is about to
  be discarded by an explicit user action.
- "Transfer settings" flows and the select-preset dialogs that serve them.

---

## 2. Inheritance — one table, every event

The single generalising rule: **a plate is a copyable unit — context plus deviation — and
every event either copies it whole, or translates it whole. Nothing ever partially copies a
plate.** Empty is not inheritance; empty is unresolved and is said out loud
(`PlateSlicingContext` doctrine).

| Event | Printer | Process (L0 name) | Plate deviation (L2) | Slots + colours | Objects | Retained slice |
|---|---|---|---|---|---|---|
| **New plate** | copy of current plate | copy of current plate | **copy of current plate** | copy of current plate | none | none |
| **Duplicate plate** | copy of source | copy of source | copy of source | copy of source | deep-copied | not copied; reslices |
| **Plate changes printer** | the new target | kept if it runs on the target, else target's declared default | kept; carried intent merged **under** existing keys (a deviation the user already made on this plate outranks a carried one) | each slot: kept if it runs, else translated by material, else named unresolved | untouched (slot indices stable) | dropped, reason recorded |
| **Project import, printer installed** | as authored | as authored (renamed with project decoration; referrers renamed — `load_config_file_config`) | as authored | as authored | as authored | kept if the freshly composed context matches (`m_sliced_config`) |
| **Project import, foreign printer** | auto-retargeted (§6); authored identity recorded | target default + carried intent | carried intent (per §3 classes) | translated by material; refusals named | untouched | dropped, reason recorded |
| **Object moves between plates** | n/a | n/a | destination's, unchanged | destination's; the object's materials are mapped into matching slots by spool identity, adding a slot only when no slot holds that spool; instances detach when they need different mappings (already built) | object L3 overrides: intent classes travel verbatim; machine-class keys are re-derived when source and destination machines differ, counted in one notification | both plates invalidated |

Details that keep the table honest:

- **New plate** is `Plater::priv::on_action_add_plate`. It already seeds the context from
  the current plate; it must ALSO copy the current plate's L2 deviation. This is the worked
  example that defines the feature: tune plate 1 on printer X, add a plate, the new plate
  slices identically. §7 item 3. ("Another one like this" — the seed is the plate the user
  is standing on, never the editing cursor.)
- **Duplicate** (`PartPlateList::duplicate_plate`) already copies context + `m_config` +
  name. It stays the reference implementation of "a plate is a copyable unit".
- **Printer change** is `PresetBundle::reresolve_plate_context_for_printer` +
  `carry_process_intent` + `translate_filament_to_printer`, called from every plate-printer
  write path. No other path may change a plate's printer.
- **Physical printer id** is never inherited and never completed
  (`complete_plate_context` excludes it): which unit on the network prints a plate is a
  dispatch decision made at send time, and two identical K1 Maxes should be chosen between
  when the job is dispatched, not when the plate is born.
- **Completion** (`PresetBundle::complete_plate_context(context, seed)`) exists for exactly
  two moments — a plate just created, and a pre-fork project — and the seed is always an
  argument, never a global read. Placeholder (`is_default`) presets are never accepted from
  a seed. Unchanged from the tree; recorded here because it is load-bearing.

Flowchart form of the same table ships in the published architecture page; the table is
normative.

---

## 3. Classification of process options — what survives a change of machine

Six classes. The test for the boundary: *standing at a different machine, would the same
person make the same choice?* If the choice is about the **object** (what gets made), it
travels. If it is about the **motion system or firmware** (how this machine achieves it),
it is re-derived from the target's vendor default. If it is object intent **denominated in
machine-bounded units**, it travels and is clamped into the target's valid range.

| Class | Name | Rule on printer change |
|---|---|---|
| **I** | Object intent | Carried verbatim as plate deviation (when it differs from the target's default). |
| **C** | Clamped intent | Carried; if outside the target's valid range (nozzle-derived), clamped to the nearest valid bound and counted in the report. Clamping, not re-deriving: 0.10 mm layers authored for fine detail become the target's finest layer, not its default mid-range. |
| **M** | Machine tuning | Never carried. The target's vendor default applies. Counted (not listed) in the report. The authored values are not lost — the source profile stays embedded in the 3MF. |
| **F** | Slot-indexed intent | Carried, with slot indices remapped alongside the slot list when slots merge or renumber. |
| **P** | Plate-owned | Never carried by translation — the plate already owns a control for it (`is_plate_owned_process_setting`): `curr_bed_type`, `print_sequence`, `first_layer_print_sequence`, `other_layers_print_sequence`, `other_layers_print_sequence_nums`, `spiral_mode`, `filament_map_mode`, `filament_map`, `filament_volume_map`. |
| **B** | Bookkeeping | Never carried: `compatible_printers(_condition)`, `compatible_prints(_condition)`, `inherits`, `print_settings_id`, `printer_settings_id`. Already excluded in `carry_process_intent`. |

**This table supersedes carry-all for parentless sources.** Today `carry_process_intent`
carries every process option when the source has no parent (the embedded-import case —
which is the 90% case). With the table, the parent is unnecessary for the I/M split: M-class
keys are never carried, parent or no parent; I/C/F keys are carried when they differ from
the target's value. The one behaviour the parent still improves is skipping I-class keys
the author never touched, which the diff gives us for free when a parent exists.
Ratio-carry for M-class (carry a speed as a fraction of the source default) was considered
and rejected: for an embedded import the source default is unknown, so the ratio is
undefined exactly where it would be needed.

### The table, page by page (Process tab, `TabPrint::build`)

Class per group; every exception named. Keys are the real config keys.

**Quality**

| Group | Class | Exceptions |
|---|---|---|
| Layer height (`layer_height`, `initial_layer_print_height`) | **C** | — |
| Line width (all 9 `*_line_width`) | **C** | — |
| Seam (`seam_position`, `staggered_inner_seams`, `seam_gap`, scarf-joint family `seam_slope_*`, `scarf_*`) | **I** | `role_based_wipe_speed`, `wipe_speed` → **M** (wipe motion is machine tuning); `wipe_on_loops`, `wipe_before_external_loop` → **I** |
| Precision (`slice_closing_radius`, `resolution`, `enable_arc_fitting`, `xy_hole_compensation`, `xy_contour_compensation`, `elefant_foot_*`, `precise_outer_wall`, `precise_z_height`, `hole_to_polyhole*`) | **I** | `enable_arc_fitting` → **M** (firmware capability: G2/G3 support) |
| Ironing (`ironing_*`) | **I** | — |
| Z contouring (`zaa_*`) | **I** | — |
| Wall generator (`wall_generator`, `wall_transition_*`, `wall_distribution_count`, `min_feature_size`, `min_length_factor`, `wall_maximum_*`) | **I** | `min_bead_width`, `initial_layer_min_bead_width` → **C** (nozzle-denominated) |
| Walls and surfaces (`wall_sequence`, `is_infill_first`, `wall_direction`, `only_one_wall_*`, `min_width_top_surface`, `reduce_crossing_wall`, `max_travel_detour_distance`, `small_area_infill_flow_compensation*`) | **I** | the surface flow-ratio family (`print_flow_ratio`, `top_solid_infill_flow_ratio`, `bottom_solid_infill_flow_ratio`, `*_flow_ratio` in this group) → **I** — deliberate: machine/material flow calibration lives in the filament layer and is re-derived by filament translation; the process-level ratios are the author's surface intent on top of it |
| Bridging (`bridge_flow`, `internal_bridge_flow`, `bridge_density`, `thick_bridges`, `counterbore_hole_bridging`, …) | **I** | — |
| Overhangs (`detect_overhang_wall`, `make_overhang_printable*`, `extra_perimeters_on_overhangs`, `overhang_reverse*`) | **I** | — |

**Wave overhangs** — the page proves the classes cut across pages:

| Group | Class |
|---|---|
| General / Detection / Pattern / Corner reinforcement / Floor layers (geometry strategy keys) | **I** |
| Motion (`wave_overhang_print_speed`, `wave_overhang_perimeter_speed`, `wave_overhang_travel_speed`, `wave_overhang_end_retract_length`) | **M** |
| Cooling (`wave_overhang_fan_speed`, `wave_overhang_aux_fan_speed`, `wave_overhang_nozzle_temp`, `wave_overhang_min_wave_time`, `wave_overhang_min_layer_time`) | **M** |
| Floor-layer speed/fan sub-keys (`wave_overhang_floor_*speed*`, `wave_overhang_floor_*fan*`) | **M** |
| Debug (`wave_overhang_debug_gcode`) | **M** |

**Strength** — the whole page is **I**: walls (`wall_loops`, `alternate_extra_wall`,
`detect_thin_wall`), top/bottom shells (all), infill (all patterns, densities, directions,
anchors, locked-zag family, `infill_wall_overlap`), advanced (`bridge_angle`,
`infill_combination*`, `ensure_vertical_shell_thickness`, …). No exceptions. This page is
the purest statement of what a person means by "how I want this object".

**Speed** — the whole page is **M**: first-layer and other-layer speeds, overhang speeds,
travel, all `*_acceleration`, `default_junction_deviation`, all `*_jerk`,
`max_volumetric_extrusion_rate_slope*`. No exceptions. A speed is a request against a
motion system; 200 mm/s authored for a CoreXY is not a choice anyone made about a BigRep.

**Support**

| Group | Class | Exceptions |
|---|---|---|
| Support (`enable_support`, `support_type`, `support_style`, `support_threshold_angle`, `support_on_build_plate_only`, `support_critical_regions_only`, `support_remove_small_overhang`, …) | **I** | — |
| Raft (`raft_layers`, `raft_contact_distance`, `raft_first_layer_*`) | **I** | — |
| Filament for supports (`support_filament`, `support_interface_filament`, `support_interface_not_for_body`) | **F** | — |
| Support ironing (`support_ironing*`) | **I** | — |
| Advanced (z distances, interface layers/pattern/spacing, `support_expansion`, `support_object_xy_distance`, `bridge_no_support`, `max_bridge_length`, `independent_support_layer_height`) | **I** | `support_line_width` (listed under Quality) already **C** |
| Tree supports (`tree_support_*`) | **I** | — |

**Multimaterial**

| Group | Class | Exceptions |
|---|---|---|
| Prime tower (`enable_prime_tower`, `prime_tower_width`, `prime_volume`, `wipe_tower_*`) | **M** | `enable_prime_tower` itself → **I** (whether to purge into a tower is a choice about the print; the tower's geometry and speeds are machine tuning) |
| Filament for features (`outer_wall_filament_id`, `inner_wall_filament_id`, `sparse_infill_filament_id`, `internal_solid_filament_id`, `top_surface_filament_id`, `bottom_surface_filament_id`, `wipe_tower_filament`) | **F** | — |
| Ooze prevention (`ooze_prevention`, `standby_temperature_delta`, `preheat_*`) | **M** | — |
| Flush options (`flush_into_infill`, `flush_into_objects`, `flush_into_support`) | **I** | — |
| Advanced (`interlocking_*`, `interface_shells`, `mmu_segmented_region_*`, `toolchange_ordering`) | **I** | `toolchange_ordering` → **M** (toolchange mechanics) |

**Others**

| Group | Class | Exceptions |
|---|---|---|
| Skirt (`skirt_*`, `min_skirt_length`, `draft_shield`, `single_loop_draft_shield`) | **I** | `skirt_speed` → **M** |
| Brim (`brim_*`, `combine_brims`) | **I** | — |
| Special mode | **P** for `print_sequence`, `spiral_mode` (plate-owned); **I** for `slicing_mode`, `print_order`, `spiral_mode_smooth`, `spiral_mode_max_xy_smoothing`, `spiral_starting_flow_ratio`, `spiral_finishing_flow_ratio`; **M** for `timelapse_type`, `enable_wrapping_detection` (machine capabilities) | — |
| Fuzzy skin (`fuzzy_skin*`) | **I** | — |
| G-code output (`reduce_infill_retraction`, `gcode_add_line_number`, `gcode_comments`, `gcode_label_objects`, `exclude_object`, filename format) | **M** — all firmware-coupled | — |
| Change-extrusion-role G-code, post-processing scripts, slicing-pipeline plugin | **M** — scripts and role G-code authored for one machine can corrupt another's output; dropped and **counted by name** in the report (a script is a big thing to drop silently) | — |
| Notes | **I** | — |

**Long tail rule** for keys not listed (new upstream options arriving by rebase): default
class is **I** when the key describes geometry/strategy, **M** when its unit is mm/s,
mm/s², seconds, °C, or it names firmware behaviour, **C** when its default is expressed as
a percentage of nozzle diameter. Every new key gets an explicit class at rebase time; the
class lives in one table in code (extend `is_plate_owned_process_setting`'s home,
`PlateSlicingContext.hpp`, with `process_option_class(key)`) so `carry_process_intent`,
the reception report and the object-move rule cannot disagree.

---

## 4. The filament model — pool, slots, objects, facets

Four levels, linked in one direction:

```
INVENTORY (app)      spools you own: material + colour + appearance. No printer, no project.
    │  assign (translates for the plate's printer; refusal is named)
    ▼
PLATE SLOTS          the plate's own bays: filament preset name + colour + appearance,
                     held BY VALUE (PlateSlicingContext). Pool edits never touch them.
    │  reference (1-based slot index)
    ▼
OBJECT / PART        each object/part names a slot index (extruder property)
    │  paint (per-triangle slot states, never clipped)
    ▼
FACETS               MMU segmentation states
```

Rules, each of which an engineer can test:

1. **The inventory is app-level.** Today the pool is `PresetBundle::filament_presets` plus
   parallel colour vectors in `project_config` — project-scoped, reconstructed at render
   time from vectors with independent lifecycles. It becomes one owned list of row structs
   `{id, material (filament_type + vendor family), colour, colour_type, multi_colour,
   finish, label}`, persisted in the app (not the 3MF), edited only through the sidebar.
   A 3MF carries plate slots by value and NO pool; importing a project therefore never
   creates, edits, or deletes an inventory row. This retires the phantom-colour class of
   bug by construction: a swatch can only render from a row, and every row has edit and
   delete. Migration: on first run, seed the inventory from the current
   `filament_presets` + colours.
2. **Assignment is translation.** Choosing a spool for a plate slot resolves it for that
   plate's printer via `PresetBundle::assign_plate_material` →
   `translate_filament_to_printer` (declaration-ordered: printer's declared default of that
   material → same vendor → any compatible → base material before the first space). When no
   preset of that material exists for the machine, the refusal names the material and the
   machine and changes nothing.
3. **Default scope is silent.** Assigning a spool to a slot applies to that plate+slot,
   with no dialog. The scope quiz in `Plater::choose_plate_material_replacement` comes off
   the primary path: broader scopes ("this preset everywhere on this plate / selected
   plates / project") move behind an explicit right-click "Replace across…" on the slot
   swatch. Rationale: the hot path is once-per-plate daily; the broad path is rare and
   deliberate.
4. **A slot is a bay; a spool is what is loaded.** Two slots holding the same spool are one
   filament to the print (`PlateSlicingContext::single_spool_slot`, applied only through
   `PartPlate::get_printing_context`). Every consumer of "how many filaments" asks there.
5. **Single-tool machines look single-tool.** A plate on a single-tool printer is born with
   one slot. Extra slots may be added (manual M600-style swaps are real) from the strip's
   trailing "+", and the last unused slot can be removed. Multi-material furniture — prime
   tower preview, flushing matrix, filament maps — appears only when the PRINTING context
   (post-cut) holds more than one spool, never merely because the slot list is long.
6. **Monotone paint collapses.** When a paint-gizmo commit leaves an object's facets in a
   single uniform state, the commit is rewritten as the object's slot assignment (extruder
   property) and the facet data is cleared. Painting an object entirely black is the same
   statement as assigning it to the black slot, and must leave the same state behind.
   (`GLGizmoMmuSegmentation` commit path.)
7. **The single-spool cut remaps paint instead of bypassing it.** Today
   `single_spool_slot` returns 0 (no cut) whenever paint exists above slot 1
   (`painted_above_slot_one`), so a plate painted monotone in slot 2 slices as a
   multi-filament print and then trips the flushing/channel checks — the reported
   "one-colour plate refuses to slice" defect. With rule 6 most such paint no longer
   exists; for genuinely painted plates whose used slots all hold one spool, the cut
   proceeds and paint states are remapped to slot 1 in the cut context. The invariant
   "facet states are never clipped" holds — remapping to the only slot is not clipping.
8. **Pool edits never touch plates** (already the rule): adding/removing/recolouring an
   inventory row changes no plate, no object, no facet. Deleting the last row is allowed
   (an empty inventory is a fact); the delete affordance is never a silent no-op — see §8
   law 3.
9. **Colour is material information.** A slot swatch shows colour AND material
   (short label: "PETG · black"). The plate-board slot menu lists inventory rows first,
   swatch and material both visible, translated for the plate's machine, with untranslatable
   rows shown disabled with the reason — never hidden (already built; kept as law).

---

## 5. Slicing and the plate board (context for the rules above)

- Composition: `resolve_plate_presets` (tier 1: exact names, FDM gate, vendor guard) →
  `compose_plate_slicing_config` (tier 2: full config, project overrides, plate colour
  vectors sized to the plate — the colour-count invariant) → plate overrides applied by the
  caller → object configs by the engine.
- Every plate keeps its finished slice until its composed context changes
  (`m_sliced_config` snapshot comparison); pool edits and unrelated preset refreshes do not
  invalidate (already built, 10 Sep).
- The board is the fleet surface: rows grouped by machine, per-row state mark (sliced /
  stale / dropped), drag between machine groups is a printer change (translation), never a
  reset. The dashed-vs-solid error edge distinguishes "does not compose" from "printer not
  installed".

---

## 6. Import reconciliation — double-click to slice-ready

The pipeline, in order. Steps marked SILENT never surface anything; REPORT lands in the
single reception report; nothing asks. **No modal may be raised anywhere in
`Plater::priv::load_files`** — the informational origin messages are already notifications;
the remaining printer-offer dialog is deleted (it is also the reported crash site, and it
was redundant before it was broken: per-plate retargeting is strictly more capable).

1. SILENT — Parse; classify origin (Orca / BambuStudio / Prusa tags, version). Geometry,
   plates, per-plate contexts, painted facets, embedded presets all load. Nothing on this
   path throws on an unresolved plate (established rule).
2. SILENT — Embedded presets install, renamed with the project decoration; renames rename
   referrers (`load_config_file_config` completes `compatible_printers`).
3. SILENT — Declared seed captured (bundle selection immediately after
   `load_config_model` — the file's own declaration resolved to installed names); legacy
   colour vectors migrated; `complete_plate_contexts(declared)` fills only legacy-empty
   fields; placeholder presets never accepted.
4. SILENT — Per plate, in index order:
   a. Printer installed → resolve; done.
   b. Printer not installed → **auto-retarget**: record the authored context verbatim as
      provenance on the plate (new field pair: `authored_printer` / kept embedded
      profiles), then `reresolve_plate_context_for_printer` onto the **reception target**.
      The reception target is chosen deterministically: the owned machine whose bed fits
      the plate's occupied footprint, tie-broken by fewest dropped/clamped keys in a dry
      translation, then by most recently used machine. Deterministic and explainable — the
      report says which rule chose.
   c. Filament slots: kept / translated by material / named unresolved (existing rules).
      Imported plate-level material overrides rebased for changed slots (existing rule).
5. SILENT — Retained G-code: kept per plate when the freshly composed context matches the
   snapshot; otherwise dropped with the reason recorded on the plate
   (`m_sliced_config_dropped_reason`).
6. REPORT — One notification, counts not lists: "Opened for Bambu P1S. 3 plates retargeted
   to K2 Pro · 14 settings carried, 6 re-derived, 1 clamped · 2 spools translated. Review".
   "Review" opens the reception report anchored to the board: per plate, what was kept,
   carried, clamped, re-derived (counts expandable to keys), spool translations, anything
   unresolved. Every retarget row has "Change machine…" (the picker) and the whole
   reception has one Undo (restores authored contexts; plates go unresolved-but-named).
7. The plate is slice-ready. Anything still unresolved (a material that exists on no owned
   machine) is a named board-row state with a click-through fix, not a gate on the rest of
   the project.

The strong prior, stated as the rule: **the app decides silently and reports afterwards; it
does not ask.** The only questions left in the product are ones whose wrong answer changes
what gets made AND cannot be undone (there are none on the import path).

---

## 7. What must change — file and function

Actionable contradictions between the current tree and this document:

1. **`src/slic3r/GUI/PlateSettingsDialog.cpp`** — ctor (~line 431) appends
   "Same as Project Printer" as entry 0 and treats empty selection as "follow the project
   printer"; `sync_printer_preset` / `get_printer_preset_choice` keep the offset. There is
   no project printer. Entry 0 goes; an unresolved plate shows an explicit "No printer —
   unresolved" state; empty can never be produced by OK.
2. **`src/slic3r/GUI/PlateBoard.cpp`** — `end_drag` (~2344): message branches
   "already follow the project printer" for an empty machine group. Empty machine = an
   unresolved plate group; the message should say that and link the fix.
3. **`src/slic3r/GUI/Plater.cpp`** — `Plater::priv::on_action_add_plate` (~11951): seeds
   context only. Must also copy the current plate's L2 deviation (`m_config`, non-plate-owned
   keys) so a new plate arrives as "another one like this" (§2 row 1).
4. **`src/slic3r/GUI/Plater.cpp`** — `Plater::save_plate_process_as_preset` (~20943) and
   its menu entry: delete (§1). Same pass removes the process/filament save-preset
   affordances from `Tab`.
5. **`src/slic3r/GUI/Plater.cpp`** — `Plater::choose_plate_material_replacement` (~21097):
   the scope dialog moves off the primary assignment path (§4 rule 3).
6. **`src/slic3r/GUI/Plater.cpp`** — `Sidebar::delete_filament` (~4264): silent early
   returns (`combos_filament.size() <= 1`, out-of-range id). Every refusal says why (§8
   law 3). `add_custom_filament` cap likewise.
7. **Import modal** — the printer-offer dialog raised on opening a foreign 3MF (the crash
   site): delete; replaced by §6 step 4b + report. Enforcement: no `ShowModal` reachable
   from `Plater::priv::load_files`.
8. **`src/libslic3r/PlateSlicingContext.hpp`** — `single_spool_slot`'s
   `painted_above_slot_one` bypass becomes a paint-state remap in the cut (§4 rule 7);
   **`GLGizmoMmuSegmentation`** commit gains monotone collapse (§4 rule 6).
9. **Pool storage** — `PresetBundle::filament_presets` + `project_config` colour vectors as
   the pool's storage: replaced by the app-level inventory row list (§4 rule 1). Sidebar,
   plate-board slot menu and pickers read rows, not parallel vectors.
10. **`PresetBundle::carry_process_intent`** — carry-all-when-parentless replaced by the
    class table (§3); classes live beside `is_plate_owned_process_setting` as
    `process_option_class(key)` so every consumer agrees.
11. **Machine/filament deviations** — UI renames "printer preset save" to machine settings
    (app-level deviation, no naming ceremony); per-(material, machine) filament deviation
    likewise (§1).

---

## 8. Codes of usability

Each law is falsifiable: point at a screen and name the violation.

1. **Opening a project never asks.** Any modal between double-click and slice-ready is a
   bug. Decisions are made silently, reported once, undoable once.
2. **The screen you see is the plate you edit.** Every preset control, settings tab and
   swatch describes and writes the current plate. Nothing edits a machine that is not on
   screen. (`follow_plate_presets` is the mechanism; a control that writes elsewhere
   violates this law.)
3. **No silent no-ops.** Every click changes something or says why it cannot. A disabled
   control carries its reason. (The dead delete on the last pool row, and disabled slot-menu
   entries, are the canonical cases.)
4. **Nothing gates slicing on a maybe.** Hard refusal only when emitting the G-code would
   damage the machine or the request is impossible; everything else informs, names the
   object, and gets out of the way.
5. **Every non-default value is visibly non-default,** shows the value it displaced on
   hover, and reverts in one click. If a user cannot answer "what did I change on this
   plate?" in one glance at the header count, this law is broken.
6. **A change of machine never silently discards a choice.** Every authored value is
   carried, clamped-with-report, re-derived-with-count, or named as unresolved. "It reset"
   is always a bug.
7. **Counts, not lists.** A message may name at most one thing; more than one becomes a
   count with an expandable detail. A named object is clickable and selects itself.
8. **One fact, one place.** The swatch is the colour control; the board row is the machine
   control; a second surface showing the same fact must be the same control, not a copy.
9. **No save ceremonies.** Work persists where and when it is made. Any dialog asking to
   name, save, or transfer settings is legacy furniture and a bug.
10. **Single-tool machines never wear AMS furniture.** Prime tower, flushing, maps appear
    only when the printing context holds more than one spool.
11. **Unresolved is a state, not a wall.** An unresolved plate is named on the board with a
    click-through fix and blocks nothing else in the project.
12. **Every autonomous decision is undoable and inspectable.** The reception report is the
    pattern: what was decided, by which rule, one Undo.

---

*Companion page (diagrams, same rules): published artifact "Podslicer Interaction
Architecture". Source of truth for behaviour: this file. 2026-09-14.*
