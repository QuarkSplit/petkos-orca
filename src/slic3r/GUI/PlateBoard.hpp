#ifndef slic3r_GUI_PlateBoard_hpp_
#define slic3r_GUI_PlateBoard_hpp_

#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/dc.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include "Widgets/PopupWindow.hpp"
#include "wxExtensions.hpp"

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
//The row's hover thumbnail. Same reason: a paint handler and a placement rule, and only
//the board ever names one.
class PlateThumbnailPreview;

//The Project row. It is not a plate, so it cannot use a plate index, and it is not
//absent either, so it cannot use "no row". -1 is the index the sidebar and the board
//both use for "no plate", which is the scope of a selection that is not exactly one.
static constexpr int PLATE_BOARD_NO_PLATE = -1;

//A group holding more than this many plates draws its rows compact. Above eight rows the
//eye stops doing the grouping for itself, which is the same threshold the default
//grouping switches on, and for the same reason.
static constexpr int PLATE_BOARD_COMPACT_ABOVE = 8;

//A machine group holding more than this many plates draws them as TILES instead of rows.
//
//A row is 64 px and says everything about one plate; four of them fit, so a thirty-six
//plate project shows a ninth of itself and every row under a machine header repeats the
//same arrow and the same machine picture. Under a header that already names the machine a
//plate does not need to say which machine it is on - it is INSIDE the machine. Containment
//states the fork's sentence once and permanently, where an arrow per row states it
//thirty-six times, and a five-machine project stops drawing the same picture as a
//one-machine project.
//
//Four is the threshold because below it the rich row is worth its height: the eye still
//reads the rows one at a time. Above it the eye scans, and scanning wants tiles.
static constexpr int PLATE_BOARD_TILE_ABOVE = 4;

//How the board files its rows. A VIEW mode, not the frame: it changes nothing about any
//plate, and it is per-project session state that is never inherited from the previously
//open project, because a large project would otherwise leave a small one grouped.
enum class PlateBoardGrouping
{
    PlateOrder = 0, //one flat list in plate-index order
    ByMachine  = 1, //one group per printer preset name; every plate names one
    ByCapacity = 2, //the same groups, sorted by descending queue hours
    ByMaterial = 3  //(colour, type) of the dominant slot, machine sub-groups nested
};

//One row's worth of facts, all of them read from the plate list and the preset
//bundle. Nothing here is derived from a plate's name: the prep pipeline writes
//machine, colour and hours into plate names as free text, and an overflow run
//repeats its parent's estimate, so parsing names double-counts.
struct PlateBoardRow
{
    int  plate_index = PLATE_BOARD_NO_PLATE;
    //the plate's own printer; every plate has one
    std::string printer_name;
    //the plate's own process and its dispatch target. Carried so the inspector can offer
    //them without a second read of the plate list.
    std::string process_name;
    std::string device_id;
    //How many process settings the plate carries on top of the process it names. A plate
    //translated from another machine has these, and a row showing only the name would be
    //describing half of what it slices with.
    size_t      process_overrides = 0;
    //the plate's own name when the user gave it one; under a header that already names
    //the machine this is the most useful thing a row can say about itself
    std::string plate_name;
    //the machine's model identity ("Creality K2 Pro"): the short display name under the
    //row's printer picture, and the key its cover image is found by. Empty when the
    //preset is missing or declares no model.
    std::string printer_model;
    bool preset_missing = false; //this installation has no such printer preset

    //THIS PLATE'S CONTEXT DOES NOT COMPOSE, and that is a different fact from the one above.
    //preset_missing is a lookup - is this printer installed. A plate can name an installed
    //printer and still not compose: the process it names may not run there, one of its
    //filaments may not, or it may name no filament at all. The two want different repairs,
    //so the board says which one this is instead of reporting both as "a problem".
    bool        unresolved = false;
    std::string unresolved_reason; //verbatim from the composer, for the hover note

    double bed_w = 0.; //mm, from the resolved preset
    double bed_d = 0.;

    bool  sliced             = false; //a retained slice this plate's context still matches
    //A retained slice the plate's own context no longer matches: the G-code is still
    //attached, it is simply not current. Distinct from "never sliced", which shows
    //nothing at all, because the two want different repairs.
    bool  stale              = false;
    //A retained slice that was DROPPED while the project was loading, carrying the reason
    //the loader gave. It is neither of the two above: nothing is attached to inspect, so it
    //is not stale, and the plate has not been sliced in this session, so it is not sliced
    //either. Empty means nothing was dropped, which is the normal case.
    std::string dropped_reason;
    bool  has_time           = false;
    float print_time_seconds = 0.f;
    double weight_grams      = 0.;

