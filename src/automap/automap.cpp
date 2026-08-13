// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap.h"

#include <array>
#include <cassert>
#include <optional>
#include <string>

#include "automap_overlay.h"
#include "automap_wiz6.h"
#include "automap_wiz6_coords.h"
#include "automap_wiz6_render.h"
#include "config/setup.h"
#include "dosbox.h"
#include "dosbox_config.h"
#include "gui/common.h"
#include "utils/checks.h"
#include "utils/math_utils.h"
#include "utils/string_utils.h"

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

	// What the guest's last touch of its save file calls for, and whether
	// one is being carried out. Both belong to the deferral described at
	// AUTOMAP_NotifyFileOpened.
	wiz6::PersistenceRequest pending_persistence = wiz6::PersistenceRequest::None;

	bool applying_persistence = false;

	// How far the pointer has travelled since the left button went down.
	// Letting go at the end of a pan must not also open the note editor,
	// and a mouse never holds perfectly still, so this is compared against
	// a small threshold rather than against zero.
	float left_drag_px = 0.0f;

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

// GFX_EndUpdate can run at the emulated refresh rate. The map itself only
// changes when the party moves, so it does not need redrawing faster than
// this.
//
// Note what this interval really buys, because it is not 30 frames a second:
// AUTOMAP_MaybeRender only runs on an emulated frame, so the wait rounds up to
// a whole number of them. At Wizardry VI's 70 Hz that is every third frame,
// 42.9 ms, a measured 24 redraws a second.
constexpr uint64_t MinPresentIntervalMs = 33;

// That is far too slow for anything the pointer is moving. A tooltip follows
// the cursor and a dialog gets dragged around, and at 24 Hz both visibly lag
// behind the mouse. While the overlay has something on screen the window is
// repainted at up to this instead -- and from the mouse events themselves,
// which arrive much more often than emulated frames do, rather than only from
// the frame tick.
//
// It is a deliberate trade rather than a free win: a redraw rasterises the
// whole map and uploads it, which measures 0.7 ms of processor time here, and
// it is the emulator's main thread that spends it.
constexpr uint64_t InteractivePresentIntervalMs = 16;

constexpr auto WindowTitle = "DOSBox Staging Automap";

// How far the pointer may travel between a left button going down and coming
// up and still count as a click rather than a pan. The original makes no such
// distinction and pops its note editor at the end of every drag; suppressing
// that is the one deliberate departure in the mouse bindings.
constexpr auto DragThresholdPx = 4.0f;

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

	// Without this the automap opens *behind* the emulator window, which is
	// worse than untidy: the pointer is then over the emulator, its motion
	// events carry that window's ID, and the pan and recentre controls look
	// broken until the user alt-tabs.
	//
	// Making it a child also minimises and restores it with the emulator. A
	// window the user has closed stays closed, because SDL only restores
	// children whose hidden status it set itself.
	if (auto* main_window = GFX_GetWindow()) {
		if (!SDL_SetWindowParent(automap.window, main_window)) {
			LOG_WARNING("AUTOMAP: Could not keep the window above the emulator: %s",
			            SDL_GetError());
		}
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

	if (!overlay::Init(automap.window, automap.renderer)) {
		LOG_WARNING("AUTOMAP: Continuing without the overlay");
	}

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

	overlay::Draw(automap.renderer);

	SDL_RenderPresent(automap.renderer);

	automap.last_present_ms = SDL_GetTicks();
}

// How long to wait between repaints, given what is on screen.
uint64_t present_interval_ms()
{
	return overlay::IsShowingSomething() ? InteractivePresentIntervalMs
	                                     : MinPresentIntervalMs;
}

// Repaints if enough time has passed. Used by everything that wants the window
// to keep up with the pointer.
void present_if_due()
{
	if (SDL_GetTicks() - automap.last_present_ms >= present_interval_ms()) {
		present();
	}
}

// The original reads the modifiers off the keyboard rather than out of the
// event, so one held down before the automap took focus still counts.
bool alt_is_held()
{
	return (SDL_GetModState() & SDL_KMOD_ALT) != 0;
}

