#include "DeviceHubPanel.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <tuple>
#include <utility>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <wx/bitmap.h>
#include <wx/cursor.h>
#include <wx/dataview.h>
#include <wx/dcclient.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/weakref.h>
#include <wx/wrapsizer.h>

#include "slic3r/GUI/AccentColor.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr int REFRESH_INTERVAL_MS = 5000;
constexpr int CARD_WIDTH          = 340;
constexpr int CARD_IMAGE          = 96;

const char* VIEW_SETTING = "device_hub_view";

bool card_view_setting() { return wxGetApp().app_config->get(VIEW_SETTING) == "cards"; }

wxString progress_text(const CloudPrinter& p) { return p.progress >= 0 ? wxString::Format("%d%%", p.progress) : wxString(); }

// Primary-button look in a brand colour, or the app's own when the colour is invalid.
void style_primary(Button* button, ButtonType type, const wxColour& brand)
{
    button->SetStyle(ButtonStyle::Confirm, type);
    if (!brand.IsOk())
        return;
    const wxColour hover = StateColor::LightenDarkenColor(brand, 20);
    button->SetBackgroundColor(StateColor(std::pair<wxColour, int>(hover, StateColor::Hovered),
                                          std::pair<wxColour, int>(brand, StateColor::Normal)));
    button->SetBorderColor(StateColor(brand));
    button->SetTextColor(StateColor(*wxWHITE));
}

// The printer's picture: the provider's bitmap, else the vendor profile's cover picture of a
// printer model of that name, else a generic one.
wxBitmap printer_image(const CloudPrinter& p, const wxString& vendor, wxWindow* win)
{
    namespace fs  = boost::filesystem;
    const int px  = win->FromDIP(CARD_IMAGE);
    const fs::path images = fs::path(Slic3r::resources_dir()) / "images";
    if (!p.image.empty() && (fs::exists(images / (p.image + ".png")) || fs::exists(images / (p.image + ".svg"))))
        return create_scaled_bitmap(p.image, win, CARD_IMAGE);

    // Cover pictures by vendor and model, looked up once each.
    static std::map<std::string, std::string> covers;
    const std::string vendor_u8 = into_u8(vendor);
    const std::string model_u8  = into_u8(p.model);
    const std::string key       = vendor_u8 + '\n' + model_u8;
    auto it = covers.find(key);
    if (it == covers.end()) {
        std::string found;
        const fs::path dir = fs::path(Slic3r::resources_dir()) / "profiles" / vendor_u8;
        boost::system::error_code ec;
        if (!model_u8.empty() && fs::is_directory(dir, ec)) {
            const std::string model = boost::algorithm::to_lower_copy(model_u8);
            for (fs::directory_iterator f(dir, ec), end; !ec && f != end; f.increment(ec)) {
                std::string stem = f->path().filename().string();
                if (!boost::algorithm::iends_with(stem, "_cover.png"))
                    continue;
                stem = boost::algorithm::to_lower_copy(stem.substr(0, stem.size() - 10));
                // "Elegoo Centauri Carbon 2" for "Centauri Carbon 2" or "Elegoo Centauri Carbon 2".
                if (stem == model || boost::algorithm::ends_with(stem, " " + model)) {
                    found = f->path().string();
                    break;
                }
            }
        }
        it = covers.emplace(key, found).first;
    }
    if (!it->second.empty()) {
        wxImage img;
        if (img.LoadFile(from_u8(it->second), wxBITMAP_TYPE_PNG) && img.IsOk()) {
            const double scale = std::min(double(px) / img.GetWidth(), double(px) / img.GetHeight());
            img.Rescale(std::max(1, int(img.GetWidth() * scale)), std::max(1, int(img.GetHeight() * scale)), wxIMAGE_QUALITY_HIGH);
            return wxBitmap(img);
        }
    }
    return create_scaled_bitmap("printer_placeholder", win, CARD_IMAGE);
}