    int  part_count    = 0;
    bool parts_outside = false; //instance_outside_set is not empty

    std::vector<int>         filament_slots;   //1-based library slots this plate's instances use
    std::vector<std::string> filament_colours; //parallel; empty where the library is shorter

    //The slot the material grouping files this plate under: most grams from the plate's
    //own slice, and the lowest used slot when there is no slice to ask. 0 means the plate
    //references no slot at all, which is its own group rather than a guess.
    int         dominant_slot   = 0;
    std::string material_type;   //filament_type of the dominant slot's preset
    std::string material_colour; //library colour of the dominant slot

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

//A run of rows under one header. Groups are held flat and in display order with a depth,
//rather than as a tree: the only nesting the design has is material > machine, and a flat
//list with a depth renders, hit-tests and collapses in one pass instead of three.
struct PlateBoardGroup
{
    //Stable identity, so a collapsed group stays collapsed across a rebuild. It carries
    //the grouping mode, because the same machine name means a different group under a
    //different mode and a shared key would carry collapse state across the change.
    std::string key;
    std::string caption;       //what the header says
    std::string detail;        //bed size for a machine group; empty otherwise
    std::string swatch_colour; //material groups draw their colour; empty means none

    //The preset name a drop on this header assigns, carried as data. It is NOT recoverable
    //from anything else the group holds: key is the mode prefix plus the name, and caption
    //is a translated string whenever the name is empty, so a drag that parsed either would
    //be a second, weaker answer to a question the group already knows. Empty is the group
    //of plates that name no printer at all - an unresolved plate, not an inherited one -
    //and dropping there writes an empty name, which is how that state is reached on purpose.
    std::string machine;
    //Whether a plate may be dropped here. Material groups are false: they key on a colour,
    //which is not something a plate can be assigned to.
    bool        drop_target = false;

    int   depth         = 0;    //0 top level, 1 a machine sub-group inside a material group
    int   plate_count   = 0;    //including descendants, so a material group can state its total
    float queue_seconds = 0.f;  //same
    //True when the header already names one machine, which is what lets a compact row
    //spend the width the name would have taken on its swatches instead.
    bool  names_machine = false;

    //Indices into PlateBoardModel::rows(). Empty for a material group, whose rows live in
    //its machine sub-groups.
    std::vector<int> rows;
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
    void rebuild(const PartPlateList &plates,
                 const PresetBundle & bundle,
                 PlateBoardGrouping   grouping = PlateBoardGrouping::PlateOrder);

    //Recompute ONE plate's row and re-derive everything that follows from the row set.
    //Assigning a printer to a plate changes one row, and rebuilding all of them to show
    //that costs a whole-config composition per plate - O(plates) work for an O(1) change,
    //which is what made the click grow with the size of the project.
    //
    //False when the model cannot safely be patched: a row count that no longer matches the
    //plate list, or an index whose row is not the plate it claims. The caller then does a
    //full rebuild, so this is an optimisation that can decline rather than a second source
    //of truth that can drift.
    [[nodiscard]] bool refresh_plate(int                  plate_index,
                                     const PartPlateList &plates,
                                     const PresetBundle & bundle,
                                     PlateBoardGrouping   grouping);

    const std::vector<PlateBoardRow> &  rows() const { return m_rows; }
    //Empty in plate order: one unnamed group holding everything is a header that says
    //nothing, and drawing it would cost a row of height to state the obvious.
    const std::vector<PlateBoardGroup> &groups() const { return m_groups; }
    const PlateBoardRollup &            rollup() const { return m_rollup; }

    //The largest bed dimension anywhere in the project, floored, which is what the row
    //glyphs are drawn in proportion to. See the note on draw_bed_plan_in in the .cpp.
    double glyph_reference_mm() const { return m_glyph_reference_mm; }

    //The collapsed printer-section title: the machine's name when the project is on one
    //machine, and "N machines" otherwise.
    std::string summary_text() const;

