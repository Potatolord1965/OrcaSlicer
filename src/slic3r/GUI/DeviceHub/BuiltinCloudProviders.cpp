#include "BuiltinCloudProviders.hpp"

#include <functional>
#include <utility>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/ElegooCloud.hpp"
#include "slic3r/Utils/MarkforgedEiger.hpp"
#include "slic3r/GUI/ElegooLoginDialog.hpp"
#include "slic3r/GUI/ElegooCloudMonitor.hpp"
#include <wx/utils.h>
#include <atomic>
#include <mutex>
#include <thread>
#include "slic3r/Utils/ICloudServiceAgent.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

namespace Slic3r { namespace GUI {

namespace {

// The printer agent a printer preset uses, with the vendor default filled in.
std::string effective_agent_id(const DynamicPrintConfig& printer_config)
{
    const auto* opt = printer_config.option<ConfigOptionString>("printer_agent");
    return wxGetApp().resolve_printer_agent_id(opt != nullptr ? opt->value : std::string());
}

wxString print_status_text(const std::string& status)
{
    if (status == "RUNNING" || status == "PREPARE" || status == "SLICING")
        return _L("Printing");
    if (status == "PAUSE")
        return _L("Paused");
    if (status == "FINISH")
        return _L("Finished");
    if (status == "FAILED")
        return _L("Failed");
    return _L("Idle");
}

} // namespace

// ---------------------------------------------------------------------------------------------
// BambuCloudProvider
// ---------------------------------------------------------------------------------------------

std::string BambuCloudProvider::id() const { return BBL_CLOUD_PROVIDER; }

wxString BambuCloudProvider::display_name() const { return "Bambu Lab"; }

bool BambuCloudProvider::is_available() const
{
    NetworkAgent* agent = wxGetApp().getAgent();
    return agent != nullptr && agent->get_cloud_agent(BBL_CLOUD_PROVIDER) != nullptr;
}

wxString BambuCloudProvider::unavailable_reason() const
{
    return _L("Bambu Lab printers need the Bambu network plug-in. It is downloaded from Bambu Lab when you install it.");
}

void BambuCloudProvider::install_or_enable(wxWindow* /*parent*/) { wxGetApp().ShowDownNetPluginDlg(); }

bool BambuCloudProvider::is_logged_in() const { return wxGetApp().is_user_login(BBL_CLOUD_PROVIDER); }

wxString BambuCloudProvider::account_name() const
{
    NetworkAgent* agent = wxGetApp().getAgent();
    if (agent == nullptr)
        return {};
    std::shared_ptr<ICloudServiceAgent> cloud = agent->get_cloud_agent(BBL_CLOUD_PROVIDER);
    if (!cloud || !cloud->is_user_login())
        return {};
    std::string name = cloud->get_user_nickname();
    if (name.empty())
        name = cloud->get_user_name();
    return from_u8(name);
}

void BambuCloudProvider::login(wxWindow* /*parent*/)
{
    wxGetApp().request_login(true, BBL_CLOUD_PROVIDER);
    CloudPrinterProviders::notify_changed(id());
}

void BambuCloudProvider::logout()
{
    wxGetApp().request_user_logout(BBL_CLOUD_PROVIDER, true);
    CloudPrinterProviders::notify_changed(id());
}

void BambuCloudProvider::refresh_printers(PrintersCallback done)
{
    std::vector<CloudPrinter> printers;
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr) {
        done(std::move(printers), {});
        return;
    }

    // Cloud machines first; a LAN machine also bound to the account is listed once.
    std::map<std::string, MachineObject*> machines = dev->get_local_machinelist();
    for (const auto& [dev_id, obj] : dev->get_user_machinelist())
        machines[dev_id] = obj;

