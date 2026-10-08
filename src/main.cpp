// SneekPeek — super-lightweight command palette for Windows.
// Pure Win32, no frameworks. Idle cost: 1 hidden window + GetMessage loop.
//   RAM idle: ~1-3 MB working set | CPU idle: 0% (blocking message loop).
//
// Hotkey : Ctrl+Space (toggle)
// UI     : top-centered popup (EDIT + owner-draw LISTBOX)
// Search : app index (Start Menu) + web + !bangs + tiny calculator
// Hide   : click outside (WM_ACTIVATE), Esc, Enter-after-launch
// Tray   : Settings / Refresh / Run-at-startup / Quit

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <wchar.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

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

// ---------------------------------------------------------------- constants
static const wchar_t* kPaletteClass  = L"SneekPeekPalette";
static const wchar_t* kSettingsClass = L"SneekPeekSettings";
static const wchar_t* kMutexName     = L"SneekPeek_SingleInstance_v1";
static const UINT HOTKEY_ID = 1;
static const UINT WM_TRAYICON = WM_APP + 10;
static const UINT WM_APP_SHOW = WM_APP + 11;
static const UINT ID_TRAY_SETTINGS = 1001;
static const UINT ID_TRAY_REFRESH  = 1002;
static const UINT ID_TRAY_STARTUP   = 1003;
static const UINT ID_TRAY_QUIT      = 1004;

static const int kWidth = 620;
static const int kInputH = 52;
static const int kItemH = 46;
static const int kMaxItems = 7;

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
    // Must contain only digits/operators/parens/dots/spaces to be considered.
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
static HWND        g_hwndSettings = NULL;
static HWND        g_hwndEngineCombo = NULL;
static HWND        g_hwndEngineUrl = NULL;
static HWND        g_hwndChkPath = NULL;
static HWND        g_hwndChkStartup = NULL;
static HFONT       g_fontInput = NULL;
static HFONT       g_fontTitle = NULL;
static HFONT       g_fontSub = NULL;
static HBRUSH      g_brBg = NULL;
static HBRUSH      g_brInput = NULL;
static HBRUSH      g_brSel = NULL;
static Settings    g_settings;
static std::vector<AppEntry> g_apps;
static std::atomic<bool> g_indexReady{false};
static std::atomic<bool> g_indexing{false};

enum class ResultKind { App, Web, Bang, Calc };
struct ResultItem {
    ResultKind kind;
    std::wstring title;   // main line
    std::wstring sub;     // hint line
    std::wstring action;  // lnk path | url | calc text
};
static std::vector<ResultItem> g_results;

static const COLORREF kBg    = RGB(30, 30, 46);
static const COLORREF kInput = RGB(24, 24, 37);
static const COLORREF kSel   = RGB(49, 65, 110);
static const COLORREF kText  = RGB(235, 235, 245);
static const COLORREF kSub   = RGB(150, 155, 175);
static const COLORREF kAccent= RGB(120, 160, 255);

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
        // If palette is open showing "Indexing…", refresh it.
        if (g_hwndPalette && IsWindowVisible(g_hwndPalette))
            PostMessageW(g_hwndPalette, WM_APP + 20, 0, 0);
    }).detach();
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
    int h = kInputH + kItemH;     // will grow in UpdateResults
    SetWindowPos(g_hwndPalette, HWND_TOPMOST, x, y, kWidth, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // Layout children for the current height
    SetWindowPos(g_hwndEdit, NULL, 12, 8, kWidth - 24, kInputH - 16, SWP_NOZORDER);
    SetWindowPos(g_hwndList, NULL, 12, kInputH, kWidth - 24, kItemH, SWP_NOZORDER);
}

static void ShowPalette() {
    if (!g_indexReady.load() && !g_indexing.load())
        BuildIndexAsync();
    PlacePalette();
    SetWindowTextW(g_hwndEdit, L"");
    ShowWindow(g_hwndPalette, SW_SHOW);
    SetForegroundWindow(g_hwndPalette);
    SetFocus(g_hwndEdit);
    UpdateResults();
}

static void HidePalette() {
    if (g_hwndPalette) ShowWindow(g_hwndPalette, SW_HIDE);
    if (g_hwndEdit) SetWindowTextW(g_hwndEdit, L"");
    g_results.clear();
    if (g_hwndList) SendMessageW(g_hwndList, LB_RESETCONTENT, 0, 0);
}