// Thin rounded bar for a job's progress.
class ProgressStrip : public wxWindow
{
public:
    ProgressStrip(wxWindow* parent, int progress, const wxColour& colour)
        : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, parent->FromDIP(6))), m_progress(progress), m_colour(colour)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC     dc(this);
            const wxSize  size   = GetSize();
            const double  radius = size.y / 2.0;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(GetParent()->GetBackgroundColour());
            dc.DrawRectangle(0, 0, size.x, size.y);
            dc.SetBrush(StateColor::darkModeColorFor(wxColour("#F0F0F1")));
            dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);
            const int filled = size.x * std::clamp(m_progress, 0, 100) / 100;
            if (filled > 0) {
                dc.SetBrush(m_colour);
                dc.DrawRoundedRectangle(0, 0, std::max(filled, size.y), size.y, radius);
            }
        });
    }

private:
    int      m_progress;
    wxColour m_colour;
};

// One printer in the card view: picture, name, model, serial, Details, then progress and status.
// Clicking anywhere on it opens the printer.
class PrinterCard : public StaticBox
{
public:
    PrinterCard(wxWindow* parent, const CloudPrinter& p, const wxString& service, const wxString& vendor, const wxColour& accent,
                std::function<void()> open)
        : StaticBox(parent, wxID_ANY, wxDefaultPosition, wxSize(parent->FromDIP(CARD_WIDTH), -1))
    {
        const wxColour bg    = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
        const wxColour text  = StateColor::darkModeColorFor(wxColour("#262E30"));
        const wxColour muted = StateColor::darkModeColorFor(wxColour("#6B6B6B"));
        const wxColour brand = accent.IsOk() ? accent : wxColour("#767676");
        SetCornerRadius(FromDIP(8));
        SetBorderWidth(1);
        SetBorderColor(StateColor(wxColour("#DBDBDB")));
        SetBackgroundColor(StateColor(wxColour("#FFFFFF")));
        SetBackgroundColour(bg);
        SetMinSize(wxSize(FromDIP(CARD_WIDTH), -1));

        auto* outer = new wxBoxSizer(wxVERTICAL);
        auto* top   = new wxBoxSizer(wxHORIZONTAL);

        auto* image = new wxStaticBitmap(this, wxID_ANY, printer_image(p, vendor, this));
        image->SetBackgroundColour(bg);
        top->Add(image, 0, wxALIGN_TOP | wxRIGHT, FromDIP(14));

        auto* info = new wxBoxSizer(wxVERTICAL);
        auto  add  = [&](const wxString& label, const wxFont& font, const wxColour& colour) {
            auto* t = new wxStaticText(this, wxID_ANY, label, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
            t->SetFont(font);
            t->SetForegroundColour(colour);
            t->SetBackgroundColour(bg);
            t->SetMaxSize(wxSize(FromDIP(CARD_WIDTH - CARD_IMAGE - 50), -1));
            t->SetToolTip(label);
            info->Add(t, 0, wxBOTTOM, FromDIP(4));
            return t;
        };
        add(p.name.empty() ? from_u8(p.id) : p.name, Label::Head_14, text);
        if (!p.model.empty())
            add(p.model, Label::Body_13, muted);
        if (!p.id.empty())
            // TRN Serial number or device id of a printer on its card in the Device tab.
            add(format_wxstr(_L("SN: %1%"), from_u8(p.id)), Label::Body_12, muted);
        if (!service.empty())
            add(service, Label::Body_12, muted);

        auto* details = new Button(this, _L("Details"));
        details->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        details->SetBorderColor(StateColor(brand));
        details->SetTextColor(StateColor(brand));
        details->Bind(wxEVT_BUTTON, [open](wxCommandEvent&) { open(); });
        info->Add(details, 0, wxTOP, FromDIP(4));
        top->Add(info, 1, wxEXPAND);
        outer->Add(top, 0, wxEXPAND | wxALL, FromDIP(14));

        if (p.progress >= 0)
            outer->Add(new ProgressStrip(this, p.progress, brand), 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(14));

        // Status pill: the status in a rounded box, green while online.
        auto* pill = new StaticBox(this, wxID_ANY);
        const wxColour pill_bg = p.online ? wxColour("#E5F6EC") : wxColour("#F0F0F1");
        pill->SetCornerRadius(FromDIP(11));
        pill->SetBorderWidth(0);
        pill->SetBackgroundColor(StateColor(pill_bg));
        pill->SetBackgroundColour(StateColor::darkModeColorFor(pill_bg));
        wxString status = p.status.empty() ? (p.online ? _L("Online") : _L("Offline")) : p.status;
        if (p.progress >= 0)
            status += wxString::Format("  %s", progress_text(p));
        auto* status_text = new wxStaticText(pill, wxID_ANY, status);
        status_text->SetFont(Label::Body_12);
        status_text->SetForegroundColour(p.online ? wxColour("#1A7F45") : muted);
        status_text->SetBackgroundColour(StateColor::darkModeColorFor(pill_bg));
        auto* pill_sizer = new wxBoxSizer(wxHORIZONTAL);
        pill_sizer->AddSpacer(FromDIP(10));
        pill_sizer->Add(status_text, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(4));
        pill_sizer->AddSpacer(FromDIP(10));
        pill->SetSizer(pill_sizer);
        outer->Add(pill, 0, wxALL, FromDIP(14));

        SetSizer(outer);
        Layout();
        Fit();

        // The whole card is the click target, children included.
        std::function<void(wxWindow*)> bind = [&](wxWindow* w) {
            if (w == details)
                return;
            w->SetCursor(wxCursor(wxCURSOR_HAND));
            w->Bind(wxEVT_LEFT_UP, [open](wxMouseEvent&) { open(); });
            for (wxWindow* child : w->GetChildren())
                bind(child);
        };
        bind(this);
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------
// CloudProviderPage
// ---------------------------------------------------------------------------------------------

CloudProviderPage::CloudProviderPage(wxWindow* parent, CloudPrinterProviderPtr provider)
    : wxPanel(parent, wxID_ANY), m_provider(std::move(provider)), m_timer(this)
{
    SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Header: service name and account, with the account and refresh buttons on the right.
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* names  = new wxBoxSizer(wxVERTICAL);
    m_title      = new wxStaticText(this, wxID_ANY, m_provider ? m_provider->display_name() : _L("All printers"));
    m_title->SetFont(Label::Head_16);
    m_account = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_account->SetFont(Label::Body_13);
    names->Add(m_title, 0, wxBOTTOM, FromDIP(4));
    names->Add(m_account, 0);
    header->Add(names, 1, wxALIGN_CENTER_VERTICAL);

    m_accent      = m_provider ? m_provider->brand_colour() : wxColour();
    m_install_btn = new Button(this, _L("Install"));
    style_primary(m_install_btn, ButtonType::Window, m_accent);
    m_install_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_provider)
            m_provider->install_or_enable(this);
        refresh();
    });
    header->Add(m_install_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    m_login_btn = new Button(this, _L("Log in"));
    style_primary(m_login_btn, ButtonType::Window, m_accent);
    m_login_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_provider)
            return;
        if (m_provider->has_account() && m_provider->is_logged_in())
            m_provider->logout();
        else
            m_provider->login(this);
        refresh();
    });
    header->Add(m_login_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    m_refresh_btn = new Button(this, _L("Refresh"));
    m_refresh_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_refresh_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh(); });
    header->Add(m_refresh_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    // Switches between list and cards, for every page.
    m_view_btn = new Button(this, _L("Card view"));
    m_view_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_view_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxGetApp().app_config->set(VIEW_SETTING, m_card_view ? "list" : "cards");
        apply_view();
    });
    header->Add(m_view_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    sizer->Add(header, 0, wxEXPAND | wxALL, FromDIP(16));

    m_notice = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_notice->SetFont(Label::Body_13);
    sizer->Add(m_notice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));

    m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_SINGLE | wxDV_ROW_LINES);
    m_list->AppendTextColumn(_L("Printer"), wxDATAVIEW_CELL_INERT, FromDIP(220));
    m_list->AppendTextColumn(_L("Model"), wxDATAVIEW_CELL_INERT, FromDIP(180));
    if (!m_provider)
        m_list->AppendTextColumn(_L("Service"), wxDATAVIEW_CELL_INERT, FromDIP(120));
    m_list->AppendTextColumn(_L("Status"), wxDATAVIEW_CELL_INERT, FromDIP(140));
    m_list->AppendTextColumn(_L("Progress"), wxDATAVIEW_CELL_INERT, FromDIP(80));
    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, &CloudProviderPage::on_activated, this);
    sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    m_cards = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_cards->SetBackgroundColour(GetBackgroundColour());
    m_cards->SetScrollRate(0, FromDIP(20));
    m_cards->SetSizer(new wxWrapSizer(wxHORIZONTAL));
    m_cards->Hide();
    sizer->Add(m_cards, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    m_empty = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_empty->SetFont(Label::Body_14);
    sizer->Add(m_empty, 0, wxEXPAND | wxALL, FromDIP(16));

    SetSizer(sizer);
    // Polls only while shown; providers that push changes also call notify_changed().
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (IsShownOnScreen())
            refresh();
    });
    apply_view();
}

