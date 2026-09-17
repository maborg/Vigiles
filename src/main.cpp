// Vigiles - ubi fumus, ibi ignis
//
// Named for the vigiles urbani, the night watch of ancient Rome, who patrolled
// the city looking for fires. This one watches a filesystem instead: folders
// glow while they are busy, and the motto is the job description - you see the
// smoke here, then go and find the fire.

#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <cmath>
#include "etw_monitor.h"
#include "log.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")

#define IDC_TREE   1001
#define IDC_LIST   1002
#define IDC_STATUS 1003
#define IDC_LEGEND 1004
#define IDC_PROMPT 1005
#define IDM_PAUSE  2001
#define IDM_CLEAR  2002
#define IDM_EXIT   2003
#define IDM_PROMPT 2004
#define TIMER_UI   1

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

// Which operation moved the most between two snapshots. Returns 0xFF if the
// folder saw nothing new, so the previous hue is kept while the tint fades.
static uint8_t DominantOp(const DirStats& now, const DirStats& prev) {
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

struct Row {
    uint64_t ts;
    uint32_t pid;
    uint32_t size;
    uint8_t  op;
    std::wstring path;
};

static HWND g_hwnd, g_tree, g_list, g_status, g_legend, g_promptBtn;
static const int kLegendH = 30;
static const int kPromptBtnW = 210;
static EtwMonitor g_mon;
static std::deque<Row> g_rows;
static const size_t kMaxRows = 2000;   // live view only, no history kept

// Activity heat: how long a folder stays tinted after its last event, and how
// saturated the tint is at its peak. The fade is eased so it holds colour for
// most of the window and drops off at the end, which makes a burst much easier
// to follow than a linear ramp.
static const uint64_t kHeatMs = 5000;

// Pastel palette, indexed by FileOp. Row colours are kept very light so the
// dark text stays readable; the tree tints are the same hues pushed a little
// further, because they get blended back toward white as the heat fades.
static COLORREF OpRowColor(uint8_t op) {
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

static COLORREF OpHeatColor(uint8_t op) {
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
static COLORREF FadeToWhite(COLORREF c, double strength) {
    if (strength < 0) strength = 0;
    if (strength > 1) strength = 1;
    int r = 255 - (int)((255 - GetRValue(c)) * strength + 0.5);
    int g = 255 - (int)((255 - GetGValue(c)) * strength + 0.5);
    int b = 255 - (int)((255 - GetBValue(c)) * strength + 0.5);
    return RGB(r, g, b);
}
static bool g_paused = false;
static std::wstring g_flash;      // transient status text
static uint64_t g_flashUntil = 0;
static std::wstring g_filter;          // lowercase prefix from the selected folder
static uint64_t g_lastTotal = 0, g_lastTick = 0;

// ------------------------------------------------------------------ utilities

static std::wstring FormatCount(uint64_t v) {
    wchar_t b[32];
    if (v < 1000)            swprintf_s(b, L"%llu", v);
    else if (v < 1000000)    swprintf_s(b, L"%.1fK", v / 1000.0);
    else if (v < 1000000000) swprintf_s(b, L"%.1fM", v / 1000000.0);
    else                     swprintf_s(b, L"%.1fG", v / 1e9);
    return b;
}

static std::wstring FormatBytes(uint64_t v) {
    wchar_t b[32];
    if (v < 1024)              swprintf_s(b, L"%llu B", v);
    else if (v < 1024ull*1024) swprintf_s(b, L"%.1f KB", v / 1024.0);
    else if (v < 1024ull*1024*1024) swprintf_s(b, L"%.1f MB", v / 1048576.0);
    else                       swprintf_s(b, L"%.2f GB", v / 1073741824.0);
    return b;
}

static std::wstring FormatTime(uint64_t filetime) {
    FILETIME ft; ft.dwLowDateTime = (DWORD)filetime; ft.dwHighDateTime = (DWORD)(filetime >> 32);
    FILETIME lft; SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &lft) || !FileTimeToSystemTime(&lft, &st)) return L"--";
    wchar_t b[32];
    swprintf_s(b, L"%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return b;
}

static ItemData* GetItemData(HTREEITEM h) {
    if (!h) return nullptr;
    TVITEMW it{}; it.mask = TVIF_PARAM; it.hItem = h;
    if (!TreeView_GetItem(g_tree, &it)) return nullptr;
    return (ItemData*)it.lParam;
}

// ------------------------------------------------------------------ tree build

static HTREEITEM InsertNode(HTREEITEM parent, const std::wstring& path,
                            const std::wstring& label, bool maybeChildren) {
    auto* d = new ItemData{ path, label, false };
    TVINSERTSTRUCTW is{};
    is.hParent = parent;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    is.item.pszText = (LPWSTR)d->label.c_str();
    is.item.lParam = (LPARAM)d;
    is.item.cChildren = maybeChildren ? 1 : 0;
    return TreeView_InsertItem(g_tree, &is);
}

static void PopulateChildren(HTREEITEM parent) {
    ItemData* d = GetItemData(parent);
    if (!d || d->populated) return;
    d->populated = true;

    std::wstring base = d->path;
    if (base.empty()) return;
    if (base.back() != L'\\') base += L'\\';

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchLimitToDirectories, nullptr, 0);
    std::vector<std::wstring> dirs;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            dirs.emplace_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(dirs.begin(), dirs.end(),
              [](const std::wstring& a, const std::wstring& b) {
                  return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
              });

    for (const auto& name : dirs)
        InsertNode(parent, base + name, name, true);

    if (dirs.empty()) {   // remove the expand arrow
        TVITEMW it{}; it.mask = TVIF_CHILDREN | TVIF_HANDLE;
        it.hItem = parent; it.cChildren = 0;
        TreeView_SetItem(g_tree, &it);
    }
}

static void AddDrives() {
    wchar_t buf[512] = {};
    if (!GetLogicalDriveStringsW(_countof(buf), buf)) return;
    for (wchar_t* p = buf; *p; p += wcslen(p) + 1) {
        UINT t = GetDriveTypeW(p);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE && t != DRIVE_RAMDISK && t != DRIVE_REMOTE)
            continue;
        wchar_t vol[MAX_PATH] = {};
        GetVolumeInformationW(p, vol, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        std::wstring label = std::wstring(p, 2);          // "C:"
        if (vol[0]) label += std::wstring(L" (") + vol + L")";
        InsertNode(TVI_ROOT, p, label, true);
    }
}

// ------------------------------------------------------------------ navigation

// Walks the tree down to `folder`, creating the nodes along the way. The tree
// is populated lazily, so the target node almost never exists yet: each level
// has to be filled and expanded before its child can be found.
//
// Returns the deepest node reached. That is not always the one asked for - a
// folder can be gone by the time you double-click the row that mentioned it,
// and FindFirstFileEx skips what the user cannot list - so stopping partway and
// selecting the closest ancestor beats refusing to move at all.
static HTREEITEM RevealFolder(const std::wstring& folder, bool& exact) {
    exact = false;
    if (folder.size() < 3 || folder[1] != L':') return nullptr;

    // The drive root, matched case-insensitively: the tree stores the spelling
    // Windows reported for the volume, the event carries whatever the kernel
    // emitted, and the two disagree often.
    std::wstring wantRoot = ToLowerW(folder.substr(0, 3));
    HTREEITEM node = nullptr;
    for (HTREEITEM it = TreeView_GetRoot(g_tree); it;
         it = TreeView_GetNextSibling(g_tree, it)) {
        ItemData* d = GetItemData(it);
        if (d && ToLowerW(d->path) == wantRoot) { node = it; break; }
    }
    if (!node) return nullptr;

    size_t pos = 3;
    while (pos < folder.size()) {
        size_t end = folder.find(L'\\', pos);
        if (end == std::wstring::npos) end = folder.size();
        std::wstring want = ToLowerW(folder.substr(pos, end - pos));
        if (want.empty()) break;

        PopulateChildren(node);
        TreeView_Expand(g_tree, node, TVE_EXPAND);

        HTREEITEM match = nullptr;
        for (HTREEITEM c = TreeView_GetChild(g_tree, node); c;
             c = TreeView_GetNextSibling(g_tree, c)) {
            ItemData* cd = GetItemData(c);
            if (!cd) continue;
            size_t sep = cd->path.find_last_of(L'\\');
            std::wstring name = (sep == std::wstring::npos) ? cd->path
                                                            : cd->path.substr(sep + 1);
            if (ToLowerW(name) == want) { match = c; break; }
        }
        if (!match) return node;          // as deep as we can get

        node = match;
        pos = end + 1;
    }

    exact = true;
    return node;
}

// Double-clicking a row jumps the tree to the folder that row lives in.
// Selecting the node fires TVN_SELCHANGED, which clears the list and refilters,
// so the path has to be copied out of g_rows before that happens.
static void JumpToRow(int index) {
    if (index < 0 || (size_t)index >= g_rows.size()) return;
    std::wstring path = g_rows[g_rows.size() - 1 - (size_t)index].path;   // copy
    if (path.size() < 3) return;

    // The row may name a directory (a DirEnum event, say) rather than a file,
    // in which case that directory is the destination, not its parent. If the
    // path is gone the query fails and the parent is the right answer anyway.
    DWORD attr = GetFileAttributesW(path.c_str());
    std::wstring target = (attr != INVALID_FILE_ATTRIBUTES &&
                           (attr & FILE_ATTRIBUTE_DIRECTORY))
                          ? path : ParentPath(path);
    if (target.empty()) return;

    bool exact = false;
    HTREEITEM node = RevealFolder(target, exact);
    if (!node) return;

    TreeView_EnsureVisible(g_tree, node);
    TreeView_SelectItem(g_tree, node);
    SetFocus(g_tree);

    if (!exact) {
        ItemData* d = GetItemData(node);
        g_flash = L"  Folder is gone - stopped at " +
                  (d ? d->path : std::wstring(L"?"));
        g_flashUntil = GetTickCount64() + 4000;
    }
}

// ------------------------------------------------------------------ UI refresh

// Rewrites the caption of every currently visible tree row with its subtree
// counters. Bounded by the number of rows on screen, so it stays cheap.
static void RefreshVisibleTree() {
    // Pass 1: collect what is on screen (no lock held).
    struct Vis { HTREEITEM h; ItemData* d; };
    static std::vector<Vis> vis;
    vis.clear();
    HTREEITEM h = TreeView_GetFirstVisible(g_tree);
    int guard = 0;
    while (h && guard++ < 500) {
        ItemData* d = GetItemData(h);
        if (d) vis.push_back({ h, d });
        h = TreeView_GetNextVisible(g_tree, h);
    }

    // Pass 2: snapshot the counters under the lock, and nothing else.
    {
        std::lock_guard<std::mutex> lk(g_mon.Dirs().mutex());
        for (auto& v : vis) {
            DirNode* n = g_mon.Dirs().get(v.d->path);
            if (n) { v.d->cached = n->subtree; v.d->hasStats = true; }
            else   { v.d->hasStats = false; }
        }
    }

    // Pass 3: touch the tree control with the lock released. TreeView_SetItem
    // can generate notifications, and re-entering the lock here would hang.
    for (auto& v : vis) {
        if (v.d->hasStats) {
            if (v.d->hasPrev) {
                uint8_t dom = DominantOp(v.d->cached, v.d->prev);
                if (dom != 0xFF) v.d->heatOp = dom;
            }
            v.d->prev = v.d->cached;
            v.d->hasPrev = true;
        }
        std::wstring text = v.d->label;
        if (v.d->hasStats) {
            const DirStats& s = v.d->cached;
            uint64_t total = s.reads + s.writes + s.creates + s.deletes + s.other;
            if (total) {
                text += L"   [R " + FormatCount(s.reads) +
                        L" / W " + FormatCount(s.writes) +
                        L" / " + FormatBytes(s.readBytes + s.writeBytes) + L"]";
            }
        }
        wchar_t cur[512] = {};
        TVITEMW it{}; it.mask = TVIF_TEXT | TVIF_HANDLE; it.hItem = v.h;
        it.pszText = cur; it.cchTextMax = 512;
        if (TreeView_GetItem(g_tree, &it) && text != cur) {
            TVITEMW set{}; set.mask = TVIF_TEXT | TVIF_HANDLE;
            set.hItem = v.h; set.pszText = (LPWSTR)text.c_str();
            TreeView_SetItem(g_tree, &set);
        }
    }
    InvalidateRect(g_tree, nullptr, FALSE);
}

// Prefix matching alone would let "c:\\windows" match "c:\\windowsapps", so the
// character after the prefix has to be a separator (unless the filter is a
// drive root, which already ends in one).
static bool MatchesFilter(const std::wstring& lowPath) {
    if (g_filter.empty()) return true;
    if (lowPath.size() < g_filter.size()) return false;
    if (lowPath.compare(0, g_filter.size(), g_filter) != 0) return false;
    if (g_filter.back() == L'\\') return true;
    return lowPath.size() > g_filter.size() && lowPath[g_filter.size()] == L'\\';
}

// Switching folders starts a fresh view: drop the rows we were showing and
// discard whatever is already queued, so the list only fills with events that
// happen from now on.
static void SetFilter(const std::wstring& displayPath) {
    g_filter = ToLowerW(displayPath);
    g_rows.clear();

    static std::vector<FileEvent> discard;
    while (g_mon.Ring().drain(discard, 65536) != 0) {}

    ListView_SetItemCountEx(g_list, 0, 0);
    InvalidateRect(g_list, nullptr, TRUE);

    std::wstring header = displayPath.empty()
        ? std::wstring(L"Path  (all folders)")
        : L"Path  (" + displayPath + L")";
    LVCOLUMNW c{}; c.mask = LVCF_TEXT; c.pszText = (LPWSTR)header.c_str();
    ListView_SetColumn(g_list, 4, &c);

}

static void DrainEvents() {
    static std::vector<FileEvent> batch;
    size_t n = g_mon.Ring().drain(batch, 4000);
    if (!g_paused) {
        for (size_t i = 0; i < n; ++i) {
            const FileEvent& e = batch[i];
            std::wstring path;
            if (!g_mon.GetName(e.nameId, path)) continue;
            if (!MatchesFilter(ToLowerW(path))) continue;
            Row r; r.ts = e.timestamp; r.pid = e.processId;
            r.size = e.ioSize; r.op = e.op; r.path = std::move(path);
            g_rows.push_back(std::move(r));
        }
        while (g_rows.size() > kMaxRows) g_rows.pop_front();
        ListView_SetItemCountEx(g_list, (int)g_rows.size(),
                                LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
        InvalidateRect(g_list, nullptr, FALSE);
    }
}

static void UpdateStatus() {
    uint64_t now = GetTickCount64();
    uint64_t total = g_mon.TotalEvents();
    double rate = 0;
    if (g_lastTick && now > g_lastTick)
        rate = (double)(total - g_lastTotal) * 1000.0 / (double)(now - g_lastTick);
    g_lastTick = now; g_lastTotal = total;

    wchar_t b[256];
    swprintf_s(b, L"  %s events  |  %.0f/s", FormatCount(total).c_str(), rate);
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)b);
    swprintf_s(b, L"  Ring %u%%  dropped %s",
               (unsigned)(g_mon.Ring().size() * 100 / g_mon.Ring().capacity()),
               FormatCount(g_mon.RingDropped()).c_str());
    SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)b);
    swprintf_s(b, L"  Kernel lost: %s  unresolved: %s",
               FormatCount(g_mon.KernelLost()).c_str(),
               FormatCount(g_mon.Unresolved()).c_str());
    SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)b);
    if (!g_flash.empty() && now < g_flashUntil) {
        SendMessageW(g_status, SB_SETTEXTW, 3, (LPARAM)g_flash.c_str());
    } else {
        g_flash.clear();
        swprintf_s(b, g_paused ? L"  PAUSED" : L"  Tracing");
        SendMessageW(g_status, SB_SETTEXTW, 3, (LPARAM)b);
    }
}