    //Plate order at eight plates or fewer, by machine above eight. Above eight the eye
    //cannot do the grouping the panel refused to do; at or below it, reordering rows away
    //from plate numbering costs more than it buys, because the number is the project's
    //shared vocabulary and every prep-pipeline filename is keyed on it.
    static PlateBoardGrouping default_grouping(int plate_count);

    //W x D of a printer preset's printable area, in mm. False when the preset is not
    //installed or carries no usable printable_area.
    static bool printer_bed_size(const PresetBundle &bundle, const std::string &preset_name, double &w, double &d);

    //First nozzle diameter of a printer preset, in mm. False when the preset is not
    //installed or does not carry the key. The option pointer is checked rather than
    //trusted: DynamicConfig::opt_float returns a reference to a temporary when the key
    //is absent, so an absent key is a crash here and not a zero.
    static bool printer_nozzle_diameter(const PresetBundle &bundle, const std::string &preset_name, double &diameter);

private:
    void build_groups(PlateBoardGrouping grouping, const PresetBundle &bundle);

    //One row's worth of reading. Split out of rebuild's loop so the targeted refresh above
    //and the full rebuild cannot describe a row differently.
    void build_row(int                             plate_index,
                   const PartPlateList &           plates,
                   const PresetBundle &            bundle,
                   PlateBoardRow &                 row) const;

    //Everything that is a pure function of the row set: the rollup totals, the glyph
    //reference and the grouping. Derived from m_rows rather than accumulated during the
    //loop, which is what lets one changed row produce correct totals without revisiting
    //the other thirty-five.
    void finalise(PlateBoardGrouping grouping, const PresetBundle &bundle);

    std::vector<PlateBoardRow>   m_rows;
    std::vector<PlateBoardGroup> m_groups;
    PlateBoardRollup             m_rollup;
    double                       m_glyph_reference_mm = 0.;
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
    ~PlatePrinterPopup() override;

    void Popup(wxWindow *focus = nullptr) override;

private:
    struct Item
    {
        std::string name;        //empty means "Same as Global"
        wxString    label;
        wxString    detail;
        double      bed_w = 0.;  //drives the mini bed glyph; 0 means the preset has no usable bed
        double      bed_d = 0.;
        //This candidate's bed is smaller than the bed the plate is on now, in at least one
        //axis. Stated as the fact it is, rather than as a count of parts that would fall
        //outside: that count needs a geometry pass against a build volume this plate does
        //not have yet, and a number produced by a cheaper test would be a guess wearing a
        //number's clothes. The row state names the parts once the assignment is committed.
        bool        smaller_bed = false;
        //HOW MANY PLATES OF THIS PROJECT ARE ALREADY ON THIS MACHINE, and whether one of them
        //is the plate being assigned. The whole question at this popup is same-or-different, and
        //it used to be answerable only by closing it and reading the board: the count was
        //appended to the right-hand detail text, next to the bed size, where the eye is not. It
        //is a column of its own beside the name now, and this plate's own machine is marked in
        //the accent, so "where am I, and where is everything else" is one glance.
        //Counted once when the popup is built, which is O(plates) - never per paint.
        int         plates_here = 0;
        bool        is_current_machine = false;
        bool        is_header = false;
        bool        keep_missing = false;
        bool        is_bulk_toggle = false;  //"every unassigned plate"
        bool        is_scope_toggle = false; //"the N selected plates"
        bool        is_back = false;         //the variant step's way home
        //A MACHINE entry: one row per printer model, never one per nozzle. Picking it
        //resolves the nozzle the way the mechanism resolves every dependent: the plate's
        //current variant when the model carries it, the model's only variant when there
        //is one, and a second step naming the variants only when the question is real.
        std::vector<std::string> variant_presets; //exact preset names, aligned with labels
        std::vector<wxString>    variant_labels;
    };

    void build_items(const std::string &current_name);
    void build_variant_items(const Item &model_item);
    void commit(const std::string &preset_name);
    void on_paint(wxPaintEvent &evt);
    void on_mouse(wxMouseEvent &evt);
    void on_wheel(wxMouseEvent &evt);
    void on_app_activate(wxActivateEvent &evt);
    int  hit_test(const wxPoint &pos) const;
    void fit_height();

