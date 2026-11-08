#pragma once

#include "CloudPrinterProvider.hpp"

#include <map>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/panel.h>
#include <wx/timer.h>

class wxBoxSizer;
class wxDataViewEvent;
class wxDataViewListCtrl;
class wxSimplebook;
class wxStaticText;
class Button;

namespace Slic3r { namespace GUI {

// Account header and printer list for one cloud service, or for all of them when built without a
// provider (the "All printers" page).
class CloudProviderPage : public wxPanel
{
public:
    CloudProviderPage(wxWindow* parent, CloudPrinterProviderPtr provider);
    ~CloudProviderPage() override;

    const CloudPrinterProviderPtr& provider() const { return m_provider; }

    // Re-reads the account state and asks the provider(s) for their printers.
    void refresh();

    bool Show(bool show = true) override;
    void msw_rescale();

    // Colours the primary button with a brand accent; invalid for the app's default.
    void set_accent(const wxColour& colour);

private:
    std::vector<CloudPrinterProviderPtr> providers() const;
    void update_header();
    void on_printers(const std::string& provider_id, std::vector<CloudPrinter> printers, const wxString& error);
    void rebuild_list();
    void on_activated(wxDataViewEvent& evt);

    CloudPrinterProviderPtr m_provider; // null: all providers

    wxStaticText*       m_title{nullptr};
    wxStaticText*       m_account{nullptr};
    wxStaticText*       m_notice{nullptr};
    Button*             m_login_btn{nullptr};
    Button*             m_install_btn{nullptr};
    Button*             m_refresh_btn{nullptr};
    wxDataViewListCtrl* m_list{nullptr};
    wxStaticText*       m_empty{nullptr};
    wxTimer             m_timer;
    wxColour            m_accent;

    std::map<std::string, std::vector<CloudPrinter>> m_printers; // by provider id
    std::map<std::string, wxString>                  m_errors;   // by provider id
    std::vector<std::pair<std::string, std::string>> m_rows;     // provider id, printer id per list row
};

// The Device tab: a rail of service buttons on the left, the selected service's page on the right.
//
// Besides one page per CloudPrinterProvider it hosts the two pages the Device tab always had:
// the native page of the selected printer's connection (MonitorPanel) and the printer's web UI
// (PrinterWebView). MainFrame creates those with book() as parent and says which of them the
// selected printer uses through show_printer_pages().
class DeviceHubPanel : public wxPanel
{
public:
    static constexpr const char* PAGE_ALL     = "all";
    static constexpr const char* PAGE_PRINTER = "printer";
    static constexpr const char* PAGE_WEB     = "web";

    explicit DeviceHubPanel(wxWindow* parent);
    ~DeviceHubPanel() override;

    // Parent for the printer pages MainFrame creates.
    wxSimplebook* book() const { return m_book; }

    void set_printer_pages(wxWindow* native_page, wxWindow* web_page);
    // Adds or removes the Printer and Web UI pages, then selects the page for the selected
    // printer when the current one went away (or when the printer changed).
    void show_printer_pages(bool native, bool web);

    bool        select(const std::string& key);
    std::string selected() const;
    wxWindow*   selected_page() const;
    // Printer page if shown, else Web UI, else the selected preset's provider, else All printers.
    void select_default();

    // Called when the user picks a page, after it is shown.
    void set_on_page_selected(std::function<void(const std::string& key)> fn) { m_on_page_selected = std::move(fn); }

    // Re-shows the selected page, so a lazily built page builds when the tab opens.
    bool Show(bool show = true) override;

    void msw_rescale();
    void on_sys_color_changed();

private:
    struct Entry
    {
        std::string key;
        wxString    label;
        wxWindow*   page{nullptr};
        Button*     button{nullptr};
        bool        owned{false}; // created here (provider pages), as opposed to MainFrame's pages
    };

    void      rebuild_entries();
    Entry*    find(const std::string& key);
    void      add_entry(const std::string& key, const wxString& label, wxWindow* page, bool owned);
    void      remove_entry(const std::string& key);
    void      update_buttons();
    // Brand colour of the selected entry: its provider's, or for the printer pages and All
    // printers that of the selected printer preset's provider. Invalid for the default accent.
    wxColour  accent_for(const std::string& key) const;
    void      on_providers_changed(const std::string& provider_id);

    wxPanel*      m_rail{nullptr};
    wxBoxSizer*   m_rail_sizer{nullptr};
    wxSimplebook* m_book{nullptr};

    wxWindow* m_native_page{nullptr};
    wxWindow* m_web_page{nullptr};
    bool      m_show_native{false};
    bool      m_show_web{false};

    std::vector<Entry> m_entries; // rail order
    std::string        m_selected;
    int                m_listener{0};
    std::function<void(const std::string&)> m_on_page_selected;
};

}} // namespace Slic3r::GUI
