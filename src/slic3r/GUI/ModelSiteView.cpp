#include "ModelSiteView.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <wx/bmpbuttn.h>
#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/secretstore.h>
#include <wx/stattext.h>
#include <wx/sizer.h>
#include <wx/uri.h>
#include <wx/utils.h>
#include <wx/webview.h>

#ifdef __linux__
#include <webkit2/webkit2.h>
#endif

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/ElegooCloud.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include "ElegooLoginDialog.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "wxExtensions.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/WebView.hpp"

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace Slic3r { namespace GUI {

namespace {

// Unlayered Slicer's Universe tab (WebAppSlot::universe() in its source): the page, the storage
// family it redirects downloads to, and the bridge limits it advertises.
constexpr const char* UNLAYERED_UNIVERSE_URL   = "https://unlayered3d.com/universe?host=unlayered-slicer";
constexpr const char* UNLAYERED_ORIGIN         = "https://unlayered3d.com";
constexpr size_t      UNLAYERED_MAX_DOWNLOAD   = 256u * 1024u * 1024u;
constexpr size_t      UNLAYERED_MAX_PRESET     = 4u * 1024u * 1024u;
const std::vector<std::string> UNLAYERED_FILE_TYPES{"3mf", "stl", "step", "stp", "obj", "amf", "svg", "drc"};
// Printer-preset keys that never travel with a shared profile (WAB_SECRET_KEYS in Unlayered Slicer).
const std::vector<std::string> UNLAYERED_SECRET_KEYS{"print_host", "print_host_webui", "printhost_apikey", "printhost_cafile",
                                                     "printhost_user", "printhost_password", "printhost_port",
                                                     "printhost_authorization_type", "printhost_ssl_ignore_revoke", "host_type",
                                                     "bed_custom_texture", "bed_custom_model"};

// Nexprint: ElegooSlicer's online-models page (ELEGOO_*_ONLINE_MODEL_URL in its libslic3r_version.h).
constexpr const char* NEXPRINT_MODELS_GLOBAL = "https://www.nexprint.com/elegooSlicer";
constexpr const char* NEXPRINT_MODELS_CHINA  = "https://www.nexprint.cn/elegooSlicer";

wxString language_code() { return wxGetApp().current_language_code_safe().BeforeFirst('_'); }

std::string elegoo_device_id()
{
    // Same key ElegooLoginDialog keeps, so the site sees one device for sign-in and browsing.
    AppConfig*  cfg = wxGetApp().app_config;
    std::string id  = cfg->get("vendor_send", "elegoo_device_id");
    if (id.empty()) {
        id = boost::uuids::to_string(boost::uuids::random_generator()());
        cfg->set("vendor_send", "elegoo_device_id", id);
    }
    return id;
}

// ElegooSlicer's web user agent; Nexprint serves its slicer layout to it.
wxString elegoo_user_agent()
{
    const std::string theme = wxGetApp().dark_mode() ? "dark" : "light";
#ifdef _WIN32
    const char* platform = "Windows NT 10.0; Win64; x64";
#elif defined(__APPLE__)
    const char* platform = "Macintosh; Intel Mac OS X 10_15_7";
#else
    const char* platform = "X11; Linux x86_64";
#endif
    return wxString::FromUTF8("ElegooSlicer/1.5.3.5 (" + theme + ") Mozilla/5.0 (" + platform + ")");
}

wxString http_origin(const wxString& url)
{
    const wxURI uri(url);
    const wxString scheme = uri.GetScheme().Lower();
    if ((scheme != "http" && scheme != "https") || uri.GetServer().empty())
        return {};
    wxString origin = scheme + "://" + uri.GetServer().Lower();
    if (uri.HasPort())
        origin += ":" + uri.GetPort();
    return origin;
}

std::string url_host(const std::string& url) { return wxURI(wxString::FromUTF8(url)).GetServer().Lower().utf8_string(); }

// "Open in slicer" links the model sites emit (MakerWorld → bambustudio, Thingiverse → cura,
// Printables → prusaslicer, Nexprint → elegooslicer). Downloader::start_download handles each.
bool is_slicer_open_url(const wxString& url)
{
    const std::string u = url.Lower().utf8_string();
    for (const char* scheme : {"orcaslicer://", "prusaslicer://", "bambustudio://", "bambustudioopen://", "cura://", "elegooslicer://"})
        if (boost::starts_with(u, scheme))
            return true;
    return false;
}

// A direct link to a model file (or a Thingiverse download / zip endpoint), which the pane
// imports instead of letting the webview save it to the Downloads folder.
bool is_model_file_url(const wxString& url)
{
    const wxURI    uri(url);
    const wxString scheme = uri.GetScheme().Lower();
    if (scheme != "http" && scheme != "https")
        return false;
    const std::string path = uri.GetPath().Lower().utf8_string();
    for (const char* ext : {".3mf", ".stl", ".step", ".stp", ".obj", ".amf", ".zip"})
        if (boost::ends_with(path, ext))
            return true;
    const std::string host = uri.GetServer().Lower().utf8_string();
    if (host == "www.thingiverse.com" || host == "thingiverse.com")
        return boost::starts_with(path, "/download:") || boost::ends_with(path, "/zip");
    return false;
}

// Hands a model link to the URL downloader, which saves it to the download folder and loads it
// into the plater. Deferred: this runs inside a native webview navigation callback.
void import_model_url(const wxString& url)
{
    std::string link = url.utf8_string();
    if (!is_slicer_open_url(url))
        link = "orcaslicer://open?file=" + Http::url_encode(link);
    BOOST_LOG_TRIVIAL(info) << "model site pane: importing " << WebView::RedactUrl(url).ToUTF8().data();
    wxGetApp().CallAfter([link] { wxGetApp().start_download(link); });
}

std::string sha256_hex(const std::string& data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    EVP_Digest(data.data(), data.size(), digest, &len, EVP_sha256(), nullptr);
    std::ostringstream out;
    for (unsigned int i = 0; i < len; ++i)
        out << std::hex << std::setw(2) << std::setfill('0') << int(digest[i]);
    return out.str();
}

std::string random_token()
{
    unsigned char bytes[32];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        return {};
    std::ostringstream out;
    for (unsigned char b : bytes)
        out << std::hex << std::setw(2) << std::setfill('0') << int(b);
    return out.str();
}

// Bare file name with path separators and control characters removed.
std::string safe_file_name(std::string name)
{
    name.erase(std::remove_if(name.begin(), name.end(),
                              [](unsigned char c) { return c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                                                           c == '"' || c == '<' || c == '>' || c == '|'; }),
               name.end());
    while (!name.empty() && (name.front() == '.' || name.front() == ' '))
        name.erase(name.begin());
    return name.substr(0, 120);
}

std::string lower_extension(const std::string& name)
{
    const auto dot = name.rfind('.');
    if (dot == std::string::npos)
        return {};
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}

fs::path webapp_cache_dir()
{
    const fs::path dir = fs::path(data_dir()) / "cache" / "webapp";
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

// Custom G-code lines that touch firmware, files, the network or safety limits, as Unlayered
// Slicer's install dialog lists them.
std::vector<std::string> risky_gcode_lines(const json& preset)
{
    static const std::regex risky(R"((^|[^A-Za-z0-9])M(997|30|28|29|559|560|500|502|552|553|554|587|588|589|564|302|143|906|913|208|669|584|999|98)([^0-9]|$))",
                                  std::regex::icase);
    std::vector<std::string> out;
    for (auto it = preset.begin(); it != preset.end(); ++it) {
        if (!boost::ends_with(it.key(), "_gcode") || !it.value().is_string())
            continue;
        std::istringstream lines(it.value().get<std::string>());
        std::string        line;
        while (std::getline(lines, line)) {
            const std::string code = line.substr(0, line.find(';'));
            if (std::regex_search(code, risky))
                out.push_back(it.key() + ": " + code);
        }
    }
    return out;
}

// Root of Unlayered's account provider (ACCOUNT_PROVIDER_PROTOCOL.md in the Unlayered Slicer source).
constexpr const char* UNLAYERED_PROVIDER     = "https://unlayered3d.com";
constexpr const char* UNLAYERED_SECRET_STORE = "UnifiedSlicer/UnlayeredAccount";

// The device token Unlayered's provider issued to this installation, kept in the OS credential
// store (Windows Credential Manager, Keychain, Secret Service) and cached for the session. Without
// a credential store it lives in memory for the session only.
std::string& unlayered_token_cache()
{
    static std::string token;
    static bool        loaded = false;
    if (!loaded) {
        loaded              = true;
        wxSecretStore store = wxSecretStore::GetDefault();
        wxString      user;
        wxSecretValue secret;
        if (store.IsOk() && store.Load(UNLAYERED_SECRET_STORE, user, secret) && secret.IsOk())
            token.assign(static_cast<const char*>(secret.GetData()), secret.GetSize());
    }
    return token;
}

std::string unlayered_token() { return unlayered_token_cache(); }

void unlayered_set_token(const std::string& token)
{
    unlayered_token_cache() = token;
    wxSecretStore store     = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return;
    if (token.empty())
        store.Delete(UNLAYERED_SECRET_STORE);
    else
        store.Save(UNLAYERED_SECRET_STORE, "device", wxSecretValue(wxString::FromUTF8(token)));
}

// One blocking JSON POST to the provider (worker threads only). Returns the HTTP status (0 when no
// answer) and fills `reply`.
unsigned unlayered_post(const std::string& path, const json& body, const std::string& token, json& reply)
{
    unsigned status = 0;
    std::string text;
    Http http = Http::post(std::string(UNLAYERED_PROVIDER) + path);
    http.header("User-Agent", std::string(SLIC3R_APP_KEY) + "/" + SLIC3R_VERSION)
        .header("Accept", "application/json")
        .header("Content-Type", "application/json")
        .timeout_connect(10)
        .timeout_max(30)
        .size_limit(64 * 1024)
        .set_post_body(body.dump());
    if (!token.empty())
        http.header("Authorization", "Bearer " + token);
    http.on_complete([&](std::string b, unsigned st) { text = std::move(b); status = st; })
        .on_error([&](std::string b, std::string, unsigned st) { text = std::move(b); status = st; })
        .perform_sync();
    reply = json::parse(text, nullptr, false);
    if (!reply.is_object())
        reply = json::object();
    return status;
}

// The refusal message a provider answer carries, if any.
std::string unlayered_error(const json& reply)
{
    if (reply.contains("error")) {
        const json& e = reply["error"];
        if (e.is_string())
            return e.get<std::string>();
        if (e.is_object() && e.contains("message") && e["message"].is_string())
            return e["message"].get<std::string>();
    }
    if (reply.contains("detail") && reply["detail"].is_string())
        return reply["detail"].get<std::string>();
    return {};
}

// A web page in its own window: a pop-up a model site opened (window.open / target=_blank, with
// its opener kept, so sign-in pop-ups can hand their result back), or a page the app opens itself
// such as a sign-in page. Pop-ups opened from it open the same way, and model links in it are
// imported like in the pane.
class ModelSiteWebDialog : public wxDialog
{
public:
    // A pop-up: hosts the child web view the event carries.
    ModelSiteWebDialog(wxWindow* parent, wxWebViewWindowFeatures* features)
        : wxDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        m_browser = features->GetChildWebView();
        m_browser->Create(this, wxID_ANY);
        wxSize size = features->GetSize();
        if (!size.IsFullySpecified() || size.x < 200 || size.y < 200)
            size = FromDIP(wxSize(560, 720));
        build(size, wxEmptyString);
    }

    // A page the app opens; `browser_label` non-empty adds a button that opens the same page in the
    // system browser instead.
    ModelSiteWebDialog(wxWindow* parent, const wxString& title, const wxString& url, const wxString& browser_label)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        m_browser = WebView::CreateWebView(this, url);
        build(FromDIP(wxSize(560, 760)), browser_label, url);
    }

    static void open_popup(wxWindow* parent, wxWebViewEvent& evt)
    {
        wxWebViewWindowFeatures* features = evt.GetTargetWindowFeatures();
        if (features == nullptr)
            return;
        auto* dlg = new ModelSiteWebDialog(wxGetTopLevelParent(parent), features);
        dlg->Show();
    }

    wxWebView* browser() const { return m_browser; }

private:
    void build(const wxSize& size, const wxString& browser_label, const wxString& url = wxString())
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        if (m_browser != nullptr) {
            sizer->Add(m_browser, 1, wxEXPAND);
            m_browser->Bind(wxEVT_WEBVIEW_TITLE_CHANGED, [this](wxWebViewEvent& e) {
                if (!e.GetString().empty())
                    SetTitle(e.GetString());
            });
            m_browser->Bind(wxEVT_WEBVIEW_WINDOW_CLOSE_REQUESTED, [this](wxWebViewEvent&) { CallAfter([this] { Destroy(); }); });
            m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, [](wxWebViewEvent& e) {
                if (is_slicer_open_url(e.GetURL()) || is_model_file_url(e.GetURL())) {
                    e.Veto();
                    import_model_url(e.GetURL());
                }
            });
            m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& e) {
                if (is_slicer_open_url(e.GetURL()) || is_model_file_url(e.GetURL())) {
                    e.Veto();
                    import_model_url(e.GetURL());
                }
            });
            m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW_FEATURES, [this](wxWebViewEvent& e) { open_popup(this, e); });
        }
        if (!browser_label.empty()) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->AddStretchSpacer();
            auto* btn = new wxButton(this, wxID_ANY, browser_label);
            btn->Bind(wxEVT_BUTTON, [url](wxCommandEvent&) { wxLaunchDefaultBrowser(url); });
            row->Add(btn, 0, wxALL, FromDIP(8));
            sizer->Add(row, 0, wxEXPAND);
        }
        SetSizer(sizer);
        SetSize(size);
        CentreOnParent();
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });
    }

    wxWebView* m_browser{nullptr};
};

} // namespace

