#pragma once

#include "libslic3r/PresetBundle.hpp"

namespace Slic3r::GUI {

// Process editing needs the named process and project layer, even while a material
// assignment is unresolved. The plate layer wins exactly as it does during slicing.
inline bool resolve_plate_process_settings(const PresetBundle &bundle,
                                           const PlateSlicingContext &context,
                                           const DynamicPrintConfig &plate_overrides,
                                           DynamicPrintConfig &inherited,
                                           DynamicPrintConfig &effective,
                                           std::string &error)
{
    inherited.clear();
    effective.clear();
    ResolvedPlatePresets presets;
    if (!bundle.resolve_plate_presets(context, presets, error))
        return false;
    inherited = presets.print->config;
    bundle.apply_project_overrides(inherited);
    effective = inherited;
    effective.apply(plate_overrides, true);
    return true;
}

// Both direct widget edits and dependent-option corrections must reach the saved
// plate. A false value is an override too; only an explicit revert removes it.
inline void write_plate_process_options(ModelConfig &editor_overrides,
                                         DynamicPrintConfig &plate_overrides,
                                         const DynamicPrintConfig &edited,
                                         const std::vector<std::string> &keys)
{
    plate_overrides.apply_only(edited, keys);
    editor_overrides.assign_config(plate_overrides);
}

} // namespace Slic3r::GUI
