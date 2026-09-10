#include "WebViewDialog.hpp"
#include "ProjectLibrary.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "libslic3r_version.h"
#include "../Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <map>
#include <set>

#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/filefn.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>
#include <wx/url.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>

#include <slic3r/GUI/Widgets/WebView.hpp>

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {
    wxDECLARE_EVENT(EVT_RESPONSE_MESSAGE, wxCommandEvent);

    wxDEFINE_EVENT(EVT_RESPONSE_MESSAGE, wxCommandEvent);

    #define LOGIN_INFO_UPDATE_TIMER_ID 10002

    BEGIN_EVENT_TABLE(WebViewPanel, wxPanel)
    EVT_TIMER(LOGIN_INFO_UPDATE_TIMER_ID, WebViewPanel::OnFreshLoginStatus)
    END_EVENT_TABLE()



bool WebViewPanel::IngestDroppedFiles(const wxArrayString &paths)
{
    if (paths.empty()) return false;
    AddLibraryFolders(paths);
    return true;
}

std::vector<std::string> WebViewPanel::LibraryRoots() const
{
    const auto value = nlohmann::json::parse(wxGetApp().app_config->get("project_library_roots"), nullptr, false);
    std::vector<std::string> roots;
    if (value.is_array())
        for (const auto &root : value)
            if (root.is_string()) roots.push_back(root.get<std::string>());
    return roots;
}

void WebViewPanel::AddLibraryFolders(const wxArrayString &paths)
{
    auto roots = LibraryRoots();
    for (const auto &path : paths) {
        wxString folder = wxDirExists(path) ? path : wxFileName(path).GetPath();
        if (folder.empty() || !wxDirExists(folder)) continue;
        const std::string value = into_u8(folder);
        if (std::none_of(roots.begin(), roots.end(), [&value](const std::string &root) {
                return ProjectLibrary::path_key(root) == ProjectLibrary::path_key(value);
            })) roots.push_back(value);
    }
    wxGetApp().app_config->set("project_library_roots", nlohmann::json(roots).dump());
    RefreshLibrary();
}

void WebViewPanel::RefreshLibrary()
{
    RunScript("if (typeof LibBusy === 'function') LibBusy(true);");
    if (m_library_worker.joinable()) {
        m_library_cancel = true;
        m_library_refresh_pending = true;
        return;
    }
    const auto roots = LibraryRoots();
    const auto previous = m_library_data;
    m_library_thumbnail_generation = m_thumbnail_generation;
    const std::string cache_dir = (boost::filesystem::path(data_dir()) / "cache" / "project-library").string();
    m_library_cancel = false;
    m_library_ready = false;
    m_library_worker = std::thread([this, roots, cache_dir, previous] {
#ifdef _WIN32
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        try { m_library_result = ProjectLibrary::scan(roots, cache_dir, previous, m_library_cancel); }
        catch (const std::exception &error) {
            m_library_result = {{"error", error.what()}};
        }
        m_library_ready = true;
    });
    m_library_timer.Start(100);
}

void WebViewPanel::OnLibraryReady(wxTimerEvent &)
{
    if (!m_library_ready) return;
    m_library_timer.Stop();
    m_library_worker.join();
    if (m_library_refresh_pending) {
        m_library_refresh_pending = false;
        RefreshLibrary();
        return;
    }
    if (!m_library_result.contains("error")) {
        // A thumbnail can finish while this metadata scan is running.
        std::map<std::string, const nlohmann::json *> completed;
        if (m_library_data.contains("items") && m_library_data["items"].is_array()) {
            for (const auto &item : m_library_data["items"])
                if (item.value("thumbState", "") == "ready" &&
                    item.value("thumbGeneration", uint64_t(0)) > m_library_thumbnail_generation)
                    completed.emplace(item.value("path", ""), &item);
        }
        for (auto &item : m_library_result["items"]) {
            const auto old = completed.find(item.value("path", ""));
            if (old == completed.end() || old->second->value("revision", "") != item.value("revision", "") ||
                old->second->value("size", uint64_t(0)) != item.value("size", uint64_t(0))) continue;
            for (auto field = old->second->begin(); field != old->second->end(); ++field)
                if (field.key() == "views" || field.key().compare(0, 5, "thumb") == 0)
                    item[field.key()] = field.value();
        }
        m_library_data = m_library_result;
        std::set<std::string> indexed;
        for (const auto &item : m_library_data["items"]) indexed.insert(item.value("path", ""));
        m_thumbnail_queue.erase(std::remove_if(m_thumbnail_queue.begin(), m_thumbnail_queue.end(),
            [&indexed](const auto &request) {
                return request.first.value("_thumbnailAutomatic", false) &&
                    indexed.count(request.first.value("path", "")) == 0;
            }), m_thumbnail_queue.end());
        if (m_thumbnail_active_automatic && indexed.count(m_thumbnail_active_path) == 0)
            m_thumbnail_cancel = true;
    }
    RunScript(from_u8("LibReceive(" + m_library_result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ");"));
    if (!m_library_result.contains("error")) {
        std::set<std::string> queued{m_thumbnail_active_path};
        for (const auto &request : m_thumbnail_queue) queued.insert(request.first.value("path", ""));
        for (const auto &item : m_library_data["items"]) {
            if (item.value("thumbState", "") == "pending" && queued.insert(item.value("path", "")).second) {
                auto request = item;
                request["_thumbnailAutomatic"] = true;
                m_thumbnail_queue.emplace_back(std::move(request), false);
            }
        }
        StartNextLibraryThumbnail();
    }
}

void WebViewPanel::QueueLibraryThumbnails(const nlohmann::json &paths, bool force)
{
    if (!paths.is_array()) return;
    // Most recently visible cards go ahead of work left from an earlier filter/page.
    const size_t count = std::min(paths.size(), size_t(256));
    for (size_t i = count; i > 0; --i) {
        if (!paths[i - 1].is_string()) continue;
        const auto path = paths[i - 1].get<std::string>();
        if (path.empty() || path.size() > 32768) continue;
        if (path == m_thumbnail_active_path) {
            m_thumbnail_active_automatic = false;
            // A cancelled automatic render needs a new explicit request.
            if (!force && !m_thumbnail_cancel) continue;
        }
        nlohmann::json item{{"path", path}};
        if (m_library_data.contains("items") && m_library_data["items"].is_array()) {
            for (const auto &entry : m_library_data["items"])
                if (entry.value("path", "") == path) { item = entry; break; }
        }
        const auto queued = std::find_if(m_thumbnail_queue.begin(), m_thumbnail_queue.end(),
            [&path](const auto &entry) { return entry.first.value("path", "") == path; });
        bool regenerate = force;
        if (queued != m_thumbnail_queue.end()) {
            regenerate |= queued->second;
            m_thumbnail_queue.erase(queued);
        }
        item.erase("_thumbnailAutomatic");
        m_thumbnail_queue.emplace_front(std::move(item), regenerate);
    }
    StartNextLibraryThumbnail();
}

void WebViewPanel::StartNextLibraryThumbnail()
{
    if (m_thumbnail_worker.joinable()) return;
    if (m_thumbnail_queue.empty()) { m_thumbnail_timer.Stop(); return; }
    auto request = std::move(m_thumbnail_queue.front());
    m_thumbnail_queue.pop_front();
    m_thumbnail_active_path = request.first.value("path", "");
    m_thumbnail_active_automatic = request.first.value("_thumbnailAutomatic", false);
    const std::string cache_dir = (boost::filesystem::path(data_dir()) / "cache" / "project-library").string();
    const auto executable = into_u8(wxStandardPaths::Get().GetExecutablePath());
    m_thumbnail_ready = false;
    m_thumbnail_cancel = false;
    m_thumbnail_worker = std::thread([this, request = std::move(request), cache_dir, executable] {
#ifdef _WIN32
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        try {
            m_thumbnail_result = ProjectLibrary::thumbnail(request.first, cache_dir, executable,
                                                            m_thumbnail_cancel, request.second);
        } catch (const std::exception &error) {
            m_thumbnail_result = {{"path", request.first.value("path", "")}, {"thumb", ""},
                {"views", nlohmann::json::array()}, {"thumbState", "unavailable"}, {"thumbError", error.what()}};
        }
        m_thumbnail_ready = true;
    });
    m_thumbnail_timer.Start(100);
}

void WebViewPanel::OnLibraryThumbnailReady(wxTimerEvent &)
{
    if (!m_thumbnail_ready) return;
    m_thumbnail_worker.join();
    m_thumbnail_ready = false;
    m_thumbnail_active_path.clear();
    m_thumbnail_active_automatic = false;
    if (m_thumbnail_cancel) {
        StartNextLibraryThumbnail();
        return;
    }
    bool stale = false;
    if (m_library_data.contains("items") && m_library_data["items"].is_array()) {
        for (auto &item : m_library_data["items"]) {
            if (item.value("path", "") != m_thumbnail_result.value("path", "")) continue;
            stale = m_thumbnail_result.contains("revision") &&
                (item.value("revision", "") != m_thumbnail_result.value("revision", "") ||
                 item.value("size", uint64_t(0)) != m_thumbnail_result.value("size", uint64_t(0)));
            if (!stale) {
                for (auto field = m_thumbnail_result.begin(); field != m_thumbnail_result.end(); ++field)
                    if (field.key() == "views" || field.key().compare(0, 5, "thumb") == 0)
                        item[field.key()] = field.value();
                item["thumbGeneration"] = ++m_thumbnail_generation;
            }
            break;
        }
    }
    if (stale) RefreshLibrary();
    else RunScript(from_u8("if (typeof LibThumbnailReceive === 'function') LibThumbnailReceive(" +
        m_thumbnail_result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + ");"));
    StartNextLibraryThumbnail();
}

bool WebViewPanel::HandleLibraryRequest(const std::string &message)
{
    const auto request = nlohmann::json::parse(message, nullptr, false);
    if (!request.is_object()) return false;
    if (!request.contains("command") || !request["command"].is_string()) return false;
    const auto command = request["command"].get<std::string>();
    if (command == "homepage_library_refresh") {
        // Adopt folders from the old index once. New installations start with Add folder.
        if (wxGetApp().app_config->get("project_library_roots").empty()) {
            std::vector<std::string> roots;
            if (request.contains("roots") && request["roots"].is_array())
                for (const auto &root : request["roots"])
                    if (root.is_string() && boost::filesystem::path(root.get<std::string>()).is_absolute())
                        roots.push_back(root.get<std::string>());
            wxGetApp().app_config->set("project_library_roots", nlohmann::json(roots).dump());
        }
        RefreshLibrary();
    } else if (command == "homepage_library_thumbnails") {
        if (request.contains("paths")) QueueLibraryThumbnails(request["paths"], request.value("force", false));
    } else if (command == "homepage_library_add_folder") {
        wxDirDialog dialog(this, _L("Add a library folder"), wxEmptyString, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) AddLibraryFolders(wxArrayString{dialog.GetPath()});
    } else if (command == "homepage_library_remove_folder") {
        auto roots = LibraryRoots();
        const std::string path = request.value("path", std::string());
        roots.erase(std::remove(roots.begin(), roots.end(), path), roots.end());
        wxGetApp().app_config->set("project_library_roots", nlohmann::json(roots).dump());
        RefreshLibrary();
    } else return false;
    return true;
}

void WebViewPanel::SetFocusOnWebView()
{
    if (!m_library_data.is_null() && !m_library_worker.joinable()) RefreshLibrary();
    if (m_browser != nullptr)
        m_browser->SetFocus();
    else
        wxPanel::SetFocus();
}

bool HomePageDropTarget::OnDropFiles(wxCoord, wxCoord, const wxArrayString &filenames)
{
    return m_panel != nullptr && m_panel->IngestDroppedFiles(filenames);
}

WebViewPanel::WebViewPanel(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize), m_library_timer(this), m_thumbnail_timer(this)
 {
    Bind(wxEVT_TIMER, &WebViewPanel::OnLibraryReady, this, m_library_timer.GetId());
    Bind(wxEVT_TIMER, &WebViewPanel::OnLibraryThumbnailReady, this, m_thumbnail_timer.GetId());
    wxString url = wxString::Format("file://%s/web/homepage/index.html", from_u8(resources_dir()));
    wxString strlang = wxGetApp().current_language_code_safe();
    if (strlang != "")
        url = wxString::Format("file://%s/web/homepage/index.html?lang=%s", from_u8(resources_dir()), strlang);

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);
    
#if !BBL_RELEASE_TO_PUBLIC
    // Create the button
    bSizer_toolbar = new wxBoxSizer(wxHORIZONTAL);

    m_button_back = new wxButton(this, wxID_ANY, _CTX("Back", "Navigation"), wxDefaultPosition, wxDefaultSize, 0);
    m_button_back->Enable(false);
    bSizer_toolbar->Add(m_button_back, 0, wxALL, 5);

    m_button_forward = new wxButton(this, wxID_ANY, _L("Forward"), wxDefaultPosition, wxDefaultSize, 0);
    m_button_forward->Enable(false);
    bSizer_toolbar->Add(m_button_forward, 0, wxALL, 5);

    m_button_stop = new wxButton(this, wxID_ANY, _L("Stop"), wxDefaultPosition, wxDefaultSize, 0);

    bSizer_toolbar->Add(m_button_stop, 0, wxALL, 5);

    m_button_reload = new wxButton(this, wxID_ANY, _L("Reload"), wxDefaultPosition, wxDefaultSize, 0);
    bSizer_toolbar->Add(m_button_reload, 0, wxALL, 5);

    m_url = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    bSizer_toolbar->Add(m_url, 1, wxALL | wxEXPAND, 5);

    m_button_tools = new wxButton(this, wxID_ANY, _L("Tools"), wxDefaultPosition, wxDefaultSize, 0);
    bSizer_toolbar->Add(m_button_tools, 0, wxALL, 5);

    topsizer->Add(bSizer_toolbar, 0, wxEXPAND, 0);
    bSizer_toolbar->Show(false);

    // Create panel for find toolbar.
    wxPanel* panel = new wxPanel(this);
    topsizer->Add(panel, wxSizerFlags().Expand());

    // Create sizer for panel.
    wxBoxSizer* panel_sizer = new wxBoxSizer(wxVERTICAL);
    panel->SetSizer(panel_sizer);
#endif //BBL_RELEASE_TO_PUBLIC
    // Create the info panel
    m_info = new wxInfoBar(this);
    topsizer->Add(m_info, wxSizerFlags().Expand());
    // Create the webview
    m_browser = WebView::CreateWebView(this, url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    m_browser->Hide();
    SetSizer(topsizer);

    // Native file drops provide paths; the page alone receives only file contents.
    SetDropTarget(new HomePageDropTarget(this));

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    // Log backend information
    /* m_browser->GetUserAgent() may lead crash
    if (wxGetApp().get_mode() == comDevelop) {
        wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("Backend: %s Version: %s", m_browser->GetClassInfo()->GetClassName(),
            wxWebView::GetBackendVersionInfo().ToString());
        wxLogMessage("User Agent: %s", m_browser->GetUserAgent());
    }
    */

    // Create the Tools menu
    m_tools_menu = new wxMenu();
    wxMenuItem* viewSource = m_tools_menu->Append(wxID_ANY, _L("View Source"));
    wxMenuItem* viewText = m_tools_menu->Append(wxID_ANY, _L("View Text"));
    m_tools_menu->AppendSeparator();
    m_tools_handle_navigation = m_tools_menu->AppendCheckItem(wxID_ANY, _L("Handle Navigation"));
    m_tools_handle_new_window = m_tools_menu->AppendCheckItem(wxID_ANY, _L("Handle New Windows"));
    m_tools_menu->AppendSeparator();

    //Create an editing menu
    wxMenu* editmenu = new wxMenu();
    m_edit_cut = editmenu->Append(wxID_ANY, _L("Cut"));
    m_edit_copy = editmenu->Append(wxID_ANY, _L("Copy"));
    m_edit_paste = editmenu->Append(wxID_ANY, _L("Paste"));
    editmenu->AppendSeparator();
    m_edit_undo = editmenu->Append(wxID_ANY, _L("Undo"));
    m_edit_redo = editmenu->Append(wxID_ANY, _L("Redo"));
    editmenu->AppendSeparator();
    m_edit_mode = editmenu->AppendCheckItem(wxID_ANY, _L("Edit Mode"));
    m_tools_menu->AppendSubMenu(editmenu, "Edit");

    wxMenu* script_menu = new wxMenu;
    m_script_string = script_menu->Append(wxID_ANY, "Return String");
    m_script_integer = script_menu->Append(wxID_ANY, "Return integer");
    m_script_double = script_menu->Append(wxID_ANY, "Return double");
    m_script_bool = script_menu->Append(wxID_ANY, "Return bool");
    m_script_object = script_menu->Append(wxID_ANY, "Return JSON object");
    m_script_array = script_menu->Append(wxID_ANY, "Return array");
    m_script_dom = script_menu->Append(wxID_ANY, "Modify DOM");
    m_script_undefined = script_menu->Append(wxID_ANY, "Return undefined");
    m_script_null = script_menu->Append(wxID_ANY, "Return null");
    m_script_date = script_menu->Append(wxID_ANY, "Return Date");
    m_script_message = script_menu->Append(wxID_ANY, "Send script message");
    m_script_custom = script_menu->Append(wxID_ANY, "Custom script");
    m_tools_menu->AppendSubMenu(script_menu, _L("Run Script"));
    wxMenuItem* addUserScript = m_tools_menu->Append(wxID_ANY, _L("Add user script"));
    wxMenuItem* setCustomUserAgent = m_tools_menu->Append(wxID_ANY, _L("Set custom user agent"));

    //Selection menu
    wxMenu* selection = new wxMenu();
    m_selection_clear = selection->Append(wxID_ANY, _L("Clear Selection"));
    m_selection_delete = selection->Append(wxID_ANY, _L("Delete Selection"));
    wxMenuItem* selectall = selection->Append(wxID_ANY, _L("Select All"));

    editmenu->AppendSubMenu(selection, "Selection");

    wxMenuItem* loadscheme = m_tools_menu->Append(wxID_ANY, _L("Custom Scheme Example"));
    wxMenuItem* usememoryfs = m_tools_menu->Append(wxID_ANY, _L("Memory File System Example"));

    m_context_menu = m_tools_menu->AppendCheckItem(wxID_ANY, _L("Enable Context Menu"));
    m_dev_tools = m_tools_menu->AppendCheckItem(wxID_ANY, _L("Enable Dev Tools"));

    //By default we want to handle navigation and new windows
    m_tools_handle_navigation->Check();
    m_tools_handle_new_window->Check();

    //Zoom
    m_zoomFactor = 100;

    // Connect the button events
#if !BBL_RELEASE_TO_PUBLIC
    Bind(wxEVT_BUTTON, &WebViewPanel::OnBack, this, m_button_back->GetId());
    Bind(wxEVT_BUTTON, &WebViewPanel::OnForward, this, m_button_forward->GetId());
    Bind(wxEVT_BUTTON, &WebViewPanel::OnStop, this, m_button_stop->GetId());
    Bind(wxEVT_BUTTON, &WebViewPanel::OnReload, this, m_button_reload->GetId());
    Bind(wxEVT_BUTTON, &WebViewPanel::OnToolsClicked, this, m_button_tools->GetId());
    Bind(wxEVT_TEXT_ENTER, &WebViewPanel::OnUrl, this, m_url->GetId());

#endif //BBL_RELEASE_TO_PUBLIC

    // Connect the webview events
    Bind(wxEVT_WEBVIEW_NAVIGATING, &WebViewPanel::OnNavigationRequest, this);
    Bind(wxEVT_WEBVIEW_NAVIGATED, &WebViewPanel::OnNavigationComplete, this);
    Bind(wxEVT_WEBVIEW_LOADED, &WebViewPanel::OnDocumentLoaded, this);
    Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &WebViewPanel::OnTitleChanged, this);
    Bind(wxEVT_WEBVIEW_ERROR, &WebViewPanel::OnError, this);
    Bind(wxEVT_WEBVIEW_NEWWINDOW, &WebViewPanel::OnNewWindow, this);
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &WebViewPanel::OnScriptMessage, this);
    Bind(EVT_RESPONSE_MESSAGE, &WebViewPanel::OnScriptResponseMessage, this);

    // Connect the menu events
    Bind(wxEVT_MENU, &WebViewPanel::OnViewSourceRequest, this, viewSource->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnViewTextRequest, this, viewText->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnCut, this, m_edit_cut->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnCopy, this, m_edit_copy->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnPaste, this, m_edit_paste->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnUndo, this, m_edit_undo->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRedo, this, m_edit_redo->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnMode, this, m_edit_mode->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptString, this, m_script_string->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptInteger, this, m_script_integer->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptDouble, this, m_script_double->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptBool, this, m_script_bool->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptObject, this, m_script_object->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptArray, this, m_script_array->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptDOM, this, m_script_dom->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptUndefined, this, m_script_undefined->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptNull, this, m_script_null->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptDate, this, m_script_date->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptMessage, this, m_script_message->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnRunScriptCustom, this, m_script_custom->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnAddUserScript, this, addUserScript->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnSetCustomUserAgent, this, setCustomUserAgent->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnClearSelection, this, m_selection_clear->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnDeleteSelection, this, m_selection_delete->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnSelectAll, this, selectall->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnLoadScheme, this, loadscheme->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnUseMemoryFS, this, usememoryfs->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnEnableContextMenu, this, m_context_menu->GetId());
    Bind(wxEVT_MENU, &WebViewPanel::OnEnableDevTools, this, m_dev_tools->GetId());

    //Connect the idle events
    Bind(wxEVT_IDLE, &WebViewPanel::OnIdle, this);
    Bind(wxEVT_CLOSE_WINDOW, &WebViewPanel::OnClose, this);

    m_LoginUpdateTimer = nullptr;
 }

