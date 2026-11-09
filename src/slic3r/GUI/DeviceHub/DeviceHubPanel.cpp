#include "DeviceHubPanel.hpp"

#include <algorithm>
#include <utility>

#include <wx/dataview.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/weakref.h>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace Slic3r { namespace GUI {

namespace {

constexpr int REFRESH_INTERVAL_MS = 5000;

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

    m_empty = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTRE_HORIZONTAL);
    m_empty->SetFont(Label::Body_14);
    sizer->Add(m_empty, 0, wxEXPAND | wxALL, FromDIP(16));

    SetSizer(sizer);
    // Polls only while shown; providers that push changes also call notify_changed().
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
        if (IsShownOnScreen())
            refresh();
    });
    update_header();
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
        m_account->SetLabel(_L("Every printer from every connected service. Double-click a printer to open it."));
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
    Layout();
}

void CloudProviderPage::on_activated(wxDataViewEvent& evt)
{
    const int row = m_list->ItemToRow(evt.GetItem());
    if (row == wxNOT_FOUND || row >= int(m_rows.size()))
        return;
    const auto& [provider_id, printer_id] = m_rows[row];
    if (CloudPrinterProviderPtr provider = CloudPrinterProviders::get(provider_id))
        provider->open_printer(this, printer_id);
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

void DeviceHubPanel::add_entry(const std::string& key, const wxString& label, wxWindow* page, bool owned)
{
    Entry e;
    e.key   = key;
    e.label = label;
    e.page  = page;
    e.owned = owned;
    e.button = new Button(m_rail, label);
    e.button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    e.button->SetMinSize(wxSize(FromDIP(130), -1));
    e.button->SetToolTip(label);
    e.button->Bind(wxEVT_BUTTON, [this, key](wxCommandEvent&) {
        if (select(key) && m_on_page_selected)
            m_on_page_selected(key);
    });
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

    add_entry(PAGE_ALL, _L("All printers"), new CloudProviderPage(m_book, nullptr), true);
    // Always offered: the Printer page monitors whichever printer was opened from a service list,
    // whatever the selected printer preset (GUI_App::monitor_printer()).
    if (m_native_page != nullptr)
        add_entry(PAGE_PRINTER, _L("Printer"), m_native_page, false);
    if (m_show_web && m_web_page != nullptr)
        add_entry(PAGE_WEB, _L("Web UI"), m_web_page, false);
    for (const CloudPrinterProviderPtr& provider : CloudPrinterProviders::all()) {
        wxWindow* page = provider->create_page(m_book);
        if (page == nullptr)
            page = new CloudProviderPage(m_book, provider);
        add_entry(provider->id(), provider->display_name(), page, true);
    }

    m_rail_sizer->AddSpacer(FromDIP(12));
    for (size_t i = 0; i < m_entries.size(); ++i) {
        m_rail_sizer->Add(m_entries[i].button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        // A gap between the selected printer's own pages and the cloud services.
        if (m_entries[i].key == PAGE_ALL || ((m_entries[i].key == PAGE_PRINTER || m_entries[i].key == PAGE_WEB) &&
                                             i + 1 < m_entries.size() && m_entries[i + 1].key != PAGE_WEB))
            m_rail_sizer->AddSpacer(FromDIP(12));
    }
    m_rail->Layout();

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
    if (native == m_show_native && web == m_show_web && !m_entries.empty()) {
        // Same pages, but the printer may have changed: follow it unless the user is on a
        // service page of their own choosing.
        if (m_selected == PAGE_ALL)
            select_default();
        return;
    }
    m_show_native = native;
    m_show_web    = web;
    m_selected.clear(); // the printer changed: open its page
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
    if ((m_show_native && select(PAGE_PRINTER)) || select(PAGE_WEB))
        return;
    if (const PresetBundle* bundle = wxGetApp().preset_bundle) {
        if (CloudPrinterProviderPtr provider = CloudPrinterProviders::for_printer_preset(bundle->printers.get_edited_preset().config))
            if (select(provider->id()))
                return;
    }
    select(PAGE_ALL);
}

wxColour DeviceHubPanel::accent_for(const std::string& key) const
{
    if (key != PAGE_ALL && key != PAGE_PRINTER && key != PAGE_WEB) {
        CloudPrinterProviderPtr provider = CloudPrinterProviders::get(key);
        return provider ? provider->brand_colour() : wxColour();
    }
    if (const PresetBundle* bundle = wxGetApp().preset_bundle)
        if (CloudPrinterProviderPtr provider = CloudPrinterProviders::for_printer_preset(bundle->printers.get_edited_preset().config))
            return provider->brand_colour();
    return {};
}

void DeviceHubPanel::update_buttons()
{
    // The accent follows the printer brand in view: the selected service, or the selected
    // printer's own service on the printer pages and All printers.
    const wxColour accent = accent_for(m_selected);
    for (Entry& e : m_entries) {
        if (e.key == m_selected)
            style_primary(e.button, ButtonType::Choice, accent);
        else
            e.button->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        e.button->Refresh();
    }
    if (Entry* e = find(m_selected))
        if (auto* page = dynamic_cast<CloudProviderPage*>(e->page); page != nullptr && !page->provider())
            page->set_accent(accent);
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
        e.button->SetMinSize(wxSize(FromDIP(130), -1));
        e.button->Rescale();
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
