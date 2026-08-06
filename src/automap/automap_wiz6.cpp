// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_wiz6.h"

#include <array>
#include <cassert>
#include <optional>
#include <span>

#include "cpu/paging.h"
#include "dosbox.h"
#include "hardware/memory.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

CHECK_NARROWING();

namespace wiz6 {

namespace {

constexpr std::string_view ExecutableName = "WROOT.EXE";

// "WMAZE\0" sits at a fixed offset into the loaded image, and the data segment
// a fixed distance in front of that. Both constants come from the original mod
// and are the anchor for every other offset in this file.
constexpr uint32_t SignatureOffset        = 0x10580;
constexpr uint32_t SignatureToDataSegment = 0x600;

// Deliberately six bytes: the terminating NUL is part of what is matched, as
// in the original.
constexpr std::string_view Signature = std::string_view("WMAZE\0", 6);

// Set once the signature check passes, and cleared whenever the guest loads
// anything else. Empty means "no game to map".
std::optional<PhysPt> data_segment_addr = {};

// Scalars in the data segment, all 16-bit. See PORTING.md section 10.1.
constexpr PhysPt GameStateOffset = 0x363A;
constexpr PhysPt LevelOffset     = 0x363C;
constexpr PhysPt FacingOffset    = 0x4F9A;
constexpr PhysPt QuadrantOffset  = 0x4F9C;
constexpr PhysPt QuadrantYOffset = 0x4F9E;
constexpr PhysPt QuadrantXOffset = 0x4FA0;

constexpr std::array<std::string_view, LevelCount> LevelNames = {
        "Castle Entrance and Towers",
        "Castle Upper Level, Tower Stairs and Bell Tower",
        "Castle Basement and Hazard Area",
        "Mountain Area and Amazulu Burial Chamber",
        "Amazulu Pyramid",
        "Mines Area",
        "Mountain Alpes and Castle Lower Level",
        "Pyramid Basement",
        "River Styx",
        "Hall of the Dead and Tomb of the Damned",
        "Swamp Area",
        "Temple of Ramm",
        "Forest Area",
        "Lower Level of the Temple of Ramm",
        "Map 14",
        "Map 15",
};

// Offsets within the map data block, whose own offset is stored at
// MapDataOffset. See PORTING.md section 10.2.
constexpr PhysPt MapDataOffset = 0x4FAA;

constexpr PhysPt HorizontalWallsOffset  = 0x060;
constexpr PhysPt VerticalWallsOffset    = 0x120;
constexpr PhysPt QuadrantStartXOffset   = 0x1E0;
constexpr PhysPt QuadrantStartYOffset   = 0x1EC;
constexpr PhysPt FeaturesOffset         = 0x1F8;
constexpr PhysPt FeatureDirectionOffset = 0x378;
constexpr PhysPt FloorOffset            = 0x43A;
constexpr PhysPt RoofOffset             = 0x49A;

constexpr int SquaresPerLevel = QuadrantCount * QuadrantSize * QuadrantSize;

// The game stores each map as a set of arrays indexed by square, packed at a
// different number of bits per square. The sizes follow from that, and are
// also what the automap's own MAP.CAC file is made of, so they are fixed.
template <int BitsPerSquare>
using PackedArray = std::array<uint8_t, SquaresPerLevel * BitsPerSquare / 8>;

struct LevelCache {
	std::array<uint8_t, QuadrantCount> quadrant_start_x = {};
	std::array<uint8_t, QuadrantCount> quadrant_start_y = {};

