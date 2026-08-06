// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_WIZ6_RENDER_H
#define DOSBOX_AUTOMAP_WIZ6_RENDER_H

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

} // namespace wiz6

#endif // DOSBOX_AUTOMAP_WIZ6_RENDER_H
