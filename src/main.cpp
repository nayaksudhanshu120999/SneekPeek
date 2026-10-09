// SneekPeek - super-lightweight command palette for Windows.
// Pure Win32, no frameworks. Idle cost: 1 hidden window + GetMessage loop.
//   RAM idle: ~1-3 MB working set | CPU idle: 0% (blocking message loop).
//
// Hotkey : Ctrl+Space (toggle)
// UI     : black, opaque, rounded, minimal top-centered popup
// Search : app index (Start Menu) + power/quick chips + web + !bang chips +
//          tiny calculator
// Chips  : type "shu" -> Shut down chip; "gmail" -> quick-link chip.
//          Enter runs the selected chip, Tab cycles, Down goes to the list.
// Bangs  : type "yt" + Space -> YouTube chip in the bar, then the query.
// Hide   : click outside / app switch (WM_ACTIVATE + WM_ACTIVATEAPP), Esc,
//          Enter-after-launch
// Tray   : Settings / Refresh / Run-at-startup / Quit

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commdlg.h>
#include <powrprof.h>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <wchar.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <algorithm>

#include "app_index.h"
#include "bangs.h"
#include "settings.h"
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------- constants
static const wchar_t* kPaletteClass  = L"SneekPeekPalette";
static const wchar_t* kSettingsClass = L"SneekPeekSettings";
static const wchar_t* kChipClass     = L"SneekPeekChip";
static const wchar_t* kChipRowClass  = L"SneekPeekChipRow";
static const wchar_t* kMutexName     = L"SneekPeek_SingleInstance_v1";
static const UINT HOTKEY_ID = 1;
static const UINT WM_TRAYICON = WM_APP + 10;
static const UINT WM_APP_SHOW = WM_APP + 11;
static const UINT ID_TRAY_SETTINGS = 1001;
static const UINT ID_TRAY_REFRESH  = 1002;
static const UINT ID_TRAY_STARTUP   = 1003;
static const UINT ID_TRAY_QUIT      = 1004;

static const int kWidth = 600;
static const int kInputH = 44;
static const int kItemH = 44;
static const int kMaxItems = 6;
static const int kCornerR = 14;
static const int kChipH = 26;
static const int kChipRowH = 40;
static const int kMaxChips = 4;

// Settings control IDs
enum {
    IDC_ENGINE_COMBO = 101, IDC_ENGINE_URL = 102, IDC_CHK_PATH = 103,
    IDC_CHK_STARTUP = 104, IDC_BROWSER_COMBO = 105, IDC_BROWSER_BROWSE = 106,
    IDC_APPS_LIST = 110,
    IDC_BANGS_LIST = 111, IDC_BANG_ALIAS = 112, IDC_BANG_NAME = 113,
    IDC_BANG_URL = 114, IDC_BANG_COLOR = 115,
    IDC_BANG_ADD = 116, IDC_BANG_UPDATE = 117, IDC_BANG_DEL = 118,
    IDC_BANG_RESET = 119,
    IDC_QUICK_LIST = 120, IDC_QUICK_NAME = 121, IDC_QUICK_URL = 122,
    IDC_QUICK_ADD = 123, IDC_QUICK_UPDATE = 124, IDC_QUICK_DEL = 125,
    IDC_SAVE = 201, IDC_RESCAN = 202, IDC_CLOSE = 203,
    IDC_PAGE_0 = 301, IDC_PAGE_1, IDC_PAGE_2, IDC_PAGE_3,
};