const std::vector<std::string>& ModelSiteView::sites()
{
    static const std::vector<std::string> ids{"unlayered", "makerworld", "nexprint", "thingiverse"};
    return ids;
}

bool ModelSiteView::is_site(const std::string& site)
{
    const auto& ids = sites();
    return std::find(ids.begin(), ids.end(), site) != ids.end();
}

ModelSiteView::ModelSiteView(wxWindow* parent, const std::string& site)
    : wxPanel(parent, wxID_ANY), m_site(site), m_alive(std::make_shared<bool>(true))
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    if (m_site == "makerworld" || m_site == "thingiverse") {
        create_toolbar();
        sizer->Add(m_toolbar, 0, wxEXPAND | wxALL, FromDIP(8));
    }

    m_browser = WebView::CreateWebView(this, wxEmptyString);
    if (m_browser == nullptr) {
        SetSizer(sizer);
        return;
    }
    m_browser->SetWindowStyleFlag(wxNO_BORDER);

#ifdef __linux__
    // Keep the sites' sign-ins across restarts, as the cloud pane does.
    if (auto* wv = static_cast<WebKitWebView*>(m_browser->GetNativeBackend())) {
        const auto cookies_path = fs::path(data_dir() + "/cache/model_site_cookies.db");
        auto*      cookie_manager = webkit_web_context_get_cookie_manager(webkit_web_view_get_context(wv));
        webkit_cookie_manager_set_persistent_storage(cookie_manager, cookies_path.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    }
#endif
    apply_user_agent();

    // Consumed here, so the pane's events never reach the homepage handlers (webview events are
    // command events and would otherwise propagate to WebViewPanel).
    for (const auto& type : {wxEVT_WEBVIEW_NAVIGATING, wxEVT_WEBVIEW_NAVIGATED, wxEVT_WEBVIEW_LOADED, wxEVT_WEBVIEW_TITLE_CHANGED,
                             wxEVT_WEBVIEW_ERROR, wxEVT_WEBVIEW_NEWWINDOW, wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED})
        m_browser->Bind(type, &ModelSiteView::on_webview_event, this);
    // Pop-ups (sign-in windows above all) open in an app window that keeps its opener, so the site's
    // sign-in completes here rather than in the system browser.
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW_FEATURES, [this](wxWebViewEvent& evt) { ModelSiteWebDialog::open_popup(this, evt); });

    // A theme or language change: the user agent carries both, and Nexprint takes the theme in its
    // URL, so it reloads its start page; the other sites just reload (WebView::RecreateAll).
    m_browser->Bind(EVT_WEBVIEW_RECREATED, [this](wxCommandEvent& evt) {
        apply_user_agent();
        if (m_site == "nexprint" && m_loaded) {
            load_home();
            return;
        }
        evt.Skip();
    });

    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);
}

