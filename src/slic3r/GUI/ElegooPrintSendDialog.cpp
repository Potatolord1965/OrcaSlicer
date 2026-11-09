#include "ElegooPrintSendDialog.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/base64.h>
#include <wx/colour.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/webview.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "VendorSendDialog.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

// ElegooSlicer's window sizes (PrintSendDialogEx): taller while the Canvas mapping is shown.
constexpr int WIDTH      = 860;
constexpr int HAS_MMS_H  = 800;
constexpr int NO_MMS_H   = 650;

fs::path page_path() { return fs::path(resources_dir()) / "web" / "elegoo" / "printer" / "print_send" / "index.html"; }

std::string recall(const std::string& key, const std::string& def)
{
    const std::string v = wxGetApp().app_config->get("vendor_send", "elegoo_" + key);
    return v.empty() ? def : v;
}
void remember(const std::string& key, const std::string& value) { wxGetApp().app_config->set("vendor_send", "elegoo_" + key, value); }

std::string upper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

// Elegoo product code ("CC2") to the printer model name of the Elegoo profiles.
std::string model_name(const std::string& code)
{
    const std::string u = upper(code);
    if (u == "CC2" || u.find("CENTAURI CARBON 2") != std::string::npos)
        return "Elegoo Centauri Carbon 2";
    if (u.find("CENTAURI 2") != std::string::npos)
        return "Elegoo Centauri 2";
    if (u == "CC" || u.find("CENTAURI CARBON") != std::string::npos)
        return "Elegoo Centauri Carbon";
    return code;
}

std::string png_base64(const wxImage& image)
{
    if (!image.IsOk())
        return {};
    wxMemoryOutputStream mem;
    image.SaveFile(mem, wxBITMAP_TYPE_PNG);
    const size_t   len = mem.GetSize();
    wxMemoryBuffer buffer(len);
    mem.CopyTo(buffer.GetData(), len);
    return wxBase64Encode(buffer.GetData(), len).ToStdString();
}

std::string file_data_uri(const fs::path& path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    if (!in)
        return {};
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return "data:image/png;base64," + wxBase64Encode(data.data(), data.size()).ToStdString();
}

double colour_distance(const std::string& a, const std::string& b)
{
    const wxColour ca(wxString::FromUTF8(a)), cb(wxString::FromUTF8(b));
    if (!ca.IsOk() || !cb.IsOk())
        return 1e9;
    const double dr = ca.Red() - cb.Red(), dg = ca.Green() - cb.Green(), db = ca.Blue() - cb.Blue();
    return std::sqrt(dr * dr + dg * dg + db * db);
}

json empty_tray()
{
    return {{"trayId", ""},           {"mmsId", ""},         {"trayName", ""},           {"settingId", ""},
            {"filamentId", ""},       {"from", ""},          {"vendor", ""},             {"serialNumber", 0},
            {"filamentType", ""},     {"filamentName", ""},  {"filamentColor", ""},      {"filamentDiameter", ""},
            {"filamentPresetName", ""}, {"filamentPresetAlias", ""}, {"minNozzleTemp", 0}, {"maxNozzleTemp", 0},
            {"minBedTemp", 0},        {"maxBedTemp", 0},     {"status", -1}};
}

class ElegooPrintSendDialog : public DPIDialog
{
public:
    ElegooPrintSendDialog(wxWindow* parent, std::shared_ptr<VendorSendBackend> backend, const SendJobSummary& summary,
                          const fs::path& default_output)
        : DPIDialog(parent, wxID_ANY, _L("Send G-code to printer host"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
        , m_backend(std::move(backend))
        , m_summary(summary)
    {
        m_extension = default_output.extension().string();
        const std::string file = default_output.filename().string();
        if (file.size() > 10 && file.compare(file.size() - 10, 10, ".gcode.3mf") == 0)
            m_extension = ".gcode.3mf";

        m_browser = WebView::CreateWebView(this, wxEmptyString);
        if (m_browser == nullptr)
            return;
        m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
        Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ElegooPrintSendDialog::on_script_message, this, m_browser->GetId());
        // ElegooSlicer keeps the dialog open on Esc; Cancel is the page's own button.
        Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent& e) {
            if (e.GetKeyCode() != WXK_ESCAPE)
                e.Skip();
        });

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(m_browser, 1, wxEXPAND);
        SetSizer(sizer);
        SetSize(FromDIP(wxSize(WIDTH, HAS_MMS_H)));
        CenterOnParent();

