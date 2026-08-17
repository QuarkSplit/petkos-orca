#include "PodPrinterPicker.hpp"

#include <algorithm>
#include <cctype>
#include <map>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/choice.h>
#include <wx/control.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
//A removal has to know which plates are standing on the machine being removed.
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "PetkosPerf.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/PodBanner.hpp"
#include "Widgets/TextInput.hpp"
#include "wxExtensions.hpp"

namespace Slic3r { namespace GUI {

using nlohmann::json;

// ---------------------------------------------------------------------------- helpers

bool PrinterPickerDialog::Model::installed() const
{
    return std::any_of(variants.begin(), variants.end(), [](const Variant &v) { return v.installed; });
}

bool PrinterPickerDialog::Model::changed() const
{
    return std::any_of(variants.begin(), variants.end(), [](const Variant &v) { return v.installed != v.was; });
}

//Search is by whole-word prefix on any of vendor / model / family, so "kobra x" finds
//"Anycubic Kobra X" and "any kob" finds it too, while "obra" does not - a substring match
//over 386 names turns a specific query into a list to read.
static bool term_matches(const std::string &haystack, const std::string &term)
{
    if (term.empty())
        return true;
    std::vector<std::string> words;
    boost::split(words, term, boost::is_space(), boost::token_compress_on);
    for (const std::string &word : words) {
        if (word.empty())
            continue;
        bool found = false;
        for (size_t i = 0; i <= haystack.size() - std::min(haystack.size(), word.size()); ++i) {
            //a word may start at the string start or after a non-alphanumeric
            const bool at_boundary = i == 0 || !std::isalnum(static_cast<unsigned char>(haystack[i - 1]));
            if (at_boundary && haystack.compare(i, word.size(), word) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------- the index

//The catalogue of every machine the resources ship, cached for the life of the process.
//
//It cannot come from PresetBundle::vendors: that is loaded from the DATADIR's system folder,
//so it holds only the vendors already installed - seven of the sixty-six. The catalogue has to
//be read from resources/profiles, and the whole point of this dialog is HOW MUCH of it.
//
//LoadVendorOnly reads each vendor's machine_model_list and each model's own file, and stops
//there: 452 files and 1.6 MB, against the web guide's 12,277 files and 22.7 MB, because the
//guide also opened every filament and every process preset - 10,324 files answering a question
//the printer list does not ask.
static const VendorMap &shipped_catalogue()
{
    static VendorMap  catalogue;
    static bool       loaded = false;
    if (!loaded) {
        loaded = true;
        PresetBundle temp;
        temp.load_system_models_from_json(ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
        catalogue = std::move(temp.vendors);
        //User-defined printers are machines too, and a picker that cannot see them would let
        //Apply delete them.
        if (PresetBundle *live = wxGetApp().preset_bundle; live != nullptr) {
            VendorProfile mine = live->get_custom_vendor_models();
            if (!mine.models.empty())
                catalogue[mine.name] = std::move(mine);
        }
    }
    return catalogue;
}

void PrinterPickerDialog::build_index()
{
    PETKOS_PERF_SCOPE_NAMED(idx, Perf::Probe::PickerIndexBuild, 0);

    const AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;

    const boost::filesystem::path profiles = boost::filesystem::path(resources_dir()) / "profiles";
    const VendorMap &catalogue = shipped_catalogue();

    m_models.clear();
    m_models.reserve(512);
    for (const auto &[vendor_name, vendor] : catalogue) {
        //The filament library is a vendor entry with no machines; it must not appear as a brand.
        if (vendor.models.empty())
            continue;
        for (const VendorProfile::PrinterModel &pm : vendor.models) {
            Model m;
            m.vendor = vendor_name;
            m.id     = pm.id;
            m.name   = pm.name.empty() ? pm.id : pm.name;
            m.family = pm.family;
            m.variants.reserve(pm.variants.size());
            for (const VendorProfile::PrinterVariant &v : pm.variants) {
                Variant var;
                var.nozzle    = v.name;
                var.was       = cfg->get_variant(vendor_name, pm.id, v.name);
                var.installed = var.was;
                m.variants.push_back(std::move(var));
            }
            if (m.variants.empty())
                continue;
            //Nozzles read as a size ladder, so they are ordered as one whatever the JSON said.
            std::sort(m.variants.begin(), m.variants.end(), [](const Variant &a, const Variant &b) {
                return atof(a.nozzle.c_str()) < atof(b.nozzle.c_str());
            });
            //Cover files are named after the machine_model_list entry, which is the id.
            m.cover  = (profiles / vendor_name / (m.id + "_cover.png")).make_preferred().string();
            m.search = boost::to_lower_copy(m.vendor + " " + m.id + " " + m.name + " " + m.family);
            m_models.push_back(std::move(m));
        }
    }

    //Anything the config says is installed but the catalogue does not describe gets an entry
    //anyway. Apply writes the whole vendor map, so a machine missing from the index would be
    //UNINSTALLED by a dialog the user only opened to add something - silently, and with the
    //presets removed underneath a plate that was using them.
    int adopted = 0;
    for (const auto &[vendor_name, models] : cfg->vendors()) {
        for (const auto &[model_id, variants] : models) {
            for (const std::string &nozzle : variants) {
                auto it = std::find_if(m_models.begin(), m_models.end(), [&](const Model &m) {
                    return m.vendor == vendor_name && m.id == model_id;
                });
                if (it == m_models.end()) {
                    Model m;
                    m.vendor = vendor_name;
                    m.id     = model_id;
                    m.name   = model_id;
                    m.cover  = (profiles / vendor_name / (model_id + "_cover.png")).make_preferred().string();
                    m.search = boost::to_lower_copy(vendor_name + " " + model_id);
                    m.variants.push_back(Variant{nozzle, true, true});
                    m_models.push_back(std::move(m));
                    ++adopted;
                    continue;
                }
                if (std::none_of(it->variants.begin(), it->variants.end(),
                                 [&](const Variant &v) { return v.nozzle == nozzle; })) {
                    it->variants.push_back(Variant{nozzle, true, true});
                    ++adopted;
                }
            }
        }
    }

    std::sort(m_models.begin(), m_models.end(), [](const Model &a, const Model &b) {
        if (a.vendor != b.vendor)
            return a.vendor < b.vendor;
        return a.name < b.name;
    });

    idx.set_aux(static_cast<int32_t>(m_models.size()));
    BOOST_LOG_TRIVIAL(info) << "PrinterPicker: index has " << m_models.size() << " models across "
                            << catalogue.size() << " vendors"
                            << (adopted > 0 ? ", " + std::to_string(adopted) + " adopted from the config" : "");
}

// ---------------------------------------------------------------------------- rows

void PrinterPickerDialog::refresh_rows()
{
    m_rows.clear();

    std::vector<int> fleet, matches;
    for (int i = 0; i < static_cast<int>(m_models.size()); ++i) {
        if (m_models[i].installed())
            fleet.push_back(i);
        else if (!m_term.empty() && term_matches(m_models[i].search, m_term))
            matches.push_back(i);
    }
    //A searching user means the search; an installed machine that matches stays in the fleet
    //block (it is already theirs) but the block is filtered so the answer is not buried.
    if (!m_term.empty()) {
        std::vector<int> kept;
        for (int i : fleet)
            if (term_matches(m_models[i].search, m_term))
                kept.push_back(i);
        fleet = std::move(kept);
    }

    Row head;
    head.kind = Row::Kind::SectionFleet;
    head.text = m_term.empty() ? wxString::Format(_L("My printers · %d"), static_cast<int>(fleet.size()))
                               : wxString::Format(_L("Installed, matching · %d"), static_cast<int>(fleet.size()));
    m_rows.push_back(head);
    if (fleet.empty()) {
        Row e;
        e.kind = Row::Kind::Empty;
        e.text = m_term.empty() ? _L("No printers installed yet — search below to add one.")
                                : _L("None of your printers match.");
        m_rows.push_back(e);
    }
    for (int i : fleet) {
        Row r;
        r.kind     = Row::Kind::Printer;
        r.model    = i;
        r.in_fleet = true;
        m_rows.push_back(r);
    }

    if (m_term.empty()) {
        //With no term the second block would be 386 rows of nothing anyone asked for, so it
        //says what to do instead. Browsing is still possible: a single letter lists a brand.
        Row head2;
        head2.kind = Row::Kind::SectionResults;
        head2.text = wxString::Format(_L("Add a printer — %d models from %d brands"),
                                      static_cast<int>(m_models.size()), [this] {
                                          std::vector<std::string> v;
                                          for (const Model &m : m_models)
                                              if (v.empty() || v.back() != m.vendor)
                                                  v.push_back(m.vendor);
                                          return static_cast<int>(v.size());
                                      }());
        m_rows.push_back(head2);
        Row e;
        e.kind = Row::Kind::Empty;
        e.text = _L("Type a brand or model above.");
        m_rows.push_back(e);
    } else {
        Row head2;
        head2.kind = Row::Kind::SectionResults;
        head2.text = wxString::Format(_L("Available · %d"), static_cast<int>(matches.size()));
        m_rows.push_back(head2);
        if (matches.empty()) {
            Row e;
            e.kind = Row::Kind::Empty;
            e.text = _L("No model matches that.");
            m_rows.push_back(e);
        }
        std::string last_vendor;
        for (int i : matches) {
            if (m_models[i].vendor != last_vendor) {
                last_vendor = m_models[i].vendor;
                Row v;
                v.kind = Row::Kind::Vendor;
                v.text = from_u8(last_vendor);
                m_rows.push_back(v);
            }
            Row r;
            r.kind  = Row::Kind::Printer;
            r.model = i;
            m_rows.push_back(r);
        }
    }

    //Lay the rows out once; the paint handler then only draws the visible slice.
    int y = 0;
    for (Row &r : m_rows) {
        r.y = y;
        r.h = r.kind == Row::Kind::Printer ? m_row_h : m_head_h;
        y += r.h;
    }
    m_list->SetVirtualSize(wxSize(-1, y));
    m_list->SetScrollRate(0, m_row_h / 4);
}

void PrinterPickerDialog::measure_geometry()
{
    const int em = wxGetApp().em_unit();
    m_pad      = em;
    m_row_h    = em * 4;
    m_head_h   = em * 5 / 2;
    m_thumb    = em * 3;
    m_chip_h   = em * 9 / 5;
    m_chip_gap = std::max(2, em / 3);
    //"0.00" is the widest nozzle label that occurs, and every chip is that width so a row of
    //them reads as a ladder rather than as ragged text.
    wxClientDC dc(this);
    dc.SetFont(Label::Body_14);
    m_chip_w = dc.GetTextExtent("0.00").GetWidth() + m_pad;
}

void PrinterPickerDialog::relayout_and_refresh()
{
    refresh_rows();
    int installed = 0;
    for (const Model &m : m_models)
        if (m.installed())
            ++installed;
    bool dirty = std::any_of(m_models.begin(), m_models.end(), [](const Model &m) { return m.changed(); });
    m_count->SetLabel(dirty ? wxString::Format(_L("%d printers — unsaved"), installed)
                            : wxString::Format(_L("%d printers"), installed));
    m_apply->Enable(dirty);
    m_list->Refresh();
}

// ---------------------------------------------------------------------------- painting

void PrinterPickerDialog::resolve_palette()
{
    const bool dark = wxGetApp().dark_mode();
    //The accent is Podslicer's muted arctic blue; tools/accent-sweep.ps1 is its authority.
    m_pal.accent     = dark ? wxColour(0x6B, 0x9D, 0xB8) : wxColour(0x4F, 0x87, 0xA5);
    m_pal.accent_dim = dark ? wxColour(0x3D, 0x6B, 0x85) : wxColour(0x8F, 0xD0, 0xEA);
    m_pal.bg         = dark ? wxColour(0x24, 0x24, 0x27) : wxColour(0xFF, 0xFF, 0xFF);
    m_pal.text       = dark ? wxColour(0xE5, 0xE5, 0xE7) : wxColour(0x1A, 0x1A, 0x1C);
    m_pal.dim        = dark ? wxColour(0x8C, 0x8C, 0x94) : wxColour(0x78, 0x78, 0x80);
    m_pal.chip_off   = dark ? wxColour(0x3A, 0x3A, 0x40) : wxColour(0xED, 0xED, 0xF0);
    m_pal.chip_on    = m_pal.accent;
    m_pal.sep        = dark ? wxColour(0x38, 0x38, 0x3D) : wxColour(0xE4, 0xE4, 0xE8);
    m_pal.hover      = dark ? wxColour(0x33, 0x33, 0x38) : wxColour(0xF2, 0xF5, 0xF7);
}

const wxBitmap *PrinterPickerDialog::cover_for(const Model &model, int px)
{
    //Covers are 11.1 MB across 386 models, so they are decoded only for rows that actually
    //get drawn, at the size they get drawn, and kept.
    static std::map<std::string, wxBitmap> cache;
    const std::string key = model.cover + "@" + std::to_string(px);
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second.IsOk() ? &it->second : nullptr;

    wxBitmap bmp;
    if (boost::filesystem::exists(model.cover)) {
        wxImage img;
        if (img.LoadFile(from_u8(model.cover), wxBITMAP_TYPE_PNG) && img.IsOk()) {
            const double s = std::min(double(px) / img.GetWidth(), double(px) / img.GetHeight());
            img = img.Scale(std::max(1, int(img.GetWidth() * s)), std::max(1, int(img.GetHeight() * s)),
                            wxIMAGE_QUALITY_HIGH);
            bmp = wxBitmap(img);
        }
    }
    auto &slot = cache.emplace(key, bmp).first->second;
    return slot.IsOk() ? &slot : nullptr;
}

//Chip geometry is derived in one place so hit-testing and painting cannot disagree about
//where a nozzle chip is - the classic way a custom-drawn list gets a dead click.
static wxRect chip_rect(const wxRect &row, int index, int chip_w, int chip_h, int gap, int right_inset)
{
    const int x = row.GetRight() - right_inset - (index + 1) * (chip_w + gap) + gap;
    return wxRect(x, row.y + (row.height - chip_h) / 2, chip_w, chip_h);
}

void PrinterPickerDialog::draw_row(wxDC &dc, const Row &row, bool hovered, const Hit &hover_hit)
{
    const int w = m_list->GetClientSize().GetWidth();
    wxRect    rect(0, row.y, w, row.h);

    if (row.kind == Row::Kind::SectionFleet || row.kind == Row::Kind::SectionResults) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_pal.bg));
        dc.DrawRectangle(rect);
        dc.SetFont(Label::Head_13);
        dc.SetTextForeground(m_pal.dim);
        wxString t = row.text.Upper();
        dc.DrawText(t, m_pad, rect.y + (rect.height - dc.GetCharHeight()) / 2);
        //A hairline under a heading is the whole separator budget; anything more competes
        //with the rows for attention and the rows are the content.
        dc.SetPen(wxPen(m_pal.sep));
        dc.DrawLine(m_pad, rect.GetBottom(), w - m_pad, rect.GetBottom());
        return;
    }

    if (row.kind == Row::Kind::Vendor) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_pal.bg));
        dc.DrawRectangle(rect);
        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(m_pal.dim);
        dc.DrawText(row.text, m_pad + m_thumb / 2, rect.y + (rect.height - dc.GetCharHeight()) / 2);
        return;
    }

