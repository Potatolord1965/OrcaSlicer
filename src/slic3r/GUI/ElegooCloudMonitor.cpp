#include "ElegooCloudMonitor.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <map>
#include <thread>
#include <vector>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <wx/webview.h>

#include "DeviceHub/DeviceHubPanel.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/ElegooCloud.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

constexpr int POLL_MS       = 4000;
constexpr int CANVAS_EVERY  = 8; // polls between Canvas reads

wxString state_text(const std::string& state)
{
    if (state == "RUNNING") return _L("Printing");
    if (state == "PAUSE") return _L("Paused");
    if (state == "PREPARE") return _L("Preparing");
    if (state == "FINISH") return _L("Finished");
    if (state == "FAILED") return _L("Failed");
    return _L("Idle");
}

wxString temp_text(double current, double target)
{
    return target > 0 ? wxString::Format("%.0f / %.0f °C", current, target) : wxString::Format("%.0f °C", current);
}

wxString duration_text(int seconds)
{
    const int h = seconds / 3600, m = (seconds % 3600) / 60;
    return h > 0 ? wxString::Format(_L("%dh %dm"), h, m) : wxString::Format(_L("%dm"), m);
}

// Built-in status and print controls, used when ElegooSlicer's page is not available.
class ElegooCloudStatusView : public wxPanel
{
public:
    ElegooCloudStatusView(wxWindow* parent, const std::string& serial)
        : wxPanel(parent, wxID_ANY)
        , m_serial(serial)
    {
        auto* top  = new wxBoxSizer(wxVERTICAL);
        auto  pad  = FromDIP(14);
        auto  row  = [&](wxFlexGridSizer* grid, const wxString& label) {
            grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
            auto* value = new wxStaticText(this, wxID_ANY, "-");
            grid->Add(value, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
            return value;
        };

        m_connection = new wxStaticText(this, wxID_ANY, _L("Connecting through the Elegoo cloud…"));
        top->Add(m_connection, 0, wxLEFT | wxRIGHT | wxTOP, pad);

        m_state = new wxStaticText(this, wxID_ANY, "-");
        wxFont big = m_state->GetFont();
        big.SetPointSize(big.GetPointSize() + 4);
        big.SetWeight(wxFONTWEIGHT_BOLD);
        m_state->SetFont(big);
        top->Add(m_state, 0, wxLEFT | wxRIGHT | wxTOP, pad);

        m_file = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_MIDDLE);
        top->Add(m_file, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

        m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, FromDIP(10)));
        top->Add(m_gauge, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

        auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(18));
        grid->AddGrowableCol(1, 1);
        m_progress  = row(grid, _L("Progress"));
        m_layers    = row(grid, _L("Layer"));
        m_remaining = row(grid, _L("Time left"));
        m_nozzle    = row(grid, _L("Nozzle"));
        m_bed       = row(grid, _L("Bed"));
        m_chamber   = row(grid, _L("Chamber"));
        m_fan       = row(grid, _L("Part fan"));
        m_canvas    = row(grid, _L("Canvas"));
        top->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_pause  = new wxButton(this, wxID_ANY, _L("Pause"));
        m_resume = new wxButton(this, wxID_ANY, _L("Resume"));
        m_stop   = new wxButton(this, wxID_ANY, _L("Stop"));
        buttons->Add(m_pause, 0, wxRIGHT, FromDIP(8));
        buttons->Add(m_resume, 0, wxRIGHT, FromDIP(8));
        buttons->Add(m_stop, 0);
        top->Add(buttons, 0, wxEXPAND | wxALL, pad);

        m_error = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_error->SetForegroundColour(wxColour("#D9534F"));
        top->Add(m_error, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

        auto* outer = new wxBoxSizer(wxVERTICAL);
        outer->Add(top, 0, wxALL, FromDIP(8));
        SetSizer(outer);
        enable_controls(nullptr);

        m_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(ElegooCommand::Pause); });
        m_resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(ElegooCommand::Resume); });
        m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (wxMessageBox(_L("Stop the current print? It cannot be resumed."), "Elegoo", wxYES_NO | wxICON_WARNING, this) == wxYES)
                send(ElegooCommand::Stop);
        });
        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); });
        m_timer.Start(POLL_MS);
        poll();
    }

    ~ElegooCloudStatusView() override
    {
        *m_alive = false;
        m_timer.Stop();
    }

