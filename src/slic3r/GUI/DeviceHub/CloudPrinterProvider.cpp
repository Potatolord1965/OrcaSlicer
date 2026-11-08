#include "CloudPrinterProvider.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

#include <wx/app.h>

namespace Slic3r { namespace GUI {

namespace {

std::mutex                           s_mutex;
std::vector<CloudPrinterProviderPtr> s_providers;
std::map<int, CloudPrinterProviders::Listener> s_listeners;
int                                  s_next_listener{1};

} // namespace

void CloudPrinterProviders::add(CloudPrinterProviderPtr provider)
{
    if (!provider)
        return;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        const std::string id = provider->id();
        s_providers.erase(std::remove_if(s_providers.begin(), s_providers.end(),
                                         [&id](const CloudPrinterProviderPtr& p) { return p->id() == id; }),
                          s_providers.end());
        s_providers.push_back(std::move(provider));
        std::stable_sort(s_providers.begin(), s_providers.end(),
                         [](const CloudPrinterProviderPtr& a, const CloudPrinterProviderPtr& b) { return a->order() < b->order(); });
    }
    notify_changed({});
}

void CloudPrinterProviders::remove(const std::string& id)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_providers.erase(std::remove_if(s_providers.begin(), s_providers.end(),
                                         [&id](const CloudPrinterProviderPtr& p) { return p->id() == id; }),
                          s_providers.end());
    }
    notify_changed({});
}

std::vector<CloudPrinterProviderPtr> CloudPrinterProviders::all()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_providers;
}

CloudPrinterProviderPtr CloudPrinterProviders::get(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    for (const CloudPrinterProviderPtr& p : s_providers)
        if (p->id() == id)
            return p;
    return nullptr;
}

CloudPrinterProviderPtr CloudPrinterProviders::for_printer_preset(const DynamicPrintConfig& printer_config)
{
    for (const CloudPrinterProviderPtr& p : all())
        if (p->handles_printer_preset(printer_config))
            return p;
    return nullptr;
}

int CloudPrinterProviders::listen(Listener listener)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    const int handle = s_next_listener++;
    s_listeners.emplace(handle, std::move(listener));
    return handle;
}

void CloudPrinterProviders::unlisten(int handle)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_listeners.erase(handle);
}

void CloudPrinterProviders::notify_changed(const std::string& provider_id)
{
    if (wxTheApp == nullptr)
        return;
    // Listeners are looked up when the call runs, so one removed in between is not called.
    wxTheApp->CallAfter([provider_id] {
        std::vector<Listener> listeners;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (const auto& [handle, listener] : s_listeners)
                listeners.push_back(listener);
        }
        for (const Listener& listener : listeners)
            listener(provider_id);
    });
}

}} // namespace Slic3r::GUI