    if (row.kind == Row::Kind::Empty) {
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(m_pal.bg));
        dc.DrawRectangle(rect);
        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(m_pal.dim);
        dc.DrawText(row.text, m_pad + m_thumb + m_pad, rect.y + (rect.height - dc.GetCharHeight()) / 2);
        return;
    }

    const Model &model = m_models[row.model];

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(hovered ? m_pal.hover : m_pal.bg));
    dc.DrawRectangle(rect);

    //A changed row carries an accent bar at its left edge: the one place the eye needs to
    //land to see what Apply is about to do.
    if (model.changed()) {
        dc.SetBrush(wxBrush(m_pal.accent));
        dc.DrawRectangle(wxRect(0, rect.y + 1, std::max(2, m_pad / 4), rect.height - 2));
    }

    int x = m_pad + m_pad / 2;
    if (const wxBitmap *bmp = cover_for(model, m_thumb)) {
        dc.DrawBitmap(*bmp, x + (m_thumb - bmp->GetWidth()) / 2,
                      rect.y + (rect.height - bmp->GetHeight()) / 2, true);
    } else {
        //No cover is not an error; the row still has to read as a machine.
        dc.SetBrush(wxBrush(m_pal.chip_off));
        dc.DrawRoundedRectangle(wxRect(x, rect.y + (rect.height - m_thumb) / 2, m_thumb, m_thumb), m_pad / 3);
    }
    x += m_thumb + m_pad;

    const int action_w    = m_row_h;
    const int chips_right = action_w + m_pad;

    dc.SetFont(Label::Body_14);
    const int name_limit = m_list->GetClientSize().GetWidth() - chips_right
                           - static_cast<int>(model.variants.size()) * (m_chip_w + m_chip_gap) - x - m_pad;
    wxString name = from_u8(model.name);
    //Under a vendor heading, "Anycubic Kobra X" says the brand twice and the second time costs
    //width. A fleet row has no heading above it, so it keeps its brand.
    if (!row.in_fleet && name.StartsWith(from_u8(model.vendor) + " "))
        name = name.Mid(model.vendor.size() + 1);
    dc.SetTextForeground(m_pal.text);
    wxString shown = name;
    if (name_limit > 0)
        shown = wxControl::Ellipsize(name, dc, wxELLIPSIZE_END, name_limit);
    const int name_h = dc.GetCharHeight();
    dc.DrawText(shown, x, rect.y + (rect.height - name_h) / 2);

    //Nozzle chips: filled when that variant is installed. This is the whole variant UI -
    //a chip is both the state and the switch, so there is nothing to learn.
    dc.SetFont(Label::Body_11);
    for (int i = 0; i < static_cast<int>(model.variants.size()); ++i) {
        const Variant &v = model.variants[i];
        const wxRect   r = chip_rect(rect, i, m_chip_w, m_chip_h, m_chip_gap, chips_right);
        const bool     chip_hover = hovered && hover_hit.variant == i;
        dc.SetPen(wxPen(v.installed ? m_pal.chip_on : m_pal.sep));
        dc.SetBrush(wxBrush(v.installed ? m_pal.chip_on : (chip_hover ? m_pal.accent_dim : m_pal.chip_off)));
        dc.DrawRoundedRectangle(r, m_chip_h / 2);
        dc.SetTextForeground(v.installed ? *wxWHITE : m_pal.dim);
        const wxString nz = from_u8(v.nozzle);
        const wxSize   ts = dc.GetTextExtent(nz);
        dc.DrawText(nz, r.x + (r.width - ts.x) / 2, r.y + (r.height - ts.y) / 2);
    }

    //The action glyph: remove when installed, add when not. Drawn only on hover for an
    //installed row, because a row of permanent ✕ glyphs reads as a list of things to delete.
    const wxRect act(rect.GetRight() - action_w, rect.y, action_w, rect.height);
    const bool   act_hover = hovered && hover_hit.action;
    if (model.installed()) {
        if (hovered) {
            dc.SetFont(Label::Body_14);
            dc.SetTextForeground(act_hover ? m_pal.accent : m_pal.dim);
            const wxString g = L"✕";
            const wxSize   ts = dc.GetTextExtent(g);
            dc.DrawText(g, act.x + (act.width - ts.x) / 2, act.y + (act.height - ts.y) / 2);
        }
    } else {
        dc.SetFont(Label::Body_14);
        dc.SetTextForeground(act_hover ? m_pal.accent : m_pal.dim);
        const wxString g = L"＋";
        const wxSize   ts = dc.GetTextExtent(g);
        dc.DrawText(g, act.x + (act.width - ts.x) / 2, act.y + (act.height - ts.y) / 2);
    }
}