// ------------------------------------------------------------------ layout

// ---------------------------------------------------------------- AI prompt
//
// The button below builds a prompt describing what is happening in the folder
// currently selected in the tree, and puts it on the clipboard. The captured
// rows are NOT emitted one by one: a few thousand near identical lines tell a
// model nothing that counts do not, and they blow past any context window. So
// everything is collapsed into per-process, per-path, per-subfolder and
// per-extension totals, each printed once with its count.

static std::unordered_map<uint32_t, std::wstring> g_procNames;

// PID -> image name, cached. Resolution can fail for a process that has already
// exited, or for a protected one; the number is kept as the label in that case.
static const std::wstring& ProcessName(uint32_t pid) {
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

struct Agg {
    std::wstring label;           // first spelling seen, for display
    uint64_t count = 0;
    uint64_t bytes = 0;
    uint32_t opMask = 0;          // bit per FileOp, so one line can say R/W/C
};

// Windows reports the same file with inconsistent casing: the kernel emits both
// \WINDOWS\SYSTEM32\ and \Windows\System32\ for the same path, sometimes
// within the same second. Keying on the lowercase form keeps those from
// splitting into separate rows, while the first spelling seen is what gets
// printed.
typedef std::map<std::wstring, Agg> AggMap;

static void Accumulate(AggMap& m, const std::wstring& key, const Row& r) {
    Agg& a = m[ToLowerW(key)];
    if (a.label.empty()) a.label = key;
    ++a.count;
    a.bytes += r.size;
    a.opMask |= (1u << r.op);
}

static std::wstring OpsFromMask(uint32_t mask) {
    std::wstring out;
    for (uint8_t op = 0; op < Op_COUNT; ++op) {
        if (!(mask & (1u << op))) continue;
        if (!out.empty()) out += L"/";
        out += OpName(op);
    }
    return out;
}

static std::wstring Extension(const std::wstring& path) {
    size_t slash = path.find_last_of(L'\\');
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash))
        return L"(no extension)";
    return ToLowerW(path.substr(dot));
}

