#include "PlateBoard.hpp"

#include <algorithm>
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

void PlateBoardModel::rebuild(const PartPlateList &plates, const PresetBundle &bundle)
{
    m_rows.clear();
    m_rollup = PlateBoardRollup();

    const std::string project_printer = bundle.printers.get_selected_preset_name();

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

        for (int slot : plate->get_extruders(true))
            row.filament_slots.push_back(slot);

        m_rows.push_back(std::move(row));
    }

    m_rollup.plates   = (int) m_rows.size();
    m_rollup.machines = (int) assigned_machines.size() + (any_inherited && !project_printer.empty() ? 1 : 0);
    for (const std::pair<const std::string, float> &queue : queue_seconds)
        m_rollup.longest_queue_seconds = std::max(m_rollup.longest_queue_seconds, queue.second);
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

wxColour board_bg(bool dark) { return dark ? wxColour(56, 56, 56) : wxColour(255, 255, 255); }
wxColour board_fg(bool dark) { return dark ? wxColour(230, 230, 230) : wxColour(38, 46, 48); }
wxColour board_dim(bool dark) { return dark ? wxColour(150, 150, 150) : wxColour(140, 140, 140); }
wxColour board_sel(bool dark) { return dark ? wxColour(0, 92, 78) : wxColour(219, 253, 231); }
wxColour board_hover(bool dark) { return dark ? wxColour(70, 70, 70) : wxColour(243, 243, 243); }
//a scoped row that is not the current plate: visibly in the set, visibly not the plate
//the 3D scene is showing
wxColour board_scope(bool dark) { return dark ? wxColour(58, 74, 72) : wxColour(236, 247, 242); }
wxColour board_line(bool dark) { return dark ? wxColour(80, 80, 80) : wxColour(230, 230, 230); }
wxColour board_warn(bool dark) { return dark ? wxColour(255, 111, 92) : wxColour(216, 62, 42); }

