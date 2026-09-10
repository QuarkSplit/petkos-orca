#include "PlateBoard.hpp"
#include "PetkosPerf.hpp"
#include "Widgets/PodBanner.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdlib>
#include <map>
#include <set>

#include <wx/colordlg.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/filefn.h>
#include <wx/image.h>
#include <wx/popupwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>

#include "libslic3r/Utils.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "DeviceCore/DevManager.h"
#include "DeviceManager.hpp"
#include "NotificationManager.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

namespace Slic3r { namespace GUI {

// ----------------------------------------------------------------------------
// PlateBoardModel
// ----------------------------------------------------------------------------

bool PlateBoardModel::printer_bed_size(const PresetBundle &bundle, const std::string &preset_name, double &w, double &d)
{
    if (preset_name.empty())
        return false;

    const Preset *preset = bundle.printers.find_preset(preset_name, false);
    if (preset == nullptr)
        return false;

    const ConfigOptionPoints *area = preset->config.option<ConfigOptionPoints>("printable_area");
    if (area == nullptr || area->values.size() < 3)
        return false;

    double min_x = area->values.front().x(), max_x = min_x;
    double min_y = area->values.front().y(), max_y = min_y;
    for (const Vec2d &pt : area->values) {
        min_x = std::min(min_x, pt.x());
        max_x = std::max(max_x, pt.x());
        min_y = std::min(min_y, pt.y());
        max_y = std::max(max_y, pt.y());
    }
    w = max_x - min_x;
    d = max_y - min_y;
    return w > 0. && d > 0.;
}

bool PlateBoardModel::printer_nozzle_diameter(const PresetBundle &bundle, const std::string &preset_name, double &diameter)
{
    if (preset_name.empty())
        return false;

    const Preset *preset = bundle.printers.find_preset(preset_name, false);
    if (preset == nullptr)
        return false;

    //the option pointer, never the accessor: DynamicConfig::opt_float(key, idx) ends in
    //"return 0;" from a function returning const double&, so an absent key hands back a
    //reference to a temporary. Reading a key that might not be there is a crash here.
    const ConfigOptionFloats *nozzle = preset->config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzle == nullptr || nozzle->values.empty())
        return false;

    diameter = nozzle->values.front();
    return diameter > 0.;
}

PlateBoardGrouping PlateBoardModel::default_grouping(int plate_count)
{
    return plate_count > PLATE_BOARD_COMPACT_ABOVE ? PlateBoardGrouping::ByMachine : PlateBoardGrouping::PlateOrder;
}

void PlateBoardModel::build_row(int                             plate_index,
                                const PartPlateList &           plates,
                                const PresetBundle &            bundle,
                                PlateBoardRow &                 row) const
{
    const PartPlate *plate = plates.get_plate(plate_index);
    if (plate == nullptr)
        return;

    row = PlateBoardRow();
    row.plate_index  = plate_index;
    row.plate_name   = plate->get_plate_name();
    row.printer_name = plate->get_printer_preset_name();
    row.process_name      = plate->get_print_preset_name();
    row.device_id         = plate->get_physical_printer_id();
    row.process_overrides = plate->process_override_count();
    //a stored name this installation does not have is preserved verbatim and
    //reported, never cleared, remapped or rendered as something else
    row.preset_missing = !printer_bed_size(bundle, row.printer_name, row.bed_w, row.bed_d);

    //Nozzle belongs to the preset the row is showing. A plate stores no nozzle, so this
    //is read-only wherever it is displayed and is labelled with the preset it came from.
    printer_nozzle_diameter(bundle, row.printer_name, row.nozzle_diameter);

    //model identity for the row's printer picture and its short caption
    if (const Preset *preset = bundle.printers.find_preset(row.printer_name, false); preset != nullptr)
        row.printer_model = preset->config.opt_string("printer_model");

    //The plate's OWN bed type and map mode, not the project's resolved values.
    //btDefault here is the plate saying it follows the global plate type, which is a
    //fact worth showing rather than one to resolve away.
    row.bed_type          = (int) plate->get_bed_type();
    row.filament_map_mode = (int) plate->get_filament_map_mode();

    //the plate's own geometry wins over the preset's when it has been applied,
    //because that is the bed the parts are actually sitting on
    const Vec2d size = plate->get_size();
    if (size.x() > 0. && size.y() > 0.) {
        row.bed_w = size.x();
        row.bed_d = size.y();
    }

    row.part_count    = plate->instance_count();
    row.parts_outside = plate->has_instances_outside();
    row.sliced        = plate->is_slice_result_valid();
    //A retained result whose context has moved out from under it. The G-code is still
    //attached and can be inspected; it simply cannot be dispatched under this context.
    //"Never sliced" and "sliced, then changed" want different repairs, so they are
    //different states rather than one absence.
    row.stale         = !row.sliced && plate->has_retained_slice_result();

    //No valid slice means no time: an em-dash in the row, zero in every total, and one
    //more in the trailing "N not estimated" note. finalise() derives that count from the
    //rows; without the note the totals would lie by omission.
    row.has_time = row.sliced &&
                   plate->get_retained_print_statistics(row.print_time_seconds, row.weight_grams);

    //A slice this plate arrived with and no longer has. See PlateBoardRow::dropped_reason:
    //it is a third state and not a variety of "stale", because nothing is attached.
    row.dropped_reason = plate->sliced_config_dropped_reason();

    //WHETHER THIS PLATE COMPOSES AT ALL - "can I dispatch this" is the question the board
    //exists to answer, and it was the one thing a row never said. It is composed HERE and
    //never in a paint: this is the expensive read on the path, and the shared ComposeScope
    //the full rebuild holds collapses it to one composition per distinct context instead of
    //one per plate. A targeted refresh composes once, for the one plate that changed.
    //
    //Skipped when the plate has a current slice, because a slice is only current if this
    //same composition has just succeeded - is_slice_result_valid() recomposes to decide it.
    //That is not an optimisation layered over the answer, it is the answer.
    if (!row.sliced) {
        DynamicPrintConfig composed;
        std::string        error;
        if (!plate->compose_slicing_config(composed, error)) {
            row.unresolved        = true;
            row.unresolved_reason = error;
        }
    }

    // Blank slot colours come from that slot's material, never the unrelated pool row.
    const auto own_colours = bundle.plate_filament_colours(plate->get_slicing_context());
    auto slot_colour = [&](int slot) -> std::string {
        if (slot >= 1 && (size_t) slot <= own_colours.size() && !own_colours[(size_t) slot - 1].empty())
            return own_colours[(size_t) slot - 1];
        return std::string();
    };

    for (int slot : plate->get_extruders(true)) {
        row.filament_slots.push_back(slot);
        row.filament_colours.push_back(slot_colour(slot));
    }

    //The slot the material grouping files this plate under. Most grams from the
    //plate's own slice when it has one; otherwise the lowest slot it uses. This is a
    //display ordering key and nothing else - it selects no preset and reaches no
    //slicing decision, so choosing the lowest slot when there is no slice to weigh is
    //a tie-break, not a substituted context.
    double best_grams = -1.;
    //auto, not the type name: PartPlate.hpp is where this vector's element type is
    //resolved, and naming it again here would be a second place for that answer to be
    //wrong if the header's include set ever moves.
    for (const auto &info : plate->get_slice_filaments_info()) {
        const int slot = info.id + 1;
        if (std::find(row.filament_slots.begin(), row.filament_slots.end(), slot) == row.filament_slots.end())
            continue;
        if ((double) info.used_g > best_grams) {
            best_grams        = (double) info.used_g;
            row.dominant_slot = slot;
        }
    }
    if (row.dominant_slot == 0 && !row.filament_slots.empty())
        row.dominant_slot = *std::min_element(row.filament_slots.begin(), row.filament_slots.end());

    if (row.dominant_slot > 0) {
        //The plate's own material in that slot, not the library's: the two lists are
        //independent now, and grouping a plate by what the library happens to hold in the
        //same slot number filed it under a material it does not print.
        const std::vector<std::string> &plate_presets = plate->get_filament_preset_names();
        if ((size_t) row.dominant_slot <= plate_presets.size()) {
            if (const Preset *preset = bundle.filaments.find_preset(plate_presets[(size_t) row.dominant_slot - 1], false);
                preset != nullptr)
                if (const ConfigOptionStrings *type = preset->config.option<ConfigOptionStrings>("filament_type");
                    type != nullptr && !type->values.empty())
                    row.material_type = type->values.front();
        }
        row.material_colour = slot_colour(row.dominant_slot);
    }
}

void PlateBoardModel::finalise(PlateBoardGrouping grouping, const PresetBundle &bundle)
{
    //Every total here is a pure function of the row set. Deriving them rather than
    //accumulating them inside the read loop is what lets one changed row produce correct
    //totals without re-reading the other thirty-five.
    m_groups.clear();
    m_rollup = PlateBoardRollup();

    //hours per machine, so the rollup can report the longest queue without proposing
    //a single move: max-of-sums is literally what the cell says it is
    std::map<std::string, float> queue_seconds;
    std::set<std::string>        machines;

    for (const PlateBoardRow &row : m_rows) {
        machines.insert(row.printer_name);
        if (row.has_time) {
            m_rollup.total_seconds += row.print_time_seconds;
            queue_seconds[row.printer_name] += row.print_time_seconds;
        } else {
            ++m_rollup.not_estimated;
        }
    }

    m_rollup.plates = (int) m_rows.size();
    //Distinct MACHINES. Every plate names one, so this is a straight count of the set
    //rather than a count of kinds of assignment - which is what once made one machine
    //reached two ways read as two.
    m_rollup.machines = (int) machines.size();
    for (const std::pair<const std::string, float> &queue : queue_seconds)
        m_rollup.longest_queue_seconds = std::max(m_rollup.longest_queue_seconds, queue.second);

    //The glyphs are drawn in proportion to the largest bed in the PROJECT rather than to a
    //fixed millimetre constant. A fixed constant has to be chosen for the largest machine
    //anyone might own, which spends most of the glyph's range on beds nobody in this
    //project has and leaves a 220 mm bed and a 256 mm bed a couple of pixels apart. Scaled
    //to the project, the largest machine fills the cell and every other bed is exactly its
    //true fraction of it, which is the most contrast the glyph can carry without lying.
    //The floor stops a project of one small machine from drawing a 180 mm bed at full size.
    m_glyph_reference_mm = 250.;
    for (const PlateBoardRow &row : m_rows)
        m_glyph_reference_mm = std::max(m_glyph_reference_mm, std::max(row.bed_w, row.bed_d));

    build_groups(grouping, bundle);
}

void PlateBoardModel::rebuild(const PartPlateList &plates, const PresetBundle &bundle, PlateBoardGrouping grouping)
{
    //Perf: aux is the plate count. This is the O(plates) path, and every row it reads
    //composes a whole config. It is the right answer when the row SET changed - plates
    //added, removed, reordered, or a new project. When ONE plate changed, refresh_plate()
    //below does the same reading for one row and derives the rest.
    PETKOS_PERF_SCOPE_AUX(Perf::Probe::BoardRowLayout, (int32_t) plates.get_plate_count());
    m_rows.clear();

    //Every row composes its plate's config to read which extruders it uses. Plates that
    //share a printer, a process and a filament set share that answer, and in a real project
    //most of them do - so the loop composes once per distinct context rather than once per
    //plate. The scope ends with this function, which is what makes it safe.
    const PresetBundle::ComposeScope compose_scope(bundle);

    const int count = plates.get_plate_count();
    m_rows.reserve((size_t) (count > 0 ? count : 0));
    for (int i = 0; i < count; ++i) {
        if (plates.get_plate(i) == nullptr)
            continue;
        PlateBoardRow row;
        build_row(i, plates, bundle, row);
        m_rows.push_back(std::move(row));
    }

    finalise(grouping, bundle);
}

bool PlateBoardModel::refresh_plate(int plate_index, const PartPlateList &plates, const PresetBundle &bundle, PlateBoardGrouping grouping)
{
    PETKOS_PERF_SCOPE_AUX(Perf::Probe::BoardRowRefresh, (int32_t) plate_index);

    //Decline rather than guess. The model is patched in place here, so it may only be
    //patched while it is provably still describing this plate list: an index the list has,
    //and a row that still claims that plate. Anything else is a full rebuild's job, and
    //false is how the caller is told to do one. An optimisation that can decline is not a
    //second source of truth that can drift.
    if (plate_index < 0 || plate_index >= plates.get_plate_count())
        return false;
    if (plates.get_plate(plate_index) == nullptr)
        return false;

    size_t pos = m_rows.size();
    for (size_t r = 0; r < m_rows.size(); ++r) {
        if (m_rows[r].plate_index == plate_index) {
            pos = r;
            break;
        }
    }
    if (pos == m_rows.size())
        return false;

    build_row(plate_index, plates, bundle, m_rows[pos]);

    //A plate that changed machine changes which group it belongs to and every total that
    //counts machines, so the derived half is recomputed in full. That is arithmetic over
    //rows which already exist, and it composes nothing.
    finalise(grouping, bundle);
    return true;
}

void PlateBoardModel::build_groups(PlateBoardGrouping grouping, const PresetBundle &bundle)
{
    //Plate order is one flat list. It draws no group header at all: a single unnamed group
    //holding everything is a header that states nothing and costs a row of height to do it.
    if (grouping == PlateBoardGrouping::PlateOrder || m_rows.size() <= 1)
        return;

    const std::string mode_key = grouping == PlateBoardGrouping::ByMachine  ? "m:" :
                                 grouping == PlateBoardGrouping::ByCapacity ? "c:" : "f:";

    //A machine group, addressed by the preset name it collects. Every plate names a
    //machine, so there is no group for the absence of one.
    auto machine_caption = [](const std::string &machine) {
        return machine.empty() ? into_u8(_L("No printer")) : machine;
    };
    auto machine_detail = [&](const std::string &machine) {
        double w = 0., d = 0.;
        if (!printer_bed_size(bundle, machine, w, d))
            return std::string();
        return into_u8(wxString::Format("%.0f x %.0f mm", w, d));
    };

    if (grouping == PlateBoardGrouping::ByMachine || grouping == PlateBoardGrouping::ByCapacity) {
        std::map<std::string, PlateBoardGroup> by_machine;
        for (int i = 0; i < (int) m_rows.size(); ++i) {
            const PlateBoardRow &row     = m_rows[(size_t) i];
            const std::string &  machine = row.printer_name;

            PlateBoardGroup &group = by_machine[machine];
            if (group.rows.empty()) {
                group.key           = mode_key + machine;
                group.caption       = machine_caption(machine);
                group.detail        = machine_detail(machine);
                group.names_machine = true;
                //These two are what a drag reads. The name is stored rather than sliced back
                //out of key or read off the translated caption, because both would answer the
                //question a second time and one of the two answers would eventually be wrong.
                group.machine       = machine;
                group.drop_target   = true;
            }
            group.rows.push_back(i);
            ++group.plate_count;
            if (row.has_time)
                group.queue_seconds += row.print_time_seconds;
        }

        for (std::pair<const std::string, PlateBoardGroup> &entry : by_machine)
            m_groups.push_back(entry.second);

        if (grouping == PlateBoardGrouping::ByCapacity) {
            //Sorted by descending queue hours, which puts the longest-queue machine first:
            //the only ordering that makes the rollup's headline cell actionable without the
            //panel proposing a single move.
            std::stable_sort(m_groups.begin(), m_groups.end(),
                             [](const PlateBoardGroup &a, const PlateBoardGroup &b) {
                                 return a.queue_seconds > b.queue_seconds;
                             });
        }
        return;
    }

    // ---- by material -------------------------------------------------------
    //Two levels: a material group containing machine sub-groups. The material key merges
    //across printers, because slots are per-printer and splitting one black-PLA project
    //across four machines would destroy the compression that is the only argument for the
    //grouping. The slot number rides on the row beside the swatch, which is the part the
    //G-code agrees with.
    struct MaterialBucket
    {
        std::string                            caption;
        std::string                            colour;
        std::map<std::string, PlateBoardGroup> machines;
        int                                    plate_count   = 0;
        float                                  queue_seconds = 0.f;
    };

    std::map<std::string, MaterialBucket> buckets;
    for (int i = 0; i < (int) m_rows.size(); ++i) {
        const PlateBoardRow &row = m_rows[(size_t) i];

        //A plate whose instances reference no slot is its own final group. It is not filed
        //under a colour it does not have.
        const std::string material_key = row.dominant_slot == 0
                                             ? std::string("\xff")
                                             : row.material_colour + "|" + row.material_type;

        MaterialBucket &bucket = buckets[material_key];
        if (bucket.caption.empty()) {
            if (row.dominant_slot == 0)
                bucket.caption = into_u8(_L("No filament assigned"));
            else if (row.material_type.empty())
                bucket.caption = into_u8(_L("Unnamed material"));
            else
                bucket.caption = row.material_type;
            bucket.colour = row.material_colour;
        }

        const std::string &machine = row.printer_name;
        PlateBoardGroup &  group   = bucket.machines[machine];
        if (group.rows.empty()) {
            group.key           = "f:" + material_key + "/" + machine;
            group.caption       = machine_caption(machine);
            group.detail        = machine_detail(machine);
            group.depth         = 1;
            group.names_machine = true;
            group.machine       = machine;
            //A machine sub-group names a machine, but nothing in the material grouping is a
            //drop target: rows are draggable only in the two machine groupings, so a target
            //here would be a destination that exists in a mode with nothing to drag onto it.
            group.drop_target   = false;
        }
        group.rows.push_back(i);
        ++group.plate_count;
        ++bucket.plate_count;
        if (row.has_time) {
            group.queue_seconds += row.print_time_seconds;
            bucket.queue_seconds += row.print_time_seconds;
        }
    }

    for (std::pair<const std::string, MaterialBucket> &entry : buckets) {
        PlateBoardGroup material;
        material.key           = "f:" + entry.first;
        material.caption       = entry.second.caption;
        material.swatch_colour = entry.second.colour;
        material.depth         = 0;
        material.plate_count   = entry.second.plate_count;
        material.queue_seconds = entry.second.queue_seconds;

        //One material on one machine needs one header, not two stacked headers saying the
        //same run of rows twice. The sub-level exists to separate machines; where there is
        //nothing to separate, the machine name moves up into the material header's detail.
        if (entry.second.machines.size() == 1) {
            PlateBoardGroup &only  = entry.second.machines.begin()->second;
            material.rows          = only.rows;
            material.names_machine = true;
            material.detail        = only.caption + (only.detail.empty() ? std::string() : "   " + only.detail);
            material.machine       = only.machine;
            //Explicitly not a drop target, for the same reason as the sub-group it absorbed:
            //this header only exists in the material grouping, where no row is draggable.
            material.drop_target   = false;
            m_groups.push_back(material);
            continue;
        }

        m_groups.push_back(material);
        for (std::pair<const std::string, PlateBoardGroup> &machine : entry.second.machines)
            m_groups.push_back(machine.second);
    }
}

std::string PlateBoardModel::summary_text() const
{
    //One machine is worth naming; several are worth counting. Naming one of several would
    //be the collapsed section describing part of what it hides.
    if (m_rollup.machines == 1 && !m_rows.empty())
        return m_rows.front().printer_name;
    //_L_PLURAL, the tree's own plural macro, rather than a fixed plural: with the count
    //fixed to count machines instead of kinds of assignment, one machine reached two ways is
    //now genuinely 1, and "1 machines" would be the visible half of the bug just removed.
    return into_u8(wxString::Format(_L_PLURAL("%d machine", "%d machines", m_rollup.machines),
                                    m_rollup.machines));
}

// ----------------------------------------------------------------------------
// shared drawing helpers
// ----------------------------------------------------------------------------

namespace {

wxString format_hours(float seconds)
{
    if (seconds <= 0.f)
        return wxString::FromUTF8("\xe2\x80\x93"); //en dash: this plate has no valid slice
    const int total_minutes = (int) (seconds / 60.f + 0.5f);
    const int hours         = total_minutes / 60;
    const int minutes       = total_minutes % 60;
    if (hours == 0)
        return wxString::Format("%dm", minutes);
    return wxString::Format("%dh %02dm", hours, minutes);
}

//Every colour on this panel is one of the application's own, taken through the same dark
//map the rest of the sidebar goes through, so a theme change moves the board with it. The
//hand-picked constants this replaced drifted from the app on both themes and had no way
//not to: they were chosen against a screenshot.
wxColour theme(bool dark, const char *light_hex)
{
    wxColour light;
    light.Set(wxString::FromUTF8(light_hex));
    return dark ? StateColor::darkModeColorFor(light) : light;
}

wxColour board_bg(bool dark)     { return theme(dark, "#FFFFFF"); } //window background
wxColour board_fg(bool dark)     { return theme(dark, "#262E30"); } //primary text
wxColour board_soft(bool dark)   { return theme(dark, "#323A3D"); } //softer text
wxColour board_dim(bool dark)    { return theme(dark, "#6B6A6A"); } //dimmed text
wxColour board_line(bool dark)   { return theme(dark, "#EEEEEE"); } //separator / title line
wxColour board_head(bool dark)   { return theme(dark, "#F8F8F8"); } //sidebar titlebar band
wxColour board_sel(bool dark)    { return theme(dark, "#C0D4E2"); } //checked item background
wxColour board_scope(bool dark)  { return theme(dark, "#EBF9F0"); } //in the scope set, not current
wxColour board_hover(bool dark)  { return theme(dark, "#E5EEF5"); } //focused item background
wxColour board_accent(bool dark) { return theme(dark, "#4F87A5"); } //ORCA colour
//GROUNDS, kept apart from the inks above because nothing in the type system keeps them
//apart: a wxColour is a wxColour, so a fill can take a text colour and did. board_soft
//(#323A3D) was the brush for both empty tiles, which painted a near-black square as the
//loudest object on a panel whose whole subject is which machine a plate goes to - and
//dark mode lightens it, so it was a light-mode-only fault that never showed up in testing.
wxColour board_tile(bool dark)      { return theme(dark, "#F4F6F6"); } //an empty picture cell
wxColour board_tile_line(bool dark) { return theme(dark, "#DCE2E2"); } //its edge
wxColour board_warn(bool dark)   { return theme(dark, "#FF6F00"); } //secondary / attention
wxColour board_err(bool dark)    { return theme(dark, "#D01B1B"); } //error

//THE BED IN PLAN, drawn the same way wherever a bed is drawn, and the board's only geometric
//signal: a smaller machine draws a visibly smaller rectangle. Sizes are in millimetres and
//scaled against the project's largest bed, so the biggest machine fills its cell.
//
//This is the body of what PlateBoard::draw_bed_plan was, lifted out so the printer picker can
//draw the same thing. The picker was filling a solid slab from its own scaling helper instead,
//and a filled dark block of nearly the same size as the one above it is an indistinct square -
//which is exactly what this glyph exists NOT to be. Two surfaces answering "how big is this
//bed" with two different pictures is the same fault as two preset combos that do different
//things, and it was worst at the moment of choosing between machines.
void draw_bed_plan_in(wxDC &dc, const wxWindow *win, const wxRect &cell, double bed_w, double bed_d,
                      double reference_mm, const wxColour &colour, int inset_dip)
{
    if (bed_w <= 0. || bed_d <= 0.)
        return;
    const double reference = std::max(1., reference_mm);
    const int    inset     = win->FromDIP(inset_dip);
    const int    room      = std::max(1, std::min(cell.GetWidth(), cell.GetHeight()) - 2 * inset);
    const int    w         = std::max(2, (int) std::lround(room * std::min(1., bed_w / reference)));
    const int    d         = std::max(2, (int) std::lround(room * std::min(1., bed_d / reference)));

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(colour, 1));
    dc.DrawRectangle(cell.x + (cell.GetWidth() - w) / 2, cell.y + (cell.GetHeight() - d) / 2, w, d);
}

//A collapse chevron drawn as geometry rather than as a text glyph. The character forms
//for this vary by installed font and render as a missing-glyph box on a machine that does
//not have one, which is a defect that only ever appears on somebody else's computer.
void draw_chevron(wxDC &dc, const wxColour &colour, int x, int y, int size, bool expanded)
{
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(colour));
    wxPoint pts[3];
    if (expanded) {
        pts[0] = wxPoint(x, y + size / 4);
        pts[1] = wxPoint(x + size, y + size / 4);
        pts[2] = wxPoint(x + size / 2, y + size * 3 / 4);
    } else {
        pts[0] = wxPoint(x + size / 4, y);
        pts[1] = wxPoint(x + size * 3 / 4, y + size / 2);
        pts[2] = wxPoint(x + size / 4, y + size);
    }
    dc.DrawPolygon(3, pts);
}

