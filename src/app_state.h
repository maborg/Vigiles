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
#include <deque>
#include <string>
#include <cstdint>
#include "etw_monitor.h"

struct Row {
    uint64_t ts;
    uint32_t pid;
    uint32_t size;
    uint8_t  op;
    std::wstring path;
};

// Main window and its child controls, created in WM_CREATE. Shared by the
// tree, AI-prompt and window-message-loop code.
extern HWND g_hwnd, g_tree, g_list, g_status, g_legend, g_promptBtn;

extern EtwMonitor g_mon;

// Live view of the most recent operations, newest last. No history is kept
// beyond kMaxRows - this is a live tail, not a log.
extern std::deque<Row> g_rows;
inline constexpr size_t kMaxRows = 2000;

// Transient status-bar text (e.g. "folder is gone", "prompt copied"), shown
// until g_flashUntil and then replaced by the regular status.
extern std::wstring g_flash;
extern uint64_t g_flashUntil;