CloudProviderPage::~CloudProviderPage() { m_timer.Stop(); }

std::vector<CloudPrinterProviderPtr> CloudProviderPage::providers() const
{
    if (m_provider)
        return {m_provider};
    return CloudPrinterProviders::all();
}

bool CloudProviderPage::Show(bool show)
{
    const bool changed = wxPanel::Show(show);
    // Not while the main window is still hidden at startup: refreshing starts the services'
    // discovery, which waits until someone looks at the page.
    if (show && wxGetTopLevelParent(this)->IsShown()) {
        if (m_card_view != card_view_setting())
            apply_view();
        refresh();
        m_timer.Start(REFRESH_INTERVAL_MS);
    } else {
        m_timer.Stop();
    }
    return changed;
}

void CloudProviderPage::refresh()
{
    update_header();
    wxWeakRef<CloudProviderPage> self(this);
    for (const CloudPrinterProviderPtr& p : providers()) {
        if (!p->is_available()) {
            m_printers.erase(p->id());
            continue;
        }
        const std::string provider_id = p->id();
        p->refresh_printers([self, provider_id](std::vector<CloudPrinter> printers, wxString error) {
            wxGetApp().CallAfter([self, provider_id, printers = std::move(printers), error]() mutable {
                if (self)
                    self->on_printers(provider_id, std::move(printers), error);
            });
        });
    }
    rebuild_list();
}