//An icon that is missing from the resources must not take the application down with it.
//create_scaled_bitmap throws, and it is reached here to decorate a row rather than to
//answer a question anything depends on, so a failure degrades to no icon.
bool load_board_icon(wxWindow *win, const char *name, int px, ScalableBitmap &out)
{
    try {
        out = ScalableBitmap(win, name, px);
        return out.bmp().IsOk();
    } catch (...) {
        return false;
    }
}

//ThumbnailData is RGBA at PartPlate::plate_thumbnail_width/height and is stored BOTTOM-UP,
//because it comes straight out of glReadPixels; the in-canvas plate strip is why nothing
//noticed, since it draws the buffer with flipped texture coordinates rather than flipping
//the bytes. Two other places in this tree already invert it into a wxImage by hand
//(SelectMachineDialog and SyncAmsInfoDialog); this is a third, kept local because both of
//those live in files this change does not own. Written through the raw RGB and alpha
//buffers rather than SetRGB/SetAlpha per pixel: at 512x512 the per-pixel form is 262144
//calls each, on a path that runs while the user is waiting for a hover to appear.
wxImage thumbnail_to_image(const ThumbnailData &data)
{
    if (!data.is_valid())
        return wxImage();

    wxImage image((int) data.width, (int) data.height);
    if (!image.IsOk())
        return wxImage();
    image.InitAlpha();

    unsigned char *rgb   = image.GetData();
    unsigned char *alpha = image.GetAlpha();
    if (rgb == nullptr || alpha == nullptr)
        return wxImage();

    for (unsigned int r = 0; r < data.height; ++r) {
        const unsigned char *src   = data.pixels.data() + 4 * (size_t)(data.height - 1 - r) * data.width;
        unsigned char *      d_rgb = rgb + 3 * (size_t) r * data.width;
        unsigned char *      d_a   = alpha + (size_t) r * data.width;
        for (unsigned int c = 0; c < data.width; ++c, src += 4) {
            *d_rgb++ = src[0];
            *d_rgb++ = src[1];
            *d_rgb++ = src[2];
            *d_a++   = src[3];
        }
    }
    return image;
}

} // namespace

// ----------------------------------------------------------------------------
// PlateThumbnailPreview
// ----------------------------------------------------------------------------

//The row's hover preview, and the reason the in-canvas plate strip's thumbnails are no
//longer the only place a plate can be told apart by its contents.
//
//A plain wxPopupWindow and deliberately NOT the transient PopupWindow the picker uses: a
//transient popup grabs the mouse and dismisses on the next click, so hovering a row would
//swallow the click that followed it. This one takes no focus, takes no capture, and is
//moved and hidden by the board alone.
class PlateThumbnailPreview : public wxPopupWindow
{
public:
    PlateThumbnailPreview(wxWindow *parent) : wxPopupWindow(parent, wxBORDER_NONE)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &PlateThumbnailPreview::on_paint, this);
        //hidden when the app itself loses the foreground; a hover preview floating over
        //another application is the board leaking out of its window
        wxTheApp->Bind(wxEVT_ACTIVATE_APP, &PlateThumbnailPreview::on_app_activate, this);
    }

    ~PlateThumbnailPreview() override
    {
        if (wxTheApp != nullptr)
            wxTheApp->Unbind(wxEVT_ACTIVATE_APP, &PlateThumbnailPreview::on_app_activate, this);
    }

    void on_app_activate(wxActivateEvent &evt)
    {
        evt.Skip();
        if (!evt.GetActive() && IsShown())
            Hide();
    }

    //The bitmap may be invalid. The preview then carries `note` instead of quietly not
    //appearing: a hover that produces nothing and says nothing is indistinguishable from
    //one that is broken, which is the silent case this fork treats as a bug.
    void show_beside(const wxRect &row_screen_rect, const wxBitmap &bitmap, const wxString &caption, const wxString &note)
    {
        m_bitmap  = bitmap;
        m_caption = caption;
        m_note    = note;

        const int pad     = FromDIP(6);
        const int line    = FromDIP(16);
        const int image_w = m_bitmap.IsOk() ? m_bitmap.GetWidth() : FromDIP(150);
        const int image_h = m_bitmap.IsOk() ? m_bitmap.GetHeight() : 0;

        const int width  = image_w + 2 * pad;
        const int height = image_h + 2 * pad + line + (m_note.IsEmpty() ? 0 : line);
        SetSize(wxSize(width, height));

        //To the LEFT of the row, because the board lives in a sidebar pinned to the right
        //edge: over the row it would hide the thing being previewed, and to the right it
        //would be off the screen. It flips back to the right only when there is genuinely
        //no room, which is a second-monitor arrangement rather than the normal case.
        //
        //The display is asked of the BOARD rather than of this window: the popup has not
        //been positioned yet, so its own answer would name whichever display holds the
        //origin instead of the one the sidebar is on. GetFromWindow rather than the
        //wxDisplay(window) constructor, which is the form the rest of this tree uses
        //(GUI_App::window_pos_sanitize) and the one that says what it does when the window
        //is on no display yet.
        const int    display_idx = wxDisplay::GetFromWindow(GetParent() != nullptr ? GetParent() : this);
        const wxRect screen      = wxDisplay(display_idx == wxNOT_FOUND ? 0u : (unsigned) display_idx).GetClientArea();
        int          x      = row_screen_rect.GetLeft() - width - FromDIP(8);
        if (x < screen.GetLeft())
            x = std::min(row_screen_rect.GetRight() + FromDIP(8), screen.GetRight() - width);
        int y = row_screen_rect.GetTop() + row_screen_rect.GetHeight() / 2 - height / 2;
        y     = std::max(screen.GetTop(), std::min(y, screen.GetBottom() - height));

        SetPosition(wxPoint(x, y));
        Refresh();
        if (!IsShown())
            Show();
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const bool            dark = wxGetApp().dark_mode();
        const wxSize          size = GetClientSize();
        const int             pad  = FromDIP(6);

        dc.SetBrush(wxBrush(board_bg(dark)));
        dc.SetPen(wxPen(board_dim(dark)));
        dc.DrawRectangle(0, 0, size.GetWidth(), size.GetHeight());

        int y = pad;
        if (m_bitmap.IsOk()) {
            dc.DrawBitmap(m_bitmap, pad, y, true);
            y += m_bitmap.GetHeight() + FromDIP(2);
        }

        dc.SetFont(Label::Body_10);
        dc.SetTextForeground(board_fg(dark));
        dc.DrawText(wxControl::Ellipsize(m_caption, dc, wxELLIPSIZE_END, size.GetWidth() - 2 * pad), pad, y);

        if (!m_note.IsEmpty()) {
            dc.SetFont(Label::Body_9);
            dc.SetTextForeground(board_warn(dark));
            dc.DrawText(wxControl::Ellipsize(m_note, dc, wxELLIPSIZE_END, size.GetWidth() - 2 * pad),
                        pad, y + FromDIP(16));
        }
    }

    wxBitmap m_bitmap;
    wxString m_caption;
    wxString m_note;
};

// ----------------------------------------------------------------------------
// PlatePrinterPopup
// ----------------------------------------------------------------------------

PlatePrinterPopup::PlatePrinterPopup(wxWindow *             parent,
                                     Plater *               plater,
                                     int                    plate_index,
                                     const std::string &    current_name,
                                     const std::vector<int> &scoped_plates)
    : PopupWindow(parent, wxBORDER_SIMPLE), m_plater(plater), m_plate_index(plate_index),
      m_scoped_plates(scoped_plates)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_row_height = FromDIP(26);

    m_current_name = current_name;
    //the nozzle the plate is on now, which is the value a machine pick carries over
    if (const Preset *cur = wxGetApp().preset_bundle->printers.find_preset(current_name, false); cur != nullptr)
        m_current_variant = cur->config.opt_string("printer_variant");

    build_items(current_name);
    SetSize(wxSize(std::max(parent->GetSize().GetWidth(), FromDIP(280)), FromDIP(100)));
    fit_height();

    Bind(wxEVT_PAINT, &PlatePrinterPopup::on_paint, this);
    Bind(wxEVT_MOTION, &PlatePrinterPopup::on_mouse, this);
    Bind(wxEVT_LEFT_UP, &PlatePrinterPopup::on_mouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &PlatePrinterPopup::on_mouse, this);
    Bind(wxEVT_MOUSEWHEEL, &PlatePrinterPopup::on_wheel, this);

    //A transient popup only hears clicks inside its own application. Alt-tabbing or
    //clicking another app leaves it floating over that app, so it listens for the app
    //losing activation and dismisses itself. Unbound in the destructor: the popup can be
    //destroyed by its owner while the app object lives on.
    wxTheApp->Bind(wxEVT_ACTIVATE_APP, &PlatePrinterPopup::on_app_activate, this);
}

PlatePrinterPopup::~PlatePrinterPopup()
{
    if (wxTheApp != nullptr)
        wxTheApp->Unbind(wxEVT_ACTIVATE_APP, &PlatePrinterPopup::on_app_activate, this);
}

void PlatePrinterPopup::on_app_activate(wxActivateEvent &evt)
{
    evt.Skip();
    if (!evt.GetActive() && IsShown())
        Dismiss();
}

void PlatePrinterPopup::build_items(const std::string &current_name)
{
    const PresetBundle &bundle = *wxGetApp().preset_bundle;

    //how many plates already sit on each machine, so the picker can say so, and which
    //plates share this plate's machine, which is the only set the bulk footer may touch:
    //"the others that are where this one is" is a set the user can see on the board.
    std::map<std::string, int> &plate_counts = m_plate_counts;
    const PartPlateList &      plates  = m_plater->get_partplate_list();
    const PartPlate *          subject = plates.get_plate(m_plate_index);
    const std::string          current_machine = subject != nullptr ? subject->get_printer_preset_name() : std::string();
    m_current_machine = current_machine;
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        const PartPlate *plate = plates.get_plate(i);
        if (plate == nullptr)
            continue;
        ++plate_counts[plate->get_printer_preset_name()];
        if (i != m_plate_index && !current_machine.empty() &&
            plate->get_printer_preset_name() == current_machine)
            m_sibling_plates.push_back(i);
    }

    //the bed this plate is on right now, so a candidate that is smaller in either axis can
    //say so before the click rather than after it
    double current_w = 0., current_d = 0.;
    if (const PartPlate *plate = plates.get_plate(m_plate_index)) {
        const Vec2d size = plate->get_size();
        current_w = size.x();
        current_d = size.y();
    }

    //1. a printer this installation cannot resolve is offered back verbatim, so
    //   opening the picker cannot be the thing that discards it
    if (!current_name.empty() && bundle.printers.find_preset(current_name, false) == nullptr) {
        Item keep;
        keep.name         = current_name;
        keep.label        = wxString::Format(_L("Keep %s"), from_u8(current_name));
        keep.detail       = _L("not installed");
        keep.keep_missing = true;
        m_items.push_back(keep);
    }

    //2. the installed printers, grouped by brand
    std::map<std::string, std::vector<const Preset *>> by_vendor;
    for (const Preset &preset : bundle.printers()) {
        if (!preset.is_visible || preset.is_default)
            continue;
        std::string vendor = preset.vendor != nullptr ? preset.vendor->name : std::string();
        if (vendor.empty())
            vendor = into_u8(_L("User presets"));
        by_vendor[vendor].push_back(&preset);
    }

    for (const std::pair<const std::string, std::vector<const Preset *>> &group : by_vendor) {
        Item header;
        header.is_header = true;
        header.label     = from_u8(group.first);
        m_items.push_back(header);

        //ONE ROW PER MACHINE, not one per nozzle. Ten spellings of the same printer
        //differing by variant is the stock-Orca idiom this fork exists to retire: the
        //machine is the identity, the nozzle is a dependent resolved after the pick.
        std::map<std::string, Item>     models;      //keyed by model name (or preset name when no model)
        std::vector<std::string>        model_order; //map iteration loses discovery order
        for (const Preset *preset : group.second) {
            std::string key = preset->config.opt_string("printer_model");
            if (key.empty())
                key = preset->name;
            auto it = models.find(key);
            if (it == models.end()) {
                Item item;
                item.label = from_u8(key);
                if (PlateBoardModel::printer_bed_size(bundle, preset->name, item.bed_w, item.bed_d)) {
                    item.detail = wxString::Format("%.0f x %.0f mm", item.bed_w, item.bed_d);
                    m_glyph_reference_mm = std::max(m_glyph_reference_mm, std::max(item.bed_w, item.bed_d));
                    item.smaller_bed = current_w > 0. && current_d > 0. &&
                                       (item.bed_w < current_w - 0.5 || item.bed_d < current_d - 0.5);
                }
                it = models.emplace(key, std::move(item)).first;
                model_order.push_back(key);
            }
            it->second.variant_presets.push_back(preset->name);
            std::string variant = preset->config.opt_string("printer_variant");
            it->second.variant_labels.push_back(variant.empty() ? from_u8(preset->name)
                                                                : wxString::Format(_L("%s nozzle"), from_u8(variant)));
        }

        for (const std::string &key : model_order) {
            Item &item = models[key];
            //Counted per MACHINE, so every nozzle variant of one printer contributes to the one
            //row that represents it - which is the same grouping the list itself uses.
            for (const std::string &preset_name : item.variant_presets) {
                const auto used = plate_counts.find(preset_name);
                if (used != plate_counts.end())
                    item.plates_here += used->second;
                if (preset_name == current_machine)
                    item.is_current_machine = true;
            }
            m_items.push_back(std::move(item));
        }
    }

    //The one bulk action. It is a modifier on the next pick rather than an action of
    //its own, because a bulk action has to be told WHICH machine, and it names its
    //count so the blast radius is on screen before the click. There is deliberately no
    //action that retargets already-assigned plates: that reaches past what the user
    //can see.
    //The scope footer. It exists only when the user has more than one plate selected on
    //the board, and it names exactly that set, so it can never reach a plate that is not
    //visibly selected. Like the footer above it, it is a modifier on the next pick.
    if (m_scoped_plates.size() > 1) {
        Item scope;
        scope.is_scope_toggle = true;
        scope.label           = wxString::Format(_L("Assign to the %d selected plates"),
                                                 (int) m_scoped_plates.size());
        m_items.push_back(scope);
    }

    if (!m_sibling_plates.empty()) {
        Item bulk;
        bulk.is_bulk_toggle = true;
        bulk.label          = wxString::Format(_L("Also move the %d other plate(s) on this machine"),
                                               (int) m_sibling_plates.size());
        m_items.push_back(bulk);
    }
}

void PlatePrinterPopup::Popup(wxWindow *focus)
{
    PopupWindow::Popup(focus);
}

//The second step, shown only when the nozzle question is real: the machine is chosen,
//its variants are the options, and the way back is the first row.
void PlatePrinterPopup::build_variant_items(const Item &model_item)
{
    const Item model = model_item; //copied: m_items is about to be replaced under it
    m_items.clear();
    m_scroll = 0;

    Item back;
    back.is_back = true;
    back.label   = wxString::FromUTF8("\xE2\x86\x90 ") + model.label;
    m_items.push_back(back);

    Item header;
    header.is_header = true;
    header.label     = _L("Which nozzle?");
    m_items.push_back(header);

    for (size_t i = 0; i < model.variant_presets.size(); ++i) {
        Item item;
        item.name        = model.variant_presets[i];
        item.label       = model.variant_labels[i];
        item.bed_w       = model.bed_w;
        item.bed_d       = model.bed_d;
        item.smaller_bed = model.smaller_bed;
        //Per VARIANT here, not per machine: at this step the question has narrowed to which
        //nozzle, and "three plates are already on the 0.4" is the answer to it. Read from the
        //counts taken when the popup opened, so navigating between the steps costs nothing.
        if (const auto used = m_plate_counts.find(item.name); used != m_plate_counts.end())
            item.plates_here = used->second;
        item.is_current_machine = !m_current_machine.empty() && item.name == m_current_machine;
        m_items.push_back(item);
    }

    fit_height();
    Refresh();
}

void PlatePrinterPopup::commit(const std::string &preset_name)
{
    Plater *  plater = m_plater;
    const int plate  = m_plate_index;

    std::vector<int> targets;
    if (m_bulk_to_scope)
        targets = m_scoped_plates;
    else if (m_bulk_to_siblings)
        targets = m_sibling_plates;
    if (!targets.empty() && std::find(targets.begin(), targets.end(), plate) == targets.end())
        targets.push_back(plate);

    Dismiss();
    //the assignment rebuilds the board this popup is parented to, so it runs after
    //the dismissal rather than underneath it. One write path either way; the batch
    //takes ONE snapshot, or undoing a five-plate action would take five presses.
    CallAfter([plater, plate, preset_name, targets]() {
        if (targets.empty()) {
            plater->set_plate_printer(plate, preset_name);
            //assigning a plate selects it: the canvas then shows the new machine's bed
            //under the parts immediately. Without this, retargeting a non-current plate
            //succeeded invisibly — the current plate is the only one that renders its
            //machine's bed texture — and a success nobody can see reads as a failure.
            plater->select_plate(plate);
        } else
            plater->set_plate_printers(targets, preset_name);
    });
}

void PlatePrinterPopup::fit_height()
{
    const int rows   = (int) m_items.size();
    const int height = std::min(FromDIP(420), rows * m_row_height + FromDIP(4));
    SetSize(wxSize(GetSize().GetWidth(), height));
}

void PlatePrinterPopup::on_wheel(wxMouseEvent &evt)
{
    const int content = (int) m_items.size() * m_row_height + FromDIP(4);
    const int max_scroll = std::max(0, content - GetClientSize().GetHeight());
    if (max_scroll == 0)
        return;
    const int step = m_row_height * 3;
    m_scroll = std::max(0, std::min(max_scroll, m_scroll + (evt.GetWheelRotation() > 0 ? -step : step)));
    m_hover  = hit_test(evt.GetPosition());
    Refresh();
}

int PlatePrinterPopup::hit_test(const wxPoint &pos) const
{
    const int index = (pos.y + m_scroll - FromDIP(2)) / m_row_height;
    if (index < 0 || index >= (int) m_items.size() || m_items[index].is_header)
        return -1;
    return index;
}

void PlatePrinterPopup::on_mouse(wxMouseEvent &evt)
{
    if (evt.GetEventType() == wxEVT_LEAVE_WINDOW) {
        m_hover = -1;
        Refresh();
        return;
    }

    const int index = hit_test(evt.GetPosition());
    if (evt.GetEventType() == wxEVT_MOTION) {
        if (index != m_hover) {
            m_hover = index;
            Refresh();
        }
        return;
    }

    if (evt.GetEventType() == wxEVT_LEFT_UP && index >= 0) {
        //the footer is a modifier, not a destination: ticking it stays in the popup so
        //the machine can still be picked
        if (m_items[index].is_bulk_toggle) {
            m_bulk_to_siblings = !m_bulk_to_siblings;
            //the two footers name different visible sets, so exactly one can be armed
            if (m_bulk_to_siblings)
                m_bulk_to_scope = false;
            Refresh();
            return;
        }

        if (m_items[index].is_scope_toggle) {
            m_bulk_to_scope = !m_bulk_to_scope;
            if (m_bulk_to_scope)
                m_bulk_to_siblings = false;
            Refresh();
            return;
        }

        if (m_items[index].is_back) {
            build_items(m_current_name);
            m_scroll = 0;
            fit_height();
            Refresh();
            return;
        }

        //a MACHINE row: the nozzle NEVER asks. It resolves silently — the plate's
        //current variant when the model carries it, 0.4 otherwise because that is what
        //is physically in nearly every machine nearly all the time, else whatever the
        //model has. Someone who actually changed a nozzle goes looking for that setting,
        //and finds it on the plate inspector's Nozzle row.
        if (!m_items[index].variant_presets.empty()) {
            const Item &model = m_items[index];
            auto variant_of = [](const std::string &preset_name) {
                const Preset *p = wxGetApp().preset_bundle->printers.find_preset(preset_name, false);
                return p != nullptr ? p->config.opt_string("printer_variant") : std::string();
            };
            std::string pick;
            for (const std::string &preset_name : model.variant_presets)
                if (!m_current_variant.empty() && variant_of(preset_name) == m_current_variant) { pick = preset_name; break; }
            if (pick.empty())
                for (const std::string &preset_name : model.variant_presets)
                    if (variant_of(preset_name) == "0.4") { pick = preset_name; break; }
            if (pick.empty())
                pick = model.variant_presets.front();
            commit(pick);
            return;
        }

        commit(m_items[index].name);
    }
}

