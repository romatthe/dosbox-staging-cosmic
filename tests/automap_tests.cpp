// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap/automap_wiz6_coords.h"

#include <gtest/gtest.h>

namespace {

using namespace wiz6;

// Anchored against the original mod rather than against ourselves. Its
// MousePositionToQuadrantPosition reaches a square's absolute Y by subtracting
// 7 from the row and then that from 255, so row 0 is absolute Y 262. Reading
// the value back out of our own expression would only prove it equals itself.
TEST(automap_wiz6_coords, TopRowMatchesTheOriginal)
{
	EXPECT_EQ(AbsYOfRow(0), 262);
	EXPECT_EQ(RowOfAbsY(262), 0);
}

TEST(automap_wiz6_coords, RowAndAbsYInvertEachOther)
{
	for (auto abs_y = 0; abs_y <= 262; ++abs_y) {
		EXPECT_EQ(AbsYOfRow(RowOfAbsY(abs_y)), abs_y);
	}
}

// The one that has teeth. A square's row is built in two steps that live in
// different functions of the rasteriser -- the quadrant's origin row, then the
// square's offset within it -- and the hit-test has to undo both at once. This
// replicates the forward pair and feeds the result back through the inverse.
//
// Getting the composed constant wrong by one is exactly the bug this covers:
// every clicked square came back one row north of the one drawn there.
TEST(automap_wiz6_coords, ClickedSquareMatchesDrawnSquare)
{
	// Real quadrant origins from level 0 of a save, which is two
	// disconnected regions, plus the extremes of the range.
	for (const auto origin_y : {0, 10, 18, 116, 124, 132, 248}) {
		for (auto y = 0; y < QuadrantSize; ++y) {
			const auto quadrant_row = RowOfAbsY(origin_y +
			                                    QuadrantSize - 1);
			const auto square_row   = quadrant_row +
			                        (QuadrantSize - 1 - y);

			EXPECT_EQ(AbsYOfRow(square_row), origin_y + y);
		}
	}
}

// Northward on the map is upward on the screen, and a square's height is one
// row. Both would still hold if the mapping were offset, which is what the
// anchor above is for.
TEST(automap_wiz6_coords, NorthIsUp)
{
	EXPECT_EQ(RowOfAbsY(100) - RowOfAbsY(101), 1);
	EXPECT_LT(RowOfAbsY(200), RowOfAbsY(100));
}

TEST(automap_wiz6_coords, ParsesASquareReference)
{
	EXPECT_EQ(ParseSquareReference(u"{3:11:7:0}"), (MapSquare{3, 11, 7, 0}));

	// Notes are free text, and the link is normally somewhere inside a
	// sentence rather than the whole of it.
	EXPECT_EQ(ParseSquareReference(u"back door {0:1:2:3} -- locked"),
	          (MapSquare{0, 1, 2, 3}));
}

TEST(automap_wiz6_coords, RejectsWhatIsNotASquareReference)
{
	// Notes without a link at all, which is most of them.
	EXPECT_FALSE(ParseSquareReference(u""));
	EXPECT_FALSE(ParseSquareReference(u"trapped chest"));

	// Malformed.
	EXPECT_FALSE(ParseSquareReference(u"{3:11:7}"));
	EXPECT_FALSE(ParseSquareReference(u"{3:11:7:0"));
	EXPECT_FALSE(ParseSquareReference(u"{3::7:0}"));
	EXPECT_FALSE(ParseSquareReference(u"{3:11:7:x}"));
	EXPECT_FALSE(ParseSquareReference(u"{-3:11:7:0}"));

	// Well formed but naming a square that cannot exist. Each field is one
	// past its limit; a note carrying one of these would otherwise index
	// out of every array the map is kept in.
	EXPECT_FALSE(ParseSquareReference(u"{16:0:0:0}"));
	EXPECT_FALSE(ParseSquareReference(u"{0:12:0:0}"));
	EXPECT_FALSE(ParseSquareReference(u"{0:0:8:0}"));
	EXPECT_FALSE(ParseSquareReference(u"{0:0:0:8}"));

	// A count long enough to overflow, rather than being truncated to
	// something in range.
	EXPECT_FALSE(ParseSquareReference(u"{99999999999:0:0:0}"));
}

} // namespace
