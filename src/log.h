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
