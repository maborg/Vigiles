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

#include "process_names.h"
#include <windows.h>
#include <unordered_map>

static std::unordered_map<uint32_t, std::wstring> g_procNames;

const std::wstring& ProcessName(uint32_t pid) {
    auto it = g_procNames.find(pid);
    if (it != g_procNames.end()) return it->second;

    std::wstring name = L"pid " + std::to_wstring(pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &n) && n) {
            std::wstring full(buf, n);
            size_t p = full.find_last_of(L'\\');
            name = (p == std::wstring::npos) ? full : full.substr(p + 1);
        }
        CloseHandle(h);
    }
    return g_procNames.emplace(pid, std::move(name)).first->second;
}
