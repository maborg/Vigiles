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

#include "tree_view.h"
#include "app_state.h"
#include "format.h"
#include "colors.h"
#include <shlwapi.h>
#include <vector>
#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

// Which operation moved the most between two snapshots is computed in
// colors.h (DominantOp); this file only feeds it the before/after pair.

static std::wstring g_filter;   // lowercase prefix from the selected folder

ItemData* GetItemData(HTREEITEM h) {
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

void PopulateChildren(HTREEITEM parent) {
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

void AddDrives() {
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
void JumpToRow(int index) {
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
void RefreshVisibleTree() {
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
bool MatchesFilter(const std::wstring& lowPath) {
    if (g_filter.empty()) return true;
    if (lowPath.size() < g_filter.size()) return false;
    if (lowPath.compare(0, g_filter.size(), g_filter) != 0) return false;
    if (g_filter.back() == L'\\') return true;
    return lowPath.size() > g_filter.size() && lowPath[g_filter.size()] == L'\\';
}

// Switching folders starts a fresh view: drop the rows we were showing and
// discard whatever is already queued, so the list only fills with events that
// happen from now on.
void SetFilter(const std::wstring& displayPath) {
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
    ListView_SetColumn(g_list, 5, &c);   // Path is now column 5

}