    for (const auto& [dev_id, obj] : machines) {
        if (obj == nullptr)
            continue;
        // The discovery list also holds every other brand's machines: the CC2 LAN plugin's
        // ("cc2-<ip>") and the one a non-Bambu preset's host creates (keyed by that address, e.g.
        // "10.0.0.45"). Only Bambu printers belong here; their ids are serial numbers, never addresses.
        if (dev_id.rfind("cc2-", 0) == 0 || dev_id.find_first_of(".:") != std::string::npos)
            continue;
        if (!obj->printer_agent_id.empty() && obj->printer_agent_id != BBL_PRINTER_AGENT_ID)
            continue;
        CloudPrinter p;
        p.provider_id = id();
        p.id          = dev_id;
        p.name        = from_u8(obj->get_dev_name());
        p.model       = obj->get_printer_type_display_str();
        p.online      = obj->is_online();
        p.status      = p.online ? print_status_text(obj->print_status) : _L("Offline");
        if (p.online && (obj->print_status == "RUNNING" || obj->print_status == "PAUSE"))
            p.progress = obj->mc_print_percent;
        printers.push_back(std::move(p));
    }
    done(std::move(printers), {});
}

void BambuCloudProvider::open_printer(wxWindow* parent, const std::string& printer_id)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || !bundle->use_bbl_device_tab()) {
        MessageDialog(parent, _L("Select a Bambu Lab printer preset to control this printer."), display_name(),
                      wxOK | wxICON_INFORMATION)
            .ShowModal();
        return;
    }
    if (MainFrame* frame = wxGetApp().mainframe)
        frame->jump_to_monitor(printer_id);
}

bool BambuCloudProvider::handles_printer_preset(const DynamicPrintConfig& printer_config) const
{
    return effective_agent_id(printer_config) == BBL_PRINTER_AGENT_ID;
}

// ---------------------------------------------------------------------------------------------
// PrinterAgentCloudProvider
// ---------------------------------------------------------------------------------------------

PrinterAgentCloudProvider::PrinterAgentCloudProvider(
    std::string provider_id, wxString display_name, int order, std::string agent_id, std::string cloud_provider, wxColour brand_colour)
    : m_id(std::move(provider_id))
    , m_name(std::move(display_name))
    , m_order(order)
    , m_agent_id(std::move(agent_id))
    , m_cloud_provider(std::move(cloud_provider))
    , m_brand(brand_colour)
{}

PrinterAgentCloudProvider::~PrinterAgentCloudProvider()
{
    if (m_agent) {
        m_agent->set_on_ssdp_msg_fn(nullptr);
        m_agent->start_discovery(false, false);
    }
}

bool PrinterAgentCloudProvider::is_available() const { return NetworkAgentFactory::is_printer_agent_registered(m_agent_id); }

wxString PrinterAgentCloudProvider::unavailable_reason() const
{
    // TRN %1% is a printer brand, e.g. "Elegoo".
    return format_wxstr(_L("The %1% printer connection is not installed or is turned off in the plug-in manager."), m_name);
}

std::string PrinterAgentCloudProvider::running_cloud_provider() const
{
    const std::string provider = m_cloud_provider.empty() ? NetworkAgentFactory::cloud_provider_for_agent(m_agent_id) : m_cloud_provider;
    // The Orca account is the fallback every agent gets; it is not this service's account.
    if (provider == ORCA_CLOUD_PROVIDER)
        return {};
    NetworkAgent* agent = wxGetApp().getAgent();
    return agent != nullptr && agent->get_cloud_agent(provider) != nullptr ? provider : std::string();
}

void PrinterAgentCloudProvider::clear_printers()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_seen.clear();
    }
    CloudPrinterProviders::notify_changed(m_id);
}

bool PrinterAgentCloudProvider::is_logged_in() const
{
    if (const std::string provider = running_cloud_provider(); !provider.empty())
        return wxGetApp().is_user_login(provider);
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_seen.empty();
}

