#include "LocalPrintersProvider.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/PhysicalPrinterDialog.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/Serial.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr const char* PRESET_PREFIX = "preset:";
constexpr const char* USB_PREFIX    = "usb:";

// A network printer to list, gathered on the GUI thread and probed on a worker.
struct LanTarget
{
    CloudPrinter printer;
    std::string  host;
    std::string  port;
};

// "http://10.0.0.5:7125/api" -> host "10.0.0.5", port "7125". Port defaults to the scheme's.
bool split_host(const std::string& url, std::string& host, std::string& port)
{
    std::string rest = url;
    port             = "80";
    if (const size_t scheme = rest.find("://"); scheme != std::string::npos) {
        if (boost::iequals(rest.substr(0, scheme), "https"))
            port = "443";
        rest = rest.substr(scheme + 3);
    }
    if (const size_t at = rest.rfind('@', rest.find('/')); at != std::string::npos)
        rest = rest.substr(at + 1); // user:password@
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (!rest.empty() && rest.front() == '[') { // [IPv6]:port
        const size_t close = rest.find(']');
        if (close == std::string::npos)
            return false;
        host = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':')
            port = rest.substr(close + 2);
    } else if (const size_t colon = rest.find(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port = rest.substr(colon + 1);
    } else {
        host = rest;
    }
    return !host.empty() && !port.empty();
}

// Whether something accepts TCP connections at host:port within the timeout.
bool reachable(const std::string& host, const std::string& port, std::chrono::milliseconds timeout)
{
    using boost::asio::ip::tcp;
    boost::asio::io_context io;
    tcp::resolver           resolver(io);
    tcp::socket             socket(io);
    bool                    ok = false;
    resolver.async_resolve(host, port, [&](const boost::system::error_code& ec, tcp::resolver::results_type results) {
        if (ec)
            return;
        boost::asio::async_connect(socket, results, [&](const boost::system::error_code& ec2, const tcp::endpoint&) { ok = !ec2; });
    });
    io.run_for(timeout);
    io.stop();
    return ok;
}

wxString host_type_label(const DynamicPrintConfig& config)
{
    const auto* opt = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    const ConfigOptionDef* def = print_config_def.get("host_type");
    if (opt == nullptr || def == nullptr)
        return {};
    const size_t idx = size_t(opt->value);
    return idx < def->enum_labels.size() ? _(def->enum_labels[idx]) : wxString();
}

// Claimed by one of the cloud services' rail entries.
bool claimed_by_other(const DynamicPrintConfig& config, const std::string& self)
{
    for (const CloudPrinterProviderPtr& p : CloudPrinterProviders::all())
        if (p->id() != self && p->handles_printer_preset(config))
            return true;
    return false;
}

bool has_print_host(const DynamicPrintConfig& config)
{
    const auto* host = config.option<ConfigOptionString>("print_host");
    return host != nullptr && !host->value.empty();
}

std::vector<CloudPrinter> usb_printers(const std::string& provider_id)
{
    std::vector<CloudPrinter> out;
    try {
        for (const Utils::SerialPortInfo& port : Utils::scan_serial_ports_extended()) {
            // USB devices only: a VID/PID is known. Leaves out the board's own COM ports and Bluetooth.
            if (port.id_vendor == unsigned(-1) && !port.is_printer)
                continue;
            CloudPrinter p;
            p.provider_id = provider_id;
            p.id          = USB_PREFIX + port.port;
            p.name        = from_u8(port.friendly_name);
            p.model       = port.is_printer ? _L("USB printer") : _L("USB serial device");
            p.online      = true;
            // TRN %1% is a serial port, e.g. "COM3" or "/dev/ttyUSB0".
            p.status      = format_wxstr(_L("USB (%1%)"), from_u8(port.port));
            out.push_back(std::move(p));
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Device hub: serial port scan failed: " << e.what();
    }
    return out;
}

} // namespace

