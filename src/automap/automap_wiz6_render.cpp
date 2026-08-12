// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_wiz6_render.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <optional>

#include "automap_wiz6.h"
#include "automap_wiz6_atlas.h"
#include "dosbox.h"
#include "dosbox_config.h"
#include "utils/checks.h"

// must be included after dosbox_config.h
#include <SDL3/SDL.h>

CHECK_NARROWING();

namespace wiz6 {

namespace {

// This is where PORTING.md section 7.4 is decided, and it is the only place.
//
// The atlas is authored as 0x00RRGGBB, and the original uploaded those bytes
// as GL_RGBA on a little-endian machine, which puts the authored blue channel
// into red and vice versa. Reading the same values back as ABGR8888 reproduces
// that exactly, so what users of the original saw is what the automap draws:
// orange doors, a red-outlined party arrow, blue water. ARGB8888 would instead
// give the author's intended colours -- which, going by the water tile being
// authored red, were never what they were looking at either.
constexpr auto AtlasPixelFormat = SDL_PIXELFORMAT_ABGR8888;

// A square is this many pixels on the map, and a level is 256 squares tall.
// Both are baked into the original's arithmetic and its file formats.
constexpr int SquarePx  = 22;
constexpr int LevelRows = 256;

// Walls are thin bars along a square's edge, drawn two pixels longer than the
// square so corners meet rather than leaving a notch.
constexpr int WallPx         = 3;
constexpr int WallOverhangPx = 2;

// Edge values shared by the horizontal and vertical wall arrays.
enum class Edge { None = 0, Passage = 1, Solid = 2, Door = 3 };

// Feature values. Only these three are ever drawn; see the note on Pit below.
constexpr int StairsUpFeature   = 1;
constexpr int StairsDownFeature = 2;
constexpr int FountainFeature   = 4;
constexpr int PortcullisFeature = 7;
constexpr int PitFeature        = 14;

// A rectangle of the tile sheet, plus how it is mirrored on the way out.
struct Tile {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;

