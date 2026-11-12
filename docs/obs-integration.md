# OBS integration (issue #9)

The scanner connects to OBS Studio on the same network for two things. They are independent, and
either one can be on or off:

1. **Results to OBS.** Every confirmed scan is sent to the SWU Scan OBS plugin, which draws it in two
   sources the streamer places apart (Pontus, 2026-10-30): **SWU Scan Card** (art, name, subtitle
   and, if switched on, the card id) and **SWU Scan Prices** (the price lines). Each carries a fixed "Identified by SWU Scan"
   badge with the app logo. The streamer customises each source in its OBS properties (see *The
   sources*). The plugin, not the phone, owns the look, and no property touches the badge: a viewer can
   see which app produced the data. Anyone can still crop or cover it in OBS, because OBS composites
   everything. The plugin only makes doing so deliberate.
2. **OBS as the camera.** Instead of the phone's camera, the scanner analyses frames of an OBS
   source, for example a streamer's overhead table cam. It pulls them as screenshots, so OBS needs
   nothing beyond its built-in WebSocket server.

Both run over **obs-websocket v5**, which ships with OBS 28 and later. The phone is always the
client. OBS never connects to the phone, so nothing has to reach a phone whose IP changes or whose
app is suspended.

OBS is free for everyone, the card source and OBS as the camera included (Pontus, 2026-10-30; it was
part of Prices until the prices got their own source). Only the price lines need the Prices product:
without it `prices` goes out empty and the SWU Scan Prices source stays hidden.

The same protocol applies to the iOS app (this repo), the Android app (`SWUScan_android`, a copy of
this file in `docs/`) and the plugin (`SWUScan_obs`). The shared test vectors are in
`obs-protocol/vectors-v1.json`. This file is canonical: change it here first, then copy.

## Connecting

In OBS, open *Tools → WebSocket Server Settings* and turn the server on. *Show Connect Info* shows
the address, the port (default 4455), the password, and a QR code containing

```
obsws://<host>:<port>/<password>
```

The password segment is left out when authentication is off. The app either scans that QR code or
takes the three fields typed in. Parsing (`OBSConnectInfo`) works as follows:

- The scheme is case-insensitive, and surrounding whitespace is trimmed.
- An empty host, a port outside 1–65535 and any other scheme are rejected.
- A missing port means 4455.
- The password is the percent-decoded remainder of the path. An empty remainder means no password.

The vectors in `connectInfo` pin this behaviour.

### Handshake (obs-websocket v5, JSON text frames)

| op | direction | payload |
|----|-----------|---------|
| 0 Hello | OBS → app | `obsWebSocketVersion`, `rpcVersion`, optional `authentication {challenge, salt}` |
| 1 Identify | app → OBS | `rpcVersion: 1`, `authentication` (only when challenged), `eventSubscriptions: 0` |
| 2 Identified | OBS → app | `negotiatedRpcVersion` |
| 6 Request | app → OBS | `requestType`, `requestId`, `requestData` |
| 7 RequestResponse | OBS → app | `requestType`, `requestId`, `requestStatus {result, code, comment?}`, `responseData` |

The app subscribes to no events (`eventSubscriptions: 0`). It only makes requests.

The authentication string is

```
secret   = base64( sha256( password + salt ) )
response = base64( sha256( secret + challenge ) )
```

Strings are UTF-8, and base64 is standard with padding. The vectors in `auth` were generated with
Python's `hashlib`, independently of either app.

The server closes the socket with code **4009** on a wrong password and **4010** on an unsupported
RPC version. The app shows both as their own errors, not as a generic "disconnected". After an
unexpected drop it reconnects with backoff (1, 2, 4, 8, max 15 s). A failed password does **not**
trigger a reconnect.

Only the first attempt after a user action (connect) or activation shows "Connecting"; background
retries leave the failure on screen until one succeeds, so an OBS the phone cannot reach does not flash
"Connecting" forever (Pontus, 2026-10-30, phone on a guest Wi-Fi). A timeout and a transport failure
both read "OBS unavailable. Check that OBS is running and that your phone is on the same Wi-Fi as your
computer." A wrong password, an old OBS and a bad address keep their own texts.

## Results: the `swuscan` vendor

The plugin registers the obs-websocket vendor `swuscan`. The app reaches it with `CallVendorRequest`:

```json
{ "op": 6, "d": { "requestType": "CallVendorRequest", "requestId": "…",
  "requestData": { "vendorName": "swuscan", "requestType": "show_card", "requestData": { … } } } }
```

The vendor's reply is in `responseData.responseData`. If the plugin isn't installed, OBS fails the
request with status 600 (resource not found). The app treats a failed
`hello` of any kind as "plugin not installed" and shows the install link. It never falls back to
drawing an unbranded overlay with OBS's own sources.