// ------------------------------------------------------------- small helpers
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::wstring GetEditText(HWND hEdit) {
    int n = GetWindowTextLengthW(hEdit);
    if (n <= 0) return L"";
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(hEdit, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}
static std::wstring GetWindowString(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}
static COLORREF Darken(COLORREF c, int pct) {
    return RGB(GetRValue(c) * pct / 100,
               GetGValue(c) * pct / 100,
               GetBValue(c) * pct / 100);
}
// Solid chip fill with readable white text: bright colors get darkened.
static COLORREF ChipFill(COLORREF c) {
    int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
    return lum > 170 ? Darken(c, 55) : c;
}

// ------------------------------------------------------------- calculator
// Tiny recursive-descent evaluator: numbers, + - * / % ^, parens, unary minus.
// Returns false if the text is not a pure arithmetic expression.
struct CalcParser {
    const wchar_t* p;
    bool ok = true;
    void Skip() { while (*p && iswspace(*p)) p++; }
    double ParseExpr() {
        double v = ParseTerm();
        for (;;) { Skip(); if (*p==L'+'){p++; v+=ParseTerm();} else if(*p==L'-'){p++; v-=ParseTerm();} else break; }
        return v;
    }
    double ParseTerm() {
        double v = ParseFactor();
        for (;;) { Skip(); if(*p==L'*'){p++; v*=ParseFactor();} else if(*p==L'/'){p++; double d=ParseFactor(); v = d==0?0:v/d;} else if(*p==L'%'){p++; double d=ParseFactor(); v=d==0?0:fmod(v,d);} else break; }
        return v;
    }
    double ParseFactor() {
        Skip();
        double base = ParseUnary();
        Skip();
        if (*p==L'^'){ p++; double e=ParseFactor(); base = pow(base,e); }
        return base;
    }
    double ParseUnary() {
        Skip();
        if (*p==L'-'){ p++; return -ParseUnary(); }
        if (*p==L'+'){ p++; return ParseUnary(); }
        return ParsePrimary();
    }
    double ParsePrimary() {
        Skip();
        if (*p==L'('){ p++; double v=ParseExpr(); Skip(); if(*p==L')') p++; else ok=false; return v; }
        if ((*p>=L'0'&&*p<=L'9')||*p==L'.'){
            wchar_t* end=nullptr; double v=wcstod(p,&end);
            if(end==p){ok=false;return 0;} p=end; return v;
        }
        ok=false; return 0;
    }
};
static bool TryCalc(const std::wstring& q, double& out) {
    if (q.empty()) return false;
    bool hasDigit=false, hasOp=false;
    for (wchar_t c:q){
        if(c>=L'0'&&c<=L'9') hasDigit=true;
        else if(c==L'+'||c==L'-'||c==L'*'||c==L'/'||c==L'%'||c==L'^'||c==L'('||c==L')'||c==L'.'||c==L' '||c==L'\t') { if(c!=L' '&&c!=L'\t'&&c!=L'.') hasOp=true; }
        else return false;
    }
    if(!hasDigit||!hasOp) return false;
    CalcParser cp{ q.c_str() };
    double v = cp.ParseExpr();
    cp.Skip();
    if(!cp.ok || *cp.p!=L'\0') return false;
    out=v; return true;
}

// ------------------------------------------------------------------ globals
static HINSTANCE   g_hInst = NULL;
static HWND        g_hwndPalette = NULL;
static HWND        g_hwndEdit = NULL;
static HWND        g_hwndList = NULL;
static HWND        g_hwndChip = NULL;
static HWND        g_hwndChipRow = NULL;
static int         g_bangChipW = 0;  // 0 = hidden
static HWND        g_hwndSettings = NULL;
static int         g_settingsPage = 0;
static HWND        g_hwndEngineCombo = NULL;
static HWND        g_hwndEngineUrl = NULL;
static HWND        g_hwndBrowserCombo = NULL;
static HWND        g_hwndAppsList = NULL;
static HWND        g_hwndBangsList = NULL;
static HWND        g_hwndQuickList = NULL;
static HWND        g_hwndCellEdit = NULL; // in-place table cell editor
static int         g_cellLv = 0;   // 1 = bangs table, 2 = quick table
static int         g_cellRow = -1;
static int         g_cellCol = 0;
static std::vector<HWND> g_pageGeneral, g_pageApps, g_pageBangs, g_pageQuick;
static bool        g_fillingSettings = false;
static HFONT       g_fontInput = NULL;
static HFONT       g_fontTitle = NULL;
static HFONT       g_fontSub = NULL;
static HFONT       g_fontChip = NULL;
static HFONT       g_fontUI = NULL; // Segoe UI for all Settings controls
static HBRUSH      g_brBg = NULL;
static HBRUSH      g_brInput = NULL;
static HBRUSH      g_brSel = NULL;
static HICON       g_hIconBig = NULL;
static HICON       g_hIconSmall = NULL;
static Settings    g_settings;
static std::vector<AppEntry> g_apps;
static std::unordered_set<std::wstring> g_hidden; // lowercased app names
static std::vector<BrowserOpt> g_browsers;        // parallel to browser combo
static std::atomic<bool> g_indexReady{false};
static std::atomic<bool> g_indexing{false};

// Bang-chip mode: -1 = off, else index into g_settings.bangs.
static int  g_activeBang = -1;
static bool g_changingEdit = false;

// ------------------------------------------------------- power + quick chips
enum class PowerOp { Shutdown, Restart, Sleep, Hibernate };
struct PowerDef {
    PowerOp op;
    const wchar_t* title;
    COLORREF color;
    const wchar_t* aliases[4];
};
static const PowerDef kPowers[] = {
    {PowerOp::Shutdown,  L"Shut down", RGB(248, 113, 113),
        {L"shutdown", L"shut down", L"shut", L"power off"}},
    {PowerOp::Restart,   L"Restart",   RGB(251, 191, 36),
        {L"restart", L"reboot", NULL, NULL}},
    {PowerOp::Sleep,     L"Sleep",     RGB(96, 165, 250),
        {L"sleep", NULL, NULL, NULL}},
    {PowerOp::Hibernate, L"Hibernate", RGB(192, 132, 252),
        {L"hibernate", L"hibernation", NULL, NULL}},
};
static const COLORREF kQuickCol = RGB(192, 132, 252);

enum class ChipKind { Power, Quick, Bang };
struct ChipItem {
    ChipKind kind;
    std::wstring title;
    COLORREF color;
    int data; // power/bang index, or quick index into g_settings.quicks
};
static std::vector<ChipItem> g_chips;
static std::vector<RECT> g_chipRects;
static int  g_chipSel = 0;
static bool g_chipsFocus = false; // true = Enter runs the chip, list has no selection

enum class ResultKind { App, Web, Bang, Calc };
struct ResultItem {
    ResultKind kind;
    std::wstring title;   // main line (never a file path)
    std::wstring sub;     // small hint line
    std::wstring action;  // lnk path | shell:AppsFolder target | url | calc text
    COLORREF color;       // category accent color
    bool isStore = false; // Store app: launch via explorer.exe
};
static std::vector<ResultItem> g_results;

// Minimal black palette. Category accents: App blue, Web green, Calc amber,
// Bang = per-site color, Hint = gray.
static const COLORREF kBg     = RGB(0, 0, 0);
static const COLORREF kInput  = RGB(0, 0, 0); // seamless with the bar: minimal
static const COLORREF kSel    = RGB(28, 28, 30);
static const COLORREF kText   = RGB(240, 240, 240);
static const COLORREF kSub    = RGB(130, 130, 135);
static const COLORREF kAppCol = RGB(96, 165, 250);
static const COLORREF kWebCol = RGB(52, 211, 153);
static const COLORREF kCalcCol= RGB(251, 191, 36);
static const COLORREF kHintCol= RGB(110, 110, 115);

// ------------------------------------------------------- forward declarations
static void ShowPalette();
static void HidePalette();
static void UpdateResults();
static void ExecuteSelected(bool forceWeb);
static void ExecuteChip(int idx);
static void BuildIndexAsync();
static void AddTrayIcon(HWND hwnd);
static void RemoveTrayIcon(HWND hwnd);
static void ShowTrayMenu(HWND hwnd);
static void OpenSettings();
static void CloseSettings();
static void OnTrayCommand(UINT id);
static void RebuildHiddenSet();
static bool IsHiddenApp(const std::wstring& name);
static void ActivateBang(int idx, const std::wstring& query);
static void DeactivateBang();
static void CloseCellEdit(bool commit);
static void ApplyRoundCorners();
static void EnableRoundCorners(HWND hwnd);
static void RunPower(PowerOp op);

// ------------------------------------------------------------- index thread
static void BuildIndexAsync() {
    bool expected = false;
    if (!g_indexing.compare_exchange_strong(expected, true)) return; // already running
    std::thread([]{
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        std::vector<AppEntry> fresh;
        BuildAppIndex(fresh, g_settings.includePathExes);
        g_apps.swap(fresh);
        g_indexReady.store(true);
        g_indexing.store(false);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        if (g_hwndPalette && IsWindowVisible(g_hwndPalette))
            PostMessageW(g_hwndPalette, WM_APP + 20, 0, 0);
    }).detach();
}

static void RebuildHiddenSet() {
    g_hidden.clear();
    for (const auto& n : g_settings.hiddenApps)
        g_hidden.insert(BangLower(BangTrim(n)));
}
static bool IsHiddenApp(const std::wstring& name) {
    return g_hidden.find(ToLower(name)) != g_hidden.end();
}

// ------------------------------------------------------------- app icon
static void LoadAppIcons() {
    g_hIconBig = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_hIconSmall = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
}

// ------------------------------------------------------- rounded corners
// Win11 native rounding via DWM (dynamically loaded, ignored on older OS)
// + round-rect window region fallback so Win10 gets rounded corners too.
static void EnableRoundCorners(HWND hwnd) {
    if (HMODULE d = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT (WINAPI *Fn)(HWND, DWORD, LPCVOID, DWORD);
        if (Fn f = (Fn)GetProcAddress(d, "DwmSetWindowAttribute")) {
            DWORD round = 2; // DWMWCP_ROUND
            f(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
        }
        FreeLibrary(d);
    }
}
static void ApplyRoundCorners() {
    if (!g_hwndPalette) return;
    RECT r; GetWindowRect(g_hwndPalette, &r);
    HRGN rg = CreateRoundRectRgn(0, 0, r.right - r.left + 1, r.bottom - r.top + 1,
                                 kCornerR, kCornerR);
    if (rg && !SetWindowRgn(g_hwndPalette, rg, TRUE)) DeleteObject(rg);
}

// ------------------------------------------------------------- show / hide
static void PlacePalette() {
    POINT pt{}; GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int sw = mi.rcWork.right - mi.rcWork.left;
    int x = mi.rcWork.left + (sw - kWidth) / 2;
    int y = mi.rcWork.top + 110; // top-of-screen feel
    int h = kInputH + kItemH;
    SetWindowPos(g_hwndPalette, HWND_TOPMOST, x, y, kWidth, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetWindowPos(g_hwndEdit, NULL, 16, 9, kWidth - 32, 30, SWP_NOZORDER);
    int chipY = 9 + (30 - kChipH) / 2;
    SetWindowPos(g_hwndChip, NULL, 16, chipY, 10, kChipH, SWP_NOZORDER | SWP_HIDEWINDOW);
    SetWindowPos(g_hwndChipRow, NULL, 16, kInputH, kWidth - 32, kChipRowH,
                 SWP_NOZORDER | SWP_HIDEWINDOW);
    SetWindowPos(g_hwndList, NULL, 16, kInputH, kWidth - 32, kItemH, SWP_NOZORDER);
}

static void ShowPalette() {
    if (!g_indexReady.load() && !g_indexing.load())
        BuildIndexAsync();
    DeactivateBang();
    g_chips.clear();
    g_chipsFocus = false;
    PlacePalette();
    ApplyRoundCorners();
    g_changingEdit = true;
    SetWindowTextW(g_hwndEdit, L"");
    g_changingEdit = false;
    ShowWindow(g_hwndPalette, SW_SHOW);
    SetForegroundWindow(g_hwndPalette);
    SetFocus(g_hwndEdit);
    UpdateResults();
}

static void HidePalette() {
    DeactivateBang();
    g_chips.clear();
    g_chipsFocus = false;
    if (g_hwndChipRow) ShowWindow(g_hwndChipRow, SW_HIDE);
    if (g_hwndPalette) ShowWindow(g_hwndPalette, SW_HIDE);
    if (g_hwndEdit) {
        g_changingEdit = true;
        SetWindowTextW(g_hwndEdit, L"");
        g_changingEdit = false;
    }
    g_results.clear();
    if (g_hwndList) SendMessageW(g_hwndList, LB_RESETCONTENT, 0, 0);
}

// ------------------------------------------------------------- bang-chip mode
static const Bang* ActiveBang() {
    if (g_activeBang < 0 || g_activeBang >= (int)g_settings.bangs.size()) return NULL;
    return &g_settings.bangs[(size_t)g_activeBang];
}

static int ChipWidthFor(HDC dc, const std::wstring& text) {
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), -1, &r, DT_SINGLELINE | DT_CALCRECT);
    return (r.right - r.left) + 10 + 24 + 8;
}

// Single place that maps chip visibility -> edit text margins.
static void ApplyEditMargins() {
    if (!g_hwndEdit) return;
    SendMessageW(g_hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(g_bangChipW ? g_bangChipW + 8 : 10, 10));
}

static void ActivateBang(int idx, const std::wstring& query) {
    if (idx < 0 || idx >= (int)g_settings.bangs.size()) return;
    g_activeBang = idx;
    const Bang& b = g_settings.bangs[(size_t)idx];
    HDC dc = GetDC(g_hwndPalette);
    HGDIOBJ old = SelectObject(dc, g_fontChip);
    int w = ChipWidthFor(dc, b.name);
    SelectObject(dc, old);
    ReleaseDC(g_hwndPalette, dc);
    // Geometry derived from the live edit rect (single source of truth):
    // the pill always stays inside the box with equal padding, never
    // crossing the outline.
    RECT er; GetWindowRect(g_hwndEdit, &er);
    MapWindowPoints(NULL, g_hwndPalette, (POINT*)&er, 2);
    int boxH = er.bottom - er.top;
    int chH = boxH - 8;
    if (chH > kChipH) chH = kChipH;
    if (chH < 18) chH = 18;
    int chipX = er.left + 2;
    int chipY = er.top + (boxH - chH) / 2;
    // HWND_TOP: the chip must stay above the edit (which has WS_CLIPSIBLINGS
    // so it never paints over the chip). Synchronous paint: no blank flash.
    SetWindowPos(g_hwndChip, HWND_TOP, chipX, chipY, w, chH, SWP_SHOWWINDOW);
    InvalidateRect(g_hwndChip, NULL, TRUE);
    UpdateWindow(g_hwndChip);
    g_bangChipW = w;
    ApplyEditMargins();
    SendMessageW(g_hwndEdit, EM_SETCUEBANNER, FALSE, (LPARAM)L""); // chip only
    g_changingEdit = true;
    SetWindowTextW(g_hwndEdit, query.c_str());
    SendMessageW(g_hwndEdit, EM_SETSEL, (WPARAM)query.size(), (LPARAM)query.size());
    g_changingEdit = false;
    UpdateResults();
}

static void DeactivateBang() {
    g_activeBang = -1;
    g_bangChipW = 0;
    if (g_hwndChip) ShowWindow(g_hwndChip, SW_HIDE);
    if (g_hwndEdit) {
        ApplyEditMargins();
        SendMessageW(g_hwndEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
    }
}

// ------------------------------------------------------------- power actions
static void RunPower(PowerOp op) {
    switch (op) {
    case PowerOp::Shutdown:
        ShellExecuteW(NULL, L"open", L"shutdown.exe", L"/s /t 0", NULL, SW_HIDE);
        break;
    case PowerOp::Restart:
        ShellExecuteW(NULL, L"open", L"shutdown.exe", L"/r /t 0", NULL, SW_HIDE);
        break;
    case PowerOp::Hibernate:
        ShellExecuteW(NULL, L"open", L"shutdown.exe", L"/h", NULL, SW_HIDE);
        break;
    case PowerOp::Sleep:
        SetSuspendState(FALSE, FALSE, FALSE);
        break;
    }
}

// ------------------------------------------------------------- results model
static void PushAppRow(const AppEntry& app) {
    ResultItem r;
    r.kind = ResultKind::App;
    r.title = app.name;      // name only - never the file path
    r.sub = L"Application";
    r.action = app.target;
    r.color = kAppCol;
    r.isStore = app.isStore;
    g_results.push_back(std::move(r));
}

// Prefix-matched power + bang + quick-link chips (separate row above list).
static void BuildChips(const std::wstring& qlower) {
    g_chips.clear();
    if (qlower.size() < 2 || qlower[0] == L'!') return;
    for (int i = 0; i < 4 && (int)g_chips.size() < kMaxChips; i++) {
        for (int a = 0; a < 4 && kPowers[i].aliases[a]; a++) {
            std::wstring al = kPowers[i].aliases[a];
            if (al.size() >= qlower.size() && al.compare(0, qlower.size(), qlower) == 0) {
                g_chips.push_back({ChipKind::Power, kPowers[i].title, kPowers[i].color, i});
                break;
            }
        }
    }
    for (size_t i = 0; i < g_settings.bangs.size() && (int)g_chips.size() < kMaxChips; i++) {
        const Bang& b = g_settings.bangs[i];
        for (const auto& a : b.aliases) {
            if (!a.empty() && a.size() >= qlower.size() &&
                a.compare(0, qlower.size(), qlower) == 0) {
                g_chips.push_back({ChipKind::Bang, b.name, b.color, (int)i});
                break;
            }
        }
    }
    for (size_t i = 0; i < g_settings.quicks.size() && (int)g_chips.size() < kMaxChips; i++) {
        std::wstring n = BangLower(g_settings.quicks[i].name);
        if (n.size() >= qlower.size() && n.compare(0, qlower.size(), qlower) == 0)
            g_chips.push_back({ChipKind::Quick, g_settings.quicks[i].name, kQuickCol, (int)i});
    }
}

static void RebuildListControl() {
    SendMessageW(g_hwndList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_results.size(); i++) {
        int idx = (int)SendMessageW(g_hwndList, LB_ADDSTRING, 0, (LPARAM)L"");
        if (idx >= 0)
            SendMessageW(g_hwndList, LB_SETITEMDATA, idx, (LPARAM)i);
    }
    // Chips own the selection when they have focus; otherwise row 0.
    if (g_chipsFocus && !g_chips.empty())
        SendMessageW(g_hwndList, LB_SETCURSEL, (WPARAM)-1, 0);
    else if (!g_results.empty())
        SendMessageW(g_hwndList, LB_SETCURSEL, 0, 0);

    bool showChips = !g_chips.empty();
    size_t rows = g_results.size() > kMaxItems ? kMaxItems : g_results.size();
    int listY = kInputH + (showChips ? kChipRowH : 0);
    int h = listY + (int)rows * kItemH + 4; // 4px bottom pad mirrors the top

    // Resize (window + region + children) only when the height changed:
    // this is what stops the per-keystroke blink.
    RECT wr; GetWindowRect(g_hwndPalette, &wr);
    if (h != wr.bottom - wr.top) {
        SetWindowPos(g_hwndPalette, HWND_TOPMOST, wr.left, wr.top, kWidth, h,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        ApplyRoundCorners();
        RECT cr; GetClientRect(g_hwndPalette, &cr);
        if (showChips)
            SetWindowPos(g_hwndChipRow, NULL, 16, kInputH, cr.right - 32, kChipRowH,
                         SWP_NOZORDER | SWP_SHOWWINDOW);
        else
            ShowWindow(g_hwndChipRow, SW_HIDE);
        if (rows == 0)
            ShowWindow(g_hwndList, SW_HIDE); // bar-only: no list at all
        else
            SetWindowPos(g_hwndList, NULL, 16, listY, cr.right - 32,
                         (int)rows * kItemH, SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    // No background erase (controls paint every pixel themselves).
    InvalidateRect(g_hwndList, NULL, FALSE);
    if (showChips) InvalidateRect(g_hwndChipRow, NULL, FALSE);
}

static void UpdateResults() {
    g_results.clear();
    g_results.reserve(8);
    std::wstring raw = GetEditText(g_hwndEdit);
    std::wstring q = Trim(raw);

    // --- bang-chip mode: chip only, no rows, no list.
    if (ActiveBang()) {
        g_chips.clear();
        g_chipsFocus = false;
        RebuildListControl();
        return;
    }

    if (q.empty()) {
        // Bar-only start: no rows, no list until the user types.
        g_chips.clear();
        g_chipsFocus = false;
        RebuildListControl();
        return;
    }

    // --- chips row (power + quick links), selected by default
    BuildChips(BangLower(q));
    g_chipsFocus = !g_chips.empty();
    g_chipSel = 0;

    // --- calculator
    double calcVal = 0;
    if (TryCalc(q, calcVal)) {
        wchar_t buf[64];
        swprintf_s(buf, 64, L"%g", calcVal);
        ResultItem r{ResultKind::Calc, std::wstring(L"= ") + buf,
                     L"Calculator - Enter copies result", buf, kCalcCol};
        g_results.push_back(std::move(r));
    }

    // --- direct "!alias term" form (no chip)
    int bidx = -1; std::wstring term;
    if (ParseBangLeading(q, g_settings.bangs, bidx, term)) {
        if (bidx >= 0) {
            const Bang& b = g_settings.bangs[(size_t)bidx];
            ResultItem r{ResultKind::Bang,
                         term.empty() ? (L"Search " + b.name) : term,
                         b.name + L" - Enter to search",
                         term.empty() ? BangHome(b.url) : ExpandUrl(b.url, UrlEncodeQuery(term)),
                         b.color};
            g_results.push_back(std::move(r));
        } else {
            ResultItem r{ResultKind::Web, q,
                         L"Search " + g_settings.engineName + L" - unknown bang",
                         ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)), kWebCol};
            g_results.push_back(std::move(r));
        }
        if (!term.empty() && g_indexReady.load()) {
            for (auto& h : SearchApps(g_apps, term, 6)) {
                if (g_hidden.count(h.app->lower) == 0) PushAppRow(*h.app);
            }
        }
        RebuildListControl();
        return;
    }

    // --- normal: app matches + web fallback
    if (g_indexReady.load()) {
        for (auto& h : SearchApps(g_apps, q, kMaxItems - 1)) {
            if (g_hidden.count(h.app->lower) == 0) PushAppRow(*h.app);
        }
    } else {
        ResultItem r{ResultKind::App, L"Indexing apps...",
                     L"First run scans once - a moment", L"", kAppCol};
        g_results.push_back(std::move(r));
    }
    ResultItem web{ResultKind::Web, q,
                   L"Search " + g_settings.engineName,
                   ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)), kWebCol};
    g_results.push_back(std::move(web));

    RebuildListControl();
}

static int CurrentSelection() {
    int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return g_results.empty() ? -1 : 0;
    LPARAM data = SendMessageW(g_hwndList, LB_GETITEMDATA, sel, 0);
    if (data < 0 || data >= (LPARAM)g_results.size()) return -1;
    return (int)data;
}

static void ExecuteChip(int idx) {
    if (idx < 0 || idx >= (int)g_chips.size()) return;
    ChipItem c = g_chips[(size_t)idx]; // copy: HidePalette clears the vector
    if (c.kind == ChipKind::Power) {
        int nPowers = (int)(sizeof(kPowers) / sizeof(kPowers[0]));
        if (c.data < 0 || c.data >= nPowers) return;
        PowerOp op = kPowers[c.data].op;
        HidePalette(); // hide first: instant, never wait for the action
        RunPower(op);
        return;
    }
    if (c.kind == ChipKind::Bang) {
        if (c.data < 0 || c.data >= (int)g_settings.bangs.size()) return;
        ActivateBang(c.data, L""); // chip arms the site, like alias + Space
        return;
    }
    if (c.data < 0 || c.data >= (int)g_settings.quicks.size()) return;
    std::wstring url = g_settings.quicks[(size_t)c.data].url;
    HidePalette(); // hide first: instant, never wait for the action
    OpenUrl(url, g_settings.browserPath);
}

static void ExecuteSelected(bool forceWeb) {
    std::wstring q = Trim(GetEditText(g_hwndEdit));

    auto CopyText = [](const std::wstring& t) {
        if (!OpenClipboard(g_hwndPalette)) return;
        EmptyClipboard();
        size_t bytes = (t.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) { memcpy(GlobalLock(h), t.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
        CloseClipboard();
    };

    // Bang-chip mode: the armed site always wins (Shift+Enter = engine instead).
    if (const Bang* b = ActiveBang()) {
        std::wstring url = forceWeb ? ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q))
            : (q.empty() ? BangHome(b->url) : ExpandUrl(b->url, UrlEncodeQuery(q)));
        HidePalette(); // hide first: never wait for the browser
        OpenUrl(url, g_settings.browserPath);
        return;
    }

    if (q.empty()) { HidePalette(); return; }

    // Shift+Enter forces a web search with the raw text.
    if (forceWeb) {
        std::wstring url;
        int bidx = -1; std::wstring term;
        if (ParseBangLeading(q, g_settings.bangs, bidx, term) && bidx >= 0) {
            const Bang& b = g_settings.bangs[(size_t)bidx];
            url = term.empty() ? BangHome(b.url) : ExpandUrl(b.url, UrlEncodeQuery(term));
        } else {
            url = ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q));
        }
        HidePalette(); // hide first: never wait for the browser
        OpenUrl(url, g_settings.browserPath);
        return;
    }

    int idx = CurrentSelection();
    if (idx < 0) {
        std::wstring url = ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q));
        HidePalette();
        OpenUrl(url, g_settings.browserPath);
        return;
    }
    ResultItem r = g_results[(size_t)idx]; // copy: HidePalette clears the vector
    HidePalette(); // hide first: never wait for the target app
    switch (r.kind) {
    case ResultKind::App:
        if (r.action.empty()) break;
        if (r.isStore)
            ShellExecuteW(NULL, L"open", L"explorer.exe", r.action.c_str(), NULL, SW_SHOWNORMAL);
        else
            ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL);
        break;
    case ResultKind::Web:
    case ResultKind::Bang:
        OpenUrl(r.action, g_settings.browserPath);
        break;
    case ResultKind::Calc:
        CopyText(r.action);
        break;
    }
}

