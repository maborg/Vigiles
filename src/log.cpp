#include "log.h"
#include <windows.h>
#include <dbghelp.h>
#include <stdarg.h>
#include <stdio.h>
#include <exception>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

static HANDLE            g_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION  g_cs;
static bool              g_ready = false;
static std::wstring      g_logPath;
static std::wstring      g_dumpPath;

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s = buf;
    size_t p = s.find_last_of(L'\\');
    return p == std::wstring::npos ? std::wstring(L".") : s.substr(0, p);
}

static HANDLE TryOpen(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                       CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
}

static void WriteRaw(const std::wstring& line) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::vector<char> utf8((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                        utf8.data(), n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(g_file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

// ---------------------------------------------------------------- crash paths

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    LOGE(L"*** UNHANDLED EXCEPTION code=0x%08X addr=0x%p flags=0x%X",
         ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0,
         ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr,
         ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionFlags : 0);

    if (ep && ep->ExceptionRecord &&
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        LOGE(L"    access violation: %s address 0x%p",
             ep->ExceptionRecord->ExceptionInformation[0] ? L"write to" : L"read from",
             (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }

    HANDLE dump = CreateFileW(g_dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        BOOL ok = MiniDumpWriteDump(
            GetCurrentProcess(), GetCurrentProcessId(), dump,
            (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
            ep ? &mei : nullptr, nullptr, nullptr);
        CloseHandle(dump);
        LOGE(L"    minidump %s: %s", ok ? L"written" : L"FAILED", g_dumpPath.c_str());
    }

    LogShutdown();
    return EXCEPTION_EXECUTE_HANDLER;   // terminate quietly, we have the log
}

static void OnTerminate() {
    LOGE(L"*** std::terminate called");
    try {
        auto e = std::current_exception();
        if (e) std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        wchar_t w[512] = {};
        MultiByteToWideChar(CP_ACP, 0, ex.what(), -1, w, 511);
        LOGE(L"    uncaught std::exception: %s", w);
    } catch (...) {
        LOGE(L"    uncaught non-standard exception");
    }
    LogShutdown();
    ExitProcess(3);
}

static void OnInvalidParameter(const wchar_t* expr, const wchar_t* func,
                               const wchar_t* file, unsigned line, uintptr_t) {
    LOGE(L"*** CRT invalid parameter: %s in %s (%s:%u)",
         expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
    LogShutdown();
    ExitProcess(4);
}

static void OnPureCall() {
    LOGE(L"*** pure virtual call");
    LogShutdown();
    ExitProcess(5);
}

// ---------------------------------------------------------------- public API

void LogInit() {
    InitializeCriticalSection(&g_cs);

    g_logPath = ExeDir() + L"\\Vigiles.log";
    g_dumpPath = ExeDir() + L"\\Vigiles.dmp";
    g_file = TryOpen(g_logPath);
    if (g_file == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        g_logPath = std::wstring(tmp) + L"Vigiles.log";
        g_dumpPath = std::wstring(tmp) + L"Vigiles.dmp";
        g_file = TryOpen(g_logPath);
    }
    g_ready = true;

    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(OnTerminate);
    _set_invalid_parameter_handler(OnInvalidParameter);
    _set_purecall_handler(OnPureCall);

    // WER would otherwise swallow our filter in some configurations.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    LOGI(L"=== Vigiles - ubi fumus, ibi ignis ===");
    LOGI(L"log file: %s", g_logPath.c_str());
    OSVERSIONINFOEXW vi{}; vi.dwOSVersionInfoSize = sizeof(vi);
    LOGI(L"pid=%u  build=%s %s", GetCurrentProcessId(),
         _CRT_WIDE(__DATE__), _CRT_WIDE(__TIME__));
}

void LogShutdown() {
    if (g_file != INVALID_HANDLE_VALUE) {
        WriteRaw(L"=== log closed ===\r\n");
        FlushFileBuffers(g_file);
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

std::wstring LogFilePath() { return g_logPath; }

void LogWrite(const wchar_t* level, const wchar_t* fmt, ...) {
    if (!g_ready) return;

    wchar_t body[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2304];
    _snwprintf_s(line, _countof(line), _TRUNCATE,
                 L"%02u:%02u:%02u.%03u [%5u] %s  %s\r\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 GetCurrentThreadId(), level, body);

    OutputDebugStringW(line);
    EnterCriticalSection(&g_cs);
    WriteRaw(line);
    LeaveCriticalSection(&g_cs);
}

void LogLastError(const wchar_t* what, unsigned long err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&msg, 0, nullptr);
    if (msg) {
        for (wchar_t* p = msg; *p; ++p) if (*p == L'\r' || *p == L'\n') *p = L' ';
        LOGE(L"%s failed: %lu (%s)", what, err, msg);
        LocalFree(msg);
    } else {
        LOGE(L"%s failed: %lu", what, err);
    }
}
