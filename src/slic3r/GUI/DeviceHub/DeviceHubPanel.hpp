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
class wxScrolledWindow;
class wxSimplebook;
class wxStaticText;
class Button;

namespace Slic3r { namespace GUI {

// Account header and printers of one cloud service, or of all of them when built without a
// provider (the "All printers" page). The printers show as a list or as cards; the choice is the
// app setting "device_hub_view" ("list" or "cards") and holds for every page.
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
    void rebuild_cards();
    void apply_view();
    void open_printer(const std::string& provider_id, const std::string& printer_id);
    void on_activated(wxDataViewEvent& evt);

    CloudPrinterProviderPtr m_provider; // null: all providers

    wxStaticText*       m_title{nullptr};
    wxStaticText*       m_account{nullptr};
    wxStaticText*       m_notice{nullptr};
    Button*             m_login_btn{nullptr};
    Button*             m_install_btn{nullptr};
    Button*             m_refresh_btn{nullptr};
    Button*             m_view_btn{nullptr};
    wxDataViewListCtrl* m_list{nullptr};
    wxScrolledWindow*   m_cards{nullptr};
    bool                m_card_view{false};
    wxStaticText*       m_empty{nullptr};
    wxTimer             m_timer;
    wxColour            m_accent;

    std::map<std::string, std::vector<CloudPrinter>> m_printers; // by provider id
    std::map<std::string, wxString>                  m_errors;   // by provider id
    std::vector<std::pair<std::string, std::string>> m_rows;     // provider id, printer id per list row
    std::vector<CloudPrinter>                        m_carded;   // printers the cards show, to rebuild only on change
};

// The Device tab: a rail of service buttons on the left, the selected service's page on the right.
//
// Besides one page per CloudPrinterProvider it hosts the two pages the Device tab always had:
// the native page of a printer's connection (MonitorPanel) and the printer's web UI
// (PrinterWebView). MainFrame creates those with book() as parent and says whether the selected
// printer has a web UI through show_printer_pages(). They have no rail button: a printer's page
// opens only from that printer (its service's open_printer(), or MainFrame's jump_to_monitor() /
// open_printer_web_ui()), and the rail button of the page it was opened from stays lit as the
// way back.
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
    // Says whether the selected printer preset uses the Printer page (native) and has a Web UI.
    // The Printer page is always there, to monitor any service's printer; the Web UI page comes
    // and goes. Falls back to select_default() when the page in view went away.
    void show_printer_pages(bool native, bool web);

    bool        select(const std::string& key);
    std::string selected() const;
    wxWindow*   selected_page() const;
    // The selected printer preset's provider, else All printers.
    void select_default();

    // Re-shows the selected page, so a lazily built page builds when the tab opens.
    bool Show(bool show = true) override;

    // A printer of this provider is being opened from a service page: the printer page it opens
    // takes that provider's brand accent. Cleared once the opening is handled.
    void set_opening_provider(const std::string& provider_id);
    // Makes the app accent the brand of the printer in view; MainFrame calls it as the tab shows.
    void apply_app_accent();

    void msw_rescale();
    void on_sys_color_changed();

private:
    struct Entry
    {
        std::string key;
        wxString    label;
        wxWindow*   page{nullptr};
        Button*     button{nullptr}; // null for the printer pages, which have no rail button
        bool        owned{false}; // created here (provider pages), as opposed to MainFrame's pages
    };

    void      rebuild_entries();
    Entry*    find(const std::string& key);
    void      add_entry(const std::string& key, const wxString& label, wxWindow* page, bool owned, bool on_rail = true);
    void      remove_entry(const std::string& key);
    void      update_buttons();
    // Brand colour of what is in view: a service page's provider, or on a printer page the brand
    // of the printer opened. Invalid (neutral) on All printers, which shows every brand.
    wxColour  current_accent() const;
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
    std::string        m_rail_selected; // last page with a rail button, lit while a printer page shows
    std::string        m_opening_provider; // provider whose printer is being opened
    wxColour           m_printer_accent;   // brand of the printer the printer pages show
    int                m_listener{0};
};

}} // namespace Slic3r::GUI
