#ifndef slic3r_GUI_PlateBoard_hpp_
#define slic3r_GUI_PlateBoard_hpp_

#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/scrolwin.h>

#include "Widgets/PopupWindow.hpp"

namespace Slic3r {

class PresetBundle;

namespace GUI {

class PartPlateList;
class Plater;

//The Project row. It is not a plate, so it cannot use a plate index, and it is not
//absent either, so it cannot use "no row". -1 is the index the sidebar and the board
//both use for "the project".
static constexpr int PLATE_BOARD_PROJECT_ROW = -1;

//One row's worth of facts, all of them read from the plate list and the preset
//bundle. Nothing here is derived from a plate's name: the prep pipeline writes
//machine, colour and hours into plate names as free text, and an overflow run
//repeats its parent's estimate, so parsing names double-counts.
struct PlateBoardRow
{
    int  plate_index = PLATE_BOARD_PROJECT_ROW;
    //what the row shows: the plate's own printer when it has one, otherwise the
    //project printer it is following
    std::string printer_name;
    bool assigned       = false; //has_printer_assignment()
    bool preset_missing = false; //assigned, but this installation has no such preset

    double bed_w = 0.; //mm, from the resolved preset, or the project bed when inherited
    double bed_d = 0.;

    bool  sliced             = false; //a retained slice this plate's context still matches
    bool  has_time           = false;
    float print_time_seconds = 0.f;
    double weight_grams      = 0.;

    int  part_count    = 0;
    bool parts_outside = false; //instance_outside_set is not empty

    std::vector<int> filament_slots; //1-based library slots this plate's instances use
};

//The four rollup cells, plus the count that stops the totals being a lie by omission.
struct PlateBoardRollup
{
    int   plates                = 0;
    int   machines              = 0;
    float total_seconds         = 0.f;
    float longest_queue_seconds = 0.f;
    int   not_estimated         = 0;
};

//A pure function of the plate list plus the preset bundle. Holds no wx types and no
//state of its own beyond the last result, so it can be exercised without a GUI.
//Resolves bed sizes from the preset collection directly rather than through
//PartPlateList::resolve_printer_bed, which bails when there is no wxApp instance.
class PlateBoardModel
{
public:
    void rebuild(const PartPlateList &plates, const PresetBundle &bundle);

    const std::vector<PlateBoardRow> &rows() const { return m_rows; }
    const PlateBoardRow &             project_row() const { return m_project_row; }
    const PlateBoardRollup &          rollup() const { return m_rollup; }

    //The collapsed printer-section title: the project printer when the project holds
    //one plate or every plate inherits, and "N machines" otherwise.
    std::string summary_text() const;

    //W x D of a printer preset's printable area, in mm. False when the preset is not
    //installed or carries no usable printable_area.
    static bool printer_bed_size(const PresetBundle &bundle, const std::string &preset_name, double &w, double &d);

private:
    std::vector<PlateBoardRow> m_rows;
    PlateBoardRow              m_project_row;
    PlateBoardRollup           m_rollup;
    bool                       m_all_inherited = true;
};

//The picker. Deliberately NOT a PlaterPresetComboBox subclass: every preset combo in
//this application mutates the global bundle, and PlaterPresetComboBox::update() has no
//argument saying which preset to display, so it would tick the globally selected
//printer rather than this plate's. A plain list inherits nothing and so leaks nothing.
class PlatePrinterPopup : public PopupWindow
{
public:
    //plate_index is the plate being assigned. current_name is its stored assignment,
    //which may name a preset this installation does not have; that entry is offered
    //back verbatim so opening the picker can never discard it.
    PlatePrinterPopup(wxWindow *parent, Plater *plater, int plate_index, const std::string &current_name);

    void Popup(wxWindow *focus = nullptr) override;

private:
    struct Item
    {
        std::string name;        //empty means "Same as Global"
        wxString    label;
        wxString    detail;
        bool        is_header = false;
        bool        keep_missing = false;
        bool        is_bulk_toggle = false;
    };

    void build_items(const std::string &current_name);
    void on_paint(wxPaintEvent &evt);
    void on_mouse(wxMouseEvent &evt);
    int  hit_test(const wxPoint &pos) const;

    Plater *          m_plater = nullptr;
    int               m_plate_index = PLATE_BOARD_PROJECT_ROW;
    std::vector<Item> m_items;
    int               m_hover = -1;
    int               m_row_height = 0;
    //Plates with no assignment of their own. The bulk footer names how many, because
    //an action whose blast radius is larger than what the user can see is the silent
    //write this whole design exists to end.
    std::vector<int>  m_unassigned_plates;
    bool              m_bulk_to_unassigned = false;
};

//The board itself: the rollup, then one scrolling row per plate.
//
//The Project row is the sidebar's existing printer-preset panel sitting directly
//above this control, not a row drawn here. That panel already IS the project printer
//combo with global semantics, so reparenting it into a custom-drawn row would buy a
//pixel and cost the widget. What matters is the guarantee, which holds either way:
//global editing is a place you point at, pinned above the board and never scrolling
//away, rather than a mode inferred from what was last clicked.
//
//The board stores no selection. PartPlateList::m_current_plate owns the current
//plate; the board renders it and routes every click back through Plater::select_plate.
class PlateBoard : public wxPanel
{
public:
    PlateBoard(wxWindow *parent, Plater *plater);

    //Recompute from the plate list. Cheap by design and not cached: the plate list is
    //mutated by arrange, 3MF load, undo and the plate list itself, so a cache goes
    //stale exactly when it matters, and MAX_PLATE_COUNT bounds the work at 36 rows.
    void reload();

    //Called by the sidebar's selection sink. Idempotent.
    void on_plate_selection_changed(int current_plate);

    std::string summary_text() const { return m_model.summary_text(); }

    //Height the board wants for the rows it currently holds, clamped so it can never
    //push the controls below it off a narrow sidebar.
    wxSize DoGetBestSize() const override;

private:
    void on_paint(wxPaintEvent &evt);
    void on_mouse(wxMouseEvent &evt);
    void on_scroll(wxMouseEvent &evt);
    int  hit_test(const wxPoint &pos) const;
    void open_picker(int plate_index);

    Plater *           m_plater = nullptr;
    PlatePrinterPopup *m_popup  = nullptr;
    PlateBoardModel    m_model;
    int             m_current_plate = 0;
    int             m_hover        = -2;
    int             m_scroll_rows  = 0; //first visible plate row
    int             m_row_height   = 0;
    int             m_rollup_height = 0;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PlateBoard_hpp_
