# Home tab: model sites

The Home tab's nav column (`resources/web/homepage`) lists four model sites under **Models**:
Unlayered Universe, MakerWorld, Nexprint and Thingiverse. Picking one shows that site in a native
web view beside the nav column (`WebViewPanel::set_model_site`), the same side-by-side layout the
OrcaCloud pane uses. The homepage is then sized to the nav column only.

## One view per site, preloaded

Each site has its own `ModelSiteView` (`src/slic3r/GUI/ModelSiteView.{hpp,cpp}`), a panel holding
the site's web view and, for MakerWorld, its toolbar. Views are kept for the life of the Home tab,
so switching sites never reloads a page and each keeps its own history and sign-in.

Once the homepage has loaded, `WebViewPanel::preload_model_sites` creates the four views hidden
and loads one site per timer tick (800 ms apart), so a model tab opens on a page that is already
there and the views do not all start loading together with the homepage.

## Each site hosted like its own slicer

| Site | Hosted as | Page ↔ app |
|---|---|---|
| Unlayered Universe | Unlayered Slicer's Universe tab: `https://unlayered3d.com/universe?host=unlayered-slicer` (override: app config `model_site_unlayered_url`) | The subset of Unlayered Slicer's web-app bridge (protocol v1, `WEB_APP_BRIDGE.md` in its source) the page uses: `slicer.getInfo`, `slicer.openProjectFromUrl`, `slicer.selectPreset` (printer only) and `slicer.installPrinterPreset`. Same shim (`window.__slicerBridge`), envelope, per-document token and error codes, so the page runs unchanged. |
| MakerWorld | Bambu Studio's Online Models: `{host}{lang}/studio/webview?from=bambustudio`, with its Back / Refresh / Open in browser toolbar | Signed in to Bambu, the first load goes through `{host}api/sign-in/ticket` with a bind ticket from the Bambu network plug-in. Page commands (`makerworld_model_open`, ...) go to `GUI_App::handle_web_request`, as in Bambu Studio. |
| Nexprint | ElegooSlicer's Online Models: `https://www.nexprint.com/elegooSlicer` (`nexprint.cn` for the China region), with its user agent and `language`, `region`, `theme`, `deviceId` parameters | ElegooSlicer's page protocol (`report.*` requests, `client.*` events through `HandleStudio`) answered from the Elegoo account in `ElegooCloud`; `report.notLogged` opens the Elegoo sign-in. |
| Thingiverse | The plain site | None. |

On every site, a link to a model file or an "Open in slicer" link (`bambustudio://`, `cura://`,
`elegooslicer://`, ...) is imported into the plater through `GUI_App::start_download`.

## Unlayered bridge constraints

- The shim is injected, and requests are honoured, only while the main document is on
  `https://unlayered3d.com`. A fresh 256-bit token is minted on each load and any navigation voids it.
- Downloads are https only, from the page's host, `*.unlayered3d.com` or `*.vercel-storage.com`
  (where Universe redirects its downloads). Models are capped at 256 MiB, printer profiles at 4 MiB,
  and a `sha256` sent by the page is checked.
- A printer profile is never installed without the user's yes in a native dialog that lists the
  printer, bed, nozzle, flavor and any custom G-code line touching firmware, files, the network or
  safety limits. Print-host settings and local bed texture/model paths are stripped whatever the
  file says, a name a system preset uses is refused, and a parent that is not installed is dropped
  (shared profiles are flattened).

## Collapsible nav column

The nav column collapses to icons (`body.nav-collapsed`, toggled by the button at its bottom and
remembered in the page's local storage). The page tells the app with `homepage_nav_collapsed`, and
`WebViewPanel::set_nav_collapsed` sizes the homepage to the narrow column while a side pane is open.