void PlatePrinterPopup::on_paint(wxPaintEvent &evt)
{
    wxAutoBufferedPaintDC dc(this);
    const bool            dark = wxGetApp().dark_mode();

    dc.SetBrush(wxBrush(board_bg(dark)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(GetClientRect());

    const int width     = GetClientSize().GetWidth();
    const int glyph_x   = FromDIP(8);
    const int glyph_col = FromDIP(20);
    //A fixed column for the "already used" badge, reserved whether or not a row has one, so the
    //names stay on one left edge and the badges read as a column rather than as ragged noise.
    const int badge_x   = glyph_x + glyph_col + FromDIP(6);
    const int badge_col = FromDIP(18);
    const int text_x    = badge_x + badge_col + FromDIP(6);
    int       y         = FromDIP(2) - m_scroll;

    for (size_t i = 0; i < m_items.size(); ++i, y += m_row_height) {
        const Item &item = m_items[i];

        if ((int) i == m_hover && !item.is_header) {
            dc.SetBrush(wxBrush(board_hover(dark)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, y, width, m_row_height);
        }

        if (item.is_header) {
            dc.SetTextForeground(board_dim(dark));
            dc.SetFont(Label::Body_9);
            dc.DrawText(item.label.Upper(), FromDIP(8), y + FromDIP(8));
            dc.SetPen(wxPen(board_line(dark)));
            const int line_x = FromDIP(8) + dc.GetTextExtent(item.label.Upper()).GetWidth() + FromDIP(6);
            dc.DrawLine(line_x, y + m_row_height / 2, width - FromDIP(8), y + m_row_height / 2);
            continue;
        }

        if (item.is_bulk_toggle || item.is_scope_toggle) {
            const bool armed = item.is_scope_toggle ? m_bulk_to_scope : m_bulk_to_siblings;
            dc.SetPen(wxPen(board_line(dark)));
            dc.DrawLine(FromDIP(6), y, width - FromDIP(6), y);

            //a real tick box rather than a box-drawing character, for the same reason the
            //chevrons are geometry: a missing glyph is somebody else's font problem
            const int box = FromDIP(11);
            const int box_y = y + (m_row_height - box) / 2;
            dc.SetBrush(armed ? wxBrush(board_accent(dark)) : *wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(armed ? board_accent(dark) : board_dim(dark)));
            dc.DrawRoundedRectangle(FromDIP(10), box_y, box, box, FromDIP(2));
            if (armed) {
                dc.SetPen(wxPen(board_bg(dark), 2));
                dc.DrawLine(FromDIP(10) + box / 4, box_y + box / 2, FromDIP(10) + box / 2, box_y + box * 3 / 4);
                dc.DrawLine(FromDIP(10) + box / 2, box_y + box * 3 / 4, FromDIP(10) + box * 4 / 5, box_y + box / 4);
            }

            dc.SetFont(Label::Body_11);
            dc.SetTextForeground(armed ? board_fg(dark) : board_dim(dark));
            const int label_x = FromDIP(10) + box + FromDIP(8);
            dc.DrawText(wxControl::Ellipsize(item.label, dc, wxELLIPSIZE_END, width - label_x - FromDIP(8)),
                        label_x, y + (m_row_height - dc.GetCharHeight()) / 2);
            continue;
        }

        //THE BED IN PLAN, the same drawing the board makes. This was a FILLED rectangle in the
        //board's line colour: at 16 px a solid slab reads as one indistinct square whatever its
        //dimensions are, so the one signal the glyph exists to give - this machine is bigger or
        //smaller than that one - was not being given at the moment of choosing. An outline in
        //proportion is what the board draws for the same fact, and now it is literally the same
        //call. Scaled against the largest bed in the list, so the biggest machine fills the cell.
        draw_bed_plan_in(dc, this, wxRect(glyph_x, y, glyph_col, m_row_height),
                         item.bed_w, item.bed_d, m_glyph_reference_mm,
                         item.is_current_machine ? board_accent(dark) : board_dim(dark), 2);

        //WHERE THE REST OF THE PROJECT ALREADY IS. A filled pill in the accent means "this
        //plate's machine"; an outlined one means "other plates of this project are here". Both
        //carry the count, so same-or-different and how-many are one look.
        if (item.plates_here > 0 && !item.is_back) {
            const int    pill_h = FromDIP(14);
            const wxRect pill(badge_x, y + (m_row_height - pill_h) / 2, badge_col, pill_h);
            dc.SetBrush(item.is_current_machine ? wxBrush(board_accent(dark)) : *wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(item.is_current_machine ? board_accent(dark) : board_dim(dark)));
            dc.DrawRoundedRectangle(pill, pill_h / 2);

            dc.SetFont(Label::Body_9);
            dc.SetTextForeground(item.is_current_machine ? board_bg(dark) : board_dim(dark));
            const wxString count  = wxString::Format("%d", item.plates_here);
            const wxSize   extent = dc.GetTextExtent(count);
            dc.DrawText(count, pill.x + (pill.GetWidth() - extent.GetWidth()) / 2,
                        pill.y + (pill.GetHeight() - extent.GetHeight()) / 2);
        }

        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(item.keep_missing ? board_err(dark) : board_fg(dark));

        wxString detail = item.detail;
        if (item.smaller_bed)
            detail = _L("smaller bed") + (detail.IsEmpty() ? wxString() : wxString("   ") + detail);

        dc.SetFont(Label::Body_9);
        const int detail_w = detail.IsEmpty() ? 0 : dc.GetTextExtent(detail).GetWidth() + FromDIP(10);

        dc.SetFont(Label::Body_12);
        dc.DrawText(wxControl::Ellipsize(item.label, dc, wxELLIPSIZE_END,
                                         std::max(FromDIP(40), width - text_x - detail_w - FromDIP(8))),
                    text_x, y + (m_row_height - dc.GetCharHeight()) / 2);

        if (!detail.IsEmpty()) {
            dc.SetFont(Label::Body_9);
            dc.SetTextForeground(item.smaller_bed ? board_warn(dark) : board_dim(dark));
            const wxSize extent = dc.GetTextExtent(detail);
            dc.DrawText(detail, width - extent.GetWidth() - FromDIP(8), y + (m_row_height - extent.GetHeight()) / 2);
        }
    }
}

// ----------------------------------------------------------------------------
// opening the picker, shared by the board row and the inspector chip
// ----------------------------------------------------------------------------

namespace {

//One popup at a time per owner, destroyed rather than left parented to its owner, or
//every pick leaves a hidden window behind for the lifetime of the sidebar. The owner
//passes its own slot, so the board and the inspector cannot free each other's window.
void show_printer_picker(wxWindow *              owner,
                         Plater *                plater,
                         int                     plate_index,
                         const std::vector<int> &scoped_plates,
                         const wxPoint &         screen_anchor,
                         PlatePrinterPopup *&    slot)
{
    if (owner == nullptr || plater == nullptr || !plater->is_initialized())
        return;

    const PartPlate *plate = plater->get_partplate_list().get_plate(plate_index);
    if (plate == nullptr)
        return;

    if (slot != nullptr) {
        slot->Destroy();
        slot = nullptr;
    }

    //the scope footer may only ever offer the set the user can see selected, and only
    //when this plate is a member of it
    std::vector<int> scope;
    if (std::find(scoped_plates.begin(), scoped_plates.end(), plate_index) != scoped_plates.end())
        scope = scoped_plates;

    slot = new PlatePrinterPopup(owner, plater, plate_index, plate->get_printer_preset_name(), scope);
    slot->Position(screen_anchor, wxSize(0, 0));
    slot->Popup();
}

//The grouping control's segments, in the order they are drawn. ByCapacity is not one of
//them: with no estimates it rendered as an exact twin of ByMachine — two tabs, one view —
//so its queue bars live inside the Machine grouping instead, appearing when estimates
//exist. The enum value survives for persisted state, which coerces to ByMachine on read.
const PlateBoardGrouping GROUPING_ORDER[3] = {PlateBoardGrouping::PlateOrder, PlateBoardGrouping::ByMachine,
                                              PlateBoardGrouping::ByMaterial};

wxString grouping_label(PlateBoardGrouping grouping)
{
    switch (grouping) {
    case PlateBoardGrouping::ByMachine: return _L("Machine");
    case PlateBoardGrouping::ByCapacity: return _L("Machine");
    case PlateBoardGrouping::ByMaterial: return _L("Material");
    case PlateBoardGrouping::PlateOrder:
    default: return _L("Plate order");
    }
}

//A fixed id rather than wxWindow::NewControlId(): this is a namespace-scope constant, and
//an allocator called during static initialisation runs before wx is up.
const int PLATE_BOARD_ANIM_TIMER_ID = wxID_HIGHEST + 4211;
//Its own id, because a second wxTimer sharing one owner and one id would deliver both
//clocks to whichever handler was bound last.
const int PLATE_BOARD_HOVER_TIMER_ID = wxID_HIGHEST + 4212;
//And a third, for the edge auto-scroll during a drag. It cannot share the hover clock: that
//one is a one-shot the drag deliberately suppresses.
const int PLATE_BOARD_AUTOSCROLL_TIMER_ID = wxID_HIGHEST + 4213;
//220 ms, which is the whole point of the animation: long enough to be seen as a resize
//rather than a repaint, short enough not to be waited on.
const int PLATE_BOARD_ANIM_MS   = 220;
const int PLATE_BOARD_ANIM_STEP = 16;
//The dwell before a row shows its thumbnail. It exists so that crossing the board on the
//way to something else does not render a thumbnail per row passed over.
const int PLATE_BOARD_HOVER_MS = 380;
//How far the pointer must travel with the button down before a press becomes a drag rather
//than a click. Below this a click on a row still selects, which is the common action.
const int PLATE_BOARD_DRAG_SLOP_DIP = 5;
//The edge auto-scroll during a drag: one tick, and how much of a row each tick moves. Driven
//by a clock rather than by motion events, because holding the pointer still against the edge
//is exactly the gesture that means "keep going" and it produces no motion events at all. A
//motion-driven scroll makes a group whose rows are all out of view unreachable while the
//button is down, which is a drag that cannot be completed rather than one that is refused.
const int PLATE_BOARD_AUTOSCROLL_MS  = 60;
const int PLATE_BOARD_AUTOSCROLL_DIV = 3; //row height per tick
//How many standard rows the board grows to before it scrolls instead. One fewer than the
//compact threshold, because the rollup tiles and the grouping control sit above the rows
//and the controls below the board must keep their place on a laptop screen.
//The board is the TOP of the sidebar, not the sidebar: filament and process settings —
//the actual slicer — live below it and must never be pushed off screen. Four rich rows
//is the budget; past that the board scrolls inside itself.
const int PLATE_BOARD_VISIBLE_ROWS = 4;

double smoothstep(double t)
{
    t = std::max(0., std::min(1., t));
    return t * t * (3. - 2. * t);
}

} // namespace

// ----------------------------------------------------------------------------
// PlateBoard
// ----------------------------------------------------------------------------

PlateBoard::PlateBoard(wxWindow *parent, Plater *plater) : wxPanel(parent, wxID_ANY), m_plater(plater)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    //one row format, tall enough for a plate render and a printer picture with their
    //captions: the row IS the fork's core object and it earns the space. m_row_compact
    //is zero so the legacy compact branch can never fire.
    m_row_height     = FromDIP(64);
    m_row_compact    = 0;
    m_header_height  = FromDIP(26);
    m_rollup_height  = FromDIP(38);
    m_segment_height = FromDIP(28); //22 of control plus the gap under it

    //The application's own icon set rather than a bare character. A tick typed as text is
    //at the mercy of the installed font; these are the same SVGs the rest of the app uses
    //for the same three facts, and they carry the theme with them. Each is checked on its
    //own at draw time, so one missing resource costs one icon rather than all three.
    const bool sliced_ok  = load_board_icon(this, "checked", 14, m_icon_sliced);
    const bool stale_ok   = load_board_icon(this, "warning", 14, m_icon_stale);
    const bool problem_ok = load_board_icon(this, "error", 14, m_icon_problem);
    //A dropped slice is not stale and not a problem: the plate is fine, the result is simply
    //gone and has to be made again. "Do this again" is what the refresh mark says, and it is
    //the fourth fact rather than a fourth shade of one of the other three.
    const bool dropped_ok = load_board_icon(this, "refresh", 14, m_icon_dropped);
    m_icons_ok            = sliced_ok || stale_ok || problem_ok || dropped_ok;

    m_anim_timer.SetOwner(this, PLATE_BOARD_ANIM_TIMER_ID);
    m_hover_timer.SetOwner(this, PLATE_BOARD_HOVER_TIMER_ID);
    m_autoscroll_timer.SetOwner(this, PLATE_BOARD_AUTOSCROLL_TIMER_ID);

    Bind(wxEVT_PAINT, &PlateBoard::on_paint, this);
    Bind(wxEVT_MOTION, &PlateBoard::on_mouse, this);
    Bind(wxEVT_LEFT_DOWN, &PlateBoard::on_left_down, this);
    Bind(wxEVT_LEFT_UP, &PlateBoard::on_mouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &PlateBoard::on_mouse, this);
    Bind(wxEVT_MOUSEWHEEL, &PlateBoard::on_scroll, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &PlateBoard::on_capture_lost, this);
    Bind(wxEVT_TIMER, &PlateBoard::on_anim_tick, this, PLATE_BOARD_ANIM_TIMER_ID);
    Bind(wxEVT_TIMER, &PlateBoard::on_hover_tick, this, PLATE_BOARD_HOVER_TIMER_ID);
    Bind(wxEVT_TIMER, &PlateBoard::on_autoscroll_tick, this, PLATE_BOARD_AUTOSCROLL_TIMER_ID);
    //The sidebar hides the board below two plates, and a hidden parent does not hide a popup
    //that is a top-level window in its own right. Without this the preview outlives the
    //control it belongs to and floats over the 3D scene with nothing to dismiss it.
    Bind(wxEVT_SHOW, [this](wxShowEvent &evt) {
        if (!evt.IsShown())
            hide_row_preview();
        evt.Skip();
    });
    //THE LAYOUT IS A FUNCTION OF THE WIDTH, so it is recomputed when the width changes. A
    //tile band's column count is read off the client size when the items are built; frozen
    //at that value, narrowing the sidebar would stop DRAWING plates that the hit test still
    //resolves clicks on - a control that has quietly become a lie about what is on screen,
    //which is the same silent gate as a button that does nothing.
    Bind(wxEVT_SIZE, [this](wxSizeEvent &evt) {
        evt.Skip();
        rebuild_items();
        clamp_scroll();
        Refresh();
    });
}

bool PlateBoard::apply_visibility(int plate_count)
{
    //One plate is the old single-printer case, and a lone row would only restate the Project
    //row above it.
    const bool show = plate_count > 1;
    if (IsShown() == show)
        return false;

    Show(show);
    //The board's parent is the printer panel's content panel, so this is the same layout the
    //sidebar used to perform on its behalf.
    if (GetParent() != nullptr)
        GetParent()->Layout();
    return true;
}

void PlateBoard::reload()
{
    //rows are about to be re-filed; an in-flight caption edit commits rather than
    //floating over a row that may no longer be under it
    commit_rename(true);
    //is_initialized(): the board is created during Plater::priv's constructor, so it
    //can be asked to reload before there is a plate list to read
    if (m_plater == nullptr || !m_plater->is_initialized() || wxGetApp().preset_bundle == nullptr)
        return;

    //The rows are about to move under it, and a preview left pointing at a row that has
    //changed index is a picture of the wrong plate.
    hide_row_preview();

    const PartPlateList &plates = m_plater->get_partplate_list();
    const int            count  = plates.get_plate_count();

    //A grouping mode is per-project session state. A different project - and a new project,
    //which always arrives at one plate - takes the default back, because a twenty-plate
    //project must not leave a two-plate one grouped by machine with its rows out of index
    //order. Collapse state and scroll position belong to the same project and go with it.
    const wxString project_id = m_plater->get_project_filename();
    if (!m_project_seen || project_id != m_project_id || count <= 1) {
        m_project_seen      = true;
        m_project_id        = project_id;
        m_grouping_explicit = false;
        m_collapsed.clear();
        m_scroll_px = 0;
    }
    if (!m_grouping_explicit)
        m_grouping = PlateBoardModel::default_grouping(count);

    m_model.rebuild(plates, *wxGetApp().preset_bundle, m_grouping);
    m_current_plate = plates.get_curr_plate_index();
    //A reload means new work arrived, so a heal that gave up earlier is worth trying again.
    m_thumb_heal_blocked = false;

    {
        PETKOS_PERF_SCOPE(Perf::Probe::BoardReloadItems);
        sync_glyph_targets();
        rebuild_items();
        clamp_scroll();
    }

    //Decided here, after the rows exist, so the board is never shown empty and never left
    //hidden while full.
    apply_visibility(count);
    //A row count change changes the height this control asks the sizer for, and nothing
    //below it moves until somebody lays the panel out. Only a real change asks, because
    //reload() runs on every preset update and a parent-wide layout on each of those is a
    //storm rather than a refresh.
    {
        PETKOS_PERF_SCOPE(Perf::Probe::BoardReloadSize);
        const int best = DoGetBestSize().GetHeight();
        if (best != m_best_height) {
            m_best_height = best;
            InvalidateBestSize();
            if (GetParent() != nullptr)
                GetParent()->Layout();
        }
    }
    Refresh();
}

void PlateBoard::reload_plate(int plate_index)
{
    //Same guards as reload(): the board exists before the plate list does.
    if (m_plater == nullptr || !m_plater->is_initialized() || wxGetApp().preset_bundle == nullptr)
        return;

    const PartPlateList &plates = m_plater->get_partplate_list();

    //Grouping by machine re-files a row the moment its machine changes, so the rows can
    //move here exactly as they do in reload() - which is why an in-flight caption edit
    //commits and a row preview is dismissed, rather than floating over a row that has
    //shifted underneath it.
    commit_rename(true);
    hide_row_preview();

    if (!m_model.refresh_plate(plate_index, plates, *wxGetApp().preset_bundle, m_grouping)) {
        //The model declined - the row set is not the one it was built from. Whatever
        //changed is bigger than one plate, so read all of it.
        reload();
        return;
    }

    m_current_plate      = plates.get_curr_plate_index();
    m_thumb_heal_blocked = false;

    //Cheap, and it costs nothing when nothing changed - but a plate list that grew past one
    //while the board was hidden has to be able to arrive on screen from this path too.
    apply_visibility(plates.get_plate_count());

    {
        PETKOS_PERF_SCOPE(Perf::Probe::BoardReloadItems);
        sync_glyph_targets();
        rebuild_items();
        clamp_scroll();
    }

    //The row COUNT cannot have changed here, so the height this control asks for cannot
    //have either. The check is kept rather than assumed away because it is one integer
    //compare, and being wrong about it would leave the sidebar laid out for a board of a
    //different size.
    {
        PETKOS_PERF_SCOPE(Perf::Probe::BoardReloadSize);
        const int best = DoGetBestSize().GetHeight();
        if (best != m_best_height) {
            m_best_height = best;
            InvalidateBestSize();
            if (GetParent() != nullptr)
                GetParent()->Layout();
        }
    }
    Refresh();
}

void PlateBoard::set_grouping(PlateBoardGrouping grouping)
{
    //ByCapacity is no longer a tab of its own; state persisted before the merge folds
    //into the Machine grouping that now carries its queue bars
    if (grouping == PlateBoardGrouping::ByCapacity)
        grouping = PlateBoardGrouping::ByMachine;

    if (m_grouping == grouping && m_grouping_explicit)
        return;

    m_grouping          = grouping;
    m_grouping_explicit = true;
    m_scroll_px         = 0;
    //the rows are about to be re-filed, so a preview anchored to one of them is stale
    hide_row_preview();
    //Whether a drag has anything to land on is a property of the mode, so the sentence that
    //says where the gesture works is owed again once the mode changes.
    m_drag_hint_shown = false;

    if (m_plater != nullptr && m_plater->is_initialized() && wxGetApp().preset_bundle != nullptr)
        m_model.rebuild(m_plater->get_partplate_list(), *wxGetApp().preset_bundle, m_grouping);

    //the rows are the same rows in a different order, so this settles rather than animates;
    //it is here so the glyph state can never be left describing a model it did not see
    sync_glyph_targets();
    rebuild_items();
    clamp_scroll();

    const int best = DoGetBestSize().GetHeight();
    if (best != m_best_height) {
        m_best_height = best;
        InvalidateBestSize();
        if (GetParent() != nullptr)
            GetParent()->Layout();
    }
    Refresh();
}

void PlateBoard::sync_glyph_targets()
{
    const std::vector<PlateBoardRow> &rows = m_model.rows();

    //Where every glyph is being drawn right now, which is what a new animation has to
    //start from: a second reassignment mid-flight must not snap back to the size the first
    //one started at.
    const double t = m_anim_running ? smoothstep((double) m_anim_elapsed_ms / PLATE_BOARD_ANIM_MS) : 1.;
    std::vector<GlyphAnim> displayed(m_glyphs.size());
    for (size_t i = 0; i < m_glyphs.size(); ++i) {
        displayed[i].from_w = m_glyphs[i].from_w + (m_glyphs[i].to_w - m_glyphs[i].from_w) * t;
        displayed[i].from_h = m_glyphs[i].from_h + (m_glyphs[i].to_h - m_glyphs[i].from_h) * t;
    }

    const size_t previous = m_glyphs.size();
    m_glyphs.assign(rows.size(), GlyphAnim());

    bool changed = false;
    for (size_t i = 0; i < rows.size(); ++i) {
        GlyphAnim &anim = m_glyphs[i];
        anim.to_w = rows[i].bed_w;
        anim.to_h = rows[i].bed_d;

        //a row that did not exist a moment ago has no previous size to animate from, so it
        //arrives at its own size instead of growing out of nothing
        if (i >= previous || displayed[i].from_w <= 0. || displayed[i].from_h <= 0.) {
            anim.from_w = anim.to_w;
            anim.from_h = anim.to_h;
            continue;
        }

        anim.from_w = displayed[i].from_w;
        anim.from_h = displayed[i].from_h;
        if (std::fabs(anim.from_w - anim.to_w) > 0.5 || std::fabs(anim.from_h - anim.to_h) > 0.5)
            changed = true;
    }

    if (!changed) {
        //nothing is moving: freeze every glyph at its target so a later tick cannot
        //resurrect a stale interpolation
        for (GlyphAnim &anim : m_glyphs) {
            anim.from_w = anim.to_w;
            anim.from_h = anim.to_h;
        }
        m_anim_running    = false;
        m_anim_elapsed_ms = PLATE_BOARD_ANIM_MS;
        if (m_anim_timer.IsRunning())
            m_anim_timer.Stop();
        return;
    }

    m_anim_running    = true;
    m_anim_elapsed_ms = 0;
    if (!m_anim_timer.IsRunning())
        m_anim_timer.Start(PLATE_BOARD_ANIM_STEP);
}

void PlateBoard::on_anim_tick(wxTimerEvent &)
{
    m_anim_elapsed_ms += PLATE_BOARD_ANIM_STEP;
    if (m_anim_elapsed_ms >= PLATE_BOARD_ANIM_MS) {
        m_anim_elapsed_ms = PLATE_BOARD_ANIM_MS;
        m_anim_running    = false;
        m_anim_timer.Stop();
    }
    Refresh();
}

void PlateBoard::rebuild_items()
{
    m_items.clear();
    m_content_height = 0;
    m_item_quantum   = 0;

    const std::vector<PlateBoardRow>   &rows   = m_model.rows();
    const std::vector<PlateBoardGroup> &groups = m_model.groups();

    auto push = [&](bool header, int group_index, int row_index, int height) {
        Item item;
        item.header = header;
        item.group  = group_index;
        item.row    = row_index;
        item.height = height;
        item.y      = m_content_height;
        m_content_height += height;
        m_item_quantum   = m_item_quantum > 0 ? std::min(m_item_quantum, height) : height;
        m_items.push_back(item);
    };

    if (groups.empty()) {
        //Plate order. Compact above eight rows for the same reason a group is: at that
        //length the list stops being read and starts being scanned. The printer name stays,
        //because in this mode nothing above the row is naming it.
        const int height = m_row_height;
        for (int i = 0; i < (int) rows.size(); ++i)
            push(false, -1, i, height);
        return;
    }

    bool skip_children = false;
    for (int g = 0; g < (int) groups.size(); ++g) {
        const PlateBoardGroup &group = groups[(size_t) g];

        if (group.depth == 0)
            skip_children = false;
        else if (skip_children)
            continue;

        push(true, g, -1, m_header_height);

        if (m_collapsed.count(group.key) > 0) {
            if (group.depth == 0)
                skip_children = true;
            continue;
        }

        //A group whose header already names its machine, holding more than a handful of
        //plates, draws them as tiles. See PLATE_BOARD_TILE_ABOVE: under that header the
        //row's arrow and machine picture repeat a fact the header states once.
        if (group.names_machine && (int) group.rows.size() > PLATE_BOARD_TILE_ABOVE) {
            int tile = 0, gap = 0, inset = 0;
            tile_metrics(tile, gap, inset);
            const int cols = tile_columns(GetClientSize().GetWidth());
            for (int first = 0; first < (int) group.rows.size(); first += cols) {
                Item band;
                band.group      = g;
                band.tile_first = first;
                band.tile_count = std::min(cols, (int) group.rows.size() - first);
                band.tile_cols  = cols;
                band.height     = tile + gap;
                band.y          = m_content_height;
                m_content_height += band.height;
                m_item_quantum   = m_item_quantum > 0 ? std::min(m_item_quantum, band.height) : band.height;
                m_items.push_back(band);
            }
            continue;
        }

        const int height = m_row_height;
        for (int row_index : group.rows)
            push(false, g, row_index, height);
    }
}

void PlateBoard::tile_metrics(int &tile, int &gap, int &inset) const
{
    //34 px is a plate you can tell apart - the bed reads at proportion and the number is
    //legible - in half a rich row's height. The inset is the index gutter a row spends on
    //its plate number, kept so a band lines up with the rows above and below it.
    tile  = FromDIP(34);
    gap   = FromDIP(4);
    inset = FromDIP(8);
}

int PlateBoard::tile_columns(int width) const
{
    int tile = 0, gap = 0, inset = 0;
    tile_metrics(tile, gap, inset);
    //At least one column, whatever the width: a band of zero columns is a group whose plates
    //are laid out nowhere, which loses them rather than crowding them.
    const int usable = std::max(tile, width - 2 * inset);
    return std::max(1, (usable + gap) / (tile + gap));
}

int PlateBoard::rollup_height() const
{
    //hidden entirely at one plate, where every cell would restate the row below it
    if (m_model.rows().size() <= 1)
        return 0;
    return m_rollup_height + (m_model.rollup().not_estimated > 0 ? FromDIP(14) : 0);
}

int PlateBoard::grouping_height() const
{
    return m_model.rows().size() <= 1 ? 0 : m_segment_height;
}

int PlateBoard::view_height() const
{
    return std::max(0, GetClientSize().GetHeight() - view_top());
}

void PlateBoard::clamp_scroll()
{
    const int max_scroll = std::max(0, m_content_height - view_height());
    m_scroll_px          = std::min(std::max(0, m_scroll_px), max_scroll);
}

int PlateBoard::find_row_item(int plate_index) const
{
    const std::vector<PlateBoardRow>   &rows   = m_model.rows();
    const std::vector<PlateBoardGroup> &groups = m_model.groups();
    for (int i = 0; i < (int) m_items.size(); ++i) {
        const Item &item = m_items[(size_t) i];
        if (item.header)
            continue;

        //A plate drawn as a tile lives in a BAND, which has no row of its own. This is the
        //ONE place that knows it: every caller that has to turn a plate into an item -
        //scrolling to the selection, anchoring the hover preview, resolving a drag onto a
        //group - goes through here, so a tiled group cannot lose any of them one at a time.
        if (item.tile_count > 0) {
            if (item.group < 0 || item.group >= (int) groups.size())
                continue;
            const PlateBoardGroup &group = groups[(size_t) item.group];
            for (int c = 0; c < item.tile_count; ++c) {
                const int index = item.tile_first + c;
                if (index < 0 || index >= (int) group.rows.size())
                    break;
                const int row_index = group.rows[(size_t) index];
                if (row_index >= 0 && row_index < (int) rows.size() &&
                    rows[(size_t) row_index].plate_index == plate_index)
                    return i;
            }
            continue;
        }

        if (item.row >= 0 && item.row < (int) rows.size() &&
            rows[(size_t) item.row].plate_index == plate_index)
            return i;
    }
    return -1;
}

void PlateBoard::scroll_row_into_view(int plate_index)
{
    int item_index = find_row_item(plate_index);

    //A selected plate inside a collapsed group is a plate the board is hiding from the
    //user while the 3D scene shows it. That is a silent gate, so the group opens.
    if (item_index < 0) {
        const std::vector<PlateBoardRow>   &rows   = m_model.rows();
        const std::vector<PlateBoardGroup> &groups = m_model.groups();
        for (int g = 0; g < (int) groups.size(); ++g)
            for (int row_index : groups[(size_t) g].rows)
                if (row_index >= 0 && row_index < (int) rows.size() &&
                    rows[(size_t) row_index].plate_index == plate_index) {
                    m_collapsed.erase(groups[(size_t) g].key);
                    //a machine sub-group is only visible while its material group is open
                    for (int p = g; p >= 0; --p)
                        if (groups[(size_t) p].depth == 0) {
                            m_collapsed.erase(groups[(size_t) p].key);
                            break;
                        }
                    g = (int) groups.size(); //break the outer loop too
                    break;
                }
        rebuild_items();
        item_index = find_row_item(plate_index);
    }

    if (item_index < 0)
        return;

    const Item &item   = m_items[(size_t) item_index];
    const int   height = view_height();
    //one header's worth of headroom, because the sticky header is drawn over the top of
    //the scroll region and would otherwise cover the row it just scrolled to
    const int   sticky = m_model.groups().empty() ? 0 : m_header_height;

    if (item.y - sticky < m_scroll_px)
        m_scroll_px = std::max(0, item.y - sticky);
    else if (item.y + item.height > m_scroll_px + height)
        m_scroll_px = item.y + item.height - height;

    clamp_scroll();
}

void PlateBoard::on_plate_selection_changed(int current_plate)
{
    //idempotent: a double delivery costs nothing, so no argument about whether one
    //can happen has to be won
    if (current_plate == m_current_plate)
        return;

    m_current_plate = current_plate;
    //scrolling and opening a collapsed group both move the rows a preview is anchored to
    hide_row_preview();
    scroll_row_into_view(current_plate);
    Refresh();
}

wxSize PlateBoard::DoGetBestSize() const
{
    //Past a handful of standard rows the board scrolls instead of growing. A narrow
    //sidebar's real failure mode is not a long list, it is a long list pushing the nozzle,
    //bed and extruder controls below it off the screen.
    //
    //The cap is a whole number of rich rows, and the content is snapped to a whole number of
    //ITEMS. An item bisected by the panel's own edge, with nothing saying the list continues,
    //is the strongest "this app is broken" signal the sidebar can produce, and it was showing
    //on a project with three plates in it.
    //
    //Snapped to the last item that fits ENTIRELY, not to a quantum: headers are 26 px, tile
    //bands 38 and rows 64, so no single unit divides the list and a quantum would bisect
    //exactly the bands tiling exists for. m_item_quantum is the floor rather than the unit,
    //so a board that cannot fit even its first item still shows something.
    const int cap = PLATE_BOARD_VISIBLE_ROWS * m_row_height;
    int       whole = 0;
    for (const Item &item : m_items) {
        if (item.y + item.height > cap)
            break;
        whole = item.y + item.height;
    }
    const int floor_height = m_item_quantum > 0 ? m_item_quantum : m_row_height;
    return wxSize(-1, rollup_height() + grouping_height() + std::max(whole, floor_height) + FromDIP(4));
}

int PlateBoard::segment_at(int x) const
{
    const int width = GetClientSize().GetWidth();
    if (width <= 0)
        return -1;
    const int index = x * 3 / std::max(1, width);
    return index < 0 ? -1 : std::min(2, index);
}

int PlateBoard::sticky_group() const
{
    if (m_model.groups().empty())
        return -1;

    int sticky = -1;
    for (const Item &item : m_items) {
        if (item.y > m_scroll_px)
            break;
        if (item.header)
            sticky = item.group;
        else if (item.group >= 0)
            sticky = item.group;
    }
    return sticky;
}

PlateBoard::Hit PlateBoard::hit_test(const wxPoint &pos) const
{
    Hit hit;

    const int rollup = rollup_height();
    if (rollup > 0 && pos.y < rollup) {
        hit.kind = HitKind::Rollup;
        return hit;
    }

    const int grouping = grouping_height();
    if (grouping > 0 && pos.y < rollup + grouping) {
        hit.kind  = HitKind::Segment;
        hit.index = segment_at(pos.x);
        return hit;
    }

    const int top = rollup + grouping;

    //the sticky header is painted over the top of the scroll region, so it takes the
    //clicks that land on it rather than passing them to the row it is covering
    const int sticky = sticky_group();
    if (sticky >= 0 && pos.y < top + m_header_height && m_scroll_px > 0) {
        hit.kind  = HitKind::GroupHeader;
        hit.index = sticky;
        return hit;
    }

    const int y = pos.y - top + m_scroll_px;
    for (const Item &item : m_items) {
        if (y < item.y || y >= item.y + item.height)
            continue;
        if (item.tile_count > 0) {
            //A band is several plates on one line, so which one it is depends on x. Landing
            //between two tiles is not a hit on either - the gap belongs to neither plate,
            //and a click that picked the nearest would select a plate the user did not
            //point at.
            const int row_index = tile_at(item, item.y, wxPoint(pos.x, y));
            if (row_index < 0)
                return hit;
            hit.kind  = HitKind::Row;
            hit.index = row_index;
            return hit;
        }
        hit.kind  = item.header ? HitKind::GroupHeader : HitKind::Row;
        hit.index = item.header ? item.group : item.row;
        return hit;
    }
    return hit;
}

int PlateBoard::tile_at(const Item &item, int band_y, const wxPoint &pos) const
{
    const std::vector<PlateBoardGroup> &groups = m_model.groups();
    if (item.group < 0 || item.group >= (int) groups.size())
        return -1;
    const PlateBoardGroup &group = groups[(size_t) item.group];

    int tile = 0, gap = 0, inset = 0;
    tile_metrics(tile, gap, inset);

    const int local = pos.x - inset;
    if (local < 0)
        return -1;
    const int column = local / (tile + gap);
    if (column < 0 || column >= item.tile_count)
        return -1;
    if (local - column * (tile + gap) >= tile)
        return -1; //the gap between two tiles
    if (pos.y - band_y >= tile)
        return -1; //the gap under the band

    const int index = item.tile_first + column;
    if (index < 0 || index >= (int) group.rows.size())
        return -1;
    return group.rows[(size_t) index];
}

void PlateBoard::on_scroll(wxMouseEvent &evt)
{
    //the preview is anchored to a row's screen rectangle, so it is wrong the moment the
    //rows move under it
    hide_row_preview();

    const int max_scroll = std::max(0, m_content_height - view_height());
    if (max_scroll == 0) {
        evt.Skip();
        return;
    }

    const int delta = evt.GetWheelDelta() > 0 ? evt.GetWheelRotation() / evt.GetWheelDelta() : 0;
    m_scroll_px     = std::min(max_scroll, std::max(0, m_scroll_px - delta * m_row_height));
    Refresh();
}

void PlateBoard::open_picker(int plate_index, const wxPoint &screen_anchor)
{
    show_printer_picker(this, m_plater, plate_index, m_scoped_plates, screen_anchor, m_popup);
}

// ----------------------------------------------------------------------------
// the row's hover thumbnail
// ----------------------------------------------------------------------------

void PlateBoard::hide_row_preview()
{
    if (m_hover_timer.IsRunning())
        m_hover_timer.Stop();
    if (m_preview != nullptr && m_preview->IsShown())
        m_preview->Hide();
}

void PlateBoard::on_hover_tick(wxTimerEvent &)
{
    //The dwell has expired. What the pointer is over NOW is the only thing worth showing,
    //so the row is read back from the hover state rather than remembered when the clock
    //started: between the two the pointer may have moved on.
    if (m_dragging || m_hover.kind != HitKind::Row)
        return;
    show_row_preview(m_hover.index);
}

void PlateBoard::show_row_preview(int row_index)
{
    if (m_plater == nullptr || !m_plater->is_initialized())
        return;

    const std::vector<PlateBoardRow> &rows = m_model.rows();
    if (row_index < 0 || row_index >= (int) rows.size())
        return;

    const PlateBoardRow &row         = rows[(size_t) row_index];
    const int            plate_index = row.plate_index;
    PartPlate *          plate       = m_plater->get_partplate_list().get_plate(plate_index);
    if (plate == nullptr)
        return;

    const int item_index = find_row_item(plate_index);
    if (item_index < 0)
        return;

    wxString note;

    //Whether what is stored still describes the scene. The flag is READ here and never
    //cleared: the in-canvas plate strip owns the clear, and clearing it from the board
    //would leave the strip rebuilding no items for buffers that had changed under it.
    //
    //The consequence, stated so it is not mistaken for free: the strip only clears the flag
    //while the Preview tab is up, so in the 3D editor it stays set and every dwell re-renders
    //one plate. That is one 512x512 offscreen render per 380 ms of deliberate pointing, on a
    //gesture the user made; it is not a per-frame cost and it is not per plate.
    const bool stale = m_plater->is_plate_toolbar_image_dirty();

    if (m_plater->is_gcode_3mf()) {
        //A project opened from an exported G-code 3MF carries its plate images in the file
        //and has no model to re-render. What is there is the truth; what is not there
        //cannot be produced, and saying so is the honest answer.
        if (!plate->thumbnail_data.is_valid())
            note = _L("This project was opened from G-code and carries no image for this plate.");
    } else if (!plate->thumbnail_data.is_valid() || stale) {
        //The 3D editor's canvas owns both the geometry this renders and the GL context the
        //render needs current, so it is asked rather than driven: refresh_plate_thumbnail
        //makes its own context current and declines when it is not the canvas on screen.
        //When the Preview tab is showing, the plate strip over there has just refreshed
        //these same buffers, so the stored image is already current.
        //
        //One plate, not all of them: the strip's refresh path renders two 512x512 images for
        //every plate in the project, which is the right shape for a strip that draws them all
        //and the wrong shape for a hover that shows one.
        GLCanvas3D *canvas = m_plater->get_view3D_canvas3D();
        if (canvas != nullptr)
            canvas->refresh_plate_thumbnail(plate_index);
        if (!plate->thumbnail_data.is_valid())
            note = _L("No image for this plate yet.");
    }

    wxBitmap bitmap;
    if (plate->thumbnail_data.is_valid()) {
        wxImage image = thumbnail_to_image(plate->thumbnail_data);
        if (image.IsOk()) {
            const int side = FromDIP(168);
            bitmap         = wxBitmap(image.Rescale(side, side, wxIMAGE_QUALITY_HIGH));
        }
    }

    //The caption is the plate number and nothing else. Everything else the preview could
    //say - machine, hours, parts - is already on the row it is anchored to, and the number
    //is the one thing that ties a picture floating beside the sidebar back to that row.
    const wxString caption = wxString::Format(_L("Plate %d"), plate_index + 1);

    if (m_preview == nullptr)
        m_preview = new PlateThumbnailPreview(this);

    const Item &  item = m_items[(size_t) item_index];
    const wxPoint top_left = ClientToScreen(wxPoint(0, view_top() + item.y - m_scroll_px));
    const wxRect  row_rect(top_left.x, top_left.y, GetClientSize().GetWidth(), item.height);

    m_preview->show_beside(row_rect, bitmap, caption, note);
}

// ----------------------------------------------------------------------------
// drag a row onto a group header
// ----------------------------------------------------------------------------

bool PlateBoard::grouping_allows_drag() const
{
    //By machine and by capacity only. Plate order draws no headers at all, and by material
    //the headers key on a colour, which is not a thing a plate can be assigned to. A drag
    //attempted in either is answered by say_where_drag_works rather than by nothing.
    return m_grouping == PlateBoardGrouping::ByMachine || m_grouping == PlateBoardGrouping::ByCapacity;
}

int PlateBoard::drop_group_at(const wxPoint &pos) const
{
    const Hit hit = hit_test(pos);

    int group_index = -1;
    if (hit.kind == HitKind::GroupHeader) {
        group_index = hit.index;
    } else if (hit.kind == HitKind::Row) {
        //A row resolves UP to the group it sits in. This is NOT a drop onto a row: the
        //highlight is painted on the group's header and the group is what gets assigned, so
        //no position inside a group can be expressed and nothing can be reordered - which is
        //the point, because plate index is load-bearing in the prep pipeline's filenames.
        //It is here because a 26 px header in a 34 px row list is a target the user has to
        //aim at, while the run of rows underneath it means the same machine and is twenty
        //times the area.
        //
        //Resolved through find_row_item rather than by scanning for item.row: a plate drawn
        //as a tile has no item of its own, so matching on item.row would find nothing in
        //exactly the large machine groups tiling exists for - and drag-to-assign would die
        //there without saying a word.
        const std::vector<PlateBoardRow> &rows = m_model.rows();
        if (hit.index >= 0 && hit.index < (int) rows.size()) {
            const int item_index = find_row_item(rows[(size_t) hit.index].plate_index);
            if (item_index >= 0)
                group_index = m_items[(size_t) item_index].group;
        }
    }

    const std::vector<PlateBoardGroup> &groups = m_model.groups();
    if (group_index < 0 || group_index >= (int) groups.size() || !groups[(size_t) group_index].drop_target)
        return -1;
    return group_index;
}

std::vector<int> PlateBoard::drag_targets() const
{
    std::vector<int> targets;
    if (m_drag_plate < 0)
        return targets;

    //A drag carries the selection when the row it grabbed is part of one, and exactly that
    //row otherwise. Scoped rows are painted differently, so the blast radius is on screen
    //before the drop - the one condition this design puts on a bulk write.
    if (m_scoped_plates.size() > 1 &&
        std::find(m_scoped_plates.begin(), m_scoped_plates.end(), m_drag_plate) != m_scoped_plates.end())
        return m_scoped_plates;

    targets.push_back(m_drag_plate);
    return targets;
}

void PlateBoard::on_left_down(wxMouseEvent &evt)
{
    //Binding LEFT_DOWN at all is new; skipping keeps whatever wxPanel did with it before,
    //rather than making "arm a drag" quietly also mean "swallow the button press". Skipped
    //on every path, including the ones that arm nothing.
    evt.Skip();

    hide_row_preview();

    m_drag_armed   = false;
    m_dragging     = false;
    m_drag_attempt = false;
    m_drag_plate   = PLATE_BOARD_NO_PLATE;
    m_drop_group   = -1;

    if (m_plater == nullptr || !m_plater->is_initialized())
        return;
    //a modifier-click is a scope change, handled on the button up. Arming a drag as well
    //would mean the same gesture could both extend the selection and move a plate.
    if (evt.ControlDown() || evt.ShiftDown())
        return;

    const Hit hit = hit_test(evt.GetPosition());
    if (hit.kind != HitKind::Row || hit.index < 0 || hit.index >= (int) m_model.rows().size())
        return;

    m_press_pos = evt.GetPosition();

    //In plate order and by material there is no machine group to land on, so the row is not
    //draggable. That is recorded rather than ignored: a user who has used the gesture in the
    //other two modes and gets nothing here has been refused by silence, which is the one
    //answer this board is not allowed to give.
    if (!grouping_allows_drag()) {
        m_drag_attempt = true;
        return;
    }

    m_drag_armed = true;
    m_drag_plate = m_model.rows()[(size_t) hit.index].plate_index;
}

void PlateBoard::say_where_drag_works()
{
    m_drag_hint_shown = true;
    if (m_plater == nullptr)
        return;
    if (NotificationManager *notifications = m_plater->get_notification_manager())
        notifications->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
            into_u8(_L("A plate is reassigned by dragging it onto a machine group. Group the board by "
                       "machine or by capacity first, or click the row's printer name to pick one.")));
}

void PlateBoard::on_capture_lost(wxMouseCaptureLostEvent &)
{
    //The capture can be taken away by anything from a modal dialog to the window manager.
    //A drag that ends this way commits nothing: the user did not let go over a target.
    m_dragging   = false;
    m_drag_armed = false;
    m_drop_group = -1;
    if (m_autoscroll_timer.IsRunning())
        m_autoscroll_timer.Stop();
    m_autoscroll_dir = 0;
    Refresh();
}

int PlateBoard::autoscroll_direction(const wxPoint &pos) const
{
    //Nothing to scroll: the whole board is in view, so an edge means nothing.
    if (m_content_height <= view_height())
        return 0;

    //One header's worth of band at each end of the scroll region. The top band starts at
    //view_top() and anything above it is NOT a scroll: the rollup and the grouping segments
    //sit there, and resting on them while thinking must not read as "scroll up for ever".
    //Below the control there is no such furniture, so a captured drag dragged past the last
    //row keeps going, which is what every list that scrolls on drag does.
    const int top    = view_top();
    const int bottom = GetClientSize().GetHeight();
    if (pos.y < top)
        return 0;
    if (pos.y < top + m_header_height)
        return -1;
    if (pos.y > bottom - m_header_height)
        return 1;
    return 0;
}

void PlateBoard::update_autoscroll(const wxPoint &pos)
{
    const int direction = m_dragging ? autoscroll_direction(pos) : 0;
    if (direction == m_autoscroll_dir)
        return;

    m_autoscroll_dir = direction;
    if (direction == 0) {
        if (m_autoscroll_timer.IsRunning())
            m_autoscroll_timer.Stop();
        return;
    }
    if (!m_autoscroll_timer.IsRunning())
        m_autoscroll_timer.Start(PLATE_BOARD_AUTOSCROLL_MS);
}

void PlateBoard::on_autoscroll_tick(wxTimerEvent &)
{
    if (!m_dragging || m_autoscroll_dir == 0) {
        m_autoscroll_timer.Stop();
        m_autoscroll_dir = 0;
        return;
    }

    const int previous = m_scroll_px;
    m_scroll_px += m_autoscroll_dir * std::max(1, m_row_height / PLATE_BOARD_AUTOSCROLL_DIV);
    clamp_scroll();
    if (m_scroll_px == previous) {
        //Already at the end. Stop rather than tick against the clamp for as long as the
        //button is held.
        m_autoscroll_timer.Stop();
        m_autoscroll_dir = 0;
        return;
    }

    //The rows moved under a stationary pointer, so what is beneath it is a different group
    //than it was a tick ago. Recomputed here because no motion event is coming.
    m_drop_group = drop_group_at(m_drag_pos);
    Refresh();
}

void PlateBoard::end_drag(bool commit)
{
    const int  group_index  = m_drop_group;
    const int  plate        = m_drag_plate;
    const bool was_dragging = m_dragging;

    if (HasCapture())
        ReleaseMouse();
    m_dragging   = false;
    m_drag_armed = false;
    m_drop_group = -1;
    if (m_autoscroll_timer.IsRunning())
        m_autoscroll_timer.Stop();
    m_autoscroll_dir = 0;
    Refresh();

    //group_index < 0 is a release over nothing, which is a cancel and not a refusal: the pill
    //under the cursor has been drawn dimmed for the whole time there was no target, so the
    //gesture has already said what letting go here would do.
    if (!commit || !was_dragging || plate < 0 || group_index < 0)
        return;
    if (m_plater == nullptr || !m_plater->is_initialized())
        return;

    const std::vector<PlateBoardGroup> &groups = m_model.groups();
    if (group_index >= (int) groups.size())
        return;
    const PlateBoardGroup &group = groups[(size_t) group_index];
    if (!group.drop_target)
        return;

    const std::vector<int> targets = drag_targets();
    if (targets.empty())
        return;

    //Nothing here renumbers anything. The drop changes which machine the plate is assigned
    //to and nothing else, because plate index is what every filename the prep pipeline
    //writes is keyed on.
    const PartPlateList &plates  = m_plater->get_partplate_list();
    int                  changed = 0;
    for (int idx : targets) {
        const PartPlate *target = plates.get_plate(idx);
        if (target != nullptr && target->get_printer_preset_name() != group.machine)
            ++changed;
    }

    if (changed == 0) {
        //The write path returns early on an unchanged name, so without this a drop onto the
        //group a plate is already in would move nothing and say nothing, which is the
        //silent no-op this fork treats as a bug. It names the plate and the machine.
        //
        //CustomNotification rather than BBLPlateInfo, which is what the plate-side code
        //elsewhere uses: NotificationManager::set_in_preview HIDES every BBLPlateInfo while
        //the Preview tab is up, and this board is in the sidebar, which is up on both tabs.
        //A message that disappears on one tab is the silence it was written to replace.
        //CustomNotification is also in m_multiple_types, compared by text, so repeating the
        //same drop does not stack the same sentence.
        wxString message;
        if (targets.size() == 1)
            message = group.machine.empty()
                          ? wxString::Format(_L("Plate %d already follows the project printer."), plate + 1)
                          : wxString::Format(_L("Plate %d is already assigned to %s."), plate + 1,
                                             from_u8(group.machine));
        else
            message = group.machine.empty()
                          ? wxString::Format(_L("Those %d plates already follow the project printer."),
                                             (int) targets.size())
                          : wxString::Format(_L("Those %d plates are already assigned to %s."),
                                             (int) targets.size(), from_u8(group.machine));

        if (NotificationManager *notifications = m_plater->get_notification_manager())
            notifications->push_notification(NotificationType::CustomNotification,
                                             NotificationManager::NotificationLevel::RegularNotificationLevel,
                                             into_u8(message));
        return;
    }

    //Deferred, because the assignment rebuilds this control from inside the mouse handler
    //that is still running. One call either way, so the undo snapshot, the bounds re-check
    //and the slice bookkeeping happen once, and the batch is a single undo press.
    Plater *          plater  = m_plater;
    const std::string machine = group.machine;
    if (targets.size() == 1) {
        const int one = targets.front();
        CallAfter([plater, one, machine]() { plater->set_plate_printer(one, machine); });
    } else {
        CallAfter([plater, targets, machine]() { plater->set_plate_printers(targets, machine); });
    }
}

void PlateBoard::set_scope(const std::vector<int> &scoped_plates)
{
    if (m_scoped_plates == scoped_plates)
        return;
    m_scoped_plates = scoped_plates;
    Refresh();
}

void PlateBoard::on_mouse(wxMouseEvent &evt)
{
    if (evt.GetEventType() == wxEVT_LEAVE_WINDOW) {
        //a captured drag keeps receiving motion outside the window, so leaving is not the
        //end of one
        if (!m_dragging) {
            m_hover = Hit();
            hide_row_preview();
            Refresh();
        }
        return;
    }

    const Hit hit = hit_test(evt.GetPosition());

    if (evt.GetEventType() == wxEVT_MOTION) {
        if (m_dragging) {
            m_drag_pos   = evt.GetPosition();
            m_drop_group = drop_group_at(m_drag_pos);
            update_autoscroll(m_drag_pos);
            Refresh();
            return;
        }

        //A press that has travelled past the slop is a drag. on_left_down set exactly one of
        //these two: armed when this grouping has machine groups to drop onto, attempted when
        //it has none, so the same gesture either starts or gets an answer.
        if ((m_drag_armed || m_drag_attempt) && evt.LeftIsDown()) {
            const wxPoint delta = evt.GetPosition() - m_press_pos;
            if (std::abs(delta.x) + std::abs(delta.y) >= FromDIP(PLATE_BOARD_DRAG_SLOP_DIP)) {
                if (m_drag_attempt) {
                    m_drag_attempt = false;
                    if (!m_drag_hint_shown)
                        say_where_drag_works();
                } else {
                    m_dragging = true;
                    m_drag_pos = evt.GetPosition();
                    hide_row_preview();
                    if (!HasCapture())
                        CaptureMouse();
                    m_drop_group = drop_group_at(m_drag_pos);
                    Refresh();
                    return;
                }
            }
        }

        if (hit != m_hover) {
            m_hover = hit;
            //Restart the dwell on every row change, and drop the preview immediately, so
            //what is on screen is never a picture of the row the pointer has just left.
            hide_row_preview();
            if (hit.kind == HitKind::Row)
                m_hover_timer.Start(PLATE_BOARD_HOVER_MS, wxTIMER_ONE_SHOT);
            Refresh();
        }
        return;
    }

    if (evt.GetEventType() != wxEVT_LEFT_UP)
        return;

    //A completed drag is the whole gesture. It must return before the chip test and before
    //select_plate below, or letting go over a header would also select a plate or open a
    //picker the user never asked for.
    if (m_dragging) {
        end_drag(true);
        return;
    }
    m_drag_armed   = false;
    m_drag_attempt = false;
    hide_row_preview();

    if (m_plater == nullptr || !m_plater->is_initialized())
        return;

    switch (hit.kind) {
    case HitKind::Rollup:
        //The rollup states the project's totals and nothing about it is editable, because
        //a project no longer holds a printer, a process or a material for the rollup to be
        //the way in to. Clicking it collapses the scope back to the current plate, which
        //is the one thing "step back out" can honestly mean here.
        m_plater->sidebar().focus_current_plate();
        return;

    case HitKind::Segment:
        if (hit.index >= 0 && hit.index < 4)
            set_grouping(GROUPING_ORDER[hit.index]);
        return;

    case HitKind::GroupHeader: {
        if (hit.index < 0 || hit.index >= (int) m_model.groups().size())
            return;
        const std::string &key = m_model.groups()[(size_t) hit.index].key;
        if (m_collapsed.count(key) > 0)
            m_collapsed.erase(key);
        else
            m_collapsed.insert(key);
        rebuild_items();
        clamp_scroll();
        Refresh();
        return;
    }

    case HitKind::Row: {
        const std::vector<PlateBoardRow> &rows = m_model.rows();
        if (hit.index < 0 || hit.index >= (int) rows.size())
            return;
        const PlateBoardRow &row = rows[(size_t) hit.index];

        //Ctrl/shift-click changes the SCOPE and never the current plate. Checked before
        //the chip, so modifier-clicking a row's printer name extends the selection rather
        //than opening a picker the user did not ask for.
        if (evt.ControlDown() || evt.ShiftDown()) {
            m_plater->sidebar().toggle_scoped_plate(row.plate_index);
            return;
        }

        //A row divides three ways: the caption under the plate render begins an inline
        //rename, the machine half (arrow + printer picture + caption) opens the picker,
        //and everything else selects the plate.
        const int item_index = find_row_item(row.plate_index);
        if (item_index >= 0) {
            const Item &item   = m_items[(size_t) item_index];
            const int   item_y = view_top() + item.y - m_scroll_px;

            //the plate caption band: single click renames. Agents name plates through the
            //same call over MCP; this is the human's end of that contract.
            const int cap_x = FromDIP(24), cap_w = FromDIP(72);
            const int cap_y = item_y + FromDIP(44), cap_h = FromDIP(18);
            if (evt.GetPosition().x >= cap_x && evt.GetPosition().x < cap_x + cap_w &&
                evt.GetPosition().y >= cap_y && evt.GetPosition().y < cap_y + cap_h) {
                begin_rename(row.plate_index, wxRect(cap_x - FromDIP(4), cap_y - FromDIP(2), cap_w + FromDIP(8), cap_h + FromDIP(2)));
                return;
            }

            //the picker chip is the machine half of the row
            const int   chip_x  = FromDIP(100);
            const int   chip_w  = FromDIP(110);
            //a row under a header that already names the machine shows no machine name, so
            //there is no chip on it to click; the picker is reached from the inspector or
            //by dragging the row onto another machine group
            const std::vector<PlateBoardGroup> &groups = m_model.groups();
            const bool names_machine_above = item.group >= 0 && item.group < (int) groups.size() &&
                                             groups[(size_t) item.group].names_machine;
            const bool has_chip = !names_machine_above;
            if (has_chip && chip_w > 0 && evt.GetPosition().x >= chip_x && evt.GetPosition().x < chip_x + chip_w) {
                const int y = item_y + item.height;
                open_picker(row.plate_index, ClientToScreen(wxPoint(0, y)));
                return;
            }
        }

        //the board never sets selection itself: it routes through the same entry point
        //the 3D scene uses, so there is one owner of the current plate
        m_plater->select_plate(row.plate_index);
        return;
    }

    case HitKind::None:
    default:
        return;
    }
}

void PlateBoard::draw_rollup(wxDC &dc, bool dark, int width)
{
    const PlateBoardRollup &rollup = m_model.rollup();
    const int               height = rollup_height();

    //Figures render as figures, each a value over its label. The time tiles exist only
    //once at least one plate has an estimate: a fresh project showing two em-dash tiles
    //and an orange "N not estimated" reads as a broken dashboard, when nothing has
    //happened yet and nothing is wrong.
    struct Tile
    {
        wxString value;
        wxString label;
    };
    const bool has_estimates = rollup.total_seconds > 0.f || rollup.longest_queue_seconds > 0.f;
    std::vector<Tile> tiles = {
        {wxString::Format("%d", rollup.plates), _L_PLURAL("plate", "plates", rollup.plates)},
        {wxString::Format("%d", rollup.machines), _L_PLURAL("machine", "machines", rollup.machines)},
    };
    if (has_estimates) {
        tiles.push_back({format_hours(rollup.total_seconds), _L("total")});
        tiles.push_back({format_hours(rollup.longest_queue_seconds), _L("longest queue")});
    }

    const int cell = std::max(FromDIP(10), width / (int) tiles.size());
    for (size_t i = 0; i < tiles.size(); ++i) {
        const int x     = (int) i * cell + FromDIP(4);
        const int max_w = cell - FromDIP(8);

        dc.SetFont(Label::Head_13);
        dc.SetTextForeground(board_fg(dark));
        dc.DrawText(wxControl::Ellipsize(tiles[i].value, dc, wxELLIPSIZE_END, max_w), x, FromDIP(4));

        dc.SetFont(Label::Body_9);
        dc.SetTextForeground(board_dim(dark));
        dc.DrawText(wxControl::Ellipsize(tiles[i].label, dc, wxELLIPSIZE_END, max_w), x, FromDIP(22));
    }

    //Once time IS shown, an uncounted plate makes the totals a lie by omission, so the
    //note appears exactly when the tiles it corrects do.
    if (has_estimates && rollup.not_estimated > 0) {
        dc.SetFont(Label::Body_9);
        dc.SetTextForeground(board_warn(dark));
        const wxString note   = wxString::Format(_L("%d not estimated"), rollup.not_estimated);
        const wxSize   extent = dc.GetTextExtent(note);
        dc.DrawText(note, std::max(FromDIP(4), width - extent.GetWidth() - FromDIP(4)), m_rollup_height);
    }

    dc.SetPen(wxPen(board_line(dark)));
    dc.DrawLine(0, height - 1, width, height - 1);
}

void PlateBoard::draw_grouping(wxDC &dc, bool dark, int width, int top)
{
    const int height = FromDIP(22);

    dc.SetBrush(wxBrush(board_head(dark)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(0, top, width, height, FromDIP(4));

    const int cell = std::max(FromDIP(10), width / 3);
    for (int i = 0; i < 3; ++i) {
        const bool active = m_grouping == GROUPING_ORDER[i];
        const bool hover  = m_hover.kind == HitKind::Segment && m_hover.index == i;
        const int  x      = i * cell;
        const int  w      = i == 2 ? width - x : cell;

        if (active) {
            dc.SetBrush(wxBrush(board_sel(dark)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(x, top, w, height, FromDIP(4));
        } else if (hover) {
            dc.SetBrush(wxBrush(board_hover(dark)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(x, top, w, height, FromDIP(4));
        }

        dc.SetFont(Label::Body_9);
        dc.SetTextForeground(active ? board_fg(dark) : board_dim(dark));
        const wxString label  = wxControl::Ellipsize(grouping_label(GROUPING_ORDER[i]), dc, wxELLIPSIZE_END,
                                                     w - FromDIP(6));
        const wxSize   extent = dc.GetTextExtent(label);
        dc.DrawText(label, x + (w - extent.GetWidth()) / 2, top + (height - extent.GetHeight()) / 2);
    }
}

void PlateBoard::draw_group_header(wxDC &                 dc,
                                   bool                   dark,
                                   int                    width,
                                   int                    y,
                                   int                    height,
                                   const PlateBoardGroup &group,
                                   bool                   collapsed)
{
    //dark mode grounds the header on gunmetal PEI; light mode keeps the flat head
    if (!PodBanner::draw(dc, wxRect(0, y, width, height), dark)) {
        dc.SetBrush(wxBrush(board_head(dark)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, y, width, height);
    }

    dc.SetPen(wxPen(board_line(dark)));
    dc.DrawLine(0, y + height - 1, width, y + height - 1);

    int x = FromDIP(4) + group.depth * FromDIP(12);

    draw_chevron(dc, board_dim(dark), x, y + (height - FromDIP(8)) / 2, FromDIP(8), !collapsed);
    x += FromDIP(12);

    //a material group carries its colour, which is the only part of "(colour, type)" that
    //a caption cannot say
    if (!group.swatch_colour.empty()) {
        wxColour colour;
        if (colour.Set(from_u8(group.swatch_colour))) {
            const int box = FromDIP(10);
            dc.SetBrush(wxBrush(colour));
            dc.SetPen(wxPen(board_line(dark)));
            dc.DrawRectangle(x, y + (height - box) / 2, box, box);
            x += box + FromDIP(6);
        }
    }

    //right-hand summary first, so the caption knows how much room it has left
    dc.SetFont(Label::Body_9);
    dc.SetTextForeground(board_dim(dark));
    wxString summary = wxString::Format(group.plate_count == 1 ? _L("%d plate") : _L("%d plates"),
                                        group.plate_count);
    if (group.queue_seconds > 0.f)
        summary += "   " + format_hours(group.queue_seconds);
    const wxSize summary_extent = dc.GetTextExtent(summary);
    dc.DrawText(summary, width - summary_extent.GetWidth() - FromDIP(6),
                y + (height - summary_extent.GetHeight()) / 2);

    const int right = width - summary_extent.GetWidth() - FromDIP(12);

    dc.SetFont(Label::Body_9);
    const int detail_w = group.detail.empty() ? 0 : dc.GetTextExtent(from_u8(group.detail)).GetWidth() + FromDIP(8);

    dc.SetFont(Label::Head_11);
    dc.SetTextForeground(board_fg(dark));
    const wxString caption = wxControl::Ellipsize(from_u8(group.caption), dc, wxELLIPSIZE_END,
                                                  std::max(FromDIP(30), right - detail_w - x));
    dc.DrawText(caption, x, y + (height - dc.GetCharHeight()) / 2);

    if (!group.detail.empty()) {
        const int caption_w = dc.GetTextExtent(caption).GetWidth();
        dc.SetFont(Label::Body_9);
        dc.SetTextForeground(board_dim(dark));
        dc.DrawText(from_u8(group.detail), x + caption_w + FromDIP(8),
                    y + (height - dc.GetCharHeight()) / 2);
    }

    //Queue bar under a machine header, measured against the project's longest queue, so
    //an idle machine reads short at a glance. Drawn only when estimates exist — this is
    //what the separate Capacity tab used to show, and estimates are the only thing that
    //ever distinguished it from Machine.
    if (m_grouping == PlateBoardGrouping::ByMachine || m_grouping == PlateBoardGrouping::ByCapacity) {
        const float longest = m_model.rollup().longest_queue_seconds;
        if (longest > 0.f) {
            const int bar_h = FromDIP(3);
            const int bar_w = (int) (width * std::min(1.f, group.queue_seconds / longest));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(board_accent(dark)));
            dc.DrawRectangle(0, y + height - bar_h - 1, bar_w, bar_h);
        }
    }
}

void PlateBoard::draw_swatches(wxDC &dc, bool dark, const PlateBoardRow &row, int x, int y, int box, int max_width)
{
    const int gap    = FromDIP(3);
    const int budget = std::max(0, max_width);
    int       drawn  = 0;

    for (size_t i = 0; i < row.filament_slots.size(); ++i) {
        //room for this swatch and for the "+N" that has to follow if there are more
        if ((drawn + 1) * (box + gap) + FromDIP(18) > budget || drawn >= 4)
            break;

        wxColour colour;
        const bool ok = i < row.filament_colours.size() && !row.filament_colours[i].empty() &&
                        colour.Set(from_u8(row.filament_colours[i]));
        //an unparsable or absent colour draws hollow. Substituting a plausible one would
        //put a filament on screen that the library does not contain.
        //Circles: a row of tiny squares reads as broken image chips; dots read as
        //materials.
        dc.SetBrush(ok ? wxBrush(colour) : *wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(board_line(dark)));
        dc.DrawEllipse(x + drawn * (box + gap), y, box, box);
        ++drawn;
    }

    if (drawn < (int) row.filament_slots.size()) {
        dc.SetFont(Label::Body_9);
        dc.SetTextForeground(board_dim(dark));
        dc.DrawText(wxString::Format("+%d", (int) row.filament_slots.size() - drawn),
                    x + drawn * (box + gap), y - FromDIP(2));
    }
}

void PlateBoard::draw_state_icon(wxDC &dc, const PlateBoardRow &row, int x, int y)
{
    if (!m_icons_ok)
        return;

    //The state column carries only what the file knows: no reachability, no online dot.
    //Orca's device layer is singular, so at most one row could ever be honest about a
    //machine being up, and a status light that is right one row in eight is worse than
    //none. Order is by what needs attention first.
    const ScalableBitmap *icon = nullptr;
    if (row.parts_outside || row.preset_missing || row.unresolved)
        icon = &m_icon_problem;
    else if (row.stale)
        icon = &m_icon_stale;
    else if (row.sliced)
        icon = &m_icon_sliced;
    //Last, and after sliced on purpose: the reason is written when a project is loaded and
    //nothing here can promise it is cleared when the plate is sliced again. A plate with a
    //current result says so, whatever it arrived as.
    else if (!row.dropped_reason.empty())
        icon = &m_icon_dropped;

    if (icon != nullptr && icon->bmp().IsOk())
        dc.DrawBitmap(icon->bmp(), x, y, true);
}

//The bed, in plan, inside a square cell. Scaled against the largest bed anywhere in the
//project rather than against a fixed millimetre constant: a constant has to be chosen for the
//largest machine anybody might own, which spends most of the range on beds nobody here has and
//leaves a 220 mm bed and a 256 mm bed a couple of pixels apart. Scaled to the project, the
//largest machine fills the cell and every other bed is exactly its true fraction of it.
//
//This is also what the glyph animation was always for. It has been interpolating bed
//dimensions that no draw call read since the row became pictures.
//
//The drawing itself is draw_bed_plan_in, shared with the printer picker so the two surfaces
//cannot drift into two different pictures of the same bed. They already had.
void PlateBoard::draw_bed_plan(wxDC &dc, const wxRect &cell, double bed_w, double bed_d, bool dark) const
{
    draw_bed_plan_in(dc, this, cell, bed_w, bed_d, m_model.glyph_reference_mm(), board_dim(dark), 5);
}

//One plate at 34 px. See PLATE_BOARD_TILE_ABOVE for why a large machine group draws these.
void PlateBoard::draw_plate_tile(wxDC &dc, const wxRect &cell, int row_index, bool dark,
                                 const std::vector<int> &dragged) const
{
    const std::vector<PlateBoardRow> &rows = m_model.rows();
    if (row_index < 0 || row_index >= (int) rows.size())
        return;
    const PlateBoardRow &row = rows[(size_t) row_index];

    const bool current = row.plate_index == m_current_plate;
    const bool scoped  = std::find(m_scoped_plates.begin(), m_scoped_plates.end(), row.plate_index) !=
                        m_scoped_plates.end();
    const bool hovered = m_hover.kind == HitKind::Row && m_hover.index == row_index;
    //Needs a person before it can print, for one of three reasons.
    const bool problem = row.preset_missing || row.unresolved || row.parts_outside;

    //THE GROUND IS WHAT THE ROW SAYS WITH A FULL-WIDTH FILL - current, in the scope set,
    //under the pointer, or none of the three - so a tiled group and a row group do not have
    //to be learned twice. Hover and drag get the same treatment they get on a row for the
    //same reason: a gesture that answers in one part of the board and not in another is a
    //control that works sometimes.
    const wxColour fill = current   ? board_sel(dark) :
                          scoped    ? board_scope(dark) :
                          hovered   ? board_hover(dark) :
                                      board_tile(dark);
    const wxColour edge = problem ? board_err(dark) : current ? board_accent(dark) : board_tile_line(dark);
    //One error colour, two edges. Dashed is "this does not compose", solid is "this printer
    //is not installed": the same severity and two different repairs, and at this size a
    //second red would read as the same red rather than as a second fact.
    const wxPenStyle edge_style = row.unresolved && !row.preset_missing ? wxPENSTYLE_SHORT_DASH
                                                                       : wxPENSTYLE_SOLID;

    dc.SetBrush(wxBrush(fill));
    dc.SetPen(wxPen(edge, current ? FromDIP(2) : 1, edge_style));
    dc.DrawRoundedRectangle(cell, FromDIP(3));

    //the bed in proportion, so two machines are still visibly two machines at 34 px
    draw_bed_plan(dc, cell, row.bed_w, row.bed_d, dark);

    dc.SetFont(Label::Body_9);
    dc.SetTextForeground(problem ? board_err(dark) : board_dim(dark));
    const wxString number = wxString::Format("%d", row.plate_index + 1);
    const wxSize   extent = dc.GetTextExtent(number);
    dc.DrawText(number, cell.x + (cell.GetWidth() - extent.GetWidth()) / 2,
                cell.y + (cell.GetHeight() - extent.GetHeight()) / 2);

    //THE CORNER MARK IS THE SLICE STATE, and it is a mark rather than a fill because the
    //fills that could carry it - #C0D4E2, #EBF9F0, #F4F6F6 - are one pale grey-green at this
    //size, and board_scope already means "in the scope set" on this same tile. Three
    //saturated colours in one small shape are three facts; three pale grounds are one.
    wxColour mark;
    bool     has_mark = true;
    if (row.sliced)
        mark = board_accent(dark); //there is a result to send
    else if (row.stale)
        mark = board_warn(dark);   //there is a result and it no longer matches the plate
    else if (!row.dropped_reason.empty())
        mark = board_dim(dark);    //there was a result and it was dropped
    else
        has_mark = false;

    if (has_mark) {
        const int size   = FromDIP(7);
        wxPoint   pts[3] = {wxPoint(cell.GetRight() - size, cell.y + 1),
                            wxPoint(cell.GetRight() - 1, cell.y + 1),
                            wxPoint(cell.GetRight() - 1, cell.y + size)};
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(mark));
        dc.DrawPolygon(3, pts);
    }

    //The drag set is on screen before the drop, which is the one condition this design puts
    //on a bulk write. A row draws this as a dashed outline; a tile owes the same guarantee,
    //or a drag begun in a tiled group carries plates nothing on screen names.
    if (!dragged.empty() &&
        std::find(dragged.begin(), dragged.end(), row.plate_index) != dragged.end()) {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(board_accent(dark), 1, wxPENSTYLE_SHORT_DASH));
        dc.DrawRoundedRectangle(cell.x - 1, cell.y - 1, cell.GetWidth() + 2, cell.GetHeight() + 2, FromDIP(3));
    }
}

void PlateBoard::draw_tile_band(wxDC &                 dc,
                                bool                   dark,
                                int                    width,
                                int                    y,
                                const Item &           item,
                                const PlateBoardGroup &group,
                                const std::vector<int> &dragged) const
{
    int tile = 0, gap = 0, inset = 0;
    tile_metrics(tile, gap, inset);

    for (int c = 0; c < item.tile_count; ++c) {
        const int index = item.tile_first + c;
        if (index < 0 || index >= (int) group.rows.size())
            break;
        const wxRect cell(inset + c * (tile + gap), y, tile, tile);
        //The band was laid out against a width the control may since have lost. Drawing past
        //the edge is worse than stopping - but stopping is only honest because wxEVT_SIZE
        //rebuilds the bands, so this clamp covers one frame rather than hiding a plate.
        if (cell.x + tile > width)
            break;
        draw_plate_tile(dc, cell, group.rows[(size_t) index], dark, dragged);
    }
}

void PlateBoard::draw_scroll_thumb(wxDC &dc, bool dark, int width, int top, int visible)
{
    //Nothing to say when everything is on screen. The board is deliberately shorter than its
    //content - the filament and process controls below it are the actual slicer and must keep
    //their place - so the rows that are off the bottom are the normal case, and a list that
    //scrolls with nothing saying so is a list whose remaining plates do not exist as far as
    //the user is concerned.
    if (visible <= 0 || m_content_height <= visible)
        return;

    const int track      = FromDIP(3);
    const int x          = width - track;
    const int thumb      = std::max(FromDIP(18), (int) ((double) visible * visible / m_content_height));
    const int span       = std::max(0, visible - thumb);
    const int max_scroll = std::max(1, m_content_height - visible);
    const int y          = top + (int) std::lround((double) m_scroll_px / max_scroll * span);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(board_line(dark)));
    dc.DrawRoundedRectangle(x, top, track, visible, track / 2.);
    dc.SetBrush(wxBrush(board_dim(dark)));
    dc.DrawRoundedRectangle(x, y, track, thumb, track / 2.);
}

void PlateBoard::begin_rename(int plate_index, const wxRect &rect)
{
    if (m_plater == nullptr || !m_plater->is_initialized())
        return;
    commit_rename(true);

    PartPlate *plate = m_plater->get_partplate_list().get_plate(plate_index);
    if (plate == nullptr)
        return;

    if (m_rename_edit == nullptr) {
        m_rename_edit = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                       wxTE_PROCESS_ENTER | wxTE_CENTER | wxBORDER_SIMPLE);
        m_rename_edit->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { commit_rename(true); });
        m_rename_edit->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &evt) {
            evt.Skip();
            commit_rename(true);
        });
        m_rename_edit->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &evt) {
            if (evt.GetKeyCode() == WXK_ESCAPE)
                commit_rename(false);
            else
                evt.Skip();
        });
    }

    m_rename_plate = plate_index;
    m_rename_edit->SetSize(rect);
    m_rename_edit->ChangeValue(from_u8(plate->get_plate_name()));
    m_rename_edit->Show();
    m_rename_edit->SetFocus();
    m_rename_edit->SelectAll();
}

