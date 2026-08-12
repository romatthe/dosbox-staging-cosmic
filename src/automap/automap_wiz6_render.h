// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_WIZ6_RENDER_H
#define DOSBOX_AUTOMAP_WIZ6_RENDER_H

#include <optional>

struct SDL_Surface;

// Draws the Wizardry VI map into surfaces the automap module owns. Nothing
// here touches a window, a renderer or the GPU: the module presents whatever
// surface comes out of this, which is what keeps the option of compositing the
// map into the emulator's own window later.
namespace wiz6 {

// Decodes the tile sheet into a surface. Safe to call more than once; returns
// false only if the tiles could not be prepared, in which case there is
// nothing to draw with.
bool InitTileAtlas();

void FreeTileAtlas();

// Draws the current map at the given size, centred on the party, and returns
// the surface it was drawn into. Returns nullptr when there is nothing to draw
// -- no game, no party in the dungeon, or the party standing in a dark zone.
//
// The surface belongs to this module and is reused between calls, so the
// caller must neither free it nor hold on to it across a size change.
SDL_Surface* RenderMap(const int width_px, const int height_px);

// Pans the map by a mouse drag, given in map pixels, so that the map follows
// the cursor. The pan lasts only until the party next moves or turns, or until
// RecentreMap(); it is a look around rather than a mode the map stays in.
void ScrollMap(const int delta_x_px, const int delta_y_px);

// Drops any pan, putting the party back in the middle of the window.
void RecentreMap();

// A square of the level the map is currently showing.
struct MapSquare {
	int level    = 0;
	int quadrant = 0;
	int x        = 0;
	int y        = 0;
};

// Which square a point in the map surface falls on, or nothing if it lands
// outside every quadrant -- quadrants do not tile a level, so most of the
// window usually does. Coordinates are map pixels, not window coordinates:
// the two differ on a HiDPI display.
//
// Answers about the last frame drawn, so it returns nothing until there has
// been one.
std::optional<MapSquare> SquareAtPixel(const int x_px, const int y_px);

} // namespace wiz6

#endif // DOSBOX_AUTOMAP_WIZ6_RENDER_H
