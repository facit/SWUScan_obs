/*
  Player sides for the card and prices pages (docs/obs-integration.md; the same rules as src/side-routing.h).
  obs-browser's emit_event reaches every page, so each page decides here whether a card or a clear is for it.
  The page's own side arrives as --side in the CSS the plugin generates from the source's Player side property
  (0 = Any, 1 or 2). A card or clear sent without a side has side 0.
*/
"use strict";
const SwuSide = {
  page() {
    const v = parseInt(getComputedStyle(document.documentElement).getPropertyValue("--side"), 10);
    return v === 1 || v === 2 ? v : 0;
  },
  of(detail) {
    const v = detail && Number(detail.side);
    return v === 1 || v === 2 ? v : 0;
  },
  /* Any takes every card; a side-less card goes everywhere; otherwise the sides must match. */
  takes(pageSide, cardSide) {
    return pageSide === 0 || cardSide === 0 || pageSide === cardSide;
  },
  /* A clear without a side clears everything; one with a side clears that side's card and a side-less one. */
  clears(clearSide, shownSide) {
    return clearSide === 0 || shownSide === 0 || shownSide === clearSide;
  },
};
