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
#include <string>
#include <cstdint>

inline std::wstring FormatCount(uint64_t v) {
    wchar_t b[32];
    if (v < 1000)            swprintf_s(b, L"%llu", v);
    else if (v < 1000000)    swprintf_s(b, L"%.1fK", v / 1000.0);
    else if (v < 1000000000) swprintf_s(b, L"%.1fM", v / 1000000.0);
    else                     swprintf_s(b, L"%.1fG", v / 1e9);
    return b;
}

inline std::wstring FormatBytes(uint64_t v) {
    wchar_t b[32];
    if (v < 1024)              swprintf_s(b, L"%llu B", v);
    else if (v < 1024ull*1024) swprintf_s(b, L"%.1f KB", v / 1024.0);
    else if (v < 1024ull*1024*1024) swprintf_s(b, L"%.1f MB", v / 1048576.0);
    else                       swprintf_s(b, L"%.2f GB", v / 1073741824.0);
    return b;
}

inline std::wstring FormatTime(uint64_t filetime) {
    FILETIME ft; ft.dwLowDateTime = (DWORD)filetime; ft.dwHighDateTime = (DWORD)(filetime >> 32);
    FILETIME lft; SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lft) || !FileTimeToSystemTime(&lft, &st)) return L"--";
    wchar_t b[32];
    swprintf_s(b, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}