// ------------------------------------------------------------- results model
static void PushAppRow(const AppEntry& app) {
    ResultItem r;
    r.kind = ResultKind::App;
    r.title = app.name;
    r.sub = std::wstring(L"Open  ·  ") + app.target;
    r.action = app.target;
    g_results.push_back(std::move(r));
}

static void RebuildListControl() {
    SendMessageW(g_hwndList, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_results.size(); i++) {
        int idx = (int)SendMessageW(g_hwndList, LB_ADDSTRING, 0, (LPARAM)L"");
        SendMessageW(g_hwndList, LB_SETITEMDATA, idx, (LPARAM)i);
    }
    if (!g_results.empty())
        SendMessageW(g_hwndList, LB_SETCURSEL, 0, 0);

    // Grow/shrink window to fit rows (1..kMaxItems, at least 1 hint row).
    size_t rows = g_results.empty() ? 1 : (g_results.size() > kMaxItems ? kMaxItems : g_results.size());
    RECT wr; GetWindowRect(g_hwndPalette, &wr);
    int h = kInputH + (int)rows * kItemH + 12;
    SetWindowPos(g_hwndPalette, HWND_TOPMOST, wr.left, wr.top, kWidth, h,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    RECT cr; GetClientRect(g_hwndPalette, &cr);
    SetWindowPos(g_hwndList, NULL, 12, kInputH, cr.right - 24,
                 (int)rows * kItemH, SWP_NOZORDER);
    InvalidateRect(g_hwndList, NULL, TRUE);
}

static void UpdateResults() {
    g_results.clear();
    g_results.reserve(9);
    std::wstring q = Trim(GetEditText(g_hwndEdit));

    if (q.empty()) {
        ResultItem hint;
        hint.kind = ResultKind::Web;
        hint.title = L"Type to search…";
        hint.sub = L"apps  ·  web  ·  !g !yt !w !gh !so  ·  2+2  ·  ↑↓ navigate  Enter open  Esc close";
        hint.action = L"";
        g_results.push_back(std::move(hint));
        RebuildListControl();
        return;
    }

    // 1) Calculator (cheap, runs before everything)
    double calcVal = 0;
    if (TryCalc(q, calcVal)) {
        wchar_t buf[64];
        swprintf_s(buf, 64, L"%g", calcVal);
        ResultItem r{ ResultKind::Calc, std::wstring(L"= ") + buf,
                      L"Calculator  ·  Enter copies to clipboard", buf };
        g_results.push_back(std::move(r));
    }

    // 2) !bang path
    std::wstring bang, term;
    if (ParseBang(q, bang, term)) {
        const Bang* b = FindBang(bang);
        if (b) {
            ResultItem r{ ResultKind::Bang,
                          term.empty() ? std::wstring(L"Open ") + b->label
                                       : term,
                          std::wstring(L"!") + bang + L"  ·  " + b->label + L"  ·  Enter to search",
                          ExpandUrl(b->url, UrlEncodeQuery(term)) };
            g_results.push_back(std::move(r));
        } else {
            // Unknown bang → fall through to web search with raw text.
            ResultItem r{ ResultKind::Web, q,
                          std::wstring(L"Search ") + g_settings.engineName + L"  ·  unknown !bang",
                          ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)) };
            g_results.push_back(std::move(r));
        }
        // Still show app matches for the term (if any) below the bang row.
        if (!term.empty() && g_indexReady.load()) {
            for (auto& h : SearchApps(g_apps, term, 6))
                PushAppRow(*h.app);
        }
        RebuildListControl();
        return;
    }

    // 3) Normal: app matches + web fallback
    if (g_indexReady.load()) {
        for (auto& h : SearchApps(g_apps, q, kMaxItems - 1))
            PushAppRow(*h.app);
    } else {
        ResultItem r{ ResultKind::App, L"Indexing apps…",
                      L"First run scans Start Menu once — a moment", L"" };
        g_results.push_back(std::move(r));
    }
    ResultItem web{ ResultKind::Web, q,
                    std::wstring(L"Search ") + g_settings.engineName + L"  ·  Shift+Enter forces web",
                    ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)) };
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
    if (q.empty()) { HidePalette(); return; }

    // Copy-to-clipboard helper for calculator.
    auto CopyText = [](const std::wstring& t) {
        if (!OpenClipboard(g_hwndPalette)) return;
        EmptyClipboard();
        size_t bytes = (t.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) { memcpy(GlobalLock(h), t.c_str(), bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
        CloseClipboard();
    };

    // Shift+Enter (or forceWeb) → web search regardless of app matches.
    if (forceWeb) {
        std::wstring bang, term;
        if (ParseBang(q, bang, term)) {
            if (const Bang* b = FindBang(bang)) {
                ShellExecuteW(NULL, L"open", ExpandUrl(b->url, UrlEncodeQuery(term)).c_str(),
                              NULL, NULL, SW_SHOWNORMAL);
                HidePalette(); return;
            }
        }
        ShellExecuteW(NULL, L"open",
                      ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)).c_str(),
                      NULL, NULL, SW_SHOWNORMAL);
        HidePalette(); return;
    }

    int idx = CurrentSelection();
    // No explicit selection → decide: strong app match wins, else web.
    if (idx < 0) {
        ShellExecuteW(NULL, L"open",
                      ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)).c_str(),
                      NULL, NULL, SW_SHOWNORMAL);
        HidePalette(); return;
    }
    ResultItem& r = g_results[idx];
    switch (r.kind) {
    case ResultKind::App:
        if (!r.action.empty())
            ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL);
        break;
    case ResultKind::Web:
        if (!r.action.empty())
            ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL);
        else {
            // Empty-query hint row: do nothing.
            return;
        }
        break;
    case ResultKind::Bang:
        ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL);
        break;
    case ResultKind::Calc:
        CopyText(r.action);
        break;
    }
    HidePalette();
}

