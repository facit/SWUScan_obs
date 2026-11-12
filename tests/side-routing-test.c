/*
 * Tests for src/side-routing.h (player sides; docs/obs-integration.md). No OBS needed:
 *
 *     cc -std=c11 -Wall -Wextra -Werror -Isrc tests/side-routing-test.c -o side-routing-test && ./side-routing-test
 *
 * Also run by .github/workflows/tests.yaml.
 */

#include <stdio.h>
#include <stdlib.h>

#include "side-routing.h"

static int failures;

#define CHECK(cond)                                                          \
	do {                                                                 \
		if (!(cond)) {                                               \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static void parse_side(void)
{
	CHECK(swu_side_from_int(1) == 1);
	CHECK(swu_side_from_int(2) == 2);
	CHECK(swu_side_from_int(0) == 0); /* missing: obs_data_get_int gives 0 */
	CHECK(swu_side_from_int(3) == 0); /* unknown sides are no side */
	CHECK(swu_side_from_int(-1) == 0);
}

static void routing(void)
{
	/* An old app's card (no side) shows everywhere. */
	for (int src = 0; src < 3; src++)
		CHECK(swu_side_takes(src, 0));
	/* Any takes every side. */
	CHECK(swu_side_takes(0, 1));
	CHECK(swu_side_takes(0, 2));
	/* A sided source takes its own side only. */
	CHECK(swu_side_takes(1, 1));
	CHECK(!swu_side_takes(1, 2));
	CHECK(swu_side_takes(2, 2));
	CHECK(!swu_side_takes(2, 1));
}

static void clearing(void)
{
	/* No side: everything goes. */
	for (int slot = 0; slot < 3; slot++)
		CHECK(swu_clear_drops(0, slot));
	/* Side 1: its card and the side-less one, never side 2's. */
	CHECK(swu_clear_drops(1, 1));
	CHECK(swu_clear_drops(1, 0));
	CHECK(!swu_clear_drops(1, 2));
	CHECK(swu_clear_drops(2, 2));
	CHECK(!swu_clear_drops(2, 1));
}

static void current_slot(void)
{
	long long none[SWU_SIDES] = {0, 0, 0};
	for (int src = 0; src < 3; src++)
		CHECK(swu_current_slot(src, none) == -1);

	/* Side 1 sent (seq 5), then side 2 (seq 6). */
	long long both[SWU_SIDES] = {0, 5, 6};
	CHECK(swu_current_slot(0, both) == 2); /* Any: the newest */
	CHECK(swu_current_slot(1, both) == 1);
	CHECK(swu_current_slot(2, both) == 2);

	/* An old app's card (seq 7) after both: newest everywhere. */
	long long old_app[SWU_SIDES] = {7, 5, 6};
	for (int src = 0; src < 3; src++)
		CHECK(swu_current_slot(src, old_app) == 0);

	/* A side card newer than an old side-less one wins on its side; the other side keeps the side-less one. */
	long long mixed[SWU_SIDES] = {3, 4, 0};
	CHECK(swu_current_slot(1, mixed) == 1);
	CHECK(swu_current_slot(2, mixed) == 0);
	CHECK(swu_current_slot(0, mixed) == 1);
}

int main(void)
{
	parse_side();
	routing();
	clearing();
	current_slot();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("side routing: all checks passed");
	return EXIT_SUCCESS;
}