    int         m_scroll = 0;          //pixels; the list scrolls when taller than the popup
    std::string m_current_name;        //the plate's stored assignment, for rebuilds
    std::string m_current_variant;     //its nozzle variant: the value that carries over
    //How many of this project's plates sit on each printer PRESET, counted once when the popup
    //is built. Kept rather than folded straight into the rows because the nozzle step rebuilds
    //the list from one machine's variants and has to answer the same question per variant; a
    //second walk of the plates there would make the cost depend on how the user navigates.
    std::map<std::string, int> m_plate_counts;
    std::string m_current_machine;     //the preset this plate names right now

    Plater *          m_plater = nullptr;
    int               m_plate_index = PLATE_BOARD_NO_PLATE;
    std::vector<Item> m_items;
    int               m_hover = -1;
    int               m_row_height = 0;
    double            m_glyph_reference_mm = 0.;
    //The other plates already on this plate's machine. The bulk footer names how many,
    //because an action whose blast radius is larger than what the user can see is the
    //silent write this whole design exists to end.
    std::vector<int>  m_sibling_plates;
    bool              m_bulk_to_siblings = false;
    //The scope set, which is what the user has selected on the board. The two bulk
    //modifiers are mutually exclusive: each names a different visible set, and a pick
    //that meant both would have a blast radius nothing on screen states.
    std::vector<int>  m_scoped_plates;
    bool              m_bulk_to_scope = false;
};

//The board itself: the rollup tiles, the grouping control, then the scrolling rows.
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

    //Reload after ONE plate changed. Same result as reload() for that case, without
    //re-reading every other row - each of which costs a whole-config composition. Falls
    //back to a full reload() whenever the model declines to be patched, so a caller
    //never has to know which case it is in.
    void reload_plate(int plate_index);

    //Called by the sidebar's selection sink. Idempotent.
    void on_plate_selection_changed(int current_plate);
    //The rows the board has already computed. Published so nothing else has to build a
    //second copy of them: a rebuild is O(plates) whole-config compositions, and the
    //inspector used to pay that a second time for a model it only ever read.
    const PlateBoardModel &model() const { return m_model; }

    //The scope set, for rendering only. Sidebar::m_scoped_plates owns it, exactly as
    //PartPlateList::m_current_plate owns the current plate; the board stores neither and
    //renders both. It always names at least the current plate: the kind of scope that
    //named none described the project's printer, and no such thing exists.
    void set_scope(const std::vector<int> &scoped_plates);

    //Open the printer picker for one plate, anchored at a screen point. Public because
    //the inspector's printer chip is the same affordance as the row's and must not grow
    //a second write path to reach it.
    void open_picker(int plate_index, const wxPoint &screen_anchor);

    std::string summary_text() const { return m_model.summary_text(); }

    //Height the board wants for the rows it currently holds, clamped so it can never
    //push the controls below it off a narrow sidebar.
    wxSize DoGetBestSize() const override;

private:
    enum class HitKind { None, Rollup, Segment, GroupHeader, Row };
    struct Hit
    {
        HitKind kind  = HitKind::None;
        int     index = -1; //segment ordinal, group index, or row index - never a plate index
        bool operator==(const Hit &other) const { return kind == other.kind && index == other.index; }
        bool operator!=(const Hit &other) const { return !(*this == other); }
    };

    //One drawable line in the scroll region, laid out once per rebuild. Group headers and
    //rows share the list so that scrolling, hit-testing and "draw only what is in view"
    //are one arithmetic problem rather than three.
    struct Item
    {
        bool header = false;
        int  group  = -1; //index into m_model.groups() when header
        int  row    = -1; //index into m_model.rows()   when not
        int  y      = 0;  //content-space top
        int  height = 0;

        //A TILE BAND: one line of small plate tiles belonging to one machine group. See
        //PLATE_BOARD_TILE_ABOVE for why a large machine group draws these instead of rows.
        //
        //tile_first indexes group.rows; the band holds tile_count of them across tile_cols
        //columns. tile_count is 0 on a header and on an ordinary row, and it is what every
        //consumer tests: a band carries row == -1, so anything that resolves a plate by
        //scanning for item.row has to go through find_row_item instead.
        int tile_first = -1;
        int tile_count = 0;
        int tile_cols  = 0;
    };

    //The bed glyph animates between two sizes so a reassignment that resizes a bed is
    //visible rather than instantaneous. One clock for the whole board: the rows all move
    //together, and a per-row clock would buy nothing but drift.
    struct GlyphAnim
    {
        double from_w = 0., from_h = 0.;
        double to_w   = 0., to_h   = 0.;
    };

