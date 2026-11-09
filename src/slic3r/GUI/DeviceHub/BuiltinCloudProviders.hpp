#pragma once

#include "CloudPrinterProvider.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

class IPrinterAgent;

namespace GUI {

// Bambu Lab: the account of Bambu's network plug-in, and the machines the DeviceManager knows.
// Its printers are controlled on the Device tab's native Printer page.
class BambuCloudProvider : public CloudPrinterProvider
{
public:
    std::string id() const override;
    wxString    display_name() const override;
    int         order() const override { return 10; }
    wxColour    brand_colour() const override { return wxColour("#2FA84F"); }

    bool     is_available() const override;
    wxString unavailable_reason() const override;
    void     install_or_enable(wxWindow* parent) override;

    bool     is_logged_in() const override;
    wxString account_name() const override;
    void     login(wxWindow* parent) override;
    void     logout() override;

    void refresh_printers(PrintersCallback done) override;
    void open_printer(wxWindow* parent, const std::string& printer_id) override;

    bool handles_printer_preset(const DynamicPrintConfig& printer_config) const override;
};

// A service whose printers come from a printer agent (built-in or Python plug-in): the provider
// runs a private instance of that agent for discovery only and lists every device it announces.
// Account: when an ICloudServiceAgent runs for cloud_provider (default: the agent's declared
// provider), login goes through GUI_App with that provider. Otherwise the provider counts as signed
// in once the agent reports a device; a service with its own login overrides login()/logout().
class PrinterAgentCloudProvider : public CloudPrinterProvider
{
public:
    PrinterAgentCloudProvider(std::string provider_id, wxString display_name, int order, std::string agent_id,
                              std::string cloud_provider = {}, wxColour brand_colour = {});
    ~PrinterAgentCloudProvider() override;

    std::string id() const override { return m_id; }
    wxString    display_name() const override { return m_name; }
    int         order() const override { return m_order; }
    wxColour    brand_colour() const override { return m_brand; }
    const std::string& agent_id() const { return m_agent_id; }

    bool     is_available() const override;
    wxString unavailable_reason() const override;

    bool     is_logged_in() const override;
    wxString account_name() const override;
    void     login(wxWindow* parent) override;
    void     logout() override;

    void refresh_printers(PrintersCallback done) override;
    void open_printer(wxWindow* parent, const std::string& printer_id) override;

    bool handles_printer_preset(const DynamicPrintConfig& printer_config) const override;

protected:
    // The account's cloud agent id when one is running, else empty.
    std::string running_cloud_provider() const;
    // Forgets the printers seen so far, e.g. after the account changed.
    void clear_printers();

    // Parses one discovery announcement (the agent's ssdp JSON) into a printer; false to skip it.
    virtual bool parse_announcement(const std::string& json, CloudPrinter& out) const;

private:
    std::shared_ptr<IPrinterAgent> ensure_agent();

    std::string m_id;
    wxString    m_name;
    int         m_order;
    std::string m_agent_id;
    std::string m_cloud_provider;
    wxColour    m_brand;

    mutable std::mutex                   m_mutex;
    std::shared_ptr<IPrinterAgent>       m_agent;
    std::map<std::string, CloudPrinter>  m_seen; // by printer id
};

// Markforged (Eiger cloud): the agent reads the account's API keys from its plug-in settings
// file, so logging in asks for the keys and stores them there.
class MarkforgedCloudProvider : public PrinterAgentCloudProvider
{
public:
    MarkforgedCloudProvider();

    // Signing in only needs the Eiger API, not the agent plug-in.
    bool     is_available() const override { return true; }
    wxString account_name() const override;
    bool     is_logged_in() const override;
    void login(wxWindow* parent) override;
    void logout() override;
};

// Elegoo: the Elegoo cloud account (native client, slic3r/Utils/ElegooCloud) lists the account's
// printers; printers on the local network also show through the CC2 LAN agent.
class ElegooCloudProvider : public PrinterAgentCloudProvider
{
public:
    ElegooCloudProvider();

    bool     is_available() const override { return true; }
    bool     is_logged_in() const override;
    wxString account_name() const override;
    void     login(wxWindow* parent) override;
    void     logout() override;

    void refresh_printers(PrintersCallback done) override;
    // The printer list, with an opened cloud printer's page shown in its place.
    wxWindow* create_page(wxWindow* parent) override;
    void open_printer(wxWindow* parent, const std::string& printer_id) override;
};

}} // namespace Slic3r::GUI
