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

	bool operator==(const PartyPosition& other) const = default;
};

// The party's position, or nothing whenever there is no map to draw: the game
// is not running, or it is showing the title screen, a menu, or character
// creation rather than the dungeon.
std::optional<PartyPosition> GetPartyPosition();

// Polls the game and, when the party has moved, copies the current level's map
// data out of guest memory into the cache the automap reads. Call once per
// rendered frame; returns the party's position, as GetPartyPosition() does.
std::optional<PartyPosition> Update();

// One square of a level, unpacked from the packed bit arrays the game keeps it
// in.
struct Square {
	// The state of the square's north and east edges. Under 2 means the
	// edge can be seen and walked through; the exact values tell doors and
	// portcullises apart, which only matters when drawing.
	int north_wall = 0;
	int east_wall  = 0;

	// 14 is a pit. The rest are stairs, fountains and similar, decoded
	// when drawing.
	int feature           = 0;
	int feature_direction = 0;

	// Named for what the original draws rather than for what the bits are
	// called in its comments, which is misleading: the "floor map" bit is
	// clear for an ordinary indoor floor -- by far the common case, and
	// drawn with its dark tile -- and set, together with a roof, for
	// water. Neither bit says whether a square has a floor at all.
	bool is_dark_floor = false;
	bool has_roof      = false;
};

// A square of the cached map. Nothing if any coordinate is out of range; a
// level the party has never entered reads back as all zeroes.
std::optional<Square> GetSquare(const int level, const int quadrant,
                                const int x, const int y);

// The level-absolute coordinates of a quadrant's south-west corner, which is
// what turns quadrant-relative squares into level-wide ones.
struct QuadrantOrigin {
	int x = 0;
	int y = 0;
};

std::optional<QuadrantOrigin> GetQuadrantOrigin(const int level, const int quadrant);

// How much of a square the party knows about. The values are written to the
// automap's MAP.VIS file, so they are fixed.
enum class Visibility : uint8_t {
	Unseen  = 0,
	Visited = 1, // walked through
	Seen    = 2, // looked into from the square next door
};

Visibility GetVisibility(const int level, const int quadrant, const int x,
                         const int y);

// Mirrors the `automap_hide_in_dark_zones` setting: whether squares in the two
// levels the game treats as dark are left off the map.
void SetHideInDarkZones(const bool enabled);

// Whether a square is somewhere the map is meant to stay blank. Only ever true
// on the two levels the game gives dark areas, and only while the setting
// above is on.
bool IsDarkZone(const int level, const int quadrant, const int x, const int y);

// The game's own name for a dungeon level. Empty for an out-of-range index.
std::string_view LevelName(const int level);

} // namespace wiz6

#endif // DOSBOX_AUTOMAP_WIZ6_H