wxString PrinterAgentCloudProvider::account_name() const
{
    const std::string provider = running_cloud_provider();
    if (provider.empty())
        return {};
    std::shared_ptr<ICloudServiceAgent> cloud = wxGetApp().getAgent()->get_cloud_agent(provider);
    if (!cloud || !cloud->is_user_login())
        return {};
    std::string name = cloud->get_user_nickname();
    if (name.empty())
        name = cloud->get_user_name();
    return from_u8(name);
}

void PrinterAgentCloudProvider::login(wxWindow* parent)
{
    if (const std::string provider = running_cloud_provider(); !provider.empty()) {
        wxGetApp().request_login(true, provider);
        clear_printers();
        return;
    }
    // TRN %1% is a printer brand, e.g. "Elegoo".
    MessageDialog(parent,
                  format_wxstr(_L("Select a %1% printer preset and connect to it on the Printer page. Its printers then show up here."), m_name),
                  m_name, wxOK | wxICON_INFORMATION)
        .ShowModal();
}

void PrinterAgentCloudProvider::logout()
{
    if (const std::string provider = running_cloud_provider(); !provider.empty())
        wxGetApp().request_user_logout(provider, true);
    clear_printers();
}

std::shared_ptr<IPrinterAgent> PrinterAgentCloudProvider::ensure_agent()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_agent)
        return m_agent;

    // Straight from the factory, not create_printer_agent_by_id(): that returns the cached
    // instance the app may be using, and setting our discovery callback on it would steal its
    // announcements.
    PrinterAgentFactory factory;
    if (const PrinterAgentInfo* info = NetworkAgentFactory::get_printer_agent_info(m_agent_id))
        factory = info->factory;
    if (!factory)
        return nullptr;

    std::shared_ptr<ICloudServiceAgent> cloud;
    if (NetworkAgent* app_agent = wxGetApp().getAgent()) {
        cloud = app_agent->get_cloud_agent(NetworkAgentFactory::cloud_provider_for_agent(m_agent_id));
        if (!cloud)
            cloud = app_agent->get_cloud_agent(ORCA_CLOUD_PROVIDER);
    }
    m_agent = factory(cloud, data_dir());
    if (!m_agent)
        return nullptr;

    m_agent->set_queue_on_main_fn([](std::function<void()> fn) { wxGetApp().CallAfter(std::move(fn)); });
    m_agent->set_on_ssdp_msg_fn([this](std::string json) {
        CloudPrinter p;
        if (!parse_announcement(json, p))
            return;
        p.provider_id = m_id;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_seen.find(p.id);
            // Only news is announced: every refresh re-runs discovery, and a notification
            // refreshes the page, so repeating unchanged devices would loop.
            if (it != m_seen.end() && it->second.name == p.name && it->second.model == p.model &&
                it->second.status == p.status && it->second.online == p.online && it->second.progress == p.progress)
                return;
            m_seen[p.id] = std::move(p);
        }
        CloudPrinterProviders::notify_changed(m_id);
    });
    return m_agent;
}

bool PrinterAgentCloudProvider::parse_announcement(const std::string& json, CloudPrinter& out) const
{
    try {
        const nlohmann::json j = nlohmann::json::parse(json);
        out.id = j.value("dev_id", std::string());
        if (out.id.empty())
            return false;
        out.name  = from_u8(j.value("dev_name", out.id));
        out.model = from_u8(j.value("dev_type", std::string()));
        out.online = j.value("online", true);
        if (j.contains("status") && j["status"].is_string())
            out.status = from_u8(j["status"].get<std::string>());
        else
            out.status = out.online ? _L("Online") : _L("Offline");
        if (j.contains("progress") && j["progress"].is_number())
            out.progress = j["progress"].get<int>();
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Device hub: bad announcement from agent " << m_agent_id << ": " << e.what();
        return false;
    }
}

void PrinterAgentCloudProvider::refresh_printers(PrintersCallback done)
{
    if (std::shared_ptr<IPrinterAgent> agent = ensure_agent())
        agent->start_discovery(true, false);

    // Announcements arrive asynchronously and re-notify; report what is known now.
    std::vector<CloudPrinter> printers;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [printer_id, printer] : m_seen)
            printers.push_back(printer);
    }
    done(std::move(printers), {});
}