// ------------------------------------------------- list navigation helpers
// Moving with arrows mirrors the selected row's text into the box WITHOUT
// refiltering: the list stays frozen until the user types or deletes.
static bool IsSelectable(size_t i) {
    return i < g_results.size();
}
static int MoveSel(int from, int dir) {
    int n = (int)g_results.size(); // listbox order == result order (no sort)
    if (n <= 0) return -1;
    int cur = from;
    for (int k = 0; k < n; k++) {
        cur = (cur + dir + n) % n;
        if (IsSelectable((size_t)cur)) return cur;
    }
    return -1;
}
static void SyncSelToBox(int listSel) {
    if (listSel < 0 || listSel >= (int)g_results.size()) return;
    const ResultItem& r = g_results[(size_t)listSel];
    std::wstring keep = GetEditText(g_hwndEdit);
    std::wstring t = keep;
    if (r.kind == ResultKind::App) t = r.title;
    if (t == keep) return;
    g_changingEdit = true; // programmatic: no refilter, list stays frozen
    SetWindowTextW(g_hwndEdit, t.c_str());
    SendMessageW(g_hwndEdit, EM_SETSEL, (WPARAM)t.size(), (LPARAM)t.size());
    g_changingEdit = false;
}

// ------------------------------------------------------------- edit subclass
// WM_CHAR swallowing kills the system beep: a single-line EDIT beeps for
// Enter/Esc/Tab chars it cannot insert.
static LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                    UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR) {
        if (w == 13 || w == 10 || w == 27 || w == 9) return 0;
    }
    if (m == WM_KEYDOWN) {
        int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
        int count = (int)SendMessageW(g_hwndList, LB_GETCOUNT, 0, 0);
        bool haveChips = !g_chips.empty();
        switch (w) {
        case VK_ESCAPE: HidePalette(); return 0;
        case VK_RETURN: {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (g_chipsFocus && haveChips && !shift) ExecuteChip(g_chipSel);
            else ExecuteSelected(shift);
            return 0;
        }
        case VK_TAB: {
            bool back = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (haveChips) {
                int n = (int)g_chips.size();
                g_chipSel = back ? (g_chipSel <= 0 ? n - 1 : g_chipSel - 1)
                                 : (g_chipSel + 1) % n;
                g_chipsFocus = true;
                SendMessageW(g_hwndList, LB_SETCURSEL, (WPARAM)-1, 0);
                InvalidateRect(g_hwndChipRow, NULL, FALSE);
            }
            return 0; // never let Tab move focus out of the box
        }
        case VK_DOWN:
            if (g_chipsFocus && haveChips) {
                g_chipsFocus = false;
                int to = MoveSel(-1, +1);
                if (to >= 0) SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                InvalidateRect(g_hwndChipRow, NULL, FALSE);
                InvalidateRect(g_hwndList, NULL, FALSE);
            } else if (count > 0) {
                int to = MoveSel(sel, +1);
                if (to >= 0) {
                    SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                    SyncSelToBox(to);
                    InvalidateRect(g_hwndList, NULL, FALSE);
                }
            }
            return 0;
        case VK_UP:
            if (!g_chipsFocus && (sel <= 0) && haveChips) {
                g_chipsFocus = true;
                SendMessageW(g_hwndList, LB_SETCURSEL, (WPARAM)-1, 0);
                InvalidateRect(g_hwndChipRow, NULL, FALSE);
                InvalidateRect(g_hwndList, NULL, FALSE);
            } else if (count > 0) {
                int to = MoveSel(sel, -1);
                if (to >= 0) {
                    SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                    SyncSelToBox(to);
                    InvalidateRect(g_hwndList, NULL, FALSE);
                }
            }
            return 0;
        case VK_BACK:
            // Empty query + Backspace exits bang-chip mode.
            if (g_activeBang >= 0 && GetWindowTextLengthW(h) == 0) {
                DeactivateBang();
                UpdateResults();
                return 0;
            }
            break;
        case VK_F5:
            BuildIndexAsync();
            return 0;
        }
    }
    return DefSubclassProc(h, m, w, l);
}

