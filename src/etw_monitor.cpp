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

#include "etw_monitor.h"
#include "log.h"
#include <tdh.h>
#include <cwchar>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")

// Microsoft-Windows-Kernel-File {EDD08927-9CC4-4E65-B970-C2560FB5C289}
static const GUID kKernelFileGuid =
    { 0xEDD08927, 0x9CC4, 0x4E65, { 0xB9, 0x70, 0xC2, 0x56, 0x0F, 0xB5, 0xC2, 0x89 } };

// Keywords published by that provider.
enum : ULONGLONG {
    KF_FILENAME             = 0x0010,
    KF_FILEIO               = 0x0020,
    KF_OP_END               = 0x0040,   // completion events - doubles volume, off by default
    KF_CREATE               = 0x0080,
    KF_READ                 = 0x0100,
    KF_WRITE                = 0x0200,
    KF_DELETE_PATH          = 0x0400,
    KF_RENAME_SETLINK_PATH  = 0x0800,
    KF_CREATE_NEW_FILE      = 0x1000,
};

const wchar_t* OpName(uint8_t op) {
    switch (op) {
        case Op_Create:  return L"Create";
        case Op_Read:    return L"Read";
        case Op_Write:   return L"Write";
        case Op_Delete:  return L"Delete";
        case Op_Rename:  return L"Rename";
        case Op_SetInfo: return L"SetInfo";
        case Op_Close:   return L"Close";
        case Op_DirEnum: return L"DirEnum";
        default:         return L"Other";
    }
}

// ---------------------------------------------------------------- TDH helpers

static bool GetPropRaw(PEVENT_RECORD rec, const wchar_t* name, std::vector<BYTE>& out) {
    PROPERTY_DATA_DESCRIPTOR d{};
    d.PropertyName = (ULONGLONG)(ULONG_PTR)name;
    d.ArrayIndex = ULONG_MAX;
    ULONG size = 0;
    if (TdhGetPropertySize(rec, 0, nullptr, 1, &d, &size) != ERROR_SUCCESS || size == 0)
        return false;
    out.resize(size);
    return TdhGetProperty(rec, 0, nullptr, 1, &d, size, out.data()) == ERROR_SUCCESS;
}

static bool GetPropU64(PEVENT_RECORD rec, const wchar_t* name, uint64_t& v) {
    std::vector<BYTE> b;
    if (!GetPropRaw(rec, name, b)) return false;
    switch (b.size()) {
        case 1: v = *(uint8_t*)b.data();  return true;
        case 2: v = *(uint16_t*)b.data(); return true;
        case 4: v = *(uint32_t*)b.data(); return true;
        case 8: v = *(uint64_t*)b.data(); return true;
        default: return false;
    }
}

static bool GetPropStr(PEVENT_RECORD rec, const wchar_t* name, std::wstring& s) {
    std::vector<BYTE> b;
    if (!GetPropRaw(rec, name, b) || b.size() < sizeof(wchar_t)) return false;
    const wchar_t* p = (const wchar_t*)b.data();
    size_t maxc = b.size() / sizeof(wchar_t);
    size_t n = 0;
    while (n < maxc && p[n]) ++n;
    if (n == 0) return false;
    s.assign(p, n);
    return true;
}

// ---------------------------------------------------------------- construction

EtwMonitor::EtwMonitor() : ring_(cfg_.ringCapacity) {
    BuildDeviceMap();
}

EtwMonitor::~EtwMonitor() { Stop(); }

