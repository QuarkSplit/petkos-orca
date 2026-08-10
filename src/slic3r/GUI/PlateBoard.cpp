#include "PlateBoard.hpp"

#include <algorithm>
#include <map>
#include <set>

#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/sizer.h>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
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

void PlateBoardModel::rebuild(const PartPlateList &plates, const PresetBundle &bundle)
{
    m_rows.clear();
    m_rollup = PlateBoardRollup();

    const std::string project_printer = bundle.printers.get_selected_preset_name();

    m_project_row               = PlateBoardRow();
    m_project_row.plate_index   = PLATE_BOARD_PROJECT_ROW;
    m_project_row.printer_name  = project_printer;
    printer_bed_size(bundle, project_printer, m_project_row.bed_w, m_project_row.bed_d);

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

PlatePrinterPopup::PlatePrinterPopup(wxWindow *parent, Plater *plater, int plate_index, const std::string &current_name)
    : PopupWindow(parent, wxBORDER_SIMPLE), m_plater(plater), m_plate_index(plate_index)
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
            Refresh();
            return;
        }

        const std::string chosen = m_items[index].name;
        Plater *          plater = m_plater;
        const int         plate  = m_plate_index;

        std::vector<int> targets;
        if (m_bulk_to_unassigned) {
            targets = m_unassigned_plates;
            if (std::find(targets.begin(), targets.end(), plate) == targets.end())
                targets.push_back(plate);
        }

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

        if (item.is_bulk_toggle) {
            dc.SetPen(wxPen(board_line(dark)));
            dc.DrawLine(FromDIP(6), y, width - FromDIP(6), y);
            dc.SetFont(Label::Body_11);
            dc.SetTextForeground(m_bulk_to_unassigned ? board_fg(dark) : board_dim(dark));
            dc.DrawText(wxString(m_bulk_to_unassigned ? wxString::FromUTF8("\xe2\x98\x91  ")
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

void PlateBoard::open_picker(int plate_index)
{
    const PartPlate *plate = m_plater->get_partplate_list().get_plate(plate_index);
    if (plate == nullptr)
        return;

    //one popup at a time, destroyed rather than left parented to the board, or every
    //pick leaves a hidden window behind for the lifetime of the sidebar
    if (m_popup != nullptr) {
        m_popup->Destroy();
        m_popup = nullptr;
    }

    m_popup = new PlatePrinterPopup(this, m_plater, plate_index, plate->get_printer_preset_name());
    const int     top    = (int) m_model.rows().size() > 1 ? m_rollup_height : 0;
    const wxPoint anchor = ClientToScreen(wxPoint(0, top + (plate_index - m_scroll_rows + 1) * m_row_height));
    m_popup->Position(anchor, wxSize(0, 0));
    m_popup->Popup();
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

    if (evt.GetEventType() != wxEVT_LEFT_UP || index < 0)
        return;

    //the printer chip opens the picker; anywhere else on the row selects the plate
    const int chip_x = FromDIP(56);
    const int chip_w = GetClientSize().GetWidth() - chip_x - FromDIP(76);
    if (evt.GetPosition().x >= chip_x && evt.GetPosition().x < chip_x + chip_w) {
        open_picker(index);
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
        dc.SetTextForeground(board_dim(dark));

        wxString text = wxString::Format(_L("%d plates   %d machines   %s total   %s longest queue"),
                                         rollup.plates, rollup.machines,
                                         format_hours(rollup.total_seconds),
                                         format_hours(rollup.longest_queue_seconds));
        if (rollup.not_estimated > 0)
            text += wxString::Format("   %s", wxString::Format(_L("%d not estimated"), rollup.not_estimated));

        dc.DrawText(text, FromDIP(8), FromDIP(6));
        y += m_rollup_height;
    }

    auto draw_row = [&](const PlateBoardRow &row, int row_y, bool selected, bool hovered) {
        if (selected) {
            dc.SetBrush(wxBrush(board_sel(dark)));
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

    for (int i = m_scroll_rows; i < (int) rows.size() && y < GetClientSize().GetHeight(); ++i, y += m_row_height)
        draw_row(rows[i], y, rows[i].plate_index == m_current_plate, m_hover == rows[i].plate_index);
}

}} // namespace Slic3r::GUI