// ------------------------------------------------------------- bang chip window
static LRESULT CALLBACK ChipProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        // Solid pill, white bold text, no outline. Click x exits bang mode.
        const Bang* b = ActiveBang();
        std::wstring text = b ? b->name : L"";
        COLORREF col = b ? b->color : kHintCol;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        FillRect(dc, &r, g_brInput); // corners too: nothing peeks through
        HBRUSH bg = CreateSolidBrush(ChipFill(col));
        HGDIOBJ oldB = SelectObject(dc, bg);
        HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
        int rad = r.bottom - r.top; // stadium ends: fully round pill
        if (rad < 8) rad = 8;
        RoundRect(dc, 0, 0, r.right, r.bottom, rad, rad);
        SelectObject(dc, oldB); SelectObject(dc, oldP);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontChip);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT tr = r; tr.left += 10; tr.right -= 24;
        DrawTextW(dc, text.c_str(), -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT xr = r; xr.left = xr.right - 20;
        DrawTextW(dc, L"x", -1, &xr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT r; GetClientRect(hwnd, &r);
        int x = GET_X_LPARAM(l);
        if (x >= r.right - 24) {
            DeactivateBang(); // clicked the x
            UpdateResults();
        }
        SetFocus(g_hwndEdit);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ------------------------------------------------------- chips-row window
// Horizontal pill row above the list: power actions + quick links.
static int ChipTextW(HDC dc, const std::wstring& text) {
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, text.c_str(), -1, &r, DT_SINGLELINE | DT_CALCRECT);
    return (r.right - r.left) + 24;
}
static void LayoutChipRects(const RECT& client) {
    g_chipRects.clear();
    int x = 12;
    int h = 26;
    int y = (client.bottom - client.top - h) / 2;
    HDC dc = GetDC(g_hwndChipRow);
    HGDIOBJ old = SelectObject(dc, g_fontChip);
    for (const auto& c : g_chips) {
        int w = ChipTextW(dc, c.title);
        RECT r{x, y, x + w, y + h};
        g_chipRects.push_back(r);
        x += w + 8;
    }
    SelectObject(dc, old);
    ReleaseDC(g_hwndChipRow, dc);
}
static LRESULT CALLBACK ChipRowProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cl; GetClientRect(hwnd, &cl);
        FillRect(dc, &cl, g_brBg);
        LayoutChipRects(cl);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontChip);
        for (size_t i = 0; i < g_chips.size() && i < g_chipRects.size(); i++) {
            const ChipItem& c = g_chips[i];
            const RECT& r = g_chipRects[i];
            bool sel = g_chipsFocus && (int)i == g_chipSel;
            COLORREF base = ChipFill(c.color);
            // Solid pills, white bold text; selection = full brightness.
            HBRUSH bg = CreateSolidBrush(sel ? base : Darken(base, 50));
            HGDIOBJ oldB = SelectObject(dc, bg);
            HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
            int rad = r.bottom - r.top; // stadium ends: fully round pill
            if (rad < 8) rad = 8;
            RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
            SelectObject(dc, oldB);
            SelectObject(dc, oldP);
            DeleteObject(bg);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT tr = r;
            DrawTextW(dc, c.title.c_str(), -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        }
        // Tiny hint at the right end (only when it does not overlap chips).
        if (!g_chipRects.empty() &&
            g_chipRects.back().right < cl.right - 190) {
            SelectObject(dc, g_fontSub);
            SetTextColor(dc, kSub);
            RECT hr = cl; hr.left = hr.right - 190;
            DrawTextW(dc, L"Enter runs - Tab switch", -1, &hr,
                      DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT cl; GetClientRect(hwnd, &cl);
        LayoutChipRects(cl);
        for (size_t i = 0; i < g_chipRects.size(); i++) {
            if (PtInRect(&g_chipRects[i], pt)) { ExecuteChip((int)i); return 0; }
        }
        SetFocus(g_hwndEdit);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ------------------------------------------------------------- palette wndproc
static LRESULT CALLBACK PaletteProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        // WS_CLIPSIBLINGS everywhere: the edit must never paint over the chip.
        g_hwndEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_LEFT | ES_AUTOHSCROLL,
            16, 9, kWidth - 32, 30,
            hwnd, (HMENU)1, g_hInst, NULL);
        SendMessageW(g_hwndEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
        SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)g_fontInput, TRUE);
        SendMessageW(g_hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(10, 10));
        SetWindowSubclass(g_hwndEdit, EditSubclass, 1, 0);

        g_hwndChip = CreateWindowExW(0, kChipClass, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            16, 9, 10, kChipH,
            hwnd, (HMENU)3, g_hInst, NULL);

        g_hwndChipRow = CreateWindowExW(0, kChipRowClass, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            16, kInputH, kWidth - 32, kChipRowH,
            hwnd, (HMENU)4, g_hInst, NULL);

        g_hwndList = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
            LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            16, kInputH, kWidth - 32, kItemH,
            hwnd, (HMENU)2, g_hInst, NULL);
        break;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)w;
        RECT r; GetClientRect(hwnd, &r);
        FillRect(dc, &r, g_brBg);
        return 1;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetTextColor(dc, kText);
        SetBkColor(dc, kInput);
        return (LRESULT)g_brInput;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetTextColor(dc, kText);
        SetBkColor(dc, kBg);
        return (LRESULT)g_brBg;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w), code = HIWORD(w);
        if ((HWND)l == g_hwndEdit && code == EN_CHANGE) {
            if (g_changingEdit) break;
            // Bang-chip trigger: "alias<space>" with nothing else typed.
            if (g_activeBang < 0) {
                std::wstring editRaw = GetEditText(g_hwndEdit);
                size_t sp = editRaw.find(L' ');
                size_t tb = editRaw.find(L'\t');
                size_t p = sp;
                if (p == std::wstring::npos || (tb != std::wstring::npos && tb < p)) p = tb;
                if (p != std::wstring::npos) {
                    std::wstring token = editRaw.substr(0, p);
                    if (!token.empty() && token[0] != L'!') {
                        int bi = FindBangByAlias(g_settings.bangs, BangLower(token));
                        if (bi >= 0) {
                            ActivateBang(bi, L""); // armed: empty box, chip only
                            break;
                        }
                    }
                }
            }
            UpdateResults();
        } else if ((HWND)l == g_hwndList) {
            if (code == LBN_DBLCLK) {
                g_chipsFocus = false;
                ExecuteSelected(false);
            } else if (code == LBN_SELCHANGE && g_chipsFocus) {
                g_chipsFocus = false; // mouse took over the list
                InvalidateRect(g_hwndChipRow, NULL, FALSE);
            }
        } else if (id >= ID_TRAY_SETTINGS && id <= ID_TRAY_QUIT) {
            OnTrayCommand(id);
        }
        break;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)l;
        mi->itemHeight = kItemH;
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)l;
        if (d->CtlID != 2) break;
        bool sel = (d->itemState & ODS_SELECTED) != 0;
        HDC dc = d->hDC;
        size_t ri = (d->itemData < (UINT)g_results.size()) ? d->itemData : 9999;
        FillRect(dc, &d->rcItem, sel ? g_brSel : g_brBg);
        if ((int)ri < (int)g_results.size()) {
            const ResultItem& r = g_results[ri];
            SetBkMode(dc, TRANSPARENT);
            // Category color bar: thin, brighter when selected.
            HBRUSH bar = CreateSolidBrush(sel ? r.color : Darken(r.color, 65));
            RECT br = d->rcItem;
            br.left += 14; br.right = br.left + 2;
            br.top += 10; br.bottom -= 10;
            FillRect(dc, &br, bar);
            DeleteObject(bar);

            RECT tr = d->rcItem; tr.left += 30; tr.top += 5; tr.right -= 10;
            RECT sr = tr; sr.top += 22;
            SetTextColor(dc, sel ? kText : RGB(218, 218, 226));
            SelectObject(dc, g_fontTitle);
            DrawTextW(dc, r.title.c_str(), -1, &tr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(dc, kSub);
            SelectObject(dc, g_fontSub);
            DrawTextW(dc, r.sub.c_str(), -1, &sr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        return TRUE;
    }
    case WM_ACTIVATE:
        // Click-outside-to-dismiss. Child focus keeps the top-level active,
        // so typing never hides. Settings is exempt.
        if (LOWORD(w) == WA_INACTIVE) {
            HWND now = (HWND)l;
            if (now != g_hwndSettings && !IsChild(g_hwndSettings, now))
                HidePalette();
        }
        break;
    case WM_ACTIVATEAPP:
        // App switch / click onto another app always dismisses.
        if (!w && IsWindowVisible(hwnd))
            HidePalette();
        break;
    case WM_HOTKEY:
        if (w == HOTKEY_ID) {
            if (IsWindowVisible(hwnd)) HidePalette();
            else ShowPalette();
        }
        break;
    case WM_APP_SHOW:
        ShowPalette();
        break;
    case WM_APP + 20: // index finished
        if (IsWindowVisible(hwnd)) UpdateResults();
        break;
    case WM_TRAYICON:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU)
            ShowTrayMenu(hwnd);
        else if (LOWORD(l) == WM_LBUTTONDBLCLK)
            ShowPalette();
        break;
    case WM_DESTROY:
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ------------------------------------------------------------- tray
static void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_hIconSmall ? g_hIconSmall : LoadIconW(NULL, IDI_APPLICATION);
    wcsncpy_s(nid.szTip, L"SneekPeek - Ctrl+Space", _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}
static void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd; nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}
static void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_STRING, ID_TRAY_REFRESH, L"Refresh apps");
    AppendMenuW(menu, MF_STRING | (g_settings.runAtStartup ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_STARTUP, L"Run at startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required for the menu to dismiss correctly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

// ------------------------------------------------------------- settings UI
static std::wstring ExeFileName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

// --- listview helpers
static void LVAddColumn(HWND lv, int i, const wchar_t* text, int w) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPWSTR)text;
    c.cx = w;
    c.iSubItem = i;
    ListView_InsertColumn(lv, i, &c);
}
static int LVAppendRow(HWND lv, const wchar_t* c0) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = (LPWSTR)c0;
    return ListView_InsertItem(lv, &it);
}
static void LVSetCell(HWND lv, int row, int col, const std::wstring& t) {
    ListView_SetItemText(lv, row, col, (LPWSTR)t.c_str());
}
static std::wstring LVGetCell(HWND lv, int row, int col) {
    wchar_t buf[4096] = {0};
    ListView_GetItemText(lv, row, col, buf, 4096);
    return buf;
}
static int LVSelectedRow(HWND lv) {
    return ListView_GetNextItem(lv, -1, LVNI_SELECTED);
}

