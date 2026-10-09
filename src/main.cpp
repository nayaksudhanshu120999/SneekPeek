// SneekPeek - super-lightweight command palette for Windows.
// Pure Win32, no frameworks. Idle cost: 1 hidden window + GetMessage loop.
//   RAM idle: ~1-3 MB working set | CPU idle: 0% (blocking message loop).
//
// Hotkey : Ctrl+Space (toggle)
// UI     : black, opaque, rounded, minimal top-centered popup
// Search : app index (Start Menu) + web + !bang chips + tiny calculator
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

// ---------------------------------------------------------------- constants
static const wchar_t* kPaletteClass  = L"SneekPeekPalette";
static const wchar_t* kSettingsClass = L"SneekPeekSettings";
static const wchar_t* kChipClass     = L"SneekPeekChip";
static const wchar_t* kMutexName     = L"SneekPeek_SingleInstance_v1";
static const UINT HOTKEY_ID = 1;
static const UINT WM_TRAYICON = WM_APP + 10;
static const UINT WM_APP_SHOW = WM_APP + 11;
static const UINT ID_TRAY_SETTINGS = 1001;
static const UINT ID_TRAY_REFRESH  = 1002;
static const UINT ID_TRAY_STARTUP   = 1003;
static const UINT ID_TRAY_QUIT      = 1004;

static const int kWidth = 600;
static const int kInputH = 52;
static const int kItemH = 42;
static const int kMaxItems = 6;
static const int kCornerR = 14;
static const int kChipH = 28;

// Settings control IDs
enum {
    IDC_ENGINE_COMBO = 101, IDC_ENGINE_URL = 102, IDC_CHK_PATH = 103,
    IDC_CHK_STARTUP = 104, IDC_BROWSER_COMBO = 105, IDC_BROWSER_BROWSE = 106,
    IDC_HIDDEN_EDIT = 107, IDC_BANGS_EDIT = 108, IDC_BANGS_RESET = 109,
    IDC_SAVE = 201, IDC_RESCAN = 202, IDC_CLOSE = 203,
};

