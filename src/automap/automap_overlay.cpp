// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_overlay.h"

#include <cassert>

#include "dosbox.h"
#include "dosbox_config.h"
#include "utils/checks.h"

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

// Builds the frame's widgets. Nothing yet: this commit is the wiring, and the
// tooltip that first uses it is the next one.
void draw_widgets() {}

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