WebViewPanel::~WebViewPanel()
{
    m_library_timer.Stop();
    m_thumbnail_timer.Stop();
    m_library_cancel = true;
    m_thumbnail_cancel = true;
    if (m_library_worker.joinable()) m_library_worker.join();
    if (m_thumbnail_worker.joinable()) m_thumbnail_worker.join();
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << " Start";
    SetEvtHandlerEnabled(false);
    
    delete m_tools_menu;

    if (m_LoginUpdateTimer != nullptr) {
        m_LoginUpdateTimer->Stop();
        delete m_LoginUpdateTimer;
        m_LoginUpdateTimer = NULL;
    }
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << " End";
}


void WebViewPanel::load_url(wxString& url)
{
    this->Show();
    this->Raise();
    m_url->SetLabelText(url);

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage(m_url->GetValue());
    m_browser->LoadURL(url);
    m_browser->SetFocus();
    UpdateState();
}

/**
    * Method that retrieves the current state from the web control and updates the GUI
    * the reflect this current state.
    */
void WebViewPanel::UpdateState()
{
#if !BBL_RELEASE_TO_PUBLIC
    if (m_browser->CanGoBack()) {
        m_button_back->Enable(true);
    }
    else {
        m_button_back->Enable(false);
    }
    if (m_browser->CanGoForward()) {
        m_button_forward->Enable(true);
    }
    else {
        m_button_forward->Enable(false);
    }
    if (m_browser->IsBusy())
    {
        m_button_stop->Enable(true);
    }
    else
    {
        m_button_stop->Enable(false);
    }

    //SetTitle(m_browser->GetCurrentTitle());
    m_url->SetValue(m_browser->GetCurrentURL());
#endif //BBL_RELEASE_TO_PUBLIC
}