// The immediate child of `base` that `path` sits under, so the prompt can say
// which branch of the selected folder is busy rather than listing leaves.
static std::wstring SubBranch(const std::wstring& base, const std::wstring& path) {
    if (base.empty() || path.size() <= base.size()) return L"(root)";
    size_t start = base.size();
    if (base.back() != L'\\') ++start;
    if (start >= path.size()) return L"(root)";
    size_t end = path.find(L'\\', start);
    if (end == std::wstring::npos) return L"(root)";   // a file directly here
    return path.substr(start, end - start);
}

static std::vector<Agg> Ranked(const AggMap& m) {
    std::vector<Agg> v;
    v.reserve(m.size());
    for (const auto& kv : m) v.push_back(kv.second);
    std::sort(v.begin(), v.end(), [](const Agg& a, const Agg& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.label < b.label;
    });
    return v;
}

static void EmitSection(std::wostringstream& o, const std::vector<Agg>& v,
                        size_t limit, bool withOps) {
    size_t shown = 0;
    for (const Agg& a : v) {
        if (shown++ >= limit) {
            o << L"- (+" << (v.size() - limit) << L" less frequent entries omitted)\r\n";
            break;
        }
        o << L"- " << a.label << L"  x" << a.count;
        if (withOps) o << L"  [" << OpsFromMask(a.opMask) << L"]";
        if (a.bytes) o << L"  " << FormatBytes(a.bytes);
        o << L"\r\n";
    }
    if (v.empty()) o << L"- (no data)\r\n";
}