private:
    void poll()
    {
        if (!IsShownOnScreen() && m_polls > 0)
            return; // another Device tab page is in front
        if (m_busy.exchange(true))
            return; // the last request is still out
        const bool        with_canvas = m_polls++ % CANVAS_EVERY == 0;
        auto              alive       = m_alive;
        const std::string serial      = m_serial;
        std::thread([this, alive, serial, with_canvas]() {
            ElegooPrinterStatus status;
            ElegooCanvasInfo    canvas;
            std::string         error;
            bool                have_canvas = false;
            try {
                status = ElegooCloud::instance().live_status(serial);
                if (with_canvas) {
                    try {
                        canvas      = ElegooCloud::instance().canvas_info(serial);
                        have_canvas = true;
                    } catch (const std::exception& e) {
                        BOOST_LOG_TRIVIAL(info) << "Elegoo monitor: Canvas not read: " << e.what();
                    }
                }
            } catch (const std::exception& e) {
                error = e.what();
            }
            wxGetApp().CallAfter([this, alive, status, canvas, have_canvas, error]() {
                if (!*alive)
                    return;
                m_busy = false;
                if (!error.empty()) {
                    m_connection->SetLabel(_L("Could not reach the printer through the Elegoo cloud."));
                    m_error->SetLabel(wxString::FromUTF8(error));
                    enable_controls(nullptr);
                    Layout();
                    return;
                }
                m_error->SetLabel(wxEmptyString);
                m_connection->SetLabel(_L("Connected through the Elegoo cloud"));
                show(status);
                if (have_canvas)
                    show(canvas);
                Layout();
            });
        }).detach();
    }

    void show(const ElegooPrinterStatus& s)
    {
        m_status = s;
        m_has_status = true;
        const bool job = s.state == "RUNNING" || s.state == "PAUSE" || s.state == "PREPARE" || s.state == "FINISH";
        m_state->SetLabel(state_text(s.state));
        m_file->SetLabel(job ? wxString::FromUTF8(s.filename) : wxString());
        m_gauge->SetValue(job ? std::max(0, std::min(100, s.progress)) : 0);
        m_progress->SetLabel(job ? wxString::Format("%d%%", s.progress) : wxString("-"));
        m_layers->SetLabel(job && s.total_layers > 0 ? wxString::Format("%d / %d", s.layer, s.total_layers) : wxString("-"));
        m_remaining->SetLabel(job && s.state != "FINISH" ? duration_text(s.remaining_s) : wxString("-"));
        m_nozzle->SetLabel(temp_text(s.nozzle, s.nozzle_target));
        m_bed->SetLabel(temp_text(s.bed, s.bed_target));
        m_chamber->SetLabel(s.chamber >= 0 ? wxString::Format("%.0f °C", s.chamber) : wxString("-"));
        m_fan->SetLabel(s.fan_percent >= 0 ? wxString::Format("%d%%", s.fan_percent) : wxString("-"));
        enable_controls(&s);
    }

    void show(const ElegooCanvasInfo& c)
    {
        if (!c.connected || c.trays.empty()) {
            m_canvas->SetLabel(_L("Not connected"));
            return;
        }
        wxString text;
        for (const ElegooCanvasTray& t : c.trays) {
            if (!text.empty())
                text += "   ";
            text += wxString::Format("%d%c ", t.canvas_id + 1, char('A' + t.tray_id));
            text += t.loaded() ? wxString::FromUTF8(t.filament_type.empty() ? "?" : t.filament_type) : _L("empty");
        }
        m_canvas->SetLabel(text);
    }

    void enable_controls(const ElegooPrinterStatus* s)
    {
        const bool printing = s != nullptr && (s->state == "RUNNING" || s->state == "PREPARE");
        const bool paused   = s != nullptr && s->state == "PAUSE";
        m_pause->Enable(printing && !m_sending);
        m_resume->Enable(paused && !m_sending);
        m_stop->Enable((printing || paused) && !m_sending);
    }

    void send(ElegooCommand cmd)
    {
        m_sending = true;
        enable_controls(m_has_status ? &m_status : nullptr);
        auto              alive  = m_alive;
        const std::string serial = m_serial;
        std::thread([this, alive, serial, cmd]() {
            std::string error;
            try {
                ElegooCloud::instance().command(serial, cmd);
            } catch (const std::exception& e) {
                error = e.what();
            }
            wxGetApp().CallAfter([this, alive, error]() {
                if (!*alive)
                    return;
                m_sending = false;
                m_error->SetLabel(error.empty() ? wxString() : wxString::FromUTF8(error));
                enable_controls(m_has_status ? &m_status : nullptr);
                Layout();
                poll();
            });
        }).detach();
    }

    std::string            m_serial;
    std::shared_ptr<bool>  m_alive{std::make_shared<bool>(true)};
    std::atomic<bool>      m_busy{false};
    bool                   m_sending{false};
    bool                   m_has_status{false};
    unsigned               m_polls{0};
    ElegooPrinterStatus    m_status;
    wxTimer                m_timer;

    wxStaticText* m_connection{nullptr};
    wxStaticText* m_state{nullptr};
    wxStaticText* m_file{nullptr};
    wxGauge*      m_gauge{nullptr};
    wxStaticText* m_progress{nullptr};
    wxStaticText* m_layers{nullptr};
    wxStaticText* m_remaining{nullptr};
    wxStaticText* m_nozzle{nullptr};
    wxStaticText* m_bed{nullptr};
    wxStaticText* m_chamber{nullptr};
    wxStaticText* m_fan{nullptr};
    wxStaticText* m_canvas{nullptr};
    wxStaticText* m_error{nullptr};
    wxButton*     m_pause{nullptr};
    wxButton*     m_resume{nullptr};
    wxButton*     m_stop{nullptr};
};

