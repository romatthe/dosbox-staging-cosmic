// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_WIZ6_H
#define DOSBOX_AUTOMAP_WIZ6_H

#include <cstdint>
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

} // namespace wiz6

#endif // DOSBOX_AUTOMAP_WIZ6_H
