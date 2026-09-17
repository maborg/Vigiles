#pragma once
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include "dir_index.h"
#include "ring_buffer.h"

enum FileOp : uint8_t {
    Op_Create = 0, Op_Read, Op_Write, Op_Delete, Op_Rename,
    Op_SetInfo, Op_Close, Op_DirEnum, Op_Other, Op_NameMap, Op_COUNT
};

const wchar_t* OpName(uint8_t op);

// 32 bytes. Names are interned, so records stay small and the ring can be huge.
struct FileEvent {
    uint64_t timestamp;    // FILETIME (session started with ClientContext = system time)
    uint32_t nameId;
    uint32_t processId;
    uint32_t ioSize;
    uint8_t  op;
    uint8_t  pad[3];
};

struct EtwConfig {
    std::wstring sessionName = L"Vigiles";
    ULONG bufferSizeKb   = 64;
    ULONG minBuffers     = 64;
    ULONG maxBuffers     = 512;   // 512 * 64 KB = 32 MB of kernel buffers
    ULONG flushTimerSec  = 1;
    size_t ringCapacity  = 1u << 20;  // 1M records ~= 32 MB
};

class EtwMonitor {
public:
    EtwMonitor();
    ~EtwMonitor();

    bool Start(std::wstring& errorOut);
    void Stop();
    bool Running() const { return running_.load(); }

    RingBuffer<FileEvent>& Ring() { return ring_; }
    DirIndex& Dirs() { return dirs_; }

    bool GetName(uint32_t id, std::wstring& out) const;

    uint64_t TotalEvents() const { return total_.load(std::memory_order_relaxed); }
    uint64_t KernelLost()  const { return kernelLost_.load(std::memory_order_relaxed); }
    uint64_t Unresolved()  const { return unresolved_.load(std::memory_order_relaxed); }
    uint64_t RingDropped() const { return ring_.overwritten(); }
    size_t   NameCount() const {
        std::shared_lock<std::shared_mutex> lk(namesMx_);
        return names_.size();
    }

private:
    static void WINAPI EventRecordCb(PEVENT_RECORD rec);
    static ULONG WINAPI BufferCb(PEVENT_TRACE_LOGFILEW log);
    void OnEvent(PEVENT_RECORD rec);        // SEH guard
    void OnEventGuarded(PEVENT_RECORD rec); // C++ exception guard
    void OnEventImpl(PEVENT_RECORD rec);    // real work

    uint8_t OpForEvent(PEVENT_RECORD rec);
    uint32_t InternName(const std::wstring& path);
    std::wstring DevicePathToDos(const std::wstring& nt);   // not const: may refresh the map
    void BuildDeviceMap();
    void RefreshDeviceMap();
    void StopSession();

    EtwConfig cfg_;
    TRACEHANDLE session_ = 0;
    TRACEHANDLE trace_ = (TRACEHANDLE)INVALID_HANDLE_VALUE;
    std::thread worker_;
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> total_{0};
    std::atomic<uint64_t> kernelLost_{0};
    std::atomic<uint64_t> unresolved_{0};   // events whose handle we never saw named

    RingBuffer<FileEvent> ring_;
    DirIndex dirs_;

    // name interning
    mutable std::shared_mutex namesMx_;
    std::vector<std::wstring> names_;
    std::unordered_map<std::wstring, uint32_t> nameIds_;

    // handle/key -> name id (only touched from the ETW thread)
    std::unordered_map<uint64_t, uint32_t> objToName_;
    std::unordered_map<uint64_t, uint32_t> keyToName_;

    // event id/version -> operation
    std::unordered_map<uint32_t, uint8_t> opCache_;

    std::vector<std::pair<std::wstring, std::wstring>> devMap_; // "\device\harddiskvolume3" -> "C:"
    std::unordered_map<std::wstring, uint32_t> unmapped_;  // device prefixes we could not translate
    uint64_t lastDevRefresh_ = 0;

};