// ElegooSlicer's own printer page for cloud printers, from its installation (it is not ours to ship).
// Empty when ElegooSlicer is not installed.
wxString elegooslicer_cloud_page()
{
#ifdef _WIN32
    const fs::path rel = fs::path("ElegooSlicer") / "resources" / "plugins" / "elegoolink" / "web" / "cloud_service_web" / "index.html";
    std::vector<fs::path> roots;
    for (const char* var : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"})
        if (const char* v = std::getenv(var))
            roots.emplace_back(v);
    if (const char* v = std::getenv("LOCALAPPDATA"))
        roots.push_back(fs::path(v) / "Programs");
    for (const fs::path& root : roots) {
        boost::system::error_code ec;
        const fs::path            page = root / rel;
        if (fs::exists(page, ec))
            { std::string s = page.generic_string(); boost::replace_all(s, " ", "%20"); return wxString::FromUTF8(s); }
    }
#endif
    return {};
}

std::string str_of(const json& v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<int64_t>());
    if (v.is_number())
        return std::to_string(v.get<double>());
    return {};
}

json parse_object(const json& v)
{
    if (v.is_object())
        return v;
    if (v.is_string()) {
        json j = json::parse(v.get<std::string>(), nullptr, false);
        if (j.is_object() || j.is_array())
            return j;
    }
    return json::object();
}

// The printer id ElegooSlicer gives an Elegoo cloud printer (elegoo-link's "cloud_" prefix). The
// page keys its requests and the events it accepts by it.
std::string cloud_printer_id(const std::string& serial) { return "cloud_" + serial; }

// The page whose listener receives the printer's messages (there is one listener slot).
const void* s_listener_owner = nullptr;

