#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <wx/event.h>
#include <wx/panel.h>
#include <wx/string.h>

class wxBitmapButton;
class wxWebView;
class wxWebViewEvent;

namespace Slic3r { namespace GUI {

// One model site of the Home tab (Unlayered Universe, MakerWorld, Nexprint, Thingiverse) in its own
// web view, shown beside the homepage nav column by WebViewPanel::set_model_site. Each site gets its
// own view so switching between them never reloads a page, and all of them are created and loaded
// in the background while Home is open (WebViewPanel::preload_model_sites).
//
// Each site is hosted the way its own slicer hosts it:
//   unlayered   - Unlayered Slicer's Universe tab: its address, plus the subset of its web-app
//                 bridge (window.__slicerBridge, WEB_APP_BRIDGE.md in the Unlayered Slicer
//                 source) the Universe page uses to open models and install printer profiles.
//   makerworld  - Bambu Studio's "Online Models": the /studio/webview page, the Back / Refresh /
//                 Open in browser toolbar above it, the Bambu sign-in carried over through a bind
//                 ticket, and the page's wx commands (makerworld_model_open, ...) handled by the app.
//   nexprint    - ElegooSlicer's "Online Models": its slicer user agent and URL parameters, and its
//                 page protocol (report.* requests, client.* events) with the Elegoo account.
//   thingiverse - the plain site.
// On every site, model files and "Open in slicer" links are imported into the plater.
class ModelSiteView : public wxPanel
{
public:
    ModelSiteView(wxWindow* parent, const std::string& site);
    ~ModelSiteView() override;

    // Ids of the sites, in nav order; and whether `site` is one of them.
    static const std::vector<std::string>& sites();
    static bool is_site(const std::string& site);

    const std::string& site() const { return m_site; }
    // Loads the site's start page. The first call happens when the view is created.
    void load_home();
    // True once the start page has been asked for (preload or first open).
    bool loaded() const { return m_loaded; }

private:
    wxString home_url() const;
    void     create_toolbar();
    void     update_toolbar();
    void     apply_user_agent();

    void on_webview_event(wxWebViewEvent& evt);
    void on_script_message(const std::string& body);
    void run_script(const wxString& script);

    // MakerWorld: page commands go to the app's homepage command handler.
    void makerworld_message(const std::string& body);
    // Loads `target` signed in to MakerWorld when the Bambu account is signed in.
    void makerworld_load(const wxString& target);

    // Nexprint: ElegooSlicer's request/response/event protocol.
    void nexprint_message(const nlohmann::json& msg);
    void nexprint_respond(const std::string& id, const std::string& method, int code, const nlohmann::json& data,
                          const std::string& message = "success");
    void nexprint_event(const std::string& method, const nlohmann::json& data);
    nlohmann::json nexprint_user_info() const;

    // Unlayered Universe: the web-app bridge subset.
    bool unlayered_trusted(const wxString& url) const;
    void unlayered_inject_bridge();
    void unlayered_message(const nlohmann::json& msg);
    void unlayered_respond(const std::string& id, const std::string& method, int code, const std::string& message,
                           const nlohmann::json& data = nlohmann::json::object());
    void unlayered_event(const std::string& id, const std::string& method, const nlohmann::json& data);
    void unlayered_open_from_url(const std::string& id, const nlohmann::json& params);
    void unlayered_install_preset(const std::string& id, const nlohmann::json& params);
    void unlayered_select_preset(const std::string& id, const nlohmann::json& params);

    std::string m_site;
    wxWebView*  m_browser{nullptr};
    bool        m_loaded{false};
    // Unlayered: per-document bridge token, minted on each load of the trusted origin.
    std::string m_bridge_token;
    bool        m_bridge_busy{false};
    // Cleared on destruction; async downloads check it before touching the view.
    std::shared_ptr<bool> m_alive;

    wxPanel*        m_toolbar{nullptr};
    wxBitmapButton* m_back_btn{nullptr};
    wxBitmapButton* m_refresh_btn{nullptr};
    wxBitmapButton* m_open_browser_btn{nullptr};
};

}} // namespace Slic3r::GUI