    void on_paint(wxPaintEvent &evt);
    void on_mouse(wxMouseEvent &evt);
    void on_left_down(wxMouseEvent &evt);
    void on_capture_lost(wxMouseCaptureLostEvent &evt);
    void on_scroll(wxMouseEvent &evt);
    void on_anim_tick(wxTimerEvent &evt);
    void on_hover_tick(wxTimerEvent &evt);

    //The hover thumbnail, which is what the in-canvas plate strip used to be the only place
    //to see. Shown after a dwell so that merely crossing the board does not fire it.
    void show_row_preview(int row_index);
    void hide_row_preview();

    //Drag a row onto a group header to reassign its machine. Never onto another row, and
    //never a reorder: plate index is load-bearing in every filename the prep pipeline
    //writes, so a drop changes the machine and nothing else.
    bool             grouping_allows_drag() const;
    int              drop_group_at(const wxPoint &pos) const;
    std::vector<int> drag_targets() const;
    void             end_drag(bool commit);
    void             draw_drag_pill(wxDC &dc, bool dark);
    //-1 up, +1 down, 0 not near an edge. Read from the pointer, applied by the clock.
    int              autoscroll_direction(const wxPoint &pos) const;
    void             update_autoscroll(const wxPoint &pos);
    void             on_autoscroll_tick(wxTimerEvent &evt);
    //Answers a drag attempted in a grouping that has nothing to drop onto.
    void             say_where_drag_works();

    void rebuild_items();
    void clamp_scroll();
    void sync_glyph_targets();
    void scroll_row_into_view(int row_index);
    int  find_row_item(int plate_index) const;

    int  rollup_height() const;
    int  grouping_height() const;
    int  view_top() const { return rollup_height() + grouping_height(); }
    int  view_height() const;
    int  sticky_group() const;
    int  segment_at(int x) const;
    Hit  hit_test(const wxPoint &pos) const;

    //ONE geometry for a tile band, asked for rather than recomputed. The row's own hit rects
    //are already a second computation of its paint and already disagree with it by 2 and 16
    //px; a band is drawn and hit-tested from these two calls so it cannot repeat that.
    void tile_metrics(int &tile, int &gap, int &inset) const;
    int  tile_columns(int width) const;

    void draw_rollup(wxDC &dc, bool dark, int width);
    void draw_grouping(wxDC &dc, bool dark, int width, int top);
    void draw_group_header(wxDC &dc, bool dark, int width, int y, int height, const PlateBoardGroup &group, bool collapsed);
    void draw_row(wxDC &dc, bool dark, int width, int y, int height, int row_index, const PlateBoardGroup *group);
    //One plate as a small tile: its bed in plan, tinted by the state it is in, its number in
    //the middle and a corner mark for what there is to dispatch.
    void draw_plate_tile(wxDC &dc, const wxRect &cell, int row_index, bool dark, const std::vector<int> &dragged) const;
    void draw_tile_band(wxDC &dc, bool dark, int width, int y, const Item &item, const PlateBoardGroup &group,
                        const std::vector<int> &dragged) const;
    //Which plate a point in a tile band names, as an index into m_model.rows(), or -1. The
    //gap between two tiles belongs to neither plate and is not a hit on either.
    int  tile_at(const Item &item, int band_y, const wxPoint &pos) const;
    //The slim right-edge thumb. Drawn whenever the content is taller than the viewport,
    //because a list that scrolls with nothing saying so is a list whose remaining rows do
    //not exist as far as the user is concerned.
    void draw_scroll_thumb(wxDC &dc, bool dark, int width, int top, int visible);
    void draw_swatches(wxDC &dc, bool dark, const PlateBoardRow &row, int x, int y, int box, int max_width);
    void draw_state_icon(wxDC &dc, const PlateBoardRow &row, int x, int y);

    void set_grouping(PlateBoardGrouping grouping);

    Plater *           m_plater = nullptr;
    PlatePrinterPopup *m_popup  = nullptr;
    PlateBoardModel    m_model;

    std::vector<Item>      m_items;
    std::vector<GlyphAnim> m_glyphs; //indexed by row index, so it is parallel to m_model.rows()
    std::set<std::string>  m_collapsed;