void WebViewPanel::OnIdle(wxIdleEvent& WXUNUSED(evt))
{
#if !BBL_RELEASE_TO_PUBLIC
    if (m_browser->IsBusy())
    {
        wxSetCursor(wxCURSOR_ARROWWAIT);
        m_button_stop->Enable(true);
    }
    else
    {
        wxSetCursor(wxNullCursor);
        m_button_stop->Enable(false);
    }
#endif //BBL_RELEASE_TO_PUBLIC
}

/**
    * Callback invoked when user entered an URL and pressed enter
    */
void WebViewPanel::OnUrl(wxCommandEvent& WXUNUSED(evt))
{
    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage(m_url->GetValue());
    m_browser->LoadURL(m_url->GetValue());
    m_browser->SetFocus();
    UpdateState();
}

/**
    * Callback invoked when user pressed the "back" button
    */
void WebViewPanel::OnBack(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->GoBack();
    UpdateState();
}

/**
    * Callback invoked when user pressed the "forward" button
    */
void WebViewPanel::OnForward(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->GoForward();
    UpdateState();
}

/**
    * Callback invoked when user pressed the "stop" button
    */
void WebViewPanel::OnStop(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->Stop();
    UpdateState();
}

/**
    * Callback invoked when user pressed the "reload" button
    */