static void FillEngineCombo() {
    SendMessageW(g_hwndEngineCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"DuckDuckGo");
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"Google");
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"Bing");
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"Brave");
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"Custom...");
    int sel = 0;
    if (g_settings.engineName == L"Google") sel = 1;
    else if (g_settings.engineName == L"Bing") sel = 2;
    else if (g_settings.engineName == L"Brave") sel = 3;
    else if (g_settings.engineName != L"DuckDuckGo") sel = 4;
    SendMessageW(g_hwndEngineCombo, CB_SETCURSEL, sel, 0);
    SetWindowTextW(g_hwndEngineUrl, g_settings.engineUrl.c_str());
    EnableWindow(g_hwndEngineUrl, sel == 4);
}

static void FillBrowserCombo() {
    g_browsers = DetectBrowsers();
    SendMessageW(g_hwndBrowserCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hwndBrowserCombo, CB_ADDSTRING, 0, (LPARAM)L"System default");
    for (const auto& b : g_browsers)
        SendMessageW(g_hwndBrowserCombo, CB_ADDSTRING, 0, (LPARAM)b.name.c_str());
    int sel = 0;
    if (!g_settings.browserPath.empty()) {
        for (size_t i = 0; i < g_browsers.size(); i++) {
            if (_wcsicmp(g_browsers[i].path.c_str(), g_settings.browserPath.c_str()) == 0) {
                sel = (int)i + 1;
                break;
            }
        }
        if (sel == 0) { // saved custom path that is no longer detected
            g_browsers.push_back({ExeFileName(g_settings.browserPath), g_settings.browserPath});
            SendMessageW(g_hwndBrowserCombo, CB_ADDSTRING, 0,
                         (LPARAM)g_browsers.back().name.c_str());
            sel = (int)g_browsers.size();
        }
    }
    SendMessageW(g_hwndBrowserCombo, CB_ADDSTRING, 0, (LPARAM)L"Browse...");
    SendMessageW(g_hwndBrowserCombo, CB_SETCURSEL, sel, 0);
}

static void FillAppsList() {
    ListView_DeleteAllItems(g_hwndAppsList);
    for (size_t i = 0; i < g_apps.size(); i++) {
        int row = LVAppendRow(g_hwndAppsList, g_apps[i].name.c_str());
        ListView_SetCheckState(g_hwndAppsList, row, IsHiddenApp(g_apps[i].name));
    }
}

static void FillBangsList() {
    ListView_DeleteAllItems(g_hwndBangsList);
    for (const auto& b : g_settings.bangs) {
        std::wstring aliases;
        for (size_t i = 0; i < b.aliases.size(); i++) {
            if (i) aliases += L", ";
            aliases += b.aliases[i];
        }
        int row = LVAppendRow(g_hwndBangsList, aliases.c_str());
        LVSetCell(g_hwndBangsList, row, 1, b.name);
        LVSetCell(g_hwndBangsList, row, 2, b.url);
    }
}

static void FillQuickList() {
    ListView_DeleteAllItems(g_hwndQuickList);
    for (const auto& q : g_settings.quicks) {
        int row = LVAppendRow(g_hwndQuickList, q.name.c_str());
        LVSetCell(g_hwndQuickList, row, 1, q.url);
    }
}

static void ShowSettingsPage(int page) {
    g_settingsPage = page;
    const std::vector<HWND>* pages[4] = {&g_pageGeneral, &g_pageApps, &g_pageBangs, &g_pageQuick};
    for (int i = 0; i < 4; i++)
        for (HWND h : *pages[i])
            ShowWindow(h, (i == page) ? SW_SHOW : SW_HIDE);
}

static HWND NewCtrl(const wchar_t* cls, const wchar_t* text, DWORD style,
                    int x, int y, int w, int h, HWND parent, UINT id) {
    DWORD st = WS_CHILD | style;
    // All buttons owner-drawn (dark theme); auto-checkbox state still managed.
    if (wcscmp(cls, L"BUTTON") == 0)
        st |= BS_OWNERDRAW;
    // ComboBoxes owner-drawn so field + list stay dark under any system theme.
    if (wcscmp(cls, WC_COMBOBOXW) == 0)
        st |= CBS_OWNERDRAWFIXED | CBS_HASSTRINGS;
    return CreateWindowW(cls, text, st, x, y, w, h,
                         parent, (HMENU)(UINT_PTR)id, g_hInst, NULL);
}

static void ApplyUIFont(HWND parent) {
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        SendMessageW(c, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
}

static void DarkTitleBar(HWND hwnd) {
    if (HMODULE d = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT (WINAPI *Fn)(HWND, DWORD, LPCVOID, DWORD);
        if (Fn f = (Fn)GetProcAddress(d, "DwmSetWindowAttribute")) {
            BOOL dark = TRUE;
            f(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
        FreeLibrary(d);
    }
}

static void OpenSettings() {
    if (g_hwndSettings) { ShowWindow(g_hwndSettings, SW_SHOW); SetForegroundWindow(g_hwndSettings); return; }
    int W = 600, H = 590;
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    g_hwndSettings = CreateWindowExW(0, kSettingsClass, L"SneekPeek Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, W, H, g_hwndPalette, NULL, g_hInst, NULL);
    ShowWindow(g_hwndSettings, SW_SHOW);
    DarkTitleBar(g_hwndSettings);
    UpdateWindow(g_hwndSettings);
}
static void CloseSettings() {
    CloseCellEdit(true); // commit any in-progress table edit first
    if (g_hwndSettings) { DestroyWindow(g_hwndSettings); g_hwndSettings = NULL; }
}

static bool BrowseForBrowser(HWND owner, std::wstring& outPath) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose browser";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return false;
    outPath = file;
    return true;
}

// Read check state of the apps list into g_settings.hiddenApps.
static void ReadHiddenFromList() {
    g_settings.hiddenApps.clear();
    int n = ListView_GetItemCount(g_hwndAppsList);
    for (int i = 0; i < n; i++)
        if (ListView_GetCheckState(g_hwndAppsList, i))
            g_settings.hiddenApps.push_back(LVGetCell(g_hwndAppsList, i, 0));
}

// Every change persists immediately: there is no Save button.
static void SaveNow() {
    SaveSettings(g_settings);
    RebuildHiddenSet();
}

// Engine + browser combos / custom URL -> settings -> disk.
static void SaveEngineBrowser(HWND owner) {
    int esel = (int)SendMessageW(g_hwndEngineCombo, CB_GETCURSEL, 0, 0);
    std::wstring eurl = GetWindowString(g_hwndEngineUrl);
    const wchar_t* names[] = {L"DuckDuckGo", L"Google", L"Bing", L"Brave", L"Custom"};
    const wchar_t* urls[] = {
        L"https://duckduckgo.com/?q=%s",
        L"https://www.google.com/search?q=%s",
        L"https://www.bing.com/search?q=%s",
        L"https://search.brave.com/search?q=%s", eurl.c_str()};
    if (esel < 0 || esel > 4) esel = 0;
    if (esel == 4 && eurl.find(L"%s") == std::wstring::npos) {
        MessageBoxW(owner, L"Custom URL must contain %s as the query placeholder.",
                    L"SneekPeek", MB_ICONWARNING | MB_OK);
        SetWindowTextW(g_hwndEngineUrl, g_settings.engineUrl.c_str());
        return;
    }
    g_settings.engineName = names[esel];
    g_settings.engineUrl = urls[esel];
    int bsel = (int)SendMessageW(g_hwndBrowserCombo, CB_GETCURSEL, 0, 0);
    if (bsel <= 0) g_settings.browserPath.clear();
    else if (bsel - 1 < (int)g_browsers.size())
        g_settings.browserPath = g_browsers[(size_t)bsel - 1].path;
    SaveNow();
}

static std::wstring JoinAliases(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += L", ";
        s += v[i];
    }
    return s;
}

// Floating editor over a table cell. Enter / focus-loss commits to the
// settings vector + disk; Esc discards.
static void CloseCellEdit(bool commit);
static LRESULT CALLBACK CellEditSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                         UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR && (w == 13 || w == 9)) {
        CloseCellEdit(true);
        SetFocus(g_hwndSettings);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        CloseCellEdit(false);
        SetFocus(g_hwndSettings);
        return 0;
    }
    if (m == WM_KILLFOCUS) {
        CloseCellEdit(true);
    }
    return DefSubclassProc(h, m, w, l);
}

