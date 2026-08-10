#ifndef slic3r_GUI_PlateBoard_hpp_
#define slic3r_GUI_PlateBoard_hpp_

#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "Widgets/PopupWindow.hpp"

//Both live at global scope, like every other widget in Widgets/. Forward-declared so
//this header does not drag DropDown and StaticBox into everything that includes it.
//They are plain classes with no dll-export decoration, so a bare declaration is exact.
class ComboBox;
class Button;

namespace Slic3r {

class PresetBundle;

namespace GUI {

class PartPlateList;
class Plater;
//The filament-usage swatch strip. Defined entirely in the .cpp: it is a paint handler
//and nothing else, and nothing outside the inspector has a reason to name its type.
class PlateSwatchStrip;

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

    //Nozzle is a property of the PRESET, never of the plate: a plate stores one printer
    //string and nothing else. It is carried here so the inspector can show it read-only
    //and say which preset it came from. 0 means the preset is not installed or names no
    //nozzle_diameter, which is reported rather than guessed at.
    double nozzle_diameter = 0.;

    //BedType as int, because this header is deliberately free of libslic3r/PrintConfig.
    //0 is btDefault, which is the plate saying "Same as Global Plate Type" - the plate's
    //own value, not the project's resolved one.
    int bed_type = 0;
    //FilamentMapMode as int, same reason. Per-plate and read-only in the inspector.
    int filament_map_mode = 0;
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

    //First nozzle diameter of a printer preset, in mm. False when the preset is not
    //installed or does not carry the key. The option pointer is checked rather than
    //trusted: DynamicConfig::opt_float returns a reference to a temporary when the key
    //is absent, so an absent key is a crash here and not a zero.
    static bool printer_nozzle_diameter(const PresetBundle &bundle, const std::string &preset_name, double &diameter);

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
    //back verbatim so opening the picker can never discard it. scoped_plates is the
    //sidebar's scope set, and produces the "Assign to the N selected plates" footer;
    //it is only ever the set the user can see selected on the board.
    PlatePrinterPopup(wxWindow *          parent,
                      Plater *           plater,
                      int                plate_index,
                      const std::string &current_name,
                      const std::vector<int> &scoped_plates = std::vector<int>());

    void Popup(wxWindow *focus = nullptr) override;

private:
    struct Item
    {
        std::string name;        //empty means "Same as Global"
        wxString    label;
        wxString    detail;
        bool        is_header = false;
        bool        keep_missing = false;
        bool        is_bulk_toggle = false;  //"every unassigned plate"
        bool        is_scope_toggle = false; //"the N selected plates"
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
    //The scope set, which is what the user has selected on the board. The two bulk
    //modifiers are mutually exclusive: each names a different visible set, and a pick
    //that meant both would have a blast radius nothing on screen states.
    std::vector<int>  m_scoped_plates;
    bool              m_bulk_to_scope = false;
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

    //The scope set, for rendering only. Sidebar::m_scoped_plates owns it, exactly as
    //PartPlateList::m_current_plate owns the current plate; the board stores neither and
    //renders both. project_scope is a separate kind, not an index.
    void set_scope(const std::vector<int> &scoped_plates, bool project_scope);

    //Open the printer picker for one plate, anchored at a screen point. Public because
    //the inspector's printer chip is the same affordance as the row's and must not grow
    //a second write path to reach it.
    void open_picker(int plate_index, const wxPoint &screen_anchor);

    std::string summary_text() const { return m_model.summary_text(); }

    //Height the board wants for the rows it currently holds, clamped so it can never
    //push the controls below it off a narrow sidebar.
    wxSize DoGetBestSize() const override;

private:
    void on_paint(wxPaintEvent &evt);
    void on_mouse(wxMouseEvent &evt);
    void on_scroll(wxMouseEvent &evt);
    int  hit_test(const wxPoint &pos) const;

    Plater *           m_plater = nullptr;
    PlatePrinterPopup *m_popup  = nullptr;
    PlateBoardModel    m_model;
    int             m_current_plate = 0;
    int             m_hover        = -2;
    int             m_scroll_rows  = 0; //first visible plate row
    int             m_row_height   = 0;
    int             m_rollup_height = 0;
    //render copies of the sidebar's scope set; see set_scope
    std::vector<int> m_scoped_plates;
    bool             m_scope_project = false;
};

//The pinned inspector. It sits below the board and above the extruder/AMS groups, so a
//growing board never costs it its place: the real failure mode of a linear list in a
//narrow column is not that the list gets long, it is that it pushes everything under it
//off the screen.
//
//Three scopes, and the scope is STATED in a badge rather than inferred from what was
//last clicked. PLATE nn is one plate, N PLATES is a modifier-click selection, PROJECT is
//a separate kind and not an index.
//
//Mixed fields carry no revert affordance. Revert is per-plate only, because a revert
//whose effect the user cannot see is a control with an invisible blast radius.
class PlateInspector : public wxPanel
{
public:
    PlateInspector(wxWindow *parent, Plater *plater);

    //scoped_plates is Sidebar::m_scoped_plates verbatim. Safe to call before the plater
    //finishes constructing: it checks Plater::is_initialized() and draws nothing.
    void reload(const std::vector<int> &scoped_plates, bool project_scope);

private:
    void build_rows();
    void rebuild_bed_type_choices(int plate_index, int current_bed_type);
    void on_bed_type_selected();
    void on_more_plate_settings();
    void open_picker();
    void set_expanded(bool expanded);
    void on_header_paint(wxPaintEvent &evt);

    Plater *    m_plater = nullptr;

    wxPanel *   m_header = nullptr;
    wxPanel *   m_body   = nullptr;
    wxBoxSizer *m_body_sizer = nullptr;

    wxStaticText *m_printer_label  = nullptr;
    wxStaticText *m_printer_value  = nullptr;
    wxStaticText *m_nozzle_label   = nullptr;
    wxStaticText *m_nozzle_value   = nullptr;
    wxStaticText *m_bed_label      = nullptr;
    ComboBox *    m_bed_choice     = nullptr; //PLATE scope only: the one editable row
    wxStaticText *m_bed_value      = nullptr; //every other scope: read-only, no revert
    wxStaticText *    m_filament_label = nullptr;
    PlateSwatchStrip *m_filament_value = nullptr;
    wxStaticText *m_mapping_label  = nullptr;
    wxStaticText *m_mapping_value  = nullptr;
    Button *      m_more_btn       = nullptr;

    PlatePrinterPopup *m_popup = nullptr;

    //Bed types behind the combo, offset by one because entry 0 is the inheritance row.
    //Ints rather than BedType for the same reason as PlateBoardRow::bed_type.
    std::vector<int> m_bed_type_values;

    wxString m_badge_text;
    //The plate the editable rows write to. PLATE_BOARD_PROJECT_ROW whenever the scope is
    //not exactly one plate, and every editable row is hidden in that case: an editor that
    //writes to a plate the badge does not name is the silent write this design ends.
    int  m_plate_index = PLATE_BOARD_PROJECT_ROW;
    bool m_expanded    = true;
    //true while reload() is populating the combo, so the selection event it raises is not
    //mistaken for the user picking a bed type
    bool m_syncing     = false;
    //Which rows were visible last time: 0 PROJECT, 1 PLATE, 2 N PLATES, -1 never drawn.
    //Only a change here can change the inspector's height, so only a change here asks the
    //sidebar to lay itself out again. reload() runs on every preset update, and a
    //parent-wide layout on each of those is a storm, not a refresh.
    int  m_last_shape  = -1;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PlateBoard_hpp_