void PlateBoard::commit_rename(bool apply)
{
    if (m_rename_edit == nullptr || m_rename_plate < 0)
        return;
    const int plate_index = m_rename_plate;
    //cleared FIRST: hiding the editor fires its kill-focus, which lands back here and
    //must find nothing left to commit
    m_rename_plate = -1;
    const std::string name = into_u8(m_rename_edit->GetValue());
    m_rename_edit->Hide();

    if (apply && m_plater != nullptr && m_plater->is_initialized())
        m_plater->rename_plate(plate_index, name);
}

const wxBitmap *PlateBoard::plate_thumb_bitmap(int plate_index, int px)
{
    if (m_plater == nullptr || !m_plater->is_initialized())
        return nullptr;
    PartPlate *plate = m_plater->get_partplate_list().get_plate(plate_index);
    if (plate == nullptr)
        return nullptr;

    //Self-healing: anything that moves an instance resets the plate's thumbnail
    //(notify_instance_update), and in the 3D editor nothing re-renders it — the strip
    //that used to is Preview-only. So an invalid render is re-requested through the same
    //canvas call the hover preview uses, at most one plate per paint.
    //
    //It is REQUESTED here and performed after the paint. Doing it here put a 512x512
    //offscreen render and a readback inside the paint handler, so every click that
    //changed a plate - assigning a printer changes its bed, which invalidates its
    //thumbnail - waited 44 ms for a picture before any of the row appeared. A paint must
    //not block on a render it can do immediately afterwards.
    if (!plate->thumbnail_data.is_valid()) {
        if (m_thumb_heal_wanted < 0 && !m_thumb_heal_blocked)
            m_thumb_heal_wanted = plate_index;

        //Show the last good picture of this plate rather than a hole. It is one paint out
        //of date, which is what a progressive refresh looks like; a cell that empties and
        //then refills reads as a glitch, and the row's other facts are already correct.
        if (const auto it = m_thumb_cache.find(plate_index);
            it != m_thumb_cache.end() && it->second.bmp.IsOk())
            return &it->second.bmp;
        return nullptr;
    }

    const ThumbnailData &data = plate->thumbnail_data;
    ThumbCacheEntry &    slot = m_thumb_cache[plate_index];
    if (slot.pixels == (const void *) data.pixels.data() && slot.size == data.pixels.size() && slot.bmp.IsOk())
        return &slot.bmp;

    wxImage image = thumbnail_to_image(data);
    if (!image.IsOk())
        return nullptr;
    //aspect-fit into a square cell; the render already sits on transparent padding
    const int side = std::max(image.GetWidth(), image.GetHeight());
    image.Resize(wxSize(side, side), wxPoint((side - image.GetWidth()) / 2, (side - image.GetHeight()) / 2));
    image.Rescale(px, px, wxIMAGE_QUALITY_HIGH);
    slot.pixels = (const void *) data.pixels.data();
    slot.size   = data.pixels.size();
    slot.bmp    = wxBitmap(image);
    return slot.bmp.IsOk() ? &slot.bmp : nullptr;
}