// Hosts ElegooSlicer's cloud printer page and answers it the way ElegooSlicer's printer view does
// (ElegooPrinterWebView + PrinterManagerView): the same address parameters, the same IPC replies
// and events, each event carrying the printer id. The page talks to the printer with its own
// messages, which the app relays over the account's Elegoo cloud messaging session, and asks the
// app for lists the cloud keeps (files, print history, errors).
class ElegooCloudPageView : public wxPanel
{
public:
    ElegooCloudPageView(wxWindow* parent, const std::string& serial, const wxString& page)
        : wxPanel(parent, wxID_ANY), m_serial(serial), m_printer_id(cloud_printer_id(serial))
    {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        m_browser   = WebView::CreateWebView(this, wxEmptyString);
        if (m_browser != nullptr) {
            Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ElegooCloudPageView::on_script_message, this, m_browser->GetId());
            Bind(wxEVT_WEBVIEW_NEWWINDOW, [](wxWebViewEvent& e) { wxLaunchDefaultBrowser(e.GetURL()); }, m_browser->GetId());
            Bind(wxEVT_WEBVIEW_LOADED, &ElegooCloudPageView::on_loaded, this, m_browser->GetId());
            m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
            // ElegooSlicer's user script: links open in the system browser, and a reconnect button.
            const std::string inject = read_text(fs::path(resources_dir()) / "web" / "elegoo" / "inject.js");
            if (!inject.empty())
                m_browser->AddUserScript(wxString::FromUTF8(inject));
            sizer->Add(m_browser, 1, wxEXPAND);
        }
        SetSizer(sizer);

        auto alive = m_alive;
        s_listener_owner = m_alive.get();
        ElegooCloud::instance().set_raw_listener([this, alive](const std::string& serial, const std::string& message) {
            wxGetApp().CallAfter([this, alive, serial, message]() {
                if (*alive && serial == m_serial)
                    on_printer_message(message);
            });
        });

        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); });
        m_timer.Start(STATUS_MS);

        // The address ElegooSlicer builds for a Centauri Carbon 2 (PrinterManagerView::openPrinterTab).
        auto enc = [](const std::string& v) { return wxString::FromUTF8(Http::url_encode(v)); };
        wxString url = wxString("file:///") + page + "?id=" + enc(m_printer_id) + "&ip=&sn=" + enc(serial) + "&access_code=";
        std::string region = wxGetApp().app_config->get("region");
        if (ElegooCloud::instance().region() == "CN")
            region = "China";
        if (!region.empty())
            url += "&region=" + enc(region);
        const wxString lang = wxGetApp().current_language_code_safe();
        if (!lang.empty())
            url += "&lang=" + lang;
        if (wxGetApp().app_config->get_bool("developer_mode"))
            url += "&dev=true";
        m_url = url;
        // Like ElegooSlicer, straight to the browser: WebView::LoadUrl would escape the address again.
        if (m_browser != nullptr)
            m_browser->LoadURL(m_url);
        poll();
    }

    ~ElegooCloudPageView() override
    {
        *m_alive = false;
        m_timer.Stop();
        // A window closes later than it is replaced: leave a newer page's listener alone.
        if (s_listener_owner == m_alive.get()) {
            ElegooCloud::instance().set_raw_listener(nullptr);
            s_listener_owner = nullptr;
        }
    }

