#include "ElegooCloudMonitor.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>

#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/weakref.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "slic3r/Utils/ElegooCloud.hpp"

namespace Slic3r { namespace GUI {

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

class ElegooCloudMonitorDialog : public wxDialog
{
public:
    ElegooCloudMonitorDialog(wxWindow* parent, const std::string& serial, const wxString& name)
        : wxDialog(parent, wxID_ANY, name.empty() ? wxString("Elegoo") : name, wxDefaultPosition, wxDefaultSize,
                   wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
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
        buttons->AddStretchSpacer();
        auto* close = new wxButton(this, wxID_CLOSE, _L("Close"));
        buttons->Add(close, 0);
        top->Add(buttons, 0, wxEXPAND | wxALL, pad);

        m_error = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_error->SetForegroundColour(wxColour("#D9534F"));
        top->Add(m_error, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

        SetSizerAndFit(top);
        SetMinSize(FromDIP(wxSize(420, -1)));
        SetSize(FromDIP(wxSize(460, -1)).x, GetSize().y);
        wxGetApp().UpdateDlgDarkUI(this);
        CentreOnParent();
        enable_controls(nullptr);

        m_pause->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(ElegooCommand::Pause); });
        m_resume->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send(ElegooCommand::Resume); });
        m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (wxMessageBox(_L("Stop the current print? It cannot be resumed."), GetTitle(), wxYES_NO | wxICON_WARNING, this) == wxYES)
                send(ElegooCommand::Stop);
        });
        close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
            *m_alive = false;
            m_timer.Stop();
            // Logging out of the messaging session blocks briefly; never on the GUI thread.
            std::thread([]() { ElegooCloud::instance().close_live_session(); }).detach();
            Destroy();
        });
        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); });
        m_timer.Start(POLL_MS);
        poll();
    }

    const std::string& serial() const { return m_serial; }

private:
    void poll()
    {
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

// One window at a time: the cloud allows one messaging session per account.
wxWeakRef<ElegooCloudMonitorDialog> s_open;

} // namespace

void show_elegoo_cloud_monitor(wxWindow* parent, const std::string& serial, const wxString& name)
{
    if (s_open) {
        if (s_open->serial() == serial) {
            s_open->Raise();
            return;
        }
        s_open->Close();
    }
    auto* dlg = new ElegooCloudMonitorDialog(parent, serial, name);
    s_open    = dlg;
    dlg->Show();
}

}} // namespace Slic3r::GUI