void PrinterAgentCloudProvider::open_printer(wxWindow* parent, const std::string& /*printer_id*/)
{
    const PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr && handles_printer_preset(bundle->printers.get_edited_preset().config)) {
        if (MainFrame* frame = wxGetApp().mainframe)
            frame->select_tab(TAB_ID_MONITOR);
        return;
    }
    login(parent);
}

bool PrinterAgentCloudProvider::handles_printer_preset(const DynamicPrintConfig& printer_config) const
{
    return effective_agent_id(printer_config) == m_agent_id;
}

// ---------------------------------------------------------------------------------------------
// MarkforgedCloudProvider
// ---------------------------------------------------------------------------------------------

namespace {

// Written by the Markforged thread's agent plug-in; other keys in it belong to the plug-in.
boost::filesystem::path markforged_settings_path()
{
    // The plug-in keeps its data next to itself, in its package folder (see seed_bundled_plugins).
    return boost::filesystem::path(data_dir()) / "orca_plugins" / "markforged_eiger" / "markforged_eiger_data" / "settings.json";
}

nlohmann::json read_markforged_settings()
{
    try {
        boost::nowide::ifstream in(markforged_settings_path().string());
        if (in)
            return nlohmann::json::parse(in);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Markforged settings unreadable: " << e.what();
    }
    return nlohmann::json::object();
}

bool write_markforged_settings(const nlohmann::json& settings)
{
    try {
        const boost::filesystem::path path = markforged_settings_path();
        boost::filesystem::create_directories(path.parent_path());
        boost::nowide::ofstream out(path.string());
        out << settings.dump(2);
        return bool(out);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Markforged settings not saved: " << e.what();
        return false;
    }
}

} // namespace

MarkforgedCloudProvider::MarkforgedCloudProvider() : PrinterAgentCloudProvider("markforged", "Markforged", 40, "markforged_eiger", {}, wxColour("#8B8F95")) {}

bool MarkforgedCloudProvider::is_logged_in() const
{
    const nlohmann::json settings = read_markforged_settings();
    return settings.is_object() && !settings.value("access_key", std::string()).empty() &&
           !settings.value("secret_key", std::string()).empty();
}

void MarkforgedCloudProvider::login(wxWindow* parent)
{
    wxDialog dlg(parent, wxID_ANY, _L("Markforged account"));
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* intro = new wxStaticText(&dlg, wxID_ANY,
                                   _L("Enter an Eiger API key pair. Create one in Eiger under Settings > API keys."));
    intro->Wrap(dlg.FromDIP(380));
    sizer->Add(intro, 0, wxALL, dlg.FromDIP(12));

    auto* grid = new wxFlexGridSizer(2, dlg.FromDIP(8), dlg.FromDIP(8));
    grid->AddGrowableCol(1);
    auto* access = new wxTextCtrl(&dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(dlg.FromDIP(260), -1));
    auto* secret = new wxTextCtrl(&dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(dlg.FromDIP(260), -1), wxTE_PASSWORD);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Access key")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(access, 1, wxEXPAND);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Secret key")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(secret, 1, wxEXPAND);
    sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, dlg.FromDIP(12));

    if (wxSizer* buttons = dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL))
        sizer->Add(buttons, 0, wxEXPAND | wxALL, dlg.FromDIP(12));
    dlg.SetSizerAndFit(sizer);

    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string access_key = into_u8(access->GetValue().Strip(wxString::both));
    const std::string secret_key = into_u8(secret->GetValue().Strip(wxString::both));
    if (access_key.empty() || secret_key.empty())
        return;

    // Check the keys before keeping them, so a typo shows up here and not as an empty printer list.
    try {
        wxBusyCursor busy;
        Eiger({access_key, secret_key}).devices();
    } catch (const std::exception& e) {
        MessageDialog(parent, wxString::FromUTF8(e.what()), display_name(), wxOK | wxICON_ERROR).ShowModal();
        return;
    }

    // Merge: the plug-in keeps its own settings in the same file.
    nlohmann::json settings = read_markforged_settings();
    if (!settings.is_object())
        settings = nlohmann::json::object();
    settings["access_key"] = access_key;
    settings["secret_key"] = secret_key;
    if (!write_markforged_settings(settings))
        MessageDialog(parent, _L("The API keys could not be saved."), display_name(), wxOK | wxICON_ERROR).ShowModal();
    clear_printers();
}

