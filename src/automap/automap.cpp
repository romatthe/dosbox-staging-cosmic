// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap.h"

#include <cassert>

#include "config/setup.h"
#include "dosbox.h"
#include "dosbox_config.h"
#include "utils/checks.h"

// must be included after dosbox_config.h
#include <SDL3/SDL.h>

CHECK_NARROWING();

namespace {

struct AutomapSettings {
	bool enabled            = false;
	bool hide_in_dark_zones = true;
	int window_width_px     = 512;
	int window_height_px    = 512;
};

struct AutomapState {
	AutomapSettings settings = {};

	// Created lazily on first game detection, then kept for the lifetime
	// of the process. Closing the window hides it rather than destroying
	// it, so it can be shown again.
	SDL_Window* window = nullptr;
};

AutomapState automap = {};

void read_settings(const SectionProp& section)
{
	automap.settings = {section.GetBool("automap_enabled"),
	                    section.GetBool("automap_hide_in_dark_zones"),
	                    section.GetInt("automap_window_width"),
	                    section.GetInt("automap_window_height")};
}

void notify_setting_updated(SectionProp& section,
                            [[maybe_unused]] const std::string& prop_name)
{
	read_settings(section);
}

void init_config_settings(SectionProp& section)
{
	using enum Property::Changeable::Value;

	auto bool_prop = section.AddBool("automap_enabled", OnlyAtStart, false);
	assert(bool_prop);
	bool_prop->SetHelp(
	        "Enable the Wizardry VI automap ('off' by default). When\n"
	        "enabled, a second window shows the explored map of the current\n"
	        "dungeon level while 'Wizardry VI: Bane of the Cosmic Forge' is\n"
	        "running. The window has no effect on any other program.");

	bool_prop = section.AddBool("automap_hide_in_dark_zones", WhenIdle, true);
	assert(bool_prop);
	bool_prop->SetHelp(
	        "Hide the automap in dungeon areas the game treats as dark ('on'\n"
	        "by default). Turn this off to map those areas regardless.");

	auto int_prop = section.AddInt("automap_window_width", OnlyAtStart, 512);
	assert(int_prop);
	int_prop->SetMinMax(2, 4096);
	int_prop->SetHelp(
	        "Initial width of the automap window in pixels (512 by default).");

	int_prop = section.AddInt("automap_window_height", OnlyAtStart, 512);
	assert(int_prop);
	int_prop->SetMinMax(2, 4096);
	int_prop->SetHelp(
	        "Initial height of the automap window in pixels (512 by default).");
}

void create_window()
{
	assert(!automap.window);

	automap.window = SDL_CreateWindow("DOSBox Staging Automap",
	                                  automap.settings.window_width_px,
	                                  automap.settings.window_height_px,
	                                  SDL_WINDOW_RESIZABLE);

	if (!automap.window) {
		LOG_WARNING("AUTOMAP: Failed to create window: %s", SDL_GetError());
	}
}

void clear_window()
{
	assert(automap.window);

	auto* surface = SDL_GetWindowSurface(automap.window);
	if (!surface) {
		return;
	}

	const auto black = SDL_MapSurfaceRGB(surface, 0, 0, 0);

	SDL_FillSurfaceRect(surface, nullptr, black);
	SDL_UpdateWindowSurface(automap.window);
}

} // namespace

void AUTOMAP_AddConfigSection(const ConfigPtr& conf)
{
	assert(conf);

	auto section = conf->AddSection("automap");
	section->AddUpdateHandler(notify_setting_updated);

	init_config_settings(*section);
}

void AUTOMAP_Init()
{
	read_settings(*get_section("automap"));
}

void AUTOMAP_NotifyProgramLoad([[maybe_unused]] const std::string_view name,
                               [[maybe_unused]] const uint16_t loadseg,
                               [[maybe_unused]] const uint32_t headersize)
{
	if (!automap.settings.enabled || automap.window) {
		return;
	}

	// TODO Phase 2: only create the window once the loaded image has been
	// confirmed to be Wizardry VI via its "WMAZE" signature.
	create_window();
}

void AUTOMAP_NotifyFileOpened([[maybe_unused]] const std::string_view dos_path)
{
	// TODO Phase 4: load MAP.CAC / MAP.VIS on SAVEGAME.DBS, zero them on
	// NEWGAME.DBS.
}

void AUTOMAP_NotifyFileCreated([[maybe_unused]] const std::string_view dos_path)
{
	// TODO Phase 4: write MAP.CAC / MAP.VIS on SAVEGAME.DBS.
}

void AUTOMAP_MaybeRender()
{
	if (!automap.window) {
		return;
	}

	// TODO Phase 3: draw the map into a module-owned surface and blit it
	// here.
	clear_window();
}

bool AUTOMAP_IsOwnEvent(const SDL_Event& event)
{
	if (!automap.window) {
		return false;
	}

	const auto automap_window_id = SDL_GetWindowID(automap.window);
	const auto event_window      = SDL_GetWindowFromEvent(&event);
	const auto event_window_id   = SDL_GetWindowID(event_window);

	return event_window_id == automap_window_id;
}

void AUTOMAP_HandleEvent(const SDL_Event& event)
{
	assert(automap.window);

	if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
		SDL_HideWindow(automap.window);
	}
}
