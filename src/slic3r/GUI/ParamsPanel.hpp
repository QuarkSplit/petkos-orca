#ifndef slic3r_params_panel_hpp_
#define slic3r_params_panel_hpp_


#include <map>
#include <vector>
#include <memory>


#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/tglbtn.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/button.h>
#include <wx/timer.h>
#include <wx/wupdlock.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/scrolwin.h>
#include <wx/panel.h>

#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"

class ModeSwitchButton;
class SwitchButton;
class MultiSwitchButton;
class StaticBox;

namespace Slic3r {
namespace GUI {

///////////////////////////////////////////////////////////////////////////

class TipsDialog : public DPIDialog
{
private:
    bool m_show_again{false};
    std::string m_app_key;

public:
    TipsDialog(wxWindow *parent, const wxString &title, const wxString &description, std::string app_key = "", long style = wxOK, std::map<wxStandardID,wxString> option_map={});
    Button *m_confirm{nullptr};
    Button *m_cancel{nullptr};
    wxPanel *m_top_line{nullptr};
    wxStaticText *m_msg;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
    wxBoxSizer *create_item_checkbox(wxString title, wxWindow *parent, wxString tooltip, std::string param);
    Button* add_button(wxWindowID btn_id, const wxString &label, bool set_focus = false);
};

///////////////////////////////////////////////////////////////////////////////
/// Class ParamsPanel
///////////////////////////////////////////////////////////////////////////////
class ParamsPanel : public wxPanel
{
#if __WXOSX__
    wxWindow*            m_tmp_panel;
    int                 m_size_move = -1;
#endif // __WXOSX__

	private:
        void free_sizers();
        void delete_subwindows();
        void refresh_tabs();

	protected:
        wxBoxSizer* m_top_sizer { nullptr };
        wxBoxSizer* m_left_sizer { nullptr };
        wxBoxSizer* m_mode_sizer { nullptr };
        // // BBS: new layout
        StaticBox* m_top_panel{ nullptr };
        ScalableButton* m_process_icon{ nullptr };
        wxStaticText* m_title_label { nullptr };
        MultiSwitchButton* m_mode_region { nullptr };
        ScalableButton *m_tips_arrow{nullptr};
        bool m_tips_arror_blink{false};
        ScalableButton* m_mode_icon { nullptr }; // ORCA
        ModeSwitchButton* m_mode_view { nullptr };
        //wxBitmapButton* m_search_button { nullptr };
        wxStaticLine* m_staticline_print { nullptr };
        //wxBoxSizer* m_print_sizer { nullptr };
        wxPanel* m_tab_print { nullptr };
        wxPanel* m_tab_print_plate { nullptr };
        wxPanel* m_tab_print_object { nullptr };
        wxStaticLine* m_staticline_print_object { nullptr };
        wxPanel* m_tab_print_part { nullptr };
        wxPanel* m_tab_print_layer { nullptr };
        wxStaticLine* m_staticline_print_part { nullptr };
        wxStaticLine* m_staticline_filament { nullptr };
        //wxBoxSizer* m_filament_sizer { nullptr };
        wxPanel* m_tab_filament { nullptr };
        wxStaticLine* m_staticline_printer { nullptr };
        //wxBoxSizer* m_printer_sizer { nullptr };
        wxPanel* m_tab_printer { nullptr };
        //wxStaticLine* m_staticline_buttons { nullptr };
        // BBS: new layout
        wxBoxSizer* m_button_sizer { nullptr };
        wxWindow* m_export_to_file { nullptr };
        wxWindow* m_import_from_file { nullptr };
        //wxStaticLine* m_staticline_middle{ nullptr };
        //wxBoxSizer* m_right_sizer { nullptr };
        wxScrolledWindow* m_page_view { nullptr };
        wxBoxSizer* m_page_sizer { nullptr };

        ScalableButton*		m_setting_btn { nullptr };
        ScalableButton*		m_search_btn { nullptr };
        ScalableButton*		m_compare_btn { nullptr };

        wxBitmap m_toggle_on_icon;
        wxBitmap m_toggle_off_icon;

