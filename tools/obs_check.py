#!/usr/bin/env python3
"""Checks a real OBS Studio + the SWU Scan OBS plugin against docs/obs-integration.md, the way the apps
talk to it: authentication, the `swuscan` vendor (hello / show_card / clear, errors inside the reply),
and GetSourceScreenshot as the scanner's camera.

    pip install websockets
    python3 tools/obs_mock/obs_check.py --url obsws://192.168.1.20:4455/password --art card.jpg

With --setup it first adds an Image source "SWU Scan check cam" (showing --art) and a "SWU Scan Card"
source to the current scene, so a fresh OBS can be checked end to end; without it, it uses the
--camera source you name and expects a card source to exist already. With the card source visible
in OBS you see the test card appear, change and clear.

With --sides (plugin 0.3) it also checks player sides: it adds a "SWU Scan Card P1" and a "SWU Scan Card P2"
source (Player side 1 and 2) next to the Any one, then checks hello's features and sideSources, that show_card
routes by side and that a side-less card (an app from before sides) still goes everywhere, and that clear with
a side keeps the other side. With --visual it also screenshots each card source (PNG; needs Pillow) to see which
ones actually draw a card.

Exits non-zero on the first failed check. Also works against tools/obs_mock/obs_mock.py.
"""

import argparse
import asyncio
import base64
import hashlib
import json
import pathlib
import sys
import urllib.parse

try:
    import websockets
except ImportError:
    sys.exit("pip install websockets")


def auth_response(password, salt, challenge):
    secret = base64.b64encode(hashlib.sha256((password + salt).encode()).digest()).decode()
    return base64.b64encode(hashlib.sha256((secret + challenge).encode()).digest()).decode()


def parse_obsws(url):
    u = urllib.parse.urlsplit(url.strip())
    if u.scheme.lower() != "obsws" or not u.hostname:
        sys.exit(f"not an obsws:// URL: {url}")
    pw = urllib.parse.unquote(u.path[1:]) if len(u.path) > 1 else None
    return u.hostname, u.port or 4455, pw


class OBS:
    def __init__(self, ws):
        self.ws, self.n = ws, 0

    async def request(self, rtype, data=None):
        self.n += 1
        rid = f"check-{self.n}"
        msg = {"op": 6, "d": {"requestType": rtype, "requestId": rid}}
        if data is not None:
            msg["d"]["requestData"] = data
        await self.ws.send(json.dumps(msg))
        while True:
            m = json.loads(await asyncio.wait_for(self.ws.recv(), 10))
            if m.get("op") == 7 and m["d"].get("requestId") == rid:
                return m["d"]

    async def vendor(self, rtype, data):
        return await self.request("CallVendorRequest", {"vendorName": "swuscan", "requestType": rtype, "requestData": data})


failures = 0


