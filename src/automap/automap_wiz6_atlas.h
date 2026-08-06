// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_WIZ6_ATLAS_H
#define DOSBOX_AUTOMAP_WIZ6_ATLAS_H

#include <array>
#include <cstdint>

namespace wiz6 {

// Every tile the automap draws comes from one 256x16 sheet.
constexpr int AtlasWidth  = 256;
constexpr int AtlasHeight = 16;

constexpr int AtlasPixelCount = AtlasWidth * AtlasHeight;

// Authored as 0x00RRGGBB, with no alpha. See the note in the .cpp before
// trusting any colour in here to be the colour that reaches the screen.
extern const std::array<uint32_t, AtlasPixelCount> TileAtlas;

} // namespace wiz6

#endif // DOSBOX_AUTOMAP_WIZ6_ATLAS_H
