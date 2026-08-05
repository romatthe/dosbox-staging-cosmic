// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_H
#define DOSBOX_AUTOMAP_H

#include "config/config.h"

#include <cstdint>
#include <string_view>

union SDL_Event;

// Live automap for Wizardry VI: Bane of the Cosmic Forge, ported from the
// DOSBox 0.74 automap mod by KoriTama.
//
// The automap polls a handful of well-known addresses in the game's data
// segment once per rendered frame and draws the result into a second window.
// It never hooks the CPU or traps memory writes, so emulation timing is
// untouched.
//
// Every function below must be called from the main thread only.

void AUTOMAP_AddConfigSection(const ConfigPtr& conf);
void AUTOMAP_Init();

// Called when the guest executes a program, before its PSP is set up.
// `headersize` is in bytes.
void AUTOMAP_NotifyProgramLoad(const std::string_view name,
                               const uint16_t loadseg,
                               const uint32_t headersize);

// Called for every file the guest opens or creates, so both are on hot paths
// and must stay cheap. `dos_path` is the DOS-canonical path.
void AUTOMAP_NotifyFileOpened(const std::string_view dos_path);
void AUTOMAP_NotifyFileCreated(const std::string_view dos_path);

void AUTOMAP_MaybeRender();

bool AUTOMAP_IsOwnEvent(const SDL_Event& event);
void AUTOMAP_HandleEvent(const SDL_Event& event);

#endif // DOSBOX_AUTOMAP_H