void CloudProviderPage::update_header()
{
    if (!m_provider) {
        m_account->SetLabel(m_card_view ? _L("Every printer from every connected service. Click a printer to open it.") :
                                          _L("Every printer from every connected service. Double-click a printer to open it."));
        m_login_btn->Hide();
        m_install_btn->Hide();
        m_notice->Hide();
        Layout();
        return;
    }

    const bool available = m_provider->is_available();
    m_install_btn->Show(!available);
    m_login_btn->Show(available);
    m_notice->Show(!available);
    if (!available) {
        m_account->SetLabel(_L("Not available"));
        m_notice->SetLabel(m_provider->unavailable_reason());
        m_notice->Wrap(FromDIP(600));
    } else if (!m_provider->has_account()) {
        m_account->SetLabel(m_provider->account_name());
        m_login_btn->SetLabel(_L("Add printer"));
        style_primary(m_login_btn, ButtonType::Window, m_accent);
    } else if (m_provider->is_logged_in()) {
        const wxString name = m_provider->account_name();
        // TRN %1% is the account name of a printer cloud service.
        m_account->SetLabel(name.empty() ? _L("Signed in") : format_wxstr(_L("Signed in as %1%"), name));
        m_login_btn->SetLabel(_L("Log out"));
        m_login_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    } else {
        m_account->SetLabel(_L("Not signed in"));
        m_login_btn->SetLabel(_L("Log in"));
        style_primary(m_login_btn, ButtonType::Window, m_accent);
    }
    Layout();
}