void WebViewPanel::OnReload(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->Reload();
    UpdateState();
}

void WebViewPanel::OnCut(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->Cut();
}

void WebViewPanel::OnCopy(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->Copy();
}

void WebViewPanel::OnPaste(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->Paste();
}

void WebViewPanel::OnUndo(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->Undo();
}

void WebViewPanel::OnRedo(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->Redo();
}

void WebViewPanel::OnMode(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->SetEditable(m_edit_mode->IsChecked());
}

void WebViewPanel::OnLoadScheme(wxCommandEvent& WXUNUSED(evt))
{
    wxPathList pathlist;
    pathlist.Add(".");
    pathlist.Add("..");
    pathlist.Add("../help");
    pathlist.Add("../../../samples/help");

    wxFileName helpfile(pathlist.FindValidPath("doc.zip"));
    helpfile.MakeAbsolute();
    wxString path = helpfile.GetFullPath();
    //Under MSW we need to flip the slashes
    path.Replace("\\", "/");
    path = "wxfs:///" + path + ";protocol=zip/doc.htm";
    m_browser->LoadURL(path);
}

void WebViewPanel::OnUseMemoryFS(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->LoadURL("memory:page1.htm");
}

void WebViewPanel::OnEnableContextMenu(wxCommandEvent& evt)
{
    m_browser->EnableContextMenu(evt.IsChecked());
}

