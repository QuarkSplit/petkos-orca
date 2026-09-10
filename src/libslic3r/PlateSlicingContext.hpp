#ifndef slic3r_PlateSlicingContext_hpp_
#define slic3r_PlateSlicingContext_hpp_

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
