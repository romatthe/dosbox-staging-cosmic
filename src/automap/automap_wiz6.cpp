// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_wiz6.h"

#include <array>
#include <cassert>
#include <optional>

#include "cpu/paging.h"
#include "dosbox.h"
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

std::optional<uint16_t> read_word(const PhysPt offset)
{
	assert(data_segment_addr);

	uint16_t value = 0;

	if (mem_readw_checked(*data_segment_addr + offset, &value)) {
		return {};
	}

	return value;
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

std::string_view LevelName(const int level)
{
	if (level < 0 || level >= LevelCount) {
		return {};
	}

	return LevelNames[static_cast<size_t>(level)];
}

} // namespace wiz6