void WebViewPanel::OnEnableDevTools(wxCommandEvent& evt)
{
    m_browser->EnableAccessToDevTools(evt.IsChecked());
}

void WebViewPanel::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

void WebViewPanel::OnFreshLoginStatus(wxTimerEvent &event)
{
    auto mainframe = Slic3r::GUI::wxGetApp().mainframe;
    if (mainframe && mainframe->m_webview == this) {
        auto* app_config = Slic3r::GUI::wxGetApp().app_config;
        if (app_config && app_config->get_stealth_mode()) return;
        Slic3r::GUI::wxGetApp().get_login_info(ORCA_CLOUD_PROVIDER);
        if (app_config && app_config->has_cloud_provider(BBL_CLOUD_PROVIDER)) {
            Slic3r::GUI::wxGetApp().get_login_info(BBL_CLOUD_PROVIDER);
        }
    }
}

void WebViewPanel::SetLoginPanelVisibility(bool bshow)
{
    wxString strJS = wxString::Format("SetLoginPanelVisibility(%s)", bshow ? "true" : "false");
    RunScript(strJS);
}
void WebViewPanel::SendRecentList(int images)
{
    boost::property_tree::wptree req;
    boost::property_tree::wptree data;
    wxGetApp().mainframe->get_recent_projects(data, images);
    req.put(L"sequence_id", "");
    req.put(L"command", L"get_recent_projects");
    req.put_child(L"response", data);
    std::wostringstream oss;
    pt::write_json(oss, req, false);
    RunScript(wxString::Format("window.postMessage(%s)", oss.str()));
}