// ------------------------------------------------------------- small helpers
static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}
static std::wstring TrimLeft(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    return (a == std::wstring::npos) ? L"" : s.substr(a);
}
static std::wstring GetEditText(HWND hEdit) {
    int n = GetWindowTextLengthW(hEdit);
    if (n <= 0) return L"";
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(hEdit, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}
static COLORREF Darken(COLORREF c, int pct) {
    return RGB(GetRValue(c) * pct / 100,
               GetGValue(c) * pct / 100,
               GetBValue(c) * pct / 100);
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
static HWND        g_hwndSettings = NULL;
static HWND        g_hwndEngineCombo = NULL;
static HWND        g_hwndEngineUrl = NULL;
static HWND        g_hwndBrowserCombo = NULL;
static HWND        g_hwndHiddenEdit = NULL;
static HWND        g_hwndBangsEdit = NULL;
static HWND        g_hwndChkPath = NULL;
static HWND        g_hwndChkStartup = NULL;
static HFONT       g_fontInput = NULL;
static HFONT       g_fontTitle = NULL;
static HFONT       g_fontSub = NULL;
static HFONT       g_fontChip = NULL;
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

enum class ResultKind { App, Web, Bang, Calc, Hint };
struct ResultItem {
    ResultKind kind;
    std::wstring title;   // main line (never a file path)
    std::wstring sub;     // small hint line
    std::wstring action;  // lnk path | url | calc text (execution only)
    COLORREF color;       // category accent color
};
static std::vector<ResultItem> g_results;

// Minimal black palette. Category accents: App blue, Web green, Calc amber,
// Bang = per-site color, Hint = gray.
static const COLORREF kBg     = RGB(0, 0, 0);
static const COLORREF kInput  = RGB(12, 12, 12);
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
static void ApplyRoundCorners();
static void EnableRoundCorners(HWND hwnd);

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
    SetWindowPos(g_hwndEdit, NULL, 16, 10, kWidth - 32, 32, SWP_NOZORDER);
    int chipY = 10 + (32 - kChipH) / 2;
    SetWindowPos(g_hwndChip, NULL, 16, chipY, 10, kChipH, SWP_NOZORDER | SWP_HIDEWINDOW);
    SetWindowPos(g_hwndList, NULL, 10, kInputH, kWidth - 20, kItemH, SWP_NOZORDER);
}

static void ShowPalette() {
    if (!g_indexReady.load() && !g_indexing.load())
        BuildIndexAsync();
    DeactivateBang();
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

static int ChipWidthFor(const Bang& b) {
    HDC dc = GetDC(g_hwndPalette);
    HGDIOBJ old = SelectObject(dc, g_fontChip);
    RECT r{0, 0, 0, 0};
    DrawTextW(dc, b.name.c_str(), -1, &r, DT_SINGLELINE | DT_CALCRECT);
    SelectObject(dc, old);
    ReleaseDC(g_hwndPalette, dc);
    return (r.right - r.left) + 10 + 24 + 8;
}

static void ActivateBang(int idx, const std::wstring& query) {
    if (idx < 0 || idx >= (int)g_settings.bangs.size()) return;
    g_activeBang = idx;
    const Bang& b = g_settings.bangs[(size_t)idx];
    int w = ChipWidthFor(b);
    int chipY = 10 + (32 - kChipH) / 2;
    SetWindowPos(g_hwndChip, NULL, 16, chipY, w, kChipH,
                 SWP_NOZORDER | SWP_SHOWWINDOW);
    SendMessageW(g_hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(w + 8, 10));
    g_changingEdit = true;
    SetWindowTextW(g_hwndEdit, query.c_str());
    // Caret to the end of the preset query (matters for pasted "alias query").
    SendMessageW(g_hwndEdit, EM_SETSEL, (WPARAM)query.size(), (LPARAM)query.size());
    g_changingEdit = false;
    UpdateResults();
}

static void DeactivateBang() {
    g_activeBang = -1;
    if (g_hwndChip) ShowWindow(g_hwndChip, SW_HIDE);
    if (g_hwndEdit)
        SendMessageW(g_hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(10, 10));
}

// ------------------------------------------------------------- results model
static void PushAppRow(const AppEntry& app) {
    ResultItem r;
    r.kind = ResultKind::App;
    r.title = app.name;      // name only - never the file path
    r.sub = L"Application";
    r.action = app.target;
    r.color = kAppCol;
    g_results.push_back(std::move(r));
}

static void RebuildListControl() {
    SendMessageW(g_hwndList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_results.size(); i++) {
        int idx = (int)SendMessageW(g_hwndList, LB_ADDSTRING, 0, (LPARAM)L"");
        if (idx >= 0)
            SendMessageW(g_hwndList, LB_SETITEMDATA, idx, (LPARAM)i);
    }
    if (!g_results.empty())
        SendMessageW(g_hwndList, LB_SETCURSEL, 0, 0);

    size_t rows = g_results.empty() ? 1 : (g_results.size() > kMaxItems ? kMaxItems : g_results.size());
    RECT wr; GetWindowRect(g_hwndPalette, &wr);
    int h = kInputH + (int)rows * kItemH + 10;
    SetWindowPos(g_hwndPalette, HWND_TOPMOST, wr.left, wr.top, kWidth, h,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    RECT cr; GetClientRect(g_hwndPalette, &cr);
    SetWindowPos(g_hwndList, NULL, 10, kInputH, cr.right - 20,
                 (int)rows * kItemH, SWP_NOZORDER);
    ApplyRoundCorners();
    InvalidateRect(g_hwndList, NULL, TRUE);
}

static void UpdateResults() {
    g_results.clear();
    g_results.reserve(8);
    std::wstring raw = GetEditText(g_hwndEdit);
    std::wstring q = Trim(raw);

    // --- bang-chip mode: one row for the armed site
    if (const Bang* b = ActiveBang()) {
        ResultItem r;
        r.kind = ResultKind::Bang;
        r.title = q.empty() ? (L"Search " + b->name) : q;
        r.sub = b->name + L" - Enter to search";
        r.action = q.empty() ? BangHome(b->url) : ExpandUrl(b->url, UrlEncodeQuery(q));
        r.color = b->color;
        g_results.push_back(std::move(r));
        RebuildListControl();
        return;
    }

    if (q.empty()) {
        ResultItem hint{ResultKind::Hint, L"Start typing to search",
                        L"apps  /  web  /  alias + Space for site  /  math", L"", kHintCol};
        g_results.push_back(std::move(hint));
        RebuildListControl();
        return;
    }

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
                if (!IsHiddenApp(h.app->name)) PushAppRow(*h.app);
            }
        }
        RebuildListControl();
        return;
    }

    // --- normal: app matches + web fallback
    if (g_indexReady.load()) {
        for (auto& h : SearchApps(g_apps, q, kMaxItems - 1)) {
            if (!IsHiddenApp(h.app->name)) PushAppRow(*h.app);
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
        if (forceWeb)
            OpenUrl(ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)), g_settings.browserPath);
        else
            OpenUrl(q.empty() ? BangHome(b->url) : ExpandUrl(b->url, UrlEncodeQuery(q)),
                    g_settings.browserPath);
        HidePalette(); return;
    }

    if (q.empty()) { HidePalette(); return; }

    // Shift+Enter forces a web search with the raw text.
    if (forceWeb) {
        int bidx = -1; std::wstring term;
        if (ParseBangLeading(q, g_settings.bangs, bidx, term) && bidx >= 0) {
            const Bang& b = g_settings.bangs[(size_t)bidx];
            OpenUrl(term.empty() ? BangHome(b.url) : ExpandUrl(b.url, UrlEncodeQuery(term)),
                    g_settings.browserPath);
        } else {
            OpenUrl(ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)), g_settings.browserPath);
        }
        HidePalette(); return;
    }

    int idx = CurrentSelection();
    if (idx < 0) {
        OpenUrl(ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)), g_settings.browserPath);
        HidePalette(); return;
    }
    ResultItem& r = g_results[idx];
    switch (r.kind) {
    case ResultKind::App:
        if (!r.action.empty())
            ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL);
        break;
    case ResultKind::Web:
    case ResultKind::Bang:
        OpenUrl(r.action, g_settings.browserPath);
        break;
    case ResultKind::Calc:
        CopyText(r.action);
        break;
    case ResultKind::Hint:
        return; // empty-query hint row: keep the palette open
    }
    HidePalette();
}

