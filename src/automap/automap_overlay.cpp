// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_overlay.h"

#include <cassert>
#include <optional>
#include <string>

#include "automap_wiz6.h"
#include "automap_wiz6_render.h"
#include "dosbox.h"
#include "dosbox_config.h"
#include "misc/unicode.h"
#include "utils/checks.h"
#include "utils/math_utils.h"
#include "utils/string_utils.h"

// must be included after dosbox_config.h
#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

CHECK_NARROWING();

namespace overlay {

namespace {

ImGuiContext* context = nullptr;

// ImGui addresses whichever context is current through a global, and the
// debugger drives a context of its own without ever setting one
// (debugger_gui.cpp:407, which calls a bare ImGui::CreateContext). So the
// automap makes its context current only for as long as it is using it and
// then puts back whatever was there. That keeps both windows working
// regardless of which was opened first, and needs no change on the debugger's
// side.
//
// Everything that reaches ImGui -- including the backends' own init, which
// stores itself in the current context's IO -- has to be inside one of these.
class CurrentContext {
public:
	CurrentContext() : previous(ImGui::GetCurrentContext())
	{
		ImGui::SetCurrentContext(context);
	}

	~CurrentContext()
	{
		ImGui::SetCurrentContext(previous);
	}

	CurrentContext(const CurrentContext&)            = delete;
	CurrentContext& operator=(const CurrentContext&) = delete;

private:
	ImGuiContext* previous = nullptr;
};

// Which square the pointer is over, or nothing when it is outside the window
// or off every quadrant -- which most of the window usually is, since quadrants
// do not tile a level.
std::optional<wiz6::MapSquare> square_under_pointer()
{
	if (!ImGui::IsMousePosValid()) {
		return {};
	}

	// ImGui works in window points while the map is drawn in pixels, and
	// the two differ on a HiDPI display. The platform backend has already
	// worked out the ratio for this frame.
	const auto& io   = ImGui::GetIO();
	const auto mouse = ImGui::GetMousePos();

	return wiz6::SquareAtPixel(iroundf(mouse.x * io.DisplayFramebufferScale.x),
	                           iroundf(mouse.y * io.DisplayFramebufferScale.y));
}

// The tooltip text for a square, or nothing when that square should have none.
//
// Transcribed from W6_OnMouseMotionInAutomapWindow (am_wiz6.cpp:1553). The
// original also has an "Invisible" string it can never display, because it
// shows the tooltip only in the two cases below.
std::optional<std::string> tooltip_for(const wiz6::MapSquare& square,
                                       const wiz6::PartyPosition& party,
                                       const bool alt_is_held)
{
	// Holding Alt names the square instead, which is what makes the Alt
	// click that copies its coordinates something other than a guess.
	if (alt_is_held) {
		return format_str("Quadrant: %d qX: %d qY: %d",
		                  square.quadrant,
		                  square.x,
		                  square.y);
	}

	if (const auto* note = wiz6::FindNote(
	            square.level, square.quadrant, square.x, square.y)) {
		// A note wins over "Current Position" when the party is
		// standing on one, as it does in the original.
		//
		// TODO Notes are UTF-16 but the overlay draws them in ImGui's
		// default font, ProggyClean, whose glyphs stop at U+00FF. The
		// conversion here is correct and complete; it is the drawing
		// that is not. A German note survives, an em dash comes out as
		// '?' and a Cyrillic or CJK note is lost altogether. Fixing it
		// means giving the overlay a font with real coverage -- see
		// PORTING.md section 8.1, which is where the decision to ship
		// this limitation for now is written down.
		return utf16_to_utf8(note->text);
	}

	const auto is_party_square = square.level == party.level &&
	                             square.quadrant == party.quadrant &&
	                             square.x == party.x && square.y == party.y;

	if (is_party_square) {
		return std::string("Current Position");
	}

	return {};
}

// Builds the frame's widgets.
void draw_widgets()
{
	const auto party = wiz6::GetPartyPosition();

	// No party, or the party standing somewhere the map is blank: the
	// original hides the tooltip in both cases rather than describing
	// squares nobody can see.
	if (!party ||
	    wiz6::IsDarkZone(party->level, party->quadrant, party->x, party->y)) {
		return;
	}

	const auto square = square_under_pointer();

	if (!square) {
		return;
	}

	// Read off the keyboard rather than out of ImGui, so that a modifier
	// held down before the automap took focus still counts -- the same
	// reasoning as the click handlers in automap.cpp.
	const auto alt_is_held = (SDL_GetModState() & SDL_KMOD_ALT) != 0;

	if (const auto text = tooltip_for(*square, *party, alt_is_held)) {
		ImGui::SetTooltip("%s", text->c_str());
	}
}

// Gives up, leaving the automap in the state it is in when the overlay was
// never set up at all -- which every entry point here already handles, because
// a build without a working ImGui still has to show a map.
void abandon()
{
	ImGui::DestroyContext(context);
	context = nullptr;
}

} // namespace

bool Init(SDL_Window* window, SDL_Renderer* renderer)
{
	assert(window && renderer);
	assert(!context);

	IMGUI_CHECKVERSION();

	// The guard has to go round the creation as well, and the reason is
	// subtle. ImGui::CreateContext leaves the new context current only when
	// there was no previous one, and otherwise puts the previous one back
	// (imgui.cpp: "Restore previous context if any, else keep new one").
	//
	// So an automap that opens *before* the debugger would leave its own
	// context current, and the debugger's CreateContext would then hand the
	// debugger's own start-up the automap's context to configure -- SDL_GPU
	// backends and all. Leaving the global exactly as it was found, null
	// included, is what keeps that from happening.
	const CurrentContext current = {};

	context = ImGui::CreateContext();

	if (!context) {
		LOG_WARNING("AUTOMAP: Could not create the overlay's ImGui context");
		return false;
	}

	auto& io = ImGui::GetIO();

	// The automap has no layout worth remembering, and writing an imgui.ini
	// into whatever directory dosbox was started from would be rude.
	io.IniFilename = nullptr;

	ImGui::StyleColorsDark();

	// A window on a HiDPI display is handed more pixels than it asks for.
	// The renderer backend scales the overlay's geometry for that on its
	// own, but the font has to be rasterised at the larger size rather than
	// magnified, or the text is the one blurry thing on a window of crisp
	// pixel art. Taken once, as the debugger does: following a window
	// dragged between displays of different densities is not worth it.
	const auto density = SDL_GetWindowPixelDensity(window);

	auto& style = ImGui::GetStyle();

	style.ScaleAllSizes(density);
	style.FontScaleDpi = density;

	if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
		LOG_WARNING("AUTOMAP: Could not set up the overlay's platform backend");
		abandon();
		return false;
	}

	if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
		LOG_WARNING("AUTOMAP: Could not set up the overlay's renderer backend");
		ImGui_ImplSDL3_Shutdown();
		abandon();
		return false;
	}

	return true;
}

bool HandleEvent(const SDL_Event& event)
{
	if (!context) {
		return false;
	}

	const CurrentContext current = {};

	ImGui_ImplSDL3_ProcessEvent(&event);

	// ProcessEvent says whether ImGui *looked* at the event, not whether it
	// wants it, so the answer comes from the IO flags instead. Only the
	// mouse is contended: the automap has no keyboard controls to lose.
	switch (event.type) {
	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
	case SDL_EVENT_MOUSE_WHEEL: return ImGui::GetIO().WantCaptureMouse;

	default: return false;
	}
}

void Draw(SDL_Renderer* renderer)
{
	if (!context) {
		return;
	}

	assert(renderer);

	const CurrentContext current = {};

	ImGui_ImplSDLRenderer3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();

	draw_widgets();

	ImGui::Render();
	ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

} // namespace overlay
