// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_wiz6.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "cpu/paging.h"
#include "dos/dos.h"
#include "dos/dos_system.h"
#include "dosbox.h"
#include "hardware/memory.h"
#include "misc/support.h"
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

// Indexed [level][quadrant][x][y], which is also the order the MAP.VIS file
// stores it in. 12 KB.
using QuadrantVisibility = std::array<std::array<Visibility, QuadrantSize>, QuadrantSize>;

std::array<std::array<QuadrantVisibility, QuadrantCount>, LevelCount> visibility = {};

bool hide_in_dark_zones = true;

// The one feature value the visibility rules care about.
constexpr int PitFeature = 14;

// An edge with a value below this can be seen and walked through.
constexpr int OpenEdge = 2;

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

// Which quadrant a level-absolute square falls in, and where in it. Quadrants
// do not tile the whole level, so a coordinate can fall outside every one.
struct QuadrantLocation {
	int quadrant = 0;
	int x        = 0;
	int y        = 0;
};

std::optional<QuadrantLocation> abs_to_quadrant(const int level,
                                                const int abs_x, const int abs_y)
{
	for (auto quadrant = 0; quadrant < QuadrantCount; ++quadrant) {
		const auto origin = GetQuadrantOrigin(level, quadrant);

		if (!origin) {
			continue;
		}

		const auto x = abs_x - origin->x;
		const auto y = abs_y - origin->y;

		if (x >= 0 && x < QuadrantSize && y >= 0 && y < QuadrantSize) {
			return QuadrantLocation{quadrant, x, y};
		}
	}

	return {};
}

// Squares are addressed absolutely from here on, because the square next to
// this one can be in the next quadrant.
std::optional<Square> square_at(const int level, const int abs_x, const int abs_y)
{
	const auto location = abs_to_quadrant(level, abs_x, abs_y);

	if (!location) {
		return {};
	}

	return GetSquare(level, location->quadrant, location->x, location->y);
}

// A square only stores its own north and east edges, so the other two are read
// from the neighbours that own them.
bool north_edge_is_open(const int level, const int abs_x, const int abs_y)
{
	const auto square = square_at(level, abs_x, abs_y);

	return square && square->north_wall < OpenEdge;
}

bool east_edge_is_open(const int level, const int abs_x, const int abs_y)
{
	const auto square = square_at(level, abs_x, abs_y);

	return square && square->east_wall < OpenEdge;
}

bool south_edge_is_open(const int level, const int abs_x, const int abs_y)
{
	return north_edge_is_open(level, abs_x, abs_y - 1);
}

bool west_edge_is_open(const int level, const int abs_x, const int abs_y)
{
	return east_edge_is_open(level, abs_x - 1, abs_y);
}

// Visibility is only ever upgraded: walking through a square makes it visited
// for good, and a square seen from next door stays seen until it is walked.
void mark_visibility(const int level, const int abs_x, const int abs_y,
                     const Visibility new_visibility)
{
	const auto location = abs_to_quadrant(level, abs_x, abs_y);

	if (!location) {
		return;
	}

	if (IsDarkZone(level, location->quadrant, location->x, location->y)) {
		return;
	}

	auto& stored = visibility[static_cast<size_t>(level)][static_cast<size_t>(
	        location->quadrant)][static_cast<size_t>(location->x)]
	                         [static_cast<size_t>(location->y)];

	if (stored == Visibility::Unseen || new_visibility == Visibility::Visited) {
		stored = new_visibility;
	}
}