static void StartCellEdit(HWND lv, int row, int col) {
    CloseCellEdit(true); // commit anything pending first
    if (!g_hwndSettings || row < 0 || col < 0) return;
    if (!g_hwndCellEdit) {
        g_hwndCellEdit = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 10, 10, g_hwndSettings, (HMENU)500, g_hInst, NULL);
        SendMessageW(g_hwndCellEdit, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
        SetWindowSubclass(g_hwndCellEdit, CellEditSubclass, 20, 0);
    }
    RECT r;
    if (!ListView_GetSubItemRect(lv, row, col, LVIR_BOUNDS, &r)) return;
    MapWindowPoints(lv, g_hwndSettings, (POINT*)&r, 2);
    g_cellLv = (lv == g_hwndBangsList) ? 1 : 2;
    g_cellRow = row;
    g_cellCol = col;
    SetWindowTextW(g_hwndCellEdit, LVGetCell(lv, row, col).c_str());
    SetWindowPos(g_hwndCellEdit, NULL, r.left, r.top,
                 r.right - r.left, r.bottom - r.top, SWP_SHOWWINDOW);
    SetFocus(g_hwndCellEdit);
    SendMessageW(g_hwndCellEdit, EM_SETSEL, 0, -1);
}

static void CloseCellEdit(bool commit) {
    if (!g_hwndCellEdit || !IsWindowVisible(g_hwndCellEdit)) return;
    std::wstring t = Trim(GetWindowString(g_hwndCellEdit));
    HWND lv = (g_cellLv == 1) ? g_hwndBangsList : g_hwndQuickList;
    int row = g_cellRow, col = g_cellCol;
    ShowWindow(g_hwndCellEdit, SW_HIDE);
    if (!commit || !lv || row < 0 || col < 0) return;
    if (g_cellLv == 1) { // bangs: rebuild the row, color stays as assigned
        if (row >= (int)g_settings.bangs.size()) return;
        std::wstring cells[3] = {LVGetCell(lv, row, 0),
                                 LVGetCell(lv, row, 1),
                                 LVGetCell(lv, row, 2)};
        cells[col] = t;
        Bang b;
        std::wstring line = cells[0] + L" | " + cells[1] + L" | " + cells[2] +
                            L" | " + SerializeColor(g_settings.bangs[(size_t)row].color);
        if (!ParseBangLine(line, b)) {
            MessageBoxW(g_hwndSettings,
                        L"Need: alias | Name | https://...%s - change reverted.",
                        L"SneekPeek", MB_ICONWARNING | MB_OK);
            return;
        }
        b.color = g_settings.bangs[(size_t)row].color;
        g_settings.bangs[(size_t)row] = b;
        LVSetCell(lv, row, 0, JoinAliases(b.aliases));
        LVSetCell(lv, row, 1, b.name);
        LVSetCell(lv, row, 2, b.url);
        SaveNow();
    } else if (g_cellLv == 2) { // quick links
        if (row >= (int)g_settings.quicks.size()) return;
        std::wstring name = (col == 0) ? t : LVGetCell(lv, row, 0);
        std::wstring url = (col == 1) ? t : LVGetCell(lv, row, 1);
        QuickLink q{name, url};
        if (q.name.empty() || q.url.find(L"://") == std::wstring::npos) {
            MessageBoxW(g_hwndSettings,
                        L"Need a name and a full URL like https://... - change reverted.",
                        L"SneekPeek", MB_ICONWARNING | MB_OK);
            return;
        }
        g_settings.quicks[(size_t)row] = q;
        LVSetCell(lv, row, 0, q.name);
        LVSetCell(lv, row, 1, q.url);
        SaveNow();
    }
}

