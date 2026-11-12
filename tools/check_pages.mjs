// Checks the card and prices pages' player-side rules (data/side.js; docs/obs-integration.md) in Chromium, the
// engine obs-browser embeds. No OBS needed: each page is loaded with the --side the plugin would generate, and the
// plugin's events are dispatched by hand.
//
//     npm install playwright   (or a global one: NODE_PATH=$(npm root -g))
//     node tools/check_pages.mjs
//
// Exits non-zero on the first failed check.
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";

// require (not import) so a globally installed Playwright is found through NODE_PATH too.
const { chromium } = createRequire(import.meta.url)("playwright");
const data = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "data");
let seq = 0;
const card = (side, name) => ({ seq: ++seq, side, name, printing: "SOR · 005",
  prices: [{ market: "Cardmarket", currency: "EUR", amount: 1.5, text: "€1.50" }] });

function check(ok, what) {
  console.log((ok ? "PASS " : "FAIL ") + what);
  if (!ok) process.exit(1);
}

async function page(browser, file, side, panelId) {
  const p = await browser.newPage({ viewport: { width: 1000, height: 300 } });
  await p.goto("file://" + path.join(data, file));
  if (side !== null) await p.addStyleTag({ content: `:root{--side:${side};}` });   // what build_css adds
  return {
    send: c => p.evaluate(c => window.dispatchEvent(new CustomEvent("swuscanCard", { detail: c })), c),
    clear: side => p.evaluate(s => window.dispatchEvent(new CustomEvent("swuscanClear", { detail: s === undefined ? null : { side: s } })), side),
    shows: () => p.evaluate(id => {
      const el = document.getElementById(id);
      return el.classList.contains("on") ? (document.getElementById("name").textContent || "?") : null;
    }, panelId),
  };
}

const browser = await chromium.launch();
for (const [file, panelId] of [["card.html", "card"], ["prices.html", "prices-panel"]]) {
  const label = file.replace(".html", "");
  const any = await page(browser, file, 0, panelId);
  const p1 = await page(browser, file, 1, panelId);
  const p2 = await page(browser, file, 2, panelId);
  const old = await page(browser, file, null, panelId);   // a page without --side: Any
  const all = [any, p1, p2, old];
  const send = async c => { for (const x of all) await x.send(c); };
  const clear = async s => { for (const x of all) await x.clear(s); };

  await send(card(1, "One"));
  check(await any.shows() === "One" && await p1.shows() === "One" && await p2.shows() === null,
    `${label}: a side 1 card shows on Any and Player 1, not Player 2`);
  await send(card(2, "Two"));
  check(await any.shows() === "Two" && await p1.shows() === "One" && await p2.shows() === "Two",
    `${label}: a side 2 card shows on Any and Player 2; Player 1 keeps its card`);
  check(await old.shows() === "Two", `${label}: a page without --side behaves as Any`);

  const resend = { seq: seq - 1, side: 1, name: "One" };   // the plugin re-sends oldest first after a source appears
  await send(resend);
  check(await any.shows() === "Two", `${label}: a re-send of an older card doesn't replace a newer one`);

  await clear(1);
  check(await p1.shows() === null && await p2.shows() === "Two" && await any.shows() === "Two",
    `${label}: clear side 1 clears Player 1 and keeps Player 2 (and Any, which shows side 2)`);
  await send(card(1, "One again"));
  await clear(2);
  check(await any.shows() === "One again" && await p2.shows() === null && await p1.shows() === "One again",
    `${label}: clear side 2 leaves Any on its side 1 card`);

  await send(card(0, "Old app"));
  check((await Promise.all(all.map(x => x.shows()))).every(n => n === "Old app"),
    `${label}: a side-less card (an app before sides) shows everywhere`);
  await clear(2);
  check((await Promise.all(all.map(x => x.shows()))).every(n => n === null),
    `${label}: clearing a side also takes down the side-less card`);

  await send(card(1, "Last"));
  await send(card(2, "Last two"));
  await clear(undefined);
  check((await Promise.all(all.map(x => x.shows()))).every(n => n === null), `${label}: clear without a side clears every page`);
}
await browser.close();
console.log("ALL PAGE CHECKS PASSED");