ModelSiteView::~ModelSiteView() { *m_alive = false; }

void ModelSiteView::apply_user_agent()
{
    if (m_browser != nullptr && m_site == "nexprint")
        m_browser->SetUserAgent(elegoo_user_agent());
}

wxString ModelSiteView::home_url() const
{
    AppConfig* cfg = wxGetApp().app_config;
    if (m_site == "unlayered") {
        const std::string url = cfg->get("model_site_unlayered_url");
        return wxString::FromUTF8(url.empty() ? UNLAYERED_UNIVERSE_URL : url);
    }
    if (m_site == "makerworld") {
        // Bambu Studio's "Online Models" page (WebViewPanel::SetMakerworldModelID there).
        const std::string host = wxGetApp().get_model_http_url(cfg->get_country_code());
        return wxString::FromUTF8(host) + language_code() + "/studio/webview?from=bambustudio";
    }
    if (m_site == "nexprint") {
        // ElegooSlicer's URL parameters (ElegooNetworkHelper::getOnlineModelsUrl).
        const bool  china = ElegooCloud::instance().region() == "CN";
        std::string url   = china ? NEXPRINT_MODELS_CHINA : NEXPRINT_MODELS_GLOBAL;
        url += "?language=" + language_code().utf8_string();
        url += "&region=" + std::string(china ? "CN" : "US");
        url += std::string("&theme=") + (wxGetApp().dark_mode() ? "dark" : "light");
        url += "&deviceId=" + elegoo_device_id();
        return wxString::FromUTF8(url);
    }
    if (m_site == "thingiverse")
        return "https://www.thingiverse.com";
    return {};
}

void ModelSiteView::load_home()
{
    if (m_browser == nullptr)
        return;
    m_loaded = true;
    const wxString url = home_url();
    if (m_site == "makerworld") {
        makerworld_load(url);
        return;
    }
    if (m_site == "unlayered" && !unlayered_token().empty() && unlayered_trusted(url)) {
        // Signed in to Unlayered: open the page signed in (ACCOUNT_PROVIDER_PROTOCOL.md §6).
        const wxURI uri(url);
        wxString    next = uri.GetPath();
        if (uri.HasQuery())
            next += "?" + uri.GetQuery();
        unlayered_web_session(next.utf8_string(), url);
        return;
    }
    // Deferred, so CreateWebView's queued setup (the "wx" script handler) runs before the first load.
    m_browser->CallAfter([this, url] { m_browser->LoadURL(url); });
}

void ModelSiteView::run_script(const wxString& script)
{
    if (m_browser != nullptr)
        WebView::RunScript(m_browser, script);
}

// ---------------------------------------------------------------------------------------------
// Toolbar (MakerWorld): Bambu Studio's Back / Refresh ... Open in browser row above the page.
// ---------------------------------------------------------------------------------------------