void WebViewPanel::SendDesignStaffpick(bool on)
{
    // if (on) {
    //     get_design_staffpick(0, 60, [this](std::string body) {
    //         if (body.empty() || body.front() != '{') {
    //             BOOST_LOG_TRIVIAL(warning) << "get_design_staffpick failed " + body;
    //             return;
    //         }
    //         CallAfter([this, body] {
    //             auto body2 = from_u8(body);
    //             body2.insert(1, "\"command\": \"modelmall_model_advise_get\", ");
    //             RunScript(wxString::Format("window.postMessage(%s)", body2));
    //         });
    //     });
    // } else {
    //     std::string body2 = "{\"total\":0, \"hits\":[]}";
    //     body2.insert(1, "\"command\": \"modelmall_model_advise_get\", ");
    //     RunScript(wxString::Format("window.postMessage(%s)", body2));
    // }
}

void WebViewPanel::OpenModelDetail(std::string id, NetworkAgent *agent)
{
    std::string url;
    if ((agent ? agent->get_model_mall_detail_url(&url, id) : get_model_mall_detail_url(&url, id)) == 0) 
    {
        if (url.find("?") != std::string::npos) 
        { 
            url += "&from=orcaslicer";
        } else {
            url += "?from=orcaslicer";
        }
        
        wxLaunchDefaultBrowser(url); 
    }
}

void WebViewPanel::SendLoginInfo()
{
    if (wxGetApp().getAgent()) {
        std::string login_info = wxGetApp().getAgent()->build_login_info();
        wxString strJS = wxString::Format("window.postMessage(%s)", login_info);
        RunScript(strJS);
    }
}

void WebViewPanel::ShowNetpluginTip()
{
    // Install Network Plugin
    const auto bblnetwork_enabled =wxGetApp().app_config->get_bool("installed_networking");
    if(!bblnetwork_enabled) {
        return;
    }
    bool        bValid       = wxGetApp().is_compatibility_version();

    int nShow = 0;
    if (!bValid) nShow = 1;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(": bValid=%1%, nShow=%2%")%bValid %nShow;

    json m_Res           = json::object();
    m_Res["command"]     = "network_plugin_installtip";
    m_Res["sequence_id"] = "10001";
    m_Res["show"]        = nShow;

    wxString strJS = wxString::Format("window.postMessage(%s)", m_Res.dump(-1, ' ', false, json::error_handler_t::ignore));

    RunScript(strJS);
}

void WebViewPanel::SendCloudProvidersInfo()
{
    auto* app_config = wxGetApp().app_config;
    if (!app_config)
        return;

    json j;
    j["command"] = "cloud_providers_info";
    json data;
    json provider_array = json::array();

    if (!app_config->get_hide_login_side_panel()) {
        auto providers = app_config->get_cloud_providers();
        for (const auto& p : providers) {
            provider_array.push_back(p);
        }
    }

    data["providers"] = provider_array;
    j["data"] = data;

    wxString strJS = wxString::Format("window.postMessage(%s)", j.dump());
    RunScript(strJS);
}

void WebViewPanel::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback)
{
    // auto host = wxGetApp().get_http_url(wxGetApp().app_config->get_country_code(), "v1/design-service/design/staffpick");
    // std::string url = (boost::format("%1%/?offset=%2%&limit=%3%") % host % offset % limit).str();

    // Http http = Http::get(url);
    // http.header("accept", "application/json")
    //     .header("Content-Type", "application/json")
    //     .on_complete([this, callback](std::string body, unsigned status) { callback(body); })
    //     .on_error([this, callback](std::string body, std::string error, unsigned status) {
    //         callback(body);
    //     })
    //     .perform();
}

int WebViewPanel::get_model_mall_detail_url(std::string *url, std::string id)
{
    // https://makerhub-qa.bambu-lab.com/en/models/2077
    std::string h = wxGetApp().get_model_http_url(wxGetApp().app_config->get_country_code());
    auto l = wxGetApp().current_language_code_safe();
    if (auto n = l.find('_'); n != std::string::npos)
        l = l.substr(0, n);
    *url = (boost::format("%1%%2%/models/%3%") % h % l % id).str();
    return 0;
}

void WebViewPanel::update_mode()
{
    GetSizer()->Show(size_t(0), wxGetApp().app_config->get("internal_developer_mode") == "true");
    GetSizer()->Layout();
}

/**
    * Callback invoked when there is a request to load a new page (for instance
    * when the user clicks a link)
    */
