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

// ---------------------------------------------------------------- AI prompt
//
// Builds a prompt describing what is happening in the folder currently
// selected in the tree, and puts it on the clipboard. The captured rows are
// NOT emitted one by one: a few thousand near identical lines tell a model
// nothing that counts do not, and they blow past any context window. So
// everything is collapsed into per-process, per-path, per-subfolder and
// per-extension totals, each printed once with its count.

#include "ai_prompt.h"
#include "app_state.h"
#include "tree_view.h"
#include "process_names.h"
#include "format.h"
#include "log.h"
#include <commctrl.h>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>

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

void OnCopyPrompt(HWND hwnd) {
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