// ------------------------------------------------------------- edit subclass
static LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                    UINT_PTR, DWORD_PTR) {
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
        case VK_F5:
            BuildIndexAsync();
            return 0;
        }
    }
    return DefSubclassProc(h, m, w, l);
}

// ------------------------------------------------------------- palette wndproc
static LRESULT CALLBACK PaletteProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_hwndEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
            12, 8, kWidth - 24, kInputH - 16,
            hwnd, (HMENU)1, g_hInst, NULL);
        SendMessageW(g_hwndEdit, EM_SETCUEBANNER, TRUE,
            (LPARAM)L"Search apps, web, !bangs  (e.g.  code  ·  !g cats  ·  12*8)");
        SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)g_fontInput, TRUE);
        SetWindowSubclass(g_hwndEdit, EditSubclass, 1, 0);

        g_hwndList = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
            LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            12, kInputH, kWidth - 24, kItemH,
            hwnd, (HMENU)2, g_hInst, NULL);
        // Dark listbox: no border flicker, parent paints background.
        break;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
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
            // Left accent kind glyph: A / W / ! / =
            const wchar_t* glyph = L"›";
            COLORREF gc = kAccent;
            if (r.kind == ResultKind::App)       { glyph = L"A"; }
            else if (r.kind == ResultKind::Bang) { glyph = L"!"; }
            else if (r.kind == ResultKind::Calc) { glyph = L"="; }
            else                                 { glyph = L"W"; gc = kSub; }

            SetBkMode(dc, TRANSPARENT);
            RECT gr = d->rcItem; gr.left += 12; gr.right = gr.left + 26;
            gr.top += 8;
            SetTextColor(dc, gc);
            SelectObject(dc, g_fontTitle);
            DrawTextW(dc, glyph, -1, &gr, DT_LEFT | DT_TOP | DT_SINGLELINE);

            RECT tr = d->rcItem; tr.left += 44; tr.top += 5; tr.right -= 10;
            RECT sr = tr; sr.top += 22;
            SetTextColor(dc, kText);
            SelectObject(dc, g_fontTitle);
            DrawTextW(dc, r.title.c_str(), -1, &tr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetTextColor(dc, kSub);
            SelectObject(dc, g_fontSub);
            DrawTextW(dc, r.sub.c_str(), -1, &sr,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (sel) {
            HPEN pen = CreatePen(PS_SOLID, 1, kAccent);
            HPEN old = (HPEN)SelectObject(dc, pen);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, d->rcItem.left, d->rcItem.top,
                      d->rcItem.right, d->rcItem.bottom);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        return TRUE;
    }
    case WM_ACTIVATE:
        // Click-outside-to-dismiss: any activation leaving this window hides it
        // (child EDIT/LISTBOX focus keeps WA_ACTIVE on the top-level, so no
        // false hide while typing). Settings window is exempt.
        if (LOWORD(w) == WA_INACTIVE) {
            HWND now = (HWND)l;
            if (now != g_hwndSettings && !IsChild(g_hwndSettings, now))
                HidePalette();
        }
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

// ------------------------------------------------------------- app icon
static HICON g_hIconBig = NULL;   // window / Alt-Tab (32px)
static HICON g_hIconSmall = NULL; // tray + title bar (16px)
static void LoadAppIcons() {
    g_hIconBig = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_hIconSmall = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
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
    wcsncpy_s(nid.szTip, L"SneekPeek — Ctrl+Space", _TRUNCATE);
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
    UINT flags = MF_STRING;
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_STRING, ID_TRAY_REFRESH, L"Refresh apps");
    AppendMenuW(menu, MF_STRING | (g_settings.runAtStartup ? MF_CHECKED : MF_UNCHECKED),
                ID_TRAY_STARTUP, L"Run at startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
    (void)flags;
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required for the menu to dismiss correctly
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

// ------------------------------------------------------------- settings UI
static void FillEngineCombo() {
    SendMessageW(g_hwndEngineCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"DuckDuckGo");
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"Google");
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"Bing");
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"Brave");
    SendMessageW(g_hwndEngineCombo, CB_ADDSTRING, 0, (LPARAM)L"Custom…");
    int sel = 0;
    if (g_settings.engineName == L"Google") sel = 1;
    else if (g_settings.engineName == L"Bing") sel = 2;
    else if (g_settings.engineName == L"Brave") sel = 3;
    else if (g_settings.engineName != L"DuckDuckGo") sel = 4;
    SendMessageW(g_hwndEngineCombo, CB_SETCURSEL, sel, 0);
    SetWindowTextW(g_hwndEngineUrl, g_settings.engineUrl.c_str());
    EnableWindow(g_hwndEngineUrl, sel == 4);
}
static void OpenSettings() {
    if (g_hwndSettings) { ShowWindow(g_hwndSettings, SW_SHOW); SetForegroundWindow(g_hwndSettings); return; }
    int W = 440, H = 330;
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
static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        CreateWindowW(L"STATIC", L"Search engine:", WS_CHILD | WS_VISIBLE,
                      16, 16, 120, 20, hwnd, NULL, g_hInst, NULL);
        g_hwndEngineCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            16, 40, 200, 160, hwnd, (HMENU)101, g_hInst, NULL);
        CreateWindowW(L"STATIC", L"Custom engine URL (must contain %s):",
                      WS_CHILD | WS_VISIBLE, 16, 78, 280, 20, hwnd, NULL, g_hInst, NULL);
        g_hwndEngineUrl = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            16, 100, 392, 24, hwnd, (HMENU)102, g_hInst, NULL);
        g_hwndChkPath = CreateWindowW(L"BUTTON", L"Include PATH executables (more results, more RAM)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            16, 136, 392, 22, hwnd, (HMENU)103, g_hInst, NULL);
        g_hwndChkStartup = CreateWindowW(L"BUTTON", L"Run at startup",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            16, 162, 392, 22, hwnd, (HMENU)104, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      16, 200, 100, 30, hwnd, (HMENU)201, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Rescan apps", WS_CHILD | WS_VISIBLE,
                      126, 200, 120, 30, hwnd, (HMENU)202, g_hInst, NULL);
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE,
                      256, 200, 100, 30, hwnd, (HMENU)203, g_hInst, NULL);
        CreateWindowW(L"STATIC",
            L"Hotkey: Ctrl+Space  ·  Shift+Enter forces web  ·  Esc dismisses\r\nBangs: !g !d !yt !w !gh !so !r !m !t !a !x",
            WS_CHILD | WS_VISIBLE, 16, 242, 392, 44, hwnd, NULL, g_hInst, NULL);
        (void)f;
        FillEngineCombo();
        SendMessageW(g_hwndChkPath, BM_SETCHECK,
                     g_settings.includePathExes ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_hwndChkStartup, BM_SETCHECK,
                     g_settings.runAtStartup ? BST_CHECKED : BST_UNCHECKED, 0);
        break;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w), code = HIWORD(w);
        if (id == 101 && code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hwndEngineCombo, CB_GETCURSEL, 0, 0);
            EnableWindow(g_hwndEngineUrl, sel == 4);
            if (sel == 0) SetWindowTextW(g_hwndEngineUrl, L"https://duckduckgo.com/?q=%s");
            if (sel == 1) SetWindowTextW(g_hwndEngineUrl, L"https://www.google.com/search?q=%s");
            if (sel == 2) SetWindowTextW(g_hwndEngineUrl, L"https://www.bing.com/search?q=%s");
            if (sel == 3) SetWindowTextW(g_hwndEngineUrl, L"https://search.brave.com/search?q=%s");
        } else if (id == 201) { // Save
            int sel = (int)SendMessageW(g_hwndEngineCombo, CB_GETCURSEL, 0, 0);
            wchar_t url[1024]; GetWindowTextW(g_hwndEngineUrl, url, 1024);
            const wchar_t* names[] = { L"DuckDuckGo", L"Google", L"Bing", L"Brave", L"Custom" };
            const wchar_t* urls[] = {
                L"https://duckduckgo.com/?q=%s",
                L"https://www.google.com/search?q=%s",
                L"https://www.bing.com/search?q=%s",
                L"https://search.brave.com/search?q=%s", url };
            g_settings.engineName = names[sel < 0 || sel > 4 ? 0 : sel];
            g_settings.engineUrl = urls[sel < 0 || sel > 4 ? 0 : sel];
            if (std::wstring(url).find(L"%s") == std::wstring::npos &&
                g_settings.engineName == L"Custom") {
                MessageBoxW(hwnd, L"Custom URL must contain %s as the query placeholder.",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
                break;
            }
            g_settings.includePathExes =
                SendMessageW(g_hwndChkPath, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_settings.runAtStartup =
                SendMessageW(g_hwndChkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveSettings(g_settings);
            ApplyRunAtStartup(g_settings.runAtStartup);
            g_indexReady.store(false);
            BuildIndexAsync();
            MessageBoxW(hwnd, L"Saved.", L"SneekPeek", MB_OK | MB_ICONINFORMATION);
        } else if (id == 202) {
            g_indexReady.store(false);
            BuildIndexAsync();
            MessageBoxW(hwnd, L"Rescanning Start Menu in the background.",
                        L"SneekPeek", MB_OK | MB_ICONINFORMATION);
        } else if (id == 203) {
            CloseSettings();
        }
        break;
    }
    case WM_CLOSE:
        CloseSettings();
        break;
    case WM_DESTROY:
        g_hwndSettings = NULL;
        g_hwndEngineCombo = g_hwndEngineUrl = g_hwndChkPath = g_hwndChkStartup = NULL;
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

    // Per-monitor DPI (dynamic load → still runs on Win7).
    if (HMODULE u = LoadLibraryW(L"user32.dll")) {
        using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto f = (Fn)GetProcAddress(u, "SetProcessDpiAwarenessContext"))
            f((DPI_AWARENESS_CONTEXT)-4 /* PER_MONITOR_AWARE_V2 */);
        FreeLibrary(u);
    }
    InitCommonControls();

    LoadSettings(g_settings);

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
    g_brBg = CreateSolidBrush(kBg);
    g_brInput = CreateSolidBrush(kInput);
    g_brSel = CreateSolidBrush(kSel);

    LoadAppIcons();

    WNDCLASSW wc{};
    wc.hInstance = hInst;
    wc.lpszClassName = kPaletteClass;
    wc.lpfnWndProc = PaletteProc;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = g_hIconBig;
    wc.hIconSm = g_hIconSmall;
    wc.hbrBackground = g_brBg;
    RegisterClassW(&wc);

    WNDCLASSW ws{};
    ws.hInstance = hInst;
    ws.lpszClassName = kSettingsClass;
    ws.lpfnWndProc = SettingsProc;
    ws.hCursor = LoadCursorW(NULL, IDC_ARROW);
    ws.hIcon = g_hIconBig;
    ws.hIconSm = g_hIconSmall;
    ws.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&ws);

    g_hwndPalette = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, // topmost + no taskbar button
        kPaletteClass, L"SneekPeek",
        WS_POPUP | WS_BORDER,
        0, 0, kWidth, kInputH + kItemH,
        NULL, NULL, hInst, NULL);
    if (!g_hwndPalette) return 1;

    if (!RegisterHotKey(g_hwndPalette, HOTKEY_ID, MOD_CONTROL | MOD_NOREPEAT, VK_SPACE)) {
        MessageBoxW(NULL,
            L"Could not register Ctrl+Space.\nAnother app (IME, keyboard switcher, launcher) may own it.\n"
            L"SneekPeek will keep running — change the other app's hotkey, then restart SneekPeek.",
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
    DeleteObject(g_fontInput); DeleteObject(g_fontTitle); DeleteObject(g_fontSub);
    DeleteObject(g_brBg); DeleteObject(g_brInput); DeleteObject(g_brSel);
    if (g_hIconBig) DestroyIcon(g_hIconBig);
    if (g_hIconSmall) DestroyIcon(g_hIconSmall);
    CloseHandle(mutex);
    return (int)msg.wParam;
}