void WebViewPanel::OnNavigationRequest(wxWebViewEvent& evt)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetURL().ToUTF8().data();
    const wxString &url = evt.GetURL();
    if (url.StartsWith("File://") || url.StartsWith("file://")) {
        if (!url.Contains("/web/homepage/index.html")) {
            auto file = wxURL::Unescape(wxURL(url).GetPath());
#ifdef _WIN32
            if (file.StartsWith('/'))
                file = file.Mid(1);
            else
                file = "//" + file; // When file from network location
#endif
            wxGetApp().plater()->load_files(wxArrayString{1, &file});
            evt.Veto();
            return;
        }
    }

    if (m_info->IsShown())
    {
        m_info->Dismiss();
    }

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("%s", "Navigation request to '" + evt.GetURL() + "' (target='" +
            evt.GetTarget() + "')");

    //If we don't want to handle navigation then veto the event and navigation
    //will not take place, we also need to stop the loading animation
    if (!m_tools_handle_navigation->IsChecked())
    {
        evt.Veto();
        m_button_stop->Enable(false);
    }
    else
    {
        UpdateState();
    }
}

/**
    * Callback invoked when a navigation request was accepted
    */
void WebViewPanel::OnNavigationComplete(wxWebViewEvent& evt)
{
    m_browser->Show();
    Layout();
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetURL().ToUTF8().data();
    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("%s", "Navigation complete; url='" + evt.GetURL() + "'");
    UpdateState();
    ShowNetpluginTip();
}

/**
    * Callback invoked when a page is finished loading
    */
void WebViewPanel::OnDocumentLoaded(wxWebViewEvent& evt)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetTarget().ToUTF8().data();
    // Only notify if the document is the main frame, not a subframe
    if (evt.GetURL() == m_browser->GetCurrentURL())
    {
        if (wxGetApp().get_mode() == comDevelop)
            wxLogMessage("%s", "Document loaded; url='" + evt.GetURL() + "'");
    }
    UpdateState();
    SendCloudProvidersInfo();
}

void WebViewPanel::OnTitleChanged(wxWebViewEvent &evt)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();
    // wxGetApp().CallAfter([this] { SendRecentList(); });
}

/**
    * On new window, we veto to stop extra windows appearing
    */
void WebViewPanel::OnNewWindow(wxWebViewEvent& evt)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetURL().ToUTF8().data();
    wxString flag = " (other)";

    if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER)
    {
        flag = " (user)";
    }

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("%s", "New window; url='" + evt.GetURL() + "'" + flag);

    //If we handle new window events then just load them in this window as we
    //are a single window browser
    if (m_tools_handle_new_window->IsChecked())
        m_browser->LoadURL(evt.GetURL());

    UpdateState();
}

void WebViewPanel::OnScriptMessage(wxWebViewEvent& evt)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();
    if (HandleLibraryRequest(into_u8(evt.GetString()))) return;
    // update login status
    if (m_LoginUpdateTimer == nullptr) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Create Timer";
        m_LoginUpdateTimer = new wxTimer(this, LOGIN_INFO_UPDATE_TIMER_ID);
        m_LoginUpdateTimer->Start(2000);
    }

    if (wxGetApp().get_mode() == comDevelop)
        wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());
    std::string response = wxGetApp().handle_web_request(evt.GetString().ToUTF8().data());
    if (response.empty()) return;

    /* remove \n in response string */
    response.erase(std::remove(response.begin(), response.end(), '\n'), response.end());
    if (!response.empty()) {
        m_response_js = wxString::Format("window.postMessage('%s')", response);
        wxCommandEvent* event = new wxCommandEvent(EVT_RESPONSE_MESSAGE, this->GetId());
        wxQueueEvent(this, event);
    }
    else {
        m_response_js.clear();
    }
}

void WebViewPanel::OnScriptResponseMessage(wxCommandEvent& WXUNUSED(evt))
{
    if (!m_response_js.empty()) {
        RunScript(m_response_js);
    }
}

/**
    * Invoked when user selects the "View Source" menu item
    */
void WebViewPanel::OnViewSourceRequest(wxCommandEvent& WXUNUSED(evt))
{
    SourceViewDialog dlg(this, m_browser->GetPageSource());
    dlg.ShowModal();
}

/**
    * Invoked when user selects the "View Text" menu item
    */