wxString MarkforgedCloudProvider::account_name() const
{
    const std::string key = read_markforged_settings().value("access_key", std::string());
    if (key.empty())
        return {};
    // TRN %1% is the last characters of an Eiger API access key.
    return format_wxstr(_L("API key …%1%"), key.size() > 4 ? key.substr(key.size() - 4) : key);
}

void MarkforgedCloudProvider::logout()
{
    nlohmann::json settings = read_markforged_settings();
    if (settings.is_object()) {
        settings.erase("access_key");
        settings.erase("secret_key");
        write_markforged_settings(settings);
    }
    clear_printers();
}

// ---------------------------------------------------------------------------------------------
// ElegooCloudProvider
// ---------------------------------------------------------------------------------------------

ElegooCloudProvider::ElegooCloudProvider()
    : PrinterAgentCloudProvider(ELEGOO_CLOUD_PROVIDER, "Elegoo", 20, "elegoo_cc2", {}, wxColour("#3A6FD8"))
{}

bool ElegooCloudProvider::is_logged_in() const { return ElegooCloud::instance().signed_in(); }

wxString ElegooCloudProvider::account_name() const
{
    return wxString::FromUTF8(ElegooCloud::instance().account_label());
}

void ElegooCloudProvider::login(wxWindow* parent) { ElegooLoginDialog::sign_in(parent); }

void ElegooCloudProvider::logout()
{
    ElegooCloud::instance().sign_out();
    clear_printers();
}

// The account's printers from the last cloud answer, shown while a new one is on its way.
static std::mutex                s_elegoo_cloud_mutex;
static std::vector<CloudPrinter> s_elegoo_cloud_printers;

// A printer of the account (keyed by serial) opens the cloud monitor, like a Bambu cloud printer
// opens the Device tab. A printer found on this network ("cc2-<ip>") opens its own web page on
// the Device tab, which talks to it directly. The generic agent provider fell back to login()
// here, so a double-click asked for the Elegoo sign-in again and never opened anything.
void ElegooCloudProvider::open_printer(wxWindow* parent, const std::string& printer_id)
{
    const std::string ip = printer_id.rfind("cc2-", 0) == 0 ? printer_id.substr(4) : std::string();
    if (ip.empty()) {
        if (!ElegooCloud::instance().signed_in()) {
            login(parent);
            if (!ElegooCloud::instance().signed_in())
                return;
        }
        wxString name;
        {
            std::lock_guard<std::mutex> lock(s_elegoo_cloud_mutex);
            for (const CloudPrinter& p : s_elegoo_cloud_printers)
                if (p.id == printer_id)
                    name = p.name;
        }
        show_elegoo_cloud_monitor(parent, printer_id, name);
        return;
    }
    if (const PresetBundle* bundle = wxGetApp().preset_bundle; bundle != nullptr) {
        const DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
        const auto*               ht  = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type");
        const bool                has_host = cfg.has("print_host") && !cfg.opt_string("print_host").empty();
        // An Elegoo preset: its own address, else the one this printer was discovered at.
        if ((ht != nullptr && ht->value == htElegooLink && (has_host || !ip.empty())) || handles_printer_preset(cfg)) {
            if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr && frame->open_printer_web_ui(ip))
                return;
        }
    }
    wxString text = _L("To control this printer here, select your Elegoo printer preset and enter the printer's IP address in "
                       "its connection settings. Its page then opens on the Device tab.");
    text += "\n\n" + format_wxstr(_L("Open the printer's page (%1%) in your web browser now?"), ip);
    if (MessageDialog(parent, text, display_name(), wxYES_NO | wxICON_QUESTION).ShowModal() == wxID_YES)
        wxLaunchDefaultBrowser(wxString::FromUTF8("http://" + ip));
}