struct LocalPrintersProvider::State
{
    std::mutex                mutex;
    std::vector<CloudPrinter> last; // last full list, returned while a probe is still running
    std::atomic<bool>         busy{false};
};

LocalPrintersProvider::LocalPrintersProvider() : m_state(std::make_shared<State>()) {}

wxString LocalPrintersProvider::display_name() const { return _L("USB & LAN"); }

wxString LocalPrintersProvider::account_name() const
{
    return _L("Printers on your network or plugged in over USB that are not on a cloud account.");
}

void LocalPrintersProvider::login(wxWindow* parent)
{
    PhysicalPrinterDialog dlg(parent);
    dlg.ShowModal();
    CloudPrinterProviders::notify_changed(id());
}

bool LocalPrintersProvider::handles_printer_preset(const DynamicPrintConfig& printer_config) const
{
    return has_print_host(printer_config) && !claimed_by_other(printer_config, id());
}

void LocalPrintersProvider::refresh_printers(PrintersCallback done)
{
    std::shared_ptr<State> state = m_state;
    if (state->busy.exchange(true)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        done(state->last, {});
        return;
    }

    // Presets are read here on the GUI thread; only the probing goes to the worker.
    std::vector<LanTarget> targets;
    if (const PresetBundle* bundle = wxGetApp().preset_bundle) {
        for (const Preset& preset : bundle->printers) {
            if (!preset.is_visible || !handles_printer_preset(preset.config))
                continue;
            LanTarget t;
            const std::string& url = preset.config.opt_string("print_host");
            if (!split_host(url, t.host, t.port))
                continue;
            t.printer.provider_id = id();
            t.printer.id          = PRESET_PREFIX + preset.name;
            t.printer.name        = from_u8(preset.name);
            const wxString type   = host_type_label(preset.config);
            // TRN %1% is a print host type (e.g. "OctoPrint"), %2% its address.
            t.printer.model = type.empty() ? from_u8(t.host) : format_wxstr(_L("%1% at %2%"), type, from_u8(t.host));
            targets.push_back(std::move(t));
        }
    }

    const std::string provider_id = id();
    std::thread([state, done, targets = std::move(targets), provider_id]() mutable {
        std::vector<CloudPrinter> out;
        for (LanTarget& t : targets) {
            t.printer.online = reachable(t.host, t.port, std::chrono::milliseconds(1200));
            t.printer.status = t.printer.online ? _L("Online") : _L("Offline");
            out.push_back(std::move(t.printer));
        }
        for (CloudPrinter& p : usb_printers(provider_id))
            out.push_back(std::move(p));
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->last = out;
        }
        state->busy = false;
        done(std::move(out), {});
    }).detach();
}

void LocalPrintersProvider::open_printer(wxWindow* parent, const std::string& printer_id)
{
    if (boost::starts_with(printer_id, USB_PREFIX)) {
        MessageDialog(parent,
                      _L("Unified Slicer can't send prints over USB. Export the G-code to the printer's SD card or USB stick, "
                         "or connect the printer to your network (for example with OctoPrint or Klipper) and add it here."),
                      display_name(), wxOK | wxICON_INFORMATION)
            .ShowModal();
        return;
    }
    if (!boost::starts_with(printer_id, PRESET_PREFIX))
        return;

    // Select the printer's preset; the hub then offers its Web UI page.
    const std::string preset_name = printer_id.substr(std::char_traits<char>::length(PRESET_PREFIX));
    PresetBundle*     bundle      = wxGetApp().preset_bundle;
    Tab*              tab         = wxGetApp().get_tab(Preset::TYPE_PRINTER);
    if (bundle == nullptr || tab == nullptr)
        return;
    if (bundle->printers.get_edited_preset().name != preset_name && !tab->select_preset(preset_name))
        return;
    wxGetApp().CallAfter([]() {
        if (MainFrame* frame = wxGetApp().mainframe)
            if (!frame->open_printer_web_ui())
                frame->jump_to_monitor();
    });
}

}} // namespace Slic3r::GUI