void WebViewPanel::OnViewTextRequest(wxCommandEvent& WXUNUSED(evt))
{
    wxDialog textViewDialog(this, wxID_ANY, "Page Text",
        wxDefaultPosition, wxSize(700, 500),
        wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    wxTextCtrl* text = new wxTextCtrl(this, wxID_ANY, m_browser->GetPageText(),
        wxDefaultPosition, wxDefaultSize,
        wxTE_MULTILINE |
        wxTE_RICH |
        wxTE_READONLY);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(text, 1, wxEXPAND);
    SetSizer(sizer);
    textViewDialog.ShowModal();
}

/**
    * Invoked when user selects the "Menu" item
    */
void WebViewPanel::OnToolsClicked(wxCommandEvent& WXUNUSED(evt))
{
    if (m_browser->GetCurrentURL() == "")
        return;

    m_edit_cut->Enable(m_browser->CanCut());
    m_edit_copy->Enable(m_browser->CanCopy());
    m_edit_paste->Enable(m_browser->CanPaste());

    m_edit_undo->Enable(m_browser->CanUndo());
    m_edit_redo->Enable(m_browser->CanRedo());

    m_selection_clear->Enable(m_browser->HasSelection());
    m_selection_delete->Enable(m_browser->HasSelection());

    m_context_menu->Check(m_browser->IsContextMenuEnabled());
    m_dev_tools->Check(m_browser->IsAccessToDevToolsEnabled());

    wxPoint position = ScreenToClient(wxGetMousePosition());
    PopupMenu(m_tools_menu, position.x, position.y);
}

void WebViewPanel::RunScript(const wxString& javascript)
{
    // Remember the script we run in any case, so the next time the user opens
    // the "Run Script" dialog box, it is shown there for convenient updating.
    m_javascript = javascript;

    if (!m_browser) return;

    WebView::RunScript(m_browser, javascript);
}

void WebViewPanel::OnRunScriptString(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("setCount(345);");
}

void WebViewPanel::OnRunScriptInteger(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("function f(a){return a;}f(123);");
}

void WebViewPanel::OnRunScriptDouble(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("function f(a){return a;}f(2.34);");
}

void WebViewPanel::OnRunScriptBool(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("function f(a){return a;}f(false);");
}

void WebViewPanel::OnRunScriptObject(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("function f(){var person = new Object();person.name = 'Foo'; \
    person.lastName = 'Bar';return person;}f();");
}

void WebViewPanel::OnRunScriptArray(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("function f(){ return [\"foo\", \"bar\"]; }f();");
}

void WebViewPanel::OnRunScriptDOM(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("document.write(\"Hello World!\");");
}

void WebViewPanel::OnRunScriptUndefined(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("function f(){var person = new Object();}f();");
}

void WebViewPanel::OnRunScriptNull(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("function f(){return null;}f();");
}

void WebViewPanel::OnRunScriptDate(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("function f(){var d = new Date('10/08/2017 21:30:40'); \
    var tzoffset = d.getTimezoneOffset() * 60000; \
    return new Date(d.getTime() - tzoffset);}f();");
}

void WebViewPanel::OnRunScriptMessage(wxCommandEvent& WXUNUSED(evt))
{
    RunScript("window.wx.postMessage('This is a web message');");
}

void WebViewPanel::OnRunScriptCustom(wxCommandEvent& WXUNUSED(evt))
{
    wxTextEntryDialog dialog
    (
        this,
        "Please enter JavaScript code to execute",
        wxGetTextFromUserPromptStr,
        m_javascript,
        wxOK | wxCANCEL | wxCENTRE | wxTE_MULTILINE
    );
    if (dialog.ShowModal() != wxID_OK)
        return;

    RunScript(dialog.GetValue());
}

void WebViewPanel::OnAddUserScript(wxCommandEvent& WXUNUSED(evt))
{
    wxString userScript = "window.wx_test_var = 'wxWidgets webview sample';";
    wxTextEntryDialog dialog
    (
        this,
        "Enter the JavaScript code to run as the initialization script that runs before any script in the HTML document.",
        wxGetTextFromUserPromptStr,
        userScript,
        wxOK | wxCANCEL | wxCENTRE | wxTE_MULTILINE
    );
    if (dialog.ShowModal() != wxID_OK)
        return;

    if (!m_browser->AddUserScript(dialog.GetValue()))
        wxLogError("Could not add user script");
}

void WebViewPanel::OnSetCustomUserAgent(wxCommandEvent& WXUNUSED(evt))
{
    wxString customUserAgent = "Mozilla/5.0 (iPhone; CPU iPhone OS 13_1_3 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/13.0.1 Mobile/15E148 Safari/604.1";
    wxTextEntryDialog dialog
    (
        this,
        "Enter the custom user agent string you would like to use.",
        wxGetTextFromUserPromptStr,
        customUserAgent,
        wxOK | wxCANCEL | wxCENTRE
    );
    if (dialog.ShowModal() != wxID_OK)
        return;

    if (!m_browser->SetUserAgent(customUserAgent))
        wxLogError("Could not set custom user agent");
}

void WebViewPanel::OnClearSelection(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->ClearSelection();
}

void WebViewPanel::OnDeleteSelection(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->DeleteSelection();
}

void WebViewPanel::OnSelectAll(wxCommandEvent& WXUNUSED(evt))
{
    m_browser->SelectAll();
}

/**
    * Callback invoked when a loading error occurs
    */
void WebViewPanel::OnError(wxWebViewEvent& evt)
{
#define WX_ERROR_CASE(type) \
    case type: \
    category = #type; \
    break;

    wxString category;
    switch (evt.GetInt())
    {
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CONNECTION);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CERTIFICATE);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_AUTH);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_SECURITY);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_NOT_FOUND);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_REQUEST);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_USER_CANCELLED);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_OTHER);
    }

    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": [" << category << "] " << evt.GetString().ToUTF8().data();

    if (wxGetApp().get_mode() == comDevelop) 
    {
        wxLogMessage("%s", "Error; url='" + evt.GetURL() + "', error='" + category + " (" + evt.GetString() + ")'");

        // Show the info bar with an error
        m_info->ShowMessage(_L("An error occurred loading ") + evt.GetURL() + "\n" + "'" + category + "'", wxICON_ERROR);
    }

    UpdateState();
}


SourceViewDialog::SourceViewDialog(wxWindow* parent, wxString source) :
                  wxDialog(parent, wxID_ANY, "Source Code",
                           wxDefaultPosition, wxSize(700,500),
                           wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    wxTextCtrl* text = new wxTextCtrl(this, wxID_ANY, source,
                                      wxDefaultPosition, wxDefaultSize,
                                      wxTE_MULTILINE |
                                      wxTE_RICH |
                                      wxTE_READONLY);

    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(text, 1, wxEXPAND);
    SetSizer(sizer);
}


} // GUI
} // Slic3r