void PrinterPickerDialog::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(m_list);
    m_list->DoPrepareDC(dc);

    const wxSize size = m_list->GetClientSize();
    int          view_x = 0, view_y = 0, unit_x = 0, unit_y = 0;
    m_list->GetViewStart(&view_x, &view_y);
    m_list->GetScrollPixelsPerUnit(&unit_x, &unit_y);
    const int top    = view_y * unit_y;
    const int bottom = top + size.GetHeight();

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(m_pal.bg));
    dc.DrawRectangle(wxRect(0, top, size.GetWidth(), size.GetHeight()));

    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        const Row &r = m_rows[i];
        if (r.y + r.h < top || r.y > bottom)
            continue;
        draw_row(dc, r, m_hover.row == i, m_hover);
    }
}

// ---------------------------------------------------------------------------- input

PrinterPickerDialog::Hit PrinterPickerDialog::hit_test(const wxPoint &pos) const
{
    Hit hit;
    wxPoint p = pos;
    m_list->CalcUnscrolledPosition(pos.x, pos.y, &p.x, &p.y);

    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        const Row &r = m_rows[i];
        if (p.y < r.y || p.y >= r.y + r.h)
            continue;
        hit.row = i;
        if (r.kind != Row::Kind::Printer)
            return hit;

        const wxRect rect(0, r.y, m_list->GetClientSize().GetWidth(), r.h);
        const int    action_w = m_row_h;
        if (p.x >= rect.GetRight() - action_w) {
            hit.action = true;
            return hit;
        }
        const Model &model = m_models[r.model];
        for (int v = 0; v < static_cast<int>(model.variants.size()); ++v) {
            if (chip_rect(rect, v, m_chip_w, m_chip_h, m_chip_gap, action_w + m_pad).Contains(p)) {
                hit.variant = v;
                return hit;
            }
        }
        return hit;
    }
    return hit;
}

