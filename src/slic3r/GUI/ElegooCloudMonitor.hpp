#pragma once

#include <memory>
#include <string>

#include <wx/string.h>

class wxWindow;

namespace Slic3r { namespace GUI {

// Status and print controls of an Elegoo printer that is connected through the Elegoo cloud
// (no local address, so the printer's own LAN web page cannot be used). Opens a window per
// printer, or raises the one already open.
void show_elegoo_cloud_monitor(wxWindow* parent, const std::string& serial, const wxString& name);

}} // namespace Slic3r::GUI
