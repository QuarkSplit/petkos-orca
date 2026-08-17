#ifndef slic3r_GUI_PodPrinterPicker_hpp_
#define slic3r_GUI_PodPrinterPicker_hpp_

#include <string>
#include <vector>

#include <wx/dialog.h>
#include <wx/scrolwin.h>

#include "GUI_Utils.hpp"

class TextInput;
class Button;

namespace Slic3r {

class PresetBundle;

namespace GUI {

//Podslicer: the printer picker.
//
//It replaces the inherited web guide's printer page, which answered this question by
//re-reading 12,277 preset JSON files (22.7 MB) from disk on one thread, serialising the
//result to a 1.85 MB JavaScript string and handing that to WebView2 - all to list 386
//machines the application had already parsed into memory at startup.
//
//This reads PresetBundle::vendors, which holds every vendor's models and nozzle variants
//from that startup load, so building the whole list touches no file at all.
//
//It is also shaped for a print farm rather than a first-run wizard: the machines already
//installed are the subject of the screen, adding one is a search, and nothing else (region,
//filaments, privacy) is asked about on the way.
class PrinterPickerDialog : public DPIDialog
{
public:
    explicit PrinterPickerDialog(wxWindow *parent);
    ~PrinterPickerDialog() override = default;

    //Number of models the index holds, for the driver's report.
    size_t model_count() const { return m_models.size(); }
    //Installs one variant without any UI interaction, so a headless run can add a printer
    //through exactly the path a click takes. Returns false when the model or variant is not
    //in the index; the caller reports that rather than silently doing nothing.
    bool install_headless(const std::string &vendor, const std::string &model, const std::string &nozzle);
    //Same for removal, so an add can be undone by the same route it arrived through.
    bool uninstall_headless(const std::string &vendor, const std::string &model);
    //Writes the staged selection through PresetBundle::apply_vendor_config. Public so a
    //headless run commits the same way the Apply button does.
    bool commit();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Variant
    {
        std::string nozzle;
        bool        installed = false;  //staged, not yet committed
        bool        was       = false;  //as found in AppConfig, for the dirty check
    };

    struct Model
    {
        std::string          vendor;
        //The key AppConfig is written under: VendorProfile::PrinterModel::id, which is the
        //vendor JSON's machine_model_list entry name. `name` is the model file's own name and
        //is for reading only - the two are usually equal and nothing may assume it.
        std::string          id;
        std::string          name;
        std::string          family;
        std::string          cover;     //absolute path to the cover PNG, may not exist
        std::vector<Variant> variants;
        std::string          search;    //lowercased "vendor name family", built once

        bool installed() const;
        bool changed() const;
    };

    //One drawn line in the list: either a section/vendor heading or a model row.
    struct Row
    {
        enum class Kind { SectionFleet, SectionResults, Vendor, Printer, Empty } kind;
        int    model = -1;   //index into m_models for Kind::Printer
        wxString text;       //heading text
        int    y = 0, h = 0; //laid out in refresh_rows()
        //Fleet rows stand alone, so they carry their brand; result rows sit under a vendor
        //heading that already says it.
        bool   in_fleet = false;
    };

    void build_index();
    void refresh_rows();
    void relayout_and_refresh();

    void on_paint(wxPaintEvent &);
    void on_mouse(wxMouseEvent &);
    void on_search(const wxString &term);

    //Hit test: which row, and which nozzle chip or action glyph inside it.
    struct Hit
    {
        int  row     = -1;
        int  variant = -1;   //index into the model's variants when a chip was hit
        bool action  = false;//the row's add/remove glyph
    };
    Hit hit_test(const wxPoint &pos) const;

    void draw_row(wxDC &dc, const Row &row, bool hovered, const Hit &hover_hit);
    const wxBitmap *cover_for(const Model &model, int px);

    //Geometry, all in scaled pixels, recomputed on DPI change by measure_geometry().
    //
    //m_chip_w lives here rather than being measured in both the painter and the hit tester,
    //because two independent measurements of the same chip is how a custom-drawn list ends up
    //with a click that lands next to the thing it looks like it hit.
    void measure_geometry();
    int m_row_h    = 0;
    int m_head_h   = 0;
    int m_pad      = 0;
    int m_thumb    = 0;
    int m_chip_h   = 0;
    int m_chip_w   = 0;
    int m_chip_gap = 0;

    std::vector<Model> m_models;
    std::vector<Row>   m_rows;
    std::string        m_term;         //lowercased search term
    Hit                m_hover;

    wxScrolledWindow *m_list   = nullptr;
    TextInput        *m_search = nullptr;
    wxStaticText     *m_count  = nullptr;
    Button           *m_apply  = nullptr;

    //Colours resolved once per theme, because a paint handler asking the theme per row is
    //the sort of thing that turns a fast list back into a slow one.
    struct Palette
    {
        wxColour bg, text, dim, accent, accent_dim, chip_off, chip_on, sep, hover;
    } m_pal;
    void resolve_palette();
};

//The one entry point the rest of the app uses. Runs the dialog modally and returns true when
//the installed set changed, so the caller knows whether to refresh its combo boxes.
bool pod_pick_printers(wxWindow *parent);

//Podslicer: saving a project without a file explorer.
//
//The library is a folder tree (`E:\3D-Printing\Projects` by default, `pod_library_dir` in the
//app config) whose groups either carry a numbered pipeline - 01-source, 02-needs-colour,
//03-prepped ... - or are flat. That convention is READ OFF DISK rather than compiled in, so
//adding a stage folder in Explorer is an instruction rather than a desync, exactly as the
//pipeline's own doc says about its manifest.
//
//Returns the absolute path to save to, or:
//  empty        the user cancelled
//  "<browse>"   the user asked for the ordinary file dialog instead
//
//`current_path` is the project's existing file if it has one, so the dialog can open on the
//group and stage it already lives in rather than on a guess.
wxString pod_save_to_library(wxWindow *parent, const wxString &suggested_name,
                             const wxString &current_path);

//True when `path` sits inside the library tree, which is what decides whether a plain Save
//needs to ask anything at all: a project already filed just gets written where it is.
bool pod_path_in_library(const wxString &path);

//Refreshes the library index after a save, so a project saved into the library is findable in
//it. The tree is the truth and the index is a cache; this is the cache's invalidation path,
//which is the thing it was missing - `ingest.py` already applies the same rule after filing a
//dropped model, and this reuses that one writer rather than adding a second.
void pod_library_note_saved(const wxString &path);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PodPrinterPicker_hpp_