// Single-line settings edits: swallow beep-producing chars (Enter/Esc/Tab).
static LRESULT CALLBACK PlainEditSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                          UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR && (w == 13 || w == 10 || w == 27 || w == 9)) return 0;
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_fillingSettings = true;
        g_pageGeneral.clear(); g_pageApps.clear();
        g_pageBangs.clear(); g_pageQuick.clear();

        // Page tabs: owner-drawn buttons matching the app theme (no tab
        // control, so there is no light-themed rim anywhere in Settings).
        const wchar_t* tabs[] = {L"General", L"Hidden apps", L"Bangs", L"Quick links"};
        const int tabX[] = {16, 140, 280, 384};
        const int tabW[] = {118, 134, 98, 120};
        for (int i = 0; i < 4; i++)
            NewCtrl(L"BUTTON", tabs[i], WS_VISIBLE | WS_TABSTOP,
                    tabX[i], 12, tabW[i], 28, hwnd, IDC_PAGE_0 + i);

        auto page = [&](std::vector<HWND>& v, HWND h) { v.push_back(h); return h; };

        // ---- General page
        {
            HWND a = page(g_pageGeneral, NewCtrl(L"STATIC", L"Search engine:",
                WS_VISIBLE, 24, 52, 200, 20, hwnd, 0));
            (void)a;
            g_hwndEngineCombo = page(g_pageGeneral, NewCtrl(L"COMBOBOX", L"",
                WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 24, 74, 220, 160, hwnd, IDC_ENGINE_COMBO));
            page(g_pageGeneral, NewCtrl(L"STATIC", L"Custom engine URL (must contain %s):",
                WS_VISIBLE, 24, 104, 320, 20, hwnd, 0));
            g_hwndEngineUrl = page(g_pageGeneral, NewCtrl(L"EDIT", L"",
                WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 24, 126, 536, 24, hwnd, IDC_ENGINE_URL));
            page(g_pageGeneral, NewCtrl(L"STATIC", L"Browser for web + bang + quick links:",
                WS_VISIBLE, 24, 158, 340, 20, hwnd, 0));
            g_hwndBrowserCombo = page(g_pageGeneral, NewCtrl(L"COMBOBOX", L"",
                WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 24, 180, 360, 200, hwnd, IDC_BROWSER_COMBO));
            page(g_pageGeneral, NewCtrl(L"BUTTON", L"Browse...",
                WS_VISIBLE, 394, 179, 122, 26, hwnd, IDC_BROWSER_BROWSE));
            page(g_pageGeneral, NewCtrl(L"STATIC",
                L"Web + bang + quick links open in the chosen browser.\r\n"
                L"Every change saves automatically.",
                WS_VISIBLE, 24, 216, 536, 44, hwnd, 0));
            FillEngineCombo();
            FillBrowserCombo();
        }

        // ---- Hidden apps page (checkbox list)
        {
            page(g_pageApps, NewCtrl(L"STATIC", L"Check apps to hide from suggestions:",
                WS_VISIBLE, 24, 52, 400, 20, hwnd, 0));
            g_hwndAppsList = page(g_pageApps, NewCtrl(WC_LISTVIEWW, L"",
                WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                LVS_SHOWSELALWAYS, 24, 76, 536, 404, hwnd, IDC_APPS_LIST));
            ListView_SetExtendedListViewStyle(g_hwndAppsList,
                LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
            LVAddColumn(g_hwndAppsList, 0, L"Application", 500);
            ListView_SetBkColor(g_hwndAppsList, kBg);
            ListView_SetTextColor(g_hwndAppsList, kText);
            ListView_SetTextBkColor(g_hwndAppsList, kBg);
            FillAppsList();
        }

        // ---- Bangs page (in-place table: Add appends, double-click edits,
        // Del removes, colors auto-picked, everything saves instantly)
        {
            page(g_pageBangs, NewCtrl(L"STATIC",
                L"Add appends an editable row - double-click a cell to edit - Del removes:",
                WS_VISIBLE, 24, 52, 536, 20, hwnd, 0));
            g_hwndBangsList = page(g_pageBangs, NewCtrl(WC_LISTVIEWW, L"",
                WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                LVS_SHOWSELALWAYS | LVS_EDITLABELS, 24, 74, 536, 366, hwnd, IDC_BANGS_LIST));
            ListView_SetExtendedListViewStyle(g_hwndBangsList,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVAddColumn(g_hwndBangsList, 0, L"Aliases", 150);
            LVAddColumn(g_hwndBangsList, 1, L"Name", 120);
            LVAddColumn(g_hwndBangsList, 2, L"URL", 262);
            ListView_SetBkColor(g_hwndBangsList, kBg);
            ListView_SetTextColor(g_hwndBangsList, kText);
            ListView_SetTextBkColor(g_hwndBangsList, kBg);
            FillBangsList();
            page(g_pageBangs, NewCtrl(L"BUTTON", L"Add",
                WS_VISIBLE, 24, 448, 100, 26, hwnd, IDC_BANG_ADD));
        }

        // ---- Quick links page (same in-place pattern, no query)
        {
            page(g_pageQuick, NewCtrl(L"STATIC",
                L"Type the name in the palette - Enter opens the fixed URL. Del removes:",
                WS_VISIBLE, 24, 52, 536, 20, hwnd, 0));
            g_hwndQuickList = page(g_pageQuick, NewCtrl(WC_LISTVIEWW, L"",
                WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                LVS_SHOWSELALWAYS | LVS_EDITLABELS, 24, 74, 536, 366, hwnd, IDC_QUICK_LIST));
            ListView_SetExtendedListViewStyle(g_hwndQuickList,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            LVAddColumn(g_hwndQuickList, 0, L"Name", 170);
            LVAddColumn(g_hwndQuickList, 1, L"URL", 362);
            ListView_SetBkColor(g_hwndQuickList, kBg);
            ListView_SetTextColor(g_hwndQuickList, kText);
            ListView_SetTextBkColor(g_hwndQuickList, kBg);
            FillQuickList();
            page(g_pageQuick, NewCtrl(L"BUTTON", L"Add",
                WS_VISIBLE, 24, 448, 100, 26, hwnd, IDC_QUICK_ADD));
        }

        // Shared bottom buttons (always visible).
        NewCtrl(L"BUTTON", L"Rescan apps", WS_VISIBLE,
                16, 512, 120, 30, hwnd, IDC_RESCAN);
        NewCtrl(L"BUTTON", L"Close", WS_VISIBLE,
                146, 512, 100, 30, hwnd, IDC_CLOSE);

        // Silence the edit-control beep in settings (Enter/Esc/Tab chars).
        HWND plainEdits[] = {g_hwndEngineUrl};
        for (size_t i = 0; i < sizeof(plainEdits) / sizeof(plainEdits[0]); i++)
            SetWindowSubclass(plainEdits[i], PlainEditSubclass, 10 + (UINT_PTR)i, 0);

        ShowSettingsPage(0);
        ApplyUIFont(hwnd);
        g_fillingSettings = false;
        break;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)w;
        RECT r; GetClientRect(hwnd, &r);
        FillRect(dc, &r, g_brBg);
        return 1;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        SetTextColor(dc, kText);
        if (m == WM_CTLCOLOREDIT) {
            // Transparent: the black dialog shows through. Theme-proof, since
            // themed (v6) controls would otherwise paint a white field.
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, kBg);
        return (LRESULT)g_brBg;
    }
    case WM_DRAWITEM: {
        // Owner-drawn dark Settings controls: pushbuttons, checkboxes, combos.
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)l;
        HDC dc = d->hDC;
        if (d->CtlType == ODT_COMBOBOX) {
            bool field = (d->itemState & ODS_COMBOBOXEDIT) != 0;
            bool sel = !field && (d->itemState & ODS_SELECTED) != 0;
            HBRUSH bg = CreateSolidBrush(sel ? RGB(29, 65, 115) : kBg);
            FillRect(dc, &d->rcItem, bg);
            DeleteObject(bg);
            if ((int)d->itemID >= 0) {
                wchar_t buf[256] = {0};
                SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, (LPARAM)buf);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, kText);
                SelectObject(dc, g_fontUI);
                RECT tr = d->rcItem; tr.left += 8; tr.right -= 4;
                DrawTextW(dc, buf, -1, &tr,
                          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            }
            if (field) { // thin frame around the closed combo
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(66, 66, 72));
                HGDIOBJ op = SelectObject(dc, pen);
                SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, d->rcItem.left, d->rcItem.top,
                          d->rcItem.right, d->rcItem.bottom, 6, 6);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
            return TRUE;
        }
        if (d->CtlType != ODT_BUTTON) break;
        DWORD bs = (DWORD)GetWindowLongW(d->hwndItem, GWL_STYLE);
        bool isCheck = (bs & BS_TYPEMASK) == BS_CHECKBOX ||
                       (bs & BS_TYPEMASK) == BS_AUTOCHECKBOX;
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontUI);
        if (isCheck) {
            FillRect(dc, &d->rcItem, g_brBg);
            int boxS = 14;
            int by = d->rcItem.top + ((d->rcItem.bottom - d->rcItem.top) - boxS) / 2;
            RECT box{d->rcItem.left + 2, by, d->rcItem.left + 2 + boxS, by + boxS};
            HBRUSH bb = CreateSolidBrush(RGB(20, 20, 22));
            HPEN bp = CreatePen(PS_SOLID, 1, RGB(100, 100, 108));
            HGDIOBJ ob = SelectObject(dc, bb);
            HGDIOBJ op = SelectObject(dc, bp);
            RoundRect(dc, box.left, box.top, box.right, box.bottom, 4, 4);
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(bb); DeleteObject(bp);
            if (d->itemState & ODS_CHECKED) {
                HPEN cp = CreatePen(PS_SOLID, 2, kAppCol);
                HGDIOBJ ocp = SelectObject(dc, cp);
                POINT pts[3] = {
                    {box.left + 3, box.top + 7},
                    {box.left + 6, box.top + 10},
                    {box.left + 11, box.top + 4},
                };
                Polyline(dc, pts, 3);
                SelectObject(dc, ocp);
                DeleteObject(cp);
            }
            wchar_t text[128] = {0};
            GetWindowTextW(d->hwndItem, text, 128);
            SetTextColor(dc, kText);
            RECT tr = d->rcItem; tr.left = box.right + 8;
            DrawTextW(dc, text, -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            return TRUE;
        }
        bool pressed = (d->itemState & ODS_SELECTED) != 0;
        bool focus = (d->itemState & ODS_FOCUS) != 0;
        bool isSave = d->CtlID == (UINT)IDC_SAVE;
        bool isPage = d->CtlID >= (UINT)IDC_PAGE_0 && d->CtlID <= (UINT)IDC_PAGE_3;
        bool pageSel = isPage && (int)(d->CtlID - (UINT)IDC_PAGE_0) == g_settingsPage;
        bool accent = isSave || pageSel;
        FillRect(dc, &d->rcItem, g_brBg);
        COLORREF fill = accent ? (pressed ? RGB(38, 82, 140) : RGB(29, 65, 115))
                               : (pressed ? RGB(52, 52, 56) : RGB(26, 26, 28));
        COLORREF edge = accent ? kAppCol
                               : (focus ? RGB(130, 130, 140) : RGB(66, 66, 72));
        HBRUSH bg = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, edge);
        HGDIOBJ oldB = SelectObject(dc, bg);
        HGDIOBJ oldP = SelectObject(dc, pen);
        RoundRect(dc, d->rcItem.left, d->rcItem.top,
                  d->rcItem.right, d->rcItem.bottom, 8, 8);
        SelectObject(dc, oldB); SelectObject(dc, oldP);
        DeleteObject(bg); DeleteObject(pen);
        wchar_t text[128] = {0};
        GetWindowTextW(d->hwndItem, text, 128);
        SetTextColor(dc, kText);
        DrawTextW(dc, text, -1, &d->rcItem,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)l;
        if (nm->code == NM_CUSTOMDRAW &&
            (nm->hwndFrom == ListView_GetHeader(g_hwndAppsList) ||
             nm->hwndFrom == ListView_GetHeader(g_hwndBangsList) ||
             nm->hwndFrom == ListView_GetHeader(g_hwndQuickList))) {
            // Dark listview headers, painted manually.
            LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)l;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                FillRect(cd->hdc, &cd->rc, g_brBg);
                wchar_t buf[128] = {0};
                HDITEMW hdi{};
                hdi.mask = HDI_TEXT;
                hdi.pszText = buf;
                hdi.cchTextMax = 128;
                if (Header_GetItem(nm->hwndFrom, (int)cd->dwItemSpec, &hdi)) {
                    SetBkMode(cd->hdc, TRANSPARENT);
                    SetTextColor(cd->hdc, kSub);
                    SelectObject(cd->hdc, g_fontUI);
                    RECT tr = cd->rc; tr.left += 8;
                    DrawTextW(cd->hdc, buf, -1, &tr,
                              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
                }
                return CDRF_SKIPDEFAULT;
            }
        } else if (nm->hwndFrom == g_hwndAppsList && nm->code == LVN_ITEMCHANGED &&
                   !g_fillingSettings) {
            // Checkbox toggled -> hide-list + instant save (no Save button).
            NMLISTVIEW* v = (NMLISTVIEW*)l;
            if ((v->uChanged & LVIF_STATE) &&
                ((v->uNewState ^ v->uOldState) & LVIS_STATEIMAGEMASK)) {
                ReadHiddenFromList();
                SaveNow();
            }
        } else if (nm->hwndFrom == g_hwndBangsList || nm->hwndFrom == g_hwndQuickList) {
            if (nm->code == NM_DBLCLK && !g_fillingSettings) {
                NMITEMACTIVATE* ia = (NMITEMACTIVATE*)l;
                if (ia->iItem >= 0 && ia->iSubItem >= 0)
                    StartCellEdit(nm->hwndFrom, ia->iItem, ia->iSubItem);
            } else if (nm->code == LVN_ENDLABELEDIT && !g_fillingSettings) {
                // Built-in label edit covers column 0; validate + save.
                NMLVDISPINFOW* di = (NMLVDISPINFOW*)l;
                if (!di->item.pszText) return FALSE; // editing cancelled
                std::wstring t = Trim(di->item.pszText);
                int diItem = di->item.iItem;
                if (nm->hwndFrom == g_hwndBangsList) {
                    if (t.empty() || diItem >= (int)g_settings.bangs.size()) return FALSE;
                    std::wstring line = t + L" | " +
                        g_settings.bangs[diItem].name + L" | " +
                        g_settings.bangs[diItem].url + L" | " +
                        SerializeColor(g_settings.bangs[diItem].color);
                    Bang b;
                    if (!ParseBangLine(line, b)) {
                        MessageBoxW(hwnd, L"Aliases cannot be empty - change reverted.",
                                    L"SneekPeek", MB_ICONWARNING | MB_OK);
                        return FALSE;
                    }
                    b.color = g_settings.bangs[diItem].color;
                    g_settings.bangs[diItem] = b;
                    LVSetCell(nm->hwndFrom, diItem, 0, JoinAliases(b.aliases));
                    SaveNow();
                } else {
                    if (t.empty() || diItem >= (int)g_settings.quicks.size()) return FALSE;
                    g_settings.quicks[diItem].name = t;
                    LVSetCell(nm->hwndFrom, diItem, 0, t);
                    SaveNow();
                }
                return TRUE;
            } else if (nm->code == LVN_KEYDOWN && !g_fillingSettings) {
                NMLVKEYDOWN* k = (NMLVKEYDOWN*)l;
                if (k->wVKey == VK_DELETE) { // Del removes the selected row
                    int row = LVSelectedRow(nm->hwndFrom);
                    if (row >= 0) {
                        if (nm->hwndFrom == g_hwndBangsList &&
                            row < (int)g_settings.bangs.size())
                            g_settings.bangs.erase(g_settings.bangs.begin() + row);
                        else if (nm->hwndFrom == g_hwndQuickList &&
                                 row < (int)g_settings.quicks.size())
                            g_settings.quicks.erase(g_settings.quicks.begin() + row);
                        else
                            break;
                        ListView_DeleteItem(nm->hwndFrom, row);
                        SaveNow();
                    }
                }
            }
        }
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w), code = HIWORD(w);
        if (id >= IDC_PAGE_0 && id <= IDC_PAGE_3) {
            ShowSettingsPage(id - IDC_PAGE_0);
            for (int i = IDC_PAGE_0; i <= IDC_PAGE_3; i++) {
                HWND b = GetDlgItem(hwnd, i);
                if (b) InvalidateRect(b, NULL, FALSE);
            }
        } else if (id == IDC_ENGINE_COMBO && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hwndEngineCombo, CB_GETCURSEL, 0, 0);
            EnableWindow(g_hwndEngineUrl, sel == 4);
            if (sel == 0) SetWindowTextW(g_hwndEngineUrl, L"https://duckduckgo.com/?q=%s");
            if (sel == 1) SetWindowTextW(g_hwndEngineUrl, L"https://www.google.com/search?q=%s");
            if (sel == 2) SetWindowTextW(g_hwndEngineUrl, L"https://www.bing.com/search?q=%s");
            if (sel == 3) SetWindowTextW(g_hwndEngineUrl, L"https://search.brave.com/search?q=%s");
            SaveEngineBrowser(hwnd); // instant save, no Save button
        } else if (id == IDC_ENGINE_URL && code == EN_KILLFOCUS) {
            SaveEngineBrowser(hwnd);
        } else if (id == IDC_BROWSER_COMBO && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hwndBrowserCombo, CB_GETCURSEL, 0, 0);
            int count = (int)SendMessageW(g_hwndBrowserCombo, CB_GETCOUNT, 0, 0);
            if (sel == count - 1 && count > 1) { // "Browse..."
                std::wstring picked;
                if (BrowseForBrowser(hwnd, picked)) {
                    g_browsers.push_back({ExeFileName(picked), picked});
                    SendMessageW(g_hwndBrowserCombo, CB_INSERTSTRING, count - 1,
                                 (LPARAM)g_browsers.back().name.c_str());
                    SendMessageW(g_hwndBrowserCombo, CB_SETCURSEL, count - 1, 0);
                } else {
                    SendMessageW(g_hwndBrowserCombo, CB_SETCURSEL, 0, 0);
                }
            }
            SaveEngineBrowser(hwnd);
        } else if (id == IDC_BROWSER_BROWSE) {
            std::wstring picked;
            if (BrowseForBrowser(hwnd, picked)) {
                g_browsers.push_back({ExeFileName(picked), picked});
                int count = (int)SendMessageW(g_hwndBrowserCombo, CB_GETCOUNT, 0, 0);
                SendMessageW(g_hwndBrowserCombo, CB_INSERTSTRING, count - 1,
                             (LPARAM)g_browsers.back().name.c_str());
                SendMessageW(g_hwndBrowserCombo, CB_SETCURSEL, count - 1, 0);
                SaveEngineBrowser(hwnd);
            }
        } else if (id == IDC_BANG_ADD) {
            Bang b{{L"newsite"}, L"New site", L"https://example.com/?q=%s",
                   RandomBangColor()};
            g_settings.bangs.push_back(b);
            int row = LVAppendRow(g_hwndBangsList, b.aliases[0].c_str());
            LVSetCell(g_hwndBangsList, row, 1, b.name);
            LVSetCell(g_hwndBangsList, row, 2, b.url);
            SaveNow();
            ListView_SetItemState(g_hwndBangsList, row, LVIS_SELECTED, LVIS_SELECTED);
            ListView_EnsureVisible(g_hwndBangsList, row, FALSE);
            ListView_EditLabel(g_hwndBangsList, row); // type the alias in place
        } else if (id == IDC_QUICK_ADD) {
            QuickLink q{L"New link", L"https://example.com"};
            g_settings.quicks.push_back(q);
            int row = LVAppendRow(g_hwndQuickList, q.name.c_str());
            LVSetCell(g_hwndQuickList, row, 1, q.url);
            SaveNow();
            ListView_SetItemState(g_hwndQuickList, row, LVIS_SELECTED, LVIS_SELECTED);
            ListView_EnsureVisible(g_hwndQuickList, row, FALSE);
            ListView_EditLabel(g_hwndQuickList, row);
        } else if (id == IDC_RESCAN) {
            g_indexReady.store(false);
            BuildIndexAsync();
            MessageBoxW(hwnd, L"Rescanning Start Menu in the background.",
                        L"SneekPeek", MB_OK | MB_ICONINFORMATION);
        } else if (id == IDC_CLOSE) {
            CloseSettings();
        }
        break;
    }
    case WM_CLOSE:
        CloseSettings();
        break;
    case WM_DESTROY:
        g_hwndSettings = NULL;
        g_hwndCellEdit = NULL;
        g_cellLv = 0; g_cellRow = -1; g_cellCol = 0;
        g_hwndEngineCombo = g_hwndEngineUrl = g_hwndBrowserCombo = NULL;
        g_hwndAppsList = g_hwndBangsList = g_hwndQuickList = NULL;
        g_pageGeneral.clear(); g_pageApps.clear();
        g_pageBangs.clear(); g_pageQuick.clear();
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ------------------------------------------------------------- tray commands
static void OnTrayCommand(UINT id) {
    switch (id) {
    case ID_TRAY_SETTINGS: OpenSettings(); break;
    case ID_TRAY_REFRESH:
        g_indexReady.store(false);
        BuildIndexAsync();
        break;
    case ID_TRAY_STARTUP:
        g_settings.runAtStartup = !g_settings.runAtStartup;
        SaveSettings(g_settings);
        ApplyRunAtStartup(g_settings.runAtStartup);
        break;
    case ID_TRAY_QUIT:
        DestroyWindow(g_hwndPalette);
        break;
    }
}

