# Unified Monitor (Android)

Companion app for Unified Slicer: watches every printer from your phone in one list.

| Source | How it connects |
| --- | --- |
| Bambu Lab | Bambu Cloud account (REST + Bambu's MQTT broker), or a printer in LAN mode (MQTT/TLS, access code) |
| Elegoo | Elegoo Cloud account (Elegoo's own sign-in page, report-data API), or a Centauri Carbon 2 on the LAN (MQTT, access code) |
| Creality | Creality Cloud account (device list + IoT telemetry) |
| Markforged | Eiger API keys (devices + active job) |
| Klipper, OctoPrint, PrusaLink | HTTP on the local network |

Features: live status (progress, time left, layer, temperatures, stage) refreshed every few seconds while open, notifications when a print finishes, fails or pauses (checked every 15 minutes in the background), brand-coloured accents, dark theme by default.

The protocols are the ones Unified Slicer's desktop code uses (`src/slic3r/Utils/ElegooCloud.cpp`, `CrealityCloud.cpp`, `MarkforgedEiger.cpp`, `resources/plugins/elegoo_cc2_lan.py`); Bambu Cloud uses Bambu's public account API.

## Layout

- `core/` — plain Kotlin/JVM: account model, one `PrinterSource` per service, `PrinterHub` that refreshes them in parallel. Unit tested on the JVM.
- `app/` — Android UI (Jetpack Compose, Material 3), encrypted account store (Android Keystore AES-GCM), WorkManager background check, notifications.

## Build

Needs JDK 17+ and the Android SDK (API 36).

```
./gradlew :core:test :app:assembleRelease
```

The GitHub workflow `companion_android.yml` builds the APK on every change and, on `main`, publishes it as the `companion-latest` release.

The APK is signed with `app/sideload.keystore`, which is committed so every build installs over the last one. It identifies this project's builds, not a person; anyone with the repository can sign with it.

## Notes

- Elegoo's sign-in page only issues device-API tokens to a client that presents itself as ElegooSlicer, so the sign-in web view uses ElegooSlicer's user agent (same as the desktop app).
- LAN printers are reachable only on the same network or over a VPN.
- Bambu LAN mode certificates are self-signed and are not verified; cloud connections are.