	PackedArray<2> horizontal_walls   = {};
	PackedArray<2> vertical_walls     = {};
	PackedArray<4> features           = {};
	PackedArray<2> feature_directions = {};
	PackedArray<1> floor              = {};
	PackedArray<1> roof               = {};
};

// Roughly 18 KB. Every level the party has visited this session stays here, so
// the automap can be drawn without the game being on that level.
std::array<LevelCache, LevelCount> level_cache = {};

// What the cache was last refreshed for. A refresh is only worth doing when
// the party has actually moved.
std::optional<PartyPosition> cached_position = {};

std::optional<uint16_t> read_word(const PhysPt offset)
{
	assert(data_segment_addr);

	uint16_t value = 0;

	if (mem_readw_checked(*data_segment_addr + offset, &value)) {
		return {};
	}

	return value;
}

// Reads one element out of a packed array. The original did this with a 16-bit
// read at an arbitrary byte offset, which is unaligned and runs one byte past
// the end of the array for its last element; both are undefined behaviour, and
// only stayed hidden because the mod was 32-bit Windows only.
uint8_t get_packed_element(const std::span<const uint8_t> array,
                           const int index, const int bits_per_element)
{
	assert(bits_per_element >= 1 && bits_per_element <= 8);

	const auto bit_offset  = index * bits_per_element;
	const auto byte_offset = static_cast<size_t>(bit_offset / 8);
	const auto shift       = bit_offset % 8;

	if (byte_offset >= array.size()) {
		return 0;
	}

	// An element can straddle two bytes, so read the next one as well
	// unless this is the last.
	const auto bytes = static_cast<unsigned>(array[byte_offset]) |
	                   (byte_offset + 1 < array.size()
	                            ? static_cast<unsigned>(array[byte_offset + 1]) << 8
	                            : 0u);

	const auto mask = (1u << bits_per_element) - 1u;

	return static_cast<uint8_t>((bytes >> shift) & mask);
}

// Copies the whole of the current level's map out of guest memory. The game
// only keeps one level's worth in memory at a time, which is why the automap
// has to cache it: the map of a level you have left is otherwise gone.
void refresh_level_cache(const int level)
{
	assert(data_segment_addr);
	assert(level >= 0 && level < LevelCount);

	const auto map_data_offset = read_word(MapDataOffset);

	if (!map_data_offset) {
		return;
	}

	const auto base = *data_segment_addr + *map_data_offset;

	auto& cache = level_cache[static_cast<size_t>(level)];

	const auto read_array = [base](const PhysPt offset, auto& destination) {
		MEM_BlockRead(base + offset, destination.data(), destination.size());
	};

	read_array(QuadrantStartXOffset, cache.quadrant_start_x);
	read_array(QuadrantStartYOffset, cache.quadrant_start_y);
	read_array(HorizontalWallsOffset, cache.horizontal_walls);
	read_array(VerticalWallsOffset, cache.vertical_walls);
	read_array(FeaturesOffset, cache.features);
	read_array(FeatureDirectionOffset, cache.feature_directions);
	read_array(FloorOffset, cache.floor);
	read_array(RoofOffset, cache.roof);
}

// The game drives its whole screen flow from one variable; these are the
// values that mean the party is standing in the maze. Everything else is the
// title screen, a menu, character creation, or a full-screen scene, none of
// which have a map behind them.
bool is_in_dungeon(const uint16_t game_state)
{
	switch (game_state) {
	case 5:
	case 10:
	case 11:
	case 12:
	case 13:
	case 14:
	case 19:
	case 20:
	case 22:
	case 24: return true;
	default: return false;
	}
}

// DOS accepts either separator, and a drive-relative path such as
// `C:WROOT.EXE` has no separator at all.
std::string_view dos_basename(const std::string_view path)
{
	const auto separator_pos = path.find_last_of("\\/:");

	return separator_pos == std::string_view::npos
	             ? path
	             : path.substr(separator_pos + 1);
}

bool has_signature_at(const PhysPt addr)
{
	for (size_t i = 0; i < Signature.size(); ++i) {
		uint8_t byte = 0;

		if (mem_readb_checked(addr + static_cast<PhysPt>(i), &byte)) {
			return false;
		}
		if (byte != static_cast<uint8_t>(Signature[i])) {
			return false;
		}
	}

	return true;
}

} // namespace

bool DetectGame(const std::string_view name, const uint16_t loadseg,
                const uint32_t headersize)
{
	data_segment_addr = {};

	// The cache belongs to whichever run of the game filled it, so a fresh
	// load has to refresh it even if the party happens to start where the
	// previous one stood.
	cached_position = {};

	if (!iequals(dos_basename(name), ExecutableName)) {
		return false;
	}

	// The name alone proves nothing -- any program can be called
	// WROOT.EXE, and reading the map out of one that isn't Wizardry VI
	// would show nonsense. The signature is what confirms the image, and
	// where it sits is what locates the data segment.
	const auto load_addr      = PhysicalMake(loadseg, 0);
	const auto signature_addr = load_addr + SignatureOffset - headersize;

	if (!has_signature_at(signature_addr)) {
		LOG_WARNING("AUTOMAP: Program is named %s but is not Wizardry VI; ignoring it",
		            ExecutableName.data());
		return false;
	}

	data_segment_addr = signature_addr - SignatureToDataSegment;

	LOG_MSG("AUTOMAP: Detected Wizardry VI with its data segment at %04xh",
	        *data_segment_addr);

	return true;
}

std::optional<PartyPosition> GetPartyPosition()
{
	if (!data_segment_addr) {
		return {};
	}

	const auto game_state = read_word(GameStateOffset);

	if (!game_state || !is_in_dungeon(*game_state)) {
		return {};
	}

	const auto level    = read_word(LevelOffset);
	const auto quadrant = read_word(QuadrantOffset);
	const auto x        = read_word(QuadrantXOffset);
	const auto y        = read_word(QuadrantYOffset);
	const auto facing   = read_word(FacingOffset);

	if (!level || !quadrant || !x || !y || !facing) {
		return {};
	}

	// The game leaves the last dungeon coordinates in place when the party
	// is not in the maze and parks the level index past the end, so these
	// are genuine "is there a map" tests and not just bounds guards.
	if (*level >= LevelCount || *quadrant >= QuadrantCount ||
	    *x >= QuadrantSize || *y >= QuadrantSize) {
		return {};
	}

	return PartyPosition{static_cast<int>(*level),
	                     static_cast<int>(*quadrant),
	                     static_cast<int>(*x),
	                     static_cast<int>(*y),
	                     // The original draws south for anything that is
	                     // not one of the three other directions.
	                     *facing < 4 ? static_cast<Facing>(*facing)
	                                 : Facing::South};
}

std::optional<PartyPosition> Update()
{
	const auto position = GetPartyPosition();

	if (!position) {
		return {};
	}

	// A turn on the spot cannot change the map, but the original refreshes
	// on it too and it costs one block copy, so keep the behaviour: the
	// game reveals squares as the party looks at them.
	if (position != cached_position) {
		refresh_level_cache(position->level);
		cached_position = position;
	}

	return position;
}

std::optional<Square> GetSquare(const int level, const int quadrant,
                                const int x, const int y)
{
	if (level < 0 || level >= LevelCount || quadrant < 0 ||
	    quadrant >= QuadrantCount || x < 0 || x >= QuadrantSize || y < 0 ||
	    y >= QuadrantSize) {
		return {};
	}

	const auto& cache = level_cache[static_cast<size_t>(level)];

	// Every packed array is indexed the same way.
	const auto index = quadrant * QuadrantSize * QuadrantSize +
	                   y * QuadrantSize + x;

	return Square{get_packed_element(cache.horizontal_walls, index, 2),
	              get_packed_element(cache.vertical_walls, index, 2),
	              get_packed_element(cache.features, index, 4),
	              get_packed_element(cache.feature_directions, index, 2),
	              get_packed_element(cache.floor, index, 1) != 0,
	              get_packed_element(cache.roof, index, 1) != 0};
}

std::optional<QuadrantOrigin> GetQuadrantOrigin(const int level, const int quadrant)
{
	if (level < 0 || level >= LevelCount || quadrant < 0 ||
	    quadrant >= QuadrantCount) {
		return {};
	}

	const auto& cache = level_cache[static_cast<size_t>(level)];

	return QuadrantOrigin{cache.quadrant_start_x[static_cast<size_t>(quadrant)],
	                      cache.quadrant_start_y[static_cast<size_t>(quadrant)]};
}

std::string_view LevelName(const int level)
{
	if (level < 0 || level >= LevelCount) {
		return {};
	}

	return LevelNames[static_cast<size_t>(level)];
}

} // namespace wiz6