// ------------------------------------------------------------- entry point
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmd, int) {
    g_hInst = hInst;
    srand((unsigned)GetTickCount()); // random bang colors for new entries

    // Single instance: forward to the running copy.
    HANDLE mutex = CreateMutexW(NULL, FALSE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(kPaletteClass, NULL);
        if (other) PostMessageW(other, WM_APP_SHOW, 0, 0);
        return 0;
    }

    // Per-monitor DPI (dynamic load, still runs on Win7).
    if (HMODULE u = LoadLibraryW(L"user32.dll")) {
        typedef BOOL (WINAPI *Fn)(DPI_AWARENESS_CONTEXT);
        if (Fn f = (Fn)GetProcAddress(u, "SetProcessDpiAwarenessContext"))
            f((DPI_AWARENESS_CONTEXT)-4 /* PER_MONITOR_AWARE_V2 */);
        FreeLibrary(u);
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);

    LoadSettings(g_settings);
    RebuildHiddenSet();

    // Fonts + brushes (created once, tiny GDI footprint).
    HDC screen = GetDC(NULL);
    int dpiY = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    g_fontInput = CreateFontW(-MulDiv(15, dpiY, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontTitle = CreateFontW(-MulDiv(11, dpiY, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontSub = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontChip = CreateFontW(-MulDiv(10, dpiY, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontUI = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_brBg = CreateSolidBrush(kBg);
    g_brInput = CreateSolidBrush(kInput);
    g_brSel = CreateSolidBrush(kSel);

    LoadAppIcons();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.lpszClassName = kPaletteClass;
    wc.lpfnWndProc = PaletteProc;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_hIconBig;
    wc.hIconSm = g_hIconSmall;
    wc.hbrBackground = g_brBg;
    // WS_CLIPCHILDREN: child repaints never flash the parent (no blink).
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    WNDCLASSEXW ws{};
    ws.cbSize = sizeof(ws);
    ws.hInstance = hInst;
    ws.lpszClassName = kSettingsClass;
    ws.lpfnWndProc = SettingsProc;
    ws.hCursor = LoadCursorW(NULL, IDC_ARROW);
    ws.hIcon = g_hIconBig;
    ws.hIconSm = g_hIconSmall;
    ws.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&ws);

    WNDCLASSEXW wcChip{};
    wcChip.cbSize = sizeof(wcChip);
    wcChip.hInstance = hInst;
    wcChip.lpszClassName = kChipClass;
    wcChip.lpfnWndProc = ChipProc;
    wcChip.hCursor = LoadCursorW(NULL, IDC_HAND);
    wcChip.hbrBackground = g_brBg;
    RegisterClassExW(&wcChip);

    WNDCLASSEXW wcRow{};
    wcRow.cbSize = sizeof(wcRow);
    wcRow.hInstance = hInst;
    wcRow.lpszClassName = kChipRowClass;
    wcRow.lpfnWndProc = ChipRowProc;
    wcRow.hCursor = LoadCursorW(NULL, IDC_HAND);
    wcRow.hbrBackground = g_brBg;
    RegisterClassExW(&wcRow);

    g_hwndPalette = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_COMPOSITED, // topmost, no taskbar, smooth
        kPaletteClass, L"SneekPeek",
        WS_POPUP | WS_CLIPCHILDREN, // borderless rounded region paints the shape
        0, 0, kWidth, kInputH + kItemH,
        NULL, NULL, hInst, NULL);
    if (!g_hwndPalette) return 1;
    EnableRoundCorners(g_hwndPalette);

    if (!RegisterHotKey(g_hwndPalette, HOTKEY_ID, MOD_CONTROL | MOD_NOREPEAT, VK_SPACE)) {
        MessageBoxW(NULL,
            L"Could not register Ctrl+Space.\nAnother app (IME, keyboard switcher, launcher) may own it.\n"
            L"SneekPeek will keep running - change the other app's hotkey, then restart SneekPeek.",
            L"SneekPeek", MB_OK | MB_ICONWARNING);
    }
    AddTrayIcon(g_hwndPalette);

    // Warm the index in the background so the first Ctrl+Space is instant.
    // Costs one short burst; afterwards the thread exits (0% CPU).
    BuildIndexAsync();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        // Dialog-manager keys (Tab navigation, Enter = Save) for Settings.
        if (g_hwndSettings && IsDialogMessageW(g_hwndSettings, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_hwndPalette, HOTKEY_ID);
    DeleteObject(g_fontInput); DeleteObject(g_fontTitle);
    DeleteObject(g_fontSub); DeleteObject(g_fontChip); DeleteObject(g_fontUI);
    DeleteObject(g_brBg); DeleteObject(g_brInput); DeleteObject(g_brSel);
    if (g_hIconBig) DestroyIcon(g_hIconBig);
    if (g_hIconSmall) DestroyIcon(g_hIconSmall);
    CloseHandle(mutex);
    return (int)msg.wParam;
}