void ModelSiteView::create_toolbar()
{
    m_toolbar               = new wxPanel(this);
    const wxColour bg       = StateColor::darkModeColorFor(*wxWHITE);
    const std::string color = StateColor::darkModeColorFor(wxColour("#262E30")).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    const std::string off   = StateColor::darkModeColorFor(wxColour("#C0BABA")).GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    m_toolbar->SetBackgroundColour(bg);
    SetBackgroundColour(bg);

    auto make_button = [this, &bg, &color, &off](const std::string& icon, const wxString& tooltip) {
        auto* btn = new wxBitmapButton(m_toolbar, wxID_ANY, create_scaled_bitmap(icon, this, 18, false, color), wxDefaultPosition,
                                       wxDefaultSize, wxBORDER_NONE);
        btn->SetBitmapDisabled(create_scaled_bitmap(icon, this, 18, false, off));
        btn->SetToolTip(tooltip);
        btn->SetBackgroundColour(bg);
        btn->SetMinSize(FromDIP(wxSize(28, 28)));
        btn->Enable(false);
        return btn;
    };
    m_back_btn         = make_button("mall_control_back", _L_CONTEXT("Back", "Navigation"));
    m_refresh_btn      = make_button("mall_control_refresh", _L("Refresh"));
    m_open_browser_btn = make_button("open_in_browser", _L("Open in browser"));
    if (m_site == "thingiverse") {
        // Sign-in and confirmation links that arrive by email: copy the link, then open it here so
        // the sign-in lands in this pane rather than in the system browser.
        m_open_link_btn = new wxButton(m_toolbar, wxID_ANY, _L("Open copied link"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        m_open_link_btn->SetToolTip(_L("Opens the web link on the clipboard in this page, for example a sign-in link from an email."));
        m_open_link_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { open_clipboard_link(); });
    }

    m_back_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_browser && m_browser->CanGoBack())
            m_browser->GoBack();
        update_toolbar();
    });
    m_refresh_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_browser)
            m_browser->Reload();
    });
    m_open_browser_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_browser)
            return;
        const wxString url = m_browser->GetCurrentURL();
        if (url.StartsWith("http"))
            wxLaunchDefaultBrowser(url);
    });

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(m_back_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(m_refresh_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->AddStretchSpacer(1);
    if (m_open_link_btn != nullptr)
        row->Add(m_open_link_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    row->Add(m_open_browser_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_toolbar->SetSizer(row);
}

void ModelSiteView::open_clipboard_link()
{
    wxString text;
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT) || wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            text = data.GetText();
        }
        wxTheClipboard->Close();
    }
    text.Trim().Trim(false);
    if (!text.StartsWith("https://") && !text.StartsWith("http://")) {
        MessageDialog(this, _L("Copy the link first (right-click it in the email and choose Copy link), then press this button."),
                      _L("Open copied link"), wxOK | wxICON_INFORMATION)
            .ShowModal();
        return;
    }
    if (m_browser != nullptr)
        m_browser->LoadURL(text);
}

void ModelSiteView::update_toolbar()
{
    if (m_toolbar == nullptr || m_browser == nullptr)
        return;
    const bool page = m_browser->GetCurrentURL().StartsWith("http");
    m_back_btn->Enable(m_browser->CanGoBack());
    m_refresh_btn->Enable(page);
    m_open_browser_btn->Enable(page);
}

// ---------------------------------------------------------------------------------------------
// Web view events
// ---------------------------------------------------------------------------------------------

void ModelSiteView::on_webview_event(wxWebViewEvent& evt)
{
    const wxEventType type = evt.GetEventType();
    const wxString    url  = evt.GetURL();
    if (type == wxEVT_WEBVIEW_NAVIGATING) {
        if (is_slicer_open_url(url) || is_model_file_url(url)) {
            evt.Veto();
            import_model_url(url);
            return;
        }
        // Unlayered's sign-in link: signed in by the app, as Unlayered Slicer does (§6).
        if (m_site == "unlayered" && is_unlayered_login(url)) {
            evt.Veto();
            unlayered_sign_in(unlayered_login_next(url));
            return;
        }
        // A new document: the bridge token of the old one is void (Unlayered).
        if (url == evt.GetTarget() || evt.GetTarget().empty())
            m_bridge_token.clear();
    } else if (type == wxEVT_WEBVIEW_NEWWINDOW) {
        if (is_slicer_open_url(url) || is_model_file_url(url)) {
            evt.Veto();
            import_model_url(url);
        } else if (m_site == "unlayered" && is_unlayered_login(url)) {
            evt.Veto();
            unlayered_sign_in(unlayered_login_next(url));
        } else if (!url.empty() && !url.StartsWith("http://") && !url.StartsWith("https://") && !url.StartsWith("about:")) {
            // mailto: and other apps' links.
            evt.Veto();
            wxLaunchDefaultBrowser(url);
        }
        // Anything else opens as a pop-up window of the app (wxEVT_WEBVIEW_NEWWINDOW_FEATURES).
    } else if (type == wxEVT_WEBVIEW_LOADED) {
        if (m_site == "unlayered" && unlayered_trusted(m_browser->GetCurrentURL()))
            unlayered_inject_bridge();
        update_toolbar();
    } else if (type == wxEVT_WEBVIEW_NAVIGATED || type == wxEVT_WEBVIEW_ERROR) {
        update_toolbar();
    } else if (type == wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED) {
        on_script_message(evt.GetString().utf8_string());
    }
}

void ModelSiteView::on_script_message(const std::string& body)
{
    if (m_site == "makerworld") {
        makerworld_message(body);
        return;
    }
    const json msg = json::parse(body, nullptr, false);
    if (!msg.is_object())
        return;
    if (m_site == "nexprint")
        nexprint_message(msg);
    else if (m_site == "unlayered")
        unlayered_message(msg);
}

// ---------------------------------------------------------------------------------------------
// MakerWorld
// ---------------------------------------------------------------------------------------------

void ModelSiteView::makerworld_message(const std::string& body)
{
    // The page speaks the homepage command set (makerworld_model_open, common_openurl, ...), which
    // GUI_App::handle_web_request already serves; a reply goes back the way Bambu Studio sends it.
    std::string response = wxGetApp().handle_web_request(body);
    response.erase(std::remove(response.begin(), response.end(), '\n'), response.end());
    if (response.empty())
        return;
    run_script("window.postMessage(" + wxString::FromUTF8(json(response).dump()) + ")");
}