private:
    static constexpr int STATUS_MS = 3000;

    static std::string read_text(const fs::path& path)
    {
        boost::nowide::ifstream in(path.string(), std::ios::binary);
        return in ? std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()) : std::string();
    }

    void send_to_page(const json& msg)
    {
        // Messages sent before the page has loaded would be lost: on_loaded() sends the state again.
        if (m_browser != nullptr && m_loaded)
            WebView::RunScript(m_browser, wxString::FromUTF8("window.HandleStudio && window.HandleStudio(" + msg.dump(-1, ' ', true) + ")"));
    }
    void event(const std::string& method, json data)
    {
        data["printerId"] = m_printer_id;
        send_to_page({{"id", "evt-" + std::to_string(++m_event_id)}, {"method", method}, {"type", "event"}, {"data", data}});
    }
    void respond(const json& id, const std::string& method, int code, const std::string& message, const json& data)
    {
        send_to_page({{"id", id}, {"method", method}, {"type", "response"}, {"code", code}, {"message", message}, {"data", data}});
    }

    void on_loaded(wxWebViewEvent& evt)
    {
        evt.Skip();
        if (!evt.GetURL().StartsWith("file:"))
            return;
        m_loaded = true;
        if (m_connect_status >= 0)
            event("onConnectionStatus", json{{"status", m_connect_status}});
        send_status();
    }

    void set_connected(int status)
    {
        if (status == m_connect_status)
            return;
        m_connect_status = status;
        for (const json& id : m_pending_connect)
            respond(id, "getConnectStatus", 0, "success", {{"printerId", m_printer_id}, {"connectStatus", status}});
        m_pending_connect.clear();
        event("onConnectionStatus", json{{"status", status}});
    }

    // The printer's full status, in the form elegoo-link hands ElegooSlicer's page
    // (ElegooFdmCC2MessageAdapter::wrapStatusData): a 6000 message whose result is the whole status.
    void send_status()
    {
        if (m_last_status.is_object() && !m_last_status.empty())
            event("onPrinterEventRaw", json{{"event", json{{"id", 0}, {"method", 6000}, {"result", m_last_status}}.dump()}});
    }

    // Live status: ElegooSlicer gets it pushed by the cloud; here the printer is also asked every
    // few seconds, while the page is in front.
    void poll()
    {
        if (m_last_status.is_object() && !m_last_status.empty() && !IsShownOnScreen())
            return;
        if (m_polling.exchange(true))
            return;
        auto              alive  = m_alive;
        const std::string serial = m_serial;
        std::thread([this, alive, serial]() {
            json        status;
            std::string error;
            try {
                status = ElegooCloud::instance().live_status_raw(serial);
            } catch (const std::exception& e) {
                error = e.what();
            }
            wxGetApp().CallAfter([this, alive, status, error]() {
                if (!*alive)
                    return;
                m_polling = false;
                if (!error.empty() || !status.is_object()) {
                    BOOST_LOG_TRIVIAL(info) << "Elegoo cloud page: status not read: " << error;
                    set_connected(0);
                    return;
                }
                set_connected(1);
                m_last_status = status;
                send_status();
            });
        }).detach();
    }

    void on_printer_message(const std::string& message)
    {
        event("onRtmMessage", json{{"message", message}});
        // A status push carries only what changed: merge it into the full status, as elegoo-link does.
        const json j = json::parse(message, nullptr, false);
        if (j.is_object() && j.value("method", 0) == 6000 && j.contains("result") && j["result"].is_object()) {
            if (!m_last_status.is_object())
                m_last_status = json::object();
            m_last_status.merge_patch(j["result"]);
            set_connected(1);
            send_status();
        }
    }

    // Runs work off the GUI thread and answers the page with its result.
    void answer_async(const json& id, const std::string& method, std::function<json()> work)
    {
        auto alive = m_alive;
        std::thread([this, alive, id, method, work]() {
            json        data;
            std::string error;
            try {
                data = work();
            } catch (const std::exception& e) {
                error = e.what();
            }
            wxGetApp().CallAfter([this, alive, id, method, data, error]() {
                if (!*alive)
                    return;
                if (!error.empty()) {
                    BOOST_LOG_TRIVIAL(warning) << "Elegoo cloud page: " << method << " failed: " << error;
                    respond(id, method, -1, error, json::object());
                } else
                    respond(id, method, 0, "success", data.is_null() ? json::object() : data);
            });
        }).detach();
    }

    void on_script_message(wxWebViewEvent& evt)
    {
        const json msg = json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
        if (!msg.is_object() || msg.value("type", "request") != "request")
            return;
        const std::string method = msg.value("method", "");
        const json        id     = msg.contains("id") ? msg["id"] : json("");
        const json        params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();
        const std::string serial = m_serial;
        const std::string page_q = "&pageNo=" + std::to_string(params.value("pageNumber", 1)) +
                                   "&pageSize=" + std::to_string(params.value("pageSize", 10));

        if (method == "getConnectStatus") {
            if (m_connect_status < 0)
                m_pending_connect.push_back(id); // answered by the first status read
            else
                respond(id, method, 0, "success", {{"printerId", m_printer_id}, {"connectStatus", m_connect_status}});
        } else if (method == "sendRtmMessage") {
            const std::string message = params.value("message", "");
            answer_async(id, method, [serial, message]() {
                ElegooCloud::instance().send_raw(serial, message);
                return json::object();
            });
        } else if (method == "getRtcToken") {
            answer_async(id, method, []() { return ElegooCloud::instance().rtc_token(); });
        } else if (method == "getPrinterStatusRaw") {
            if (m_last_status.is_object() && !m_last_status.empty())
                respond(id, method, 0, "success", m_last_status);
            else
                answer_async(id, method, [serial]() { return ElegooCloud::instance().live_status_raw(serial); });
        } else if (method == "getFileList") {
            answer_async(id, method, [serial, page_q]() {
                const json d   = ElegooCloud::instance().api_get("local-file/page?serialNo=" + Http::url_encode(serial) + page_q);
                json       out = {{"totalFiles", d.is_object() ? d.value("total", 0) : 0}, {"fileList", json::array()}};
                if (d.is_object() && d.contains("list") && d["list"].is_array())
                    for (const json& f : d["list"])
                        out["fileList"].push_back({{"fileId", f.value("filename", "")},
                                                   {"fileName", f.value("filename", "")},
                                                   {"printTime", f.value("printTime", json(0))},
                                                   {"layer", f.value("layer", json(0))},
                                                   {"layerHeight", f.value("layerHeight", json(0))},
                                                   {"thumbnail", f.value("thumbnail", "")},
                                                   {"size", f.value("size", json(0))},
                                                   {"createTime", f.value("createTime", json(0))},
                                                   {"totalFilamentUsed", f.value("totalFilamentUsed", json(0))},
                                                   {"totalFilamentUsedLength", f.value("totalFilamentUsedLength", json(0))},
                                                   {"totalPrintTimes", f.value("totalPrintTimes", json(0))},
                                                   {"lastPrintTime", f.value("lastPrintTime", json(0))}});
                return out;
            });
        } else if (method == "getFileDetail") {
            const std::string file = params.value("fileName", "");
            answer_async(id, method, [serial, file]() {
                const json d = ElegooCloud::instance().api_get("local-file/filename?serialNo=" + Http::url_encode(serial) + "&filename=" + Http::url_encode(file));
                json       out = json::object();
                if (!d.is_object())
                    return out;
                out = {{"fileName", d.value("filename", "")},
                       {"printTime", d.value("printTime", json(0))},
                       {"layer", d.value("layer", json(0))},
                       {"layerHeight", d.value("layerHeight", json(0))},
                       {"thumbnail", ElegooCloud::instance().object_url(d.value("thumbnail", ""))},
                       {"size", d.value("size", json(0))},
                       {"createTime", d.value("createTime", json(0))},
                       {"totalFilamentUsed", d.value("totalFilamentUsed", json(0))},
                       {"totalFilamentUsedLength", d.value("totalFilamentUsedLength", json(0))},
                       {"totalPrintTimes", d.value("totalPrintTimes", json(0))},
                       {"lastPrintTime", d.value("lastPrintTime", json(0))},
                       {"colorMapping", json::array()}};
                const json colors = parse_object(d.contains("colorMap") ? d["colorMap"] : json());
                if (colors.is_array())
                    for (const json& c : colors)
                        out["colorMapping"].push_back(
                            {{"color", c.value("color", "")}, {"type", c.value("type", "")}, {"t", c.value("t", -1)}});
                return out;
            });
        } else if (method == "getPrintTaskList") {
            answer_async(id, method, [serial, page_q]() {
                const json d = ElegooCloud::instance().api_get("device/event-data/page?deviceCode=" + Http::url_encode(serial) + page_q +
                                             "&eventKey=history_task&sort=create_time,desc");
                json out = {{"totalTasks", d.is_object() ? d.value("total", 0) : 0}, {"taskList", json::array()}};
                if (d.is_object() && d.contains("list") && d["list"].is_array())
                    for (const json& item : d["list"]) {
                        const json v = parse_object(item.contains("eventValue") ? item["eventValue"] : json());
                        out["taskList"].push_back({{"taskId", str_of(item.value("id", json()))},
                                                   {"fileName", ""},
                                                   {"thumbnail", v.value("thumbnail", "")},
                                                   {"taskName", v.value("task_name", "")},
                                                   {"totalTime", 0},
                                                   {"currentTime", 0},
                                                   {"estimatedTime", 0},
                                                   {"beginTime", v.value("begin_time", json(0))},
                                                   {"endTime", v.value("end_time", json(0))},
                                                   {"progress", 0},
                                                   {"taskStatus", v.value("task_status", json(0))}});
                    }
                return out;
            });
        } else if (method == "getExceptionList") {
            answer_async(id, method, [serial, page_q]() {
                const json d = ElegooCloud::instance().api_get("device/event-data/page?deviceCode=" + Http::url_encode(serial) + page_q +
                                             "&eventKey=exception&sort=create_time,desc");
                json out = {{"total", d.is_object() ? d.value("total", 0) : 0}, {"exceptionList", json::array()}};
                if (d.is_object() && d.contains("list") && d["list"].is_array())
                    for (const json& item : d["list"]) {
                        const json v = parse_object(item.contains("eventValue") ? item["eventValue"] : json());
                        out["exceptionList"].push_back({{"id", str_of(item.value("id", json()))},
                                                        {"refId", str_of(item.value("eventDataRefId", json()))},
                                                        {"code", str_of(v.value("code", json("0")))},
                                                        {"level", v.value("level", json(0))},
                                                        {"time", v.value("time", json(0))}});
                    }
                return out;
            });
        } else if (method == "deletePrintTasks") {
            std::string ids;
            for (const json& t : params.value("taskIds", json::array()))
                ids += (ids.empty() ? "" : ",") + Http::url_encode(str_of(t));
            answer_async(id, method, [serial, ids]() {
                ElegooCloud::instance().api_delete("device-data/event-data?ids=" + ids + "&deviceCode=" + Http::url_encode(serial));
                return json{{"error_code", 0}, {"error_message", "success"}};
            });
        } else if (method == "get_sn") {
            respond(id, method, 0, "success", serial);
        } else if (method == "open") {
            respond(id, method, 0, "success", json::object());
            const std::string url = params.value("url", "");
            if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
                wxLaunchDefaultBrowser(wxString::FromUTF8(url));
        } else if (method == "reload") {
            respond(id, method, 0, "success", json::object());
            CallAfter([this]() {
                if (m_browser != nullptr) {
                    m_loaded = false;
                    m_browser->LoadURL(m_url);
                }
            });
        } else if (method == "printer_command_delivery" || method == "print_job_start") {
            respond(id, method, 0, "success", json::object()); // ElegooSlicer's usage reports; nothing to do here
        } else {
            BOOST_LOG_TRIVIAL(info) << "Elegoo cloud page: unsupported request " << method;
            respond(id, method, -1, "Not supported in this slicer. Send prints with the Print button.", json::object());
        }
    }

    std::string           m_serial;
    std::string           m_printer_id;
    wxString              m_url;
    wxWebView*            m_browser{nullptr};
    bool                  m_loaded{false};
    std::shared_ptr<bool> m_alive{std::make_shared<bool>(true)};
    std::atomic<bool>     m_polling{false};
    int                   m_connect_status{-1}; // -1 until the first status read
    std::vector<json>     m_pending_connect;    // getConnectStatus requests waiting for it
    unsigned              m_event_id{0};
    json                  m_last_status;
    wxTimer               m_timer;
};

