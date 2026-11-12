/*
SWU Scan for OBS
Copyright (C) 2026 The SWU Scan authors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Player sides (docs/obs-integration.md, "show_card" and "clear"; the Stream mode spec,
 * docs/design/2026-11-12-obs-redesign.md in facit/SWUScan). Pure rules with no OBS dependency, so
 * tests/side-routing-test.c checks them with a plain C compiler. The pages (data/side.js) apply the same rules.
 *
 * Side 0 means "no side": a source set to Any, or a card sent without a side (an app from before sides).
 * A card is kept per side in slots 0, 1 and 2, each with the seq it was sent with (0 = empty).
 */

#pragma once

#include <stdbool.h>

#define SWU_SIDES 3

/* The side a request names: 1 or 2, else 0 (no side, or anything unknown). */
static inline int swu_side_from_int(long long value)
{
	return value == 1 || value == 2 ? (int)value : 0;
}

/* Whether a source set to source_side takes a card sent with card_side. Any takes every card, and a side-less
 * card goes everywhere, as before sides existed. */
static inline bool swu_side_takes(int source_side, int card_side)
{
	return source_side == 0 || card_side == 0 || source_side == card_side;
}

/* Whether clear(clear_side) drops the card kept in slot: a clear without a side drops everything; one with a
 * side drops that side's card and the side-less one. */
static inline bool swu_clear_drops(int clear_side, int slot)
{
	return clear_side == 0 || slot == 0 || slot == clear_side;
}

/* The slot a source set to source_side shows: the newest kept card it takes, or -1 for none. */
static inline int swu_current_slot(int source_side, const long long seqs[SWU_SIDES])
{
	int best = -1;
	for (int slot = 0; slot < SWU_SIDES; slot++) {
		if (seqs[slot] > 0 && swu_side_takes(source_side, slot) && (best < 0 || seqs[slot] > seqs[best]))
			best = slot;
	}
	return best;
}