        wxString url = "file:///" + wxString::FromUTF8(page_path().generic_string());
        url.Replace(" ", "%20");
        const wxString lang = wxGetApp().current_language_code_safe();
        url += "?lang=" + (lang.empty() ? wxString("en") : lang);
        if (wxGetApp().app_config->get_bool("developer_mode"))
            url += "&dev=true";
        m_browser->LoadURL(url);
    }

    ~ElegooPrintSendDialog() override { *m_alive = false; }

    bool                        ok() const { return m_browser != nullptr; }
    std::optional<PrintHostJob> take_job() { return std::move(m_job); }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        Layout();
        Refresh();
    }

private:
    // -- IPC, as ElegooSlicer's WebviewIPCManager ------------------------------------------------

    void send_to_page(const json& msg)
    {
        if (m_browser != nullptr)
            WebView::RunScript(m_browser, wxString::FromUTF8("window.HandleStudio && window.HandleStudio(" + msg.dump(-1, ' ', true) + ")"));
    }
    void respond(const json& id, const std::string& method, int code, const std::string& message, const json& data)
    {
        send_to_page({{"id", id}, {"method", method}, {"type", "response"}, {"code", code}, {"message", message}, {"data", data}});
    }
    void on_script_message(wxWebViewEvent& evt)
    {
        const json msg = json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
        if (!msg.is_object())
            return;
        const std::string type   = msg.value("type", "request");
        const std::string method = msg.value("method", "");
        const json        id     = msg.contains("id") ? msg["id"] : json("");
        if (type == "event") {
            const json data = msg.contains("data") && msg["data"].is_object() ? msg["data"] : json::object();
            if (method == "cancel_print")
                CallAfter([this]() { EndModal(wxID_CANCEL); });
            else if (method == "expand_window") {
                const bool expand = data.value("expand", false);
                CallAfter([this, expand]() { SetSize(FromDIP(wxSize(WIDTH, expand ? HAS_MMS_H : NO_MMS_H))); });
            }
            return;
        }
        if (type != "request")
            return;
        const json params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : json::object();

        if (method == "request_printer_list") {
            // Not from inside the WebView's message handler: signing in opens a window.
            CallAfter([this, id, method]() { list_printers(id, method); });
        } else if (method == "request_print_task") {
            respond(id, method, 0, "success", print_task());
        } else if (method == "request_mms_info") {
            const std::string printer_id = params.value("printerId", "");
            auto              it         = std::find_if(m_printers.begin(), m_printers.end(), [&](const SendPrinter& p) { return p.id == printer_id; });
            if (it == m_printers.end()) {
                respond(id, method, -1, "Printer not found", json::object());
                return;
            }
            auto              backend = m_backend;
            const SendPrinter printer = *it;
            auto              alive   = m_alive;
            std::thread([this, alive, backend, printer, id, method]() {
                SendSlots   slots;
                std::string error;
                try {
                    slots = backend->slots(printer);
                } catch (const std::exception& e) {
                    error = e.what();
                }
                wxGetApp().CallAfter([this, alive, slots, error, id, method]() {
                    if (!*alive)
                        return;
                    if (!error.empty())
                        respond(id, method, -1, error, json::object());
                    else
                        respond(id, method, 0, "success", mms_info(slots));
                });
            }).detach();
        } else if (method == "get_current_bed_type") {
            const int bed = std::atoi(wxGetApp().app_config->get("curr_bed_type").c_str());
            respond(id, method, 0, "success", {{"bedType", bed == btPC ? "btPC" : bed == btPTE ? "btPTE" : "unknown"}});
        } else if (method == "start_upload") {
            wxString error;
            if (start_upload(params, error))
                CallAfter([this]() { EndModal(wxID_OK); });
            else
                respond(id, method, -1, into_u8(error), json::object());
        } else {
            BOOST_LOG_TRIVIAL(info) << "Elegoo send dialog: unsupported request " << method;
            respond(id, method, 404, "Method not found", json::object());
        }
    }

    void list_printers(const json& id, const std::string& method)
    {
        if (m_backend->needs_sign_in() && !m_backend->signed_in() && !m_backend->has_local_printers())
            m_backend->sign_in(this);
        auto backend = m_backend;
        auto alive   = m_alive;
        std::thread([this, alive, backend, id, method]() {
            std::vector<SendPrinter> printers;
            std::string              error;
            try {
                printers = backend->printers();
            } catch (const std::exception& e) {
                error = e.what();
            }
            wxGetApp().CallAfter([this, alive, printers = std::move(printers), error, id, method]() mutable {
                if (!*alive)
                    return;
                if (!error.empty())
                    respond(id, method, -1, error, json::array());
                else
                    respond(id, method, 0, "success", printer_list(std::move(printers)));
            });
        }).detach();
    }

    // -- data, in the shapes of ElegooSlicer's PrinterNetworkInfo.cpp converters -----------------

    json printer_list(std::vector<SendPrinter> printers)
    {
        const std::string project_model = project_printer_model();
        const std::string last          = recall("printer", "");
        json              out           = json::array();
        bool              any_selected  = false;
        for (const SendPrinter& p : printers) {
            const bool        cloud = p.id.rfind("cloud:", 0) == 0;
            const std::string model = p.model.empty() ? project_model : model_name(p.model);
            json              j;
            j["printerId"]       = p.id;
            j["printerName"]     = into_u8(p.name);
            j["host"]            = cloud ? std::string() : into_u8(p.detail);
            j["serialNumber"]    = cloud ? p.id.substr(6) : std::string();
            j["vendor"]          = "Elegoo";
            j["printerModel"]    = model;
            j["hostType"]        = "ElegooLink";
            j["networkType"]     = cloud ? 1 : 0;
            j["connectStatus"]   = p.online ? 1 : 0;
            j["printerStatus"]   = !p.online ? -1 : p.busy ? 1 : 0;
            j["printTask"]       = {{"taskId", ""}, {"fileName", ""}, {"totalTime", 0}, {"currentTime", 0}, {"estimatedTime", 0}, {"progress", 0}};
            j["printCapabilities"] = {{"supportsAutoBedLeveling", true}, {"supportsTimeLapse", true}, {"supportsHeatedBedSwitching", true},
                                      {"supportsFilamentMapping", cloud}, {"supportsAutoRefill", cloud}};
            // Canvas mapping goes over the cloud; for LAN prints it is chosen on the printer.
            j["systemCapabilities"] = {{"supportsMultiFilament", cloud}, {"canGetDiskInfo", false}, {"canSetPrinterName", false}};
            j["exceptions"]         = json::array();
            j["extraInfo"]          = json::object();
            j["printerImg"]         = file_data_uri(fs::path(resources_dir()) / "profiles" / "Elegoo" / (model + "_cover.png"));
            j["selected"]           = p.id == last;
            any_selected            = any_selected || p.id == last;
            out.push_back(std::move(j));
        }
        if (!any_selected && !out.empty())
            out[0]["selected"] = true;
        m_printers = std::move(printers);
        return out;
    }

    std::string project_printer_model() const
    {
        const auto& cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        return cfg.has("printer_model") ? cfg.opt_string("printer_model") : std::string();
    }

    json filament_list()
    {
        const DynamicPrintConfig full    = wxGetApp().preset_bundle->full_config();
        const auto*              vendors = full.option<ConfigOptionStrings>("filament_vendor");
        const auto&              names   = wxGetApp().preset_bundle->filament_presets;
        json                     out     = json::array();
        for (const FilamentInfo& f : m_summary.filaments) {
            if (f.id < 0)
                continue;
            json j;
            j["index"]             = f.id;
            j["filamentId"]        = f.filament_id;
            j["vendor"]            = vendors != nullptr && size_t(f.id) < vendors->values.size() ? vendors->values[f.id] : std::string();
            j["filamentName"]      = size_t(f.id) < names.size() ? names[f.id] : f.type;
            j["filamentAlias"]     = "";
            j["filamentColor"]     = f.color;
            j["filamentType"]      = f.type;
            j["filamentWeight"]    = std::round(f.used_g * 100.) / 100.;
            j["filamentDensity"]   = 0;
            j["mappedMmsFilament"] = empty_tray();
            out.push_back(std::move(j));
        }
        return out;
    }

    json print_task()
    {
        m_has_mms = false; // a printer was picked: its Canvas is read next
        json info;
        info["modelName"]                  = into_u8(m_summary.name);
        info["printTime"]                  = get_time_dhms(m_summary.time_s);
        info["totalWeight"]                = m_summary.weight_g;
        info["layerCount"]                 = m_summary.layers;
        info["timeLapse"]                  = recall("timelapse", "0") == "1";
        info["heatedBedLeveling"]          = recall("bed_leveling", "1") == "1";
        info["uploadAndPrint"]             = recall("upload_and_print", "1") == "1";
        info["switchToDeviceTab"]          = recall("switch_tab", "0") == "1";
        info["autoRefill"]                 = recall("auto_refill", "0") == "1";
        info["bedType"]                    = recall("plate_side", "A") == "B" ? "btPC" : "btPTE";
        info["currentProjectPrinterModel"] = project_printer_model();
        if (m_summary.thumbnail.IsOk()) {
            const int size = FromDIP(256);
            info["thumbnail"] = png_base64(m_summary.thumbnail.Scale(size, size, wxIMAGE_QUALITY_HIGH));
        }
        info["filamentList"] = filament_list();
        return info;
    }

    // The Canvas trays, and each project filament mapped to the closest loaded tray of its type
    // (ElegooSlicer's PrinterMmsManager::getFilamentMmsMapping matches the same way).
    json mms_info(const SendSlots& slots)
    {
        m_has_mms = slots.available && !slots.slots.empty();
        json group = {{"connectNum", 0},     {"connected", m_has_mms},        {"activeMmsId", ""}, {"activeTrayId", ""},
                      {"autoRefill", slots.auto_refill}, {"mmsSystemName", "CANVAS"}, {"vtTray", empty_tray()}, {"mmsList", json::array()}};
        std::map<std::string, json> units;
        std::vector<json>           trays;
        for (const SendSlot& s : slots.slots) {
            const auto        colon = s.id.find(':');
            const std::string mms   = colon == std::string::npos ? std::string("0") : s.id.substr(0, colon);
            const std::string tray  = colon == std::string::npos ? s.id : s.id.substr(colon + 1);
            json t                  = empty_tray();
            t["trayId"]             = tray;
            t["mmsId"]              = mms;
            t["trayName"]           = into_u8(s.label);
            t["filamentType"]       = s.type;
            t["filamentName"]       = into_u8(s.name);
            t["filamentColor"]      = s.color.IsOk() ? into_u8(s.color.GetAsString(wxC2S_HTML_SYNTAX)) : std::string();
            t["status"]             = s.loaded ? 3 : 0;
            if (units.count(mms) == 0)
                units[mms] = {{"mmsId", mms}, {"temperature", 0}, {"humidity", 0}, {"connected", true}, {"trayList", json::array()}};
            units[mms]["trayList"].push_back(t);
            if (s.loaded)
                trays.push_back(t);
        }
        for (auto& [id, unit] : units)
            group["mmsList"].push_back(unit);
        group["connectNum"] = int(units.size());

        json mapped = filament_list();
        if (m_has_mms)
            for (json& f : mapped) {
                const std::string type = upper(f.value("filamentType", ""));
                const json*       best = nullptr;
                double            best_d = 1e18;
                for (const json& t : trays) {
                    if (upper(t.value("filamentType", "")) != type)
                        continue;
                    const double d = colour_distance(f.value("filamentColor", ""), t.value("filamentColor", ""));
                    if (d < best_d) {
                        best_d = d;
                        best   = &t;
                    }
                }
                if (best != nullptr)
                    f["mappedMmsFilament"] = *best;
            }
        return {{"mmsInfo", group}, {"mappedFilamentList", mapped}};
    }

    bool start_upload(const json& info, wxString& error)
    {
        const std::string printer_id = info.value("selectedPrinterId", "");
        if (printer_id.empty()) {
            error = _L("Choose a printer.");
            return false;
        }
        const bool start    = info.value("uploadAndPrint", false);
        const bool cloud    = printer_id.rfind("cloud:", 0) == 0;
        SendRequest req;
        req.printer_id  = printer_id;
        req.start_print = start;
        wxString name   = wxString::FromUTF8(info.value("modelName", ""));
        if (name.EndsWith(".gcode"))
            name = name.Left(name.size() - 6);
        if (name.empty())
            name = m_summary.name;
        req.file_name                 = name + wxString::FromUTF8(m_extension);
        req.options["plate_side"]     = info.value("bedType", "btPTE") == "btPC" ? "B" : "A";
        req.options["bed_leveling"]   = info.value("heatedBedLeveling", false) ? "1" : "0";
        req.options["timelapse"]      = info.value("timeLapse", false) ? "1" : "0";
        if (cloud && m_has_mms)
            req.options["auto_refill"] = info.value("autoRefill", false) ? "1" : "0";

        if (start && cloud && m_has_mms) {
            for (const json& f : info.value("filamentList", json::array())) {
                const json        m    = f.value("mappedMmsFilament", json::object());
                const std::string mms  = m.value("mmsId", "");
                const std::string tray = m.value("trayId", "");
                if (mms.empty() || tray.empty()) {
                    error = _L("Some filaments are not mapped to a Canvas slot.");
                    return false;
                }
                req.slot_map[f.value("index", 0)] = mms + ":" + tray;
            }
        }

        PrintHostJob job = m_backend->make_job(req, fs::path(), error);
        if (!error.empty())
            return false;

        remember("printer", printer_id);
        remember("plate_side", req.options["plate_side"]);
        remember("bed_leveling", req.options["bed_leveling"]);
        remember("timelapse", req.options["timelapse"]);
        remember("upload_and_print", start ? "1" : "0");
        remember("switch_tab", info.value("switchToDeviceTab", false) ? "1" : "0");
        if (req.options.count("auto_refill"))
            remember("auto_refill", req.options["auto_refill"]);

        if (!job.empty()) {
            job.switch_to_device_tab = info.value("switchToDeviceTab", false);
            m_job                    = std::move(job);
        }
        return true;
    }

    std::shared_ptr<VendorSendBackend> m_backend;
    SendJobSummary                     m_summary;
    std::string                        m_extension;
    wxWebView*                         m_browser{nullptr};
    std::shared_ptr<bool>              m_alive{std::make_shared<bool>(true)};
    std::vector<SendPrinter>           m_printers; // as last listed, by the page's printerId
    bool                               m_has_mms{false};
    std::optional<PrintHostJob>        m_job;
};

} // namespace

bool show_elegoo_print_send_dialog(wxWindow* parent, std::shared_ptr<VendorSendBackend> backend, const SendJobSummary& summary,
                                   const fs::path& default_output, std::optional<PrintHostJob>& job)
{
    boost::system::error_code ec;
    if (!fs::exists(page_path(), ec))
        return false;
#if wxUSE_WEBVIEW_EDGE
    if (!WebView::CheckWebViewRuntime())
        return false;
#endif
    ElegooPrintSendDialog dlg(parent, std::move(backend), summary, default_output);
    if (!dlg.ok())
        return false;
    job.reset();
    if (dlg.ShowModal() == wxID_OK)
        job = dlg.take_job();
    return true;
}

}} // namespace Slic3r::GUI
