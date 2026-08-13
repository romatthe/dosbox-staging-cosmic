// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_overlay.h"

#include <cassert>
#include <cmath>
#include <optional>
#include <string>

#include "automap_wiz6.h"
#include "automap_wiz6_render.h"
#include "dosbox.h"
#include "dosbox_config.h"
#include "misc/support.h"
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

// The two dialogs. Only one can be open at a time, which is what the original
// got for free from its modal Win32 dialogs.
enum class Dialog { None, NoteText, NoteColour };

Dialog dialog              = Dialog::None;
wiz6::MapSquare dialog_for = {};

// ImGui wants OpenPopup called exactly once, on the frame the popup opens,
// rather than every frame the popup is up.
bool dialog_opening = false;

// The note being edited, in UTF-8 because that is what ImGui edits. The
// original's buffer is 1023 wide characters; this holds more text than that in
// any script, and a note is capped at 32,766 code units on load anyway.
std::array<char, 4096> note_buffer = {};

uint32_t chosen_colour = 0;

// Where the next "Add to palette" goes. Win32's colour dialog lets the user
// pick which of the 16 custom slots to fill; this fills them left to right and
// wraps, which is the one part of the picker that is not a transcription.
int next_palette_slot = 0;

// Note colours are stored the way the map surface wants them, 0xAABBGGRR, so
// the channels come out in the opposite order from ImGui's float RGBA.
ImVec4 to_imgui_colour(const uint32_t packed)
{
	constexpr auto Max = 255.0f;

	return {static_cast<float>(packed & 0xff) / Max,
	        static_cast<float>((packed >> 8) & 0xff) / Max,
	        static_cast<float>((packed >> 16) & 0xff) / Max,
	        1.0f};
}

uint32_t from_imgui_colour(const ImVec4& colour)
{
	const auto channel = [](const float value) {
		return static_cast<uint32_t>(
		        std::clamp(iroundf(value * 255.0f), 0, 255));
	};

	// The stored alpha never reaches the screen -- the original passes the
	// colour to glColor3f, which ignores it, and draw_box forces it opaque
	// -- but storing it opaque keeps the files sensible to look at.
	return 0xff000000 | (channel(colour.z) << 16) |
	       (channel(colour.y) << 8) | channel(colour.x);
}

// Notes are UTF-16 and people write them in whatever script they think in, so
// the overlay needs a font with more than ImGui's default ProggyClean, whose
// glyphs stop at U+00FF. Cozette carries about 6,000 glyphs -- Latin including
// the Extended-A that Latin-1 misses, Greek, Cyrillic and kana -- and is a
// bitmap face, which sits better next to the map's pixel art than a smooth
// outline font would, and matches the pixel font the debugger window uses.
//
// It does not cover everything: Hebrew, Arabic and the CJK ideographs are
// absent, so text_is_drawable() below still has work to do. Fonts that do
// cover those cost either a great deal more space (GNU Unifont, 5.3 MB) or a
// subsetting step producing a file nobody can regenerate without the recipe.
// PORTING.md section 8.1 records how that was measured and decided.
constexpr auto FontDir  = "fonts";
constexpr auto FontFile = "CozetteVector.otf";

// Cozette is a 6x13 bitmap face and this is the size its pixel grid lands on.
// Asking for anything else makes it blurry, which would give up the one thing
// a bitmap font is for.
constexpr auto FontSizePx = 13.0f;

void load_font(const float density)
{
	const auto path = get_resource_path(FontDir, FontFile);

	if (path.empty()) {
		LOG_WARNING(
		        "AUTOMAP: Could not find the overlay font '%s'; note text "
		        "outside Latin-1 will not display",
		        FontFile);
		return;
	}

	auto& io = ImGui::GetIO();

	// Rounded because a bitmap face wants whole pixels, and scaled here
	// rather than through FontScaleDpi so that the rasterised size is the
	// one asked for.
	const auto size_px = std::round(FontSizePx * density);

	if (!io.Fonts->AddFontFromFileTTF(path.string().c_str(), size_px)) {
		LOG_WARNING(
		        "AUTOMAP: Could not load the overlay font '%s'; note text "
		        "outside Latin-1 will not display",
		        path.string().c_str());
	}
}

void close_dialog()
{
	dialog = Dialog::None;
	ImGui::CloseCurrentPopup();
}

// Both dialogs share this: centred, auto-sized, and dismissed by Escape as
// well as by its own button.
bool begin_dialog(const char* title)
{
	if (dialog_opening) {
		ImGui::OpenPopup(title);
		dialog_opening = false;
	}

	const auto centre = ImGui::GetMainViewport()->GetCenter();

	ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, {0.5f, 0.5f});

	return ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
}

void draw_note_editor()
{
	if (!begin_dialog("Enter comment")) {
		return;
	}

	ImGui::TextUnformatted(
	        "If you leave it blank, the marker is removed from the map.");

	// So that the note can be typed straight away, without clicking the
	// field first.
	//
	// Asking on IsWindowAppearing() alone is not enough and looks like it
	// should be: a popup that auto-resizes spends its first frame being
	// measured rather than drawn, and the focus request made on that frame
	// does not survive it. Asking until something is actually active is the
	// form that works, and it stops the moment the field takes focus -- or
	// the user puts it somewhere else themselves.
	if (!ImGui::IsAnyItemActive()) {
		ImGui::SetKeyboardFocusHere();
	}

	const auto entered = ImGui::InputText("##note",
	                                      note_buffer.data(),
	                                      note_buffer.size(),
	                                      ImGuiInputTextFlags_EnterReturnsTrue);

	const auto accepted = ImGui::Button("OK") || entered;

	ImGui::SameLine();

	const auto cancelled = ImGui::Button("Cancel") ||
	                       ImGui::IsKeyPressed(ImGuiKey_Escape);

	if (accepted) {
		// SetNoteText does the whole of the original's branch here:
		// replaces the text, creates the note, or -- given empty text
		// -- removes it.
		wiz6::SetNoteText(dialog_for.level,
		                  dialog_for.quadrant,
		                  dialog_for.x,
		                  dialog_for.y,
		                  utf8_to_utf16(std::string(note_buffer.data())));
		close_dialog();

	} else if (cancelled) {
		close_dialog();
	}

	ImGui::EndPopup();
}