const wxBitmap *PlateBoard::printer_cover_bitmap(const std::string &model, int px)
{
    if (model.empty())
        return nullptr;
    auto it = m_cover_cache.find(model);
    if (it != m_cover_cache.end())
        return it->second.IsOk() ? &it->second : nullptr;

    //the wizard's cover art: resources/profiles/<vendor>/<model>_cover.png. A miss is
    //cached too, so an uncovered model costs one directory probe per session, not one
    //per paint.
    wxBitmap &slot = m_cover_cache[model];
    if (wxGetApp().preset_bundle != nullptr) {
        for (const auto &vendor : wxGetApp().preset_bundle->vendors) {
            for (const auto &vendor_model : vendor.second.models) {
                if (vendor_model.name != model)
                    continue;
                const std::string path = Slic3r::resources_dir() + "/profiles/" + vendor.second.id + "/" + model + "_cover.png";
                if (wxFileExists(from_u8(path))) {
                    wxImage image(from_u8(path), wxBITMAP_TYPE_PNG);
                    if (image.IsOk()) {
                        const int side = std::max(image.GetWidth(), image.GetHeight());
                        image.Resize(wxSize(side, side), wxPoint((side - image.GetWidth()) / 2, (side - image.GetHeight()) / 2));
                        image.Rescale(px, px, wxIMAGE_QUALITY_HIGH);
                        slot = wxBitmap(image);
                    }
                }
                break;
            }
        }
    }
    return slot.IsOk() ? &slot : nullptr;
}

