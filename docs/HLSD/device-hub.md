# Device hub (multi-service Device tab)

The Device tab shows every printer the user can reach, across several printer cloud services, each
with its own account. A rail of buttons on the left switches between pages; the page on the right
shows the selected one.

```
+-------------+--------------------------------------------+
| All printers|                                            |
|             |                                            |
| Printer     |   page of the selected rail entry          |
| Web UI      |                                            |
|             |                                            |
| Bambu Lab   |                                            |
| Elegoo      |                                            |
| Creality    |                                            |
| Markforged  |                                            |
| USB & LAN   |                                            |
+-------------+--------------------------------------------+
```

Code: `src/slic3r/GUI/DeviceHub/`.

## Rail entries

- **All printers**: one list of the printers of every provider, with a Service column.
- **Printer**: the native device page (`MonitorPanel`), driven by the live printer agent. Always on
  the rail, so any service's printer can be monitored whatever printer preset is selected.
- **Web UI**: the printer's own web interface (`PrinterWebView`). Shown when the printer has one.
- **One entry per cloud provider**, in `order()`.

`MainFrame` creates the Printer and Web UI pages as `LazyPage`s whose parent is the hub's book, and
`MainFrame::show_device()` tells the hub whether the selected preset uses the Printer page and has
a Web UI (`DeviceHubPanel::show_printer_pages`). When the printer changes, the hub opens its page
(Printer if the preset uses it, else Web UI, else the provider whose `handles_printer_preset()`
accepts the preset, else All printers).

The Multi-device and Calibration tabs still follow the selected preset, as before.

## Monitoring any printer

The Printer page shows the machines of the live printer agent, which normally is the selected
preset's. Opening a printer from a service list calls `GUI_App::monitor_printer(agent_id, dev_id)`:
it makes that printer's agent live without touching the preset, starts its discovery (and for
Bambu fetches the account's machine list), opens the Printer page and selects the printer once the
agent knows it. The swap is remembered as an override, so `get_printer_cloud_provider()` answers
for the live agent and a late plug-in load does not swap it back.

Printing goes through the preset's agent: the Plater's print and send actions call
`restore_preset_printer_agent()` first, and any printer preset change ends the override too.

## Provider interface

`CloudPrinterProvider` (`CloudPrinterProvider.hpp`) is what a service implements to get a rail
button:

| Method | Purpose |
|---|---|
| `id()`, `display_name()`, `order()` | Rail key, label, position. |
| `brand_colour()` | Accent of the selected rail button and the page's primary button. On the Printer, Web UI and All printers pages the accent is that of the selected printer preset's provider, so it follows the printer's brand. |
| `is_available()`, `unavailable_reason()`, `install_or_enable()` | The service can't be used at all (plug-in missing); the page says why and offers an Install button. |
| `is_logged_in()`, `account_name()`, `login(parent)`, `logout()` | The service's own account, independent of the others. |
| `has_account()` | False for an entry with no account: the page shows `account_name()` as its subtitle and the primary button runs `login()` as "Add printer". |
| `refresh_printers(done)` | The printers to list. `done` may run on any thread. |
| `create_page(parent)` | Optional page of the provider's own instead of the generic one. |
| `open_printer(parent, id)` | Double-click in a list. |
| `handles_printer_preset(config)`, `can_print()`, `show_print_dialog(parent, request)` | Printing to the service from the Plater with its own send dialog. |

`CloudPrinterProviders` is the registry. `register_builtin()`
(`CloudPrinterProviderList.cpp`) adds the built-in providers, one line each.
A provider calls `CloudPrinterProviders::notify_changed(id)` from any thread when its login state or
printers change; the hub refreshes the page that shows it. The generic page also polls every 5 s
while it is on screen, and does nothing while hidden, so a service's discovery starts only when the
user looks at it.

## Built-in providers

- `BambuCloudProvider` (`bbl`): the Bambu network plug-in's account (`GUI_App::request_login` /
  `request_user_logout` with `BBL_CLOUD_PROVIDER`) and the `DeviceManager`'s cloud and LAN machines.
  Opening a printer monitors it on the Printer page. Unavailable until the plug-in is installed.
  The device manager keeps an account machine list only for the live agent's account, so the
  provider also fetches the account's list itself (`get_user_print_info` on the Bambu cloud agent,
  at most every 30 s while the page is watched) and merges it with the device manager's machines.
  A printer is online when it is reporting over MQTT, online in the account list, or announced on
  the LAN in the last 60 s; LAN printers remembered from an earlier session count as not seen until
  they announce themselves. Its status is the live report, else the account's job state, else
  "Online"; never "Idle" without a report saying so.
  Bambu Cloud is built in: `AppConfig::get_cloud_providers()` always includes `bbl`, so its account
  shows on the Home page next to Orca's and Preferences has no switch for it.
- `PrinterAgentCloudProvider(provider_id, name, order, agent_id, cloud_provider)`: a service whose
  printers come from a printer agent, built in or a Python plug-in. It builds a private agent
  instance straight from the registered factory (never the cached instance the app drives, whose
  discovery callback it would steal), sets `set_on_ssdp_msg_fn`, and calls
  `start_discovery(true, false)`. It never connects. Each announcement is the agent's ssdp JSON:
  `dev_id`, `dev_name`, `dev_type`, plus the optional `online` (bool), `status` (short text) and
  `progress` (0-100). Opening a printer monitors it with that agent on the Printer page. The account goes through `GUI_App` when an `ICloudServiceAgent` runs for
  `cloud_provider`; otherwise the provider counts as signed in once a device is announced.
  Used for Elegoo (`elegoo_cc2`, account `elegoo`) and Creality (`creality_cloud`, account
  `creality`).
- `MarkforgedCloudProvider` (`markforged`, agent `markforged_eiger`): Eiger uses API key pairs, so
  Log in asks for the access and secret key and merges them into the agent plug-in's settings file
  `<data_dir>/orca_plugins/markforged_eiger_data/settings.json`.

A provider whose agent is not registered (plug-in not installed or turned off) shows as unavailable
rather than disappearing, so the rail stays the same from one start to the next.
- `LocalPrintersProvider` (`local`, "USB & LAN", last on the rail): printers that belong to none of
  the services. It lists visible printer presets with a `print_host` (OctoPrint, Moonraker,
  PrusaLink, Duet, ...) that no other provider's `handles_printer_preset()` claims, with Online or
  Offline from a TCP connect to the host (on a worker thread, one probe run at a time), and USB
  serial devices (`scan_serial_ports_extended()`, ports with a USB VID/PID). There is no account:
  "Add printer" opens the Connection dialog (`PhysicalPrinterDialog`) for the selected preset.
  Opening a network printer selects its preset and opens its Web UI page; the app can't print over
  USB, so opening a USB device explains the alternatives.
