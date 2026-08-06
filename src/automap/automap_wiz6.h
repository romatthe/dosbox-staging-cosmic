// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_WIZ6_H
#define DOSBOX_AUTOMAP_WIZ6_H

#include <cstdint>
#include <optional>
#include <string_view>

// State model for Wizardry VI: Bane of the Cosmic Forge.
//
// The game keeps everything the automap needs in one data segment with a fixed
// layout, so all of it can be read as offsets from a single base address. That
// base is resolved once, when the guest loads the game.
namespace wiz6 {

// Identifies the program the guest has just loaded. Returns true if it is
// Wizardry VI, in which case the game's data segment is now known and the rest
// of this module can be used.
//
// `headersize` is zero for .COM files, which have no header.
bool DetectGame(const std::string_view name, const uint16_t loadseg,
                const uint32_t headersize);

// A level is tiled by up to 12 quadrants of 8x8 squares. Both counts are baked
// into the game's data and into the automap's own file format, so they are
// fixed rather than configurable.
constexpr int LevelCount    = 16;
constexpr int QuadrantCount = 12;
constexpr int QuadrantSize  = 8;

enum class Facing { North, East, South, West };

// Where the party is standing. `x` and `y` are square coordinates within
// `quadrant`, both in the range [0, QuadrantSize).
struct PartyPosition {
	int level     = 0;
	int quadrant  = 0;
	int x         = 0;
	int y         = 0;
	Facing facing = Facing::North;
};

// The party's position, or nothing whenever there is no map to draw: the game
// is not running, or it is showing the title screen, a menu, or character
// creation rather than the dungeon.
std::optional<PartyPosition> GetPartyPosition();

// The game's own name for a dungeon level. Empty for an out-of-range index.
std::string_view LevelName(const int level);

} // namespace wiz6

#endif // DOSBOX_AUTOMAP_WIZ6_H
