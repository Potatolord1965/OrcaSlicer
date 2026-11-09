#pragma once

#include <string>

#include <wx/panel.h>
#include <wx/string.h>
#include <wx/weakref.h>

class wxSimplebook;
class wxStaticText;
class wxWindow;
class Button;

namespace Slic3r { namespace GUI {

// The Elegoo entry of the Device tab: the account's printer list, and in its place the page of
// one printer when it is opened (ElegooSlicer's own cloud printer page when ElegooSlicer is
// installed, else a built-in status view). Only one printer page at a time: the cloud allows one
// messaging session per account.
class ElegooDevicePage : public wxPanel
{
public:
    explicit ElegooDevicePage(wxWindow* parent);

    // The provider's printer list, built with book() as parent.
    wxWindow* book() const;
    void      set_list_page(wxWindow* list_page);

    void show_printer(const std::string& serial, const wxString& name);
    void show_list();

private:
    wxSimplebook* m_book{nullptr};
    wxWindow*     m_list{nullptr};
    wxPanel*      m_printer_page{nullptr};
    wxStaticText* m_title{nullptr};
    Button*       m_back{nullptr};
    wxWindow*     m_view{nullptr};
    std::string   m_serial;
};

// Opens an Elegoo cloud printer (no local address) on the Device tab's Elegoo page. from is the
// window the request came from (a Device tab page); without a Device tab around it, the printer
// opens in a window of its own.
void show_elegoo_cloud_monitor(wxWindow* from, const std::string& serial, const wxString& name);

}} // namespace Slic3r::GUI