    int  m_current_plate  = 0;
    Hit  m_hover;
    int  m_scroll_px      = 0;
    int  m_content_height = 0;
    //The shortest item currently laid out. The board grows to a whole number of items and
    //then scrolls, and with rows, headers and bands all in one list the unit of "whole" is
    //not the row height any more. Zero when there is nothing laid out.
    int  m_item_quantum   = 0;
    int  m_row_height     = 0;
    int  m_row_compact    = 0;
    int  m_header_height  = 0;
    int  m_rollup_height  = 0;
    int  m_segment_height = 0;
    int  m_best_height    = 0; //last height handed to the sizer; a change asks for one layout

    //The grouping mode, and whether the user picked it. While it is not explicit the
    //default is recomputed from the plate count on every reload, so a project that grows
    //past eight plates regroups itself without being asked twice.
    PlateBoardGrouping m_grouping          = PlateBoardGrouping::PlateOrder;
    bool               m_grouping_explicit = false;
    //Identity of the project the explicit choice belongs to. A different project resets
    //the mode and the collapse set: a large project must not leave a small one grouped.
    wxString           m_project_id;
    bool               m_project_seen = false;

    wxTimer  m_anim_timer;
    int      m_anim_elapsed_ms = 0;
    bool     m_anim_running    = false;

    //The hover preview and the dwell that gates it. One window for the board's lifetime,
    //hidden rather than destroyed: a popup rebuilt per hover leaves one dead window behind
    //per row crossed.
    PlateThumbnailPreview *m_preview = nullptr;
    wxTimer                m_hover_timer;

    //Drag state. m_drag_armed is a press that has not yet moved far enough to be a drag, so
    //a plain click still selects; m_dragging is the captured drag itself.
    bool    m_drag_armed = false;
    bool    m_dragging   = false;
    wxPoint m_press_pos;
    wxPoint m_drag_pos;
    int     m_drag_plate = PLATE_BOARD_NO_PLATE;
    int     m_drop_group = -1; //index into m_model.groups(), -1 when the cursor is over none

    //The edge auto-scroll clock. Separate from the hover clock, which the drag suppresses.
    wxTimer m_autoscroll_timer;
    int     m_autoscroll_dir = 0;

    //A press on a row in a grouping that has no machine groups to drop onto. Tracked so that
    //trying to drag there answers with where the action lives instead of with nothing; said
    //once per grouping mode, because a sentence repeated on every attempt is noise.
    bool m_drag_attempt     = false;
    bool m_drag_hint_shown  = false;

    ScalableBitmap m_icon_sliced;  //a retained slice this plate's context still matches
    ScalableBitmap m_icon_stale;   //a retained slice the context has moved out from under
    ScalableBitmap m_icon_problem; //parts outside the bed, a missing preset, or no context
    ScalableBitmap m_icon_dropped; //a retained slice the loader could not vouch for and dropped

    //the row's plate render, scaled once per thumbnail generation: keyed by the pixel
    //buffer's address+size, which changes exactly when the thumbnail is re-rendered
    struct ThumbCacheEntry { const void *pixels = nullptr; size_t size = 0; wxBitmap bmp; };
    std::map<int, ThumbCacheEntry> m_thumb_cache;
    //The plate whose thumbnail this paint wants re-rendered, or -1. The render itself is
    //a 512x512 offscreen pass and a readback - it happens AFTER the paint, never inside
    //it, because a click that invalidates a thumbnail would otherwise wait 44 ms for
    //pixels before anything at all appeared. Still one per paint, so a 36-plate project
    //heals over a second of frames rather than stalling one.
    int  m_thumb_heal_wanted  = -1;
    //A canvas that cannot render right now must not become a repaint spin. Set when a heal
    //produced nothing; cleared whenever the board reloads, which is when new work arrives.
    bool m_thumb_heal_blocked = false;
    //the row's printer picture, scaled once per model: the vendor cover art the wizard
    //uses, so the board and the wizard agree on what a machine looks like
    std::map<std::string, wxBitmap> m_cover_cache;
    //Whether the board belongs on screen at all is a property of the plate list, not of
    //whichever caller happened to reload last. It lives here because THREE paths reload the
    //board - the sidebar's refresh, its targeted sibling, and on_plate_selection_changed -
    //and only one of them used to decide this. A board holding 36 rows could therefore stay
    //hidden for a whole session, and did. Deciding it as part of reloading is what makes
    //that unforgettable rather than remembered in one place out of three.
    //
    //True when the visibility actually changed, which is also when the parent was laid out.
    bool apply_visibility(int plate_count);

