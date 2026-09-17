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
#include <string>

// Write-through file log + unhandled-exception handler that drops a minidump.
// The log is opened with FILE_FLAG_WRITE_THROUGH so that whatever was written
// before a hard crash is actually on disk afterwards.
void LogInit();
void LogShutdown();
void LogWrite(const wchar_t* level, const wchar_t* fmt, ...);
void LogLastError(const wchar_t* what, unsigned long err);
std::wstring LogFilePath();

#define LOGI(...) LogWrite(L"INFO", __VA_ARGS__)
#define LOGW(...) LogWrite(L"WARN", __VA_ARGS__)
#define LOGE(...) LogWrite(L"ERR ", __VA_ARGS__)

// Logs entry and exit of a scope, so a crash shows up as an entry with no exit.
struct LogScope {
    const wchar_t* name;
    explicit LogScope(const wchar_t* n) : name(n) { LOGI(L">> %s", n); }
    ~LogScope() { LOGI(L"<< %s", name); }
};
#define LOG_SCOPE(n) LogScope _logscope_##__LINE__(L##n)