void EtwMonitor::BuildDeviceMap() {
    LOG_SCOPE("BuildDeviceMap");
    devMap_.clear();

    wchar_t drives[512] = {};
    DWORD n = GetLogicalDriveStringsW(_countof(drives), drives);
    if (!n) {
        LogLastError(L"GetLogicalDriveStrings", GetLastError());
        return;
    }
    for (wchar_t* p = drives; *p; p += wcslen(p) + 1) {
        wchar_t letter[3] = { p[0], L':', 0 };
        wchar_t target[MAX_PATH] = {};
        UINT type = GetDriveTypeW(p);
        wchar_t fs[32] = {};
        GetVolumeInformationW(p, nullptr, 0, nullptr, nullptr, nullptr, fs, _countof(fs));

        if (QueryDosDeviceW(letter, target, MAX_PATH)) {
            LOGI(L"  device map: %s -> %s   (type=%u fs=%s)", letter, target, type,
                 fs[0] ? fs : L"?");
            devMap_.emplace_back(ToLowerW(target), letter);

            // A Dev Drive, or any VHD-backed volume, can expose a symlink chain.
            // Resolve one more hop so both spellings match.
            wchar_t second[MAX_PATH] = {};
            if (QueryDosDeviceW(target + 8 /* skip "\\Device\\" */, second, MAX_PATH) &&
                _wcsicmp(second, target) != 0) {
                LOGI(L"    also: %s -> %s", second, letter);
                devMap_.emplace_back(ToLowerW(second), letter);
            }
        } else {
            LogLastError(L"QueryDosDevice", GetLastError());
            LOGE(L"  no device path for %s (type=%u fs=%s)", letter, type,
                 fs[0] ? fs : L"?");
        }
    }

    // Volume GUID form: events occasionally arrive as \\?\Volume{...}\path.
    wchar_t vol[MAX_PATH] = {};
    HANDLE hv = FindFirstVolumeW(vol, _countof(vol));
    if (hv != INVALID_HANDLE_VALUE) {
        do {
            wchar_t names[512] = {};
            DWORD len = 0;
            if (GetVolumePathNamesForVolumeNameW(vol, names, _countof(names), &len) &&
                names[0]) {
                std::wstring guid = vol;            // "\\?\Volume{...}\"
                if (guid.size() > 4) {
                    guid = L"\\Device" + guid.substr(3);  // "\Device\Volume{...}\"
                    if (!guid.empty() && guid.back() == L'\\') guid.pop_back();
                    std::wstring letter(names, 2);
                    LOGI(L"  volume map: %s -> %s", guid.c_str(), letter.c_str());
                    devMap_.emplace_back(ToLowerW(guid), letter);
                }
            }
        } while (FindNextVolumeW(hv, vol, _countof(vol)));
        FindVolumeClose(hv);
    }

    lastDevRefresh_ = GetTickCount64();
    LOGI(L"device map has %zu entries", devMap_.size());
}

void EtwMonitor::RefreshDeviceMap() {
    uint64_t now = GetTickCount64();
    if (now - lastDevRefresh_ < 5000) return;   // at most once every 5s
    LOGI(L"refreshing device map (unresolved path seen)");
    BuildDeviceMap();
}

std::wstring EtwMonitor::DevicePathToDos(const std::wstring& nt) {
    std::wstring low = ToLowerW(nt);

    for (const auto& kv : devMap_) {
        const std::wstring& dev = kv.first;
        if (low.size() > dev.size() && low.compare(0, dev.size(), dev) == 0 &&
            low[dev.size()] == L'\\') {
            return kv.second + nt.substr(dev.size());
        }
    }

    // \Device\Mup\server\share -> \\server\share
    if (low.compare(0, 12, L"\\device\\mup\\") == 0)
        return L"\\\\" + nt.substr(12);

    // Unresolved. Log each distinct device prefix once: this is what a Dev
    // Drive or a freshly mounted VHD looks like when the map is stale.
    if (low.compare(0, 8, L"\\device\\") == 0) {
        size_t end = low.find(L'\\', 8);
        std::wstring prefix = (end == std::wstring::npos) ? low : low.substr(0, end);
        bool hasSubPath = (end != std::wstring::npos && end + 1 < low.size());
        auto it = unmapped_.find(prefix);
        if (!hasSubPath) return nt;    // bare volume open, not a file
        if (it == unmapped_.end()) {
            unmapped_.emplace(prefix, 1);
            LOGW(L"unmapped device prefix '%s' (example: %s)", prefix.c_str(), nt.c_str());
            RefreshDeviceMap();
            // Retry once against the refreshed map.
            for (const auto& kv : devMap_) {
                const std::wstring& dev = kv.first;
                if (low.size() > dev.size() && low.compare(0, dev.size(), dev) == 0 &&
                    low[dev.size()] == L'\\') {
                    LOGI(L"  resolved after refresh -> %s", kv.second.c_str());
                    return kv.second + nt.substr(dev.size());
                }
            }
        } else {
            ++it->second;
        }
    }
    return nt;
}

// ---------------------------------------------------------------- session ctrl

static size_t PropsSize(const std::wstring& name) {
    return sizeof(EVENT_TRACE_PROPERTIES) + (name.size() + 1) * sizeof(wchar_t) * 2 + 64;
}