void draw_colour_picker()
{
	if (!begin_dialog("Note colour")) {
		return;
	}

	auto colour = to_imgui_colour(chosen_colour);

	if (ImGui::ColorPicker4("##colour", &colour.x, ImGuiColorEditFlags_NoAlpha)) {
		chosen_colour = from_imgui_colour(colour);
	}

	// The 16 shared custom colours, which are what MAP.PAL stores.
	ImGui::TextUnformatted("Palette");

	const auto palette = wiz6::CustomPalette();

	for (auto i = 0; i < wiz6::PaletteSize; ++i) {
		constexpr ImVec2 SwatchSize = {20.0f, 20.0f};

		ImGui::PushID(i);

		if (ImGui::ColorButton("##swatch",
		                       to_imgui_colour(palette[static_cast<size_t>(i)]),
		                       ImGuiColorEditFlags_NoAlpha,
		                       SwatchSize)) {
			chosen_colour = palette[static_cast<size_t>(i)];
		}

		ImGui::PopID();

		if (i % 8 != 7) {
			ImGui::SameLine();
		}
	}

	if (ImGui::Button("Add to palette")) {
		palette[static_cast<size_t>(next_palette_slot)] = chosen_colour;

		next_palette_slot = (next_palette_slot + 1) % wiz6::PaletteSize;
	}

	ImGui::Separator();

	const auto accepted = ImGui::Button("OK");

	ImGui::SameLine();

	const auto cancelled = ImGui::Button("Cancel") ||
	                       ImGui::IsKeyPressed(ImGuiKey_Escape);

	if (accepted) {
		wiz6::SetNoteColour(dialog_for.level,
		                    dialog_for.quadrant,
		                    dialog_for.x,
		                    dialog_for.y,
		                    chosen_colour);
		close_dialog();

	} else if (cancelled) {
		close_dialog();
	}

	ImGui::EndPopup();
}

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

// Whether every character of a note can actually be drawn. False for the
// scripts the font does not carry -- Hebrew, Arabic, CJK -- for anything above
// the Basic Multilingual Plane such as emoji, and for nearly everything if the
// font resource is missing and ImGui has fallen back to its built-in one.
//
// Worth checking rather than assuming: a glyph the font does not have is drawn
// as a small box or nothing at all, which looks exactly like the note having
// been stored wrong. It has not been -- see the note on MAP.NTS in PORTING.md
// section 6 -- so the tooltip says which of the two it is.
bool text_is_drawable(const std::u16string_view text)
{
	auto* font = ImGui::GetFont();

	if (!font) {
		return true;
	}

	for (const auto unit : text) {
		// Surrogates are halves of a character above the BMP, which
		// no font the overlay is likely to carry will cover.
		constexpr char16_t FirstSurrogate = 0xd800;
		constexpr char16_t LastSurrogate  = 0xdfff;

		if (unit >= FirstSurrogate && unit <= LastSurrogate) {
			return false;
		}

		if (!font->IsGlyphInFont(unit)) {
			return false;
		}
	}

	return true;
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
		auto text = utf16_to_utf8(note->text);

		if (!text_is_drawable(note->text)) {
			text += "\n(some characters cannot be shown, but they "
			        "are stored correctly)";
		}

		return text;
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
	// A dialog is modal, so it is the whole of the overlay while it is up
	// -- including instead of the tooltip, which the original also hides
	// before opening either of these.
	switch (dialog) {
	case Dialog::NoteText: draw_note_editor(); return;
	case Dialog::NoteColour: draw_colour_picker(); return;
	case Dialog::None: break;
	}

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

	// FontScaleDpi is deliberately left alone: the font below is a bitmap
	// face rasterised at a whole-pixel size, and scaling it afterwards is
	// exactly what would blur it.
	load_font(density);

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

void EditNote(const wiz6::MapSquare& square)
{
	if (!context) {
		return;
	}

	// A dialog is already up. ImGui claims the mouse from the frame after
	// one opens, so a second click in the same frame -- the second half of
	// a double click, say -- still reaches this and would otherwise
	// re-target a dialog that has only just opened.
	if (dialog != Dialog::None) {
		return;
	}

	dialog         = Dialog::NoteText;
	dialog_for     = square;
	dialog_opening = true;

	note_buffer = {};

	if (const auto* note = wiz6::FindNote(
	            square.level, square.quadrant, square.x, square.y)) {
		// Truncating rather than refusing: the cap is far above any
		// note a person would type, and the alternative is a dialog
		// that cannot be opened.
		const auto text = utf16_to_utf8(note->text);

		std::snprintf(note_buffer.data(), note_buffer.size(), "%s", text.c_str());
	}
}

void EditNoteColour(const wiz6::MapSquare& square)
{
	if (!context) {
		return;
	}

	if (dialog != Dialog::None) {
		return;
	}

	const auto* note = wiz6::FindNote(square.level,
	                                  square.quadrant,
	                                  square.x,
	                                  square.y);
	if (!note) {
		return;
	}

	dialog         = Dialog::NoteColour;
	dialog_for     = square;
	dialog_opening = true;
	chosen_colour  = note->colour;
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