bool ctrl_is_held()
{
	return (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
}

// Window coordinates are not map pixels on a HiDPI display; this is the same
// correction the pan applies to its deltas.
SDL_Point window_to_map_px(const float x, const float y)
{
	const auto density = SDL_GetWindowPixelDensity(automap.window);

	return {iroundf(x * density), iroundf(y * density)};
}

// Puts the clicked square on the clipboard in the form a note's hyperlink
// uses, so it can be pasted straight into another note.
void copy_square_reference(const float x, const float y)
{
	const auto point  = window_to_map_px(x, y);
	const auto square = wiz6::SquareAtPixel(point.x, point.y);

	if (!square) {
		return;
	}

	const auto reference = format_str("{%d:%d:%d:%d}",
	                                  square->level,
	                                  square->quadrant,
	                                  square->x,
	                                  square->y);

	if (!SDL_SetClipboardText(reference.c_str())) {
		LOG_WARNING("AUTOMAP: Could not copy to the clipboard: %s",
		            SDL_GetError());
	}
}

// Follows the hyperlink in the note on the clicked square, if it has one. A
// note is free text, so most have none and clicking them does nothing.
void follow_note_link(const float x, const float y)
{
	const auto point  = window_to_map_px(x, y);
	const auto square = wiz6::SquareAtPixel(point.x, point.y);

	if (!square) {
		return;
	}

	const auto* note = wiz6::FindNote(square->level,
	                                  square->quadrant,
	                                  square->x,
	                                  square->y);
	if (!note) {
		return;
	}

	const auto target = wiz6::ParseSquareReference(note->text);

	if (!target) {
		return;
	}

	wiz6::JumpToSquare(*target);
	present();
}

// The square under a click, for the two bindings that open a dialog on one.
std::optional<wiz6::MapSquare> square_at(const float x, const float y)
{
	const auto point = window_to_map_px(x, y);

	return wiz6::SquareAtPixel(point.x, point.y);
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

// Both file hooks run from inside DOS_OpenFile and DOS_CreateFile, just before
// they return and while the guest's INT 21h call is still in flight. Doing the
// automap's own file I/O there would allocate DOS handles out of the guest's
// table at an arbitrary moment, and near the handle limit the map would fail to
// save with nothing to show for it (PORTING.md 7.5). So the hooks only record
// what is wanted, and the next frame does it.
//
// They also fire for every file the guest touches -- a bare boot to the DOS
// prompt opens AUTOEXEC.BAT about a dozen times -- so the work here has to stay
// down to a name comparison.
static bool is_persistence_watched()
{
	// While a request is being carried out the automap is creating MAP.CAC
	// and MAP.VIS itself, and those come back through these same hooks.
	// Neither name can match the game's save, so this is belt and braces --
	// but it keeps the recursion impossible by construction rather than by
	// the file names happening to differ.
	return automap.settings.enabled && !automap.applying_persistence;
}

void AUTOMAP_NotifyFileOpened(const std::string_view dos_path)
{
	if (!is_persistence_watched()) {
		return;
	}

	const auto request = wiz6::RequestForOpenedFile(dos_path);

	if (request != wiz6::PersistenceRequest::None) {
		automap.pending_persistence = request;
	}
}

void AUTOMAP_NotifyFileCreated(const std::string_view dos_path)
{
	if (!is_persistence_watched()) {
		return;
	}

	const auto request = wiz6::RequestForCreatedFile(dos_path);

	if (request != wiz6::PersistenceRequest::None) {
		automap.pending_persistence = request;
	}
}

// Runs whatever the file hooks asked for, now that no guest DOS call is in
// progress. This is deliberately not tied to the window: the map has to be
// loaded and saved whether or not the user has it open.
static void apply_pending_persistence()
{
	if (automap.pending_persistence == wiz6::PersistenceRequest::None) {
		return;
	}

	const auto request          = automap.pending_persistence;
	automap.pending_persistence = wiz6::PersistenceRequest::None;

	automap.applying_persistence = true;
	wiz6::ApplyPersistence(request);
	automap.applying_persistence = false;
}

void AUTOMAP_MaybeRender()
{
	apply_pending_persistence();

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

	present_if_due();
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

	// The overlay sees every event, and answers whether it has taken this
	// one -- clicking a widget must not also drag the map behind it.
	//
	// An event it has taken still needs painting, though, and this is the
	// path that dragging a dialog or a colour picker goes down. Without the
	// repaint here the only redraws would be the emulator's frame tick, and
	// the widget would trail the pointer at 24 Hz.
	if (overlay::HandleEvent(event)) {
		present_if_due();
		return;
	}

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
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		if (event.button.button == SDL_BUTTON_LEFT) {
			automap.left_drag_px = 0.0f;
		}
		break;

	case SDL_EVENT_MOUSE_MOTION:
		if ((event.motion.state & SDL_BUTTON_LMASK) != 0) {
			automap.left_drag_px += std::abs(event.motion.xrel) +
			                        std::abs(event.motion.yrel);

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
			present_if_due();

		} else if (overlay::IsShowingSomething()) {
			// Not dragging, but a tooltip is up and follows the
			// pointer. Without this it would only move on the
			// emulator's frame tick and lag noticeably behind.
			present_if_due();
		}
		break;

	// Middle-click puts the party back in the middle of the window.
	case SDL_EVENT_MOUSE_BUTTON_UP:
		if (event.button.button == SDL_BUTTON_MIDDLE) {
			wiz6::RecentreMap();
			present();
			break;
		}

		// Alt and left copies the clicked square's coordinates in the
		// form a note's hyperlink uses, so one can be pasted into
		// another note. Phase 3 left the Alt and Ctrl guards out
		// because nothing needed them; this is the first thing that
		// does.
		if (event.button.button == SDL_BUTTON_LEFT && alt_is_held() &&
		    !ctrl_is_held()) {
			copy_square_reference(event.button.x, event.button.y);
		}

		// Ctrl and left goes the other way: it follows a link a note
		// carries, showing the square it names and, if that is on
		// another level, that level.
		if (event.button.button == SDL_BUTTON_LEFT && ctrl_is_held() &&
		    !alt_is_held()) {
			follow_note_link(event.button.x, event.button.y);
		}

		// Unmodified left writes the square's note, and right colours
		// it. Both are last, so every modifier combination above has
		// already had its say.
		//
		// A left button coming up at the end of a pan is not a click.
		if (event.button.button == SDL_BUTTON_LEFT && !alt_is_held() &&
		    !ctrl_is_held() && automap.left_drag_px <= DragThresholdPx) {

			if (const auto square = square_at(event.button.x,
			                                  event.button.y)) {
				overlay::EditNote(*square);
			}
		}

		if (event.button.button == SDL_BUTTON_RIGHT && !alt_is_held() &&
		    !ctrl_is_held()) {

			if (const auto square = square_at(event.button.x,
			                                  event.button.y)) {
				overlay::EditNoteColour(*square);
			}
		}
		break;

	default: break;
	}
}
