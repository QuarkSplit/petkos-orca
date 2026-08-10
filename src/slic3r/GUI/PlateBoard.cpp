#include "PlateBoard.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/sizer.h>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
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

namespace {

//The project's filament library, in slot order. Slot numbers on a plate are 1-based; this
//vector is 0-based, so a slot indexes it at slot - 1.
std::vector<std::string> project_filament_colours(const PresetBundle &bundle)
{
    const ConfigOptionStrings *colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    return colours != nullptr ? colours->values : std::vector<std::string>();
}

//filament_type of the preset currently loaded in a library slot. Empty when the slot is
//past the end of the library or its preset is not installed, which the material grouping
//reports as an unnamed material rather than filling in with a plausible one.
std::string slot_material_type(const PresetBundle &bundle, int slot)
{
    if (slot < 1 || (size_t) slot > bundle.filament_presets.size())
        return std::string();

    const Preset *preset = bundle.filaments.find_preset(bundle.filament_presets[(size_t) slot - 1], false);
    if (preset == nullptr)
        return std::string();

    const ConfigOptionStrings *type = preset->config.option<ConfigOptionStrings>("filament_type");
    if (type == nullptr || type->values.empty())
        return std::string();
    return type->values.front();
}

} // namespace

void PlateBoardModel::rebuild(const PartPlateList &plates, const PresetBundle &bundle, PlateBoardGrouping grouping)
{
    m_rows.clear();
    m_groups.clear();
    m_rollup = PlateBoardRollup();

    const std::string              project_printer = bundle.printers.get_selected_preset_name();
    const std::vector<std::string> colours         = project_filament_colours(bundle);

    m_project_row               = PlateBoardRow();
    m_project_row.plate_index   = PLATE_BOARD_PROJECT_ROW;
    m_project_row.printer_name  = project_printer;
    printer_bed_size(bundle, project_printer, m_project_row.bed_w, m_project_row.bed_d);
    printer_nozzle_diameter(bundle, project_printer, m_project_row.nozzle_diameter);

    const int count = plates.get_plate_count();
    m_all_inherited = true;

    //hours per machine, so the rollup can report the longest queue without proposing
    //a single move: max-of-sums is literally what the cell says it is
    std::map<std::string, float> queue_seconds;
    std::set<std::string>        assigned_machines;
    bool                         any_inherited = false;

    for (int i = 0; i < count; ++i) {
        const PartPlate *plate = plates.get_plate(i);
        if (plate == nullptr)
            continue;

        PlateBoardRow row;
        row.plate_index = i;
        row.assigned    = plate->has_printer_assignment();

        if (row.assigned) {
            m_all_inherited  = false;
            row.printer_name = plate->get_printer_preset_name();
            assigned_machines.insert(row.printer_name);
            //a stored name this installation does not have is preserved verbatim and
            //reported, never cleared, remapped or rendered as unassigned
            row.preset_missing = !printer_bed_size(bundle, row.printer_name, row.bed_w, row.bed_d);
        } else {
            any_inherited    = true;
            row.printer_name = project_printer;
            row.bed_w        = m_project_row.bed_w;
            row.bed_d        = m_project_row.bed_d;
        }

        //Nozzle belongs to the preset the row is showing, whether that is the plate's own
        //machine or the project one it is following. A plate stores no nozzle, so this is
        //read-only wherever it is displayed and is labelled with the preset it came from.
        printer_nozzle_diameter(bundle, row.printer_name, row.nozzle_diameter);

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

        if (row.sliced && plate->get_retained_print_statistics(row.print_time_seconds, row.weight_grams)) {
            row.has_time = true;
            m_rollup.total_seconds += row.print_time_seconds;
            queue_seconds[row.assigned ? row.printer_name : project_printer] += row.print_time_seconds;
        } else {
            //no valid slice: an em-dash in the row, zero in every total, and one more
            //in the trailing "N not estimated" note. Without the note the totals lie
            //by omission.
            ++m_rollup.not_estimated;
        }

        for (int slot : plate->get_extruders(true)) {
            row.filament_slots.push_back(slot);
            row.filament_colours.push_back(slot >= 1 && (size_t) slot <= colours.size()
                                               ? colours[(size_t) slot - 1]
                                               : std::string());
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
            row.material_type = slot_material_type(bundle, row.dominant_slot);
            if ((size_t) row.dominant_slot <= colours.size())
                row.material_colour = colours[(size_t) row.dominant_slot - 1];
        }

        m_rows.push_back(std::move(row));
    }

    m_rollup.plates   = (int) m_rows.size();
    m_rollup.machines = (int) assigned_machines.size() + (any_inherited && !project_printer.empty() ? 1 : 0);
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

    build_groups(grouping, project_printer, bundle);
}

