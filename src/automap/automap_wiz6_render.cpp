// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_wiz6_render.h"

#include <cassert>

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
// orange doors, a red-outlined party arrow. ARGB8888 here would instead give
// the author's intended colours -- blue doors -- which nobody has ever seen.
constexpr auto AtlasPixelFormat = SDL_PIXELFORMAT_ABGR8888;

SDL_Surface* tile_atlas = nullptr;

} // namespace

bool InitTileAtlas()
{
	if (tile_atlas) {
		return true;
	}

	tile_atlas = SDL_CreateSurface(AtlasWidth, AtlasHeight, AtlasPixelFormat);

	if (!tile_atlas) {
		LOG_WARNING("AUTOMAP: Failed to create the tile atlas surface: %s",
		            SDL_GetError());
		return false;
	}

	// The atlas is authored without alpha, so every pixel needs it added.
	// Colour 0 is the sheet's background and must stay transparent, or the
	// tiles would be drawn as opaque rectangles over each other.
	constexpr uint32_t Opaque      = 0xFF000000;
	constexpr uint32_t Transparent = 0x00000000;

	auto* pixels = static_cast<uint8_t*>(tile_atlas->pixels);

	for (auto y = 0; y < AtlasHeight; ++y) {
		auto* row = reinterpret_cast<uint32_t*>(pixels + y * tile_atlas->pitch);

		for (auto x = 0; x < AtlasWidth; ++x) {
			const auto colour =
			        TileAtlas[static_cast<size_t>(y * AtlasWidth + x)];

			row[x] = colour | (colour == 0 ? Transparent : Opaque);
		}
	}

	return true;
}

void FreeTileAtlas()
{
	if (tile_atlas) {
		SDL_DestroySurface(tile_atlas);
		tile_atlas = nullptr;
	}
}

SDL_Surface* GetTileAtlas()
{
	return tile_atlas;
}

} // namespace wiz6