void PrinterPickerDialog::on_mouse(wxMouseEvent &evt)
{
    if (evt.Leaving()) {
        if (m_hover.row != -1) {
            m_hover = Hit{};
            m_list->Refresh();
        }
        evt.Skip();
        return;
    }

    const Hit hit = hit_test(evt.GetPosition());

    if (evt.Moving() || evt.Entering()) {
        if (hit.row != m_hover.row || hit.variant != m_hover.variant || hit.action != m_hover.action) {
            m_hover = hit;
            m_list->Refresh();
        }
        evt.Skip();
        return;
    }

    if (!evt.LeftDown() || hit.row < 0 || m_rows[hit.row].kind != Row::Kind::Printer) {
        evt.Skip();
        return;
    }

    Model &model = m_models[m_rows[hit.row].model];
    if (hit.variant >= 0) {
        Variant &v = model.variants[hit.variant];
        //Turning off the last chip is a removal, and it is allowed: the alternative is a
        //disabled chip with no explanation, which is the silent refusal the fork forbids.
        v.installed = !v.installed;
    } else if (hit.action) {
        if (model.installed()) {
            for (Variant &v : model.variants)
                v.installed = false;
        } else {
            //Adding a machine means the nozzle it ships with. 0.4 when it has one, because
            //that is what is in the printer; otherwise its smallest.
            auto it = std::find_if(model.variants.begin(), model.variants.end(),
                                   [](const Variant &v) { return v.nozzle == "0.4"; });
            (it != model.variants.end() ? *it : model.variants.front()).installed = true;
        }
    } else {
        //Clicking the body of a row is the same intent as its action glyph.
        if (!model.installed()) {
            auto it = std::find_if(model.variants.begin(), model.variants.end(),
                                   [](const Variant &v) { return v.nozzle == "0.4"; });
            (it != model.variants.end() ? *it : model.variants.front()).installed = true;
        }
    }
    relayout_and_refresh();
}

void PrinterPickerDialog::on_search(const wxString &term)
{
    const std::string next = boost::to_lower_copy(into_u8(term));
    if (next == m_term)
        return;
    m_term = next;
    m_hover = Hit{};
    m_list->Scroll(0, 0);
    relayout_and_refresh();
}

// ---------------------------------------------------------------------------- headless

