#ifndef slic3r_PlateSlicingContext_hpp_
#define slic3r_PlateSlicingContext_hpp_

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

// The process settings a PLATE has always owned a control of its own for, and the ONE
// definition of that set. Two sides read it and they must never disagree:
//
//  - PresetBundle::carry_process_intent leaves these out of the candidate set, so a change
//    of machine never carries one across as "intent".
//  - PartPlate::process_override_keys / _count / clear_process_overrides leave them out of
//    the count the inspector shows and out of what "clear" clears.
//
// While those were two lists, print_sequence and spiral_mode sat in one and not the other:
// a machine change carried them onto the plate as overrides that nothing counted and
// nothing could clear. print-by-object is also not translatable in the first place - a
// by-object plate needs its toolhead collisions resolved for the new machine's kinematics,
// and copying the flag is not that.
inline bool is_plate_owned_process_setting(const std::string &key)
{
    static const std::set<std::string> own_controls = {
        "curr_bed_type", "print_sequence", "first_layer_print_sequence",
        "other_layers_print_sequence", "other_layers_print_sequence_nums",
        "spiral_mode", "filament_map_mode", "filament_map", "filament_volume_map",
    };
    return own_controls.count(key) > 0;
}

// A plate's complete slicing identity, owned by the plate and by nothing else.
//
// There is no project printer to fall back to. An empty field is NOT inheritance: it is
// a plate that has not been given one yet, which happens in exactly two places - a plate
// that has just been created, and a project written before per-plate machines. Both are
// completed at once by PresetBundle::complete_plate_context, and after that every field
// is a name that must resolve exactly.
//
// The distinction matters because "empty means the project's" was the whole of the
// singular engine. It let one global selection stand behind every plate that had not
// been told otherwise, so a project could hold several printers in its data and still
// slice on one. Deleting the meaning is what deletes the state.
struct PlateSlicingContext
{
    std::string              printer_preset_name;
    std::string              printer_vendor_id;
    std::string              print_preset_name;
    std::vector<std::string> filament_preset_names;
    // Colour is material information and the plate owns its materials, so the plate owns
    // the colours too - one per slot, parallel to filament_preset_names. Before this the
    // only colour store was the project-wide filament_colour vector, sized to the global
    // slot count, which recoloured every plate at once and made a one-filament plate look
    // four filaments wide to the engine. An empty entry means "no colour chosen for this
    // slot"; composition fills it from the preset's own default. The vector may be shorter
    // than the slot list (older projects); it is never longer.
    std::vector<std::string> filament_colours;
    std::string              physical_printer_id;
    // Spool appearance belongs to the same plate slot as its material and colour.
    // Empty vectors are legacy data; composition supplies solid, standard defaults.
    std::vector<std::string> filament_colour_types;
    std::vector<std::string> filament_multi_colours;
    std::vector<int>         filament_finishes;

    // Everything a slice needs to be named. printer_vendor_id is excluded: it is a
    // guard recorded against the printer name, not an identity of its own, and an
    // empty one means "no vendor was recorded", which resolves fine.
    // physical_printer_id is excluded too - which machine on the network prints this
    // is a dispatch question, and a plate slices without an answer to it.
    bool is_complete() const
    {
        return !printer_preset_name.empty() && !print_preset_name.empty() && !filament_preset_names.empty();
    }

    // --- The spools a plate actually prints with -------------------------------------
    //
    // A slot is a bay; a spool is what is loaded in it. Two slots holding the same spool -
    // same preset, same colour, same appearance - are one filament to the print: no tool
    // change between them changes anything the nozzle lays down, so there is nothing to
    // flush and nothing to prime. Before this rule the engine counted SLOTS, so an all-black
    // plate whose parts sat in slots 1 and 2 (both black) was a two-filament print: a prime
    // tower appeared, the flushing matrix was consulted, and the zero in it blocked slicing.
    // "Every part is black" is the whole of what the user sees, and it has to be the whole
    // of what the engine sees.

    // What is loaded in slot i (0-based).
    //
    // The fields are NORMALISED exactly as PresetBundle::apply_plate_filament_colours normalises
    // them when it writes the composed config, because the question this answers is "will these
    // two slots put the same thing on the bed", and the bed sees the composed values. A slot
    // whose colour_type was never recorded and one recorded as "" both compose to solid "1", so
    // comparing the raw cells made two identical spools read as different and kept a one-colour
    // plate on the multi-filament path - the same defect one layer down.
    //
    // The one field that cannot be normalised here is the colour: an empty cell means "this
    // material's own default", and only the filament preset knows that. The caller resolves it
    // first - PresetBundle::plate_filament_colours is the one definition, and it is the same
    // answer composition reaches. See PartPlate::get_printing_context.
    struct Spool
    {
        std::string preset_name;
        std::string colour;
        std::string colour_type;
        std::string multi_colours;
        int         finish = 0;
        bool operator==(const Spool &rhs) const
        {
            return preset_name == rhs.preset_name && colour == rhs.colour && colour_type == rhs.colour_type &&
                   multi_colours == rhs.multi_colours && finish == rhs.finish;
        }
        bool operator!=(const Spool &rhs) const { return !(*this == rhs); }
    };

