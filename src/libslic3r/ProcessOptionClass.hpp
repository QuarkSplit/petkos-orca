#ifndef slic3r_ProcessOptionClass_hpp_
#define slic3r_ProcessOptionClass_hpp_

#include <set>
#include <string>

#include "PlateSlicingContext.hpp"

namespace Slic3r {

// WHAT SURVIVES A CHANGE OF MACHINE.
//
// Moving a plate to another printer is a translation, not a reset: a preset NAME means nothing
// across machines, but the VALUES a person chose mostly do. The question this file answers is
// which ones.
//
// The test for the boundary is one sentence: standing at a different machine, would the same
// person make the same choice? Three walls and 15% gyroid is a decision about the OBJECT, and it
// is the same decision on a Creality, a Prusa or a BigRep. 200 mm/s and 10000 mm/s2 is a request
// against a MOTION SYSTEM, and nobody who typed it for a CoreXY meant it for a 1 m gantry.
//
// Why this exists at all: PresetBundle::carry_process_intent carries the DIFF from the source
// preset's parent, which is right - the base is a vendor's tuning for one machine, the deviation
// is what a person decided, and only the deviation crosses. But a preset embedded in a downloaded
// project has no parent in this installation, and that is the normal case for a download. With no
// parent there is no diff, so the code carried EVERYTHING, and a Bambu's accelerations landed on a
// BigRep as though somebody had chosen them.
//
// The classes are deliberately asymmetric in their confidence. Machine is an enumerated list:
// membership is a claim that a key is a property of the machine, and it is checked key by key.
// Intent is the DEFAULT, because the fork's standing rule is that a value somebody chose is never
// dropped on the floor, and being wrong in that direction carries a value that did not need
// carrying - visible, and named in the reception report - while being wrong the other way
// silently discards a choice. Add to Machine only what is unambiguously about the machine.
enum class ProcessOptionClass
{
    Intent,        // carried verbatim as a plate deviation
    ClampedIntent, // carried, then clamped into the target's valid range
    Machine,       // never carried; the target's own default applies
    SlotIndexed,   // carried, with slot numbers remapped alongside the slot list
    PlateOwned,    // never carried by translation; the plate already owns a control for it
    Bookkeeping,   // never carried; identity and compatibility, not settings
};

namespace detail {

// Identity and compatibility. Carrying any of these would make a preset claim to be, or to run
// on, something it is not.
inline const std::set<std::string> &bookkeeping_options()
{
    static const std::set<std::string> keys = {
        "compatible_printers", "compatible_printers_condition",
        "compatible_prints",   "compatible_prints_condition",
        "inherits",            "print_settings_id",
        "printer_settings_id", "print_settings_name",
    };
    return keys;
}

// Values that ARE a slot number. They travel, but the number has to be remapped when the slot
// list does, or a wall assigned to slot 3 silently becomes a wall assigned to whatever is in
// slot 3 on the other machine.
inline const std::set<std::string> &slot_indexed_options()
{
    static const std::set<std::string> keys = {
        "outer_wall_filament_id",     "inner_wall_filament_id",
        "sparse_infill_filament_id",  "internal_solid_filament_id",
        "top_surface_filament_id",    "bottom_surface_filament_id",
        "support_filament",           "support_interface_filament",
        "wipe_tower_filament",
    };
    return keys;
}

// Object intent denominated in machine-bounded units. 0.10 mm layers authored for fine detail
// are still a request for fine detail on a 0.8 nozzle; the answer is that machine's finest
// layer, NOT its default mid-range. Clamping keeps the intent, re-deriving throws it away.
inline const std::set<std::string> &clamped_intent_options()
{
    static const std::set<std::string> keys = {
        "layer_height",                    "initial_layer_print_height",
        "min_bead_width",                  "initial_layer_min_bead_width",
        "initial_layer_line_width",        "inner_wall_line_width",
        "outer_wall_line_width",           "sparse_infill_line_width",
        "internal_solid_infill_line_width", "skin_infill_line_width",
        "skeleton_infill_line_width",      "top_surface_line_width",
        "support_line_width",              "bridge_line_width",
    };
    return keys;
}

// Properties of the motion system, the thermal system or the firmware. Every entry is here
// because the same person at a different machine would not make the same choice.
//
// Speeds, accelerations, jerks and junction deviation: requests against a motion system.
// Travel: the same.
// Wave-overhang motion and cooling: the geometry strategy on that page is intent and stays
//   intent; its speeds, fan and nozzle temperatures are not.
// Wipe and retraction motion: how this extruder behaves, not what the object should look like.
// enable_arc_fitting: whether the firmware understands G2/G3.
// timelapse_type, single_extruder_multi_material_priming, print_extruder_*: machine features.
// gcode_label_objects, process_change_extrusion_role_gcode: firmware capability and per-machine
//   custom G-code. gcode_comments and gcode_add_line_number are deliberately NOT here - they are
//   output preferences that mean the same thing on every machine, so they travel.
inline const std::set<std::string> &machine_options()
{
    static const std::set<std::string> keys = {
        "bridge_acceleration", "bridge_speed", "default_acceleration", "default_jerk",
        "default_junction_deviation", "enable_arc_fitting", "enable_overhang_speed",
        "gap_infill_speed", "gcode_label_objects", "infill_jerk", "initial_layer_acceleration",
        "initial_layer_infill_speed", "initial_layer_jerk", "initial_layer_speed",
        "initial_layer_travel_acceleration", "initial_layer_travel_jerk",
        "initial_layer_travel_speed", "inner_wall_acceleration", "inner_wall_jerk",
        "inner_wall_speed", "internal_bridge_speed", "internal_solid_infill_acceleration",
        "internal_solid_infill_speed", "ironing_speed", "max_volumetric_extrusion_rate_slope",
        "max_volumetric_extrusion_rate_slope_segment_length", "outer_wall_acceleration",
        "outer_wall_jerk", "outer_wall_speed", "overhang_1_4_speed", "overhang_2_4_speed",
        "overhang_3_4_speed", "overhang_4_4_speed", "print_extruder_id", "print_extruder_variant",
        "process_change_extrusion_role_gcode", "reduce_infill_retraction", "role_based_wipe_speed",
        "scarf_joint_speed", "single_extruder_multi_material_priming", "skirt_speed",
        "slow_down_layers", "small_perimeter_speed", "small_support_perimeter_speed",
        "sparse_infill_acceleration", "sparse_infill_speed", "support_interface_speed",
        "support_speed", "timelapse_type", "top_surface_acceleration", "top_surface_jerk",
        "top_surface_speed", "travel_acceleration", "travel_jerk", "travel_speed",
        "travel_speed_z", "wave_overhang_aux_fan_speed", "wave_overhang_debug_gcode",
        "wave_overhang_end_retract_length", "wave_overhang_fan_speed",
        "wave_overhang_floor_aux_fan_speed", "wave_overhang_floor_fan_speed",
        "wave_overhang_floor_perimeter_speed", "wave_overhang_floor_print_speed",
        "wave_overhang_floor_speed_ramp", "wave_overhang_flow_mm3_per_mm",
        "wave_overhang_min_layer_time", "wave_overhang_min_wave_time",
        "wave_overhang_nozzle_temp", "wave_overhang_perimeter_speed", "wave_overhang_print_speed",
        "wave_overhang_travel_speed", "wipe_speed", "wipe_tower_max_purge_speed",
        "wiping_volumes_extruders",
    };
    return keys;
}

} // namespace detail

// The class of one process option. Order matters: a key that is plate-owned is plate-owned
// whatever else it looks like, and identity is never a setting.
inline ProcessOptionClass process_option_class(const std::string &key)
{
    if (detail::bookkeeping_options().count(key) > 0)
        return ProcessOptionClass::Bookkeeping;
    if (is_plate_owned_process_setting(key))
        return ProcessOptionClass::PlateOwned;
    if (detail::slot_indexed_options().count(key) > 0)
        return ProcessOptionClass::SlotIndexed;
    if (detail::machine_options().count(key) > 0)
        return ProcessOptionClass::Machine;
    if (detail::clamped_intent_options().count(key) > 0)
        return ProcessOptionClass::ClampedIntent;
    return ProcessOptionClass::Intent;
}

// Does this value travel to another machine at all? The single question the translation path
// asks. Clamping the ClampedIntent ones into the target's range is a separate step and belongs
// to the caller that knows the target.
inline bool process_option_survives_printer_change(const std::string &key)
{
    switch (process_option_class(key)) {
    case ProcessOptionClass::Intent:
    case ProcessOptionClass::ClampedIntent:
    case ProcessOptionClass::SlotIndexed:
        return true;
    case ProcessOptionClass::Machine:
    case ProcessOptionClass::PlateOwned:
    case ProcessOptionClass::Bookkeeping:
        return false;
    }
    return true;
}

} // namespace Slic3r

#endif // slic3r_ProcessOptionClass_hpp_