Every request body carries `protocol` (an integer, currently 1). A vendor can't fail an
obs-websocket request: a request a registered vendor handles always succeeds at the obs-websocket
level, and only an unknown vendor or request type fails, with 600 and "No vendor was found by that
name." / "No request was found by that name.". So the plugin reports its own errors **inside**
the vendor's response, as `{"error": "…"}`. For example, a request whose `protocol` is newer than
it knows gets `{"error": "protocol N not supported; update the SWU Scan OBS plugin"}`. The app
treats a response with `error` as a failed request and shows the text as-is. Fields added later
must be optional, so that a v1 plugin keeps working.

### `hello`

Request: `{ "protocol": 1, "app": { "platform": "ios" | "android", "version": "2.5.0" } }`

Response: `{ "protocol": 1, "pluginVersion": "0.2.0", "cardSources": 2, "priceSources": 1 }`

`cardSources` and `priceSources` are the numbers of "SWU Scan Card" and "SWU Scan Prices" sources that
exist in OBS. `priceSources` is missing from a 0.1 plugin; read it as 0. When both are 0 the app warns
"Add a SWU Scan Card or SWU Scan Prices source in OBS". The app sends `hello` right after Identified, and again before
the first scan if the last answer is more than a minute old.

### `show_card`

Sent for a **confirmed** scan and for an opened card. Concretely:
- **Each transition into `.confirmed`** (iOS) or **each new `Converged`** (Android). That includes the
  same card again after the scanner was reset, and a printing the user picked in the printing picker.
  Tentative leaders are never sent, so the stream doesn't flicker.