bool PrinterPickerDialog::install_headless(const std::string &vendor, const std::string &model_name,
                                           const std::string &nozzle)
{
    for (Model &m : m_models) {
        //Either identity is accepted, because a caller typing a printer's name off the screen
        //has the display name and a caller reading the config has the id.
        if (m.vendor != vendor || (m.id != model_name && m.name != model_name))
            continue;
        for (Variant &v : m.variants) {
            if (v.nozzle != nozzle)
                continue;
            v.installed = true;
            return true;
        }
        BOOST_LOG_TRIVIAL(error) << "PrinterPicker: " << vendor << " / " << model_name
                                 << " has no " << nozzle << " variant";
        return false;
    }
    BOOST_LOG_TRIVIAL(error) << "PrinterPicker: no such model " << vendor << " / " << model_name;
    return false;
}

bool PrinterPickerDialog::uninstall_headless(const std::string &vendor, const std::string &model_name)
{
    for (Model &m : m_models) {
        //Either identity is accepted, because a caller typing a printer's name off the screen
        //has the display name and a caller reading the config has the id.
        if (m.vendor != vendor || (m.id != model_name && m.name != model_name))
            continue;
        for (Variant &v : m.variants)
            v.installed = false;
        return true;
    }
    return false;
}

bool PrinterPickerDialog::commit()
{
    AppConfig    *cfg    = wxGetApp().app_config;
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (cfg == nullptr || bundle == nullptr)
        return false;

    AppConfig::VendorMap next;
    for (const Model &m : m_models) {
        for (const Variant &v : m.variants)
            if (v.installed)
                next[m.vendor][m.id].insert(v.nozzle);
    }
    //An empty set for a vendor is not the same as the vendor being absent; drop the empties
    //so apply_vendor_config's install/remove diff sees the same shape the guide gave it.
    for (auto it = next.begin(); it != next.end();) {
        for (auto mit = it->second.begin(); mit != it->second.end();)
            mit = mit->second.empty() ? it->second.erase(mit) : std::next(mit);
        it = it->second.empty() ? next.erase(it) : std::next(it);
    }

    if (next.empty()) {
        //Removing every printer leaves an app that cannot slice. Say so and change nothing;
        //this is a real impossibility, not a maybe, so a refusal is the honest answer.
        show_error(this, _L("At least one printer has to stay installed."));
        return false;
    }

    //A removal can pull a machine out from under a plate that is assigned to it. The plate
    //board already degrades honestly when that happens, but being told afterwards is not the
    //same as being asked first. This informs and lets the removal proceed - the plate is the
    //user's, and a slicer that refuses is worse than one that warns.
    std::vector<std::string> orphaned;
    for (const Model &m : m_models) {
        if (m.installed() || !m.changed())
            continue;
        if (Plater *plater = wxGetApp().plater(); plater != nullptr) {
            PartPlateList &plates = plater->get_partplate_list();
            for (int i = 0; i < plates.get_plate_count(); ++i) {
                PartPlate *plate = plates.get_plate(i);
                if (plate == nullptr || plate->get_printer_preset_name().empty())
                    continue;
                const Preset *p = bundle->printers.find_preset(plate->get_printer_preset_name(), false);
                //option<>() rather than opt_string(): the latter dereferences whatever it finds,
                //so a preset without printer_model would crash instead of not matching.
                const ConfigOptionString *pm =
                    p != nullptr ? p->config.option<ConfigOptionString>("printer_model") : nullptr;
                if (pm != nullptr && pm->value == m.id)
                    orphaned.push_back(into_u8(wxString::Format(_L("Plate %d"), i + 1)) + " — " + m.name);
            }
        }
    }
    if (!orphaned.empty()) {
        wxString detail;
        for (const std::string &s : orphaned)
            detail += "\n    " + from_u8(s);
        MessageDialog ask(this,
                          wxString::Format(_L("Removing these printers leaves plates without a machine:%s\n\n"
                                              "The plates keep their assignment and can be re-pointed later. Remove anyway?"),
                                           detail),
                          _L("Printers still in use"), wxYES_NO | wxNO_DEFAULT);
        if (ask.ShowModal() != wxID_YES)
            return false;
    }

    const auto filaments = cfg->has_section(AppConfig::SECTION_FILAMENTS)
                               ? cfg->get_section(AppConfig::SECTION_FILAMENTS)
                               : std::map<std::string, std::string>();

    //Newly installed machines bring their own default materials, so the printer arrives
    //usable rather than arriving and then needing a second, separate filament errand.
    std::map<std::string, std::string> next_filaments = filaments;
    for (const Model &m : m_models) {
        if (!m.installed() || !m.changed())
            continue;
        auto vit = bundle->vendors.find(m.vendor);
        if (vit == bundle->vendors.end())
            continue;
        for (const VendorProfile::PrinterModel &pm : vit->second.models) {
            if (pm.id != m.id)
                continue;
            for (const std::string &mat : pm.default_materials)
                next_filaments[mat] = "true";
        }
    }

    BOOST_LOG_TRIVIAL(info) << "PrinterPicker: applying " << next.size() << " vendors";
    //overwrite=true, because a removal has to be able to take an entry away; the merge mode
    //can only ever add.
    if (!bundle->apply_vendor_config(next, next_filaments, cfg, true))
        return false;
    cfg->save();

    for (Model &m : m_models)
        for (Variant &v : m.variants)
            v.was = v.installed;
    return true;
}

// ---------------------------------------------------------------------------- the dialog