// Records what the party can see from where it stands: its own square, plus
// each of the four next to it whose shared edge is open.
void update_visibility(const PartyPosition& position)
{
	const auto origin = GetQuadrantOrigin(position.level, position.quadrant);

	if (!origin) {
		return;
	}

	const auto level = position.level;
	const auto x     = origin->x + position.x;
	const auto y     = origin->y + position.y;

	mark_visibility(level, x, y, Visibility::Visited);

	const struct {
		int dx;
		int dy;
		bool is_open;
	} neighbours[] = {
	        { 0,  1, north_edge_is_open(level, x, y)},
	        { 1,  0,  east_edge_is_open(level, x, y)},
	        { 0, -1, south_edge_is_open(level, x, y)},
	        {-1,  0,  west_edge_is_open(level, x, y)},
	};

	for (const auto& neighbour : neighbours) {
		if (neighbour.is_open) {
			mark_visibility(level,
			                x + neighbour.dx,
			                y + neighbour.dy,
			                Visibility::Seen);
		}
	}
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

// The game's own files, watched for but never touched.
constexpr std::string_view NewGameFileName  = "NEWGAME.DBS";
constexpr std::string_view SaveGameFileName = "SAVEGAME.DBS";

// The automap's files, and the one previous generation of each it keeps.
//
// These are plain pointers rather than string_views because every use is a DOS
// call wanting a C string, and nothing here compares them.
constexpr auto CacheFileName        = "MAP.CAC";
constexpr auto CacheBackupFileName  = "MAPCAC.BAK";
constexpr auto VisibilityFileName   = "MAP.VIS";
constexpr auto VisibilityBackupName = "MAPVIS.BAK";
constexpr auto NotesFileName        = "MAP.NTS";
constexpr auto NotesBackupName      = "MAPNTS.BAK";
constexpr auto PaletteFileName      = "MAP.PAL";
constexpr auto PaletteBackupName    = "MAPPAL.BAK";

// The two files are a plain image of the arrays below, so their sizes are the
// arrays' sizes and both are fixed by the format the original wrote in 2014.
// Every member is a byte array, so neither struct has any padding to make the
// image differ from what is in memory.
constexpr size_t CacheFileSize      = 18816;
constexpr size_t VisibilityFileSize = 12288;

static_assert(sizeof(level_cache) == CacheFileSize);
static_assert(sizeof(visibility) == VisibilityFileSize);

// MAP.CAC is array-major -- every level's quadrant_start_x, then every
// level's quadrant_start_y, and so on -- while `level_cache` is an array of
// per-level structs. This walks the cache in the file's order, so reading and
// writing it are both a plain sequence of transfers.
template <typename Callback>
void for_each_cache_array(const Callback& callback)
{
	const auto every_level = [&callback](auto member) {
		for (auto& cache : level_cache) {
			callback(std::span<uint8_t>(cache.*member));
		}
	};

	every_level(&LevelCache::quadrant_start_x);
	every_level(&LevelCache::quadrant_start_y);
	every_level(&LevelCache::horizontal_walls);
	every_level(&LevelCache::vertical_walls);
	every_level(&LevelCache::features);
	every_level(&LevelCache::feature_directions);
	every_level(&LevelCache::floor);
	every_level(&LevelCache::roof);
}

// The visibility array is contiguous and already in the file's order, so it
// moves in one transfer.
std::span<uint8_t> visibility_bytes()
{
	return {reinterpret_cast<uint8_t*>(visibility.data()), sizeof(visibility)};
}

// DOS reports a short transfer by lowering the count rather than failing, so
// anything less than the whole array means a truncated or corrupt file.
bool read_exactly(const uint16_t handle, const std::span<uint8_t> destination)
{
	auto amount = check_cast<uint16_t>(destination.size());

	return DOS_ReadFile(handle, destination.data(), &amount) &&
	       amount == destination.size();
}

bool write_exactly(const uint16_t handle, const std::span<uint8_t> source)
{
	auto amount = check_cast<uint16_t>(source.size());

	return DOS_WriteFile(handle, source.data(), &amount) &&
	       amount == source.size();
}

// Notes and the palette live beside the explored map but are the player's
// writing rather than anything read out of the game, so they are kept apart
// from the caches the game's memory refills every frame.
std::array<std::vector<Note>, LevelCount> notes = {};

std::array<uint32_t, PaletteSize> palette = {};

void clear_cache()
{
	level_cache = {};
	visibility  = {};
}

void clear_notes()
{
	notes   = {};
	palette = {};
}

// MAP.NTS is a stream of fixed-width little-endian scalars rather than an
// image of an array, so each field states its own width. Little-endian is
// what the original wrote and what every host this runs on uses.
template <typename T>
std::span<uint8_t> scalar_bytes(T& value)
{
	static_assert(std::is_trivially_copyable_v<T>);

	return {reinterpret_cast<uint8_t*>(&value), sizeof(T)};
}

// The text is the one variable-width field: a count of 16-bit code units,
// then that many units plus a terminator. Both are on disk, and the
// terminator is read and written to keep the byte count the original's.
bool read_note_text(const uint16_t handle, std::u16string& text)
{
	uint32_t length = 0;

	if (!read_exactly(handle, scalar_bytes(length))) {
		return false;
	}

	// A length that cannot fit in one DOS transfer is a corrupt file, not a
	// note anybody typed; `read_exactly` would otherwise narrow it and read
	// the wrong amount.
	constexpr uint32_t MaxLength = UINT16_MAX / sizeof(char16_t) - 1;

	if (length > MaxLength) {
		return false;
	}

	text.assign(length + 1, u'\0');

	if (!read_exactly(handle,
	                  {reinterpret_cast<uint8_t*>(text.data()),
	                   (length + 1) * sizeof(char16_t)})) {
		return false;
	}

	text.resize(length);

	return true;
}

bool write_note_text(const uint16_t handle, const std::u16string& text)
{
	auto length = check_cast<uint32_t>(text.size());

	if (!write_exactly(handle, scalar_bytes(length))) {
		return false;
	}

	// `std::u16string` keeps a terminator past `size()` that `c_str()`
	// guarantees, which is exactly the extra unit the format wants.
	return write_exactly(handle,
	                     {reinterpret_cast<uint8_t*>(
	                              const_cast<char16_t*>(text.c_str())),
	                      (text.size() + 1) * sizeof(char16_t)});
}

// A note is pinned to a square and there is only ever one per square, so a
// level cannot hold more notes than it has squares. Anything past that is a
// corrupt count rather than a number to start allocating against.
constexpr uint32_t MaxNotesPerLevel = QuadrantCount * QuadrantSize * QuadrantSize;

bool read_notes(const uint16_t handle)
{
	for (auto& level_notes : notes) {
		uint32_t count = 0;

		if (!read_exactly(handle, scalar_bytes(count)) ||
		    count > MaxNotesPerLevel) {
			return false;
		}

		level_notes.clear();
		level_notes.reserve(count);

		for (uint32_t i = 0; i < count; ++i) {
			uint16_t quadrant = 0;
			uint16_t x        = 0;
			uint16_t y        = 0;
			uint32_t colour   = 0;

			Note note = {};

			if (!read_exactly(handle, scalar_bytes(quadrant)) ||
			    !read_exactly(handle, scalar_bytes(x)) ||
			    !read_exactly(handle, scalar_bytes(y)) ||
			    !read_exactly(handle, scalar_bytes(colour)) ||
			    !read_note_text(handle, note.text)) {
				return false;
			}

			// A note outside the map would never draw and could
			// not be reached to delete, so treat it as corruption
			// rather than carry it around.
			if (quadrant >= QuadrantCount || x >= QuadrantSize ||
			    y >= QuadrantSize) {
				return false;
			}

			note.quadrant = quadrant;
			note.x        = x;
			note.y        = y;
			note.colour   = colour;

			level_notes.push_back(std::move(note));
		}
	}

	return true;
}

bool write_notes(const uint16_t handle)
{
	for (const auto& level_notes : notes) {
		auto count = check_cast<uint32_t>(level_notes.size());

		if (!write_exactly(handle, scalar_bytes(count))) {
			return false;
		}

		for (const auto& note : level_notes) {
			auto quadrant = check_cast<uint16_t>(note.quadrant);
			auto x        = check_cast<uint16_t>(note.x);
			auto y        = check_cast<uint16_t>(note.y);
			auto colour   = note.colour;

			if (!write_exactly(handle, scalar_bytes(quadrant)) ||
			    !write_exactly(handle, scalar_bytes(x)) ||
			    !write_exactly(handle, scalar_bytes(y)) ||
			    !write_exactly(handle, scalar_bytes(colour)) ||
			    !write_note_text(handle, note.text)) {
				return false;
			}
		}
	}

	return true;
}

std::span<uint8_t> palette_bytes()
{
	return {reinterpret_cast<uint8_t*>(palette.data()), sizeof(palette)};
}

// Notes are keyed by square, so this is both the lookup and the uniqueness
// rule the file format relies on.
std::vector<Note>::iterator find_note(std::vector<Note>& level_notes,
                                      const int quadrant, const int x, const int y)
{
	return std::ranges::find_if(level_notes, [&](const Note& note) {
		return note.quadrant == quadrant && note.x == x && note.y == y;
	});
}

bool level_is_valid(const int level)
{
	return level >= 0 && level < LevelCount;
}

// Keeps one previous generation of a file, exactly as the original does: a
// save that goes wrong halfway costs the player the session's exploring, not
// the whole map.
void rotate_backup(const char* const name, const char* const backup_name)
{
	if (!DOS_FileExists(name)) {
		return;
	}

	if (DOS_FileExists(backup_name)) {
		DOS_UnlinkFile(backup_name);
	}

	DOS_Rename(name, backup_name);
}

// `transfer` is handed the open file and returns whether every byte it wanted
// arrived. A file that is not there yet is not an error: it is what the first
// save on an existing game looks like.
template <typename Transfer>
bool load_file(const char* const name, const Transfer& transfer)
{
	uint16_t handle = 0;

	if (!DOS_FileExists(name) || !DOS_OpenFile(name, OPEN_READ, &handle)) {
		return true;
	}

	const auto complete = transfer(handle);

	DOS_CloseFile(handle);

	if (!complete) {
		LOG_WARNING("AUTOMAP: %s is truncated or corrupt; ignoring it", name);
	}

	return complete;
}

template <typename Transfer>
void save_file(const char* const name, const char* const backup_name,
               const Transfer& transfer)
{
	rotate_backup(name, backup_name);

	uint16_t handle = 0;

	if (!DOS_CreateFile(name, FatAttributeFlags{}, &handle)) {
		LOG_WARNING("AUTOMAP: Could not create %s", name);
		return;
	}

	if (!transfer(handle)) {
		LOG_WARNING("AUTOMAP: Could not write all of %s", name);
	}

	DOS_FlushFile(handle);
	DOS_CloseFile(handle);
}

// Moves the whole level cache through one open file, in MAP.CAC's order.
template <typename Element>
bool transfer_cache(const uint16_t handle, const Element& element)
{
	auto complete = true;

	for_each_cache_array([&](const std::span<uint8_t> array) {
		complete = complete && element(handle, array);
	});

	return complete;
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

std::optional<PartyPosition> Update()
{
	const auto position = GetPartyPosition();

	if (!position) {
		return {};
	}

	// Refreshed every frame, where the original refreshes only when the
	// party moves.
	//
	// The original tests walls by reading guest memory directly and only
	// keeps this cache for drawing, so a stale cache costs it nothing. This
	// port tests walls against the cache instead, which makes staleness
	// visible: the game does not update the level index and the map data
	// block on the same frame, so a cache filled mid-transition holds the
	// wrong level's walls. The map corrects itself on the next move, but
	// visibility never does -- it only ever upgrades -- so a square wrongly
	// marked "seen" while stepping between levels stays on the map for good.
	//
	// Eight block copies of about 1.2 KB each per frame is not worth
	// optimising to avoid that.
	refresh_level_cache(position->level);

	// Deliberately every frame, not just on movement: the map data the
	// dark-zone test reads can change under a stationary party.
	update_visibility(*position);

	return position;
}

Visibility GetVisibility(const int level, const int quadrant, const int x, const int y)
{
	if (level < 0 || level >= LevelCount || quadrant < 0 ||
	    quadrant >= QuadrantCount || x < 0 || x >= QuadrantSize || y < 0 ||
	    y >= QuadrantSize) {
		return Visibility::Unseen;
	}

	return visibility[static_cast<size_t>(level)][static_cast<size_t>(quadrant)]
	                 [static_cast<size_t>(x)][static_cast<size_t>(y)];
}

// Two levels of the game have areas the party cannot see in, and the original
// hardcodes which -- that is game knowledge, not a heuristic.
bool IsDarkZone(const int level, const int quadrant, const int x, const int y)
{
	if (!hide_in_dark_zones || (level != 5 && level != 12)) {
		return false;
	}

	const auto square = GetSquare(level, quadrant, x, y);

	if (!square) {
		return false;
	}

	// This reads backwards and is meant to: on those two levels it is the
	// ordinary floor squares -- the ones the original draws with its dark
	// tile, pits excepted -- that stay visible, and everything else that
	// is hidden.
	const auto drawn_as_dark_tile = square->is_dark_floor &&
	                                square->feature != PitFeature;

	return !drawn_as_dark_tile;
}
void SetHideInDarkZones(const bool enabled)
{
	hide_in_dark_zones = enabled;
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
	              get_packed_element(cache.floor, index, 1) == 0,
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

PersistenceRequest RequestForOpenedFile(const std::string_view dos_path)
{
	if (!data_segment_addr) {
		return PersistenceRequest::None;
	}

	const auto name = dos_basename(dos_path);

	if (iequals(name, SaveGameFileName)) {
		return PersistenceRequest::Load;
	}

	// The game opens NEWGAME.DBS to start a fresh party, which is the only
	// notice the automap gets that the old map no longer applies.
	if (iequals(name, NewGameFileName)) {
		return PersistenceRequest::NewGame;
	}

	return PersistenceRequest::None;
}

PersistenceRequest RequestForCreatedFile(const std::string_view dos_path)
{
	if (!data_segment_addr) {
		return PersistenceRequest::None;
	}

	// Creating the save is the game overwriting it, so the map is written
	// alongside it. NEWGAME.DBS is not watched for here: the game only ever
	// reads it.
	return iequals(dos_basename(dos_path), SaveGameFileName)
	             ? PersistenceRequest::Save
	             : PersistenceRequest::None;
}

std::span<const Note> NotesOnLevel(const int level)
{
	if (!level_is_valid(level)) {
		return {};
	}

	return notes[level];
}

const Note* FindNote(const int level, const int quadrant, const int x, const int y)
{
	if (!level_is_valid(level)) {
		return nullptr;
	}

	auto& level_notes = notes[level];
	const auto found  = find_note(level_notes, quadrant, x, y);

	return (found == level_notes.end()) ? nullptr : &*found;
}

void SetNoteText(const int level, const int quadrant, const int x, const int y,
                 const std::u16string_view text)
{
	if (!level_is_valid(level) || quadrant < 0 || quadrant >= QuadrantCount ||
	    x < 0 || x >= QuadrantSize || y < 0 || y >= QuadrantSize) {
		return;
	}

	auto& level_notes = notes[level];
	const auto found  = find_note(level_notes, quadrant, x, y);

	if (text.empty()) {
		if (found != level_notes.end()) {
			level_notes.erase(found);
		}
		return;
	}

	if (found != level_notes.end()) {
		found->text = text;
		return;
	}

	level_notes.push_back({.quadrant = quadrant,
	                       .x        = x,
	                       .y        = y,
	                       .colour   = DefaultNoteColour,
	                       .text     = std::u16string(text)});
}

void SetNoteColour(const int level, const int quadrant, const int x,
                   const int y, const uint32_t colour)
{
	if (!level_is_valid(level)) {
		return;
	}

	auto& level_notes = notes[level];
	const auto found  = find_note(level_notes, quadrant, x, y);

	if (found != level_notes.end()) {
		found->colour = colour;
	}
}

std::span<uint32_t> CustomPalette()
{
	return palette;
}

void ApplyPersistence(const PersistenceRequest request)
{
	switch (request) {
	case PersistenceRequest::None: return;

	case PersistenceRequest::NewGame:
		clear_cache();
		clear_notes();
		return;

	case PersistenceRequest::Load:
		// Anything that cannot be read back is dropped rather than half
		// applied, so a corrupt file costs the explored map and nothing
		// worse.
		clear_cache();

		if (!load_file(CacheFileName, [](const uint16_t handle) {
			    return transfer_cache(handle, read_exactly);
		    })) {
			level_cache = {};
		}

		if (!load_file(VisibilityFileName, [](const uint16_t handle) {
			    return read_exactly(handle, visibility_bytes());
		    })) {
			visibility = {};
		}

		clear_notes();

		if (!load_file(NotesFileName, read_notes)) {
			notes = {};
		}

		if (!load_file(PaletteFileName, [](const uint16_t handle) {
			    return read_exactly(handle, palette_bytes());
		    })) {
			palette = {};
		}

		return;

	case PersistenceRequest::Save:
		save_file(CacheFileName, CacheBackupFileName, [](const uint16_t handle) {
			return transfer_cache(handle, write_exactly);
		});

		save_file(VisibilityFileName,
		          VisibilityBackupName,
		          [](const uint16_t handle) {
			          return write_exactly(handle, visibility_bytes());
		          });

		save_file(NotesFileName, NotesBackupName, write_notes);

		save_file(PaletteFileName, PaletteBackupName, [](const uint16_t handle) {
			return write_exactly(handle, palette_bytes());
		});

		return;
	}
}

} // namespace wiz6
