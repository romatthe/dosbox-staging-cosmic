# Fonts

## CozetteVector.otf

Used by the Wizardry VI automap's overlay to draw note text, which is UTF-16
and so may be in any script. Dear ImGui's built-in font stops at U+00FF;
Cozette carries roughly 6,000 glyphs, including the Latin Extended-A, Greek,
Cyrillic and kana that notes are most likely to need.

It is a 6x13 bitmap face, drawn at 13 pixels so that its pixel grid lands
exactly. That keeps it crisp beside the map's tile art, and consistent with the
pixel font the debugger window uses.

- Version 1.30.0
- Copyright (c) 2020 Slavfox
- MIT licence, the text of which ships as `doc/licenses/MIT.txt`
- <https://github.com/the-moonwitch/cozette>

Hebrew, Arabic and the CJK ideographs are not covered. The automap detects text
it cannot draw and says so rather than showing it wrongly; see
`src/automap/automap_overlay.cpp`.