PrinterPickerDialog::PrinterPickerDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Printers"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    PETKOS_PERF_SCOPE(Perf::Probe::PickerBuildUi);

    const int em = wxGetApp().em_unit();
    measure_geometry();

    resolve_palette();
    SetBackgroundColour(m_pal.bg);

    build_index();

    auto *root = new wxBoxSizer(wxVERTICAL);

    //The banner is the fork's chrome, and it is what says at a glance which slicer this is.
    auto *banner = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, em * 4));
    banner->SetBackgroundStyle(wxBG_STYLE_PAINT);
    banner->Bind(wxEVT_PAINT, [this, banner, em](wxPaintEvent &) {
        wxAutoBufferedPaintDC dc(banner);
        const wxRect          r(wxPoint(0, 0), banner->GetClientSize());
        if (!PodBanner::draw(dc, r, true)) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(wxColour(0x32, 0x34, 0x38)));
            dc.DrawRectangle(r);
        }
        dc.SetFont(Label::Head_16);
        dc.SetTextForeground(wxColour(0xEC, 0xEC, 0xEF));
        dc.DrawText(_L("Printers"), m_pad * 3 / 2, (r.height - dc.GetCharHeight()) / 2);
    });
    root->Add(banner, 0, wxEXPAND);

    //TextInput adds wxTE_PROCESS_ENTER to its inner control itself, so the style here is 0:
    //passing a text-control flag as a window style is how that widget's assert gets tripped.
    m_search = new TextInput(this, "", "", "search", wxDefaultPosition, wxSize(-1, em * 3), 0);
    m_search->GetTextCtrl()->SetHint(_L("Search a brand or model — “kobra x”"));
    m_search->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) {
        on_search(e.GetString());
        e.Skip();
    });
    //Enter on a single match installs it, so adding a known machine is type-and-return.
    m_search->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &e) {
        std::vector<int> hits;
        for (int i = 0; i < static_cast<int>(m_models.size()); ++i)
            if (!m_models[i].installed() && term_matches(m_models[i].search, m_term))
                hits.push_back(i);
        if (hits.size() == 1) {
            Model &m = m_models[hits.front()];
            auto   it = std::find_if(m.variants.begin(), m.variants.end(),
                                     [](const Variant &v) { return v.nozzle == "0.4"; });
            (it != m.variants.end() ? *it : m.variants.front()).installed = true;
            relayout_and_refresh();
        }
        e.Skip();
    });
    root->Add(m_search, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, m_pad);

    m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(em * 40, em * 30),
                                  wxVSCROLL | wxFULL_REPAINT_ON_RESIZE);
    m_list->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_list->SetBackgroundColour(m_pal.bg);
    m_list->Bind(wxEVT_PAINT, &PrinterPickerDialog::on_paint, this);
    m_list->Bind(wxEVT_MOTION, &PrinterPickerDialog::on_mouse, this);
    m_list->Bind(wxEVT_LEFT_DOWN, &PrinterPickerDialog::on_mouse, this);
    m_list->Bind(wxEVT_ENTER_WINDOW, &PrinterPickerDialog::on_mouse, this);
    m_list->Bind(wxEVT_LEAVE_WINDOW, &PrinterPickerDialog::on_mouse, this);
    m_list->Bind(wxEVT_SIZE, [this](wxSizeEvent &e) { refresh_rows(); m_list->Refresh(); e.Skip(); });
    root->Add(m_list, 1, wxEXPAND | wxALL, m_pad);

    auto *foot = new wxBoxSizer(wxHORIZONTAL);
    m_count = new wxStaticText(this, wxID_ANY, "");
    m_count->SetFont(Label::Body_12);
    m_count->SetForegroundColour(m_pal.dim);
    foot->Add(m_count, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, m_pad);
    foot->AddStretchSpacer();

    auto *cancel = new Button(this, _L("Cancel"));
    cancel->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    foot->Add(cancel, 0, wxRIGHT, m_pad);

    m_apply = new Button(this, _L("Apply"));
    m_apply->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (commit())
            EndModal(wxID_OK);
    });
    foot->Add(m_apply, 0, wxRIGHT, m_pad);
    root->Add(foot, 0, wxEXPAND | wxBOTTOM | wxTOP, m_pad);

    SetSizer(root);
    relayout_and_refresh();
    root->SetSizeHints(this);
    CenterOnParent();
    m_search->GetTextCtrl()->SetFocus();

    wxGetApp().UpdateDlgDarkUI(this);
}

void PrinterPickerDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    measure_geometry();
    resolve_palette();
    relayout_and_refresh();
    Fit();
    Refresh();
}

bool pod_pick_printers(wxWindow *parent)
{
    PrinterPickerDialog dlg(parent);
    const bool changed = dlg.ShowModal() == wxID_OK;
    if (changed) {
        //The same three refreshes the guide did on success, so the combo boxes and the
        //plate board see the new set without a restart.
        wxGetApp().load_current_presets();
        wxGetApp().update_publish_status();
        if (wxGetApp().mainframe != nullptr)
            wxGetApp().mainframe->refresh_plugin_tips();
    }
    return changed;
}

// ======================================================================== save to library
//
//Saving a project used to mean the OS file dialog: pick a folder, remember which of seven
//numbered stage folders is the right one, get it wrong, and find the project missing from the
//Library afterwards. All three of those are the same defect - the app knows where projects
//belong and was making the user carry that knowledge.

namespace {

//The library root. Configurable, because a path compiled into a slicer is a path that is
//wrong on the next machine; defaulted, because the fork already names this tree in its own
//home page and pretending otherwise would just mean an empty dialog on first use.
boost::filesystem::path library_root()
{
    std::string configured = wxGetApp().app_config->get("pod_library_dir");
    if (configured.empty())
        configured = "E:/3D-Printing/Projects";
    return boost::filesystem::path(configured).make_preferred();
}

//A stage folder is one whose name starts "NN-". Read off disk so a stage added in Explorer
//appears here without a rebuild - the pipeline's own doc says the tree is the truth.
bool is_stage_dir(const std::string &name)
{
    return name.size() > 3 && std::isdigit((unsigned char) name[0]) &&
           std::isdigit((unsigned char) name[1]) && name[2] == '-';
}

std::vector<std::string> subdirs(const boost::filesystem::path &dir, bool stages_only)
{
    std::vector<std::string> out;
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(dir, ec))
        return out;
    for (boost::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!boost::filesystem::is_directory(it->status()))
            continue;
        const std::string name = it->path().filename().string();
        //Leading-underscore folders are the tree's own bookkeeping (_dropped, _Unsorted).
        if (name.empty() || name[0] == '_' || name[0] == '.')
            continue;
        if (stages_only != is_stage_dir(name))
            continue;
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

bool pod_path_in_library(const wxString &path)
{
    if (path.empty())
        return false;
    const std::string root = boost::to_lower_copy(library_root().string());
    std::string       p    = boost::to_lower_copy(into_u8(path));
    std::replace(p.begin(), p.end(), '/', '\\');
    std::string r = root;
    std::replace(r.begin(), r.end(), '/', '\\');
    return !r.empty() && p.compare(0, r.size(), r) == 0;
}

class SaveToLibraryDialog : public DPIDialog
{
public:
    SaveToLibraryDialog(wxWindow *parent, const wxString &suggested_name, const wxString &current_path);
    //Absolute path chosen, or "<browse>" when the user wants the ordinary dialog.
    wxString result() const { return m_result; }

protected:
    void on_dpi_changed(const wxRect &) override { Fit(); Refresh(); }

private:
    void rebuild_stages();
    void refresh_preview();
    boost::filesystem::path chosen_path() const;

