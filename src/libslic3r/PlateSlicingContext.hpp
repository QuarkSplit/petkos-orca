#ifndef slic3r_PlateSlicingContext_hpp_
#define slic3r_PlateSlicingContext_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// Empty preset names mean explicit inheritance from the Project row. Once a
// field is set, resolving it is exact: callers must never substitute another
// preset when the named preset is unavailable or incompatible.
struct PlateSlicingContext
{
    std::string              printer_preset_name;
    std::string              printer_vendor_id;
    std::string              print_preset_name;
    std::vector<std::string> filament_preset_names;
    std::string              physical_printer_id;

    bool operator==(const PlateSlicingContext &rhs) const
    {
        return printer_preset_name == rhs.printer_preset_name &&
               printer_vendor_id == rhs.printer_vendor_id &&
               print_preset_name == rhs.print_preset_name &&
               filament_preset_names == rhs.filament_preset_names &&
               physical_printer_id == rhs.physical_printer_id;
    }

    bool operator!=(const PlateSlicingContext &rhs) const { return !(*this == rhs); }
};

} // namespace Slic3r

#endif