wxWindow* create_printer_view(wxWindow* parent, const std::string& serial)
{
    const wxString page = elegooslicer_cloud_page();
#if wxUSE_WEBVIEW_EDGE
    const bool webview_ok = !page.empty() && WebView::CheckWebViewRuntime();
#else
    const bool webview_ok = !page.empty();
#endif
    BOOST_LOG_TRIVIAL(info) << "Elegoo cloud printer page: "
                            << (webview_ok ? "ElegooSlicer page" : page.empty() ? "built-in view (ElegooSlicer page not found)" : "built-in view (no WebView runtime)");
    if (webview_ok)
        return new ElegooCloudPageView(parent, serial, page);
    return new ElegooCloudStatusView(parent, serial);
}

// Ends the account's messaging session once no printer page uses it. Logging out blocks briefly,
// so never on the GUI thread.
void close_session_later()
{
    std::thread([]() { ElegooCloud::instance().close_live_session(); }).detach();
}

// Used without a Device tab: one window at a time (the cloud allows one messaging session).
wxWeakRef<wxDialog> s_window;
std::string         s_window_serial;

} // namespace

ElegooDevicePage::ElegooDevicePage(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_book      = new wxSimplebook(this, wxID_ANY);
    sizer->Add(m_book, 1, wxEXPAND);
    SetSizer(sizer);

    m_printer_page = new wxPanel(m_book, wxID_ANY);
    auto* page     = new wxBoxSizer(wxVERTICAL);
    auto* header   = new wxBoxSizer(wxHORIZONTAL);
    m_back         = new Button(m_printer_page, _L("Printers"));
    m_back->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_back->SetToolTip(_L("Back to the list of printers"));
    m_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_list(); });
    header->Add(m_back, 0, wxALIGN_CENTER_VERTICAL);
    m_title = new wxStaticText(m_printer_page, wxID_ANY, wxEmptyString);
    m_title->SetFont(Label::Head_16);
    header->Add(m_title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    page->Add(header, 0, wxEXPAND | wxALL, FromDIP(8));
    m_printer_page->SetSizer(page);
}

