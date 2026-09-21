// Vigiles - live disk activity for Windows, drawn on a folder tree.
// Copyright (C) 2026 Marco Borgna
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once
#include <windows.h>
#include <cstdint>
#include "etw_monitor.h"

// Activity heat: how long a folder stays tinted after its last event, and how
// saturated the tint is at its peak. The fade is eased so it holds colour for
// most of the window and drops off at the end, which makes a burst much easier
// to follow than a linear ramp.
inline constexpr uint64_t kHeatMs = 5000;

// Pastel palette, indexed by FileOp. Row colours are kept very light so the
// dark text stays readable; the tree tints are the same hues pushed a little
// further, because they get blended back toward white as the heat fades.
inline COLORREF OpRowColor(uint8_t op) {
    switch (op) {
        case Op_Create:  return RGB(217, 242, 224);   // mint
        case Op_Read:    return RGB(220, 235, 250);   // powder blue
        case Op_Write:   return RGB(251, 228, 213);   // apricot
        case Op_Delete:  return RGB(250, 218, 221);   // rose
        case Op_Rename:  return RGB(230, 223, 245);   // lavender
        case Op_SetInfo: return RGB(251, 243, 208);   // butter
        case Op_Close:   return RGB(238, 238, 238);   // light grey
        case Op_DirEnum: return RGB(216, 240, 240);   // pale teal
        default:         return RGB(247, 247, 247);
    }
}

inline COLORREF OpHeatColor(uint8_t op) {
    switch (op) {
        case Op_Create:  return RGB(140, 216, 165);
        case Op_Read:    return RGB(146, 197, 243);
        case Op_Write:   return RGB(247, 176, 126);
        case Op_Delete:  return RGB(246, 152, 163);
        case Op_Rename:  return RGB(193, 180, 234);
        case Op_SetInfo: return RGB(243, 224, 138);
        case Op_DirEnum: return RGB(150, 214, 214);
        default:         return RGB(198, 198, 198);
    }
}

// Blends `c` toward white. strength 1 = full colour, 0 = white.
inline COLORREF FadeToWhite(COLORREF c, double strength) {
    if (strength < 0) strength = 0;
    if (strength > 1) strength = 1;
    int r = 255 - (int)((255 - GetRValue(c)) * strength + 0.5);
    int g = 255 - (int)((255 - GetGValue(c)) * strength + 0.5);
    int b = 255 - (int)((255 - GetBValue(c)) * strength + 0.5);
    return RGB(r, g, b);
}

// Which operation moved the most between two snapshots. Returns 0xFF if the
// folder saw nothing new, so the previous hue is kept while the tint fades.
inline uint8_t DominantOp(const DirStats& now, const DirStats& prev) {
    uint64_t r = now.reads   - prev.reads;
    uint64_t w = now.writes  - prev.writes;
    uint64_t c = now.creates - prev.creates;
    uint64_t d = now.deletes - prev.deletes;
    uint64_t o = now.other   - prev.other;
    uint64_t best = r; uint8_t op = Op_Read;
    if (w > best) { best = w; op = Op_Write;  }
    if (c > best) { best = c; op = Op_Create; }
    if (d > best) { best = d; op = Op_Delete; }
    if (o > best) { best = o; op = Op_Other;  }
    return best ? op : 0xFF;
}