    wxString      m_result;
    TextInput    *m_name  = nullptr;
    wxChoice     *m_group = nullptr;
    wxChoice     *m_stage = nullptr;
    wxStaticText *m_path  = nullptr;
    wxStaticText *m_note  = nullptr;
    Button       *m_save  = nullptr;
};

SaveToLibraryDialog::SaveToLibraryDialog(wxWindow *parent, const wxString &suggested_name,
                                         const wxString &current_path)
    : DPIDialog(parent, wxID_ANY, _L("Save to library"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
{
    const int em = wxGetApp().em_unit();
    const bool dark = wxGetApp().dark_mode();
    SetBackgroundColour(dark ? wxColour(0x24, 0x24, 0x27) : wxColour(0xFF, 0xFF, 0xFF));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *grid = new wxFlexGridSizer(2, em / 2, em);
    grid->AddGrowableCol(1, 1);

    auto label = [&](const wxString &t) {
        auto *st = new wxStaticText(this, wxID_ANY, t);
        st->SetFont(Label::Body_13);
        st->SetForegroundColour(dark ? wxColour(0x8C, 0x8C, 0x94) : wxColour(0x78, 0x78, 0x80));
        return st;
    };

    m_name = new TextInput(this, suggested_name, "", "", wxDefaultPosition, wxSize(em * 22, em * 3), 0);
    m_name->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) { refresh_preview(); e.Skip(); });
    grid->Add(label(_L("Name")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_name, 1, wxEXPAND);

    m_group = new wxChoice(this, wxID_ANY);
    for (const std::string &g : subdirs(library_root(), false))
        m_group->Append(from_u8(g));
    //A group that does not exist yet is a normal thing to want, and making the user leave the
    //dialog to create a folder is the file-explorer detour this exists to remove.
    m_group->Append(_L("New group…"));
    grid->Add(label(_L("Group")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_group, 1, wxEXPAND);

    m_stage = new wxChoice(this, wxID_ANY);
    grid->Add(label(_L("Stage")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_stage, 1, wxEXPAND);
    root->Add(grid, 0, wxEXPAND | wxALL, em);

    //The path is shown, always. Hiding the file explorer is not the same as hiding where the
    //file goes, and a save whose destination cannot be read is not more intuitive, it is less.
    m_path = new wxStaticText(this, wxID_ANY, "");
    m_path->SetFont(Label::Body_12);
    m_path->SetForegroundColour(dark ? wxColour(0x6B, 0x9D, 0xB8) : wxColour(0x4F, 0x87, 0xA5));
    root->Add(m_path, 0, wxEXPAND | wxLEFT | wxRIGHT, em);

    m_note = new wxStaticText(this, wxID_ANY, "");
    m_note->SetFont(Label::Body_12);
    root->Add(m_note, 0, wxEXPAND | wxALL, em);

    auto *foot = new wxBoxSizer(wxHORIZONTAL);
    auto *browse = new Button(this, _L("Browse…"));
    browse->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    browse->SetToolTip(_L("Use the ordinary file dialog instead"));
    browse->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_result = "<browse>"; EndModal(wxID_OK); });
    foot->Add(browse, 0, wxRIGHT, em);
    foot->AddStretchSpacer();

    auto *cancel = new Button(this, _L("Cancel"));
    cancel->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_result.clear(); EndModal(wxID_CANCEL); });
    foot->Add(cancel, 0, wxRIGHT, em);

    m_save = new Button(this, _L("Save"));
    m_save->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        const boost::filesystem::path p = chosen_path();
        if (p.empty())
            return;
        boost::system::error_code ec;
        boost::filesystem::create_directories(p.parent_path(), ec);
        if (ec) {
            show_error(this, wxString::Format(_L("Could not create %s:\n%s"),
                                              from_u8(p.parent_path().string()), from_u8(ec.message())));
            return;
        }
        m_result = from_u8(p.string());
        EndModal(wxID_OK);
    });
    foot->Add(m_save, 0, wxRIGHT, em);
    root->Add(foot, 0, wxEXPAND | wxBOTTOM, em);

    //Open where the project already lives, when it lives anywhere, so re-saving is one click.
    if (!current_path.empty() && pod_path_in_library(current_path)) {
        std::string rel = into_u8(current_path).substr(library_root().string().size());
        std::replace(rel.begin(), rel.end(), '/', '\\');
        while (!rel.empty() && rel.front() == '\\') rel.erase(rel.begin());
        std::vector<std::string> parts;
        boost::split(parts, rel, boost::is_any_of("\\"));
        if (!parts.empty())
            m_group->SetStringSelection(from_u8(parts[0]));
    }
    if (m_group->GetSelection() == wxNOT_FOUND && m_group->GetCount() > 1)
        m_group->SetSelection(0);

    rebuild_stages();
    m_group->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        //"New group" asks for the name here and makes the folder here. Sending the user to
        //Explorer to create a directory is the detour this dialog exists to remove, so it
        //would be a strange place to reintroduce one.
        if (m_group->GetStringSelection() == _L("New group…")) {
            wxTextEntryDialog ask(this, _L("Name for the new group:"), _L("New group"));
            if (ask.ShowModal() != wxID_OK || ask.GetValue().Trim().empty()) {
                m_group->SetSelection(0);
            } else {
                const wxString name = ask.GetValue().Trim(true).Trim(false);
                boost::system::error_code ec;
                boost::filesystem::create_directories(library_root() / into_u8(name), ec);
                if (ec) {
                    show_error(this, wxString::Format(_L("Could not create %s:\n%s"), name,
                                                      from_u8(ec.message())));
                    m_group->SetSelection(0);
                } else {
                    m_group->Insert(name, m_group->GetCount() - 1);
                    m_group->SetStringSelection(name);
                }
            }
        }
        rebuild_stages();
    });
    m_stage->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { refresh_preview(); });

    SetSizer(root);
    root->SetSizeHints(this);
    CenterOnParent();
    m_name->GetTextCtrl()->SetFocus();
    m_name->GetTextCtrl()->SelectAll();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SaveToLibraryDialog::rebuild_stages()
{
    m_stage->Clear();
    const wxString group = m_group->GetStringSelection();
    std::vector<std::string> stages;
    if (!group.empty() && group != _L("New group…"))
        stages = subdirs(library_root() / into_u8(group), true);

    if (stages.empty()) {
        //A flat group is a real shape, not a missing one - most groups in the tree are flat.
        m_stage->Append(_L("(this group has no stages)"));
        m_stage->SetSelection(0);
        m_stage->Enable(false);
    } else {
        for (const std::string &s : stages)
            m_stage->Append(from_u8(s));
        m_stage->Enable(true);
        //Default to the stage a plated Orca project belongs in. "03-prepped" is that stage by
        //the pipeline's own definition; if this tree has renamed it, fall back to the last
        //stage that is not the immutable source rather than guessing at a number.
        int want = wxNOT_FOUND;
        for (size_t i = 0; i < stages.size(); ++i)
            if (stages[i].find("prepped") != std::string::npos)
                want = int(i);
        if (want == wxNOT_FOUND)
            want = stages.size() > 1 ? 1 : 0;
        m_stage->SetSelection(want);
    }
    refresh_preview();
}