	bool flip_h = false;
	bool flip_v = false;
};

// Transcribed from the original's UV pairs (am_wiz6.cpp:203-280). Those encode
// mirroring through coordinate *order* rather than a flag: the larger v is
// passed first when a tile should come out upright, the smaller first when it
// should be mirrored. Nothing else distinguishes stairs-up from stairs-down, or
// the left cursor from the right one -- they are the same pixels, flipped.
constexpr Tile WaterTile      = {0, 0, 7, 7, false, true};
constexpr Tile DarkFloorTile  = {16, 0, 7, 7, false, true};
constexpr Tile VPortcullis    = {42, 0, 3, 9, false, true};
constexpr Tile HPassage       = {48, 0, 9, 3, false, true};
constexpr Tile HPortcullis    = {60, 0, 9, 3, false, true};
constexpr Tile VPassage       = {71, 0, 3, 9, false, true};
constexpr Tile HWall          = {80, 0, 9, 3, false, true};
constexpr Tile VWall          = {103, 0, 3, 9, false, true};
constexpr Tile HDoor          = {112, 0, 9, 3, false, true};
constexpr Tile VDoor          = {135, 0, 3, 9, false, true};
constexpr Tile StairsUpTile   = {142, 0, 13, 13, false, false};
constexpr Tile StairsDownTile = {142, 0, 13, 13, false, true};
constexpr Tile FountainTile   = {189, 0, 16, 15, false, false};
constexpr Tile CursorUp       = {174, 0, 11, 14, false, false};
constexpr Tile CursorDown     = {174, 0, 11, 14, false, true};
constexpr Tile CursorRight    = {157, 2, 14, 11, false, true};
constexpr Tile CursorLeft     = {157, 2, 14, 11, true, true};

// Three things in the sheet are deliberately unused, all inherited from the
// original rather than dropped by this port:
//
//   - The "light floor" tile at x=32 has a draw helper that nothing calls.
//   - `W6_DrawFountainDown` is likewise never called, which is why nobody
//     noticed its `u1 = 105.0f/256.0f` where every sibling has 205.0f
//     (PORTING.md 7.6) -- the tile it names does not exist.
//   - Pits are not drawn: the original comments out `case 14` in its feature
//     pass. There is a 13-pixel-wide tile at x=207-219 that no helper
//     references at all, which is very likely the art for exactly that.
//
// Drawing pits and the light floor would be a small, self-contained addition
// on top of this -- the art is already here and the feature value is already
// decoded -- and worth trying if the original's map ever feels like it is
// missing something. It is left out only so v1 matches the original.

// How far a drag has pushed the map away from the party. The original clamps
// this to a whole level's worth of squares in each direction, which is far
// enough to bring any part of the level under the window from anywhere in it.
constexpr int MaxScrollPx = SquarePx * LevelRows;

SDL_Point scroll_px = {};

// The party's position as of the last frame drawn, so that a move can be
// spotted and the pan dropped.
std::optional<PartyPosition> last_drawn_position = {};

SDL_Surface* map_surface = nullptr;

// The sheet in all four mirrorings, so a flipped tile is a plain blit from a
// different sheet rather than a per-pixel transform. Indexed [flip_h][flip_v].
std::array<std::array<SDL_Surface*, 2>, 2> atlases = {};

SDL_Surface* atlas_for(const Tile& tile)
{
	return atlases[static_cast<size_t>(tile.flip_h)][static_cast<size_t>(tile.flip_v)];
}

// Where a tile's pixels sit once the sheet itself has been mirrored.
SDL_Rect source_rect(const Tile& tile)
{
	const auto x = tile.flip_h ? AtlasWidth - (tile.x + tile.w) : tile.x;
	const auto y = tile.flip_v ? AtlasHeight - (tile.y + tile.h) : tile.y;

	return {x, y, tile.w, tile.h};
}

// Whether a tile's transparent pixels show what is underneath.
//
// The original enables GL_BLEND in exactly one place, around the party cursor
// (am_wiz6.cpp:1025). Every other tile is drawn with blending off, so alpha is
// ignored and the sheet's background pixels are written as opaque black -- the
// gaps between a staircase's rungs are black in the original, not the floor
// showing through. Blending everything looks subtly wrong in a way that is
// hard to place, so this follows the original exactly.
enum class Blending { Off, On };

// `dimmed` is the original's `dark` flag: a square that has been seen from next
// door but not walked through is drawn at half brightness.
void draw_tile(const Tile& tile, const int x, const int y, const int w, const int h,
               const bool dimmed, const Blending blending = Blending::Off)
{
	auto* atlas = atlas_for(tile);

	if (!atlas || !map_surface) {
		return;
	}

	// Cheap reject: SDL would clip these anyway, but a level is 256 squares
	// across and most of it is off the window on any given frame.
	if (x + w <= 0 || y + h <= 0 || x >= map_surface->w || y >= map_surface->h) {
		return;
	}

	constexpr uint8_t HalfBrightness = 128;
	constexpr uint8_t FullBrightness = 255;

	const auto brightness = dimmed ? HalfBrightness : FullBrightness;

	SDL_SetSurfaceColorMod(atlas, brightness, brightness, brightness);

	SDL_SetSurfaceBlendMode(atlas,
	                        blending == Blending::On ? SDL_BLENDMODE_BLEND
	                                                 : SDL_BLENDMODE_NONE);

	auto source          = source_rect(tile);
	SDL_Rect destination = {x, y, w, h};

	SDL_BlitSurfaceScaled(atlas, &source, map_surface, &destination, SDL_SCALEMODE_NEAREST);
}

struct QuadrantLocation {
	int quadrant = 0;
	int x        = 0;
	int y        = 0;
};

std::optional<QuadrantLocation> find_quadrant(const int level, const int abs_x,
                                              const int abs_y)
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

// Whether some quadrant's west column starts exactly here. The original asks
// this eight squares west of a quadrant to find out whether another one
// adjoins it on that side.
bool quadrant_starts_at(const int level, const int abs_x, const int abs_y)
{
	for (auto quadrant = 0; quadrant < QuadrantCount; ++quadrant) {
		const auto origin = GetQuadrantOrigin(level, quadrant);

		if (!origin) {
			continue;
		}

		if (origin->x == abs_x && abs_y >= origin->y &&
		    abs_y < origin->y + QuadrantSize) {
			return true;
		}
	}

	return false;
}

bool is_dimmed(const int level, const int quadrant, const int x, const int y)
{
	return GetVisibility(level, quadrant, x, y) != Visibility::Visited;
}

bool is_seen(const int level, const int quadrant, const int x, const int y)
{
	return GetVisibility(level, quadrant, x, y) != Visibility::Unseen;
}

// Pass 1: the floor. Only squares the party has seen get one, which is what
// makes the map fill in as it explores.
void draw_floors(const int level, const int quadrant, const int origin_x,
                 const int origin_y)
{
	// Water is floor and roof together, and only on the three levels that
	// have any -- game knowledge from the original.
	const auto level_has_water = level == 8 || level == 10 || level == 12;

	for (auto y = 0; y < QuadrantSize; ++y) {
		for (auto x = 0; x < QuadrantSize; ++x) {
			if (!is_seen(level, quadrant, x, y)) {
				continue;
			}

			const auto square = GetSquare(level, quadrant, x, y);

			if (!square) {
				continue;
			}

			const auto px = origin_x + SquarePx * x;
			const auto py = origin_y + SquarePx * (QuadrantSize - 1 - y);
			const auto dimmed = is_dimmed(level, quadrant, x, y);

			if (square->is_dark_floor && square->feature != PitFeature) {
				draw_tile(DarkFloorTile,
				          px + 1,
				          py + 1,
				          SquarePx - 1,
				          SquarePx - 1,
				          dimmed);
			}

			if (level_has_water && !square->is_dark_floor &&
			    (square->has_roof || level == 8)) {
				draw_tile(WaterTile,
				          px + 1,
				          py + 1,
				          SquarePx - 1,
				          SquarePx - 1,
				          dimmed);
			}
		}
	}
}

// An edge is drawn if either of the squares it separates has been seen, and is
// dimmed unless one of them has actually been walked.
struct EdgeVisibility {
	bool draw   = false;
	bool dimmed = false;
};

EdgeVisibility edge_visibility(const int level, const int quadrant, const int x,
                               const int y, const int neighbour_abs_x,
                               const int neighbour_abs_y)
{
	auto dimmed = is_dimmed(level, quadrant, x, y);
	auto seen   = false;

	if (const auto neighbour = find_quadrant(level, neighbour_abs_x, neighbour_abs_y)) {
		seen = is_seen(level,
		               neighbour->quadrant,
		               neighbour->x,
		               neighbour->y);

		dimmed = is_dimmed(level,
		                   neighbour->quadrant,
		                   neighbour->x,
		                   neighbour->y) &&
		         dimmed;
	}

	return {seen || is_seen(level, quadrant, x, y), dimmed};
}

// Pass 2: walls, doors and portcullises. A square owns its north and east
// edges; the quadrant's own west and south edges are drawn separately below,
// but only where no neighbouring quadrant will draw them instead.
void draw_walls(const int level, const int quadrant, const int origin_x,
                const int origin_y)
{
	const auto quadrant_origin = GetQuadrantOrigin(level, quadrant);

	if (!quadrant_origin) {
		return;
	}

	for (auto y = 0; y < QuadrantSize; ++y) {
		for (auto x = 0; x < QuadrantSize; ++x) {
			const auto square = GetSquare(level, quadrant, x, y);

			if (!square) {
				continue;
			}

			const auto px = origin_x + SquarePx * x;
			const auto py = origin_y + SquarePx * (QuadrantSize - 1 - y);

			const auto portcullis = square->feature == PortcullisFeature;

			// East edge.
			const auto east = edge_visibility(level,
			                                  quadrant,
			                                  x,
			                                  y,
			                                  quadrant_origin->x + x + 1,
			                                  quadrant_origin->y + y);
			if (east.draw) {
				const auto edge_x = px + SquarePx;

				switch (static_cast<Edge>(square->east_wall)) {
				case Edge::None: break;

				case Edge::Passage:
					draw_tile(VPassage,
					          edge_x,
					          py,
					          WallPx,
					          SquarePx + WallOverhangPx,
					          east.dimmed);
					break;

				case Edge::Solid:
					draw_tile(portcullis && square->feature_direction == 1
					                  ? VPortcullis
					                  : VWall,
					          edge_x,
					          py,
					          WallPx,
					          SquarePx + WallOverhangPx,
					          east.dimmed);
					break;

				case Edge::Door:
					draw_tile(VDoor,
					          edge_x,
					          py,
					          WallPx,
					          SquarePx + WallOverhangPx,
					          east.dimmed);
					break;
				}
			}

			// North edge.
			const auto north = edge_visibility(level,
			                                   quadrant,
			                                   x,
			                                   y,
			                                   quadrant_origin->x + x,
			                                   quadrant_origin->y + y + 1);
			if (north.draw) {
				switch (static_cast<Edge>(square->north_wall)) {
				case Edge::None: break;

				case Edge::Passage:
					draw_tile(HPassage,
					          px,
					          py,
					          SquarePx + WallOverhangPx,
					          WallPx,
					          north.dimmed);
					break;

				case Edge::Solid:
					draw_tile(portcullis && square->feature_direction == 0
					                  ? HPortcullis
					                  : HWall,
					          px,
					          py,
					          SquarePx + WallOverhangPx,
					          WallPx,
					          north.dimmed);
					break;

				case Edge::Door:
					draw_tile(HDoor,
					          px,
					          py,
					          SquarePx + WallOverhangPx,
					          WallPx,
					          north.dimmed);
					break;
				}
			}
		}
	}

	// The quadrant's outer west and south edges. The game does not store
	// them, so they are drawn as solid wall wherever no other quadrant
	// adjoins -- otherwise the map would have open sides.
	for (auto i = 0; i < QuadrantSize; ++i) {
		if (is_seen(level, quadrant, 0, i) &&
		    !quadrant_starts_at(level,
		                        quadrant_origin->x - QuadrantSize,
		                        quadrant_origin->y + i)) {

			draw_tile(VWall,
			          origin_x,
			          origin_y + SquarePx * (QuadrantSize - 1 - i),
			          WallPx,
			          SquarePx + WallOverhangPx,
			          is_dimmed(level, quadrant, 0, i));
		}

		if (is_seen(level, quadrant, i, 0) &&
		    !find_quadrant(level,
		                   quadrant_origin->x + i,
		                   quadrant_origin->y - 1)) {

			draw_tile(HWall,
			          origin_x + SquarePx * i,
			          origin_y + SquarePx * QuadrantSize,
			          SquarePx + WallOverhangPx,
			          WallPx,
			          is_dimmed(level, quadrant, i, 0));
		}
	}
}

// Pass 3: stairs and fountains. Stairs are nudged towards the side of the
// square they face; the offsets are the original's.
void draw_features(const int level, const int quadrant, const int origin_x,
                   const int origin_y)
{
	constexpr std::array<SDL_Point, 4> DirectionOffsets = {
	        SDL_Point{        0, -SquarePx},
	        SDL_Point{ SquarePx,         0},
	        SDL_Point{        0,  SquarePx},
	        SDL_Point{-SquarePx,         0},
	};

	for (auto y = 0; y < QuadrantSize; ++y) {
		for (auto x = 0; x < QuadrantSize; ++x) {
			if (!is_seen(level, quadrant, x, y)) {
				continue;
			}

			const auto square = GetSquare(level, quadrant, x, y);

			if (!square) {
				continue;
			}

			const auto px = origin_x + SquarePx * x;
			const auto py = origin_y + SquarePx * (QuadrantSize - 1 - y);
			const auto dimmed = is_dimmed(level, quadrant, x, y);

			const auto& offset = DirectionOffsets[static_cast<size_t>(
			        square->feature_direction & 3)];

			switch (square->feature) {
			case StairsUpFeature:
				draw_tile(StairsUpTile,
				          px + 3 + offset.x,
				          py + 3 + offset.y,
				          SquarePx - 3,
				          SquarePx - 3,
				          dimmed);
				break;

			case StairsDownFeature:
				draw_tile(StairsDownTile,
				          px + 3 + offset.x,
				          py + 3 + offset.y,
				          SquarePx - 3,
				          SquarePx - 3,
				          dimmed);
				break;

			case FountainFeature:
				draw_tile(FountainTile,
				          px + 3,
				          py + 3,
				          SquarePx - 5,
				          SquarePx - 5,
				          dimmed);
				break;

			default: break;
			}
		}
	}
}

// Pass 4: the party. The original's fourth pass also drew notes, which this
// port does not implement.
// A note's box is the one thing on the map that is not a blit from the tile
// sheet. The original draws it as four GL lines (am_wiz6.cpp:986); this fills
// four rectangles instead, which differs only in that a wide GL line straddles
// its path by half its width while these are drawn inward. At three pixels on
// a 22-pixel square that is a one-pixel difference on each edge, and there is
// no reference to compare it against: the Linux reference build cannot create
// notes at all, so Phase 3's pixel comparison has no equivalent here.
void draw_box(const int x, const int y, const int w, const int h,
              const int thickness, const uint32_t colour)
{
	if (!map_surface) {
		return;
	}

	if (x + w <= 0 || y + h <= 0 || x >= map_surface->w || y >= map_surface->h) {
		return;
	}

	// The original passes the colour to glColor3f, which ignores alpha, so
	// whatever is stored in the note's top byte never reaches the screen.
	constexpr uint32_t OpaqueAlpha = 0xff000000;

	const std::array<SDL_Rect, 4> edges = {
	        {
                 {x, y, w, thickness},
                 {x, y + h - thickness, w, thickness},
                 {x, y, thickness, h},
                 {x + w - thickness, y, thickness, h},
	         }
        };

	for (const auto& edge : edges) {
		SDL_FillSurfaceRect(map_surface, &edge, colour | OpaqueAlpha);
	}
}

// The box is inset within its square, and a pixel shorter than it is wide.
// Both come from the original, which has no stated reason for the asymmetry.
constexpr int NoteBoxInsetPx     = 5;
constexpr int NoteBoxThicknessPx = 3;
constexpr int NoteBoxWidthPx     = SquarePx - 6;
constexpr int NoteBoxHeightPx    = SquarePx - 7;

// Notes are drawn wherever they are, without consulting visibility: a square
// the party has never seen still shows its note. That is the original's
// behaviour and it is the useful one, since a note is often put somewhere to
// mark a place worth going back to.
void draw_notes(const int level, const int quadrant, const int origin_x,
                const int origin_y)
{
	for (const auto& note : NotesOnLevel(level)) {
		if (note.quadrant != quadrant) {
			continue;
		}

		const auto px = origin_x + SquarePx * note.x;
		const auto py = origin_y + SquarePx * (QuadrantSize - 1 - note.y);

		draw_box(px + NoteBoxInsetPx,
		         py + NoteBoxInsetPx,
		         NoteBoxWidthPx,
		         NoteBoxHeightPx,
		         NoteBoxThicknessPx,
		         note.colour);
	}
}

void draw_party(const PartyPosition& position, const int origin_x, const int origin_y)
{
	if (IsDarkZone(position.level, position.quadrant, position.x, position.y)) {
		return;
	}

	const auto px = origin_x + SquarePx * position.x;
	const auto py = origin_y + SquarePx * (QuadrantSize - 1 - position.y);

	const auto& cursor = position.facing == Facing::North ? CursorUp
	                   : position.facing == Facing::East  ? CursorRight
	                   : position.facing == Facing::West  ? CursorLeft
	                                                      : CursorDown;

	// The one blended tile, so the floor shows around the arrow.
	draw_tile(cursor, px, py, SquarePx, SquarePx, false, Blending::On);
}

// Whether there is a map to look at at all. The original blanks the whole map
// while the party is out of the dungeon or standing in a dark zone, rather than
// hiding the squares it cannot show.
bool is_map_visible(const std::optional<PartyPosition>& position)
{
	return position.has_value() && !IsDarkZone(position->level,
	                                           position->quadrant,
	                                           position->x,
	                                           position->y);
}

bool resize_map_surface(const int width_px, const int height_px)
{
	if (map_surface && map_surface->w == width_px && map_surface->h == height_px) {
		return true;
	}

	if (map_surface) {
		SDL_DestroySurface(map_surface);
	}

	map_surface = SDL_CreateSurface(width_px, height_px, AtlasPixelFormat);

	if (!map_surface) {
		LOG_WARNING("AUTOMAP: Failed to create the map surface: %s",
		            SDL_GetError());
	}

	return map_surface != nullptr;
}

} // namespace

bool InitTileAtlas()
{
	if (atlases[0][0]) {
		return true;
	}

	auto* unflipped = SDL_CreateSurface(AtlasWidth, AtlasHeight, AtlasPixelFormat);

	if (!unflipped) {
		LOG_WARNING("AUTOMAP: Failed to create the tile atlas surface: %s",
		            SDL_GetError());
		return false;
	}

	// The atlas is authored without alpha, so every pixel needs it added.
	// Colour 0 is the sheet's background and must stay transparent, or the
	// tiles would be drawn as opaque rectangles over each other.
	constexpr uint32_t Opaque = 0xFF000000;

	auto* pixels = static_cast<uint8_t*>(unflipped->pixels);

	for (auto y = 0; y < AtlasHeight; ++y) {
		auto* row = reinterpret_cast<uint32_t*>(pixels + y * unflipped->pitch);

		for (auto x = 0; x < AtlasWidth; ++x) {
			const auto colour =
			        TileAtlas[static_cast<size_t>(y * AtlasWidth + x)];

			row[x] = colour == 0 ? 0 : colour | Opaque;
		}
	}

	// Mirrored copies, so drawing a flipped tile stays a plain blit.
	for (auto flip_h = 0; flip_h < 2; ++flip_h) {
		for (auto flip_v = 0; flip_v < 2; ++flip_v) {
			auto* copy = SDL_DuplicateSurface(unflipped);

			if (!copy) {
				LOG_WARNING("AUTOMAP: Failed to copy the tile atlas: %s",
				            SDL_GetError());
				FreeTileAtlas();
				SDL_DestroySurface(unflipped);
				return false;
			}

			if (flip_h) {
				SDL_FlipSurface(copy, SDL_FLIP_HORIZONTAL);
			}
			if (flip_v) {
				SDL_FlipSurface(copy, SDL_FLIP_VERTICAL);
			}

			// Set per draw; see the Blending note above.

			atlases[static_cast<size_t>(flip_h)][static_cast<size_t>(flip_v)] = copy;
		}
	}

	SDL_DestroySurface(unflipped);

	return true;
}

void FreeTileAtlas()
{
	for (auto& row : atlases) {
		for (auto& atlas : row) {
			if (atlas) {
				SDL_DestroySurface(atlas);
				atlas = nullptr;
			}
		}
	}

	if (map_surface) {
		SDL_DestroySurface(map_surface);
		map_surface = nullptr;
	}
}

SDL_Surface* RenderMap(const int width_px, const int height_px)
{
	const auto position = GetPartyPosition();

	if (!atlases[0][0] || !is_map_visible(position)) {
		return nullptr;
	}

	// The original drops any pan the moment the party moves or turns
	// (am_wiz6.cpp:1083). Without this the view would stay offset exactly
	// while the party is walking, which is when it most needs to be centred.
	if (position != last_drawn_position) {
		last_drawn_position = position;
		scroll_px           = {};
	}

	if (!resize_map_surface(width_px, height_px)) {
		return nullptr;
	}

	SDL_ClearSurface(map_surface, 0.0f, 0.0f, 0.0f, 1.0f);

	const auto party_origin = GetQuadrantOrigin(position->level,
	                                            position->quadrant);

	if (!party_origin) {
		return nullptr;
	}

	// Centre the party's square in the window, then shift by however far
	// the user has dragged. The Y term counts down from the top of the
	// level because the game's Y axis points north while the surface's
	// points south.
	const auto party_abs_x = party_origin->x + position->x;
	const auto party_abs_y = party_origin->y + position->y;

	const auto view_x = scroll_px.x - (party_abs_x * SquarePx -
	                                   (width_px / 2 - SquarePx / 2));
	const auto view_y = scroll_px.y - ((LevelRows - 1 + QuadrantSize) * SquarePx -
	                                   party_abs_y * SquarePx -
	                                   (height_px / 2 - SquarePx / 2));

	// Five passes over all twelve quadrants, in this order, because later
	// passes are meant to draw over earlier ones: floors, then the walls
	// that bound them, then what stands on them, then the player's own
	// notes, then the party.
	const auto quadrant_origin_px = [&](const int quadrant, int& out_x, int& out_y) {
		const auto origin = GetQuadrantOrigin(position->level, quadrant);

		if (!origin) {
			return false;
		}

		out_x = view_x + origin->x * SquarePx;
		out_y = view_y + (LevelRows - 1) * SquarePx - origin->y * SquarePx;

		return true;
	};

	for (auto quadrant = 0; quadrant < QuadrantCount; ++quadrant) {
		int x = 0;
		int y = 0;

		if (quadrant_origin_px(quadrant, x, y)) {
			draw_floors(position->level, quadrant, x, y);
		}
	}

	for (auto quadrant = 0; quadrant < QuadrantCount; ++quadrant) {
		int x = 0;
		int y = 0;

		if (quadrant_origin_px(quadrant, x, y)) {
			draw_walls(position->level, quadrant, x, y);
		}
	}

	for (auto quadrant = 0; quadrant < QuadrantCount; ++quadrant) {
		int x = 0;
		int y = 0;

		if (quadrant_origin_px(quadrant, x, y)) {
			draw_features(position->level, quadrant, x, y);
		}
	}

	for (auto quadrant = 0; quadrant < QuadrantCount; ++quadrant) {
		int x = 0;
		int y = 0;

		if (quadrant_origin_px(quadrant, x, y)) {
			draw_notes(position->level, quadrant, x, y);
		}
	}

	int party_x = 0;
	int party_y = 0;

	if (quadrant_origin_px(position->quadrant, party_x, party_y)) {
		draw_party(*position, party_x, party_y);
	}

	return map_surface;
}

void ScrollMap(const int delta_x_px, const int delta_y_px)
{
	// The original refuses to pan a map it is not drawing
	// (am_wiz6.cpp:1531), so a drag nobody can see cannot leave the view
	// offset for whenever the party walks back into the light.
	if (!is_map_visible(GetPartyPosition())) {
		return;
	}

	scroll_px.x = std::clamp(scroll_px.x + delta_x_px, -MaxScrollPx, MaxScrollPx);
	scroll_px.y = std::clamp(scroll_px.y + delta_y_px, -MaxScrollPx, MaxScrollPx);
}

void RecentreMap()
{
	scroll_px = {};
}

} // namespace wiz6
