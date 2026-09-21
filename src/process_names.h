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
#include <cstdint>
#include <string>

// PID -> image name, cached for the life of the session. Resolution fails for a
// process that has already exited, and for a protected one; the number is kept
// as the label in that case, and the failure is cached too so we do not pay the
// syscall again on every event from that PID.
//
// Caveat: Windows recycles PIDs. During heavy process churn - a build, say - a
// number can be handed to a new process while this cache still holds the old
// name for it, and that row will be labelled wrongly. The clean fix is the
// PROCESS_START_KEY in the event's extended data, which is unique and never
// reused; the session already enables it (EVENT_ENABLE_PROPERTY_PROCESS_START_KEY
// in EtwMonitor::Start), it just is not read yet.
const std::wstring& ProcessName(uint32_t pid);