- **Whenever a card's detail sheet opens, anywhere in the app** (Search, Collection, decks), and again
  when that sheet switches printing (the foil toggle's twin). The streamer can put any card on stream
  on demand (Pontus, 2026-10-30).

The live scanner's own confirmation card isn't a detail sheet; the scan is already sent when it
confirms.

```json
{
  "protocol": 1,
  "cardId": "SOR/010",
  "name": "Darth Vader",
  "subtitle": "Dark Lord of the Sith",
  "printing": "SOR · 010",
  "prices": [ { "market": "Cardmarket", "currency": "EUR", "amount": 1.5, "text": "€1.50" },
              { "market": "TCGplayer",  "currency": "USD", "amount": 1.89, "text": "$1.89" } ],
  "art": { "mime": "image/jpeg", "base64": "…", "width": 716, "height": 1000 },
  "app": { "platform": "ios", "version": "2.5.0" }
}
```

- `subtitle` is optional.
- `printing` is the display label the app already shows: the set code and number, with the variant
  when it isn't Standard.
- `prices` holds the markets the user shows in the app (Settings › Prices). It is empty when the user
  doesn't own Prices or no price is known. Each `amount` is a number in `currency`, and `text` is the
  app's own formatting, which the plugin shows verbatim.
- `art` is the front of the card as a JPEG at the art's own size, with no cap (owner, 2026-11-09: "the
  highest quality available"; SWU's newer sets are 1000 px on the long side, older ones about 419 px),
  encoded as base64 with no `data:` prefix. Landscape cards (leaders' backs, bases) stay landscape, and
  the plugin lays them out by their size. Sending the bytes means the plugin never touches the network
  and works offline. The art is the official CDN's image first; offline, the app's own art on the phone
  (its bundled reference art). When no art can be had, `art` is left out and
  the plugin shows the name panel alone.
- The art's corners are rounded by the app, on black, with a radius of **4 % of its short side**
  (`artCornerFraction`). JPEG has no transparency, so the official images' transparent corners would
  otherwise come out filled; a card's own corner is 3.0–3.6 % of its short side, so 4 % covers every card.
  The plugin clips the art it shows with the same fraction, to the rectangle the art actually fills
  (the art can be letterboxed in its box), so the black corners never show (Pontus, 2026-11-12).

Response: `{ "shown": 2 }`, the number of sources updated, of both kinds.

### `clear`

Request: `{ "protocol": 1 }`. It hides the card in every source. Sources also hide on their own after
their *Auto-hide after* property (default 0 = never).

## The sources

Both sources are drawn by pages bundled with the plugin (`data/card.html`, `data/prices.html`). The plugin
passes a source's properties to its page as custom properties in generated CSS. They are options, never
free-form CSS, so no setting can restyle or hide the badge, which also ignores the colour options. The
badge takes its own room in the layout and never sits on the art, name, printing or prices. The panel
hugs its content, so a source larger than the card shows nothing in the spare room.

| Property | SWU Scan Card | SWU Scan Prices |
| --- | --- | --- |
| Width × height | 1000 × 300 | 640 × 240 |
| Auto-hide after | 0 s | 0 s |
| Layout | Horizontal or vertical | Horizontal or vertical |
| Shown | Art, name and subtitle, card id (the `printing` label, off by default) | Card name, Cardmarket, TCGplayer |
| Colours | Background, text, border (with alpha) | Background, text, border (with alpha) |

The prices source hides itself while the card on show has no
price lines left to draw: no Prices, no known price, or only markets switched off in its properties.
The app sends no settings for any of this; `show_card` is the same for every source and no protocol
change is needed.

## OBS as the camera

The app lists candidate sources with `GetInputList` (inputs) and `GetSceneList` (scenes), and the
user picks one. Audio-only inputs are left out, by `unversionedInputKind` (else `inputKind`):
`wasapi_input_capture`, `wasapi_output_capture`, `wasapi_process_output_capture`,
`coreaudio_input_capture`, `coreaudio_output_capture`, `sck_audio_capture`, `pulse_input_capture`,
`pulse_output_capture`, `alsa_input_capture`, `jack_output_capture` and `oss_input_capture`. Everything
else stays, because media, NDI, browser and window sources and scenes can all be cameras (Pontus,
2026-11-12). While "OBS camera" is the scan input, the app runs a loop that keeps exactly one
request in flight:

```json
{ "requestType": "GetSourceScreenshot",
  "requestData": { "sourceName": "Table cam", "imageFormat": "jpg",
                   "imageWidth": 1920, "imageCompressionQuality": 85 } }
```

- `responseData.imageData` is a `data:image/jpg;base64,…` URI. The app decodes it and hands it to
  the same frame entry point the camera uses: `CameraScanController.ingestExternalFrame` on iOS,
  and `CardScanPipeline.process` through `FrameIngest` on Android. The orientation is upright.
- The loop paces itself to about 6 frames a second: the next request goes out no sooner than 160 ms
  after the previous one started. That is enough for the 0.5 s decision window, which needs 4
  analysed frames, and it keeps OBS's render thread and the Wi-Fi link light. The scanner only
  needs a few sharp frames, not video.
- Nothing about focus applies. There is no lens, so there is no "lens adjusting" skip, no sharpness
  watchdog, no zoom, and no tap-to-focus. The scanner hides those controls and shows the latest
  frame instead of the camera preview.
- An unknown source name (a renamed source) stops the loop with an error. The loop never retries
  forever on a failure.

### When OBS is unreachable

Not connected shows the phone's camera (Pontus, 2026-11-01). The saved source choice is kept, and the
connection retries in the background; the scanner returns to the OBS source by itself when OBS is back.
The decision is pure (`OBSCameraPolicy` on iOS) and takes the time of the drop, not only the status:

- The scanner uses OBS only when a source is picked, the user wants to be connected, and the link is
  usable. A first attempt in progress (launch, nothing has failed yet) counts as usable, so OBS does not
  flash the phone's camera while it connects.
- **The first failure** (no connection yet this launch) switches to the phone's camera at once.
- **A drop from connected** keeps the OBS view (the frame view with "Reconnecting") for a grace period of
  **10 seconds**, so a Wi-Fi blip does not flip to the phone's camera and back; then it falls back. The
  background retries leave the status at failed, so the app records when the connection dropped. A new
  attempt (a return to the foreground) inside that period keeps OBS; after it, it does not.
- **Non-retryable failures** (wrong password, old OBS, bad address) fall back at once, grace or not.
- The view must re-evaluate when the grace period ends, not only on the next state change.
- The switch back to OBS, like the switch away, is held while a card is being committed (a confirmation
  or printing picker up, a card selected in Multi), a Scan all run is in progress, or a sheet covers the scanner, because changing the
  source restarts the scanner and drops its evidence. It happens when the hold ends.

Settings: *Disconnect* is offered while a connection is failing too; it stops the retries and keeps the
saved address and password. *Scan with* is reachable when not connected: "Phone camera" and the saved
source (the list of sources is empty until OBS answers). *Forget this OBS* asks for confirmation first,
because it deletes the saved address and password and the QR code has to be scanned again.

**Multi scan** (docs/multi-scan.md) works on OBS frames exactly as on camera frames: they go through
the same detector and tracker, the outlines are drawn over the OBS frame view in **fit** mode (the
view letterboxes the whole frame, unlike the camera preview's fill), and tapping an outline scans that
card. Tapping elsewhere does nothing, because there is no lens to focus. *Scan all* is hidden while OBS
is the camera (Pontus, 2026-10-30): a stream shows one card at a time, picked by a tap. So the OBS
session has no still capture, which overrides the OBS line in docs/multi-scan.md M6.

**Calibration caveat.** The scan constants (`decisionWindow`, `commitConfidence`, the distance
temperature) were tuned on close-range phone frames. A card on an overhead webcam covers far fewer
pixels, and its collector line may be unreadable at 1080p. Expect OBS input to need a crop/zoom
region or its own constants after real-world testing. That is calibration work, tracked separately.

## Privacy and permissions

- iOS shows the Local Network prompt (`NSLocalNetworkUsageDescription`) on the first connect.
  There is no Bonjour, because the address comes from the QR code or is typed in.
- Android needs cleartext traffic allowed, because obs-websocket is plain `ws://`. The
  `network_security_config` explains why it cannot be limited to private addresses.
- The password is stored in the Keychain (iOS) or the Keystore-backed encrypted preferences
  (Android), never in plain settings.
- No card or collection data leaves the phone except the confirmed card above, and only to the OBS
  instance the user connected to.