void PlateBoard::draw_row(wxDC &                 dc,
                          bool                   dark,
                          int                    width,
                          int                    y,
                          int                    height,
                          int                    row_index,
                          const PlateBoardGroup *group)
{
    if (row_index < 0 || row_index >= (int) m_model.rows().size())
        return;
    const PlateBoardRow &row = m_model.rows()[(size_t) row_index];

    const bool selected = row.plate_index == m_current_plate;
    const bool scoped   = std::find(m_scoped_plates.begin(), m_scoped_plates.end(), row.plate_index) !=
                        m_scoped_plates.end();
    const bool hovered = m_hover.kind == HitKind::Row && m_hover.index >= 0 &&
                         m_hover.index < (int) m_model.rows().size() &&
                         m_model.rows()[(size_t) m_hover.index].plate_index == row.plate_index;

    if (selected) {
        dc.SetBrush(wxBrush(board_sel(dark)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, y, width, height);
    } else if (scoped) {
        //in the scope set but not the current plate: the inspector is describing it, the
        //3D scene is not showing it, and the row says both
        dc.SetBrush(wxBrush(board_scope(dark)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, y, width, height);
    } else if (hovered) {
        dc.SetBrush(wxBrush(board_hover(dark)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, y, width, height);
    }

    if (selected) {
        //a 2 px spine on the current row, so the selection survives being read at a glance
        //on a theme where the fill is subtle
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(board_accent(dark)));
        dc.DrawRectangle(0, y, FromDIP(2), height);
    }

    //THE ROW IS THE FORK'S CORE SENTENCE, drawn as pictures: [this plate] → [that
    //machine], each image over its caption. Under a header that already names the
    //machine the sentence loses its second half rather than repeating it.
    const bool names_machine_above = group != nullptr && group->names_machine;

    const int icon_x = width - FromDIP(18);
    const int right  = icon_x - FromDIP(6);
    const int img    = FromDIP(40);
    const int col_w  = FromDIP(72);
    const int img_y  = y + FromDIP(3);
    const int cap_y  = y + FromDIP(46);

    // ---- index gutter ------------------------------------------------------
    dc.SetFont(Label::Body_10);
    dc.SetTextForeground(selected ? board_fg(dark) : board_dim(dark));
    const wxString index_text   = wxString::Format("%d", row.plate_index + 1);
    const wxSize   index_extent = dc.GetTextExtent(index_text);
    dc.DrawText(index_text, FromDIP(20) - index_extent.GetWidth(), y + (height - index_extent.GetHeight()) / 2);

    // ---- plate column: render over name ------------------------------------
    const int plate_col_x = FromDIP(24);
    const int plate_img_x = plate_col_x + (col_w - img) / 2;
    if (const wxBitmap *thumb = plate_thumb_bitmap(row.plate_index, img)) {
        dc.DrawBitmap(*thumb, plate_img_x, img_y, true);
    } else {
        //No render yet. Rather than a blank cell with a number in it, draw the BED this plate
        //prints on, in plan, scaled against the largest bed in the project - which is the same
        //proportion the glyph reference already exists for. A plate with no thumbnail then
        //still says the thing that matters when you are assigning plates: how big is this
        //machine, and is it bigger or smaller than the others. That is more information than
        //the empty tile carried, in the same pixels.
        dc.SetBrush(wxBrush(board_tile(dark)));
        dc.SetPen(wxPen(board_tile_line(dark)));
        dc.DrawRoundedRectangle(plate_img_x, img_y, img, img, FromDIP(4));
        draw_bed_plan(dc, wxRect(plate_img_x, img_y, img, img), row.bed_w, row.bed_d, dark);
        dc.SetFont(Label::Body_10);
        dc.SetTextForeground(board_dim(dark));
        const wxSize ne = dc.GetTextExtent(index_text);
        dc.DrawText(index_text, plate_img_x + (img - ne.GetWidth()) / 2, img_y + (img - ne.GetHeight()) / 2);
    }

    wxString plate_label = from_u8(row.plate_name);
    if (plate_label.IsEmpty())
        plate_label = wxString::Format(_L("Plate %d"), row.plate_index + 1);
    dc.SetFont(Label::Body_10);
    dc.SetTextForeground(board_fg(dark));
    {
        const wxString shown = wxControl::Ellipsize(plate_label, dc, wxELLIPSIZE_END, col_w);
        const wxSize   ext   = dc.GetTextExtent(shown);
        dc.DrawText(shown, plate_col_x + (col_w - ext.GetWidth()) / 2, cap_y);
    }

    int content_right = plate_col_x + col_w;

    // ---- arrow and machine column ------------------------------------------
    if (!names_machine_above) {
        const int arrow_x0 = plate_col_x + col_w + FromDIP(4);
        const int arrow_x1 = arrow_x0 + FromDIP(18);
        const int arrow_y  = img_y + img / 2;
        //One accent, one error colour. The dim third state said "changeable default,
        //not a choice", and there are no defaults left for it to describe.
        const wxColour arrow_colour = row.preset_missing ? board_err(dark) : board_accent(dark);
        dc.SetPen(wxPen(arrow_colour, FromDIP(2)));
        dc.DrawLine(arrow_x0, arrow_y, arrow_x1, arrow_y);
        dc.DrawLine(arrow_x1 - FromDIP(5), arrow_y - FromDIP(4), arrow_x1, arrow_y);
        dc.DrawLine(arrow_x1 - FromDIP(5), arrow_y + FromDIP(4), arrow_x1, arrow_y);

        const int machine_col_x = arrow_x1 + FromDIP(4);
        const int machine_img_x = machine_col_x + (col_w - img) / 2;
        if (const wxBitmap *cover = printer_cover_bitmap(row.printer_model, img)) {
            dc.DrawBitmap(*cover, machine_img_x, img_y, true);
        } else {
            //Same rule as the plate cell: no cover art for this machine, so say its bed size
            //instead of painting a rectangle that means "we have no picture".
            dc.SetBrush(wxBrush(board_tile(dark)));
            dc.SetPen(wxPen(board_tile_line(dark)));
            dc.DrawRoundedRectangle(machine_img_x, img_y, img, img, FromDIP(4));
            draw_bed_plan(dc, wxRect(machine_img_x, img_y, img, img), row.bed_w, row.bed_d, dark);
        }

        //the caption is the MODEL when the preset declares one — short and human — and
        //the exact preset name only when nothing better exists. A project-embedded
        //preset with an empty base name would otherwise caption a machine "(file.3mf)".
        wxString machine_caption = from_u8(!row.printer_model.empty() ? row.printer_model : row.printer_name);
        if (row.preset_missing)
            machine_caption += _L(" (not installed)");
        dc.SetFont(Label::Body_10);
        dc.SetTextForeground(row.preset_missing ? board_err(dark) : board_fg(dark));
        const wxString shown = wxControl::Ellipsize(machine_caption, dc, wxELLIPSIZE_END, col_w + FromDIP(16));
        const wxSize   ext   = dc.GetTextExtent(shown);
        int            mx    = machine_col_x + (col_w - ext.GetWidth()) / 2;
        mx                   = std::max(mx, machine_col_x - FromDIP(8));
        dc.DrawText(shown, mx, cap_y);
        if (row.preset_missing) {
            dc.SetPen(wxPen(board_err(dark)));
            dc.DrawLine(mx, cap_y + ext.GetHeight() / 2, mx + ext.GetWidth(), cap_y + ext.GetHeight() / 2);
        }

        content_right = machine_col_x + col_w;
    }

    // ---- right block: state, time, parts, materials ------------------------
    draw_state_icon(dc, row, icon_x, y + FromDIP(4));

    dc.SetFont(Label::Body_10);
    dc.SetTextForeground(board_soft(dark));
    if (row.has_time) {
        //shown only when there is a number: a column of em-dashes reads as a broken
        //table, and "no estimate" is the default state of every plate
        const wxString hours = format_hours(row.print_time_seconds);
        const wxSize   ext   = dc.GetTextExtent(hours);
        dc.DrawText(hours, right - ext.GetWidth(), y + FromDIP(22));
    } else {
        //WHERE THE HOURS WOULD BE is where the eye already looks for this row's standing,
        //and a plate that will not compose has no hours to put there - the composition IS
        //what an estimate comes from, so the two can never collide.
        //
        //preset_missing is said here only when there is no machine caption to carry it: the
        //caption strikes the name through and appends "(not installed)", and a row under a
        //header that names the machine has no caption at all, so without this the fact
        //vanished for exactly the rows that are hardest to read. Unresolved is a different
        //problem with a different repair - the printer IS installed, something else in the
        //context does not resolve - so it gets its own word rather than sharing that one.
        wxString standing;
        if (row.preset_missing && names_machine_above)
            standing = _L("Not installed");
        else if (row.unresolved && !row.preset_missing)
            standing = _L("Unresolved");
        if (!standing.IsEmpty()) {
            dc.SetTextForeground(board_err(dark));
            const wxString shown = wxControl::Ellipsize(standing, dc, wxELLIPSIZE_END,
                                                        std::max(FromDIP(30), right - content_right));
            const wxSize   ext   = dc.GetTextExtent(shown);
            dc.DrawText(shown, right - ext.GetWidth(), y + FromDIP(22));
        }
    }

    dc.SetFont(Label::Body_9);
    dc.SetTextForeground(board_dim(dark));
    const wxString parts = row.part_count == 1 ? _L("1 part") : wxString::Format(_L("%d parts"), row.part_count);
    const wxSize   parts_extent = dc.GetTextExtent(parts);
    dc.DrawText(parts, right - parts_extent.GetWidth(), y + FromDIP(38));

    //material dots between the columns and the right block, vertically centred on the
    //images
    const int box       = FromDIP(8);
    const int swatch_x  = content_right + FromDIP(10);
    const int swatch_max = right - FromDIP(64) - swatch_x;
    if (swatch_max > box)
        draw_swatches(dc, dark, row, swatch_x, img_y + (img - box) / 2, box, swatch_max);
}

void PlateBoard::on_paint(wxPaintEvent &evt)
{
    PETKOS_PERF_SCOPE(Perf::Probe::BoardPaint);
    m_thumb_heal_wanted = -1;

    wxAutoBufferedPaintDC dc(this);
    const bool            dark   = wxGetApp().dark_mode();
    const wxSize          client = GetClientSize();
    const int             width  = client.GetWidth();

    dc.SetBrush(wxBrush(board_bg(dark)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(GetClientRect());

    const std::vector<PlateBoardRow>   &rows   = m_model.rows();
    const std::vector<PlateBoardGroup> &groups = m_model.groups();
    if (rows.empty())
        return;

    if (rollup_height() > 0)
        draw_rollup(dc, dark, width);
    if (grouping_height() > 0)
        draw_grouping(dc, dark, width, rollup_height());

    const int top     = view_top();
    const int visible = client.GetHeight() - top;
    if (visible <= 0)
        return;

    //Only what is on screen is drawn. The list is bounded at MAX_PLATE_COUNT, so the worst
    //case is finite either way, but the arithmetic is the same either way too and this
    //keeps a thirty-six plate project's repaint the same cost as a three plate one.
    dc.SetClippingRegion(0, top, width, visible);

    //The plates the drag is carrying, resolved once rather than per row: it is the scope
    //set when the grabbed row is part of one, and that set is already on screen.
    const std::vector<int> dragged = m_dragging ? drag_targets() : std::vector<int>();

    for (const Item &item : m_items) {
        const int y = top + item.y - m_scroll_px;
        if (y + item.height <= top)
            continue;
        if (y >= top + visible)
            break;

        if (item.header) {
            if (item.group >= 0 && item.group < (int) groups.size()) {
                draw_group_header(dc, dark, width, y, item.height, groups[(size_t) item.group],
                                  m_collapsed.count(groups[(size_t) item.group].key) > 0);
                //the header the drop would land on says so, so the machine being assigned is
                //named on screen before the button is released
                if (m_dragging && item.group == m_drop_group) {
                    dc.SetBrush(*wxTRANSPARENT_BRUSH);
                    dc.SetPen(wxPen(board_accent(dark), FromDIP(2)));
                    dc.DrawRectangle(1, y + 1, width - 2, item.height - 2);
                }
            }
        } else if (item.tile_count > 0) {
            //A band draws its own plates and its own drag outlines; there is no row to draw.
            if (item.group >= 0 && item.group < (int) groups.size())
                draw_tile_band(dc, dark, width, y, item, groups[(size_t) item.group], dragged);
        } else if (item.row >= 0 && item.row < (int) rows.size()) {
            const PlateBoardGroup *group = item.group >= 0 && item.group < (int) groups.size()
                                               ? &groups[(size_t) item.group]
                                               : nullptr;
            draw_row(dc, dark, width, y, item.height, item.row, group);

            if (!dragged.empty() &&
                std::find(dragged.begin(), dragged.end(), rows[(size_t) item.row].plate_index) != dragged.end()) {
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(board_accent(dark), 1, wxPENSTYLE_SHORT_DASH));
                dc.DrawRectangle(0, y, width, item.height);
            }
        }
    }

    //Sticky: the group a scrolled row belongs to stays named at the top of the region, or
    //a scrolled list is a list of rows with nothing saying what they are under.
    const int sticky = m_scroll_px > 0 ? sticky_group() : -1;
    if (sticky >= 0 && sticky < (int) groups.size()) {
        draw_group_header(dc, dark, width, top, m_header_height, groups[(size_t) sticky],
                          m_collapsed.count(groups[(size_t) sticky].key) > 0);
        if (m_dragging && sticky == m_drop_group) {
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(board_accent(dark), FromDIP(2)));
            dc.DrawRectangle(1, top + 1, width - 2, m_header_height - 2);
        }
    }

    //Inside the clip and last, so it rides over the rows rather than under them, and so it
    //cannot be painted over the rollup or the grouping control above the scroll region.
    draw_scroll_thumb(dc, dark, width, top, visible);

    dc.DestroyClippingRegion();

    //After the clip is dropped, not inside it: a captured drag can put the pointer over the
    //rollup or below the last row, and a pill clipped to the scroll region would vanish at
    //exactly the moment the user is farthest from a target and most needs telling.
    if (m_dragging)
        draw_drag_pill(dc, dark);

    //One healed thumbnail per paint, and the heal happens after this paint rather than
    //inside it. The repaint that follows a successful heal is what finds the next row
    //needing one, so a project heals a row at a time without any of them costing a click.
    if (m_thumb_heal_wanted >= 0) {
        const int heal_index = m_thumb_heal_wanted;
        m_thumb_heal_wanted  = -1;
        CallAfter([this, heal_index]() {
            if (m_plater == nullptr || !m_plater->is_initialized())
                return;
            PartPlate *plate = m_plater->get_partplate_list().get_plate(heal_index);
            if (plate == nullptr || plate->thumbnail_data.is_valid())
                return;
            GLCanvas3D *canvas = m_plater->get_view3D_canvas3D();
            if (canvas == nullptr)
                return;
            {
                //Perf: the offscreen GL render and readback, now outside any paint.
                PETKOS_PERF_SCOPE_AUX(Perf::Probe::BoardThumbHeal, (int32_t) heal_index);
                canvas->refresh_plate_thumbnail(heal_index);
            }
            //Only paint again when this actually produced pixels. A canvas that cannot
            //render right now would otherwise turn into a repaint spin.
            m_thumb_heal_blocked = !plate->thumbnail_data.is_valid();
            if (!m_thumb_heal_blocked)
                Refresh();
        });
    }

    //Perf: a board paint is a surface the user sees, so it closes an interaction just as a
    //canvas frame does. Surface 100 distinguishes it from a canvas type.
    Perf::note_painted(100);
}

void PlateBoard::draw_drag_pill(wxDC &dc, bool dark)
{
    //What the cursor is carrying, said in words. Without it a drag over a long list is a
    //highlight moving with no statement of what would land there.
    const std::vector<int> targets = drag_targets();
    if (targets.empty())
        return;

    const wxString text = targets.size() == 1
                              ? wxString::Format(_L("Plate %d"), m_drag_plate + 1)
                              : wxString::Format(_L("%d plates"), (int) targets.size());

    dc.SetFont(Label::Body_9);
    const wxSize extent = dc.GetTextExtent(text);
    const int    pad    = FromDIP(6);
    const int    w      = extent.GetWidth() + 2 * pad;
    const int    h      = extent.GetHeight() + FromDIP(4);
    const int    x      = std::max(0, std::min(m_drag_pos.x + FromDIP(10), GetClientSize().GetWidth() - w));
    //Clamped into the control, because a captured drag reports positions outside it and an
    //unclipped pill drawn at a negative y is simply not there.
    const int    y      = std::max(0, std::min(m_drag_pos.y - h / 2, GetClientSize().GetHeight() - h));

    //Dimmed while the pointer is not over a target, so "there is nowhere to drop this here"
    //is visible without a refusal and without a modal.
    const wxColour fill = m_drop_group >= 0 ? board_accent(dark) : board_dim(dark);
    dc.SetBrush(wxBrush(fill));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(x, y, w, h, FromDIP(3));
    dc.SetTextForeground(board_bg(dark));
    dc.DrawText(text, x + pad, y + FromDIP(2));
}

// ----------------------------------------------------------------------------
// PlateSwatchStrip
// ----------------------------------------------------------------------------

//What a plate shows for filament is which slots its instances reference and what is
//loaded in them. The plate owns that list - filament_preset_names is a plate-context
//field like any other - so the strip is also where it is changed: clicking a swatch
//picks the material for that slot on this plate.
class PlateSwatchStrip : public wxPanel
{
public:
    PlateSwatchStrip(wxWindow *parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, FromDIP(18)));
        Bind(wxEVT_PAINT, &PlateSwatchStrip::on_paint, this);
        Bind(wxEVT_LEFT_UP, &PlateSwatchStrip::on_click, this);
    }

    //slots are the 1-based slot numbers, in slot order. The colour is whatever this
    //plate holds for that slot, or an empty string when the plate references a slot it
    //has no material for, which is drawn hollow rather than guessed at.
    void set_slots(const std::vector<std::pair<int, std::string>> &slots)
    {
        if (m_slots == slots)
            return;
        m_slots = slots;
        Refresh();
    }

    //The swatch IS the control. A material row of its own would be a second place showing
    //the same thing, and the thing the user points at to change a filament is the colour
    //they are looking at. Passing an empty callback makes the strip read-only again, which
    //is what every scope other than one plate wants.
    void set_on_slot_clicked(std::function<void(int)> handler)
    {
        m_on_slot_clicked = std::move(handler);
        SetCursor(wxCursor(m_on_slot_clicked ? wxCURSOR_HAND : wxCURSOR_ARROW));
    }

private:
    //Where each swatch was drawn, so a click can name the slot it landed on rather than
    //recomputing the layout and eventually disagreeing with the paint.
    std::vector<std::pair<wxRect, int>> m_hit_boxes;
    std::function<void(int)>            m_on_slot_clicked;

    void on_click(wxMouseEvent &evt)
    {
        if (!m_on_slot_clicked)
            return;
        for (const std::pair<wxRect, int> &box : m_hit_boxes)
            if (box.first.Contains(evt.GetPosition())) {
                m_on_slot_clicked(box.second);
                return;
            }
    }

    void on_paint(wxPaintEvent &)
    {
        wxAutoBufferedPaintDC dc(this);
        const bool            dark = wxGetApp().dark_mode();

        dc.SetBrush(wxBrush(board_bg(dark)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(GetClientRect());

        const int box = FromDIP(11);
        int       x   = 0;
        const int y   = std::max(0, (GetClientSize().GetHeight() - box) / 2);

        m_hit_boxes.clear();
        dc.SetFont(Label::Body_9);
        for (const std::pair<int, std::string> &slot : m_slots) {
            if (x + box + FromDIP(18) > GetClientSize().GetWidth())
                break;
            //the number beside the swatch is part of the target: an 11 px square is not
            //a click area anyone should have to hit exactly
            m_hit_boxes.emplace_back(wxRect(x, 0, box + FromDIP(20), GetClientSize().GetHeight()), slot.first);

            //slot 0 is the add affordance: a hollow box wearing a plus, clickable only
            //when the strip is writable at all
            if (slot.first == 0) {
                if (!m_on_slot_clicked)
                    continue;
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(board_dim(dark)));
                dc.DrawRectangle(x, y, box, box);
                dc.SetTextForeground(board_dim(dark));
                const wxString plus = "+";
                const wxSize   ext  = dc.GetTextExtent(plus);
                dc.DrawText(plus, x + (box - ext.GetWidth()) / 2, y + (box - ext.GetHeight()) / 2);
                x += box + FromDIP(8);
                continue;
            }

            //an unparsable or absent colour draws hollow. Substituting a plausible one
            //would put a filament on screen that the library does not contain.
            wxColour colour;
            const bool ok = !slot.second.empty() && colour.Set(from_u8(slot.second));
            dc.SetBrush(ok ? wxBrush(colour) : *wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(board_line(dark)));
            dc.DrawRectangle(x, y, box, box);
            x += box + FromDIP(3);

            //the slot number rides beside the swatch: slots are per-printer, and the
            //number is the only part of this that the G-code agrees with
            dc.SetTextForeground(board_dim(dark));
            const wxString label = wxString::Format("%d", slot.first);
            dc.DrawText(label, x, y);
            x += dc.GetTextExtent(label).GetWidth() + FromDIP(8);
        }

        if (m_slots.empty()) {
            dc.SetTextForeground(board_dim(dark));
            dc.DrawText(_L("None"), 0, y);
        }
    }

    std::vector<std::pair<int, std::string>> m_slots;
};

// ----------------------------------------------------------------------------
// PlateInspector
// ----------------------------------------------------------------------------

namespace {

//Bed-type labels come from the same place the dialog's do, so the two surfaces cannot
//drift into two vocabularies for one fact. The inheritance row is the dialog's literal
//string, not a new one.
wxString bed_type_label(int bed_type)
{
    if (bed_type <= 0)
        return _L("Same as Global Plate Type");
    const ConfigOptionDef *def = print_config_def.get("curr_bed_type");
    if (def == nullptr || (size_t) bed_type > def->enum_labels.size())
        return wxString();
    return _L(def->enum_labels[(size_t) bed_type - 1]);
}

//Same rule for the filament map mode: the label is the one the option definition
//already carries.
wxString filament_map_mode_label(int mode)
{
    const ConfigOptionDef *def = print_config_def.get("filament_map_mode");
    if (def == nullptr || mode < 0 || (size_t) mode >= def->enum_labels.size())
        return wxString();
    return _L(def->enum_labels[(size_t) mode]);
}

} // namespace

PlateInspector::PlateInspector(wxWindow *parent, Plater *plater)
    : wxPanel(parent, wxID_ANY), m_plater(plater)
{
    const bool dark = wxGetApp().dark_mode();
    SetBackgroundColour(board_bg(dark));

    m_header = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(24)));
    m_header->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_header->Bind(wxEVT_PAINT, &PlateInspector::on_header_paint, this);
    m_header->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { set_expanded(!m_expanded); });

    m_body = new wxPanel(this, wxID_ANY);
    m_body->SetBackgroundColour(board_bg(dark));

    build_rows();

    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(m_header, 0, wxEXPAND);
    outer->Add(m_body, 0, wxEXPAND);
    SetSizer(outer);

    m_badge_text = _L("PROJECT");
}