void CloudProviderPage::on_printers(const std::string& provider_id, std::vector<CloudPrinter> printers, const wxString& error)
{
    m_printers[provider_id] = std::move(printers);
    if (error.empty())
        m_errors.erase(provider_id);
    else
        m_errors[provider_id] = error;
    update_header();
    rebuild_list();
}

void CloudProviderPage::rebuild_list()
{
    // Keep the selection across refreshes.
    std::pair<std::string, std::string> selected;
    if (const int row = m_list->GetSelectedRow(); row != wxNOT_FOUND && row < int(m_rows.size()))
        selected = m_rows[row];

    m_list->DeleteAllItems();
    m_rows.clear();
    for (const CloudPrinterProviderPtr& provider : providers()) {
        auto it = m_printers.find(provider->id());
        if (it == m_printers.end())
            continue;
        for (const CloudPrinter& p : it->second) {
            wxVector<wxVariant> row;
            row.push_back(wxVariant(p.name));
            row.push_back(wxVariant(p.model));
            if (!m_provider)
                row.push_back(wxVariant(provider->display_name()));
            row.push_back(wxVariant(p.status));
            row.push_back(wxVariant(progress_text(p)));
            m_list->AppendItem(row);
            m_rows.emplace_back(provider->id(), p.id);
            if (m_rows.back() == selected)
                m_list->SelectRow(m_rows.size() - 1);
        }
    }

    wxString empty;
    if (m_rows.empty()) {
        if (!m_errors.empty())
            empty = m_errors.begin()->second;
        else if (m_provider && !m_provider->is_available())
            empty = wxEmptyString;
        else if (m_provider && !m_provider->is_logged_in())
            // TRN %1% is a printer brand, e.g. "Elegoo".
            empty = format_wxstr(_L("Log in to your %1% account to see your printers."), m_provider->display_name());
        else
            empty = _L("No printers found.");
    }
    m_empty->SetLabel(empty);
    m_empty->Show(!empty.empty());
    if (m_card_view)
        rebuild_cards();
    Layout();
}

void CloudProviderPage::rebuild_cards()
{
    std::vector<CloudPrinter> shown;
    for (const CloudPrinterProviderPtr& provider : providers())
        if (auto it = m_printers.find(provider->id()); it != m_printers.end())
            for (CloudPrinter p : it->second) {
                p.provider_id = provider->id();
                shown.push_back(std::move(p));
            }
    const auto same = [](const CloudPrinter& a, const CloudPrinter& b) {
        return std::tie(a.provider_id, a.id, a.name, a.model, a.status, a.online, a.progress, a.image) ==
               std::tie(b.provider_id, b.id, b.name, b.model, b.status, b.online, b.progress, b.image);
    };
    // Rebuilt only on change: the page refreshes every few seconds.
    if (m_cards->GetSizer()->GetItemCount() == shown.size() && std::equal(shown.begin(), shown.end(), m_carded.begin(), m_carded.end(), same))
        return;
    m_carded = shown;

    m_cards->Freeze();
    m_cards->DestroyChildren();
    wxSizer* sizer = m_cards->GetSizer();
    sizer->Clear(false);
    for (const CloudPrinter& p : shown) {
        CloudPrinterProviderPtr provider = CloudPrinterProviders::get(p.provider_id);
        const wxString vendor  = provider ? provider->display_name() : wxString();
        const wxColour accent  = provider ? provider->brand_colour() : wxColour();
        const std::string pid  = p.provider_id;
        const std::string id   = p.id;
        wxWeakRef<CloudProviderPage> self(this);
        // Opened after the click is handled: opening may rebuild the cards.
        auto* card = new PrinterCard(m_cards, p, m_provider ? wxString() : vendor, vendor, accent, [self, pid, id]() {
            wxGetApp().CallAfter([self, pid, id]() {
                if (self)
                    self->open_printer(pid, id);
            });
        });
        sizer->Add(card, 0, wxRIGHT | wxBOTTOM, FromDIP(16));
    }
    m_cards->FitInside();
    m_cards->Layout();
    m_cards->Thaw();
}