static EVENT_TRACE_PROPERTIES* MakeProps(std::vector<BYTE>& buf, const EtwConfig& c) {
    buf.assign(PropsSize(c.sessionName), 0);
    auto* p = (EVENT_TRACE_PROPERTIES*)buf.data();
    p->Wnode.BufferSize = (ULONG)buf.size();
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 2;                 // timestamps as system time (FILETIME)
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->BufferSize = c.bufferSizeKb;
    p->MinimumBuffers = c.minBuffers;
    p->MaximumBuffers = c.maxBuffers;
    p->FlushTimer = c.flushTimerSec;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    p->LogFileNameOffset = 0;
    return p;
}

bool EtwMonitor::Start(std::wstring& err) {
    if (running_.load()) return true;

    std::vector<BYTE> buf;
    auto* props = MakeProps(buf, cfg_);

    // A session survives a crash of this process; clear any leftover first.
    ControlTraceW(0, cfg_.sessionName.c_str(), props, EVENT_TRACE_CONTROL_STOP);

    props = MakeProps(buf, cfg_);
    LOGI(L"StartTrace: session='%s' bufSize=%luKB min=%lu max=%lu flush=%lus",
         cfg_.sessionName.c_str(), cfg_.bufferSizeKb, cfg_.minBuffers,
         cfg_.maxBuffers, cfg_.flushTimerSec);
    ULONG st = StartTraceW(&session_, cfg_.sessionName.c_str(), props);
    LOGI(L"StartTrace -> %lu (handle=%llu)", st, (unsigned long long)session_);
    if (st != ERROR_SUCCESS) {
        if (st == ERROR_ACCESS_DENIED) {
            err = L"Access denied starting the ETW session.\n\n"
                  L"Starting a trace needs membership of Administrators or of "
                  L"Performance Log Users. Either run this elevated, or add "
                  L"yourself to that group and sign out and back in:\n\n"
                  L"    net localgroup \"Performance Log Users\" %USERNAME% /add\n\n"
                  L"The folder tree still works, but the counters will stay at zero.";
        } else if (st == ERROR_ALREADY_EXISTS) {
            err = L"An ETW session with this name is already running. Stop it with:\n\n"
                  L"    logman stop Vigiles -ets";
        } else {
            err = L"StartTrace failed, Win32 error " + std::to_wstring(st);
        }
        return false;
    }

    ULONGLONG keywords = KF_FILENAME | KF_FILEIO | KF_CREATE | KF_READ | KF_WRITE |
                         KF_DELETE_PATH | KF_RENAME_SETLINK_PATH | KF_CREATE_NEW_FILE;

    ENABLE_TRACE_PARAMETERS ep{};
    ep.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    ep.EnableProperty = EVENT_ENABLE_PROPERTY_PROCESS_START_KEY;

    st = EnableTraceEx2(session_, &kKernelFileGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        TRACE_LEVEL_INFORMATION, keywords, 0, 0, &ep);
    LOGI(L"EnableTraceEx2(keywords=0x%llX) -> %lu", keywords, st);
    if (st != ERROR_SUCCESS) {
        err = L"EnableTraceEx2 failed, Win32 error " + std::to_wstring(st);
        StopSession();
        return false;
    }

    // Ask the provider to replay its current file-name table so that handles
    // opened before we started can still be resolved to a path.
    EnableTraceEx2(session_, &kKernelFileGuid, EVENT_CONTROL_CODE_CAPTURE_STATE,
                   TRACE_LEVEL_INFORMATION, keywords, 0, 0, &ep);

    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = (LPWSTR)cfg_.sessionName.c_str();
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    lf.EventRecordCallback = &EtwMonitor::EventRecordCb;
    lf.BufferCallback = &EtwMonitor::BufferCb;
    lf.Context = this;

    trace_ = OpenTraceW(&lf);
    LOGI(L"OpenTrace -> %llu", (unsigned long long)trace_);
    if (trace_ == (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        err = L"OpenTrace failed, Win32 error " + std::to_wstring(GetLastError());
        StopSession();
        return false;
    }

    running_.store(true);
    worker_ = std::thread([this] {
        LOGI(L"ProcessTrace thread started");
        TRACEHANDLE h = trace_;
        ULONG r = ProcessTrace(&h, 1, nullptr, nullptr);   // blocks until CloseTrace
        LOGI(L"ProcessTrace returned %lu after %llu events", r, TotalEvents());
        running_.store(false);
    });
    LOGI(L"tracing started, ring capacity %zu records", ring_.capacity());
    return true;
}

void EtwMonitor::StopSession() {
    if (session_) {
        std::vector<BYTE> buf;
        auto* props = MakeProps(buf, cfg_);
        ControlTraceW(session_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
}

void EtwMonitor::Stop() {
    LOGI(L"EtwMonitor::Stop (total=%llu kernelLost=%llu ringDropped=%llu)",
         TotalEvents(), KernelLost(), RingDropped());
    if (trace_ != (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        CloseTrace(trace_);                      // makes ProcessTrace return
        trace_ = (TRACEHANDLE)INVALID_HANDLE_VALUE;
    }
    if (worker_.joinable()) worker_.join();
    StopSession();
    running_.store(false);
}

ULONG WINAPI EtwMonitor::BufferCb(PEVENT_TRACE_LOGFILEW log) {
    auto* self = (EtwMonitor*)log->Context;
    if (self && log->EventsLost)
        self->kernelLost_.store(log->EventsLost, std::memory_order_relaxed);
    return TRUE;   // keep processing
}

void WINAPI EtwMonitor::EventRecordCb(PEVENT_RECORD rec) {
    ((EtwMonitor*)rec->UserContext)->OnEvent(rec);
}

// ---------------------------------------------------------------- decoding

uint8_t EtwMonitor::OpForEvent(PEVENT_RECORD rec) {
    uint32_t key = ((uint32_t)rec->EventHeader.EventDescriptor.Id << 8) |
                   rec->EventHeader.EventDescriptor.Version;
    auto it = opCache_.find(key);
    if (it != opCache_.end()) return it->second;

    uint8_t op = Op_Other;
    ULONG size = 0;
    TdhGetEventInformation(rec, 0, nullptr, nullptr, &size);
    if (size) {
        std::vector<BYTE> b(size);
        auto* tei = (PTRACE_EVENT_INFO)b.data();
        if (TdhGetEventInformation(rec, 0, nullptr, tei, &size) == ERROR_SUCCESS &&
            tei->TaskNameOffset) {
            const wchar_t* task = (const wchar_t*)(b.data() + tei->TaskNameOffset);
            auto is = [&](const wchar_t* s) { return _wcsicmp(task, s) == 0; };
            if      (is(L"Create") || is(L"CreateNewFile")) op = Op_Create;
            else if (is(L"Read"))                            op = Op_Read;
            else if (is(L"Write"))                           op = Op_Write;
            else if (is(L"DeletePath") || is(L"SetDelete"))  op = Op_Delete;
            else if (is(L"RenamePath") || is(L"Rename") ||
                     is(L"SetLinkPath"))                     op = Op_Rename;
            else if (is(L"SetInformation"))                  op = Op_SetInfo;
            else if (is(L"Close") || is(L"Cleanup"))         op = Op_Close;
            else if (is(L"DirEnum") || is(L"DirNotify"))     op = Op_DirEnum;
            else if (is(L"NameCreate") || is(L"NameDelete") ||
                     is(L"FileRundown"))                     op = Op_NameMap;
        }
    }
    opCache_[key] = op;
    return op;
}

uint32_t EtwMonitor::InternName(const std::wstring& path) {
    {
        std::shared_lock<std::shared_mutex> lk(namesMx_);
        auto it = nameIds_.find(path);
        if (it != nameIds_.end()) return it->second;
    }
    std::unique_lock<std::shared_mutex> lk(namesMx_);
    auto it = nameIds_.find(path);
    if (it != nameIds_.end()) return it->second;
    uint32_t id = (uint32_t)names_.size();
    names_.push_back(path);
    nameIds_.emplace(path, id);
    return id;
}

bool EtwMonitor::GetName(uint32_t id, std::wstring& out) const {
    std::shared_lock<std::shared_mutex> lk(namesMx_);
    if (id >= names_.size()) return false;
    out = names_[id];
    return true;
}

// An exception escaping into ProcessTrace kills the process with no dialog,
// so everything the callback does is contained here.
void EtwMonitor::OnEvent(PEVENT_RECORD rec) {
    __try {
        OnEventGuarded(rec);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static LONG reported = 0;
        if (InterlockedIncrement(&reported) <= 20)
            LOGE(L"SEH in OnEvent, code=0x%08X eventId=%u ver=%u",
                 GetExceptionCode(), rec->EventHeader.EventDescriptor.Id,
                 rec->EventHeader.EventDescriptor.Version);
    }
}

void EtwMonitor::OnEventGuarded(PEVENT_RECORD rec) {
    try {
        OnEventImpl(rec);
    } catch (const std::exception& e) {
        static LONG reported = 0;
        if (InterlockedIncrement(&reported) <= 20) {
            wchar_t w[256] = {};
            MultiByteToWideChar(CP_ACP, 0, e.what(), -1, w, 255);
            LOGE(L"C++ exception in OnEvent: %s", w);
        }
    } catch (...) {
        LOGE(L"unknown exception in OnEvent");
    }
}

// A file name is only usable as a path if it is absolute. Directory
// enumeration events put the matched entry, or the search wildcard, in the
// same FileName property, and treating those as paths both invents bogus
// folders and destroys the FileObject -> path mapping for the real directory.
static bool IsAbsoluteName(const std::wstring& n) {
    if (n.size() >= 2 && n[1] == L':') return true;                 // C:\...
    if (n.size() >= 2 && n[0] == L'\\' && n[1] == L'\\') return true; // \\server\...
    if (n.size() >= 8 && _wcsnicmp(n.c_str(), L"\\Device\\", 8) == 0) return true;
    if (n.size() >= 4 && _wcsnicmp(n.c_str(), L"\\??\\", 4) == 0) return true;
    return false;
}

void EtwMonitor::OnEventImpl(PEVENT_RECORD rec) {
    // Positive filter: only Kernel-File records. This also discards the
    // EventTrace header/rundown records the session emits, without needing
    // EventTraceGuid (which is extern in evntrace.h and only defined when
    // INITGUID is set before the include).
    if (!IsEqualGUID(rec->EventHeader.ProviderId, kKernelFileGuid)) return;

    uint8_t op = OpForEvent(rec);
    total_.fetch_add(1, std::memory_order_relaxed);

    uint64_t fileObject = 0, fileKey = 0, ioSize = 0;
    GetPropU64(rec, L"FileObject", fileObject);
    GetPropU64(rec, L"FileKey", fileKey);

    std::wstring raw;
    bool haveName = GetPropStr(rec, L"FileName", raw) ||
                    GetPropStr(rec, L"FilePath", raw);

    uint32_t id = UINT32_MAX;
    std::wstring path;

    if (haveName && IsAbsoluteName(raw)) {
        // Authoritative: this event tells us what the handle refers to.
        path = DevicePathToDos(raw);
        id = InternName(path);
        if (fileObject) objToName_[fileObject] = id;
        if (fileKey)    keyToName_[fileKey] = id;
    } else {
        // No name, or a relative one. Resolve the handle instead, and leave the
        // maps alone. For a directory enumeration the event is attributed to
        // the directory being enumerated, which is what we want anyway.
        if (fileObject) {
            auto it = objToName_.find(fileObject);
            if (it != objToName_.end()) id = it->second;
        }
        if (id == UINT32_MAX && fileKey) {
            auto it = keyToName_.find(fileKey);
            if (it != keyToName_.end()) id = it->second;
        }
        if (id != UINT32_MAX) GetName(id, path);
    }

    if (op == Op_NameMap) return;      // bookkeeping event, not user activity
    if (op == Op_Close && fileObject) objToName_.erase(fileObject);
    if (id == UINT32_MAX || path.empty()) {
        unresolved_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    GetPropU64(rec, L"IOSize", ioSize);

    FileEvent ev{};
    ev.timestamp = (uint64_t)rec->EventHeader.TimeStamp.QuadPart;
    ev.nameId = id;
    ev.processId = rec->EventHeader.ProcessId;
    ev.ioSize = (uint32_t)ioSize;
    ev.op = op;

    {
        std::lock_guard<std::mutex> lk(dirs_.mutex());
        const auto& chain = dirs_.chainFor(id, path);
        uint64_t now = GetTickCount64();
        for (size_t i = 0; i < chain.size(); ++i) {
            if (i == 0) {
                DirStats& s = chain[i]->self;
                if (op == Op_Read)       { ++s.reads;  s.readBytes  += ioSize; }
                else if (op == Op_Write) { ++s.writes; s.writeBytes += ioSize; }
                else if (op == Op_Create) ++s.creates;
                else if (op == Op_Delete) ++s.deletes;
                else                      ++s.other;
                s.lastTick = now;
            }
            DirStats& t = chain[i]->subtree;
            if (op == Op_Read)       { ++t.reads;  t.readBytes  += ioSize; }
            else if (op == Op_Write) { ++t.writes; t.writeBytes += ioSize; }
            else if (op == Op_Create) ++t.creates;
            else if (op == Op_Delete) ++t.deletes;
            else                      ++t.other;
            t.lastTick = now;
        }
    }
    ring_.push(ev);
}
