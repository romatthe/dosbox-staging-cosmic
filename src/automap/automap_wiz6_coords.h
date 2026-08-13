// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef DOSBOX_AUTOMAP_WIZ6_COORDS_H
#define DOSBOX_AUTOMAP_WIZ6_COORDS_H

#include <optional>
#include <string_view>

#include "automap_wiz6.h"

// The two ways of naming a square of a Wizardry VI level -- a place on the map,
// and the text a note's hyperlink uses -- and the conversions back. Kept apart
// from the rasteriser so that each pair is obliged to agree, and so they can be
// tested without a window or a running game.
namespace wiz6 {

// A square of the level the map is currently showing.
struct MapSquare {
	int level    = 0;
	int quadrant = 0;
	int x        = 0;
	int y        = 0;

	bool operator==(const MapSquare& other) const = default;
};

// A square is this many pixels on the map, and a level is 256 squares tall.
// Both are baked into the original's arithmetic and its file formats.
constexpr int SquarePx  = 22;
constexpr int LevelRows = 256;

// The game's Y axis points north while the map's points south, so a square's
// row counts *down* from the level's northern edge. The rasteriser arrives at
// it in two steps, in two different functions: a quadrant's origin sits at row
// (LevelRows - 1 - origin_y), and a square sits (QuadrantSize - 1 - y) below
// that. Composing them gives the row of any level-absolute Y.
//
// Both directions must go through these. They did not, once: the hit-test
// spelled the constant out for itself as (LevelRows - 1 + QuadrantSize) and
// every clicked square came back one row too far north.
//
// Beware that this is *not* the constant in the rasteriser's `view_y`, which is
// one larger and correct at that value -- the original does the same, at
// am_wiz6.cpp:1181 against 262 in its own hit-test, and Phase 3's pixel
// comparison validated it. The two look like they should match and must not be
// made to.
constexpr int RowOfAbsY(const int abs_y)
{
	return (LevelRows - 1) + (QuadrantSize - 1) - abs_y;
}

constexpr int AbsYOfRow(const int row)
{
	return (LevelRows - 1) + (QuadrantSize - 1) - row;
}

// A note can carry a hyperlink to another square, written as
// {level:quadrant:x:y} -- the same form Alt-clicking a square puts on the
// clipboard. This returns the first one in a note's text, or nothing if there
// is none, it is malformed, or it names a square that cannot exist.
//
// Parsed by hand rather than with swscanf, which is what the original uses
// (am_wiz6.cpp:1668). Note text is UTF-16, and `wchar_t` is four bytes here
// against the two the original was built for, so every wide C function would
// read it at the wrong width -- see PORTING.md section 8.1.
inline std::optional<MapSquare> ParseSquareReference(const std::u16string_view text)
{
	auto at = text.find(u'{');

	if (at == std::u16string_view::npos) {
		return {};
	}
	++at;

	// A field is a run of decimal digits ending in the separator that
	// follows it. Digits are counted so that an empty field is refused and
	// a long one cannot overflow rather than being silently truncated.
	const auto next_field = [&](const char16_t separator) -> std::optional<int> {
		constexpr int MaxDigits = 3;

		auto value  = 0;
		auto digits = 0;

		while (at < text.size() && text[at] >= u'0' && text[at] <= u'9') {
			if (++digits > MaxDigits) {
				return {};
			}
			value = value * 10 + (text[at] - u'0');
			++at;
		}

		if (digits == 0 || at >= text.size() || text[at] != separator) {
			return {};
		}
		++at;

		return value;
	};

	const auto level    = next_field(u':');
	const auto quadrant = next_field(u':');
	const auto x        = next_field(u':');
	const auto y        = next_field(u'}');

	if (!level || !quadrant || !x || !y) {
		return {};
	}

	if (*level >= LevelCount || *quadrant >= QuadrantCount ||
	    *x >= QuadrantSize || *y >= QuadrantSize) {
		return {};
	}

	return MapSquare{*level, *quadrant, *x, *y};
}

} // namespace wiz6

#endif // DOSBOX_AUTOMAP_WIZ6_COORDS_H