void PlateBoardModel::build_groups(PlateBoardGrouping grouping, const std::string &project_printer, const PresetBundle &bundle)
{
    //Plate order is one flat list. It draws no group header at all: a single unnamed group
    //holding everything is a header that states nothing and costs a row of height to do it.
    if (grouping == PlateBoardGrouping::PlateOrder || m_rows.size() <= 1)
        return;

    const std::string mode_key = grouping == PlateBoardGrouping::ByMachine  ? "m:" :
                                 grouping == PlateBoardGrouping::ByCapacity ? "c:" : "f:";

    //A machine group, addressed by the preset name it collects. The empty name is the
    //inherited group: plates with no assignment of their own, following the Project row.
    auto machine_caption = [&](const std::string &machine) {
        return machine.empty() ? into_u8(_L("Following the project printer")) : machine;
    };
    auto machine_detail = [&](const std::string &machine) {
        double            w = 0., d = 0.;
        const std::string name = machine.empty() ? project_printer : machine;
        if (!printer_bed_size(bundle, name, w, d))
            return std::string();
        return into_u8(wxString::Format("%.0f x %.0f mm", w, d));
    };

    if (grouping == PlateBoardGrouping::ByMachine || grouping == PlateBoardGrouping::ByCapacity) {
        std::map<std::string, PlateBoardGroup> by_machine;
        for (int i = 0; i < (int) m_rows.size(); ++i) {
            const PlateBoardRow &row     = m_rows[(size_t) i];
            const std::string    machine = row.assigned ? row.printer_name : std::string();

            PlateBoardGroup &group = by_machine[machine];
            if (group.rows.empty()) {
                group.key           = mode_key + machine;
                group.caption       = machine_caption(machine);
                group.detail        = machine_detail(machine);
                group.names_machine = true;
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
            //panel proposing a single move. The inherited group sorts with the rest rather
            //than being pinned last, because a group that cannot sink is not "by capacity".
            std::stable_sort(m_groups.begin(), m_groups.end(),
                             [](const PlateBoardGroup &a, const PlateBoardGroup &b) {
                                 return a.queue_seconds > b.queue_seconds;
                             });
        } else {
            //by machine: the inherited group is the final one, because it is the absence of
            //an assignment rather than one more machine
            std::stable_partition(m_groups.begin(), m_groups.end(),
                                  [&](const PlateBoardGroup &g) { return g.key != mode_key; });
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

        const std::string machine = row.assigned ? row.printer_name : std::string();
        PlateBoardGroup & group   = bucket.machines[machine];
        if (group.rows.empty()) {
            group.key           = "f:" + material_key + "/" + machine;
            group.caption       = machine_caption(machine);
            group.detail        = machine_detail(machine);
            group.depth         = 1;
            group.names_machine = true;
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
    if (m_rows.size() <= 1 || m_all_inherited)
        return m_project_row.printer_name;
    return std::to_string(m_rollup.machines) + " " + into_u8(_L("machines"));
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
wxColour board_sel(bool dark)    { return theme(dark, "#BFE1DE"); } //checked item background
wxColour board_scope(bool dark)  { return theme(dark, "#EBF9F0"); } //in the scope set, not current
wxColour board_hover(bool dark)  { return theme(dark, "#E5F0EE"); } //focused item background
wxColour board_accent(bool dark) { return theme(dark, "#009688"); } //ORCA colour
wxColour board_warn(bool dark)   { return theme(dark, "#FF6F00"); } //secondary / attention
wxColour board_err(bool dark)    { return theme(dark, "#D01B1B"); } //error

//The bed glyph is the board's only geometric signal, and the whole point of it is that a
//smaller machine draws a visibly smaller rectangle. Sizes are given in millimetres and
//scaled against the project's largest bed, so the biggest machine fills the cell; see the
//note where glyph_reference_mm is computed.
wxSize bed_glyph_size(const wxWindow *win, double bed_w, double bed_d, double reference_mm, int cell_dip)
{
    if (reference_mm <= 0.)
        reference_mm = 250.;

    const int    cell  = win->FromDIP(cell_dip);
    const double scale = (double) cell / reference_mm;
    const int    w     = std::max(win->FromDIP(6), (int) (bed_w * scale + 0.5));
    const int    h     = std::max(win->FromDIP(5), (int) (bed_d * scale + 0.5));
    return wxSize(std::min(w, cell), std::min(h, cell));
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

} // namespace

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

    build_items(current_name);

    const int rows   = (int) m_items.size();
    const int height = std::min(FromDIP(420), rows * m_row_height + FromDIP(4));
    SetSize(wxSize(std::max(parent->GetSize().GetWidth(), FromDIP(280)), height));

    Bind(wxEVT_PAINT, &PlatePrinterPopup::on_paint, this);
    Bind(wxEVT_MOTION, &PlatePrinterPopup::on_mouse, this);
    Bind(wxEVT_LEFT_UP, &PlatePrinterPopup::on_mouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &PlatePrinterPopup::on_mouse, this);
}

void PlatePrinterPopup::build_items(const std::string &current_name)
{
    const PresetBundle &bundle = *wxGetApp().preset_bundle;

    //how many plates already sit on each machine, so the picker can say so, and which
    //plates have no assignment of their own, which is the only set the bulk footer
    //may touch
    std::map<std::string, int> plate_counts;
    const PartPlateList &      plates = m_plater->get_partplate_list();
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        const PartPlate *plate = plates.get_plate(i);
        if (plate == nullptr)
            continue;
        if (plate->has_printer_assignment())
            ++plate_counts[plate->get_printer_preset_name()];
        else
            m_unassigned_plates.push_back(i);
    }

    //the bed this plate is on right now, so a candidate that is smaller in either axis can
    //say so before the click rather than after it
    double current_w = 0., current_d = 0.;
    if (const PartPlate *plate = plates.get_plate(m_plate_index)) {
        const Vec2d size = plate->get_size();
        current_w = size.x();
        current_d = size.y();
    }

    //1. clearing the assignment is always the first item, so the pre-per-plate meaning
    //   of a plate is always one click away
    Item same;
    same.name  = std::string();
    same.label = _L("Same as Global");
    same.detail = from_u8(bundle.printers.get_selected_preset_name());
    m_items.push_back(same);

    //2. an assignment this installation cannot resolve is offered back verbatim, so
    //   opening the picker cannot be the thing that discards it
    if (!current_name.empty() && bundle.printers.find_preset(current_name, false) == nullptr) {
        Item keep;
        keep.name         = current_name;
        keep.label        = wxString::Format(_L("Keep %s"), from_u8(current_name));
        keep.detail       = _L("not installed");
        keep.keep_missing = true;
        m_items.push_back(keep);
    }

    //3. the installed printers, grouped by brand
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

        for (const Preset *preset : group.second) {
            Item item;
            item.name  = preset->name;
            item.label = from_u8(preset->name);

            if (PlateBoardModel::printer_bed_size(bundle, preset->name, item.bed_w, item.bed_d)) {
                item.detail = wxString::Format("%.0f x %.0f mm", item.bed_w, item.bed_d);
                m_glyph_reference_mm = std::max(m_glyph_reference_mm, std::max(item.bed_w, item.bed_d));
                item.smaller_bed = current_w > 0. && current_d > 0. &&
                                   (item.bed_w < current_w - 0.5 || item.bed_d < current_d - 0.5);
            }

            const std::map<std::string, int>::const_iterator used = plate_counts.find(preset->name);
            if (used != plate_counts.end())
                item.detail += wxString::Format("   %d %s", used->second,
                                                used->second == 1 ? _L("plate") : _L("plates"));
            m_items.push_back(item);
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

    if (!m_unassigned_plates.empty()) {
        Item bulk;
        bulk.is_bulk_toggle = true;
        bulk.label          = wxString::Format(_L("Also assign to every unassigned plate (%d)"),
                                               (int) m_unassigned_plates.size());
        m_items.push_back(bulk);
    }
}

void PlatePrinterPopup::Popup(wxWindow *focus)
{
    PopupWindow::Popup(focus);
}

int PlatePrinterPopup::hit_test(const wxPoint &pos) const
{
    const int index = (pos.y - FromDIP(2)) / m_row_height;
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
            m_bulk_to_unassigned = !m_bulk_to_unassigned;
            //the two footers name different visible sets, so exactly one can be armed
            if (m_bulk_to_unassigned)
                m_bulk_to_scope = false;
            Refresh();
            return;
        }

        if (m_items[index].is_scope_toggle) {
            m_bulk_to_scope = !m_bulk_to_scope;
            if (m_bulk_to_scope)
                m_bulk_to_unassigned = false;
            Refresh();
            return;
        }

        const std::string chosen = m_items[index].name;
        Plater *          plater = m_plater;
        const int         plate  = m_plate_index;

        std::vector<int> targets;
        if (m_bulk_to_scope)
            targets = m_scoped_plates;
        else if (m_bulk_to_unassigned)
            targets = m_unassigned_plates;
        if (!targets.empty() && std::find(targets.begin(), targets.end(), plate) == targets.end())
            targets.push_back(plate);

        Dismiss();
        //the assignment rebuilds the board this popup is parented to, so it runs after
        //the dismissal rather than underneath it. One write path either way, so the
        //undo snapshot, the bounds re-check and the slice bookkeeping happen once; the
        //batch takes ONE snapshot, or undoing a five-plate action would take five
        //presses.
        CallAfter([plater, plate, chosen, targets]() {
            if (targets.empty())
                plater->set_plate_printer(plate, chosen);
            else
                plater->set_plate_printers(targets, chosen);
        });
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
    const int glyph_col = FromDIP(18);
    const int text_x    = glyph_x + glyph_col + FromDIP(8);
    int       y         = FromDIP(2);

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
            const bool armed = item.is_scope_toggle ? m_bulk_to_scope : m_bulk_to_unassigned;
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

        //mini bed glyph, on the same scale as every other glyph in this popup, so the
        //machines in the list are visibly different sizes before one is picked
        if (item.bed_w > 0. && item.bed_d > 0.) {
            const wxSize glyph = bed_glyph_size(this, item.bed_w, item.bed_d, m_glyph_reference_mm, 16);
            dc.SetBrush(wxBrush(board_line(dark)));
            dc.SetPen(wxPen(board_dim(dark)));
            dc.DrawRectangle(glyph_x + (glyph_col - glyph.GetWidth()) / 2,
                             y + (m_row_height - glyph.GetHeight()) / 2,
                             glyph.GetWidth(), glyph.GetHeight());
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

//The grouping control's four segments, in the order they are drawn.
const PlateBoardGrouping GROUPING_ORDER[4] = {PlateBoardGrouping::PlateOrder, PlateBoardGrouping::ByMachine,
                                              PlateBoardGrouping::ByCapacity, PlateBoardGrouping::ByMaterial};

wxString grouping_label(PlateBoardGrouping grouping)
{
    switch (grouping) {
    case PlateBoardGrouping::ByMachine: return _L("Machine");
    case PlateBoardGrouping::ByCapacity: return _L("Capacity");
    case PlateBoardGrouping::ByMaterial: return _L("Material");
    case PlateBoardGrouping::PlateOrder:
    default: return _L("Plate order");
    }
}

//A fixed id rather than wxWindow::NewControlId(): this is a namespace-scope constant, and
//an allocator called during static initialisation runs before wx is up.
const int PLATE_BOARD_ANIM_TIMER_ID = wxID_HIGHEST + 4211;
//220 ms, which is the whole point of the animation: long enough to be seen as a resize
//rather than a repaint, short enough not to be waited on.
const int PLATE_BOARD_ANIM_MS   = 220;
const int PLATE_BOARD_ANIM_STEP = 16;
//How many standard rows the board grows to before it scrolls instead. One fewer than the
//compact threshold, because the rollup tiles and the grouping control sit above the rows
//and the controls below the board must keep their place on a laptop screen.
const int PLATE_BOARD_VISIBLE_ROWS = 7;

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
    m_row_height     = FromDIP(34);
    m_row_compact    = FromDIP(22);
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
    m_icons_ok            = sliced_ok || stale_ok || problem_ok;

    m_anim_timer.SetOwner(this, PLATE_BOARD_ANIM_TIMER_ID);

    Bind(wxEVT_PAINT, &PlateBoard::on_paint, this);
    Bind(wxEVT_MOTION, &PlateBoard::on_mouse, this);
    Bind(wxEVT_LEFT_UP, &PlateBoard::on_mouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &PlateBoard::on_mouse, this);
    Bind(wxEVT_MOUSEWHEEL, &PlateBoard::on_scroll, this);
    Bind(wxEVT_TIMER, &PlateBoard::on_anim_tick, this, PLATE_BOARD_ANIM_TIMER_ID);
}

void PlateBoard::reload()
{
    //is_initialized(): the board is created during Plater::priv's constructor, so it
    //can be asked to reload before there is a plate list to read
    if (m_plater == nullptr || !m_plater->is_initialized() || wxGetApp().preset_bundle == nullptr)
        return;

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

    sync_glyph_targets();
    rebuild_items();
    clamp_scroll();

    //A row count change changes the height this control asks the sizer for, and nothing
    //below it moves until somebody lays the panel out. Only a real change asks, because
    //reload() runs on every preset update and a parent-wide layout on each of those is a
    //storm rather than a refresh.
    const int best = DoGetBestSize().GetHeight();
    if (best != m_best_height) {
        m_best_height = best;
        InvalidateBestSize();
        if (GetParent() != nullptr)
            GetParent()->Layout();
    }
    Refresh();
}

void PlateBoard::set_grouping(PlateBoardGrouping grouping)
{
    if (m_grouping == grouping && m_grouping_explicit)
        return;

    m_grouping          = grouping;
    m_grouping_explicit = true;
    m_scroll_px         = 0;

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
        m_items.push_back(item);
    };

    if (groups.empty()) {
        //Plate order. Compact above eight rows for the same reason a group is: at that
        //length the list stops being read and starts being scanned. The printer name stays,
        //because in this mode nothing above the row is naming it.
        const int height = (int) rows.size() > PLATE_BOARD_COMPACT_ABOVE ? m_row_compact : m_row_height;
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

        const int height = group.plate_count > PLATE_BOARD_COMPACT_ABOVE ? m_row_compact : m_row_height;
        for (int row_index : group.rows)
            push(false, g, row_index, height);
    }
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
    const std::vector<PlateBoardRow> &rows = m_model.rows();
    for (int i = 0; i < (int) m_items.size(); ++i) {
        const Item &item = m_items[(size_t) i];
        if (!item.header && item.row >= 0 && item.row < (int) rows.size() &&
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
    scroll_row_into_view(current_plate);
    Refresh();
}

wxSize PlateBoard::DoGetBestSize() const
{
    //past a handful of standard rows the board scrolls instead of growing. A narrow
    //sidebar's real failure mode is not a long list, it is a long list pushing the nozzle,
    //bed and extruder controls below it off the screen.
    const int cap     = PLATE_BOARD_VISIBLE_ROWS * m_row_height;
    const int content = std::min(m_content_height, cap);
    return wxSize(-1, rollup_height() + grouping_height() + content + FromDIP(4));
}

int PlateBoard::segment_at(int x) const
{
    const int width = GetClientSize().GetWidth();
    if (width <= 0)
        return -1;
    const int index = x * 4 / std::max(1, width);
    return index < 0 ? -1 : std::min(3, index);
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
        hit.kind  = item.header ? HitKind::GroupHeader : HitKind::Row;
        hit.index = item.header ? item.group : item.row;
        return hit;
    }
    return hit;
}

void PlateBoard::on_scroll(wxMouseEvent &evt)
{
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

void PlateBoard::set_scope(const std::vector<int> &scoped_plates, bool project_scope)
{
    if (m_scoped_plates == scoped_plates && m_scope_project == project_scope)
        return;
    m_scoped_plates = scoped_plates;
    m_scope_project = project_scope;
    Refresh();
}

void PlateBoard::on_mouse(wxMouseEvent &evt)
{
    if (evt.GetEventType() == wxEVT_LEAVE_WINDOW) {
        m_hover = Hit();
        Refresh();
        return;
    }

    const Hit hit = hit_test(evt.GetPosition());

    if (evt.GetEventType() == wxEVT_MOTION) {
        if (hit != m_hover) {
            m_hover = hit;
            Refresh();
        }
        return;
    }

    if (evt.GetEventType() != wxEVT_LEFT_UP)
        return;

    if (m_plater == nullptr || !m_plater->is_initialized())
        return;

    switch (hit.kind) {
    case HitKind::Rollup:
        //The rollup is the one thing the board draws that is about the whole project
        //rather than about a plate, so it is where PROJECT scope is pointed at. Global
        //editing has to be a destination you can click, not a mode inferred from what was
        //last touched; the project printer combo pinned above the board stays the editor,
        //and this only moves the inspector's scope onto it.
        m_plater->sidebar().set_project_scope();
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

        //the printer chip opens the picker; anywhere else on the row selects the plate.
        //A compact row starts its content further left, so the chip has to follow the row
        //rather than sit at a constant that is right for one of the two heights.
        const int item_index = find_row_item(row.plate_index);
        if (item_index >= 0) {
            const Item &item    = m_items[(size_t) item_index];
            const bool  compact = item.height <= m_row_compact;
            const int   chip_x  = compact ? FromDIP(28) : FromDIP(60);
            const int   chip_w  = GetClientSize().GetWidth() - chip_x - FromDIP(80);
            //a compact row under a header that already names the machine shows no name, so
            //there is no chip on it to click; the picker is reached from the inspector
            const std::vector<PlateBoardGroup> &groups = m_model.groups();
            const bool names_machine_above = item.group >= 0 && item.group < (int) groups.size() &&
                                             groups[(size_t) item.group].names_machine;
            const bool has_chip = !(compact && names_machine_above);
            if (has_chip && chip_w > 0 && evt.GetPosition().x >= chip_x && evt.GetPosition().x < chip_x + chip_w) {
                const int y = view_top() + item.y - m_scroll_px + item.height;
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

    //the rollup is the PROJECT scope's destination, so it says when it is the thing the
    //inspector is describing
    if (m_scope_project) {
        dc.SetBrush(wxBrush(board_sel(dark)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRoundedRectangle(0, 0, width, height, FromDIP(4));
    }

    //Figures render as figures. Four tiles, each a value over its label, rather than one
    //run-on sentence of numbers: a line that is mostly digits is the wrong shape for the
    //thing it is describing.
    struct Tile
    {
        wxString value;
        wxString label;
    };
    const Tile tiles[4] = {
        {wxString::Format("%d", rollup.plates), _L("plates")},
        {wxString::Format("%d", rollup.machines), _L("machines")},
        {format_hours(rollup.total_seconds), _L("total")},
        {format_hours(rollup.longest_queue_seconds), _L("longest queue")},
    };

    const int cell = std::max(FromDIP(10), width / 4);
    for (int i = 0; i < 4; ++i) {
        const int x     = i * cell + FromDIP(4);
        const int max_w = cell - FromDIP(8);

        dc.SetFont(Label::Head_13);
        dc.SetTextForeground(board_fg(dark));
        dc.DrawText(wxControl::Ellipsize(tiles[i].value, dc, wxELLIPSIZE_END, max_w), x, FromDIP(4));

        dc.SetFont(Label::Body_9);
        dc.SetTextForeground(board_dim(dark));
        dc.DrawText(wxControl::Ellipsize(tiles[i].label, dc, wxELLIPSIZE_END, max_w), x, FromDIP(22));
    }

    //Without this the totals are a lie by omission: a plate with no valid slice reads as
    //zero hours in every cell above.
    if (rollup.not_estimated > 0) {
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

    const int cell = std::max(FromDIP(10), width / 4);
    for (int i = 0; i < 4; ++i) {
        const bool active = m_grouping == GROUPING_ORDER[i];
        const bool hover  = m_hover.kind == HitKind::Segment && m_hover.index == i;
        const int  x      = i * cell;
        const int  w      = i == 3 ? width - x : cell;

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
    dc.SetBrush(wxBrush(board_head(dark)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, y, width, height);

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

    //By capacity: a bar measured against the project's longest queue, so an idle machine
    //reads short at a glance. It states the number the rollup already headlines and
    //proposes nothing; rebalancing machines is the farm's job, not a sidebar's.
    if (m_grouping == PlateBoardGrouping::ByCapacity) {
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
        dc.SetBrush(ok ? wxBrush(colour) : *wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(board_line(dark)));
        dc.DrawRectangle(x + drawn * (box + gap), y, box, box);
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
    if (row.parts_outside || row.preset_missing)
        icon = &m_icon_problem;
    else if (row.stale)
        icon = &m_icon_stale;
    else if (row.sliced)
        icon = &m_icon_sliced;

    if (icon != nullptr && icon->bmp().IsOk())
        dc.DrawBitmap(icon->bmp(), x, y, true);
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

    const bool compact = height <= m_row_compact;
    const int  icon_x  = width - FromDIP(18);
    const int  right   = icon_x - FromDIP(6);

    // ---- index -------------------------------------------------------------
    //1-based, because that is the numbering the rest of the app uses and the one every
    //filename the prep pipeline writes is keyed on. Right-aligned so 1 and 36 line up.
    dc.SetFont(compact ? Label::Body_9 : Label::Body_10);
    dc.SetTextForeground(selected ? board_fg(dark) : board_dim(dark));
    const wxString index_text   = wxString::Format("%d", row.plate_index + 1);
    const wxSize   index_extent = dc.GetTextExtent(index_text);
    dc.DrawText(index_text, FromDIP(22) - index_extent.GetWidth(), y + (height - index_extent.GetHeight()) / 2);

    // ---- hours -------------------------------------------------------------
    dc.SetFont(compact ? Label::Body_10 : Label::Head_11);
    dc.SetTextForeground(row.has_time ? board_soft(dark) : board_dim(dark));
    const wxString hours        = format_hours(row.has_time ? row.print_time_seconds : 0.f);
    const wxSize   hours_extent = dc.GetTextExtent(hours);
    const int      hours_x      = right - hours_extent.GetWidth();
    dc.DrawText(hours, hours_x, y + (compact ? (height - hours_extent.GetHeight()) / 2 : FromDIP(4)));

    draw_state_icon(dc, row, icon_x, y + (height - FromDIP(14)) / 2);

    // ---- the name, and whether this row has to carry it --------------------
    //A compact row spends the width it saves on its swatches, and it only saves that width
    //when the header above it already names the machine. Dropping the name under a header
    //that does not state one would leave the row unable to say what it is on.
    const bool names_machine_above = group != nullptr && group->names_machine;
    const int  content_x           = compact ? FromDIP(28) : FromDIP(60);

    if (!compact) {
        //bed glyph: proportional to the largest bed in this project, animated so a
        //reassignment that resizes it is seen rather than merely applied. This is the
        //board's only geometric signal; it does not draw plates. The 3D scene is the
        //scene, and a second one inside a narrow column is a worse copy of it.
        double glyph_w = row.bed_w, glyph_h = row.bed_d;
        if ((size_t) row_index < m_glyphs.size()) {
            const GlyphAnim &anim = m_glyphs[(size_t) row_index];
            const double     t = m_anim_running ? smoothstep((double) m_anim_elapsed_ms / PLATE_BOARD_ANIM_MS) : 1.;
            glyph_w = anim.from_w + (anim.to_w - anim.from_w) * t;
            glyph_h = anim.from_h + (anim.to_h - anim.from_h) * t;
        }

        const wxSize glyph = bed_glyph_size(this, glyph_w, glyph_h, m_model.glyph_reference_mm(), 26);
        const int    cell  = FromDIP(26);
        dc.SetBrush(wxBrush(row.assigned ? board_soft(dark) : board_line(dark)));
        dc.SetPen(wxPen(board_dim(dark)));
        dc.DrawRectangle(FromDIP(28) + (cell - glyph.GetWidth()) / 2,
                         y + (height - glyph.GetHeight()) / 2, glyph.GetWidth(), glyph.GetHeight());
    }

    const bool show_name = !compact || !names_machine_above;
    if (show_name) {
        //an inherited row is greyed and shows the project printer it is following; an
        //assigned row is in normal weight. The distinction comes from
        //has_printer_assignment() and from nowhere else, so a project loaded from a 3MF
        //that already carries assignments shows them.
        dc.SetTextForeground(row.preset_missing ? board_err(dark)
                                                : (row.assigned ? board_fg(dark) : board_dim(dark)));
        dc.SetFont(compact ? Label::Body_10 : Label::Body_12);

        wxString name = from_u8(row.printer_name);
        if (row.preset_missing)
            name += _L(" (not installed)");

        const int      name_max = std::max(FromDIP(30), hours_x - content_x - FromDIP(8));
        const wxString shown    = wxControl::Ellipsize(name, dc, wxELLIPSIZE_END, name_max);
        const wxSize   extent   = dc.GetTextExtent(shown);
        const int      name_y   = compact ? y + (height - extent.GetHeight()) / 2 : y + FromDIP(4);
        dc.DrawText(shown, content_x, name_y);

        //struck through, so an assignment this installation cannot resolve reads as an
        //assignment that is being kept rather than one that is in force. Drawn rather than
        //asked of the font, which is the same reason the chevrons are geometry.
        if (row.preset_missing) {
            dc.SetPen(wxPen(board_err(dark)));
            const int mid = name_y + extent.GetHeight() / 2;
            dc.DrawLine(content_x, mid, content_x + extent.GetWidth(), mid);
        }
    }

    // ---- second line, or the width the name did not take -------------------
    const int box = FromDIP(9);
    if (compact) {
        if (!show_name)
            draw_swatches(dc, dark, row, content_x, y + (height - box) / 2, box, hours_x - content_x - FromDIP(8));
        return;
    }

    draw_swatches(dc, dark, row, content_x, y + FromDIP(21), box, FromDIP(90));

    dc.SetFont(Label::Body_9);
    dc.SetTextForeground(board_dim(dark));
    const wxString parts = row.part_count == 1 ? _L("1 part") : wxString::Format(_L("%d parts"), row.part_count);
    const wxSize   parts_extent = dc.GetTextExtent(parts);
    dc.DrawText(parts, right - parts_extent.GetWidth(), y + FromDIP(21));
}

void PlateBoard::on_paint(wxPaintEvent &evt)
{
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

    for (const Item &item : m_items) {
        const int y = top + item.y - m_scroll_px;
        if (y + item.height <= top)
            continue;
        if (y >= top + visible)
            break;

        if (item.header) {
            if (item.group >= 0 && item.group < (int) groups.size())
                draw_group_header(dc, dark, width, y, item.height, groups[(size_t) item.group],
                                  m_collapsed.count(groups[(size_t) item.group].key) > 0);
        } else if (item.row >= 0 && item.row < (int) rows.size()) {
            const PlateBoardGroup *group = item.group >= 0 && item.group < (int) groups.size()
                                               ? &groups[(size_t) item.group]
                                               : nullptr;
            draw_row(dc, dark, width, y, item.height, item.row, group);
        }
    }

    //Sticky: the group a scrolled row belongs to stays named at the top of the region, or
    //a scrolled list is a list of rows with nothing saying what they are under.
    const int sticky = m_scroll_px > 0 ? sticky_group() : -1;
    if (sticky >= 0 && sticky < (int) groups.size())
        draw_group_header(dc, dark, width, top, m_header_height, groups[(size_t) sticky],
                          m_collapsed.count(groups[(size_t) sticky].key) > 0);

    dc.DestroyClippingRegion();
}

// ----------------------------------------------------------------------------
// PlateSwatchStrip
// ----------------------------------------------------------------------------

//What a plate shows for filament is USAGE: which library slots its instances reference.
//That is a fact the file already carries. A filament combo scoped to a plate would write
//nowhere, because filament selection is not per-plate, so this strip is read-only by
//construction rather than by a disabled flag.
class PlateSwatchStrip : public wxPanel
{
public:
    PlateSwatchStrip(wxWindow *parent) : wxPanel(parent, wxID_ANY)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(wxSize(-1, FromDIP(18)));
        Bind(wxEVT_PAINT, &PlateSwatchStrip::on_paint, this);
    }

    //slots are the 1-based library slot numbers, in slot order. The colour is whatever
    //the project library holds for that slot, or an empty string when the library is
    //shorter than the slot the plate references, which is drawn hollow rather than
    //guessed at.
    void set_slots(const std::vector<std::pair<int, std::string>> &slots)
    {
        if (m_slots == slots)
            return;
        m_slots = slots;
        Refresh();
    }

private:
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

        dc.SetFont(Label::Body_9);
        for (const std::pair<int, std::string> &slot : m_slots) {
            if (x + box + FromDIP(18) > GetClientSize().GetWidth())
                break;

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

    m_source_label = make_label(_L("Source"));
    m_source_value = make_value(wxString());
    grid->Add(m_source_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_source_value, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

    m_nozzle_label = make_label(_L("Nozzle"));
    m_nozzle_value = make_value(wxString());
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
    grid->Add(m_filament_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_filament_value, 1, wxEXPAND);

    m_mapping_label = make_label(_L("Mapping"));
    m_mapping_value = make_value(wxString());
    grid->Add(m_mapping_label, 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_mapping_value, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND);

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
    if (m_plate_index == PLATE_BOARD_PROJECT_ROW)
        return;

    //the scope the board is rendering is the scope the picker may act on, so the two can
    //never disagree about what "the selected plates" means
    std::vector<int> scope;
    if (m_plater->sidebar().scoped_plates().size() > 1)
        scope = m_plater->sidebar().scoped_plates();

    const wxPoint anchor = m_printer_value->ClientToScreen(wxPoint(0, m_printer_value->GetSize().GetHeight()));
    show_printer_picker(this, m_plater, m_plate_index, scope, anchor, m_popup);
}

void PlateInspector::on_more_plate_settings()
{
    if (m_plater == nullptr || !m_plater->is_initialized() || m_plate_index == PLATE_BOARD_PROJECT_ROW)
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
    if (m_plate_index == PLATE_BOARD_PROJECT_ROW)
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

void PlateInspector::reload(const std::vector<int> &scoped_plates, bool project_scope)
{
    //is_initialized(): the inspector is built inside Plater::priv's constructor, where
    //there is no plate list and no resolver to ask. Every one of the crashes this guard
    //exists for arrived through a sidebar control asking a question one frame too early.
    if (m_plater == nullptr || !m_plater->is_initialized() || wxGetApp().preset_bundle == nullptr)
        return;

    PlateBoardModel model;
    model.rebuild(m_plater->get_partplate_list(), *wxGetApp().preset_bundle);
    const std::vector<PlateBoardRow> &rows = model.rows();

    //only members that name a real row: if the set is ever handed an index the model has
    //no row for, the stale member is dropped rather than reconciled
    std::vector<const PlateBoardRow *> scope;
    for (int idx : scoped_plates)
        for (const PlateBoardRow &row : rows)
            if (row.plate_index == idx)
                scope.push_back(&row);

    const bool project = project_scope || scope.empty();
    const bool single  = !project && scope.size() == 1;

    m_plate_index = single ? scope.front()->plate_index : PLATE_BOARD_PROJECT_ROW;

    if (project)
        m_badge_text = _L("PROJECT");
    else if (single)
        m_badge_text = wxString::Format(_L("PLATE %02d"), scope.front()->plate_index + 1);
    else
        m_badge_text = wxString::Format(_L("%d PLATES"), (int) scope.size());
    m_header->Refresh();

    const PlateBoardRow &project_row = model.project_row();

    // ---- Printer -----------------------------------------------------------
    if (project) {
        m_printer_value->SetLabel(from_u8(project_row.printer_name));
        //the combo pinned above the board is the project printer's editor; this row
        //states it rather than offering a second way to change it
        m_printer_value->SetCursor(wxCursor(wxCURSOR_ARROW));
    } else if (single) {
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

    // ---- Source ------------------------------------------------------------
    //Whether the printer above is this plate's own assignment or the Project row it is
    //still following. The board greys an inherited chip; this says the same thing in
    //words, because the greying is a convention and this is the row that has to be read
    //before the picker is opened.
    m_source_label->Show(!project);
    m_source_value->Show(!project);
    if (single) {
        m_source_value->SetLabel(scope.front()->assigned ? _L("Assigned to this plate")
                                                         : _L("Same as Global"));
    } else if (!project) {
        int assigned = 0;
        for (const PlateBoardRow *row : scope)
            assigned += row->assigned ? 1 : 0;
        m_source_value->SetLabel(assigned == 0                  ? _L("Same as Global") :
                                 assigned == (int) scope.size() ? _L("Assigned to these plates") :
                                                                  _L("Mixed"));
    }

    // ---- Nozzle ------------------------------------------------------------
    //Read-only in every scope. A plate stores one printer string; the nozzle is a
    //property of the preset, so an editable-looking nozzle scoped to a plate would be a
    //lie the moment anyone clicked it.
    if (project) {
        m_nozzle_value->SetLabel(project_row.nozzle_diameter > 0.
                                     ? wxString::Format("%.2f mm", project_row.nozzle_diameter)
                                     : wxString::FromUTF8("\xe2\x80\x93"));
    } else if (single) {
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
        if (project) {
            //the project bed combo above the board is the editor for this one
            m_bed_value->SetLabel(bed_type_label(0));
        } else {
            std::set<int> bed_types;
            for (const PlateBoardRow *row : scope)
                bed_types.insert(row->bed_type);
            m_bed_value->SetLabel(bed_types.size() == 1 ? bed_type_label(*bed_types.begin()) : _L("Mixed"));
        }
    }

    // ---- Filament usage ----------------------------------------------------
    std::vector<std::pair<int, std::string>> swatches;
    if (project) {
        //PROJECT scope shows the library itself, not any plate's usage of it
        const ConfigOptionStrings *colour_opt =
            wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        if (colour_opt != nullptr)
            for (size_t i = 0; i < colour_opt->values.size(); ++i)
                swatches.emplace_back((int) i + 1, colour_opt->values[i]);
    } else {
        //the union across the selection, in slot order, with the colours the model already
        //resolved: two places resolving one library is two places to disagree about it
        std::map<int, std::string> used;
        for (const PlateBoardRow *row : scope)
            for (size_t i = 0; i < row->filament_slots.size(); ++i)
                used[row->filament_slots[i]] = i < row->filament_colours.size() ? row->filament_colours[i]
                                                                                : std::string();
        for (const std::pair<const int, std::string> &slot : used)
            swatches.emplace_back(slot.first, slot.second);
    }
    m_filament_value->set_slots(swatches);

    // ---- Mapping -----------------------------------------------------------
    //Per-plate and read-only. Omitted outside PLATE scope: a single summary line cannot
    //describe several plates' maps without inventing a word for the disagreement.
    m_mapping_label->Show(single);
    m_mapping_value->Show(single);
    if (single)
        m_mapping_value->SetLabel(filament_map_mode_label(scope.front()->filament_map_mode));

    m_more_btn->Show(single);

    m_body->Layout();
    Layout();

    const int shape = project ? 0 : (single ? 1 : 2);
    if (shape != m_last_shape) {
        m_last_shape = shape;
        InvalidateBestSize();
        if (GetParent() != nullptr)
            GetParent()->Layout();
    }
}

}} // namespace Slic3r::GUI
