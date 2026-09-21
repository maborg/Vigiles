// Vigiles - ubi fumus, ibi ignis
//
// Named for the vigiles urbani, the night watch of ancient Rome, who patrolled
// the city looking for fires. This one watches a filesystem instead: folders
// glow while they are busy, and the motto is the job description - you see the
// smoke here, then go and find the fire.
//
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
//
// This file owns the window and its message loop. The pieces it delegates to:
//   app_state       - shared window handles / live-row buffer / EtwMonitor
//   format           - byte/count/time formatting
//   colors           - the per-operation palette and heat-fade math
//   process_names    - PID -> image name cache
//   tree_view        - the folder tree: build, navigate, filter, refresh
//   ai_prompt        - the "Copy AI prompt" clipboard feature

#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <cmath>
#include "etw_monitor.h"
#include "log.h"
#include "resource.h"
#include "app_state.h"
#include "format.h"
#include "colors.h"
#include "process_names.h"
#include "tree_view.h"
#include "ai_prompt.h"

#pragma comment(lib, "comctl32.lib")

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

static const int kLegendH = 30;
static const int kPromptBtnW = 210;
static bool g_paused = false;
static uint64_t g_lastTotal = 0, g_lastTick = 0;

// ------------------------------------------------------------------ UI refresh

static void DrainEvents() {
    static std::vector<FileEvent> batch;
    size_t n = g_mon.Ring().drain(batch, 4000);
    if (!g_paused) {
        for (size_t i = 0; i < n; ++i) {
            const FileEvent& e = batch[i];
            std::wstring path;
            if (!g_mon.GetName(e.nameId, path)) continue;
            if (!MatchesFilter(ToLowerW(path))) continue;
            // Resolve the image name now, while the process is most likely
            // still alive. Doing it when the prompt is built instead loses
            // every short-lived process - a compiler run is mostly cl.exe
            // instances that exited seconds ago. Only the cache is warmed
            // here; the row still stores just the PID.
            ProcessName(e.processId);

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
            { L"Process", 150 }, { L"Size", 80 }, { L"Path", 640 }
        };
        for (int i = 0; i < 6; ++i) {
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
            case 3: wcsncpy_s(buf, ProcessName(r.pid).c_str(), 1023); break;
            case 4: wcscpy_s(buf, r.size ? FormatBytes(r.size).c_str() : L""); break;
            case 5: wcsncpy_s(buf, r.path.c_str(), 1023); break;
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

    // Large icon for Alt-Tab and the window's system menu, small one for the
    // title bar and taskbar. Asking for the exact pixel sizes makes Windows
    // pick the matching image out of the .ico instead of rescaling the 256px
    // one, which is what makes the title bar icon look muddy in most apps.
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (wc.hIcon) {
        LOGI(L"application icon loaded (%dx%d large, %dx%d small)",
             GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
             GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    } else {
        LOGW(L"no icon resource in this executable (LoadImage: %lu) - the .rc "
             L"was not compiled in. Re-run cmake configure and check for the "
             L"'Vigiles icon:' line.", GetLastError());
    }
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