void PlateInspector::build_rows()
{
    const bool dark = wxGetApp().dark_mode();

    auto make_label = [this, dark](const wxString &text) {
        wxStaticText *label = new wxStaticText(m_body, wxID_ANY, text);
        label->SetFont(Label::Body_9);
        label->SetForegroundColour(board_dim(dark));
        label->SetBackgroundColour(board_bg(dark));
        return label;
    };
    //Ellipsized and capped. A preset name is arbitrarily long and the sidebar is a fixed
    //narrow column: a value row that reports its full text as its minimum size widens the
    //whole sidebar, which is a layout bug that only shows up on the machines with the
    //longest names.
    auto make_value = [this, dark](const wxString &text) {
        wxStaticText *value = new wxStaticText(m_body, wxID_ANY, text, wxDefaultPosition, wxDefaultSize,
                                               wxST_ELLIPSIZE_END);
        value->SetFont(Label::Body_11);
        value->SetForegroundColour(board_fg(dark));
        value->SetBackgroundColour(board_bg(dark));
        value->SetMinSize(wxSize(FromDIP(60), -1));
        value->SetMaxSize(wxSize(FromDIP(210), -1));
        return value;
    };

    wxFlexGridSizer *grid = new wxFlexGridSizer(0, 2, FromDIP(SidebarProps::ElementSpacing()),
                                                FromDIP(SidebarProps::IconSpacing()));
    grid->AddGrowableCol(1, 1);
    grid->SetFlexibleDirection(wxHORIZONTAL);

    m_printer_label = make_label(_L("Printer"));
    m_printer_value = make_value(wxString());
    //the same affordance as the row chip, reaching the same picker and therefore the
    //same single write path
    m_printer_value->SetCursor(wxCursor(wxCURSOR_HAND));
    m_printer_value->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { open_picker(); });
    grid->Add(m_printer_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_printer_value, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    m_process_label = make_label(_L("Process"));
    m_process_value = make_value(wxString());
    m_process_value->SetCursor(wxCursor(wxCURSOR_HAND));
    m_process_value->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { on_process_click(); });
    grid->Add(m_process_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_process_value, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    m_nozzle_label = make_label(_L("Nozzle"));
    m_nozzle_value = make_value(wxString());
    //clicking the value switches the nozzle by picking the machine's sibling preset —
    //the setting a changed nozzle is LOOKED for, never one the app asks about
    m_nozzle_value->SetCursor(wxCursor(wxCURSOR_HAND));
    m_nozzle_value->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { on_nozzle_click(); });
    grid->Add(m_nozzle_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_nozzle_value, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    m_bed_label  = make_label(_L("Bed type"));
    m_bed_choice = new ComboBox(m_body, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                wxSize(FromDIP(150), -1), 0, nullptr, wxCB_READONLY);
    m_bed_choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &evt) {
        on_bed_type_selected();
        evt.Skip();
    });
    m_bed_value = make_value(wxString());
    //exactly one of the two is ever visible; reload() picks which. Hidden here so the
    //panel does not draw two bed-type rows in the frame before the first reload.
    m_bed_value->Hide();

    wxBoxSizer *bed_sizer = new wxBoxSizer(wxHORIZONTAL);
    bed_sizer->Add(m_bed_choice, 1, wxALIGN_CENTER_VERTICAL);
    bed_sizer->Add(m_bed_value, 1, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_bed_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(bed_sizer, 1, wxEXPAND);

    m_filament_label = make_label(_L("Filament"));
    m_filament_value = new PlateSwatchStrip(m_body);
    m_filament_value->set_on_slot_clicked([this](int slot) { on_filament_slot_click(slot); });
    grid->Add(m_filament_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_filament_value, 1, wxEXPAND);

    m_mapping_label = make_label(_L("Mapping"));
    m_mapping_value = make_value(wxString());
    grid->Add(m_mapping_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_mapping_value, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    m_device_label = make_label(_L("Prints on"));
    m_device_value = make_value(wxString());
    m_device_value->SetCursor(wxCursor(wxCURSOR_HAND));
    m_device_value->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) { on_device_click(); });
    grid->Add(m_device_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_device_value, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    //Print sequence, both filament sequences and spiral vase stay in the dialog. This is
    //the door to them, and it opens on the plate the badge names rather than on whichever
    //plate happens to be current.
    m_more_btn = new Button(m_body, _L("More plate settings"));
    m_more_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_more_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_more_plate_settings(); });

    m_body_sizer = new wxBoxSizer(wxVERTICAL);
    m_body_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(SidebarProps::ContentMarginV()));
    m_body_sizer->Add(m_more_btn, 0, wxLEFT | wxBOTTOM, FromDIP(SidebarProps::ContentMarginV()));
    m_body->SetSizer(m_body_sizer);
}

void PlateInspector::set_expanded(bool expanded)
{
    if (m_expanded == expanded)
        return;
    m_expanded = expanded;
    m_body->Show(m_expanded);
    m_header->Refresh();
    Layout();
    InvalidateBestSize();
    if (GetParent() != nullptr)
        GetParent()->Layout();
}