    const wxBitmap *plate_thumb_bitmap(int plate_index, int px);
    const wxBitmap *printer_cover_bitmap(const std::string &model, int px);

    //inline rename of a plate, entered by a single click on the row's caption. Commit on
    //Enter or focus loss, abandon on Escape; both routes end at Plater::rename_plate.
    wxTextCtrl *m_rename_edit  = nullptr;
    int         m_rename_plate = -1;
    //A bed in plan inside a square cell, scaled against the project's largest. See the
    //definition: it is what the empty picture cells say instead of nothing.
    void draw_bed_plan(wxDC &dc, const wxRect &cell, double bed_w, double bed_d, bool dark) const;

    void begin_rename(int plate_index, const wxRect &rect);
    void commit_rename(bool apply);
    bool           m_icons_ok = false;

    //render copies of the sidebar's scope set; see set_scope
    std::vector<int> m_scoped_plates;
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
//Shared by the inspector swatches and the assigned-material rows above the spool pool.
void show_plate_filament_menu(wxWindow *parent, Plater *plater, int plate_index, int slot);

class PlateInspector : public wxPanel
{
public:
    PlateInspector(wxWindow *parent, Plater *plater);

    //scoped_plates is Sidebar::m_scoped_plates verbatim. Safe to call before the plater
    //finishes constructing: it checks Plater::is_initialized() and draws nothing.
    void reload(const PlateBoardModel &model, const std::vector<int> &scoped_plates);

private:
    void build_rows();
    void rebuild_bed_type_choices(int plate_index, int current_bed_type);
    void on_bed_type_selected();
    void on_more_plate_settings();
    void open_picker();
    //the nozzle switch: a native menu of the plate's machine's variants, committing the
    //sibling preset through the one write path. This is where a changed nozzle is looked
    //for; nothing ever asks unprompted.
    void on_nozzle_click();
    //Every plate-context field is written from the row that shows it, through the Plater
    //write path that owns that field's consequences. One idiom - click the value, get a
    //menu of what will resolve - so a new field costs a row and nothing else.
    void on_process_click();
    void on_filament_slot_click(int slot);
    void on_device_click();
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
    wxStaticText *m_process_label = nullptr;
    wxStaticText *m_process_value = nullptr;
    wxStaticText *    m_filament_label = nullptr;
    PlateSwatchStrip *m_filament_value = nullptr;
    wxStaticText *m_mapping_label  = nullptr;
    wxStaticText *m_mapping_value  = nullptr;
    //Which physical machine this plate is dispatched to. Separate from the printer preset
    //on purpose: several machines can share one slicing preset, and one machine can change
    //nozzle. Empty is a real answer - a plate slices without a dispatch target.
    wxStaticText *m_device_label = nullptr;
    wxStaticText *m_device_value = nullptr;
    Button *      m_more_btn       = nullptr;

    PlatePrinterPopup *m_popup = nullptr;

    //Bed types behind the combo, offset by one because entry 0 is the inheritance row.
    //Ints rather than BedType for the same reason as PlateBoardRow::bed_type.
    std::vector<int> m_bed_type_values;

    wxString m_badge_text;
    //The plate the editable rows write to. PLATE_BOARD_NO_PLATE whenever the scope is
    //not exactly one plate, and every editable row is hidden in that case: an editor that
    //writes to a plate the badge does not name is the silent write this design ends.
    int  m_plate_index = PLATE_BOARD_NO_PLATE;
    //Slot numbers behind the swatches, in draw order, so a click knows which of the
    //plate's filament slots it landed on.
    std::vector<int> m_filament_slots;
    bool m_expanded    = true;
    //true while reload() is populating the combo, so the selection event it raises is not
    //mistaken for the user picking a bed type
    bool m_syncing     = false;
    //Which rows were visible last time: 1 PLATE, 2 N PLATES, -1 never drawn.
    //Only a change here can change the inspector's height, so only a change here asks the
    //sidebar to lay itself out again. reload() runs on every preset update, and a
    //parent-wide layout on each of those is a storm, not a refresh.
    int  m_last_shape  = -1;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PlateBoard_hpp_