void CloudProviderPage::apply_view()
{
    m_card_view = card_view_setting();
    m_view_btn->SetLabel(m_card_view ? _L("List view") : _L("Card view"));
    m_list->Show(!m_card_view);
    m_cards->Show(m_card_view);
    m_carded.clear();
    m_cards->DestroyChildren();
    m_cards->GetSizer()->Clear(false);
    update_header();
    rebuild_list();
}

void CloudProviderPage::open_printer(const std::string& provider_id, const std::string& printer_id)
{
    CloudPrinterProviderPtr provider = CloudPrinterProviders::get(provider_id);
    if (!provider)
        return;
    DeviceHubPanel* hub = nullptr;
    for (wxWindow* w = GetParent(); w != nullptr && hub == nullptr; w = w->GetParent())
        hub = dynamic_cast<DeviceHubPanel*>(w);
    if (hub != nullptr)
        hub->set_opening_provider(provider_id);
    provider->open_printer(this, printer_id);
    // Queued after anything open_printer() queued, so a deferred opening still sees the provider.
    if (hub != nullptr) {
        wxWeakRef<DeviceHubPanel> weak(hub);
        wxGetApp().CallAfter([weak]() {
            if (weak)
                weak->set_opening_provider({});
        });
    }
}

void CloudProviderPage::on_activated(wxDataViewEvent& evt)
{
    const int row = m_list->ItemToRow(evt.GetItem());
    if (row == wxNOT_FOUND || row >= int(m_rows.size()))
        return;
    open_printer(m_rows[row].first, m_rows[row].second);
}

void CloudProviderPage::set_accent(const wxColour& colour)
{
    m_accent = colour;
    style_primary(m_install_btn, ButtonType::Window, m_accent);
    update_header();
}

void CloudProviderPage::msw_rescale()
{
    m_install_btn->Rescale();
    m_login_btn->Rescale();
    m_refresh_btn->Rescale();
    m_view_btn->Rescale();
    // Cards are built at the current DPI.
    if (m_card_view)
        apply_view();
    Layout();
}

// ---------------------------------------------------------------------------------------------
// DeviceHubPanel
// ---------------------------------------------------------------------------------------------

DeviceHubPanel::DeviceHubPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    CloudPrinterProviders::register_builtin();

    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    m_rail = new wxPanel(this, wxID_ANY);
    m_rail->SetBackgroundColour(wxColour("#F4F4F4"));
    m_rail_sizer = new wxBoxSizer(wxVERTICAL);
    m_rail->SetSizer(m_rail_sizer);
    m_rail->SetMinSize(wxSize(FromDIP(150), -1));
    sizer->Add(m_rail, 0, wxEXPAND);

    m_book = new wxSimplebook(this, wxID_ANY);
    sizer->Add(m_book, 1, wxEXPAND);

    SetSizer(sizer);

    m_listener = CloudPrinterProviders::listen([this](const std::string& provider_id) { on_providers_changed(provider_id); });
    rebuild_entries();
}

DeviceHubPanel::~DeviceHubPanel() { CloudPrinterProviders::unlisten(m_listener); }

DeviceHubPanel::Entry* DeviceHubPanel::find(const std::string& key)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&key](const Entry& e) { return e.key == key; });
    return it == m_entries.end() ? nullptr : &*it;
}

