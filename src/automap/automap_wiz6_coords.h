// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_WIZ6_COORDS_H
#define DOSBOX_AUTOMAP_WIZ6_COORDS_H

#include "automap_wiz6.h"

// The arithmetic that turns a square of a Wizardry VI level into a place on the
// map, and back. Kept apart from the rasteriser so that both directions are
// obliged to agree, and so it can be tested without a window or a running game.
namespace wiz6 {

// A square is this many pixels on the map, and a level is 256 squares tall.
// Both are baked into the original's arithmetic and its file formats.
constexpr int SquarePx  = 22;
constexpr int LevelRows = 256;

// The game's Y axis points north while the map's points south, so a square's
// row counts *down* from the level's northern edge. The rasteriser arrives at
// it in two steps, in two different functions: a quadrant's origin sits at row
// (LevelRows - 1 - origin_y), and a square sits (QuadrantSize - 1 - y) below
// that. Composing them gives the row of any level-absolute Y.
//
// Both directions must go through these. They did not, once: the hit-test
// spelled the constant out for itself as (LevelRows - 1 + QuadrantSize) and
// every clicked square came back one row too far north.
//
// Beware that this is *not* the constant in the rasteriser's `view_y`, which is
// one larger and correct at that value -- the original does the same, at
// am_wiz6.cpp:1181 against 262 in its own hit-test, and Phase 3's pixel
// comparison validated it. The two look like they should match and must not be
// made to.
constexpr int RowOfAbsY(const int abs_y)
{
	return (LevelRows - 1) + (QuadrantSize - 1) - abs_y;
}

constexpr int AbsYOfRow(const int row)
{
	return (LevelRows - 1) + (QuadrantSize - 1) - row;
}

} // namespace wiz6

#endif // DOSBOX_AUTOMAP_WIZ6_COORDS_H