void PlateInspector::on_header_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(m_header);
    const bool            dark = wxGetApp().dark_mode();

    dc.SetBrush(wxBrush(board_bg(dark)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(m_header->GetClientRect());

    //The scope is STATED. Nothing here infers it from what was last clicked, because a
    //scope you have to deduce is a scope you will eventually deduce wrongly, and the
    //controls below this line write to whatever it says.
    dc.SetFont(Label::Body_9);
    const wxSize extent = dc.GetTextExtent(m_badge_text);
    const int    pad    = m_header->FromDIP(7);
    const int    height = m_header->FromDIP(16);
    const int    top    = std::max(0, (m_header->GetClientSize().GetHeight() - height) / 2);
    const int    left   = m_header->FromDIP(SidebarProps::ElementSpacing());

    dc.SetBrush(wxBrush(board_sel(dark)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(left, top, extent.GetWidth() + 2 * pad, height, m_header->FromDIP(3));
    dc.SetTextForeground(board_fg(dark));
    dc.DrawText(m_badge_text, left + pad, top + (height - extent.GetHeight()) / 2);

    //a hairline out to the right edge, so the badge reads as a section rule rather than as
    //a loose chip
    dc.SetPen(wxPen(board_line(dark)));
    const int rule_y = top + height / 2;
    dc.DrawLine(left + extent.GetWidth() + 2 * pad + m_header->FromDIP(8), rule_y,
                m_header->GetClientSize().GetWidth() - m_header->FromDIP(22), rule_y);

    draw_chevron(dc, board_dim(dark), m_header->GetClientSize().GetWidth() - m_header->FromDIP(16),
                 rule_y - m_header->FromDIP(4), m_header->FromDIP(8), m_expanded);
}

void PlateInspector::open_picker()
{
    if (m_plater == nullptr || !m_plater->is_initialized())
        return;
    if (m_plate_index == PLATE_BOARD_NO_PLATE)
        return;

    //the scope the board is rendering is the scope the picker may act on, so the two can
    //never disagree about what "the selected plates" means
    std::vector<int> scope;
    if (m_plater->sidebar().scoped_plates().size() > 1)
        scope = m_plater->sidebar().scoped_plates();

    const wxPoint anchor = m_printer_value->ClientToScreen(wxPoint(0, m_printer_value->GetSize().GetHeight()));
    show_printer_picker(this, m_plater, m_plate_index, scope, anchor, m_popup);
}

void PlateInspector::on_nozzle_click()
{
    if (m_plater == nullptr || !m_plater->is_initialized() || m_plate_index == PLATE_BOARD_NO_PLATE)
        return;
    PartPlate *plate = m_plater->get_partplate_list().get_plate(m_plate_index);
    if (plate == nullptr)
        return;

    PresetBundle *bundle  = wxGetApp().preset_bundle;
    const Preset *current = bundle->printers.find_preset(plate->get_printer_preset_name(), false);
    if (current == nullptr)
        return;
    const std::string model = current->config.opt_string("printer_model");
    if (model.empty())
        return;

    //the machine's variants, in the collection's order
    std::vector<const Preset *> siblings;
    for (const Preset &preset : bundle->printers) {
        if (preset.is_visible && !preset.is_default && preset.printer_technology() == ptFFF &&
            preset.config.opt_string("printer_model") == model)
            siblings.push_back(&preset);
    }
    if (siblings.size() < 2)
        return; //one nozzle is not a choice

    wxMenu menu;
    const int base_id = wxID_HIGHEST + 4300;
    for (size_t i = 0; i < siblings.size(); ++i) {
        const std::string variant = siblings[i]->config.opt_string("printer_variant");
        wxMenuItem *item = menu.AppendCheckItem((int) (base_id + i),
                                                variant.empty() ? from_u8(siblings[i]->name)
                                                                : wxString::Format(_L("%s nozzle"), from_u8(variant)));
        if (siblings[i]->name == current->name)
            item->Check(true);
    }
    Plater *plater = m_plater;
    const int plate_index = m_plate_index;
    menu.Bind(wxEVT_MENU, [plater, plate_index, siblings, base_id](wxCommandEvent &evt) {
        const size_t i = (size_t) (evt.GetId() - base_id);
        if (i < siblings.size())
            plater->set_plate_printer(plate_index, siblings[i]->name);
    });
    PopupMenu(&menu);
}

//The process this plate slices with.
//
//Only processes that will actually run on this plate's printer are offered. A menu that
//lists what cannot resolve is a menu that turns half its entries into an error message
//after the click. A stored name this installation does not have is offered back verbatim
//at the top, because opening the picker must never be the thing that discards it.
void PlateInspector::on_process_click()
{
    if (m_plater == nullptr || !m_plater->is_initialized() || m_plate_index == PLATE_BOARD_NO_PLATE)
        return;
    PartPlate *plate = m_plater->get_partplate_list().get_plate(m_plate_index);
    if (plate == nullptr)
        return;

    PresetBundle *bundle  = wxGetApp().preset_bundle;
    const Preset *printer = bundle->printers.find_preset(plate->get_printer_preset_name(), false);
    if (printer == nullptr) {
        //Nothing to filter against, so nothing honest to offer. Say which question cannot
        //be answered rather than showing an empty menu.
        m_plater->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
            into_u8(wxString::Format(_L("Plate %d is on \"%s\", which this installation does not have, so its processes cannot be listed."),
                                     m_plate_index + 1, from_u8(plate->get_printer_preset_name()))));
        return;
    }

    const PresetWithVendorProfile printer_profile = bundle->printers.get_preset_with_vendor_profile(*printer);
    const std::string             current         = plate->get_print_preset_name();

    std::vector<const Preset *> candidates;
    for (const Preset &preset : bundle->prints) {
        if (!preset.is_visible || preset.is_default)
            continue;
        const PresetWithVendorProfile profile = bundle->prints.get_preset_with_vendor_profile(preset);
        if (is_compatible_with_printer(profile, printer_profile, &bundle->project_config))
            candidates.push_back(&preset);
    }

    wxMenu                   menu;
    const int                base_id = wxID_HIGHEST + 4400;
    std::vector<std::string> names;

    if (!current.empty() && bundle->prints.find_preset(current, false) == nullptr) {
        names.push_back(current);
        menu.AppendCheckItem(base_id, wxString::Format(_L("Keep %s (not installed)"), from_u8(current)))->Check(true);
        menu.AppendSeparator();
    }
    for (const Preset *preset : candidates) {
        names.push_back(preset->name);
        wxMenuItem *item = menu.AppendCheckItem((int) (base_id + names.size() - 1), from_u8(preset->name));
        if (preset->name == current)
            item->Check(true);
    }
    //What this plate changed about whichever process it names, and the two things to do with
    //it. They are opposites - make the settings permanent and reusable, or drop them - and
    //both are the user's: a carried set is still values somebody chose, and either decision
    //has a physical consequence.
    //
    //ABOVE the "no process runs here" exit below, deliberately. That exit fires when this
    //installation has nothing compatible to offer, which is precisely the state a plate is in
    //after landing on a machine whose processes are not installed - carrying its overrides and
    //with nowhere to put them. Leaving these items after the return made the feature disappear
    //in the one case it was built for.
    const size_t overrides = plate->process_override_count();
    if (overrides > 0) {
        if (menu.GetMenuItemCount() > 0)
            menu.AppendSeparator();
        menu.Append(base_id + 9002,
                    wxString::Format(_L("Save the %d changed setting(s) as a preset for this machine..."),
                                     (int) overrides));
        menu.Append(base_id + 9001,
                    wxString::Format(_L("Clear the %d setting(s) this plate changed"), (int) overrides));
    }

    Plater *  plater      = m_plater;
    const int plate_index = m_plate_index;
    menu.Bind(wxEVT_MENU, [plater, plate_index, names, base_id](wxCommandEvent &evt) {
        if (evt.GetId() == base_id + 9001) {
            plater->clear_plate_process_overrides(plate_index);
            return;
        }
        if (evt.GetId() == base_id + 9002) {
            plater->save_plate_process_as_preset(plate_index);
            return;
        }
        const size_t i = (size_t) (evt.GetId() - base_id);
        if (i < names.size())
            plater->set_plate_process(plate_index, names[i]);
    });

    if (names.empty()) {
        //Said as a disabled line rather than as an empty menu, and it is no longer the whole
        //menu: the override items above are still reachable.
        menu.Prepend(base_id + 9000, wxString::Format(_L("No process in this installation runs on \"%s\""),
                                                      from_u8(printer->name)))->Enable(false);
        if (overrides > 0)
            menu.InsertSeparator(1);
    }
    PopupMenu(&menu);
}

//The material in one of this plate's slots.
//
//A slot is a MATERIAL, not a colour. A project routinely wants several at once - PETG for
//the support interface, PLA everywhere else - and this is the surface where that is
//decided, which is why the menu leads with the material type rather than with the preset
//name. The swatch is the control because the swatch is the thing showing the value.
//
//The same picker serves the sidebar's assigned-material rows. Replacing an existing slot
//asks which matching assignments to change, leaving unrelated materials alone.
void PlateInspector::on_filament_slot_click(int slot)
{
    show_plate_filament_menu(this, m_plater, m_plate_index, slot);
}

void show_plate_filament_menu(wxWindow *parent, Plater *plater, int plate_index, int slot)
{
    if (plater == nullptr || !plater->is_initialized() || plate_index == PLATE_BOARD_NO_PLATE)
        return;
    PartPlate *plate = plater->get_partplate_list().get_plate(plate_index);
    if (plate == nullptr || slot < 0)
        return;

    PlateSlicingContext context = plate->get_slicing_context();
    //slot 0 is the strip's trailing "+": a new slot is being added, so the write lands one
    //past the current list.
    const bool adding = slot == 0;
    if (adding)
        slot = int(context.filament_preset_names.size()) + 1;
    if (!adding && (size_t) slot > context.filament_preset_names.size())
        return;
    const std::string current = adding ? std::string() : context.filament_preset_names[(size_t) slot - 1];

    PresetBundle *bundle  = wxGetApp().preset_bundle;
    const Preset *printer = bundle->printers.find_preset(context.printer_preset_name, false);
    const Preset *process = bundle->prints.find_preset(context.print_preset_name, false);
    if (printer == nullptr || process == nullptr) {
        show_error(parent, _L("Choose an installed printer and process for this plate before assigning materials."), false);
        return;
    }

    const PresetWithVendorProfile printer_profile = bundle->printers.get_preset_with_vendor_profile(*printer);
    const PresetWithVendorProfile process_profile = bundle->prints.get_preset_with_vendor_profile(*process);

    //Compatible with BOTH the printer and the process, which is exactly the pair of checks
    //the composer makes after the click. Two surfaces asking one question is a problem only
    //when they can answer it differently, and these cannot: it is the same pair of calls.
    auto runs_here = [&](const Preset &preset) {
        const PresetWithVendorProfile profile = bundle->filaments.get_preset_with_vendor_profile(preset);
        return is_compatible_with_printer(profile, printer_profile, &bundle->project_config) &&
               is_compatible_with_print(profile, process_profile, printer_profile);
    };
    std::vector<const Preset *> candidates;
    for (const Preset &preset : bundle->filaments) {
        if (!preset.is_visible || preset.is_default)
            continue;
        if (runs_here(preset))
            candidates.push_back(&preset);
    }

    //a small filled square, so a spool row shows the colour it is
    auto colour_bitmap = [parent](const std::string &colour_str) -> wxBitmap {
        const int side = parent->FromDIP(14);
        wxBitmap  bmp(side, side);
        wxMemoryDC dc(bmp);
        wxColour   colour;
        const bool ok = !colour_str.empty() && colour.Set(from_u8(colour_str));
        dc.SetBrush(ok ? wxBrush(colour) : *wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(board_line(wxGetApp().dark_mode())));
        dc.DrawRectangle(0, 0, side, side);
        dc.SelectObject(wxNullBitmap);
        return bmp;
    };

    wxMenu    menu;
    const int base_id = wxID_HIGHEST + 4500;

    //One action list for every clickable row. The menu id is an index into it, so the
    //handler is one lookup with no id arithmetic to get wrong.
    struct SlotAction
    {
        enum Kind { Assign, ChangeColour, RemoveSlot, Keep } kind { Assign };
        std::string preset_name; //Assign: the preset to write (already translated)
        std::string colour;      //Assign: the colour that rides with it; empty keeps the slot's
        std::string colour_type { "1" };
        std::string multi_colour;
        int finish { int(FilamentFinish::ffStandard) };
    };
    std::vector<SlotAction> actions;

    menu.Append(base_id + 9000, adding ? wxString(_L("New slot")) : wxString::Format(_L("Slot %d"), slot))->Enable(false);
    menu.AppendSeparator();
    if (!current.empty() && bundle->filaments.find_preset(current, false) == nullptr) {
        actions.push_back({SlotAction::Keep, current, std::string()});
        menu.AppendCheckItem(base_id + (int) actions.size() - 1,
                             wxString::Format(_L("Keep %s (not installed)"), from_u8(current)))->Check(true);
        menu.AppendSeparator();
    }

    //THE SPOOL POOL FIRST. The sidebar's filament rows are what is physically available -
    //material plus colour - independent of any printer. Choosing one here is the moment it
    //meets THIS plate's machine, so it is translated: kept as-is when it runs on this
    //printer and process, re-expressed as this machine's preset of the same material when
    //it does not, refused by name when no such material exists here. The colour rides with
    //the material either way.
    {
        const auto *pool_colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        bool        header_done  = false;
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            const Preset *pool_preset = bundle->filaments.find_preset(bundle->filament_presets[i], false);
            if (pool_preset == nullptr || pool_preset->is_default)
                continue;
            std::string colour;
            if (pool_colours != nullptr && i < pool_colours->values.size())
                colour = pool_colours->values[i];
            if (colour.empty())
                if (const auto *defaults = pool_preset->config.option<ConfigOptionStrings>("default_filament_colour");
                    defaults != nullptr && !defaults->values.empty())
                    colour = defaults->values.front();

            std::string target = pool_preset->name;
            std::string note;
            if (!runs_here(*pool_preset)) {
                target = bundle->translate_filament_to_printer(pool_preset->name, printer_profile, process_profile);
                if (target.empty()) {
                    //Named, not hidden: a spool this machine cannot express is a fact the
                    //user can act on (change the printer, buy the material), and a menu
                    //that silently omits it looks like the spool vanished.
                    if (!header_done) {
                        menu.Append(base_id + 9001, _L("Spools"))->Enable(false);
                        header_done = true;
                    }
                    std::string type = pool_preset->config.opt_string("filament_type", 0u);
                    wxMenuItem *dead = menu.Append(base_id + 9200 + (int) i,
                        wxString::Format(_L("%s — no %s for this printer"), from_u8(pool_preset->name),
                                         from_u8(type.empty() ? std::string("material") : type)));
                    dead->SetBitmap(colour_bitmap(colour));
                    dead->Enable(false);
                    continue;
                }
                note = " → " + target;
            }
            if (!header_done) {
                menu.Append(base_id + 9001, _L("Spools"))->Enable(false);
                header_done = true;
            }
            actions.push_back({SlotAction::Assign, target, colour});
            SlotAction &action = actions.back();
            if (const auto *types = bundle->project_config.option<ConfigOptionStrings>("filament_colour_type");
                types != nullptr && i < types->values.size())
                action.colour_type = types->values[i];
            if (const auto *multi = bundle->project_config.option<ConfigOptionStrings>("filament_multi_colour");
                multi != nullptr && i < multi->values.size())
                action.multi_colour = multi->values[i];
            if (const auto *finishes = bundle->project_config.option<ConfigOptionEnumsGeneric>("filament_finish");
                finishes != nullptr && i < finishes->values.size())
                action.finish = finishes->values[i];
            wxMenuItem *item = menu.AppendCheckItem(base_id + (int) actions.size() - 1,
                                                    from_u8(pool_preset->name + note));
            item->SetBitmap(colour_bitmap(colour));
            if (!adding && target == current)
                item->Check(true);
        }
    }

    //The slot's own controls, before the long list.
    if (!adding) {
        menu.AppendSeparator();
        actions.push_back({SlotAction::ChangeColour, std::string(), std::string()});
        menu.Append(base_id + (int) actions.size() - 1, _L("Change colour…"));

        //Only the LAST slot can be removed, and only while nothing on the plate references
        //it: removing a middle slot renumbers every reference above it, which is a rename
        //this menu has no business performing silently.
        if (context.filament_preset_names.size() > 1 && (size_t) slot == context.filament_preset_names.size()) {
            std::vector<int> used = plate->get_extruders(true);
            if (std::find(used.begin(), used.end(), slot) == used.end()) {
                actions.push_back({SlotAction::RemoveSlot, std::string(), std::string()});
                menu.Append(base_id + (int) actions.size() - 1, _L("Remove this slot"));
            }
        }
    }

    //Everything installed that runs here, grouped by material type, so choosing "the PETG
    //one" is one glance rather than a scan of forty preset names that all start with the
    //same vendor.
    std::map<std::string, std::vector<const Preset *>> by_type;
    for (const Preset *preset : candidates) {
        std::string type;
        if (const ConfigOptionStrings *opt = preset->config.option<ConfigOptionStrings>("filament_type");
            opt != nullptr && !opt->values.empty())
            type = opt->values.front();
        by_type[type.empty() ? into_u8(_L("Unnamed material")) : type].push_back(preset);
    }
    int header_idx = 0;
    for (const std::pair<const std::string, std::vector<const Preset *>> &group : by_type) {
        menu.AppendSeparator();
        menu.Append(base_id + 9500 + header_idx++, from_u8(group.first))->Enable(false);
        for (const Preset *preset : group.second) {
            //Picked raw, the material's own declared colour comes with it when it has one;
            //an empty colour keeps whatever the slot already shows.
            std::string colour;
            if (const auto *def = preset->config.option<ConfigOptionStrings>("default_filament_colour");
                def != nullptr && !def->values.empty())
                colour = def->values.front();
            actions.push_back({SlotAction::Assign, preset->name, colour});
            wxMenuItem *item = menu.AppendCheckItem(base_id + (int) actions.size() - 1, from_u8(preset->name));
            if (!adding && preset->name == current)
                item->Check(true);
        }
    }
    if (actions.empty()) {
        menu.Append(base_id + 9002, _L("No filament in this installation runs on this plate"))->Enable(false);
        parent->PopupMenu(&menu);
        return;
    }

    const int slot_index  = slot - 1;
    wxWindow *self = parent;
    std::vector<std::string> names   = context.filament_preset_names;
    std::vector<std::string> colours = bundle->plate_filament_colours(context);
    colours.resize(names.size());
    PlateSlicingContext appearance = context;
    appearance.filament_colour_types.resize(names.size(), "1");
    appearance.filament_multi_colours.resize(names.size());
    appearance.filament_finishes.resize(names.size(), int(FilamentFinish::ffStandard));
    menu.Bind(wxEVT_MENU,
              [plater, plate_index, slot_index, adding, actions, names, colours, appearance, base_id, self](wxCommandEvent &evt) mutable {
        const size_t i = (size_t) (evt.GetId() - base_id);
        if (i >= actions.size())
            return;
        const SlotAction &action = actions[i];
        if (adding && action.kind == SlotAction::Assign) {
            names.emplace_back();
            colours.emplace_back();
            appearance.filament_colour_types.emplace_back("1");
            appearance.filament_multi_colours.emplace_back();
            appearance.filament_finishes.emplace_back(int(FilamentFinish::ffStandard));
        }
        if ((size_t) slot_index >= names.size())
            return;
        switch (action.kind) {
        case SlotAction::Keep:
            return;
        case SlotAction::Assign:
            names[(size_t) slot_index] = action.preset_name;
            if (!action.colour.empty())
                colours[(size_t) slot_index] = action.colour;
            appearance.filament_colour_types[(size_t) slot_index] = action.colour_type;
            appearance.filament_multi_colours[(size_t) slot_index] = action.multi_colour;
            appearance.filament_finishes[(size_t) slot_index] = action.finish;
            break;
        case SlotAction::ChangeColour: {
            wxColourData data;
            data.SetChooseFull(true);
            wxColour seed;
            if (!colours[(size_t) slot_index].empty())
                seed.Set(from_u8(colours[(size_t) slot_index]));
            if (seed.IsOk())
                data.SetColour(seed);
            wxColourDialog dialog(self, &data);
            if (dialog.ShowModal() != wxID_OK)
                return;
            colours[(size_t) slot_index] =
                dialog.GetColourData().GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
            appearance.filament_colour_types[(size_t) slot_index] = "1";
            appearance.filament_multi_colours[(size_t) slot_index] = colours[(size_t) slot_index];
            break;
        }
        case SlotAction::RemoveSlot:
            names.pop_back();
            colours.pop_back();
            appearance.filament_colour_types.pop_back();
            appearance.filament_multi_colours.pop_back();
            appearance.filament_finishes.pop_back();
            break;
        }
        if (!adding && action.kind == SlotAction::Assign) {
            appearance.filament_colours = colours;
            plater->choose_plate_material_replacement(plate_index, size_t(slot_index), action.preset_name,
                                                       false, std::move(appearance));
        } else {
            plater->set_plate_filaments(plate_index, std::move(names), std::move(colours), std::move(appearance));
        }
    });
    parent->PopupMenu(&menu);
}

//Which physical machine this plate is dispatched to.
//
//A separate property from the printer preset, and deliberately so: several machines can
//share one slicing preset, and one machine changes nozzle over its life. Clearing it is
//the first entry, because a plate that slices but has not been told where to print is a
//real and common state rather than an incomplete one.
void PlateInspector::on_device_click()
{
    if (m_plater == nullptr || !m_plater->is_initialized() || m_plate_index == PLATE_BOARD_NO_PLATE)
        return;
    PartPlate *plate = m_plater->get_partplate_list().get_plate(m_plate_index);
    if (plate == nullptr)
        return;

    const std::string current = plate->get_physical_printer_id();

    std::vector<std::pair<std::string, wxString>> devices; //id, label
    if (DeviceManager *dev = wxGetApp().getDeviceManager()) {
        for (const std::pair<const std::string, MachineObject *> &entry : dev->get_my_machine_list()) {
            if (entry.second == nullptr)
                continue;
            wxString label = from_u8(entry.second->get_dev_name());
            if (label.empty())
                label = from_u8(entry.first);
            devices.emplace_back(entry.first, label);
        }
    }
    //A stored device this account can no longer see is offered back rather than dropped:
    //the project may be opened on the machine that owns it.
    if (!current.empty() &&
        std::find_if(devices.begin(), devices.end(),
                     [&current](const std::pair<std::string, wxString> &d) { return d.first == current; }) == devices.end())
        devices.emplace_back(current, wxString::Format(_L("%s (not on this account)"), from_u8(current)));

    wxMenu      menu;
    const int   base_id = wxID_HIGHEST + 4600;
    wxMenuItem *none    = menu.AppendCheckItem(base_id, _L("Not assigned to a machine"));
    none->Check(current.empty());
    if (!devices.empty())
        menu.AppendSeparator();
    for (size_t i = 0; i < devices.size(); ++i) {
        wxMenuItem *item = menu.AppendCheckItem((int) (base_id + 1 + i), devices[i].second);
        if (devices[i].first == current)
            item->Check(true);
    }
    if (devices.empty())
        menu.Append(base_id + 9000, _L("No machines are bound to this account"))->Enable(false);

    Plater *  plater      = m_plater;
    const int plate_index = m_plate_index;
    menu.Bind(wxEVT_MENU, [plater, plate_index, devices, base_id](wxCommandEvent &evt) {
        const int i = evt.GetId() - base_id;
        if (i == 0)
            plater->set_plate_physical_printer(plate_index, std::string());
        else if (i >= 1 && (size_t) (i - 1) < devices.size())
            plater->set_plate_physical_printer(plate_index, devices[(size_t) (i - 1)].first);
    });
    PopupMenu(&menu);
}

void PlateInspector::on_more_plate_settings()
{
    if (m_plater == nullptr || !m_plater->is_initialized() || m_plate_index == PLATE_BOARD_NO_PLATE)
        return;

    //Posted rather than constructed here. Plater::open_platesettings_dialog already
    //builds the dialog on the event's plate index, syncs every one of its controls from
    //that plate, and owns the confirm handler that writes them back through the single
    //printer write path. Constructing PlateSettingsDialog directly would duplicate that
    //synchronisation, and a copy of it is a copy that can fall behind.
    wxCommandEvent evt(EVT_OPEN_PLATESETTINGSDIALOG);
    evt.SetInt(m_plate_index);
    evt.SetEventObject(m_plater);
    wxPostEvent(m_plater, evt);
}

void PlateInspector::rebuild_bed_type_choices(int plate_index, int current_bed_type)
{
    if (m_bed_choice == nullptr)
        return;

    m_bed_choice->Clear();
    m_bed_type_values.clear();

    //Entry 0 is the plate saying it follows the project, worded exactly as the dialog
    //words it. The rest are the bed types THIS plate's machine supports, which is why the
    //list is rebuilt per plate rather than once.
    m_bed_choice->AppendString(_L("Same as Global Plate Type"));

    const VendorProfile::PrinterModel *model = m_plater->get_plate_printer_model(plate_index);
    const ConfigOptionDef *            def   = print_config_def.get("curr_bed_type");
    if (def != nullptr) {
        for (size_t i = 0; i < def->enum_labels.size(); ++i) {
            const std::string &label = def->enum_labels[i];
            if (model != nullptr && std::find(model->not_support_bed_types.begin(),
                                              model->not_support_bed_types.end(),
                                              label) != model->not_support_bed_types.end())
                continue;
            m_bed_choice->AppendString(_L(label));
            m_bed_type_values.push_back((int) i + 1); //btDefault is 0, so labels start at 1
        }
    }

    m_syncing = true;
    int selection = 0;
    for (size_t i = 0; i < m_bed_type_values.size(); ++i)
        if (m_bed_type_values[i] == current_bed_type)
            selection = (int) i + 1;
    m_bed_choice->SetSelection(selection);
    m_syncing = false;
}

void PlateInspector::on_bed_type_selected()
{
    if (m_syncing || m_plater == nullptr || !m_plater->is_initialized())
        return;
    if (m_plate_index == PLATE_BOARD_NO_PLATE)
        return;

    PartPlate *plate = m_plater->get_partplate_list().get_plate(m_plate_index);
    if (plate == nullptr)
        return;

    const int selection = m_bed_choice->GetSelection();
    int       chosen    = 0; //btDefault: follow the global plate type
    if (selection > 0 && (size_t) selection <= m_bed_type_values.size())
        chosen = m_bed_type_values[selection - 1];

    if (chosen == (int) plate->get_bed_type())
        return;

    //PartPlate::set_bed_type resolves both the old and the new value against the project
    //bed type and only marks the slice stale when the EFFECTIVE bed actually changed, so
    //nothing here has to second-guess it.
    plate->set_bed_type((BedType) chosen);
    m_plater->update_project_dirty_from_presets();
    m_plater->set_plater_dirty(true);

    //deferred, because this is running inside the combo's own selection event and the
    //refresh repaints and re-lays out the panel the combo lives in
    Plater *plater = m_plater;
    CallAfter([plater]() { plater->update(); });
}

void PlateInspector::reload(const PlateBoardModel &model, const std::vector<int> &scoped_plates)
{
    //is_initialized(): the inspector is built inside Plater::priv's constructor, where
    //there is no plate list and no resolver to ask. Every one of the crashes this guard
    //exists for arrived through a sidebar control asking a question one frame too early.
    if (m_plater == nullptr || !m_plater->is_initialized() || wxGetApp().preset_bundle == nullptr)
        return;

    //The board rebuilt this immediately above us, in the one call that owns it. Building a
    //second one here doubled the cost of every plate click for a model that is only read.
    const std::vector<PlateBoardRow> &rows = model.rows();

    //only members that name a real row: if the set is ever handed an index the model has
    //no row for, the stale member is dropped rather than reconciled
    std::vector<const PlateBoardRow *> scope;
    for (int idx : scoped_plates)
        for (const PlateBoardRow &row : rows)
            if (row.plate_index == idx)
                scope.push_back(&row);

    //Every scope names at least one plate. The scope that named none described the
    //project's printer, and there is no such thing to describe.
    const bool single = scope.size() == 1;
    const bool empty  = scope.empty();

    m_plate_index = single ? scope.front()->plate_index : PLATE_BOARD_NO_PLATE;

    if (empty)
        m_badge_text = _L("NO PLATE");
    else if (single)
        m_badge_text = wxString::Format(_L("PLATE %02d"), scope.front()->plate_index + 1);
    else
        m_badge_text = wxString::Format(_L("%d PLATES"), (int) scope.size());
    m_header->Refresh();

    if (empty) {
        //Nothing selected and nothing to say about it. Every row below writes to a plate,
        //so with no plate the panel shows the one honest thing and stops.
        m_body->Show(false);
        m_last_shape = 0;
        Layout();
        if (GetParent() != nullptr)
            GetParent()->Layout();
        return;
    }
    m_body->Show(m_expanded);

    // ---- Printer -----------------------------------------------------------
    if (single) {
        wxString name = from_u8(scope.front()->printer_name);
        if (scope.front()->preset_missing)
            name += _L(" (not installed)");
        m_printer_value->SetLabel(name);
        m_printer_value->SetCursor(wxCursor(wxCURSOR_HAND));
    } else {
        std::set<std::string> machines;
        for (const PlateBoardRow *row : scope)
            machines.insert(row->printer_name);
        //Mixed carries NO revert affordance, here or anywhere below. Revert is per-plate,
        //because a revert whose effect the user cannot see is a control with an invisible
        //blast radius.
        m_printer_value->SetLabel(machines.size() == 1
                                      ? from_u8(*machines.begin())
                                      : wxString::Format(_L("Mixed, %d machines"), (int) machines.size()));
        m_printer_value->SetCursor(wxCursor(wxCURSOR_HAND));
    }

    // ---- Process -----------------------------------------------------------
    //A plate-context field like the printer, so it is written the same way: click the
    //value, pick from what will run here.
    m_process_label->Show(single);
    m_process_value->Show(single);
    if (single) {
        wxString process = scope.front()->process_name.empty() ? wxString(_L("None"))
                                                               : from_u8(scope.front()->process_name);
        //The name plus what this plate changed about it. A plate carried over from another
        //machine keeps the values that were chosen and takes the new machine's process
        //name, so the name on its own would be the smaller half of the truth.
        if (scope.front()->process_overrides > 0)
            process += wxString::Format(_L("  +%d changed"), (int) scope.front()->process_overrides);
        m_process_value->SetLabel(process);
    }

    // ---- Nozzle ------------------------------------------------------------
    //Read-only in every scope. A plate stores one printer string; the nozzle is a
    //property of the preset, so an editable-looking nozzle scoped to a plate would be a
    //lie the moment anyone clicked it.
    if (single) {
        const wxString diameter = scope.front()->nozzle_diameter > 0.
                                      ? wxString::Format("%.2f mm", scope.front()->nozzle_diameter)
                                      : wxString::FromUTF8("\xe2\x80\x93");
        m_nozzle_value->SetLabel(diameter + "   " +
                                 wxString::Format(_L("from %s"), from_u8(scope.front()->printer_name)));
    } else {
        m_nozzle_value->SetLabel(_L("from each plate's printer"));
    }

    // ---- Bed type ----------------------------------------------------------
    if (single) {
        rebuild_bed_type_choices(m_plate_index, scope.front()->bed_type);
        m_bed_choice->Show(true);
        m_bed_value->Show(false);
    } else {
        m_bed_choice->Show(false);
        m_bed_value->Show(true);
        std::set<int> bed_types;
        for (const PlateBoardRow *row : scope)
            bed_types.insert(row->bed_type);
        m_bed_value->SetLabel(bed_types.size() == 1 ? bed_type_label(*bed_types.begin()) : _L("Mixed"));
    }

    // ---- Filament ----------------------------------------------------------
    //One plate: its DECLARED slots - the plate's own spool bays, whether or not an object
    //references them yet - plus the trailing "+" that adds one. Several plates: the union
    //of slots their objects use, read-only, with the colours the model already resolved.
    std::vector<std::pair<int, std::string>> swatches;
    if (single) {
        if (PartPlate *plate = m_plater->get_partplate_list().get_plate(m_plate_index); plate != nullptr) {
            const std::vector<std::string> &names       = plate->get_filament_preset_names();
            const auto own_colours = wxGetApp().preset_bundle->plate_filament_colours(plate->get_slicing_context());
            for (size_t i = 0; i < names.size(); ++i) {
                std::string colour = i < own_colours.size() ? own_colours[i] : std::string();
                swatches.emplace_back(int(i + 1), colour);
            }
            //slot 0 is the add affordance; the strip draws it as "+"
            swatches.emplace_back(0, std::string());
        }
    } else {
        std::map<int, std::string> used;
        for (const PlateBoardRow *row : scope)
            for (size_t i = 0; i < row->filament_slots.size(); ++i)
                used[row->filament_slots[i]] = i < row->filament_colours.size() ? row->filament_colours[i]
                                                                                : std::string();
        for (const std::pair<const int, std::string> &slot : used)
            swatches.emplace_back(slot.first, slot.second);
    }
    m_filament_value->set_slots(swatches);
    //Writable only when the badge names one plate. A material picked into a mixed scope
    //would write a slot on plates whose other slots the panel is not showing.
    if (single)
        m_filament_value->set_on_slot_clicked([this](int slot) { on_filament_slot_click(slot); });
    else
        m_filament_value->set_on_slot_clicked(nullptr);

    // ---- Mapping -----------------------------------------------------------
    //Per-plate and read-only. Omitted outside PLATE scope: a single summary line cannot
    //describe several plates' maps without inventing a word for the disagreement.
    m_mapping_label->Show(single);
    m_mapping_value->Show(single);
    if (single)
        m_mapping_value->SetLabel(filament_map_mode_label(scope.front()->filament_map_mode));

    // ---- Prints on ---------------------------------------------------------
    //The physical machine. Its own field, because a slicing preset is not a machine: one
    //preset can serve several, and one machine changes nozzle over its life.
    m_device_label->Show(single);
    m_device_value->Show(single);
    if (single) {
        wxString device = _L("Not assigned");
        if (!scope.front()->device_id.empty()) {
            device = from_u8(scope.front()->device_id);
            if (DeviceManager *dev = wxGetApp().getDeviceManager())
                if (MachineObject *obj = dev->get_my_machine(scope.front()->device_id);
                    obj != nullptr && !obj->get_dev_name().empty())
                    device = from_u8(obj->get_dev_name());
        }
        m_device_value->SetLabel(device);
    }

    m_more_btn->Show(single);

    m_body->Layout();
    Layout();

    const int shape = single ? 1 : 2;
    if (shape != m_last_shape) {
        m_last_shape = shape;
        InvalidateBestSize();
        if (GetParent() != nullptr)
            GetParent()->Layout();
    }
}

}} // namespace Slic3r::GUI