void DeviceHubPanel::add_entry(const std::string& key, const wxString& label, wxWindow* page, bool owned, bool on_rail)
{
    Entry e;
    e.key   = key;
    e.label = label;
    e.page  = page;
    e.owned = owned;
    if (on_rail) {
        e.button = new Button(m_rail, label);
        e.button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        e.button->SetMinSize(wxSize(FromDIP(130), -1));
        e.button->SetToolTip(label);
        e.button->Bind(wxEVT_BUTTON, [this, key](wxCommandEvent&) { select(key); });
    }
    if (m_book->FindPage(page) == wxNOT_FOUND)
        m_book->AddPage(page, label);
    m_entries.push_back(std::move(e));
}

void DeviceHubPanel::remove_entry(const std::string& key)
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [&key](const Entry& e) { return e.key == key; });
    if (it == m_entries.end())
        return;
    const int idx = m_book->FindPage(it->page);
    if (idx != wxNOT_FOUND) {
        if (it->owned)
            m_book->DeletePage(idx);
        else {
            // MainFrame's pages outlive their time on the rail.
            m_book->RemovePage(idx);
            it->page->Hide();
        }
    }
    if (it->button)
        it->button->Destroy();
    m_entries.erase(it);
    if (m_selected == key)
        m_selected.clear();
}

