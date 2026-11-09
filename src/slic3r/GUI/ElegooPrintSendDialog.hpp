#pragma once

#include <memory>
#include <optional>

#include <boost/filesystem/path.hpp>

#include "slic3r/Utils/PrintHost.hpp"

class wxWindow;

namespace Slic3r { namespace GUI {

class VendorSendBackend;
struct SendJobSummary;

// ElegooSlicer's print-send dialog: its own web page (resources/web/elegoo/printer/print_send,
// taken from ElegooSlicer), answered over the same IPC messages ElegooSlicer's PrintSendDialogEx
// answers, with the printers, Canvas slots and upload job of the Elegoo send backend.
//
// Returns false when the page can't be shown (no WebView runtime, page missing); the caller then
// shows the built-in dialog. Otherwise fills job with the job to export and upload, or leaves it
// empty when the user cancelled.
bool show_elegoo_print_send_dialog(wxWindow* parent, std::shared_ptr<VendorSendBackend> backend, const SendJobSummary& summary,
                                   const boost::filesystem::path& default_output, std::optional<PrintHostJob>& job);

}} // namespace Slic3r::GUI