// ------------------------------------------------------------- edit subclass
// Returns 0 (swallowed). WM_CHAR swallowing is what kills the system beep:
// a single-line EDIT beeps for Enter/Esc/Tab chars it cannot insert.
static LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                    UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR) {
        if (w == 13 || w == 10 || w == 27 || w == 9) return 0;
    }
    if (m == WM_KEYDOWN) {
        int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
        int count = (int)SendMessageW(g_hwndList, LB_GETCOUNT, 0, 0);
        switch (w) {
        case VK_ESCAPE: HidePalette(); return 0;
        case VK_RETURN: {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            ExecuteSelected(shift);
            return 0;
        }
        case VK_DOWN:
            if (count > 0) SendMessageW(g_hwndList, LB_SETCURSEL, (sel + 1) % count, 0);
            InvalidateRect(g_hwndList, NULL, TRUE);
            return 0;
        case VK_UP:
            if (count > 0) SendMessageW(g_hwndList, LB_SETCURSEL,
                          (sel <= 0 ? count - 1 : sel - 1), 0);
            InvalidateRect(g_hwndList, NULL, TRUE);
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

// ------------------------------------------------------------- chip window
static LRESULT CALLBACK ChipProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        const Bang* b = ActiveBang();
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        COLORREF col = b ? b->color : kHintCol;
        HBRUSH bg = CreateSolidBrush(Darken(col, 32));
        HPEN pen = CreatePen(PS_SOLID, 1, col);
        HGDIOBJ oldB = SelectObject(dc, bg);
        HGDIOBJ oldP = SelectObject(dc, pen);
        RoundRect(dc, 0, 0, r.right, r.bottom, 14, 14);
        SelectObject(dc, oldB); SelectObject(dc, oldP);
        DeleteObject(bg); DeleteObject(pen);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontChip);
        RECT tr = r; tr.left += 10; tr.right -= 24;
        if (b) {
            SetTextColor(dc, kText);
            DrawTextW(dc, b->name.c_str(), -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
        RECT xr = r; xr.left = xr.right - 20;
        SetTextColor(dc, RGB(170, 170, 175));
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

// ------------------------------------------------------------- palette wndproc
static LRESULT CALLBACK PaletteProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_hwndEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            16, 10, kWidth - 32, 32,
            hwnd, (HMENU)1, g_hInst, NULL);
        SendMessageW(g_hwndEdit, EM_SETCUEBANNER, TRUE,
            (LPARAM)L"Search apps, web, or type a bang + Space (yt, gh, w...)");
        SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)g_fontInput, TRUE);
        SendMessageW(g_hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(10, 10));
        SetWindowSubclass(g_hwndEdit, EditSubclass, 1, 0);

        g_hwndChip = CreateWindowExW(0, kChipClass, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            16, 12, 10, kChipH,
            hwnd, (HMENU)3, g_hInst, NULL);

        g_hwndList = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
            LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            10, kInputH, kWidth - 20, kItemH,
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
                            ActivateBang(bi, TrimLeft(editRaw.substr(p + 1)));
                            break;
                        }
                    }
                }
            }
            UpdateResults();
        } else if ((HWND)l == g_hwndList) {
            if (code == LBN_DBLCLK) ExecuteSelected(false);
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
            // Category color bar.
            HBRUSH bar = CreateSolidBrush(r.color);
            RECT br = d->rcItem;
            br.left += 12; br.right = br.left + 3;
            br.top += 9; br.bottom -= 9;
            FillRect(dc, &br, bar);
            DeleteObject(bar);

            RECT tr = d->rcItem; tr.left += 26; tr.top += 4; tr.right -= 10;
            RECT sr = tr; sr.top += 21;
            SetTextColor(dc, kText);
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
        // Click-outside-to-dismiss. Child EDIT/LISTBOX/CHIP focus keeps the
        // top-level active, so typing never hides. Settings is exempt.
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
            sel = (int)g_browsers.size(); // +1 offset, last item before Browse
        }
    }
    SendMessageW(g_hwndBrowserCombo, CB_ADDSTRING, 0, (LPARAM)L"Browse...");
    SendMessageW(g_hwndBrowserCombo, CB_SETCURSEL, sel, 0);
}