void ModelSiteView::makerworld_load(const wxString& target)
{
    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent == nullptr || !agent->is_user_login(BBL_CLOUD_PROVIDER)) {
        m_browser->CallAfter([this, target] { m_browser->LoadURL(target); });
        return;
    }
    // Signed in to Bambu: carry the session over with a bind ticket, as Bambu Studio does. The ticket
    // is a network request, so it is fetched off the UI thread.
    const std::string host  = wxGetApp().get_model_http_url(wxGetApp().app_config->get_country_code());
    std::weak_ptr<bool> alive = m_alive;
    std::thread([this, alive, target, host] {
        std::string ticket;
        int         ret  = -1;
        auto&       bbl  = BBLNetworkPlugin::instance();
        auto        func = bbl.get_request_bind_ticket();
        if (func && bbl.get_agent())
            ret = func(bbl.get_agent(), &ticket);
        wxString url = target;
        if (ret == 0 && !ticket.empty())
            url = wxString::FromUTF8(host + "api/sign-in/ticket?to=" + Http::url_encode(target.utf8_string()) + "&ticket=" + ticket);
        else
            BOOST_LOG_TRIVIAL(warning) << "MakerWorld pane: no bind ticket (" << ret << "), loading signed out";
        wxGetApp().CallAfter([this, alive, url] {
            if (auto a = alive.lock(); a && *a && m_browser)
                m_browser->LoadURL(url);
        });
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// Nexprint: ElegooSlicer's online-models page protocol (OnlineModelsHomepageView there).
// ---------------------------------------------------------------------------------------------

json ModelSiteView::nexprint_user_info() const
{
    const ElegooCloud& cloud = ElegooCloud::instance();
    if (!cloud.signed_in())
        return {{"userId", ""}, {"accessToken", ""}, {"refreshToken", ""}, {"loginStatus", -1}};
    const ElegooCloudAccount a = cloud.account();
    return {{"userId", a.user_id},
            {"accessToken", a.access_token},
            {"refreshToken", a.refresh_token},
            {"expiresTime", a.access_expire_ms},
            {"refreshTokenExpireTime", 0},
            {"lastTokenRefreshTime", 0},
            {"avatar", a.avatar},
            {"email", a.email},
            {"nickname", a.nickname},
            {"loginStatus", 1}};
}

void ModelSiteView::nexprint_respond(const std::string& id, const std::string& method, int code, const json& data,
                                     const std::string& message)
{
    const json reply = {{"id", id}, {"method", method}, {"type", "response"}, {"code", code}, {"message", message}, {"data", data}};
    run_script(wxString::FromUTF8("window.HandleStudio && window.HandleStudio(" + reply.dump() + ")"));
}

void ModelSiteView::nexprint_event(const std::string& method, const json& data)
{
    const json event = {{"id", random_token().substr(0, 16)}, {"method", method}, {"type", "event"}, {"data", data}};
    run_script(wxString::FromUTF8("window.HandleStudio && window.HandleStudio(" + event.dump() + ")"));
}

void ModelSiteView::nexprint_message(const json& msg)
{
    if (msg.value("type", "request") != "request")
        return;
    const std::string method = msg.value("method", "");
    const std::string id     = msg.contains("id") && msg["id"].is_string() ? msg["id"].get<std::string>() : "";
    const json        params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();

    if (method == "report.getClientUserInfo" || method == "report.refreshToken") {
        // ElegooCloud refreshes its own tokens on use, so a refresh request gets the current ones.
        nexprint_respond(id, method, 0, nexprint_user_info());
    } else if (method == "report.notLogged") {
        if (ElegooCloud::instance().signed_in()) {
            nexprint_respond(id, method, 0, nexprint_user_info());
            return;
        }
        nexprint_respond(id, method, -1, json::object(), "not logged in");
        std::weak_ptr<bool> alive = m_alive;
        wxGetApp().CallAfter([this, alive] {
            if (ElegooLoginDialog::sign_in(wxGetApp().mainframe))
                if (auto a = alive.lock(); a && *a)
                    nexprint_event("client.onUserInfoUpdated", nexprint_user_info());
        });
    } else if (method == "report.ready") {
        nexprint_respond(id, method, 0, json::object());
        nexprint_event("client.setDeviceId", {{"deviceId", elegoo_device_id()}});
        if (ElegooCloud::instance().signed_in())
            nexprint_event("client.onUserInfoUpdated", nexprint_user_info());
    } else if (method == "report.slicerOpen") {
        nexprint_respond(id, method, 0, json::object());
        const std::string url = params.value("url", "");
        if (!url.empty())
            wxGetApp().CallAfter([url] { wxGetApp().request_model_download(wxString::FromUTF8(url)); });
    } else if (method == "report.websiteOpen") {
        nexprint_respond(id, method, 0, json::object());
        const std::string url = params.value("url", "");
        if (boost::starts_with(url, "https://") || boost::starts_with(url, "http://"))
            wxGetApp().CallAfter([url] { wxLaunchDefaultBrowser(wxString::FromUTF8(url)); });
    } else if (method == "reload") {
        nexprint_respond(id, method, 0, json::object());
        CallAfter([this] { load_home(); });
    } else if (method == "isLoading") {
        nexprint_respond(id, method, 0, {{"isLoading", m_browser != nullptr && m_browser->IsBusy()}});
    } else {
        nexprint_respond(id, method, 0, json::object());
    }
}

// ---------------------------------------------------------------------------------------------
// Unlayered Universe: the part of Unlayered Slicer's web-app bridge (protocol v1) its Universe
// page uses. Same transport, envelope, token and error codes, so the page runs unchanged.
// ---------------------------------------------------------------------------------------------

bool ModelSiteView::unlayered_trusted(const wxString& url) const
{
    const wxString origin = http_origin(url);
    return origin == UNLAYERED_ORIGIN || origin == "https://www.unlayered3d.com";
}

void ModelSiteView::unlayered_inject_bridge()
{
    m_bridge_token = random_token();
    if (m_bridge_token.empty())
        return;
    // The shim Unlayered Slicer injects (WebAppBridge::Impl::inject_bridge_script there).
    const wxString script = wxString::Format(
        "(function(){"
        "  var TOKEN=%s;"
        "  var seq=0, pending={}, listeners={};"
        "  function send(method, params){"
        "    return new Promise(function(resolve, reject){"
        "      if(!window.wx || typeof window.wx.postMessage!=='function'){"
        "        reject({code:'no-bridge', message:'not running inside the slicer'}); return;"
        "      }"
        "      var id='wa-'+(++seq);"
        "      var t=setTimeout(function(){ if(pending[id]){ delete pending[id];"
        "        reject({code:'timeout', message:'no response from the slicer', method:method}); } }, 60000);"
        "      pending[id]={resolve:resolve, reject:reject, timer:t};"
        "      window.wx.postMessage(JSON.stringify({v:1, id:id, method:method, token:TOKEN, params:params||{}}));"
        "    });"
        "  }"
        "  function emit(evt, data){"
        "    var fns=listeners[evt]||[];"
        "    for(var i=0;i<fns.length;i++){ try{ fns[i](data); }catch(e){} }"
        "  }"
        "  function recv(p){"
        "    if(!p) return;"
        "    if(typeof p==='string'){ try{ p=JSON.parse(p); }catch(e){ return; } }"
        "    if(p.type==='event'){ emit(p.method, p.data); return; }"
        "    var entry=pending[p.id];"
        "    if(!entry) return;"
        "    delete pending[p.id]; clearTimeout(entry.timer);"
        "    if(p.code===0) entry.resolve(p.data||{});"
        "    else entry.reject({code:p.code, message:p.message||'error', method:p.method, data:p.data||{}});"
        "  }"
        "  function on(evt, fn){ (listeners[evt]=listeners[evt]||[]).push(fn); }"
        "  function off(evt, fn){ var a=listeners[evt]||[]; var i=a.indexOf(fn); if(i>=0) a.splice(i,1); }"
        "  window.__slicerBridge=Object.freeze({version:1, token:TOKEN, send:send, on:on, off:off, _recv:recv});"
        "  try{ window.dispatchEvent(new Event('slicerbridgeready')); }catch(e){}"
        "})();",
        wxString::FromUTF8(json(m_bridge_token).dump()));
    run_script(script);
}

void ModelSiteView::unlayered_respond(const std::string& id, const std::string& method, int code, const std::string& message,
                                      const json& data)
{
    if (id.empty())
        return;
    const json body = {{"type", "response"}, {"v", 1}, {"id", id}, {"method", method}, {"code", code}, {"message", message}, {"data", data}};
    run_script("(function(p){ if (window.__slicerBridge && typeof window.__slicerBridge._recv === 'function')"
               " return window.__slicerBridge._recv(p); window.postMessage(p, window.location.origin); })(" +
               wxString::FromUTF8(body.dump()) + ");");
}

void ModelSiteView::unlayered_event(const std::string& id, const std::string& method, const json& data)
{
    json body = {{"type", "event"}, {"v", 1}, {"method", method}, {"data", data}};
    if (!id.empty())
        body["id"] = id;
    run_script("(function(p){ if (window.__slicerBridge && typeof window.__slicerBridge._recv === 'function')"
               " return window.__slicerBridge._recv(p); window.postMessage(p, window.location.origin); })(" +
               wxString::FromUTF8(body.dump()) + ");");
}

void ModelSiteView::unlayered_message(const json& msg)
{
    std::string method = msg.value("method", "");
    if (method.empty())
        method = msg.value("command", "");
    if (!boost::starts_with(method, "slicer."))
        return; // the channel is shared; anything else is not ours
    const std::string id     = msg.contains("id") && msg["id"].is_string() ? msg["id"].get<std::string>() : "";
    json              params = msg.contains("params") ? msg["params"] : msg.value("data", json::object());
    if (!params.is_object())
        params = json::object();

    if (!unlayered_trusted(m_browser->GetCurrentURL())) {
        unlayered_respond(id, method, 1003, "origin not permitted");
        return;
    }
    if (m_bridge_token.empty() || msg.value("token", "") != m_bridge_token) {
        unlayered_respond(id, method, 1004, "missing or stale session token");
        return;
    }

    if (method == "slicer.getInfo") {
#ifdef _WIN32
        const char* os = "windows";
#elif defined(__APPLE__)
        const char* os = "macos";
#else
        const char* os = "linux";
#endif
        json info = {
            {"api", {{"version", 1}, {"envelope", "slicer-rpc/1"}}},
            {"app",
             {{"name", SLIC3R_APP_NAME},
              {"version", SLIC3R_VERSION},
              {"sourceUrl", "https://github.com/Potatolord1965/Unified-Slicer-Vibecoded"},
              {"license", "AGPL-3.0"}}},
            {"platform", {{"os", os}, {"locale", language_code().utf8_string()}, {"darkMode", wxGetApp().dark_mode()}}},
            {"capabilities", {"slicer.getInfo", "slicer.openProjectFromUrl", "slicer.selectPreset", "slicer.installPrinterPreset"}},
            {"limits", {{"maxDownloadBytes", UNLAYERED_MAX_DOWNLOAD}, {"fileTypes", UNLAYERED_FILE_TYPES}}},
            {"origin", {{"configured", UNLAYERED_ORIGIN}, {"trusted", true}, {"loopback", false}}},
            {"tab", {{"slot", "universe_app"}, {"title", "Universe"}, {"layout", "tab"}}},
            {"settingsAccess", false},
            {"printerAccess", false},
            {"account", {{"signedIn", !unlayered_token().empty()}, {"provider", UNLAYERED_PROVIDER}}},
            {"features", json::array()},
        };
        unlayered_respond(id, method, 0, "success", info);
    } else if (method == "slicer.openProjectFromUrl") {
        unlayered_open_from_url(id, params);
    } else if (method == "slicer.installPrinterPreset") {
        unlayered_install_preset(id, params);
    } else if (method == "slicer.selectPreset") {
        unlayered_select_preset(id, params);
    } else {
        unlayered_respond(id, method, 1001, "unknown method");
    }
}

bool ModelSiteView::is_unlayered_login(const wxString& url)
{
    const wxURI uri(url);
    return http_origin(url) == UNLAYERED_PROVIDER && (uri.GetPath() == "/login" || uri.GetPath() == "/login/");
}

std::string ModelSiteView::unlayered_login_next(const wxString& url)
{
    // `next` must be a path on the provider; anything else falls back to the Universe page.
    const wxURI uri(url);
    for (const wxString& pair : wxSplit(uri.GetQuery(), '&')) {
        if (!pair.StartsWith("next="))
            continue;
        const std::string next = Http::url_decode(pair.Mid(5).utf8_string());
        if (boost::starts_with(next, "/") && !boost::starts_with(next, "//"))
            return next;
    }
    return "/universe?host=unlayered-slicer";
}

void ModelSiteView::unlayered_web_session(const std::string& next, const wxString& fallback)
{
    std::weak_ptr<bool> alive = m_alive;
    const std::string   token = unlayered_token();
    std::thread([this, alive, next, fallback, token] {
        json           reply;
        const unsigned status = unlayered_post("/api/slicer/web-session", {{"next", next}}, token, reply);
        std::string    path   = reply.value("path", "");
        const bool     ok     = status >= 200 && status < 300 && boost::starts_with(path, "/api/slicer/web-session?code=") &&
                         path.find_first_of(" \t\r\n") == std::string::npos;
        wxGetApp().CallAfter([this, alive, ok, path, status, fallback] {
            if (status == 401)
                unlayered_set_token({}); // the device token is no longer valid
            auto a = alive.lock();
            if (!a || !*a || m_browser == nullptr)
                return;
            if (ok)
                m_browser->LoadURL(wxString::FromUTF8(std::string(UNLAYERED_PROVIDER) + path));
            else if (!fallback.empty())
                m_browser->LoadURL(fallback);
        });
    }).detach();
}

// Unlayered Slicer's device hand-off (ACCOUNT_PROVIDER_PROTOCOL.md §2): the provider's sign-in
// page opens in an app window (with a button for the system browser instead), the app collects
// the device token once the person has signed in there, and the pane is then signed in (§6).
void ModelSiteView::unlayered_sign_in(const std::string& next)
{
    const wxString fallback = wxString::FromUTF8(std::string(UNLAYERED_PROVIDER) + next);
    if (!unlayered_token().empty()) {
        unlayered_web_session(next, fallback);
        return;
    }
    if (m_signing_in)
        return;
    m_signing_in = true;

    std::weak_ptr<bool> alive = m_alive;
    const std::string   label = std::string(SLIC3R_APP_NAME) + " " + SLIC3R_VERSION + " on " + wxGetHostName().utf8_string();
    std::thread([this, alive, next, fallback, label] {
        auto finish = [this, alive](const std::string& error) {
            wxGetApp().CallAfter([this, alive, error] {
                auto a = alive.lock();
                if (!a || !*a)
                    return;
                m_signing_in = false;
                if (m_sign_in_dialog != nullptr) {
                    m_sign_in_dialog->Destroy();
                    m_sign_in_dialog = nullptr;
                }
                if (!error.empty())
                    MessageDialog(this, from_u8(error), _L("Sign in to Unlayered"), wxOK | wxICON_WARNING).ShowModal();
            });
        };

        json     reply;
        unsigned status = unlayered_post("/api/login/handoff/start", json::object(), {}, reply);
        const std::string id = reply.value("id", ""), secret = reply.value("secret", "");
        if (status < 200 || status >= 300 || id.empty() || secret.empty()) {
            const std::string why = unlayered_error(reply);
            finish(why.empty() ? _u8L("Could not reach the Unlayered sign-in service. Check the internet connection and try again.") : why);
            return;
        }

        const wxString page = wxString::FromUTF8(std::string(UNLAYERED_PROVIDER) + "/auth/slicer?id=" + Http::url_encode(id));
        wxGetApp().CallAfter([this, alive, page] {
            auto a = alive.lock();
            if (!a || !*a)
                return;
            m_sign_in_dialog = new ModelSiteWebDialog(wxGetTopLevelParent(this), _L("Sign in to Unlayered"), page,
                                                      _L("Use my web browser instead"));
            // Closing the window does not stop the sign-in: it may be finishing in the browser.
            m_sign_in_dialog->Bind(wxEVT_DESTROY, [this, alive](wxWindowDestroyEvent& e) {
                if (auto a2 = alive.lock(); a2 && *a2 && e.GetEventObject() == m_sign_in_dialog)
                    m_sign_in_dialog = nullptr;
                e.Skip();
            });
            m_sign_in_dialog->Show();
        });

        // Poll every 2 s for up to 290 s; no answer at all counts as pending.
        for (int i = 0; i < 145; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (auto a = alive.lock(); !a || !*a)
                return;
            json     r;
            unsigned st = unlayered_post("/api/slicer/device/collect", {{"id", id}, {"secret", secret}, {"label", label}}, {}, r);
            if (st == 0 || (st >= 200 && st < 300 && r.value("pending", false)))
                continue;
            const std::string token = r.value("token", "");
            if (st >= 200 && st < 300 && !token.empty()) {
                wxGetApp().CallAfter([this, alive, token, next, fallback] {
                    unlayered_set_token(token);
                    if (auto a2 = alive.lock(); a2 && *a2)
                        unlayered_web_session(next, fallback);
                });
                finish({});
                return;
            }
            const std::string why = unlayered_error(r);
            finish(why.empty() ? _u8L("Signing in to Unlayered failed.") : why);
            return;
        }
        finish(_u8L("Signing in to Unlayered timed out. Please try again."));
    }).detach();
}

// Common checks of the two download methods. Returns 0, or the error code with `error` set.
static int check_unlayered_download(const json& params, const std::string& page_url, std::string& url, std::string& filename,
                                    std::string& error)
{
    url = params.value("url", "");
    if (!boost::starts_with(url, "https://")) {
        error = "url must be https";
        return 1002;
    }
    // The page's own host, or the storage its downloads redirect to (Unlayered Slicer ships
    // ".vercel-storage.com" as the Universe slot's download hosts).
    const std::string host = url_host(url);
    const std::string page = url_host(page_url);
    if (host.empty() || !(host == page || boost::ends_with(host, ".vercel-storage.com") || host == "unlayered3d.com" ||
                          boost::ends_with(host, ".unlayered3d.com"))) {
        error = "download host not permitted";
        return 1003;
    }
    filename = safe_file_name(params.value("filename", ""));
    if (filename.empty())
        filename = safe_file_name(wxURI(wxString::FromUTF8(url)).GetPath().AfterLast('/').utf8_string());
    return 0;
}

void ModelSiteView::unlayered_open_from_url(const std::string& id, const json& params)
{
    const std::string method = "slicer.openProjectFromUrl";
    std::string       url, filename, error;
    if (int code = check_unlayered_download(params, m_browser->GetCurrentURL().utf8_string(), url, filename, error)) {
        unlayered_respond(id, method, code, error);
        return;
    }
    const std::string ext = lower_extension(filename);
    if (std::find(UNLAYERED_FILE_TYPES.begin(), UNLAYERED_FILE_TYPES.end(), ext) == UNLAYERED_FILE_TYPES.end()) {
        unlayered_respond(id, method, 1009, "unsupported file type");
        return;
    }
    Plater* plater = wxGetApp().plater();
    if (m_bridge_busy || plater == nullptr || plater->is_background_process_slicing()) {
        unlayered_respond(id, method, 1007, "busy");
        return;
    }
    unlayered_respond(id, method, 0, "success", json::object());

    m_bridge_busy                = true;
    const std::string   expected = params.value("sha256", "");
    std::weak_ptr<bool> alive    = m_alive;
    auto                last_mib = std::make_shared<size_t>(0);
    Http::get(url)
        .size_limit(UNLAYERED_MAX_DOWNLOAD)
        .timeout_max(600)
        .on_progress([this, alive, id, last_mib](Http::Progress p, bool&) {
            const size_t mib = p.dlnow >> 20;
            if (mib == *last_mib)
                return;
            *last_mib = mib;
            const json data = {{"received", p.dlnow}, {"total", p.dltotal}};
            wxGetApp().CallAfter([this, alive, id, data] {
                if (auto a = alive.lock(); a && *a)
                    unlayered_event(id, "slicer.openProjectFromUrl.progress", data);
            });
        })
        .on_complete([this, alive, id, filename, ext, expected](std::string body, unsigned) {
            json        result;
            fs::path    path;
            if (!expected.empty() && sha256_hex(body) != expected) {
                result = {{"ok", false}, {"error", "checksum mismatch"}, {"code", 1010}};
            } else {
                path = webapp_cache_dir() / fs::path(filename);
                boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
                out.write(body.data(), std::streamsize(body.size()));
                out.close();
                result = out ? json{{"ok", true}, {"path", path.string()}} : json{{"ok", false}, {"error", "could not save the file"}, {"code", 1010}};
            }
            wxGetApp().CallAfter([this, alive, id, result, path, ext] {
                if (result.value("ok", false)) {
                    if (MainFrame* frame = wxGetApp().mainframe)
                        frame->select_tab(wxString(TAB_ID_PREPARE));
                    if (Plater* plater = wxGetApp().plater()) {
                        if (ext == "3mf")
                            plater->load_project(from_path(path));
                        else
                            plater->load_files(std::vector<fs::path>{path}, LoadStrategy::LoadModel);
                    }
                }
                if (auto a = alive.lock(); a && *a) {
                    m_bridge_busy = false;
                    unlayered_event(id, "slicer.projectOpened", result);
                }
            });
        })
        .on_error([this, alive, id](std::string, std::string error, unsigned status) {
            const json result = {{"ok", false}, {"error", status ? "HTTP " + std::to_string(status) : error}, {"code", 1010}};
            wxGetApp().CallAfter([this, alive, id, result] {
                if (auto a = alive.lock(); a && *a) {
                    m_bridge_busy = false;
                    unlayered_event(id, "slicer.projectOpened", result);
                }
            });
        })
        .perform();
}

void ModelSiteView::unlayered_select_preset(const std::string& id, const json& params)
{
    const std::string method = "slicer.selectPreset";
    if (params.value("type", "printer") != "printer") {
        unlayered_respond(id, method, 1005, "only printer presets can be selected from this page");
        return;
    }
    std::string name = params.value("name", "");
    if (boost::ends_with(name, " (modified)"))
        name.resize(name.size() - 11);
    PresetBundle* bundle = wxGetApp().preset_bundle;
    const Preset* preset = bundle ? bundle->printers.find_preset(name, false) : nullptr;
    if (preset == nullptr && bundle != nullptr)
        for (const Preset& p : bundle->printers)
            if (p.is_visible && p.alias == name) {
                preset = &p;
                break;
            }
    if (preset == nullptr) {
        unlayered_respond(id, method, 1008, "not found");
        return;
    }
    const std::string target  = preset->name;
    const bool        changed = bundle->printers.get_selected_preset_name() != target;
    if (changed)
        if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER))
            tab->select_preset(target);
    const std::string selected = bundle->printers.get_selected_preset_name();
    unlayered_respond(id, method, 0, "success", {{"type", "printer"}, {"selected", selected}, {"changed", changed && selected == target}});
}