// One account fetch at a time: the page polls every few seconds, and on a slow connection a
// fetch outlasts that; overlapping fetches would pile up and could answer out of order.
static std::atomic<bool>         s_elegoo_cloud_fetching{false};

static std::vector<CloudPrinter> merge_lan(std::vector<CloudPrinter> out, const std::vector<CloudPrinter>& lan)
{
    // A LAN printer the account also lists shows once (the cloud entry, keyed by serial).
    for (const CloudPrinter& p : lan) {
        bool dup = false;
        for (const CloudPrinter& c : out)
            if (c.id == p.id) { dup = true; break; }
        if (!dup)
            out.push_back(p);
    }
    return out;
}

void ElegooCloudProvider::refresh_printers(PrintersCallback done)
{
    // Printers on the local network, from the LAN agent.
    std::vector<CloudPrinter> lan;
    PrinterAgentCloudProvider::refresh_printers([&lan](std::vector<CloudPrinter> printers, wxString) { lan = std::move(printers); });
    if (!ElegooCloud::instance().signed_in()) {
        done(std::move(lan), {});
        return;
    }
    // Show the LAN printers (and the last known cloud ones) right away: the cloud call can take
    // up to a connect timeout per request when the internet is slow or down.
    {
        std::lock_guard<std::mutex> lock(s_elegoo_cloud_mutex);
        done(merge_lan(s_elegoo_cloud_printers, lan), {});
    }
    if (s_elegoo_cloud_fetching.exchange(true))
        return; // the fetch in flight answers with fresh data
    // The account's printers: blocking HTTPS, so off the GUI thread (the caller marshals the callback).
    const std::string provider = id();
    std::thread([done, lan = std::move(lan), provider]() {
        std::vector<CloudPrinter> out;
        wxString                  error;
        try {
            for (const ElegooCloudPrinter& p : ElegooCloud::instance().printers()) {
                // The device list carries no status; the report data does (1 idle, 2 printing).
                const int    state = ElegooCloud::instance().printer_state(p.serial);
                CloudPrinter cp;
                cp.provider_id = provider;
                cp.id          = p.serial;
                cp.name        = wxString::FromUTF8(p.name.empty() ? p.serial : p.name);
                cp.model       = wxString::FromUTF8(p.model);
                cp.online      = state >= 0;
                cp.status      = state == 2 ? _L("Printing") : state == 1 ? _L("Idle") : _L("Unknown");
                out.push_back(std::move(cp));
            }
            std::lock_guard<std::mutex> lock(s_elegoo_cloud_mutex);
            s_elegoo_cloud_printers = out;
        } catch (const ElegooAuthError& ex) {
            error = wxString::FromUTF8(ex.what());
            std::lock_guard<std::mutex> lock(s_elegoo_cloud_mutex);
            s_elegoo_cloud_printers.clear();
        } catch (const std::exception& ex) {
            // Offline or the cloud is slow: keep the last known account printers, marked unreachable.
            error = _L("Could not reach the Elegoo cloud. Check the internet connection; printers on this network still work.") +
                    "\n" + wxString::FromUTF8(ex.what());
            std::lock_guard<std::mutex> lock(s_elegoo_cloud_mutex);
            out = s_elegoo_cloud_printers;
            for (CloudPrinter& c : out) {
                c.online = false;
                c.status = _L("Unknown");
            }
        }
        s_elegoo_cloud_fetching = false;
        done(merge_lan(std::move(out), lan), error);
    }).detach();
}
}} // namespace Slic3r::GUI
