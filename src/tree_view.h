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
#include <commctrl.h>
#include <string>
#include "etw_monitor.h"

struct ItemData {
    std::wstring path;      // "C:\" or "C:\Windows"
    std::wstring label;     // display name without the stats suffix
    bool populated = false;
    // Snapshot refreshed on the UI timer. Custom draw reads this instead of
    // locking the DirIndex mutex, which would re-enter a non-recursive mutex
    // already held by RefreshVisibleTree on this same thread.
    DirStats cached{};
    bool hasStats = false;
    DirStats prev{};        // previous snapshot, for per-tick deltas
    bool hasPrev = false;
    uint8_t heatOp = Op_Other;   // which operation dominated most recently
};

ItemData* GetItemData(HTREEITEM h);

// Populates the drive roots at start-up, and one level of a lazily-expanded
// node on demand.
void AddDrives();
void PopulateChildren(HTREEITEM parent);

// Double-clicking a row jumps the tree to the folder that row lives in,
// expanding it on the way, so the operator can follow activity from a file
// back to where it comes from.
void JumpToRow(int index);

// Rewrites the caption of every currently visible tree row with its subtree
// counters, and refreshes the heat state (ItemData::cached/prev/heatOp) that
// the tree's NM_CUSTOMDRAW handler reads to tint recently-active folders.
void RefreshVisibleTree();

// True if a (lowercased) path falls under the folder currently selected in
// the tree, or if no folder is selected.
bool MatchesFilter(const std::wstring& lowPath);

// Switches the list view to show only events under `displayPath` from now
// on, discarding whatever was queued for the previous selection.
void SetFilter(const std::wstring& displayPath);