void DeviceHubPanel::rebuild_entries()
{
    const std::string previous = m_selected;

    // Tear down in reverse, then lay the rail out again in its fixed order.
    while (!m_entries.empty())
        remove_entry(m_entries.back().key);
    m_rail_sizer->Clear(false);

    const std::string previous_rail = m_rail_selected;
    add_entry(PAGE_ALL, _L("All printers"), new CloudProviderPage(m_book, nullptr), true);
    // Off the rail: they open from a printer. The Printer page monitors whichever printer was
    // opened, whatever the selected printer preset (GUI_App::monitor_printer()).
    if (m_native_page != nullptr)
        add_entry(PAGE_PRINTER, _L("Printer"), m_native_page, false, false);
    if (m_show_web && m_web_page != nullptr)
        add_entry(PAGE_WEB, _L("Web UI"), m_web_page, false, false);
    for (const CloudPrinterProviderPtr& provider : CloudPrinterProviders::all()) {
        wxWindow* page = provider->create_page(m_book);
        if (page == nullptr)
            page = new CloudProviderPage(m_book, provider);
        add_entry(provider->id(), provider->display_name(), page, true);
    }

    m_rail_sizer->AddSpacer(FromDIP(12));
    for (const Entry& e : m_entries) {
        if (e.button == nullptr)
            continue;
        m_rail_sizer->Add(e.button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        // A gap between All printers and the services.
        if (e.key == PAGE_ALL)
            m_rail_sizer->AddSpacer(FromDIP(12));
    }
    m_rail->Layout();

    // A printer page keeps the rail button it was opened from.
    if (find(previous_rail) != nullptr)
        m_rail_selected = previous_rail;
    if (previous.empty() || !select(previous))
        select_default();
    Layout();
}

void DeviceHubPanel::set_printer_pages(wxWindow* native_page, wxWindow* web_page)
{
    m_native_page = native_page;
    m_web_page    = web_page;
    if (m_native_page)
        m_native_page->Hide();
    if (m_web_page)
        m_web_page->Hide();
}

void DeviceHubPanel::show_printer_pages(bool native, bool web)
{
    // A printer page shows only when the user opens a printer, so a preset change leaves the page
    // in view alone unless it went away.
    if (native == m_show_native && web == m_show_web && !m_entries.empty())
        return;
    m_show_native = native;
    m_show_web    = web;
    rebuild_entries();
}

bool DeviceHubPanel::select(const std::string& key)
{
    Entry* e = find(key);
    if (e == nullptr)
        return false;
    const int idx = m_book->FindPage(e->page);
    if (idx == wxNOT_FOUND)
        return false;
    m_selected = key;
    if (e->button != nullptr)
        m_rail_selected = key;
    else {
        // A printer page: the brand of the service it was opened from, else (opened from Prepare
        // or a print dialog) that of the selected printer preset, whose printer it then shows.
        CloudPrinterProviderPtr provider = CloudPrinterProviders::get(m_opening_provider);
        m_printer_accent                 = provider ? provider->brand_colour() : wxColour();
        if (!m_printer_accent.IsOk())
            if (PresetBundle* bundle = wxGetApp().preset_bundle)
                m_printer_accent = Accent::brand_for_vendor(bundle->get_current_vendor_name());
    }
    if (m_book->GetSelection() != idx)
        m_book->ChangeSelection(idx);
    // ChangeSelection() hides the old page and shows the new one without events; make sure the
    // page sees Show() even when it already was the book's selection.
    e->page->Show(true);
    update_buttons();
    return true;
}

std::string DeviceHubPanel::selected() const { return m_selected; }

wxWindow* DeviceHubPanel::selected_page() const
{
    auto it = std::find_if(m_entries.begin(), m_entries.end(), [this](const Entry& e) { return e.key == m_selected; });
    return it == m_entries.end() ? nullptr : it->page;
}

void DeviceHubPanel::select_default()
{
    if (const PresetBundle* bundle = wxGetApp().preset_bundle) {
        if (CloudPrinterProviderPtr provider = CloudPrinterProviders::for_printer_preset(bundle->printers.get_edited_preset().config))
            if (select(provider->id()))
                return;
    }
    select(PAGE_ALL);
}

wxColour DeviceHubPanel::current_accent() const
{
    if (m_selected == PAGE_PRINTER || m_selected == PAGE_WEB)
        return m_printer_accent;
    if (m_selected == PAGE_ALL || m_selected.empty())
        return {};
    CloudPrinterProviderPtr provider = CloudPrinterProviders::get(m_selected);
    return provider ? provider->brand_colour() : wxColour();
}

void DeviceHubPanel::set_opening_provider(const std::string& provider_id) { m_opening_provider = provider_id; }

void DeviceHubPanel::apply_app_accent() { wxGetApp().set_device_accent(current_accent()); }

void DeviceHubPanel::update_buttons()
{
    // The accent follows the printer brand in view. A printer page lights the button it was
    // opened from, in that printer's brand.
    const std::string lit    = m_rail_selected.empty() ? std::string(PAGE_ALL) : m_rail_selected;
    const wxColour    accent = current_accent();
    for (Entry& e : m_entries) {
        if (e.button == nullptr)
            continue;
        if (e.key == lit)
            style_primary(e.button, ButtonType::Choice, accent);
        else
            e.button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        e.button->Refresh();
    }
    if (Entry* e = find(lit))
        if (auto* page = dynamic_cast<CloudProviderPage*>(e->page); page != nullptr && !page->provider())
            page->set_accent(accent);
    if (IsShownOnScreen())
        apply_app_accent();
}

void DeviceHubPanel::on_providers_changed(const std::string& provider_id)
{
    if (provider_id.empty()) {
        // A provider was added, replaced or removed.
        rebuild_entries();
        return;
    }
    // Refresh the page showing that provider, and All printers.
    if (wxWindow* page = selected_page())
        if (auto* cloud_page = dynamic_cast<CloudProviderPage*>(page))
            if (!cloud_page->provider() || cloud_page->provider()->id() == provider_id)
                cloud_page->refresh();
}

bool DeviceHubPanel::Show(bool show)
{
    const bool changed = wxPanel::Show(show);
    if (show)
        if (wxWindow* page = selected_page())
            page->Show(true);
    return changed;
}

void DeviceHubPanel::msw_rescale()
{
    for (Entry& e : m_entries) {
        if (e.button) {
            e.button->SetMinSize(wxSize(FromDIP(130), -1));
            e.button->Rescale();
        }
        if (e.owned)
            if (auto* cloud_page = dynamic_cast<CloudProviderPage*>(e.page))
                cloud_page->msw_rescale();
    }
    m_rail->SetMinSize(wxSize(FromDIP(150), -1));
    Layout();
}

void DeviceHubPanel::on_sys_color_changed()
{
    m_rail->SetBackgroundColour(wxColour("#F4F4F4"));
    update_buttons();
    Refresh();
}

}} // namespace Slic3r::GUI