wxWindow* ElegooDevicePage::book() const { return m_book; }

void ElegooDevicePage::set_list_page(wxWindow* list_page)
{
    m_list = list_page;
    m_book->InsertPage(0, list_page, wxEmptyString, true);
    m_book->AddPage(m_printer_page, wxEmptyString, false);
}

void ElegooDevicePage::show_printer(const std::string& serial, const wxString& name)
{
    if (m_view == nullptr || serial != m_serial) {
        if (m_view != nullptr) {
            m_view->Destroy();
            m_view = nullptr;
        }
        // The page outside the Device tab, if open, holds the messaging session.
        if (s_window)
            s_window->Destroy();
        m_serial = serial;
        m_view   = create_printer_view(m_printer_page, serial);
        m_printer_page->GetSizer()->Add(m_view, 1, wxEXPAND);
    }
    m_title->SetLabel(name.empty() ? wxString::FromUTF8(serial) : name);
    m_book->SetSelection(m_book->FindPage(m_printer_page));
    m_printer_page->Layout();
}

void ElegooDevicePage::show_list()
{
    if (m_list != nullptr)
        m_book->SetSelection(m_book->FindPage(m_list));
    // Leaving the printer frees the account's messaging session for the send dialog and others.
    if (m_view != nullptr) {
        m_view->Destroy();
        m_view = nullptr;
        m_serial.clear();
        close_session_later();
    }
}