void ModelSiteView::unlayered_install_preset(const std::string& id, const json& params)
{
    const std::string method = "slicer.installPrinterPreset";
    std::string       url, filename, error;
    if (int code = check_unlayered_download(params, m_browser->GetCurrentURL().utf8_string(), url, filename, error)) {
        unlayered_respond(id, method, code, error);
        return;
    }
    if (!filename.empty() && lower_extension(filename) != "json") {
        unlayered_respond(id, method, 1009, "a printer profile is a .json file");
        return;
    }
    Plater* plater = wxGetApp().plater();
    if (m_bridge_busy || plater == nullptr || plater->is_background_process_slicing()) {
        unlayered_respond(id, method, 1007, "busy");
        return;
    }
    unlayered_respond(id, method, 0, "success", {{"accepted", true}});

    m_bridge_busy                = true;
    const std::string   expected = params.value("sha256", "");
    std::weak_ptr<bool> alive    = m_alive;
    auto finish = [this, alive, id](json result) {
        if (auto a = alive.lock(); a && *a) {
            m_bridge_busy = false;
            unlayered_event(id, "slicer.presetInstalled", result);
        }
    };

    Http::get(url)
        .size_limit(UNLAYERED_MAX_PRESET)
        .timeout_max(60)
        .on_complete([finish, expected](std::string body, unsigned) {
            wxGetApp().CallAfter([finish, expected, body] {
                if (!expected.empty() && sha256_hex(body) != expected) {
                    finish({{"ok", false}, {"error", "checksum mismatch"}, {"code", 1010}});
                    return;
                }
                json preset = json::parse(body, nullptr, false);
                if (!preset.is_object() || !preset.contains("printer_settings_id") || !preset.contains("name") ||
                    !preset["name"].is_string() || !preset.contains("version")) {
                    finish({{"ok", false}, {"error", "not a printer profile"}, {"code", 1009}});
                    return;
                }
                PresetBundle*     bundle = wxGetApp().preset_bundle;
                const std::string name   = preset["name"].get<std::string>();
                const Preset*     same   = bundle->printers.find_preset(name, false);
                if (name.empty() || (same != nullptr && (same->is_system || same->is_default))) {
                    finish({{"ok", false}, {"error", "a system printer preset already has this name"}, {"code", 1002}});
                    return;
                }

                // Sanitise: no print host or local paths, whatever the file says; and a parent that is
                // not installed here is dropped (shared profiles are flattened, so they stand alone).
                for (const std::string& key : UNLAYERED_SECRET_KEYS)
                    preset.erase(key);
                const std::string inherits = preset.value("inherits", "");
                if (!inherits.empty() && bundle->printers.find_preset(inherits, false) == nullptr)
                    preset["inherits"] = "";
                preset["from"] = "User";

                // Confirm in a native dialog the page cannot answer.
                auto field = [&preset](const char* key) {
                    if (!preset.contains(key))
                        return std::string("-");
                    const json& v = preset[key];
                    if (v.is_array()) {
                        std::string out;
                        for (const json& e : v)
                            out += (out.empty() ? "" : ", ") + (e.is_string() ? e.get<std::string>() : e.dump());
                        return out;
                    }
                    return v.is_string() ? v.get<std::string>() : v.dump();
                };
                wxString text = format_wxstr(_L("Install the printer profile \"%1%\" offered by Unlayered Universe?"), name) + "\n\n";
                if (same != nullptr)
                    text += _L("It replaces your own printer preset of the same name.") + "\n";
                text += _L("Printer model") + ": " + from_u8(field("printer_model")) + "\n";
                text += _L("Bed shape") + ": " + from_u8(field("printable_area")) + "\n";
                text += _L("Nozzle diameter") + ": " + from_u8(field("nozzle_diameter")) + "\n";
                text += _L("G-code flavor") + ": " + from_u8(field("gcode_flavor")) + "\n";
                if (!inherits.empty())
                    text += _L("Based on") + ": " + from_u8(inherits) + "\n";
                const std::vector<std::string> risky = risky_gcode_lines(preset);
                if (!risky.empty()) {
                    text += "\n" + _L("Its custom G-code changes firmware, files, network or safety settings:") + "\n";
                    for (size_t i = 0; i < risky.size() && i < 15; ++i)
                        text += "  " + from_u8(risky[i]) + "\n";
                    if (risky.size() > 15)
                        text += "  ...\n";
                }
                MessageDialog dlg(wxGetApp().mainframe, text, _L("Install printer profile from Unlayered Universe"),
                                  wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
                if (dlg.ShowModal() != wxID_YES) {
                    finish({{"ok", false}, {"cancelled", true}});
                    return;
                }

                const fs::path path = webapp_cache_dir() / fs::path(safe_file_name(name) + ".json");
                {
                    boost::nowide::ofstream out(path.string(), std::ios::trunc);
                    out << preset.dump(4);
                }
                std::vector<std::string>   files{path.string()};
                PresetBundle::ImportReport report;
                // The dialog already said whether it replaces the user's preset: "Yes" to overwrite.
                bundle->import_presets(files, [](const std::string&) { return 1; }, ForwardCompatibilitySubstitutionRule::Enable,
                                       *wxGetApp().app_config, &report);
                boost::system::error_code ec;
                fs::remove(path, ec);
                if (report.imported.empty()) {
                    finish({{"ok", false}, {"error", "the import failed"}, {"code", 1012}});
                    return;
                }
                wxGetApp().load_current_presets();
                if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER))
                    tab->select_preset(name);
                finish({{"ok", true}, {"name", name}, {"selected", bundle->printers.get_selected_preset_name()}});
            });
        })
        .on_error([finish](std::string, std::string error, unsigned status) {
            const json result = {{"ok", false}, {"error", status ? "HTTP " + std::to_string(status) : error}, {"code", 1010}};
            wxGetApp().CallAfter([finish, result] { finish(result); });
        })
        .perform();
}

}} // namespace Slic3r::GUI