def check(ok, what):
    global failures
    print(("PASS " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures += 1
        sys.exit(1)


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", required=True, help="obsws://host:port/password (OBS: Tools › WebSocket Server Settings › Show Connect Info)")
    ap.add_argument("--art", help="a card JPEG: the test card's art, and the --setup camera image")
    ap.add_argument("--camera", default="SWU Scan check cam", help="source to screenshot as the camera")
    ap.add_argument("--setup", action="store_true", help="add a camera image source and a SWU Scan Card source first")
    ap.add_argument("--sides", action="store_true", help="also check player sides (plugin 0.3)")
    ap.add_argument("--visual", action="store_true", help="with --sides: screenshot the card sources to see what they draw")
    args = ap.parse_args()
    host, port, password = parse_obsws(args.url)

    async with websockets.connect(f"ws://{host}:{port}", max_size=32 * 1024 * 1024) as ws:
        hello = json.loads(await ws.recv())
        check(hello.get("op") == 0, f"Hello (obs-websocket {hello['d'].get('obsWebSocketVersion')})")
        d = {"rpcVersion": 1, "eventSubscriptions": 0}
        a = hello["d"].get("authentication")
        if a:
            if password is None:
                sys.exit("OBS wants a password; put it in the URL")
            d["authentication"] = auth_response(password, a["salt"], a["challenge"])
        await ws.send(json.dumps({"op": 1, "d": d}))
        ident = json.loads(await ws.recv())
        check(ident.get("op") == 2, "Identified (authentication accepted)")
        obs = OBS(ws)

        if args.setup:
            scene = (await obs.request("GetSceneList"))["responseData"]["currentProgramSceneName"]
            if args.art:
                r = await obs.request("CreateInput", {"sceneName": scene, "inputName": args.camera, "inputKind": "image_source",
                                                      "inputSettings": {"file": str(pathlib.Path(args.art).resolve())}})
                check(r["requestStatus"]["result"] or r["requestStatus"]["code"] == 601, f"camera image source in “{scene}”")
            r = await obs.request("CreateInput", {"sceneName": scene, "inputName": "SWU Scan Card", "inputKind": "swuscan_card",
                                                  "inputSettings": {}})
            check(r["requestStatus"]["result"] or r["requestStatus"]["code"] == 601,
                  "SWU Scan Card source created (the plugin registered swuscan_card)")
            r = await obs.request("CreateInput", {"sceneName": scene, "inputName": "SWU Scan Prices", "inputKind": "swuscan_prices",
                                                  "inputSettings": {}})
            check(r["requestStatus"]["result"] or r["requestStatus"]["code"] == 601,
                  "SWU Scan Prices source created (the plugin registered swuscan_prices)")
            await asyncio.sleep(0.5)

        r = await obs.vendor("hello", {"protocol": 1, "app": {"platform": "check", "version": "0"}})
        check(r["requestStatus"]["result"], f"vendor hello answered: {r['requestStatus'].get('comment', 'ok')}")
        h = r["responseData"]["responseData"]
        check(h.get("protocol") == 1, f"plugin {h.get('pluginVersion')} speaks protocol {h.get('protocol')}")
        check(h.get("cardSources", 0) >= 1, f"{h.get('cardSources')} SWU Scan Card source(s)")
        print(f"     {h.get('priceSources', 0)} SWU Scan Prices source(s)")

        card = {"protocol": 1, "cardId": "SOR/005", "name": "Luke Skywalker", "subtitle": "Faithful Friend", "printing": "SOR · 005",
                "prices": [{"market": "Cardmarket", "currency": "EUR", "amount": 1.5, "text": "€1.50"}],
                "app": {"platform": "check", "version": "0"}}
        if args.art:
            card["art"] = {"mime": "image/jpeg", "base64": base64.b64encode(pathlib.Path(args.art).read_bytes()).decode(),
                           "width": 0, "height": 0}
        r = await obs.vendor("show_card", card)
        inner = r["responseData"]["responseData"]
        check(r["requestStatus"]["result"] and "error" not in inner and inner.get("shown", 0) >= 1,
              f"show_card shown in {inner.get('shown')} source(s)")
        r = await obs.vendor("show_card", {"protocol": 2})
        err = r["responseData"]["responseData"].get("error", "")
        check(r["requestStatus"]["result"] and "update the SWU Scan OBS plugin" in err, f"newer protocol refused in the reply: {err!r}")
        r = await obs.vendor("clear", {"protocol": 1})
        check(r["requestStatus"]["result"], "clear")
        r = await obs.request("CallVendorRequest", {"vendorName": "swuscan", "requestType": "nope", "requestData": {}})
        check(not r["requestStatus"]["result"] and r["requestStatus"]["code"] == 600,
              f"unknown vendor request: {r['requestStatus']['code']} {r['requestStatus'].get('comment')}")
        r = await obs.request("CallVendorRequest", {"vendorName": "not-installed", "requestType": "hello", "requestData": {}})
        check(not r["requestStatus"]["result"] and r["requestStatus"]["code"] == 600,
              f"missing plugin looks like: {r['requestStatus']['code']} {r['requestStatus'].get('comment')}")

        inputs = (await obs.request("GetInputList"))["responseData"]["inputs"]
        scenes = (await obs.request("GetSceneList"))["responseData"]["scenes"]
        names = [i["inputName"] for i in inputs] + [s["sceneName"] for s in scenes]
        check(args.camera in names, f"camera source listed among {names}")
        r = await obs.request("GetSourceScreenshot", {"sourceName": args.camera, "imageFormat": "jpg", "imageWidth": 1920,
                                                      "imageCompressionQuality": 85})
        uri = r["responseData"].get("imageData", "") if r["requestStatus"]["result"] else ""
        jpeg = base64.b64decode(uri.split(";base64,", 1)[-1]) if uri else b""
        check(uri.startswith("data:image/jpg;base64,") and jpeg[:2] == b"\xff\xd8",
              f"screenshot is a {len(jpeg)} B JPEG data URI ({uri[:22]}…)")
        r = await obs.request("GetSourceScreenshot", {"sourceName": "No such source", "imageFormat": "jpg"})
        check(not r["requestStatus"]["result"], f"unknown camera source: {r['requestStatus']['code']} {r['requestStatus'].get('comment')}")
        if args.sides:
            await check_sides(obs, card, args.visual)
    print("ALL CHECKS PASSED")


async def drawn(obs, source):
    """How many pixels of a source's screenshot aren't transparent (the card panel drawn), or None without Pillow."""
    try:
        from PIL import Image
        import io
    except ImportError:
        return None
    r = await obs.request("GetSourceScreenshot", {"sourceName": source, "imageFormat": "png", "imageWidth": 400})
    if not r["requestStatus"]["result"]:
        return 0
    png = base64.b64decode(r["responseData"]["imageData"].split(";base64,", 1)[-1])
    alpha = Image.open(io.BytesIO(png)).convert("RGBA").getchannel("A")
    return sum(alpha.histogram()[33:])


async def check_sides(obs, card, visual):
    """Player sides (docs/obs-integration.md): routing, clearing, hello, and old apps without sides."""
    scene = (await obs.request("GetSceneList"))["responseData"]["currentProgramSceneName"]
    for name, side in (("SWU Scan Card P1", 1), ("SWU Scan Card P2", 2)):
        r = await obs.request("CreateInput", {"sceneName": scene, "inputName": name, "inputKind": "swuscan_card",
                                              "inputSettings": {"side": side}})
        if not r["requestStatus"]["result"]:
            r = await obs.request("SetInputSettings", {"inputName": name, "inputSettings": {"side": side}})
        check(r["requestStatus"]["result"], f"{name} set to Player side {side}")
    await asyncio.sleep(2.5)  # the new pages load

    h = (await obs.vendor("hello", {"protocol": 1, "app": {"platform": "check", "version": "0"}}))["responseData"]["responseData"]
    check(h.get("features", {}).get("side") is True, f"hello features: {h.get('features')}")
    sides = h.get("sideSources", {})
    card_sides = sides.get("card", {})
    check(card_sides.get("1", 0) >= 1 and card_sides.get("2", 0) >= 1, f"hello sideSources: {sides}")
    any_cards = card_sides.get("any", 0)
    prices = sides.get("price", {})
    any_all = any_cards + prices.get("any", 0)

    async def show(side, name):
        body = dict(card, name=name)
        if side:
            body["side"] = side
        return (await obs.vendor("show_card", body))["responseData"]["responseData"].get("shown")

    n = await show(1, "Side one")
    check(n == any_all + card_sides["1"] + prices.get("1", 0), f"show_card side 1 reached {n} source(s): Any and Player 1 only")
    n = await show(2, "Side two")
    check(n == any_all + card_sides["2"] + prices.get("2", 0), f"show_card side 2 reached {n} source(s): Any and Player 2 only")
    if visual:
        await asyncio.sleep(1.5)
        p1, p2 = await drawn(obs, "SWU Scan Card P1"), await drawn(obs, "SWU Scan Card P2")
        if p1 is not None:
            check(p1 > 0 and p2 > 0, f"both sides drawn (P1 {p1} px, P2 {p2} px)")

    r = await obs.vendor("clear", {"protocol": 1, "side": 1})
    check(r["requestStatus"]["result"], "clear side 1")
    if visual:
        await asyncio.sleep(1.0)
        p1, p2 = await drawn(obs, "SWU Scan Card P1"), await drawn(obs, "SWU Scan Card P2")
        if p1 is not None:
            check(p1 == 0 and p2 > 0, f"clear side 1 keeps Player 2 (P1 {p1} px, P2 {p2} px)")

    total = sum(card_sides.values()) + sum(prices.values())
    n = await show(None, "Old app")
    check(n == total, f"a side-less show_card (an app before sides) reached all {n} source(s)")
    if visual:
        await asyncio.sleep(1.5)
        p1, p2 = await drawn(obs, "SWU Scan Card P1"), await drawn(obs, "SWU Scan Card P2")
        if p1 is not None:
            check(p1 > 0 and p2 > 0, f"the side-less card shows on both sides (P1 {p1} px, P2 {p2} px)")
    r = await obs.vendor("clear", {"protocol": 1})
    check(r["requestStatus"]["result"], "clear everything")
    if visual:
        await asyncio.sleep(1.0)
        p1, p2 = await drawn(obs, "SWU Scan Card P1"), await drawn(obs, "SWU Scan Card P2")
        if p1 is not None:
            check(p1 == 0 and p2 == 0, f"clear without a side clears both (P1 {p1} px, P2 {p2} px)")


if __name__ == "__main__":
    asyncio.run(main())