    Spool spool_in_slot(size_t i) const
    {
        Spool spool;
        if (i < filament_preset_names.size()) spool.preset_name = filament_preset_names[i];
        if (i < filament_colours.size()) spool.colour = filament_colours[i];
        if (i < filament_colour_types.size()) spool.colour_type = filament_colour_types[i];
        if (spool.colour_type.empty()) spool.colour_type = "1";       // composition: solid
        if (i < filament_multi_colours.size()) spool.multi_colours = filament_multi_colours[i];
        if (spool.multi_colours.empty()) spool.multi_colours = spool.colour; // composition: the colour itself
        if (i < filament_finishes.size()) spool.finish = filament_finishes[i];
        return spool;
    }

    // The ONE slot this plate prints with, or 0 when it prints with several spools.
    //
    // used_slots are the 1-based slots the plate's objects reference (PartPlate::get_extruders
    // in the GUI, get_extruders_under_cli in the CLI - custom G-code tool changes included).
    // Both already include the slots per-triangle MMU painting names, because
    // ModelVolume::get_extruders merges mmuseg_extruders with the volume's own slot. So paint
    // needs no separate veto here: paint in a slot holding a DIFFERENT spool is caught by the
    // comparison below like any other reference, and paint in a slot holding the same spool
    // changes nothing the nozzle lays down.
    //
    // It used to carry one - "the plate is painted at all, so keep the full width" - on the
    // grounds that stored paint states are slot numbers, are never clipped, and would index
    // MultiMaterialSegmentation's per-facet arrays out of range once the width was cut. That is
    // not what the engine does. Both readers of paint are gated on the PRINTING filament count,
    // not on the stored states: PrintObject::slice_volumes skips apply_mm_segmentation entirely
    // unless filament_diameter.size() > 1, and PrintApply's painting_extruders stays empty
    // unless num_extruders > 1. At one filament nothing reads a painted state at all, which is
    // also why a plate that has only ever had one slot already slices painted today. The veto
    // protected against a hazard that does not exist and cost the bug it was reported as: a
    // plate painted monotone in a single colour refused to slice, because "painted at all" was
    // being passed where "painted in a second spool" was meant.
    //
    // The answer is the lowest used slot when every used slot holds the same spool, and 0
    // otherwise. A context with one slot is already single; the caller has nothing to cut.
    int single_spool_slot(std::vector<int> used_slots) const
    {
        if (filament_preset_names.size() <= 1) return 0;
        // A reference to a slot this plate does not have is stale data, not a spool: there is
        // nothing loaded in it to compare. Dropping it is the same answer the engine reaches
        // (clamp_feature_filament_to_valid for a config reference, and a painted state above
        // the width is never read), so it is not a substitution - it is agreeing with the
        // engine about which slots exist.
        used_slots.erase(std::remove_if(used_slots.begin(), used_slots.end(),
                                        [this](int s) { return s < 1 || s > int(filament_preset_names.size()); }),
                         used_slots.end());
        std::sort(used_slots.begin(), used_slots.end());
        used_slots.erase(std::unique(used_slots.begin(), used_slots.end()), used_slots.end());
        if (used_slots.empty()) return 0;
        const Spool first = spool_in_slot(size_t(used_slots.front() - 1));
        for (int slot : used_slots)
            if (spool_in_slot(size_t(slot - 1)) != first) return 0;
        return used_slots.front();
    }

    // The same context reduced to one slot (1-based), every filament-indexed vector born
    // single-entry. Object references to any other slot then clamp to the only entry in
    // the engine (clamp_feature_filament_to_valid), which is the same spool by construction.
    PlateSlicingContext cut_down_to_slot(int slot) const
    {
        PlateSlicingContext cut = *this;
        const Spool spool = spool_in_slot(size_t(slot - 1));
        cut.filament_preset_names  = {spool.preset_name};
        cut.filament_colours       = {spool.colour};
        cut.filament_colour_types  = {spool.colour_type};
        cut.filament_multi_colours = {spool.multi_colours};
        cut.filament_finishes      = {spool.finish};
        return cut;
    }

    // One entry of a per-slot map (filament_map / filament_volume_map) kept, the rest dropped;
    // a map too short to reach the slot keeps the caller's default.
    static std::vector<int> cut_map_to_slot(const std::vector<int> &map, int slot, int fallback)
    {
        return {size_t(slot) <= map.size() ? map[size_t(slot - 1)] : fallback};
    }

    bool operator==(const PlateSlicingContext &rhs) const
    {
        return printer_preset_name == rhs.printer_preset_name &&
               printer_vendor_id == rhs.printer_vendor_id &&
               print_preset_name == rhs.print_preset_name &&
               filament_preset_names == rhs.filament_preset_names &&
               filament_colours == rhs.filament_colours &&
               physical_printer_id == rhs.physical_printer_id &&
               filament_colour_types == rhs.filament_colour_types &&
               filament_multi_colours == rhs.filament_multi_colours &&
               filament_finishes == rhs.filament_finishes;
    }

    bool operator!=(const PlateSlicingContext &rhs) const { return !(*this == rhs); }
};

} // namespace Slic3r

#endif
