#include "BuiltinCloudProviders.hpp"
#include "CloudPrinterProvider.hpp"

#include <memory>

#include "slic3r/Utils/CloudProvider.hpp"

namespace Slic3r { namespace GUI {

// The services on the Device tab's rail, in rail order. One line per service, so each
// integration replaces only its own line.
void CloudPrinterProviders::register_builtin()
{
    static bool registered = false;
    if (registered)
        return;
    registered = true;

    add(std::make_shared<BambuCloudProvider>());
    // Elegoo: Elegoo cloud account (native) plus the CC2 LAN agent for printers on the local network.
    add(std::make_shared<ElegooCloudProvider>());
    // Creality: cloud agent "creality_cloud" with account "creality".
    add(std::make_shared<PrinterAgentCloudProvider>("creality", "Creality", 30, "creality_cloud", "creality",
                                                    wxColour("#D8762C")));
    // Markforged: Eiger cloud agent "markforged_eiger", API keys.
    add(std::make_shared<MarkforgedCloudProvider>());
}

}} // namespace Slic3r::GUI
