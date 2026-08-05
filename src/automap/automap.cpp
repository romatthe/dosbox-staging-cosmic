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
	SDL_Window* window     = nullptr;
	SDL_Renderer* renderer = nullptr;

	uint64_t last_present_ms = 0;
};

AutomapState automap = {};

// The automap must never present through OpenGL.
// `src/gui/render/opengl_renderer.cpp` creates the emulator window's GL context
// once and never re-binds it, so anything that makes a different context current
// silently kills the emulator's rendering. Both obvious approaches do exactly
// that:
//
//   - `SDL_GetWindowSurface()` is not the plain blit it looks like. SDL only has
//     a native software framebuffer for some video drivers -- x11 has one,
//     **wayland has none** -- and otherwise falls back to
//     `SDL_CreateWindowTexture()`, which creates a full `SDL_Renderer` and
//     therefore a GL context (`SDL_video.c`, `ShouldAttemptTextureFramebuffer`).
//   - `SDL_CreateRenderer(window, nullptr)` picks the first available driver,
//     which on Linux is OpenGL.
//
// SDL_GPU (Vulkan/Metal/D3D12 underneath) touches no GL state, which is why the
// debugger window uses it too -- see `src/debugger/debugger_gui.cpp`.
constexpr auto RendererDriver = "gpu";

// GFX_EndUpdate can run at the emulated refresh rate. A map redraw does not
// need to be faster than this.
constexpr uint64_t MinPresentIntervalMs = 33;

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
		return;
	}

	automap.renderer = SDL_CreateRenderer(automap.window, RendererDriver);

	if (!automap.renderer) {
		// Falling back to another driver would risk taking the OpenGL
		// one and breaking the emulator's rendering, so refuse instead.
		LOG_WARNING("AUTOMAP: No '%s' renderer available (%s); disabling the automap",
		            RendererDriver,
		            SDL_GetError());

		SDL_DestroyWindow(automap.window);
		automap.window = nullptr;
		return;
	}

	// The emulator's main thread is waiting behind every present.
	SDL_SetRenderVSync(automap.renderer, 0);

	LOG_MSG("AUTOMAP: Opened automap window using the '%s' renderer",
	        SDL_GetRendererName(automap.renderer));
}

// A window that is not on screen may never receive a frame callback from the
// compositor, and presenting to it can then stall the caller -- which here is
// the emulator's main thread.
bool is_window_on_screen()
{
	assert(automap.window);

	constexpr auto OffScreen = SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED;

	return (SDL_GetWindowFlags(automap.window) & OffScreen) == 0;
}

void present()
{
	assert(automap.renderer);

	// TODO Phase 3: upload the module-owned map surface into a streaming
	// texture and draw it here. Keeping the map in a CPU-side surface is
	// what preserves the option of compositing it into the emulator window
	// later; this renderer only presents it.
	SDL_SetRenderDrawColor(automap.renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(automap.renderer);
	SDL_RenderPresent(automap.renderer);

	automap.last_present_ms = SDL_GetTicks();
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
	if (!automap.window || !is_window_on_screen()) {
		return;
	}

	if (SDL_GetTicks() - automap.last_present_ms < MinPresentIntervalMs) {
		return;
	}

	present();
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

	switch (event.type) {
	case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
		SDL_HideWindow(automap.window);
		break;

	// Repaint straight away rather than waiting for the next emulated
	// frame. A resize invalidates the window surface and leaves the newly
	// exposed area undefined, so deferring it shows garbage; and while the
	// user drags the window edge the emulator may not be producing frames
	// at all, which is what makes a resize look like it only lands on
	// mouse release.
	case SDL_EVENT_WINDOW_EXPOSED:
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_SHOWN:
	case SDL_EVENT_WINDOW_RESTORED:
		if (is_window_on_screen()) {
			present();
		}
		break;

	default: break;
	}
}