//The bed glyph is the board's only geometric signal, and the whole point of it is
//that a smaller machine draws a smaller rectangle. 26 px is one 280 mm bed.
wxSize bed_glyph_size(wxWindow *win, double bed_w, double bed_d)
{
    const int w = std::max(win->FromDIP(8), (int) (bed_w * win->FromDIP(26) / 280.));
    const int h = std::max(win->FromDIP(7), (int) (bed_d * win->FromDIP(26) / 280.));
    return wxSize(std::min(w, win->FromDIP(34)), std::min(h, win->FromDIP(34)));
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
    SetSize(wxSize(std::max(parent->GetSize().GetWidth(), FromDIP(260)), height));

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

            double w = 0., d = 0.;
            if (PlateBoardModel::printer_bed_size(bundle, preset->name, w, d))
                item.detail = wxString::Format("%.0f x %.0f mm", w, d);

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

    const int width = GetClientSize().GetWidth();
    int       y     = FromDIP(2);

    for (size_t i = 0; i < m_items.size(); ++i, y += m_row_height) {
        const Item &item = m_items[i];

        if ((int) i == m_hover && !item.is_header) {
            dc.SetBrush(wxBrush(board_hover(dark)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, y, width, m_row_height);
        }

        if (item.is_header) {
            dc.SetTextForeground(board_dim(dark));
            dc.SetFont(Label::Body_10);
            dc.DrawText(item.label, FromDIP(6), y + FromDIP(6));
            continue;
        }

        if (item.is_bulk_toggle || item.is_scope_toggle) {
            const bool armed = item.is_scope_toggle ? m_bulk_to_scope : m_bulk_to_unassigned;
            dc.SetPen(wxPen(board_line(dark)));
            dc.DrawLine(FromDIP(6), y, width - FromDIP(6), y);
            dc.SetFont(Label::Body_11);
            dc.SetTextForeground(armed ? board_fg(dark) : board_dim(dark));
            dc.DrawText(wxString(armed ? wxString::FromUTF8("\xe2\x98\x91  ")
                                       : wxString::FromUTF8("\xe2\x98\x90  ")) + item.label,
                        FromDIP(10), y + FromDIP(5));
            continue;
        }

        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(item.keep_missing ? board_warn(dark) : board_fg(dark));
        dc.DrawText(item.label, FromDIP(14), y + FromDIP(4));

        if (!item.detail.IsEmpty()) {
            dc.SetFont(Label::Body_10);
            dc.SetTextForeground(board_dim(dark));
            const wxSize extent = dc.GetTextExtent(item.detail);
            dc.DrawText(item.detail, width - extent.GetWidth() - FromDIP(8), y + FromDIP(6));
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

} // namespace

// ----------------------------------------------------------------------------
// PlateBoard
// ----------------------------------------------------------------------------

PlateBoard::PlateBoard(wxWindow *parent, Plater *plater) : wxPanel(parent, wxID_ANY), m_plater(plater)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_row_height    = FromDIP(34);
    m_rollup_height = FromDIP(24);

    Bind(wxEVT_PAINT, &PlateBoard::on_paint, this);
    Bind(wxEVT_MOTION, &PlateBoard::on_mouse, this);
    Bind(wxEVT_LEFT_UP, &PlateBoard::on_mouse, this);
    Bind(wxEVT_LEAVE_WINDOW, &PlateBoard::on_mouse, this);
    Bind(wxEVT_MOUSEWHEEL, &PlateBoard::on_scroll, this);
}

void PlateBoard::reload()
{
    //is_initialized(): the board is created during Plater::priv's constructor, so it
    //can be asked to reload before there is a plate list to read
    if (m_plater == nullptr || !m_plater->is_initialized() || wxGetApp().preset_bundle == nullptr)
        return;

    m_model.rebuild(m_plater->get_partplate_list(), *wxGetApp().preset_bundle);
    m_current_plate = m_plater->get_partplate_list().get_curr_plate_index();

    const int rows       = (int) m_model.rows().size();
    const int max_scroll = std::max(0, rows - 8);
    m_scroll_rows        = std::min(m_scroll_rows, max_scroll);

    InvalidateBestSize();
    Refresh();
}

void PlateBoard::on_plate_selection_changed(int current_plate)
{
    //idempotent: a double delivery costs nothing, so no argument about whether one
    //can happen has to be won
    if (current_plate == m_current_plate)
        return;

    m_current_plate = current_plate;

    //scroll the current row into view rather than leaving the selection off-screen
    if (current_plate < m_scroll_rows)
        m_scroll_rows = current_plate;
    else if (current_plate >= m_scroll_rows + 8)
        m_scroll_rows = std::max(0, current_plate - 7);

    Refresh();
}

wxSize PlateBoard::DoGetBestSize() const
{
    const int rows   = (int) m_model.rows().size();
    //past eight rows the board scrolls instead of growing. A narrow sidebar's real
    //failure mode is not a long list, it is a long list pushing the nozzle, bed and
    //extruder controls below it off the screen.
    const int shown  = std::min(rows, 8);
    const int rollup = rows > 1 ? m_rollup_height : 0;
    return wxSize(-1, rollup + shown * m_row_height + FromDIP(4));
}

int PlateBoard::hit_test(const wxPoint &pos) const
{
    const int top = (int) m_model.rows().size() > 1 ? m_rollup_height : 0;
    if (pos.y < top)
        return -2; //the rollup is not a row

    const int index = (pos.y - top) / m_row_height + m_scroll_rows;
    if (index < 0 || index >= (int) m_model.rows().size())
        return -2;
    return index;
}

void PlateBoard::on_scroll(wxMouseEvent &evt)
{
    const int rows       = (int) m_model.rows().size();
    const int max_scroll = std::max(0, rows - 8);
    if (max_scroll == 0) {
        evt.Skip();
        return;
    }

    m_scroll_rows = std::min(max_scroll, std::max(0, m_scroll_rows - (evt.GetWheelRotation() > 0 ? 1 : -1)));
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
        m_hover = -2;
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

    if (evt.GetEventType() != wxEVT_LEFT_UP)
        return;

    if (m_plater == nullptr || !m_plater->is_initialized())
        return;

    const int top = (int) m_model.rows().size() > 1 ? m_rollup_height : 0;

    if (index < 0) {
        //The rollup line is the one thing the board draws that is about the whole
        //project rather than about a plate, so it is where PROJECT scope is pointed at.
        //Global editing has to be a destination you can click, not a mode inferred from
        //what was last touched; the project printer combo pinned above the board stays
        //the editor, and this only moves the inspector's scope onto it.
        if (top > 0 && evt.GetPosition().y < top)
            m_plater->sidebar().set_project_scope();
        return;
    }

    //Ctrl/shift-click changes the SCOPE and never the current plate. Checked before the
    //chip, so modifier-clicking a row's printer name extends the selection rather than
    //opening a picker the user did not ask for.
    if (evt.ControlDown() || evt.ShiftDown()) {
        m_plater->sidebar().toggle_scoped_plate(index);
        return;
    }

    //the printer chip opens the picker; anywhere else on the row selects the plate
    const int chip_x = FromDIP(56);
    const int chip_w = GetClientSize().GetWidth() - chip_x - FromDIP(76);
    if (evt.GetPosition().x >= chip_x && evt.GetPosition().x < chip_x + chip_w) {
        open_picker(index, ClientToScreen(wxPoint(0, top + (index - m_scroll_rows + 1) * m_row_height)));
        return;
    }

    //the board never sets selection itself: it routes through the same entry point
    //the 3D scene uses, so there is one owner of the current plate
    m_plater->select_plate(index);
}

void PlateBoard::on_paint(wxPaintEvent &evt)
{
    wxAutoBufferedPaintDC dc(this);
    const bool            dark  = wxGetApp().dark_mode();
    const int             width = GetClientSize().GetWidth();

    dc.SetBrush(wxBrush(board_bg(dark)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(GetClientRect());

    const std::vector<PlateBoardRow> &rows = m_model.rows();
    int                               y    = 0;

    //rollup: hidden entirely at one plate, where every cell would restate the row below
    if (rows.size() > 1) {
        const PlateBoardRollup &rollup = m_model.rollup();
        dc.SetFont(Label::Body_10);
        //the rollup is the PROJECT scope's destination, so it says when it is the thing
        //the inspector is describing
        if (m_scope_project) {
            dc.SetBrush(wxBrush(board_sel(dark)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, width, m_rollup_height);
        }
        dc.SetTextForeground(m_scope_project ? board_fg(dark) : board_dim(dark));

        wxString text = wxString::Format(_L("%d plates   %d machines   %s total   %s longest queue"),
                                         rollup.plates, rollup.machines,
                                         format_hours(rollup.total_seconds),
                                         format_hours(rollup.longest_queue_seconds));
        if (rollup.not_estimated > 0)
            text += wxString::Format("   %s", wxString::Format(_L("%d not estimated"), rollup.not_estimated));

        dc.DrawText(text, FromDIP(8), FromDIP(6));
        y += m_rollup_height;
    }

    auto draw_row = [&](const PlateBoardRow &row, int row_y, bool selected, bool hovered, bool scoped) {
        if (selected) {
            dc.SetBrush(wxBrush(board_sel(dark)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, row_y, width, m_row_height);
        } else if (scoped) {
            //in the scope set but not the current plate: the inspector is describing it,
            //the 3D scene is not showing it, and the row says both
            dc.SetBrush(wxBrush(board_scope(dark)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, row_y, width, m_row_height);
        } else if (hovered) {
            dc.SetBrush(wxBrush(board_hover(dark)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, row_y, width, m_row_height);
        }

        //1-based, because that is the numbering the rest of the app uses and the one
        //every filename the prep pipeline writes is keyed on
        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(board_fg(dark));
        dc.DrawText(wxString::Format("%d", row.plate_index + 1), FromDIP(8), row_y + FromDIP(9));

        //bed glyph: proportional, so a smaller machine is visibly a smaller bed. This
        //is the board's only geometric signal; it does not draw plates. The 3D scene
        //is the scene, and a second one inside a narrow column is a worse copy of it.
        const wxSize glyph = bed_glyph_size(this, row.bed_w, row.bed_d);
        dc.SetBrush(wxBrush(board_dim(dark)));
        dc.SetPen(wxPen(board_line(dark)));
        dc.DrawRectangle(FromDIP(26), row_y + (m_row_height - glyph.GetHeight()) / 2, glyph.GetWidth(), glyph.GetHeight());

        //an inherited row is greyed and shows the project printer it is following;
        //an assigned row is in normal weight. The distinction comes from
        //has_printer_assignment() and from nowhere else, so a project loaded from a
        //3MF that already carries assignments shows them.
        dc.SetTextForeground(row.assigned ? board_fg(dark) : board_dim(dark));
        if (row.preset_missing)
            dc.SetTextForeground(board_warn(dark));

        wxString name = from_u8(row.printer_name);
        if (row.preset_missing)
            name += _L(" (not installed)");

        const int name_x   = FromDIP(56);
        const int name_max = width - name_x - FromDIP(76);
        dc.SetFont(Label::Body_12);
        dc.DrawText(wxControl::Ellipsize(name, dc, wxELLIPSIZE_END, name_max), name_x, row_y + FromDIP(9));

        //hours from this plate's own retained slice, and an en-dash when it has none
        dc.SetFont(Label::Body_10);
        dc.SetTextForeground(board_dim(dark));
        const wxString hours = format_hours(row.has_time ? row.print_time_seconds : 0.f);
        dc.DrawText(hours, width - FromDIP(72), row_y + FromDIP(11));

        dc.DrawText(wxString::Format("%d", row.part_count), width - FromDIP(28), row_y + FromDIP(11));

        //the state column carries only what the file knows: no reachability, no
        //online dot. Orca's device layer is singular, so at most one row could ever
        //be honest about a machine being up.
        if (row.parts_outside) {
            dc.SetTextForeground(board_warn(dark));
            dc.DrawText("!", width - FromDIP(14), row_y + FromDIP(11));
        } else if (row.sliced) {
            dc.SetTextForeground(board_dim(dark));
            dc.DrawText(wxString::FromUTF8("\xe2\x9c\x93"), width - FromDIP(14), row_y + FromDIP(11));
        }
    };

    for (int i = m_scroll_rows; i < (int) rows.size() && y < GetClientSize().GetHeight(); ++i, y += m_row_height) {
        const bool scoped = std::find(m_scoped_plates.begin(), m_scoped_plates.end(), rows[i].plate_index) !=
                            m_scoped_plates.end();
        draw_row(rows[i], y, rows[i].plate_index == m_current_plate, m_hover == rows[i].plate_index, scoped);
    }
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

        dc.SetFont(Label::Body_10);
        for (const std::pair<int, std::string> &slot : m_slots) {
            if (x + box + FromDIP(18) > GetClientSize().GetWidth())
                break;

            //an unparsable or absent colour draws hollow. Substituting a plausible one
            //would put a filament on screen that the library does not contain.
            const wxColour colour = slot.second.empty() ? wxColour() : wxColour(from_u8(slot.second));
            if (colour.IsOk())
                dc.SetBrush(wxBrush(colour));
            else
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(board_line(dark)));
            dc.DrawRectangle(x, y, box, box);
            x += box + FromDIP(2);

            //the slot number rides beside the swatch: slots are per-printer, and the
            //number is the only part of this that the G-code agrees with
            dc.SetTextForeground(board_dim(dark));
            dc.DrawText(wxString::Format("%d", slot.first), x, y);
            x += dc.GetTextExtent(wxString::Format("%d", slot.first)).GetWidth() + FromDIP(8);
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

    m_header = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(22)));
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
        label->SetFont(Label::Body_10);
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

    wxFlexGridSizer *grid = new wxFlexGridSizer(0, 2, FromDIP(4), FromDIP(8));
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
    m_body_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(6));
    m_body_sizer->Add(m_more_btn, 0, wxLEFT | wxBOTTOM, FromDIP(6));
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
    dc.SetFont(Label::Body_10);
    const wxSize extent = dc.GetTextExtent(m_badge_text);
    const int    pad    = m_header->FromDIP(6);
    const int    height = m_header->FromDIP(16);
    const int    top    = std::max(0, (m_header->GetClientSize().GetHeight() - height) / 2);

    dc.SetBrush(wxBrush(board_sel(dark)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(m_header->FromDIP(6), top, extent.GetWidth() + 2 * pad, height,
                            m_header->FromDIP(3));
    dc.SetTextForeground(board_fg(dark));
    dc.DrawText(m_badge_text, m_header->FromDIP(6) + pad, top + (height - extent.GetHeight()) / 2);

    dc.SetTextForeground(board_dim(dark));
    const wxString chevron = m_expanded ? wxString::FromUTF8("\xe2\x8c\x84") : wxString::FromUTF8("\xe2\x8c\x83");
    dc.DrawText(chevron, m_header->GetClientSize().GetWidth() - m_header->FromDIP(18), top);
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
    std::vector<std::string> colours;
    const ConfigOptionStrings *colour_opt =
        wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (colour_opt != nullptr)
        colours = colour_opt->values;

    std::vector<int> slots;
    if (project) {
        //PROJECT scope shows the library itself, not any plate's usage of it
        for (size_t i = 0; i < colours.size(); ++i)
            slots.push_back((int) i + 1);
    } else {
        //the union across the selection, in slot order
        std::set<int> used;
        for (const PlateBoardRow *row : scope)
            used.insert(row->filament_slots.begin(), row->filament_slots.end());
        slots.assign(used.begin(), used.end());
    }

    std::vector<std::pair<int, std::string>> swatches;
    for (int slot : slots)
        swatches.emplace_back(slot, slot >= 1 && (size_t) slot <= colours.size()
                                        ? colours[(size_t) slot - 1]
                                        : std::string());
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