void show_elegoo_cloud_monitor(wxWindow* from, const std::string& serial, const wxString& name)
{
    // On the Device tab: show the printer on the Elegoo page, in place of the printer list.
    for (wxWindow* w = from; w != nullptr && !w->IsTopLevel(); w = w->GetParent()) {
        if (auto* hub = dynamic_cast<DeviceHubPanel*>(w); hub != nullptr && hub->select(ELEGOO_CLOUD_PROVIDER)) {
            if (auto* page = dynamic_cast<ElegooDevicePage*>(hub->selected_page()); page != nullptr) {
                page->show_printer(serial, name);
                return;
            }
        }
    }

    if (s_window) {
        if (s_window_serial == serial) {
            s_window->Raise();
            return;
        }
        s_window->Destroy();
    }
    auto* dlg = new wxDialog(from, wxID_ANY, name.empty() ? wxString("Elegoo") : name, wxDefaultPosition, wxDefaultSize,
                             wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxMAXIMIZE_BOX | wxMINIMIZE_BOX);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(create_printer_view(dlg, serial), 1, wxEXPAND);
    dlg->SetSizer(sizer);
    dlg->SetSize(dlg->FromDIP(wxSize(1180, 800)));
    dlg->CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(dlg);
    dlg->Bind(wxEVT_CLOSE_WINDOW, [dlg](wxCloseEvent&) {
        dlg->Destroy();
        close_session_later();
    });
    s_window        = dlg;
    s_window_serial = serial;
    dlg->Show();
}

}} // namespace Slic3r::GUI
