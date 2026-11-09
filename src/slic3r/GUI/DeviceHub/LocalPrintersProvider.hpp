#pragma once

#include "CloudPrinterProvider.hpp"

#include <memory>

namespace Slic3r { namespace GUI {

// Printers that belong to none of the cloud services: printer presets (and physical printers) with
// a network print host (OctoPrint, Moonraker, PrusaLink, Duet, ...), and printers plugged in over USB.
// There is no account; the page's primary button adds a printer through the Connection dialog.
class LocalPrintersProvider : public CloudPrinterProvider
{
public:
    LocalPrintersProvider();

    std::string id() const override { return "local"; }
    wxString    display_name() const override;
    int         order() const override { return 50; }

    bool     has_account() const override { return false; }
    bool     is_logged_in() const override { return true; }
    wxString account_name() const override;
    // "Add printer": the Connection dialog of the selected printer preset.
    void     login(wxWindow* parent) override;
    void     logout() override {}

    void refresh_printers(PrintersCallback done) override;
    void open_printer(wxWindow* parent, const std::string& printer_id) override;

    // A preset with a print host that no other service claims.
    bool handles_printer_preset(const DynamicPrintConfig& printer_config) const override;

    struct State;

private:
    std::shared_ptr<State> m_state; // shared with the reachability probe thread
};

}} // namespace Slic3r::GUI
