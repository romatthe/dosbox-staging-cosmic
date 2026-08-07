// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap.h"

#include <array>
#include <cassert>
#include <optional>
#include <string>

#include "automap_wiz6.h"
#include "automap_wiz6_render.h"
#include "config/setup.h"
#include "dosbox.h"
#include "dosbox_config.h"
#include "utils/checks.h"
#include "utils/math_utils.h"

// must be included after dosbox_config.h
#include <SDL3/SDL.h>

CHECK_NARROWING();

// Set to 1 to log the party's position and its surroundings once a second. The
// state model has no other visible output until the map itself is drawn, so
// this is how it gets checked against the running game.
#define AUTOMAP_LOG_STATE 0

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

	// Streaming texture the module's surface is uploaded into, and the size
	// it was created at, so a differently sized surface reallocates it.
	SDL_Texture* texture = nullptr;

	struct {
		int width  = 0;
		int height = 0;
	} texture_size = {};

	// The level the window title currently names, if any.
	std::optional<int> titled_level = {};

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

constexpr auto WindowTitle = "DOSBox Staging Automap";

void read_settings(const SectionProp& section)
{
	automap.settings = {section.GetBool("automap_enabled"),
	                    section.GetBool("automap_hide_in_dark_zones"),
	                    section.GetInt("automap_window_width"),
	                    section.GetInt("automap_window_height")};

	wiz6::SetHideInDarkZones(automap.settings.hide_in_dark_zones);
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

	automap.window = SDL_CreateWindow(WindowTitle,
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

	if (!wiz6::InitTileAtlas()) {
		LOG_WARNING("AUTOMAP: Continuing without tiles; the map will be blank");
	}

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

// The level name is the only label the automap has -- there is no text
// rendering -- so the title bar is where it goes.
void update_window_title(const std::optional<wiz6::PartyPosition>& position)
{
	assert(automap.window);

	const auto level = position ? std::optional(position->level) : std::nullopt;

	// This runs once per emulated frame, and setting the title is a round
	// trip to the window manager.
	if (level == automap.titled_level) {
		return;
	}
	automap.titled_level = level;

	auto title = std::string(WindowTitle);

	if (level) {
		title += " - " + std::to_string(*level) + ": " +
		         std::string(wiz6::LevelName(*level));
	}

	SDL_SetWindowTitle(automap.window, title.c_str());
}

#if AUTOMAP_LOG_STATE
char square_char(const wiz6::Square& square, const wiz6::Visibility visibility,
                 const std::optional<wiz6::Facing> party_facing)
{
	if (party_facing) {
		constexpr std::array PartyChars = {'^', '>', 'v', '<'};
		return PartyChars[static_cast<size_t>(*party_facing)];
	}

	if (visibility == wiz6::Visibility::Unseen) {
		return ' ';
	}

	constexpr int Pit = 14;

	if (square.feature == Pit) {
		return 'P';
	}
	if (square.feature != 0) {
		// One hex digit, so the feature's raw value stays visible. This
		// hides the square's visibility, which only matters here.
		return "0123456789ABCDEF"[square.feature];
	}

	// A square only looked into from next door has not been walked yet.
	return visibility == wiz6::Visibility::Visited ? '.' : ':';
}

// Draws the quadrant the party is standing in as text. The map data is packed
// into bit arrays at six different widths, so seeing it come back out as a
// maze -- with the walls where the game's own view says they are -- is what
// confirms the unpacking is right.
void log_current_quadrant(const wiz6::PartyPosition& position)
{
	// The quadrant's own west and south edges belong to its neighbours, so
	// they are left blank.
	for (auto y = wiz6::QuadrantSize - 1; y >= 0; --y) {
		std::string edges   = " ";
		std::string squares = " ";

		for (auto x = 0; x < wiz6::QuadrantSize; ++x) {
			const auto square = wiz6::GetSquare(position.level,
			                                    position.quadrant,
			                                    x,
			                                    y);
			if (!square) {
				return;
			}

			const auto west = x > 0 ? wiz6::GetSquare(position.level,
			                                          position.quadrant,
			                                          x - 1,
			                                          y)
			                        : std::nullopt;

			const auto is_party = x == position.x && y == position.y;

			const auto visibility = wiz6::GetVisibility(
			        position.level, position.quadrant, x, y);

			edges += square->north_wall >= 2 ? "+---" : "+   ";
			squares += west && west->east_wall >= 2 ? "| " : "  ";
			squares += square_char(*square,
			                       visibility,
			                       is_party ? std::optional(position.facing)
			                                : std::nullopt);
			squares += ' ';
		}

		edges += '+';
		squares += ' ';

		LOG_MSG("AUTOMAP: %s", edges.c_str());
		LOG_MSG("AUTOMAP: %s", squares.c_str());
	}
}

void log_state(const std::optional<wiz6::PartyPosition>& position)
{
	constexpr uint64_t LogIntervalMs = 1000;

	static uint64_t last_log_ms = 0;

	if (SDL_GetTicks() - last_log_ms < LogIntervalMs) {
		return;
	}
	last_log_ms = SDL_GetTicks();

	if (!position) {
		LOG_MSG("AUTOMAP: Party is not in the dungeon");
		return;
	}

	constexpr std::array FacingNames = {"north", "east", "south", "west"};

	LOG_MSG("AUTOMAP: Level %d (%s), quadrant %2d, x %d, y %d, facing %s",
	        position->level,
	        std::string(wiz6::LevelName(position->level)).c_str(),
	        position->quadrant,
	        position->x,
	        position->y,
	        FacingNames[static_cast<size_t>(position->facing)]);

	log_current_quadrant(*position);
}
#endif

// Uploads a module-owned surface into the streaming texture, reallocating it
// when the surface changes size. Keeping the map on the CPU side and treating
// the renderer as nothing but a way to get it on screen is what preserves the
// option of compositing the map into the emulator's window later.
bool upload(const SDL_Surface& surface)
{
	assert(automap.renderer);

	if (automap.texture_size.width != surface.w ||
	    automap.texture_size.height != surface.h) {

		if (automap.texture) {
			SDL_DestroyTexture(automap.texture);
		}

		automap.texture = SDL_CreateTexture(automap.renderer,
		                                    surface.format,
		                                    SDL_TEXTUREACCESS_STREAMING,
		                                    surface.w,
		                                    surface.h);
		if (!automap.texture) {
			LOG_WARNING("AUTOMAP: Failed to create texture: %s",
			            SDL_GetError());
			automap.texture_size = {};
			return false;
		}

		// The map is pixel art at a fixed tile size; smoothing it would
		// blur the one-pixel walls into the floor.
		SDL_SetTextureScaleMode(automap.texture, SDL_SCALEMODE_NEAREST);

		automap.texture_size = {surface.w, surface.h};
	}

	return SDL_UpdateTexture(automap.texture, nullptr, surface.pixels, surface.pitch);
}

void present()
{
	assert(automap.renderer);

	SDL_SetRenderDrawColor(automap.renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(automap.renderer);

	// The map is drawn at the window's own pixel size, so it stays 1:1 and
	// the texture only ever scales when the window is mid-resize.
	int width  = 0;
	int height = 0;

	SDL_GetWindowSizeInPixels(automap.window, &width, &height);

	if (const auto* map = wiz6::RenderMap(width, height); map && upload(*map)) {
		SDL_RenderTexture(automap.renderer, automap.texture, nullptr, nullptr);
	}

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

void AUTOMAP_NotifyProgramLoad(const std::string_view name,
                               const uint16_t loadseg, const uint32_t headersize)
{
	if (!automap.settings.enabled) {
		return;
	}

	// Every program load is a candidate, because the guest can quit the
	// game and start it again within one session.
	if (!wiz6::DetectGame(name, loadseg, headersize)) {
		return;
	}

	if (!automap.window) {
		create_window();
	} else {
		SDL_ShowWindow(automap.window);
	}
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

	// The state model has to keep up even while the window is hidden or
	// minimised, or the map would end up with holes wherever the user had
	// it closed.
	const auto position = wiz6::Update();

#if AUTOMAP_LOG_STATE
	log_state(position);
#endif

	update_window_title(position);

	if (!is_window_on_screen()) {
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
	case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
	case SDL_EVENT_WINDOW_SHOWN:
	case SDL_EVENT_WINDOW_RESTORED:
		if (is_window_on_screen()) {
			present();
		}
		break;

	// Dragging with the left button held pans the map. Taking the button
	// state from the event rather than tracking press and release means a
	// release that lands outside the window -- which is never delivered
	// here -- cannot leave the map stuck to the cursor.
	case SDL_EVENT_MOUSE_MOTION:
		if ((event.motion.state & SDL_BUTTON_LMASK) != 0) {
			// Motion arrives in window coordinates while the map is
			// drawn in pixels, and the two differ on a HiDPI display.
			const auto density = SDL_GetWindowPixelDensity(automap.window);

			wiz6::ScrollMap(iroundf(event.motion.xrel * density),
			                iroundf(event.motion.yrel * density));

			// A drag has to keep up with the cursor even when the
			// guest has stopped producing frames, but a redraw is a
			// full pass over the level and a mouse can report far
			// more often than the map needs redrawing. Dropping the
			// last motion of a drag costs nothing, because the pan
			// itself is kept and the next frame draws it.
			if (SDL_GetTicks() - automap.last_present_ms >=
			    MinPresentIntervalMs) {
				present();
			}
		}
		break;

	// Middle-click puts the party back in the middle of the window.
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event.button.button == SDL_BUTTON_MIDDLE) {
			wiz6::RecentreMap();
			present();
		}
		break;

	default: break;
	}
}