static std::wstring JoinLines(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += L"\r\n";
        s += v[i];
    }
    return s;
}
static std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find_first_of(L"\r\n", start);
        std::wstring t = Trim(s.substr(start, p == std::wstring::npos ? p : p - start));
        if (!t.empty()) out.push_back(t);
        if (p == std::wstring::npos) break;
        start = (s[p] == L'\r' && p + 1 < s.size() && s[p + 1] == L'\n') ? p + 2 : p + 1;
    }
    return out;
}
static std::wstring GetWindowString(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}

static void FillSettingsControls() {
    FillEngineCombo();
    FillBrowserCombo();
    SetWindowTextW(g_hwndHiddenEdit, JoinLines(g_settings.hiddenApps).c_str());
    std::wstring bangs;
    for (size_t i = 0; i < g_settings.bangs.size(); i++) {
        if (i) bangs += L"\r\n";
        bangs += SerializeBang(g_settings.bangs[i]);
    }
    SetWindowTextW(g_hwndBangsEdit, bangs.c_str());
    SendMessageW(g_hwndChkPath, BM_SETCHECK,
                 g_settings.includePathExes ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_hwndChkStartup, BM_SETCHECK,
                 g_settings.runAtStartup ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void OpenSettings() {
    if (g_hwndSettings) { ShowWindow(g_hwndSettings, SW_SHOW); SetForegroundWindow(g_hwndSettings); return; }
    int W = 480, H = 640;
    int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    g_hwndSettings = CreateWindowExW(0, kSettingsClass, L"SneekPeek Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, W, H, g_hwndPalette, NULL, g_hInst, NULL);
    ShowWindow(g_hwndSettings, SW_SHOW);
    UpdateWindow(g_hwndSettings);
}
static void CloseSettings() {
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

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        CreateWindowW(L"STATIC", L"Search engine:", WS_CHILD | WS_VISIBLE,
                      16, 14, 200, 20, hwnd, NULL, g_hInst, NULL);
        g_hwndEngineCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            16, 36, 200, 160, hwnd, (HMENU)IDC_ENGINE_COMBO, g_hInst, NULL);
        CreateWindowW(L"STATIC", L"Custom engine URL (must contain %s):",
                      WS_CHILD | WS_VISIBLE, 16, 66, 300, 20, hwnd, NULL, g_hInst, NULL);
        g_hwndEngineUrl = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            16, 88, 432, 24, hwnd, (HMENU)IDC_ENGINE_URL, g_hInst, NULL);

        CreateWindowW(L"STATIC", L"Browser for web + bang links:",
                      WS_CHILD | WS_VISIBLE, 16, 122, 300, 20, hwnd, NULL, g_hInst, NULL);
        g_hwndBrowserCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            16, 144, 300, 180, hwnd, (HMENU)IDC_BROWSER_COMBO, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Browse...",
            WS_CHILD | WS_VISIBLE, 326, 143, 122, 26,
            hwnd, (HMENU)IDC_BROWSER_BROWSE, g_hInst, NULL);

        CreateWindowW(L"STATIC", L"Hidden apps (one exact name per line, case-insensitive):",
                      WS_CHILD | WS_VISIBLE, 16, 180, 432, 20, hwnd, NULL, g_hInst, NULL);
        g_hwndHiddenEdit = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL,
            16, 202, 432, 76, hwnd, (HMENU)IDC_HIDDEN_EDIT, g_hInst, NULL);

        CreateWindowW(L"STATIC", L"Bangs (alias, alias | Name | url with %s | #RRGGBB):",
                      WS_CHILD | WS_VISIBLE, 16, 288, 432, 20, hwnd, NULL, g_hInst, NULL);
        g_hwndBangsEdit = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            16, 310, 432, 108, hwnd, (HMENU)IDC_BANGS_EDIT, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Reset bangs",
            WS_CHILD | WS_VISIBLE, 326, 424, 122, 26,
            hwnd, (HMENU)IDC_BANGS_RESET, g_hInst, NULL);

        g_hwndChkPath = CreateWindowW(L"BUTTON", L"Include PATH executables (more results, more RAM)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            16, 458, 432, 22, hwnd, (HMENU)IDC_CHK_PATH, g_hInst, NULL);
        g_hwndChkStartup = CreateWindowW(L"BUTTON", L"Run at startup",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            16, 482, 432, 22, hwnd, (HMENU)IDC_CHK_STARTUP, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      16, 512, 100, 30, hwnd, (HMENU)IDC_SAVE, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Rescan apps", WS_CHILD | WS_VISIBLE,
                      126, 512, 120, 30, hwnd, (HMENU)IDC_RESCAN, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
                      256, 512, 100, 30, hwnd, (HMENU)IDC_CLOSE, g_hInst, NULL);
        CreateWindowW(L"STATIC",
            L"Ctrl+Space toggle - Shift+Enter forces web - Esc hides\r\n"
            L"Bang chip: type alias + Space (yt, gh, w...) - Backspace on empty exits",
            WS_CHILD | WS_VISIBLE, 16, 552, 432, 40, hwnd, NULL, g_hInst, NULL);
        FillSettingsControls();
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w), code = HIWORD(w);
        if (id == IDC_ENGINE_COMBO && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hwndEngineCombo, CB_GETCURSEL, 0, 0);
            EnableWindow(g_hwndEngineUrl, sel == 4);
            if (sel == 0) SetWindowTextW(g_hwndEngineUrl, L"https://duckduckgo.com/?q=%s");
            if (sel == 1) SetWindowTextW(g_hwndEngineUrl, L"https://www.google.com/search?q=%s");
            if (sel == 2) SetWindowTextW(g_hwndEngineUrl, L"https://www.bing.com/search?q=%s");
            if (sel == 3) SetWindowTextW(g_hwndEngineUrl, L"https://search.brave.com/search?q=%s");
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
        } else if (id == IDC_BROWSER_BROWSE) {
            std::wstring picked;
            if (BrowseForBrowser(hwnd, picked)) {
                g_browsers.push_back({ExeFileName(picked), picked});
                int count = (int)SendMessageW(g_hwndBrowserCombo, CB_GETCOUNT, 0, 0);
                SendMessageW(g_hwndBrowserCombo, CB_INSERTSTRING, count - 1,
                             (LPARAM)g_browsers.back().name.c_str());
                SendMessageW(g_hwndBrowserCombo, CB_SETCURSEL, count - 1, 0);
            }
        } else if (id == IDC_BANGS_RESET) {
            std::wstring bangs;
            auto def = DefaultBangs();
            for (size_t i = 0; i < def.size(); i++) {
                if (i) bangs += L"\r\n";
                bangs += SerializeBang(def[i]);
            }
            SetWindowTextW(g_hwndBangsEdit, bangs.c_str());
        } else if (id == IDC_SAVE) {
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
                MessageBoxW(hwnd, L"Custom URL must contain %s as the query placeholder.",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
                break;
            }
            g_settings.engineName = names[esel];
            g_settings.engineUrl = urls[esel];

            int bsel = (int)SendMessageW(g_hwndBrowserCombo, CB_GETCURSEL, 0, 0);
            if (bsel <= 0) g_settings.browserPath.clear();
            else if (bsel - 1 < (int)g_browsers.size())
                g_settings.browserPath = g_browsers[(size_t)bsel - 1].path;

            g_settings.hiddenApps = SplitLines(GetWindowString(g_hwndHiddenEdit));

            std::vector<Bang> bangs;
            for (const auto& line : SplitLines(GetWindowString(g_hwndBangsEdit))) {
                Bang b;
                if (ParseBangLine(line, b)) bangs.push_back(std::move(b));
            }
            if (bangs.empty()) {
                MessageBoxW(hwnd, L"No valid bang lines. Format per line:\r\n"
                                  L"alias, alias | Name | https://...%s | #RRGGBB",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
                break;
            }
            g_settings.bangs = std::move(bangs);

            g_settings.includePathExes =
                SendMessageW(g_hwndChkPath, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_settings.runAtStartup =
                SendMessageW(g_hwndChkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveSettings(g_settings);
            ApplyRunAtStartup(g_settings.runAtStartup);
            RebuildHiddenSet();
            g_indexReady.store(false);
            BuildIndexAsync();
            MessageBoxW(hwnd, L"Saved.", L"SneekPeek", MB_OK | MB_ICONINFORMATION);
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
        g_hwndEngineCombo = g_hwndEngineUrl = g_hwndBrowserCombo = NULL;
        g_hwndHiddenEdit = g_hwndBangsEdit = g_hwndChkPath = g_hwndChkStartup = NULL;
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
    InitCommonControls();

    LoadSettings(g_settings);
    RebuildHiddenSet();

    // Fonts + brushes (created once, tiny GDI footprint).
    HDC screen = GetDC(NULL);
    int dpiY = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    g_fontInput = CreateFontW(-MulDiv(15, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

    g_hwndPalette = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, // topmost + no taskbar button
        kPaletteClass, L"SneekPeek",
        WS_POPUP, // borderless: rounded region paints the shape
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
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_hwndPalette, HOTKEY_ID);
    DeleteObject(g_fontInput); DeleteObject(g_fontTitle);
    DeleteObject(g_fontSub); DeleteObject(g_fontChip);
    DeleteObject(g_brBg); DeleteObject(g_brInput); DeleteObject(g_brSel);
    if (g_hIconBig) DestroyIcon(g_hIconBig);
    if (g_hIconSmall) DestroyIcon(g_hIconSmall);
    CloseHandle(mutex);
    return (int)msg.wParam;
}
