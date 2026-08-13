// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_OVERLAY_H
#define DOSBOX_AUTOMAP_OVERLAY_H

#include "automap_wiz6_coords.h"

struct SDL_Renderer;
struct SDL_Window;
union SDL_Event;

// The automap window's ImGui layer, drawn over the map texture. The note
// tooltip, the note editor and the colour picker all live behind this, and it
// is the only part of the automap that knows ImGui exists -- which matters
// more than it usually would, because ImGui keeps its state in a single global
// context pointer and the debugger window owns a context of its own.
//
// The map itself is never drawn through ImGui. It stays a CPU-side surface
// uploaded to a texture, so the option of compositing it into the emulator's
// own window is not spent here.
namespace overlay {

// Sets the overlay up for a window and the renderer presenting it. Returns
// false if that failed, in which case the automap simply carries on without an
// overlay: the map does not depend on it.
bool Init(SDL_Window* window, SDL_Renderer* renderer);

// Whether the overlay has claimed the event, in which case the automap's own
// controls should leave it alone -- a drag on a widget must not also pan the
// map behind it. Only mouse events are ever claimed.
bool HandleEvent(const SDL_Event& event);

// Draws the overlay into whatever the renderer is currently targeting. Call
// after the map and before presenting.
void Draw(SDL_Renderer* renderer);

// Opens the note editor on a square, with whatever note is already there in
// the field. Confirming it empty removes the note, which is the only way to
// delete one -- in this port as in the original.
void EditNote(const wiz6::MapSquare& square);

// Opens the colour picker for the note on a square. Does nothing if that
// square has no note: a colour is a property of a note, not of a square.
void EditNoteColour(const wiz6::MapSquare& square);

} // namespace overlay

#endif // DOSBOX_AUTOMAP_OVERLAY_H