static std::wstring BuildPrompt() {
    if (g_rows.empty()) return std::wstring();

    HTREEITEM sel = TreeView_GetSelection(g_tree);
    ItemData* d = GetItemData(sel);
    std::wstring folder = d ? d->path : std::wstring(L"(all drives)");

    AggMap byProc, byPath, byBranch, byExt;
    uint64_t totalBytes = 0, reads = 0, writes = 0;

    for (const Row& r : g_rows) {
        Accumulate(byProc, ProcessName(r.pid), r);
        Accumulate(byPath, r.path, r);
        Accumulate(byExt, Extension(r.path), r);
        if (d) Accumulate(byBranch, SubBranch(d->path, r.path), r);
        totalBytes += r.size;
        if (r.op == Op_Read) ++reads;
        else if (r.op == Op_Write) ++writes;
    }

    // Exact cumulative counters for this folder. Unlike the rows above these
    // are never dropped, but they run from application start, not from the
    // moment the folder was selected - so they are labelled separately.
    DirStats cum{};
    bool haveCum = false;
    if (d) {
        std::lock_guard<std::mutex> lk(g_mon.Dirs().mutex());
        if (DirNode* n = g_mon.Dirs().get(d->path)) { cum = n->subtree; haveCum = true; }
    }

    std::wostringstream o;
    o << L"Analyse the disk activity of a Windows machine, captured live with "
         L"ETW (Microsoft-Windows-Kernel-File provider).\r\n\r\n"
      << L"## Context\r\n"
      << L"- Folder watched: " << folder << L"  (subfolders included)\r\n"
      << L"- Time window: " << FormatTime(g_rows.front().ts)
      << L" - " << FormatTime(g_rows.back().ts) << L"\r\n"
      << L"- Operations in sample: " << g_rows.size()
      << L"  (reads " << reads << L", writes " << writes << L")\r\n"
      << L"- Total I/O in sample: " << FormatBytes(totalBytes) << L"\r\n";
    if (haveCum) {
        o << L"- Cumulative counters for this folder since the monitor started: "
          << L"read " << cum.reads << L", write " << cum.writes
          << L", create " << cum.creates << L", delete " << cum.deletes
          << L", other " << cum.other
          << L"  (" << FormatBytes(cum.readBytes + cum.writeBytes) << L")\r\n";
    }
    o << L"\r\nThe data below is AGGREGATED: each line is a count of identical or "
         L"similar events, not a single event. 'xN' is how many times, and the "
         L"square brackets list the operation types seen on that entry.\r\n";

    o << L"\r\n## Processes responsible\r\n";
    EmitSection(o, Ranked(byProc), 12, true);

    if (d && byBranch.size() > 1) {
        o << L"\r\n## Busiest subfolders\r\n";
        EmitSection(o, Ranked(byBranch), 12, true);
    }

    o << L"\r\n## Most touched files\r\n";
    EmitSection(o, Ranked(byPath), 20, true);

    o << L"\r\n## Extensions\r\n";
    EmitSection(o, Ranked(byExt), 10, false);

    o << L"\r\n## What I need\r\n"
      << L"1. **Cause**: what is generating this activity? Identify the process, "
         L"service or scheduled task responsible and explain what the work it is "
         L"doing is for.\r\n"
      << L"2. **Normal or not**: is this expected behaviour, or does it point at a "
         L"problem (a loop, runaway indexing, a log growing unchecked, a stuck "
         L"update, unwanted software)? Justify the answer from the counts above.\r\n"
      << L"3. **How to intervene**: how can this activity be stopped, reduced or "
         L"reconfigured? Give concrete commands, settings, registry keys, group "
         L"policies or exclusions, and for each option the risks and what is lost "
         L"by applying it.\r\n"
      << L"4. **Web search**: look online for current information about the "
         L"processes and paths listed above (they are real Windows files and "
         L"services) and cite your sources.\r\n";

    return o.str();
}

static bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* p = GlobalLock(mem)) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);   // ownership stays with us on failure
        }
    }
    CloseClipboard();
    return ok;
}

static void OnCopyPrompt(HWND hwnd) {
    std::wstring prompt = BuildPrompt();
    if (prompt.empty()) {
        MessageBoxW(hwnd,
            L"No activity recorded for the selected folder.\r\n\r\n"
            L"Pick a folder in the tree, give it a few seconds to collect "
            L"events, then try again.",
            L"AI prompt", MB_ICONINFORMATION | MB_OK);
        return;
    }
    if (CopyToClipboard(hwnd, prompt)) {
        wchar_t msg[128];
        swprintf_s(msg, L"  AI prompt copied (%zu chars)", prompt.size());
        g_flash = msg;
        g_flashUntil = GetTickCount64() + 4000;
        SendMessageW(g_status, SB_SETTEXTW, 3, (LPARAM)msg);
    } else {
        LOGE(L"clipboard copy failed: %lu", GetLastError());
        MessageBoxW(hwnd, L"Could not open the clipboard: another application "
                          L"is holding it. Try again in a moment.",
                    L"AI prompt", MB_ICONERROR | MB_OK);
    }
}

// The legend is an owner-drawn static: a swatch per operation, filled with the
// colour used for list rows and outlined with the stronger tone used for the
// tree tint, so both encodings read as the same thing.
static void PaintLegend(LPDRAWITEMSTRUCT di) {
    static const uint8_t kOps[] = { Op_Create, Op_Read, Op_Write, Op_Delete,
                                    Op_Rename, Op_SetInfo, Op_DirEnum, Op_Other };

    HDC dc = di->hDC;
    FillRect(dc, &di->rcItem, GetSysColorBrush(COLOR_BTNFACE));

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT old = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int mid = (di->rcItem.bottom + di->rcItem.top) / 2;
    int sw = 12;                       // swatch size
    int x = 10;

    for (uint8_t op : kOps) {
        if (x + 90 > di->rcItem.right) break;   // ran out of room

        RECT sr{ x, mid - sw / 2, x + sw, mid + sw / 2 };
        HBRUSH fill = CreateSolidBrush(OpRowColor(op));
        FillRect(dc, &sr, fill);
        DeleteObject(fill);
        HBRUSH edge = CreateSolidBrush(OpHeatColor(op));
        FrameRect(dc, &sr, edge);
        DeleteObject(edge);

        x += sw + 5;
        const wchar_t* label = OpName(op);
        SIZE ts{};
        GetTextExtentPoint32W(dc, label, (int)wcslen(label), &ts);
        TextOutW(dc, x, mid - tm.tmHeight / 2, label, (int)wcslen(label));
        x += ts.cx + 16;
    }

    SelectObject(dc, old);
}