        wxPanel* m_current_tab { nullptr };

        bool m_has_object_config { false };

        struct Highlighter
        {
            void set_timer_owner(wxEvtHandler *owner, int timerid = wxID_ANY);
            void init(std::pair<wxWindow *, bool *>, wxWindow *parent = nullptr);
            void blink();
            void invalidate();

        private:
            wxWindow *      m_bitmap{nullptr};
            bool *         m_show_blink_ptr{nullptr};
            int            m_blink_counter{0};
            wxTimer        m_timer;
            wxWindow *      m_parent { nullptr };
        } m_highlighter;

        void OnToggled(wxCommandEvent& event);

	public:
		ParamsPanel( wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxSize( 1800,1080 ), long style = wxTAB_TRAVERSAL, const wxString& type = wxEmptyString );
		~ParamsPanel();

        void rebuild_panels();
        void create_layout();
        //clear the right page
        void clear_page();
        void OnActivate();
        void set_active_tab(wxPanel*tab);
        bool is_active_and_shown_tab(wxPanel*tab);
        void update_mode();
        void msw_rescale();
        //PetkosOrca: the three layers a process value can come from, in the order they are
        //applied. The panel used to offer the outer and the inner one and nothing between, so
        //the plate - the thing that actually owns a printer and gets sliced - had no settings
        //surface at all. Left to right is broad to narrow, which is also the layering order:
        //the project's preset, then this plate's overrides, then the selected objects'.
        enum Scope { ScopeGlobal = 0, ScopePlate = 1, ScopeObjects = 2 };

        void switch_to_global();
        void switch_to_plate();
        void switch_to_object(bool with_tips = false);
        //Point the plate tab at whichever plate is current. Called on entering the Plate scope
        //and whenever the current plate changes while that scope is showing, because a scope
        //that keeps describing the plate you navigated away from is worse than none.
        void bind_plate_scope();
        //Refresh the "this scope holds something" markers on the switch.
        void update_scope_markers();
        //The current plate changed. Re-point the Plate scope and refresh the markers.
        void on_plate_selection_changed();

        void notify_object_config_changed();
        void switch_to_object_if_has_object_configs();

        StaticBox* get_top_panel() { return m_top_panel; }

        wxPanel* filament_panel() { return m_tab_filament; }

        wxScrolledWindow* get_paged_view() { return m_page_view;}
        wxPanel*    get_current_tab() { return m_current_tab; }

        //PetkosOrca: THE ACTIVE SETTINGS PAGE BELONGS TO THE USER, NOT TO THE PRESET SYSTEM.
        //
        //Selecting a preset reloads its tab, which rebuilds that tab's page list, which moves
        //that list's selection, which - through Tab::tree_sel_change_delayed - promotes the tab
        //to the shown one. That chain is correct when the user clicked a page. It is wrong when
        //the selection moved because the editing cursor followed the plate: the user clicked a
        //PLATE, was editing process settings, and the Printer page arrives in front of them.
        //Every plate click then costs a click back, and the only thing gained is a highlight the
        //board row already shows.
        //
        //So a cursor move pins the panel: whatever it reloads, the page on screen stays where
        //the user put it. Explicitly choosing a printer is not a cursor move and still brings
        //its page forward. Held as a COUNTER for the same reason Tab::PlateWriteSuspend is - a
        //preset selection can pump a nested modal loop, and a nested scope's exit must restore
        //rather than clear.
        class ActiveTabPin
        {
        public:
            ActiveTabPin();
            ~ActiveTabPin();
            ActiveTabPin(const ActiveTabPin &) = delete;
            ActiveTabPin &operator=(const ActiveTabPin &) = delete;
        };
        //True only while pinned AND there is already a page on screen to keep. With no current
        //tab there is nothing to protect and the first promotion has to be allowed through, or
        //the panel would open empty.
        bool is_active_tab_pinned() const { return s_active_tab_pin_depth > 0 && m_current_tab != nullptr; }

    private:
        static int s_active_tab_pin_depth;
};

} // GUI
} // Slic3r

#endif //slic3r_params_panel_hpp_
