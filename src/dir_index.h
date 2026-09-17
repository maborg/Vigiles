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
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <cwctype>
#include <cstdint>

struct DirStats {
    uint64_t reads = 0, writes = 0, creates = 0, deletes = 0, other = 0;
    uint64_t readBytes = 0, writeBytes = 0;
    uint64_t lastTick = 0;   // GetTickCount64 of the last event
};

struct DirNode {
    std::wstring path;    // display form, e.g. "C:\Windows\System32" or "C:\"
    DirStats self;        // events on files directly in this folder
    DirStats subtree;     // events anywhere below (including self)
};

inline std::wstring ToLowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// "C:\a\b\f.txt" -> "C:\a\b" ; "C:\a" -> "C:\" ; "C:\" -> ""
inline std::wstring ParentPath(const std::wstring& p) {
    if (p.size() <= 3) return std::wstring();
    size_t pos = p.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos == 0) return std::wstring();
    if (pos == 2 && p[1] == L':') return p.substr(0, 3);   // keep "C:\"
    return p.substr(0, pos);
}

// Owns every directory ever touched, plus a per-name-id cache of the
// ancestor chain so that hot-path updates are just pointer increments.
// All methods must be called with the caller holding `mutex()`.
class DirIndex {
public:
    std::mutex& mutex() { return m_; }

    DirNode* get(const std::wstring& dirPath) {
        auto it = nodes_.find(ToLowerW(dirPath));
        return it == nodes_.end() ? nullptr : it->second.get();
    }

    DirNode* getOrCreate(const std::wstring& dirPath) {
        std::wstring key = ToLowerW(dirPath);
        auto it = nodes_.find(key);
        if (it != nodes_.end()) return it->second.get();
        auto n = std::make_unique<DirNode>();
        n->path = dirPath;
        DirNode* raw = n.get();
        nodes_.emplace(std::move(key), std::move(n));
        return raw;
    }

    // chain[0] is the file's own folder, then each ancestor up to the root.
    const std::vector<DirNode*>& chainFor(uint32_t nameId, const std::wstring& filePath) {
        if (nameId >= chains_.size()) chains_.resize(nameId + 1);
        auto& c = chains_[nameId];
        if (!c.empty() || built_.count(nameId)) return c;
        built_.insert(nameId);
        std::wstring d = ParentPath(filePath);
        while (!d.empty()) {
            c.push_back(getOrCreate(d));
            d = ParentPath(d);
        }
        return c;
    }

    size_t size() const { return nodes_.size(); }

private:
    std::mutex m_;
    std::unordered_map<std::wstring, std::unique_ptr<DirNode>> nodes_;
    std::vector<std::vector<DirNode*>> chains_;
    std::unordered_set<uint32_t> built_;
};