static void Layout() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sb; GetWindowRect(g_status, &sb);
    int sbh = sb.bottom - sb.top;
    int h = rc.bottom - sbh - kLegendH;
    if (h < 0) h = 0;
    int split = (rc.right * 42) / 100;
    MoveWindow(g_tree, 0, 0, split, h, TRUE);
    MoveWindow(g_list, split + 4, 0, rc.right - split - 4, h, TRUE);
    int btnW = (rc.right > kPromptBtnW + 120) ? kPromptBtnW : 0;
    MoveWindow(g_legend, 0, h, rc.right - btnW, kLegendH, TRUE);
    if (btnW) {
        MoveWindow(g_promptBtn, rc.right - btnW + 4, h + 3, btnW - 8, kLegendH - 6, TRUE);
        ShowWindow(g_promptBtn, SW_SHOW);
    } else {
        ShowWindow(g_promptBtn, SW_HIDE);   // too narrow, menu item still works
    }
    InvalidateRect(g_legend, nullptr, TRUE);
    int parts[4] = { rc.right / 4, rc.right / 2, (rc.right * 3) / 4, -1 };
    SendMessageW(g_status, SB_SETPARTS, 4, (LPARAM)parts);
}

// ------------------------------------------------------------------ wndproc

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        LOG_SCOPE("WM_CREATE");
        g_hwnd = hwnd;
        g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES |
            TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0, 0, 0, 0, hwnd, (HMENU)IDC_TREE, nullptr, nullptr);

        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_NOSORTHEADER,
            0, 0, 0, 0, hwnd, (HMENU)IDC_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

        struct { const wchar_t* t; int w; } cols[] = {
            { L"Time", 100 }, { L"Op", 70 }, { L"PID", 60 },
            { L"Size", 80 }, { L"Path", 700 }
        };
        for (int i = 0; i < 5; ++i) {
            LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            c.pszText = (LPWSTR)cols[i].t; c.cx = cols[i].w; c.iSubItem = i;
            ListView_InsertColumn(g_list, i, &c);
        }

        g_legend = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_LEGEND, nullptr, nullptr);

        g_promptBtn = CreateWindowExW(0, L"BUTTON", L"Copy AI prompt",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_PROMPT, nullptr, nullptr);
        SendMessageW(g_promptBtn, WM_SETFONT,
                     (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_STATUS, nullptr, nullptr);

        LOGI(L"controls: tree=%p list=%p status=%p (lastErr=%lu)",
             g_tree, g_list, g_status, GetLastError());
        if (!g_tree || !g_list || !g_status) {
            LOGE(L"control creation failed - is comctl32 v6 in the manifest?");
            MessageBoxW(hwnd, L"Failed to create controls. See the log.",
                        L"Vigiles", MB_ICONERROR);
            return -1;
        }

        AddDrives();
        LOGI(L"tree populated with drives");

        std::wstring err;
        if (!g_mon.Start(err)) {
            LOGE(L"EtwMonitor::Start failed: %s", err.c_str());
            MessageBoxW(hwnd, err.c_str(), L"Tracing disabled", MB_ICONWARNING);
        }

        SetTimer(hwnd, TIMER_UI, 150, nullptr);
        return 0;
    }

    case WM_SIZE: Layout(); return 0;

    case WM_DRAWITEM:
        if (wp == IDC_LEGEND) {
            PaintLegend((LPDRAWITEMSTRUCT)lp);
            return TRUE;
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_UI) { DrainEvents(); RefreshVisibleTree(); UpdateStatus(); }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_PAUSE:
            g_paused = !g_paused;
            CheckMenuItem(GetMenu(hwnd), IDM_PAUSE,
                          MF_BYCOMMAND | (g_paused ? MF_CHECKED : MF_UNCHECKED));
            return 0;
        case IDM_CLEAR:
            g_rows.clear();
            ListView_SetItemCountEx(g_list, 0, 0);
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        case IDC_PROMPT:
        case IDM_PROMPT:
            OnCopyPrompt(hwnd);
            return 0;
        }
        return 0;

    case WM_NOTIFY: {
        auto* nh = (LPNMHDR)lp;

        if (nh->idFrom == IDC_TREE && nh->code == TVN_ITEMEXPANDINGW) {
            auto* tv = (LPNMTREEVIEWW)lp;
            if (tv->action == TVE_EXPAND) PopulateChildren(tv->itemNew.hItem);
            return 0;
        }
        if (nh->idFrom == IDC_TREE && nh->code == TVN_SELCHANGEDW) {
            auto* tv = (LPNMTREEVIEWW)lp;
            ItemData* d = GetItemData(tv->itemNew.hItem);
            SetFilter(d ? d->path : std::wstring());
            return 0;
        }
        if (nh->idFrom == IDC_TREE && nh->code == TVN_DELETEITEMW) {
            auto* tv = (LPNMTREEVIEWW)lp;
            delete (ItemData*)tv->itemOld.lParam;
            return 0;
        }
        // Heat colouring: folders touched in the last 3 seconds get a warm tint.
        if (nh->idFrom == IDC_TREE && nh->code == NM_CUSTOMDRAW) {
            auto* cd = (LPNMTVCUSTOMDRAW)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                ItemData* d = (ItemData*)cd->nmcd.lItemlParam;
                if (d && d->hasStats && d->cached.lastTick) {
                    uint64_t now = GetTickCount64();
                    uint64_t age = now > d->cached.lastTick ? now - d->cached.lastTick : 0;
                    if (age < kHeatMs) {
                        double t = 1.0 - (double)age / (double)kHeatMs;  // 1 -> 0
                        double strength = sqrt(t);   // holds colour, then falls away
                        cd->clrTextBk = FadeToWhite(OpHeatColor(d->heatOp), strength);
                    }
                }
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }

        if (nh->idFrom == IDC_LIST && nh->code == NM_CUSTOMDRAW) {
            auto* cd = (LPNMLVCUSTOMDRAW)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                size_t i = (size_t)cd->nmcd.dwItemSpec;
                if (i < g_rows.size()) {
                    const Row& r = g_rows[g_rows.size() - 1 - i];
                    cd->clrTextBk = OpRowColor(r.op);
                    cd->clrText = RGB(40, 40, 40);
                }
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }

        if (nh->idFrom == IDC_LIST && nh->code == NM_DBLCLK) {
            JumpToRow(((LPNMITEMACTIVATE)lp)->iItem);
            return 0;
        }

        if (nh->idFrom == IDC_LIST && nh->code == LVN_GETDISPINFOW) {
            auto* di = (NMLVDISPINFOW*)lp;
            int i = di->item.iItem;
            if (i < 0 || (size_t)i >= g_rows.size()) return 0;
            const Row& r = g_rows[g_rows.size() - 1 - (size_t)i];   // newest first
            static wchar_t buf[1024];
            switch (di->item.iSubItem) {
            case 0: wcscpy_s(buf, FormatTime(r.ts).c_str()); break;
            case 1: wcscpy_s(buf, OpName(r.op)); break;
            case 2: swprintf_s(buf, L"%u", r.pid); break;
            case 3: wcscpy_s(buf, r.size ? FormatBytes(r.size).c_str() : L""); break;
            case 4: wcsncpy_s(buf, r.path.c_str(), 1023); break;
            }
            di->item.pszText = buf;
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_UI);
        g_mon.Stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    LogInit();
    LOGI(L"wWinMain entered");

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    if (!InitCommonControlsEx(&icc))
        LogLastError(L"InitCommonControlsEx", GetLastError());

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"VigilesMainWnd";
    if (!RegisterClassExW(&wc)) {
        LogLastError(L"RegisterClassEx", GetLastError());
        LogShutdown();
        return 1;
    }

    HMENU menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_PAUSE, L"&Pause\tSpace");
    AppendMenuW(file, MF_STRING, IDM_CLEAR, L"&Clear list");
    AppendMenuW(file, MF_STRING, IDM_PROMPT, L"Copy &AI prompt\tCtrl+P");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Vigiles - ubi fumus, ibi ignis",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, menu, hInst, nullptr);
    if (!hwnd) {
        LogLastError(L"CreateWindowEx", GetLastError());
        MessageBoxW(nullptr, (L"Window creation failed. Log: " + LogFilePath()).c_str(),
                    L"Vigiles", MB_ICONERROR);
        LogShutdown();
        return 1;
    }
    LOGI(L"main window created: %p", hwnd);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    LOGI(L"entering message loop");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE &&
            GetFocus() != g_promptBtn)
            SendMessageW(hwnd, WM_COMMAND, IDM_PAUSE, 0);
        if (msg.message == WM_KEYDOWN && msg.wParam == 'P' &&
            (GetKeyState(VK_CONTROL) & 0x8000))
            SendMessageW(hwnd, WM_COMMAND, IDM_PROMPT, 0);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LogShutdown();
    return 0;
}