boost::filesystem::path SaveToLibraryDialog::chosen_path() const
{
    wxString name = m_name->GetTextCtrl()->GetValue();
    name.Trim(true).Trim(false);
    if (name.empty())
        return {};
    if (!name.Lower().EndsWith(".3mf"))
        name += ".3mf";

    wxString group = m_group->GetStringSelection();
    if (group.empty() || group == _L("New group…"))
        return {};

    boost::filesystem::path p = library_root() / into_u8(group);
    if (m_stage->IsEnabled() && m_stage->GetSelection() != wxNOT_FOUND)
        p /= into_u8(m_stage->GetStringSelection());
    return (p / into_u8(name)).make_preferred();
}

void SaveToLibraryDialog::refresh_preview()
{
    const bool dark = wxGetApp().dark_mode();
    const wxString group = m_group->GetStringSelection();

    if (group == _L("New group…")) {
        //Only reachable for the instant between the choice and the prompt it opens.
        m_path->SetLabel(_L("Naming a new group…"));
        m_note->SetLabel("");
        m_save->Enable(false);
        Layout();
        return;
    }

    const boost::filesystem::path p = chosen_path();
    if (p.empty()) {
        m_path->SetLabel(_L("Give the project a name."));
        m_note->SetLabel("");
        m_save->Enable(false);
        Layout();
        return;
    }

    m_path->SetLabel(from_u8(p.string()));

    //Two things are worth saying and nothing else is: that this would overwrite, and that
    //this stage is the one the tree says never to write into.
    wxString note;
    wxColour colour = dark ? wxColour(0x8C, 0x8C, 0x94) : wxColour(0x78, 0x78, 0x80);
    const std::string stage = m_stage->IsEnabled() ? into_u8(m_stage->GetStringSelection()) : "";
    if (stage.find("source") != std::string::npos) {
        note   = _L("This stage is the untouched download and is meant to stay that way. "
                    "Saving here is allowed, but it is the one folder the pipeline says never to edit.");
        colour = wxColour(0xD9, 0x8A, 0x3B);
    } else if (boost::filesystem::exists(p)) {
        note   = _L("A project of this name is already here. Saving replaces it.");
        colour = wxColour(0xD9, 0x8A, 0x3B);
    }
    m_note->SetLabel(note);
    m_note->SetForegroundColour(colour);
    m_note->Wrap(GetSize().GetWidth() - wxGetApp().em_unit() * 3);
    m_save->Enable(true);
    Layout();
}

wxString pod_save_to_library(wxWindow *parent, const wxString &suggested_name,
                             const wxString &current_path)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_directory(library_root(), ec)) {
        //No library on this machine: fall straight through rather than showing an empty dialog
        //and making the user work out why it is empty.
        BOOST_LOG_TRIVIAL(info) << "pod_save_to_library: no library at " << library_root().string()
                                << "; using the ordinary file dialog";
        return "<browse>";
    }
    SaveToLibraryDialog dlg(parent, suggested_name, current_path);
    if (dlg.ShowModal() != wxID_OK)
        return {};
    return dlg.result();
}

void pod_library_note_saved(const wxString &path)
{
    //The Library page reads a generated index, and until now nothing invalidated it except a
    //script run by hand - so a project saved from inside the app was invisible in the very
    //library it had been saved into. The tree is the truth and the index is a cache.
    //
    //`ingest.py` already applies exactly this rule after filing a dropped model, in its own
    //words: filing without refreshing leaves the work invisible. So this calls the same
    //builder rather than becoming a second writer of the same index - two writers of one cache
    //is how the two disagree later.
    //
    //Asynchronous, unlike the drop path. A drop reloads the page to show its result and so has
    //to wait; a save has nothing on screen to update, and making the user wait ~9 s at the end
    //of every save to refresh a page they are not looking at would be a strange trade.
    wxString script;
    if (!wxGetEnv("PETKOS_ORCA_LIBRARY_BUILD", &script) || script.empty())
        //Raw string: every separator is a literal backslash, and "\b" would be a backspace.
        script = wxString::FromUTF8(R"(E:\3D-Printing\Scripts\build_library.py)");
    if (!wxFileExists(script)) {
        BOOST_LOG_TRIVIAL(info) << "pod_library_note_saved: no library builder at "
                                << script.ToUTF8().data() << "; the index will be stale until "
                                   "it is run";
        return;
    }
    const wxString cmd = wxString::Format("python \"%s\"", script);
    const long     rc  = wxExecute(cmd, wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE);
    BOOST_LOG_TRIVIAL(info) << "pod_library_note_saved: refreshing the library index after "
                            << into_u8(path) << " (pid " << rc << ")";
}

}} // namespace Slic3r::GUI
