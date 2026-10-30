# SWU Scan for OBS

Shows the cards the SWU Scan app (iOS and Android) scans on your stream, in two sources you place
apart: **SWU Scan Card** (the card's art, name and printing) and **SWU Scan Prices** (its prices,
with the app's Prices purchase). Both carry an "Identified by SWU Scan" badge with the app logo, so
viewers can see where the data comes from.

The phone talks to OBS's built-in WebSocket server (OBS 28 or later). There is nothing to host
and no browser page to set up. The phone sends the card art itself, so the plugin never goes
online.

## Setup

1. Install the plugin for your platform from [Releases](../../releases) and restart OBS.
2. Add a **SWU Scan Card** source and, if you like, a **SWU Scan Prices** source to a scene. In a
   source's properties you can set its size, *Auto-hide after*, horizontal or vertical layout,
   which lines it shows, and its colours (including a transparent background).
3. In OBS, open *Tools → WebSocket Server Settings*, enable the server, then click
   *Show Connect Info*.
4. In the app, open *Settings → OBS* and scan the QR code.

The app can also use an OBS source, such as a table camera, as its camera: choose it under
*Scan with*.

## How it works

The protocol is described in [`docs/obs-integration.md`](docs/obs-integration.md). It is a copy of
the canonical file in the SWU Scan iOS repo.

- The plugin registers the obs-websocket vendor `swuscan` with the requests `hello`,
  `show_card` and `clear`.
- Each source owns a private browser source that shows `data/card.html` or `data/prices.html`.
  The plugin passes each card to those pages as a JavaScript event, using obs-browser's
  `emit_event`.
- The pages ship inside the plugin. A source's properties reach its page as options in generated
  CSS, never as free-form CSS, so the badge isn't a setting.

To check a running OBS with this plugin installed:

```bash
pip install websockets
python3 tools/obs_check.py --url 'obsws://127.0.0.1:4455/<password>' --art some-card.jpg --setup
```

## Building

This repository started from
[obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate). Its GitHub Actions build
Windows, macOS and Ubuntu packages, and a pushed tag creates a draft release. Local builds use the
template's CMake presets (`cmake --preset macos` / `windows-x64` / `ubuntu-x86_64`). See the
template's README for the details.

macOS packages are only signed and notarized when the repository has the Apple Developer ID
secrets that the template's workflow expects.

## License

GPL-2.0-or-later, like OBS. `src/obs-websocket-api.h` is from
[obs-websocket](https://github.com/obsproject/obs-websocket).
