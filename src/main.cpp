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
#include <gdiplus.h>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring> // memcpy in the tile ellipsis probe
#include <new>     // std::nothrow
#include <wchar.h>
#include <string>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <thread>
#include <atomic>
#include <utility> // std::move in the hot result/file paths

#include "app_index.h"
#include "bangs.h"
#include "calc.h"
#include "files.h"
#include "icons.h"
#include "settings.h"
#include "syspages.h"
#include "results.h"
#include "verbs.h"
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
#pragma comment(lib, "gdiplus.lib")
#ifdef _MSC_VER
#pragma comment(lib, "delayimp.lib")
#endif

// ---------------------------------------------------------------- constants
static const wchar_t* kPaletteClass  = L"SneekPeekPalette";
static const wchar_t* kSettingsClass = L"SneekPeekSettings";
static const wchar_t* kChipClass     = L"SneekPeekChip";
static const wchar_t* kChipRowClass  = L"SneekPeekChipRow";
static const wchar_t* kFileBarClass  = L"SneekPeekFileBar";
static const wchar_t* kFileGridClass = L"SneekPeekFileGrid";
static const wchar_t* kMutexName     = L"SneekPeek_SingleInstance_v1";
static const UINT HOTKEY_ID = 1;
static const UINT WM_TRAYICON = WM_APP + 10;
static const UINT WM_APP_SHOW = WM_APP + 11;
static const UINT ID_TRAY_SETTINGS = 1001;
static const UINT ID_TRAY_REFRESH  = 1002;
static const UINT ID_TRAY_STARTUP   = 1003;
static const UINT ID_TRAY_QUIT      = 1004;
static const UINT ID_TRAY_AWAKE     = 1005;
// Deferred post-boot refresh: fired once via SetTimer so logon does no
// scan work inside Windows' startup measurement window.
static const UINT_PTR IDT_DEFERRED_REFRESH = 9001;
static const UINT DEFERRED_REFRESH_MS = 90000; // 90s: boot settles first

static int g_dpi = 96; // refreshed on WM_DPICHANGED
static int Dpx(int px) { return MulDiv(px, g_dpi, 96); }
// Layout metrics scale with DPI (recomputed by UpdateLayoutMetrics).
// Width stays fixed; compactness comes from slimmer rows, not density:
// data rows keep breathing room while headers/padding tighten up.
static int kWidth = 520;
static int kInputH = 40;   // one text line incl. vertical padding (grows when wrapping)
static int kItemH = 34;    // data rows
static int kHeadH = 22;    // section header rows (variable-height listbox)
static int kPadX = 12;     // horizontal margin (was 16)
static const int kMaxItems = 9;
static const int kMaxEditLines = 3; // wrapped search lines before scrolling
static int kCornerR = 12;
static int kChipH = 28;
static int kChipRowH = 34;
static int kBottomBarH = 26;
static int kGridTileH = 74; // Explorer-like tile height (glyph + 2 name lines)
static int kGridHeadH = 20; // slim count header above the tiles
static const int kMaxChips = 4;
static void UpdateLayoutMetrics() {
    kWidth = Dpx(520); kInputH = Dpx(40); kItemH = Dpx(34); kHeadH = Dpx(22);
    kPadX = Dpx(12);
    kCornerR = Dpx(12); kChipH = Dpx(28); kChipRowH = Dpx(34);
    kBottomBarH = Dpx(26);
    kGridTileH = Dpx(74); kGridHeadH = Dpx(20);
}

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
    IDC_RESCAN = 202, IDC_CLOSE = 203,
    IDC_VIEW_LIST = 204, IDC_VIEW_GRID = 205,
    IDC_ROWS_DEC = 206, IDC_ROWS_INC = 207,
    IDC_COLS_DEC = 208, IDC_COLS_INC = 209,
    IDC_PAGE_0 = 301, IDC_PAGE_1, IDC_PAGE_2, IDC_PAGE_3, IDC_PAGE_4, IDC_PAGE_5,
    // Theme swatches: IDC_THEME_BG + table index (29 fields: 401-429).
    IDC_THEME_BG = 401, IDC_THEME_INPUT, IDC_THEME_SEL, IDC_THEME_TEXT,
    IDC_THEME_ROWTEXT, IDC_THEME_SUB, IDC_THEME_APP, IDC_THEME_WEB,
    IDC_THEME_CALC, IDC_THEME_HINT, IDC_THEME_SET, IDC_THEME_AWAKE,
    IDC_THEME_FILE, IDC_THEME_FOLDER, IDC_THEME_QUICK, IDC_THEME_BTNFILL,
    IDC_THEME_BTNFILLHOT, IDC_THEME_BTNEDGE, IDC_THEME_EDGEHOT,
    IDC_THEME_BTNSEL, IDC_THEME_BTNSELHOT, IDC_THEME_SEP, IDC_THEME_MENUACC,
    IDC_THEME_CHIPTEXT, IDC_THEME_DANGER, IDC_THEME_POWDOWN,
    IDC_THEME_POWREBOOT, IDC_THEME_POWSLEEP, IDC_THEME_POWHIB,
    IDC_THEME_RESET, IDC_THEME_EDITFILE, IDC_THEME_RELOAD,
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
// Clipboard write shared by calc-copy, menu Copy verbs, and drag helpers.
// (NULL owner: placed before the globals; equivalent for task clipboard.)
static void CopyTextToClipboard(const std::wstring& t) {
    if (t.empty()) return;
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    size_t bytes = (t.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, t.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        } else {
            GlobalFree(h); // lock failed: don't leak, nothing copied
        }
    }
    CloseClipboard();
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
// Antialiased stadium pill (GDI shapes are jagged; GDI+ is not).
// Held for the duration of one paint pass: constructing a Gdiplus::Graphics
// saves/restores HDC state, and the chip row paints up to four pills per
// WM_PAINT (once per keystroke while chips are visible), so one Graphics per
// pass instead of one per pill. Lifetime is strictly bounded by the paint.
struct PillPainter {
    Gdiplus::Graphics* g;
    explicit PillPainter(HDC dc) : g(new (std::nothrow) Gdiplus::Graphics(dc)) {
        if (g) g->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    }
    ~PillPainter() { delete g; }
    PillPainter(const PillPainter&) = delete;
    PillPainter& operator=(const PillPainter&) = delete;
    void Fill(const RECT& r, COLORREF col) {
        if (!g) return;
        int d = r.bottom - r.top;
        if (d < 8) d = 8;
        if (r.right - r.left < d) return;
        Gdiplus::GraphicsPath path;
        path.AddArc(r.left, r.top, d, d, 90.0f, 180.0f);
        path.AddArc(r.right - d, r.top, d, d, 270.0f, 180.0f);
        path.CloseFigure();
        Gdiplus::SolidBrush br(Gdiplus::Color(255, GetRValue(col),
                                              GetGValue(col), GetBValue(col)));
        g->FillPath(&br, &path);
    }
};
static void FillPill(HDC dc, const RECT& r, COLORREF col) {
    PillPainter p(dc);
    p.Fill(r, col);
}

// Calculator lives in calc.h (shared with the unit tests).

// ------------------------------------------------------------------ globals


// ------------------------------------------------------------------ globals
static HINSTANCE   g_hInst = NULL;
static HWND        g_hwndPalette = NULL;
static HWND        g_hwndEdit = NULL;
static HWND        g_hwndList = NULL;
static HWND        g_hwndChip = NULL;
static HWND        g_hwndChipRow = NULL;
static HWND        g_hwndFileBar = NULL; // slim Explorer bar, file mode only
static HWND        g_hwndFileGrid = NULL; // tile grid, file grid-view only
static int         g_bangChipW = 0;  // 0 = hidden
// Hover preview row (results index, -1 = none): painted as a subtle fill,
// never touches selection or the multi-set.
static int         g_hoverRow = -1;
// Hover fill brush + the theme color it was built for (theme-derived: a theme
// change must drop it, see ApplyThemeFromSettings).
static HBRUSH      g_hoverBrush = NULL;
static COLORREF    g_hoverBrushFor = (COLORREF)-1;
static bool        g_hexOpen = false; // color-picker modal loop guard
static HWND        g_hwndSettings = NULL;
static int         g_settingsPage = 0;
static HWND        g_hwndEngineCombo = NULL;
static HWND        g_hwndEngineUrl = NULL;
static HWND        g_hwndBrowserCombo = NULL;
static HWND        g_hwndSavedLabel = NULL; // "Saved HH:MM:SS" indicator
static HWND        g_hwndRowsVal = NULL; // grid rows value label (Files tab)
static HWND        g_hwndColsVal = NULL; // grid columns value label (Files tab)
static HWND        g_hwndAppsList = NULL;
static HWND        g_hwndBangsList = NULL;
static HWND        g_hwndQuickList = NULL;
static HWND        g_hwndCellEdit = NULL; // in-place table cell editor
static int         g_cellLv = 0;   // 1 = bangs table, 2 = quick table
static int         g_cellRow = -1;
static int         g_cellCol = 0;
static std::vector<HWND> g_pageGeneral, g_pageApps, g_pageBangs, g_pageQuick, g_pageFiles,
    g_pageTheme;
static bool        g_fillingSettings = false;
// Theme swatches (Settings > Theme), IDC_THEME_BG + table index. The table
// itself lives in settings.h and drives defaults, INI, theme.ini, and UI.
static HWND        g_themeBtn[32] = {};
static HFONT       g_fontInput = NULL;
static HFONT       g_fontTitle = NULL;
static HFONT       g_fontSub = NULL;
static HFONT       g_fontChip = NULL;
static HFONT       g_fontGrid = NULL; // large bang-letter font for file tiles
static HFONT       g_fontUI = NULL; // Segoe UI for all Settings controls
// Lazy boot state (defined before the Ensure helpers that use them): the
// startup path avoids file/GDI/DLL work so Windows' startup-impact
// accounting sees a near-idle process. Heavy pieces load on first use.
static bool g_settingsListsLoaded = false; // hidden + bangs + quicks
static bool g_gdiplusStarted = false;
static bool g_appFontsReady = false;
static bool g_comCtlReady = false;
static bool g_startupDeferredArmed = false; // 90s light-refresh timer pending
static ULONG_PTR g_gdiplusToken = 0;
static void DeleteAppFonts() {
    HFONT fonts[] = {g_fontInput, g_fontTitle, g_fontSub, g_fontChip, g_fontGrid, g_fontUI};
    for (size_t i = 0; i < 6; i++)
        if (fonts[i]) DeleteObject(fonts[i]);
    g_fontInput = g_fontTitle = g_fontSub = g_fontChip = g_fontGrid = g_fontUI = NULL;
}
static void CreateAppFonts() {
    DeleteAppFonts();
    int dpiY = g_dpi;
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
    g_fontGrid = CreateFontW(-MulDiv(16, dpiY, 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontUI = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    // Row-height floor from the real font metrics: at any DPI the title text
    // must fit its row with padding, so glyphs can never clip vertically or
    // overlap. The search box gets the same treatment (typed "g"/"q"
    // descenders must clear the edit control). UpdateLayoutMetrics() owns
    // the DPI base; this only raises it.
    {
        HDC dc = GetDC(NULL);
        if (dc) {
            TEXTMETRICW tm{};
            if (g_fontTitle) {
                HGDIOBJ old = SelectObject(dc, g_fontTitle);
                if (GetTextMetricsW(dc, &tm)) {
                    int need = tm.tmHeight + Dpx(16);
                    if (need > kItemH) kItemH = need;
                }
                SelectObject(dc, old);
            }
            if (g_fontSub) {
                HGDIOBJ old = SelectObject(dc, g_fontSub);
                if (GetTextMetricsW(dc, &tm)) {
                    int need = tm.tmHeight + Dpx(10);
                    if (need > kHeadH) kHeadH = need;
                }
                SelectObject(dc, old);
            }
            if (g_fontInput) {
                HGDIOBJ old = SelectObject(dc, g_fontInput);
                if (GetTextMetricsW(dc, &tm)) {
                    // Full line (ascent + descent + leading) plus air above
                    // and below: the query's descenders never touch the edge.
                    int need = tm.tmHeight + tm.tmExternalLeading + Dpx(18);
                    if (need > kInputH) kInputH = need;
                }
                SelectObject(dc, old);
            }
            ReleaseDC(NULL, dc);
        }
    }
    g_appFontsReady = true;
}
static void RefreshEditLineH(); // defined with the layout helpers below
static void EnsureAppFonts() {
    if (g_appFontsReady) return;
    CreateAppFonts(); // sets g_appFontsReady
    RefreshEditLineH();
    if (g_hwndEdit && g_fontInput)
        SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)g_fontInput, TRUE);
    if (g_hwndList && g_fontTitle)
        SendMessageW(g_hwndList, WM_SETFONT, (WPARAM)g_fontTitle, TRUE);
}
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

// (Lazy boot state lives above, next to the font globals.)

static void RebuildHiddenSet(); // forward: used by EnsureSettingsLists
// (CreateAppFonts is defined above; EnsureAppFonts wraps it.)

// Loads hidden/bangs/quicks once. Must precede any SaveSettings (which
// rewrites those sections) and any palette search / Settings fill.
static void EnsureSettingsLists() {
    if (g_settingsListsLoaded) return;
    LoadSettingsLists(g_settings);
    RebuildHiddenSet();
    g_settingsListsLoaded = true;
}

// GDI+ is only needed to paint chip pills. Starting it on first show keeps
// gdiplus.dll out of the logon path.
static void EnsureGdiplus() {
    if (g_gdiplusStarted) return;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, NULL) == 0)
        g_gdiplusStarted = true;
}

// Segoe UI faces map font files; creating them on first show (not at logon)
// keeps that disk I/O out of the startup window.
// (EnsureAppFonts is defined above, next to CreateAppFonts.)

// comctl32 (listview/tab for Settings) loads on first Settings open.
static void EnsureComCtl() {
    if (g_comCtlReady) return;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);
    g_comCtlReady = true;
}

// Bang-chip mode: -1 = off, else index into g_settings.bangs.
static int  g_activeBang = -1;
static bool g_changingEdit = false;
// File-browse mode: single-level names-only listing. g_fileDir empty =
// drive list level, else a trailing-backslash dir. Only the open path is
// ever scanned (no parent/child prefetch, no cache).
static bool g_fileActive = false;
static std::wstring g_fileDir;
// File grid view (Settings toggle): Explorer-like tiles instead of rows.
// The hidden listbox stays the selection store; the grid only paints +
// hit-tests, so all nav/execute code keeps working unchanged.
static int g_gridTopRow = 0; // first visible tile row in grid mode
// Arrow navigation: original typed text, restored when wrapping past the ends.
static std::wstring g_origQuery;
static bool g_navigating = false;
// History recall (Up on an empty box): when true, g_results holds recent
// paths instead of search results; typing/esc/hide exits back to normal.
static bool g_histActive = false;
// (g_gdiplusToken is defined above, next to the font globals.)

// ------------------------------------------------------- power + quick chips
enum class PowerOp { Shutdown, Restart, Sleep, Hibernate };
struct PowerDef {
    PowerOp op;
    const wchar_t* title;
    COLORREF color;
    const wchar_t* aliases[4];
};
static PowerDef kPowers[] = { // colors follow the theme (see ApplyTheme)
    {PowerOp::Shutdown,  L"Shut down", RGB(255, 90, 90),
        {L"shutdown", L"shut down", L"shut", L"power off"}},
    {PowerOp::Restart,   L"Restart",   RGB(251, 191, 36),
        {L"restart", L"reboot", NULL, NULL}},
    {PowerOp::Sleep,     L"Sleep",     RGB(80, 140, 255),
        {L"sleep", NULL, NULL, NULL}},
    {PowerOp::Hibernate, L"Hibernate", RGB(160, 110, 255),
        {L"hibernate", L"hibernation", NULL, NULL}},
};
enum class ChipKind { Power, Quick, Bang, Awake };
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
static int  g_confirmPower = -1;  // armed PowerOp awaiting a second Enter

// Result model lives in results.h (shared with the unit tests).
static std::vector<ResultItem> g_results;

// Palette theme: mutable copies of Settings.theme (Settings > Theme edits
// these live, ApplyThemeFromSettings pushes them out). Defaults match
// DefaultTheme() so the first paint is correct even before settings load.
static COLORREF kBg     = RGB(0, 0, 0);
static COLORREF kInput  = RGB(0, 0, 0); // seamless with the bar: minimal
static COLORREF kSel    = RGB(28, 28, 30);
static COLORREF kText   = RGB(240, 240, 240);
static COLORREF kRowText= RGB(218, 218, 226); // unselected row text
static COLORREF kSub    = RGB(130, 130, 135);
static COLORREF kAppCol = RGB(96, 165, 250);
static COLORREF kWebCol = RGB(46, 224, 158);
static COLORREF kCalcCol= RGB(251, 191, 36);
static COLORREF kHintCol= RGB(110, 110, 115);
static COLORREF kSetCol = RGB(45, 212, 191);  // system settings pages
static COLORREF kAwakeCol = RGB(45, 212, 191);
static COLORREF kFileCol = RGB(96, 165, 250);   // files: blue
static COLORREF kFolderCol = RGB(251, 191, 36); // folders/drives: amber
static COLORREF kQuickCol = RGB(255, 95, 170);
static COLORREF kBtnFill = RGB(26, 26, 28);   // settings buttons
static COLORREF kBtnFillHot = RGB(52, 52, 56);
static COLORREF kBtnEdge = RGB(66, 66, 72);
static COLORREF kEdgeHot = RGB(130, 130, 140);
static COLORREF kBtnSel = RGB(29, 65, 115);   // accent selection blue
static COLORREF kBtnSelHot = RGB(38, 82, 140);
static COLORREF kSep    = RGB(30, 30, 32);    // hairlines + menu separators
static COLORREF kMenuAcc= RGB(150, 150, 155); // default menu accent
static COLORREF kChipText = RGB(255, 255, 255);
static COLORREF kDanger = RGB(255, 90, 90);   // quit + destructive-adjacent

// Push Settings.theme into the live palette: colors, brushes, repaints.
// Safe before windows exist (boot path): every handle use is guarded.
static void ApplyThemeFromSettings() {
    const Theme& t = g_settings.theme;
    kBg = t.bg; kInput = t.input; kSel = t.sel; kText = t.text;
    kRowText = t.rowtext; kSub = t.sub;
    kAppCol = t.app; kWebCol = t.web; kCalcCol = t.calc; kHintCol = t.hint;
    kSetCol = t.set; kAwakeCol = t.awake; kFileCol = t.file;
    kFolderCol = t.folder; kQuickCol = t.quick;
    kBtnFill = t.btnfill; kBtnFillHot = t.btnfillhot; kBtnEdge = t.btnedge;
    kEdgeHot = t.edgehot; kBtnSel = t.btnsel; kBtnSelHot = t.btnselhot;
    kSep = t.sep; kMenuAcc = t.menuacc; kChipText = t.chiptext;
    kDanger = t.danger;
    kPowers[0].color = t.powdown; kPowers[1].color = t.powreboot;
    kPowers[2].color = t.powsleep; kPowers[3].color = t.powhib;
    // Theme-derived paint resources: glyph pens/brushes and the hover fill
    // bake their colors into the handles, so a theme change must drop them or
    // they keep painting the previous palette.
    ReleaseGlyphGdi();
    ReleaseHoverBrush();
    if (g_brBg) DeleteObject(g_brBg);
    if (g_brInput) DeleteObject(g_brInput);
    if (g_brSel) DeleteObject(g_brSel);
    g_brBg = CreateSolidBrush(kBg);
    g_brInput = CreateSolidBrush(kInput);
    g_brSel = CreateSolidBrush(kSel);
    // Open Settings tables follow the theme without a reopen.
    HWND tables[] = {g_hwndAppsList, g_hwndBangsList, g_hwndQuickList};
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) {
        if (tables[i] && IsWindow(tables[i])) {
            ListView_SetBkColor(tables[i], kBg);
            ListView_SetTextColor(tables[i], kText);
            ListView_SetTextBkColor(tables[i], kBg);
            InvalidateRect(tables[i], NULL, TRUE);
        }
    }
    if (g_hwndPalette && IsWindow(g_hwndPalette))
        InvalidateRect(g_hwndPalette, NULL, TRUE);
    if (g_hwndSettings && IsWindow(g_hwndSettings))
        InvalidateRect(g_hwndSettings, NULL, TRUE);
    // Window classes cache the background brush handle: point them at the
    // new brush so a stale (deleted) handle is never referenced.
    HWND themed[] = {g_hwndPalette, g_hwndChip, g_hwndChipRow,
                     g_hwndFileBar, g_hwndFileGrid};
    for (size_t i = 0; i < sizeof(themed) / sizeof(themed[0]); i++) {
        if (themed[i] && IsWindow(themed[i]))
            SetClassLongPtrW(themed[i], GCLP_HBRBACKGROUND, (LONG_PTR)g_brBg);
    }
}

// Theme swatch buttons: IDC_THEME_BG + table index (see the IDs below).
// Awake state (see below).
static bool g_awakeOn = false;

// ------------------------------------------------------- forward declarations
static void ShowPalette();
static void HidePalette();
static void UpdateResults();
static void ExecuteSelected(bool forceWeb);
static void ExecuteChip(int idx);
static void BuildIndexAsync(bool full = true);
static void AddTrayIcon(HWND hwnd);
static void RemoveTrayIcon(HWND hwnd);
static void ShowTrayMenu(HWND hwnd);
static void OpenSettings();
static void CloseSettings();
static void SaveNow();
static void OnTrayCommand(UINT id);
// Drop the theme-derived hover-fill brush. Defined with the painters further
// down; declared here because ApplyThemeFromSettings and teardown both call it
// before that point in the file.
static void ReleaseHoverBrush();
static void RebuildHiddenSet();
static bool IsHiddenAppLower(const std::wstring& nameLower);
static void ActivateBang(int idx, const std::wstring& query);
static void DeactivateBang();
static bool InFileMode();
static void ActivateFileMode(const std::wstring& dir);
static void DeactivateFileMode();
static void UpdateFileResults(const std::wstring& filter);
static void ExecuteFileSelected(bool inExplorer);
static void HistoryRecord(const HistoryItem& item);
static void KeyboardContextMenu();
static std::wstring SystemExe(const wchar_t* name);
static void RebuildListControl();
static int CurrentSelection();
static void EnsureVisible(int sel);
static LRESULT CALLBACK ListSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                     UINT_PTR id, DWORD_PTR ref);
static void CloseCellEdit(bool commit);
static void ApplyAwake();
static void ApplyUIFont(HWND parent);
static void NotifyLaunchFailed(const wchar_t* msg);
static int MoveSelNoWrap(int from, int dir);
static void FillAppsList();
static std::wstring LVGetCell(HWND lv, int row, int col);
static void ApplyRoundCorners();
static void EnableRoundCorners(HWND hwnd);
static void RunPower(PowerOp op);
static void EnsureOle();
static void ArmRowDrag(int resultsIdx, POINT pt);
static void MoveRowDrag(HWND h, POINT pt);
static void CancelRowDrag();

// ------------------------------------------------------------- index thread
// Publish pattern: the worker builds into a fresh vector, swaps it into an
// atomic slot, and the UI thread adopts it on WM_APP_INDEX. The UI never
// touches g_apps while the worker runs, so no lock is needed. Options are
// captured on the UI thread before the worker starts. The worker thread is
// joinable (never detached): shutdown sets the stop flag and joins it.
static std::atomic<std::vector<AppEntry>*> g_pendingIndex{nullptr};
static std::atomic<bool> g_indexStop{false};
static std::atomic<bool> g_quitting{false}; // set at teardown: deferred work stands down
static std::thread g_indexThread; // guarded by g_indexing: join before reuse
static const UINT WM_APP_INDEX = WM_APP + 22; // background index published
// full=true runs every phase incl. PowerShell Get-StartApps (manual Refresh,
// F5, cache-miss). full=false is the light pass for the automatic post-boot
// refresh: .lnk + primary Store enum only, no child process.
static void BuildIndexAsync(bool full) {
    bool expected = false;
    if (!g_indexing.compare_exchange_strong(expected, true)) return; // already running
    if (g_indexThread.joinable()) g_indexThread.join(); // reap finished worker
    g_indexStop.store(false);
    bool pathExes = g_settings.includePathExes; // captured on the UI thread
    g_indexThread = std::thread([pathExes, full]{
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        try {
            std::vector<AppEntry> fresh;
            BuildAppIndex(fresh, pathExes, &g_indexStop, /*skipHeavy=*/!full);
            if (!g_indexStop.load()) {
                auto* old = g_pendingIndex.exchange(
                    new std::vector<AppEntry>(std::move(fresh)));
                delete old; // drop a superseded pending index, if any
                if (g_hwndPalette) PostMessageW(g_hwndPalette, WM_APP_INDEX, 0, 0);
            }
        } catch (...) {
            // Allocation failure (OOM) or any exception: publish nothing and
            // always release the indexing flag so future scans aren't blocked.
        }
        g_indexing.store(false);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    });
}

static void RebuildHiddenSet() {
    g_hidden.clear();
    for (const auto& n : g_settings.hiddenApps)
        g_hidden.insert(BangLower(BangTrim(n)));
}
// `nameLower` must already be lowercase. AppEntry caches it, and g_hidden
// holds lowercased keys, so this needs no per-app temporary copy (the previous
// ToLower-by-value allocated one string for every indexed app).
static bool IsHiddenAppLower(const std::wstring& nameLower) {
    return g_hidden.find(nameLower) != g_hidden.end();
}

// Refill the Hidden-apps checklist after a background index finishes,
// preserving whatever the user has currently checked (unsaved toggles win).
static void RefreshAppsListPreservingChecks() {
    if (!g_hwndAppsList) return;
    std::unordered_set<std::wstring> checked;
    int n = ListView_GetItemCount(g_hwndAppsList);
    for (int i = 0; i < n; i++)
        if (ListView_GetCheckState(g_hwndAppsList, i))
            checked.insert(LVGetCell(g_hwndAppsList, i, 0));
    FillAppsList();
    n = ListView_GetItemCount(g_hwndAppsList);
    for (int i = 0; i < n; i++)
        if (checked.count(LVGetCell(g_hwndAppsList, i, 0)))
            ListView_SetCheckState(g_hwndAppsList, i, TRUE);
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
//
// dwmapi.dll is resolved once for the process and deliberately never freed.
// Both call sites used to LoadLibrary + GetProcAddress + FreeLibrary on every
// invocation, so EnableRoundCorners did that on every single Ctrl+Space.
typedef HRESULT (WINAPI *FnDwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
static FnDwmSetWindowAttribute DwmSetWindowAttribute() {
    static FnDwmSetWindowAttribute fn = [] {
        HMODULE d = LoadLibraryW(L"dwmapi.dll");
        if (!d) return (FnDwmSetWindowAttribute)NULL;
        return (FnDwmSetWindowAttribute)GetProcAddress(d, "DwmSetWindowAttribute");
    }();
    return fn;
}
static void EnableRoundCorners(HWND hwnd) {
    if (FnDwmSetWindowAttribute f = DwmSetWindowAttribute()) {
        DWORD round = 2; // DWMWCP_ROUND
        f(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
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
// Search-box line height (edit font + a little leading), cached whenever
// fonts are (re)built. Multiline wrap grows the input area by whole lines,
// capped so the palette stays compact.
static int g_editLineH = 0;
static void RefreshEditLineH() {
    g_editLineH = 0;
    if (!g_fontInput) return;
    HWND own = (g_hwndEdit && IsWindow(g_hwndEdit)) ? g_hwndEdit : NULL;
    HDC dc = own ? GetDC(own) : GetDC(NULL);
    if (!dc) return;
    HGDIOBJ old = SelectObject(dc, g_fontInput);
    TEXTMETRICW tm{};
    if (GetTextMetricsW(dc, &tm))
        g_editLineH = tm.tmHeight + tm.tmExternalLeading + Dpx(4);
    SelectObject(dc, old);
    ReleaseDC(own, dc);
    if (g_editLineH < Dpx(16)) g_editLineH = Dpx(16);
}
// Displayed (wrapped) line count, clamped to the scroll cap.
static int EditLineCount() {
    if (!g_hwndEdit || !IsWindow(g_hwndEdit)) return 1;
    int n = (int)SendMessageW(g_hwndEdit, EM_GETLINECOUNT, 0, 0);
    if (n < 1) n = 1;
    if (n > kMaxEditLines) n = kMaxEditLines;
    return n;
}
// Current input-area height: one-line base plus extra wrapped lines.
static int CurrentInputH() {
    int lines = EditLineCount();
    if (lines <= 1 || g_editLineH <= 0) return kInputH;
    return kInputH + (lines - 1) * g_editLineH;
}
// Height of one result row (headers are slim).
static int RowH(size_t i) {
    if (i < g_results.size() && g_results[i].kind == ResultKind::Header) return kHeadH;
    return kItemH;
}
// Visible data rows from the current top index. Row heights come from RowH(),
// which is exactly what our WM_MEASUREITEM handler reports for the listbox —
// so this matches what is painted, without a LB_GETITEMHEIGHT round-trip per
// row (on LBS_OWNERDRAWVARIABLE that message dispatches WM_MEASUREITEM right
// back to us, once per row, on every keystroke).
static int VisibleRowCount() {
    if (!g_hwndList) return 1;
    RECT lr; GetClientRect(g_hwndList, &lr);
    int h = lr.bottom - lr.top;
    int top = (int)SendMessageW(g_hwndList, LB_GETTOPINDEX, 0, 0);
    if (top < 0) top = 0;
    int n = 0, y = 0;
    for (size_t i = (size_t)top; i < g_results.size() && y < h; i++) {
        y += RowH(i);
        n++;
    }
    return n < 1 ? 1 : n;
}
static void LayoutPaletteChildren() {
    // Child geometry in current metrics (no move/show of the top-level).
    // The input area grows with wrapped search lines; the chip stays pinned
    // to the first line.
    RECT cr; GetClientRect(g_hwndPalette, &cr);
    int W = cr.right;
    int inputH = CurrentInputH();
    int editH = inputH - Dpx(14);
    int editY = Dpx(9);
    SetWindowPos(g_hwndEdit, NULL, kPadX, editY, W - kPadX * 2, editH, SWP_NOZORDER);
    int firstH = g_editLineH > 0 ? g_editLineH : editH;
    int chipY = editY + (firstH - kChipH) / 2;
    if (chipY < editY) chipY = editY;
    UINT chipFlags = SWP_NOZORDER | SWP_NOSIZE |
        (IsWindowVisible(g_hwndChip) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    SetWindowPos(g_hwndChip, NULL, kPadX, chipY, 0, 0, chipFlags);
    UINT rowFlags = SWP_NOZORDER | SWP_NOSIZE |
        (IsWindowVisible(g_hwndChipRow) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
    SetWindowPos(g_hwndChipRow, NULL, kPadX, inputH, 0, 0, rowFlags);
    if (IsWindowVisible(g_hwndList)) {
        RECT lr; GetWindowRect(g_hwndList, &lr);
        MapWindowPoints(NULL, g_hwndPalette, (POINT*)&lr, 2);
        SetWindowPos(g_hwndList, NULL, kPadX, lr.top, W - kPadX * 2,
                     lr.bottom - lr.top, SWP_NOZORDER);
        if (g_hwndFileBar && IsWindowVisible(g_hwndFileBar))
            SetWindowPos(g_hwndFileBar, NULL, kPadX, lr.bottom, W - kPadX * 2,
                         kBottomBarH, SWP_NOZORDER);
    }
}

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
    LayoutPaletteChildren();
    // Chips row + list start hidden until results arrive.
    ShowWindow(g_hwndChipRow, SW_HIDE);
    ShowWindow(g_hwndChip, SW_HIDE);
    ShowWindow(g_hwndFileBar, SW_HIDE);
    ShowWindow(g_hwndFileGrid, SW_HIDE);
}

static void ShowPalette() {
    EnsureSettingsLists();
    EnsureGdiplus();
    EnsureAppFonts();
    EnsureOle(); // drag-out / drop-in need UI-thread OLE, lazily past boot
    EnableRoundCorners(g_hwndPalette);
    if (!g_indexReady.load() && !g_indexing.load())
        BuildIndexAsync(true); // cache miss during use: full scan now
    DeactivateBang();
    DeactivateFileMode();
    g_chips.clear();
    g_chipsFocus = false;
    g_histActive = false;
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
    DeactivateFileMode();
    g_hoverRow = -1;
    g_histActive = false;
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
    return (r.right - r.left) + Dpx(10) + Dpx(24) + Dpx(8);
}

// Single place that maps chip visibility -> edit text margins.
static void ApplyEditMargins() {
    if (!g_hwndEdit) return;
    SendMessageW(g_hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(g_bangChipW ? g_bangChipW + Dpx(8) : Dpx(10), Dpx(10)));
}

static void ActivateBang(int idx, const std::wstring& query) {
    if (idx < 0 || idx >= (int)g_settings.bangs.size()) return;
    g_activeBang = idx;
    g_navigating = false; // fresh mode, fresh arrow cycling
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
    // Same first-line pinning as PositionFileChip (see above).
    int firstH = er.bottom - er.top;
    if (g_editLineH > 0 && firstH > g_editLineH) firstH = g_editLineH;
    int chH = firstH - 4;
    if (chH > kChipH) chH = kChipH;
    if (chH < 18) chH = 18;
    int chipX = er.left + Dpx(2);
    int chipY = er.top + (firstH - chH) / 2;
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
    g_confirmPower = -1; // any context change disarms a pending confirm
    g_navigating = false;
    g_bangChipW = 0;
    if (g_hwndChip) ShowWindow(g_hwndChip, SW_HIDE);
    if (g_hwndEdit) {
        ApplyEditMargins();
        SendMessageW(g_hwndEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
    }
}

// ------------------------------------------------------------- file-browse mode
// ":" + Space enters a names-only browser: drives -> single-level dir
// listing. Only g_fileDir is ever scanned (one FindFirstFileW per refresh).
static bool InFileMode() { return g_fileActive; }

// Drive list cache: pure enumeration, no volume I/O. Rows show the bare letter
// ("C:\") rather than "Win 10 (C:)", because GetVolumeInformationW blocks for as
// long as the OS retries a volume that isn't answering - an empty optical drive
// or an offline network share froze the palette on the ": " that opens file mode.
// Reading labels on a worker thread would keep them, at the cost of thread +
// message state this design otherwise avoids; the bare letter is the better
// trade for a launcher.
static std::vector<DriveInfo> g_drives;
static void RefreshDriveCache() {
    g_drives.clear();
    for (const auto& root : GetLogicalDrivesWin32())
        g_drives.push_back(DriveInfo{root, root});
}

static void PositionFileChip() {
    if (!g_hwndChip || !g_hwndEdit || !g_hwndPalette) return;
    HDC dc = GetDC(g_hwndPalette);
    HGDIOBJ old = SelectObject(dc, g_fontChip);
    int w = ChipWidthFor(dc, L"Files");
    SelectObject(dc, old);
    ReleaseDC(g_hwndPalette, dc);
    RECT er; GetWindowRect(g_hwndEdit, &er);
    MapWindowPoints(NULL, g_hwndPalette, (POINT*)&er, 2);
    // Pin to the first text line: a wrapped query must not stretch the pill.
    int firstH = er.bottom - er.top;
    if (g_editLineH > 0 && firstH > g_editLineH) firstH = g_editLineH;
    int chH = firstH - 4;
    if (chH > kChipH) chH = kChipH;
    if (chH < 18) chH = 18;
    int chipX = er.left + Dpx(2);
    int chipY = er.top + (firstH - chH) / 2;
    SetWindowPos(g_hwndChip, HWND_TOP, chipX, chipY, w, chH, SWP_SHOWWINDOW);
    InvalidateRect(g_hwndChip, NULL, TRUE);
    UpdateWindow(g_hwndChip);
    g_bangChipW = w;
    ApplyEditMargins();
    SendMessageW(g_hwndEdit, EM_SETCUEBANNER, FALSE, (LPARAM)L"");
}

static void ActivateFileMode(const std::wstring& dir) {
    DeactivateBang(); // mutually exclusive with bang-chip mode
    g_fileActive = true;
    g_fileDir = dir.empty() ? L"" : EnsureTrailingSlash(dir);
    g_navigating = false;
    g_confirmPower = -1;
    RefreshDriveCache(); // labels once per entry, not per keystroke
    PositionFileChip();
    g_changingEdit = true;
    SetWindowTextW(g_hwndEdit, L"");
    SendMessageW(g_hwndEdit, EM_SETSEL, 0, 0);
    g_changingEdit = false;
    UpdateResults();
}

static void DeactivateFileMode() {
    if (!g_fileActive) return; // don't hide an armed bang chip
    g_fileActive = false;
    g_fileDir.clear();
    g_navigating = false;
    g_bangChipW = 0;
    if (g_hwndChip) ShowWindow(g_hwndChip, SW_HIDE);
    if (g_hwndEdit) {
        ApplyEditMargins();
        SendMessageW(g_hwndEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
    }
}

// File-mode list: header carries "<path> - N items" (count + names only),
// rows carry names only. Current path alone is scanned; filter narrows it.
// Backspace goes up (empty box or a mirrored name); there is no ".." row.
static void UpdateFileResults(const std::wstring& filter) {
    g_results.clear();
    g_results.reserve(9);
    std::wstring fl = FileLower(filter);
    if (g_fileDir.empty()) {
        if (g_drives.empty()) RefreshDriveCache(); // safety: enumeration ready
        const size_t totalDrives = g_drives.size();
        // Keep the lowercased title alongside each drive: the filter and the
        // rank comparator both need it, and FileLower(title) inside those
        // loops allocated a string per drive per comparison.
        std::vector<DriveInfo> shown = g_drives; // copy: cache order untouched
        std::vector<std::wstring> shownLow;
        shownLow.reserve(shown.size());
        for (const auto& d : shown) shownLow.push_back(FileLower(d.title));
        size_t keep = 0;
        for (size_t i = 0; i < shown.size(); i++) {
            if (FileMatchesFilter(shownLow[i], fl)) {
                if (keep != i) {
                    shown[keep] = shown[i];
                    shownLow[keep] = shownLow[i];
                }
                keep++;
            }
        }
        shown.resize(keep);
        shownLow.resize(keep);
        // Empty filter: Explorer letter order (enumeration order, untouched).
        // Filtered: best fuzzy match first ("g" surfaces G:\ ahead of the
        // rest), ties keep letter order.
        SortDrivesByRank(shown, shownLow, fl);
        std::vector<ResultItem> rows;
        for (const auto& d : shown) {
            ResultItem r{ResultKind::Folder, d.title, L"", d.root, kFolderCol};
            rows.push_back(std::move(r));
        }
        wchar_t head[64];
        if (filter.empty())
            swprintf_s(head, 64, L"Drives - %u %s", (unsigned)totalDrives,
                       totalDrives == 1 ? L"drive" : L"drives");
        else
            swprintf_s(head, 64, L"Drives - %u of %u", (unsigned)rows.size(),
                       (unsigned)totalDrives);
        ResultItem hh{ResultKind::Header, head, L"", L"", kHintCol};
        g_results.push_back(std::move(hh));
        // Matches go in uncapped here (drives are few): the window stays
        // compact and the list scrolls internally with no scrollbar.
        for (size_t i = 0; i < rows.size(); i++) g_results.push_back(std::move(rows[i]));
        RebuildListControl();
        return;
    }
    std::vector<FileEntry> entries;
    size_t total = 0;
    bool truncated = false;
    if (!ListDirOneLevel(g_fileDir, entries, total, truncated)) {
        std::wstring head = g_fileDir + L" - Access denied";
        ResultItem hh{ResultKind::Header, head, L"", L"", kHintCol};
        g_results.push_back(std::move(hh));
        RebuildListControl();
        return;
    }
    std::vector<FileEntry> kept;
    kept.reserve(entries.size());
    // Lowercase each name exactly once. The previous version lowercased inside
    // the filter loop AND inside the sort comparator, so ranking a 1000-entry
    // folder allocated on the order of n log n throwaway strings.
    std::vector<std::wstring> klow;
    klow.reserve(entries.size());
    for (const auto& e : entries) {
        std::wstring low = FileLower(e.name);
        if (FileMatchesFilter(low, fl)) {
            kept.push_back(e);
            klow.push_back(std::move(low));
        }
    }
    // Rank + cap: never fully sort more than kMaxFileList entries per
    // keystroke, so huge folders (System32, etc.) can't stall typing.
    // Folders first, then best fuzzy match first, then Explorer order.
    // Sorted as an index permutation so the comparator can stay allocation-free.
    std::vector<size_t> order(kept.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    auto fileLessIdx = [&](size_t x, size_t y) {
        const FileEntry& a = kept[x];
        const FileEntry& b = kept[y];
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return RankedNameLess(klow[x], klow[y], fl, a.name, b.name);
    };
    bool listCapped = kept.size() > kMaxFileList;
    if (listCapped)
        std::partial_sort(order.begin(), order.begin() + (std::ptrdiff_t)kMaxFileList,
                          order.end(), fileLessIdx);
    else
        std::sort(order.begin(), order.end(), fileLessIdx);
    // Materialize only the capped prefix, in rank order.
    size_t want = kept.size() < kMaxFileList ? kept.size() : kMaxFileList;
    std::vector<FileEntry> ranked;
    ranked.reserve(want);
    for (size_t i = 0; i < want; i++) ranked.push_back(std::move(kept[order[i]]));
    kept.swap(ranked);
    size_t shown = kept.size();
    std::wstring head;
    if (truncated) {
        wchar_t b[512];
        swprintf_s(b, 512, L"%s - %u+ items", g_fileDir.c_str(), (unsigned)total);
        head = b;
    } else if (filter.empty()) {
        head = g_fileDir + L" - " + FormatFileCount(total);
    } else {
        wchar_t b[512];
        swprintf_s(b, 512, L"%s - %u of %u", g_fileDir.c_str(),
                   (unsigned)kept.size(), (unsigned)total);
        head = b;
    }
    if (listCapped) {
        wchar_t b[64];
        swprintf_s(b, 64, L", showing %u", (unsigned)shown);
        head += b;
    }
    ResultItem hh{ResultKind::Header, head, L"", L"", kHintCol};
    g_results.push_back(std::move(hh));
    // No ".." Up entry: Backspace goes up (empty filter or mirrored name).
    // Matches go in (capped): the window stays 9 rows tall and the list
    // scrolls internally (no scrollbar, arrows + wheel work). The header
    // always reports the true count, so narrowing the filter reveals the rest.
    for (size_t i = 0; i < shown; i++) {
        ResultItem r;
        r.kind = kept[i].isDir ? ResultKind::Folder : ResultKind::File;
        r.title = kept[i].name;
        r.sub = L"";
        r.action = JoinFilePath(g_fileDir, kept[i].name);
        r.color = kept[i].isDir ? kFolderCol : kFileCol;
        g_results.push_back(std::move(r));
    }
    RebuildListControl();
}

// Enter/Space in file mode: folders drill (stay open, rescan new path only),
// files open directly. Headers / empty matches stay open (never web-search).
// Shift+Enter always opens the current folder in Explorer (drive level:
// the selected drive, if any).
static void ExecuteFileSelected(bool inExplorer) {
    int idx = EffectiveSelection(CurrentSelection(), g_results);
    if (inExplorer) {
        // Always the opened folder, whatever is selected. At drive level
        // there is none: open the selected drive instead, if any.
        if (!g_fileDir.empty()) {
            std::wstring dir = g_fileDir; // copy: HidePalette clears state
            HidePalette(); // hide first: never wait for Explorer
            if ((UINT_PTR)ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL,
                                        SW_SHOWNORMAL) <= 32)
                NotifyLaunchFailed(L"Couldn't open this folder");
            return;
        }
        if (idx >= 0) {
            const ResultItem& r = g_results[(size_t)idx];
            if (r.kind == ResultKind::Folder && !r.action.empty()) {
                std::wstring drive = r.action; // copy: HidePalette clears state
                HidePalette(); // hide first: never wait for Explorer
                if ((UINT_PTR)ShellExecuteW(NULL, L"open", drive.c_str(), NULL, NULL,
                                            SW_SHOWNORMAL) <= 32)
                    NotifyLaunchFailed(L"Couldn't open this folder");
            }
        }
        return; // nothing picked: stay open
    }
    if (idx < 0) return; // header or nothing: stay open
    ResultItem r = g_results[(size_t)idx]; // copy: HidePalette clears state
    if (r.kind == ResultKind::Folder) {
        g_fileDir = r.action.empty() ? L"" : EnsureTrailingSlash(r.action);
        if (g_fileDir.empty()) RefreshDriveCache(); // back at drives: re-read labels
        g_navigating = false;
        if (!r.action.empty()) {
            HistoryItem dh; // Up-on-empty can jump straight back here
            dh.kind = ResultKind::Folder;
            dh.action = r.action;
            dh.title = HistoryBaseName(r.action);
            HistoryRecord(dh);
        }
        if (g_hwndEdit) {
            g_changingEdit = true;
            SetWindowTextW(g_hwndEdit, L"");
            SendMessageW(g_hwndEdit, EM_SETSEL, 0, 0);
            g_changingEdit = false;
        }
        UpdateResults(); // scans the new path only
        if (g_hwndEdit) SetFocus(g_hwndEdit);
        return;
    }
    if (r.kind == ResultKind::File) {
        if (r.action.empty()) return;
        std::wstring full = r.action; // copy: HidePalette clears file state
        HistoryItem fh; // Up-on-empty can jump straight back here
        fh.kind = ResultKind::File;
        fh.action = full;
        fh.title = HistoryBaseName(full);
        HistoryRecord(fh);
        HidePalette(); // hide first: never wait for the target
        if ((UINT_PTR)ShellExecuteW(NULL, L"open", full.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Couldn't open this file");
        return;
    }
    if (r.kind == ResultKind::Explorer) {
        if (r.action.empty()) return;
        std::wstring dir = r.action; // copy: HidePalette clears file state
        HidePalette(); // hide first: never wait for Explorer
        if ((UINT_PTR)ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Couldn't open this folder");
        return;
    }
}

// ------------------------------------------------- Up-on-empty history
// One merged recency list across apps, files/folders, and web searches
// (persisted in [History]). Up on an empty box shows it newest-first;
// Enter acts per row kind. Names only, never paths or URLs.
static bool g_historyDirty = false; // set when history changes in-flight

static void HistoryRecord(const HistoryItem& item) {
    if (item.title.empty() && item.action.empty()) return;
    EnsureSettingsLists(); // SaveSettings rewrites every section
    HistoryAdd(g_settings.history, item);
    g_historyDirty = true; // persist on next explicit SaveNow() or at exit
}
// Existence/dir-ness of one history path, with a short-lived cache.
//
// GetFileAttributesW blocks for as long as the filesystem takes, so an offline
// network drive or an ejected volume could stall the UI thread for seconds on
// every Up-press (up to 20 stats). Cached entries are only trusted briefly:
// a long TTL would let a file opened then deleted keep showing, a zero TTL
// would defeat the point. Hosts that need a tighter guarantee can clear the
// cache from a file watcher; today the short TTL plus the live re-check on
// Enter is the trade-off.
struct HistPathState {
    DWORD attrs = INVALID_FILE_ATTRIBUTES;
    ULONGLONG checkedAt = 0;
};
static DWORD ProbeHistoryPath(const std::wstring& path) {
    static const unsigned kTtlSeconds = 2;
    static std::unordered_map<std::wstring, HistPathState> memo;
    const ULONGLONG now = GetTickCount64() / 1000; // seconds, monotonic
    auto it = memo.find(path);
    if (it != memo.end() && now - it->second.checkedAt < kTtlSeconds)
        return it->second.attrs;
    DWORD a = GetFileAttributesW(path.c_str());
    memo[path] = HistPathState{a, now};
    return a;
}

// Up on an empty box: replace results with the merged recency list.
// Returns false when there is nothing to show (caller falls through to
// normal navigation). Vanished files are hidden, never shown dead.
static bool ShowHistory() {
    EnsureSettingsLists();
    if (g_settings.history.empty()) return false;
    g_results.clear();
    ResultItem hh{ResultKind::Header, L"Recent", L"", L"", kHintCol};
    g_results.push_back(std::move(hh));
    for (const auto& e : g_settings.history) {
        if (e.kind != ResultKind::App && e.kind != ResultKind::Web &&
            e.kind != ResultKind::File && e.kind != ResultKind::Folder)
            continue;
        ResultKind kind = e.kind;
        if (kind == ResultKind::File || kind == ResultKind::Folder) {
            // Resolve dir-ness live (covers legacy bare lines + FS changes);
            // missing paths never show.
            DWORD a = ProbeHistoryPath(e.action);
            if (a == INVALID_FILE_ATTRIBUTES) continue;
            kind = (a & FILE_ATTRIBUTE_DIRECTORY) ? ResultKind::Folder
                                                  : ResultKind::File;
        }
        ResultItem r;
        r.kind = kind;
        r.title = e.title; // short name only
        r.sub = L"";
        r.action = e.action;
        r.isStore = e.isStore;
        r.color = (kind == ResultKind::App) ? kAppCol
                : (kind == ResultKind::Web) ? kWebCol
                : (kind == ResultKind::Folder) ? kFolderCol : kFileCol;
        g_results.push_back(std::move(r));
    }
    if (g_results.size() <= 1) return false; // everything vanished
    g_histActive = true;
    g_navigating = false;
    g_chipsFocus = false;
    g_gridTopRow = 0; // history always starts at the top
    RebuildListControl();
    return true;
}

// ------------------------------------------------------- file grid metrics
// Grid is file-mode-only and setting-gated (rows/columns from Settings).
// Tiles map 1:1 onto g_results indices (after one header).
static bool GridActive() { return InFileMode() && g_settings.fileGrid; }
static int GridRows() { return ClampGridRows(g_settings.gridRows); }
static int GridColumns() { return ClampGridCols(g_settings.gridCols); }
// Effective columns: the setting is a maximum; fewer columns are used when
// tiles would get too narrow for their labels. All tile math (paint,
// hit-test, nav, content height) uses this so labels never clip mid-glyph.
static int GridColsEff() {
    return EffectiveGridCols(kWidth - kPadX * 2, GridColumns(), Dpx(110));
}

// ------------------------------------------------- hover preview helpers
// Hover highlights a row/tile visually only: selection, drag, and Enter
// targets are never disturbed by the mouse position.
static void TrackHoverLeave(HWND h) {
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = h;
    TrackMouseEvent(&tme);
}
// List-box point -> results index, or -1 (shared by hover tracking).
// Items are appended in g_results order, so the listbox index IS the results
// index and no LB_GETITEMDATA lookup is needed.
static int ListRowFromPoint(HWND h, LPARAM l) {
    int idx = (int)SendMessageW(h, LB_ITEMFROMPOINT, 0, l);
    if (HIWORD(idx) != 0) return -1;
    int row = LOWORD(idx);
    if (row < 0 || row >= (int)g_results.size()) return -1;
    return row;
}
static int GridContentH() {
    bool head = !g_results.empty() && g_results.front().kind == ResultKind::Header;
    size_t base = head ? 1 : 0;
    size_t tiles = g_results.size() > base ? g_results.size() - base : 0;
    int cols = GridColsEff();
    int gr = GridRows();
    int need = tiles ? (int)((tiles + (size_t)cols - 1) / (size_t)cols) : 0;
    int vis = need < gr ? need : gr;
    return (head ? kGridHeadH : 0) + vis * kGridTileH;
}

// ------------------------------------------------------------- power actions
// Absolute system paths only: never resolve power tools from CWD or PATH.
static std::wstring SystemExe(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    if (!GetSystemDirectoryW(dir, MAX_PATH)) return L"";
    return std::wstring(dir) + L"\\" + name;
}
static void RunPower(PowerOp op) {
    switch (op) {
    case PowerOp::Shutdown: {
        std::wstring exe = SystemExe(L"shutdown.exe");
        // /f: force-close running apps first (unsaved work is lost) so
        // nothing can veto the shutdown. Double-Enter confirm guards this.
        if (!exe.empty() &&
            (UINT_PTR)ShellExecuteW(NULL, L"open", exe.c_str(), L"/s /f /t 0", NULL, SW_HIDE) <= 32)
            NotifyLaunchFailed(L"Shutdown command failed");
        break;
    }
    case PowerOp::Restart: {
        std::wstring exe = SystemExe(L"shutdown.exe");
        // /f: same force-close behavior as shutdown (unsaved work is lost).
        if (!exe.empty() &&
            (UINT_PTR)ShellExecuteW(NULL, L"open", exe.c_str(), L"/r /f /t 0", NULL, SW_HIDE) <= 32)
            NotifyLaunchFailed(L"Restart command failed");
        break;
    }
    case PowerOp::Hibernate: {
        std::wstring exe = SystemExe(L"shutdown.exe");
        if (!exe.empty() &&
            (UINT_PTR)ShellExecuteW(NULL, L"open", exe.c_str(), L"/h", NULL, SW_HIDE) <= 32)
            NotifyLaunchFailed(L"Hibernate command failed");
        break;
    }
    case PowerOp::Sleep:
        if (!SetSuspendState(FALSE, FALSE, FALSE))
            NotifyLaunchFailed(L"Sleep failed");
        break;
    }
}

// Instant Screen Stays Awake: hold the machine + display awake without
// touching permanent power settings. ES_CONTINUOUS persists until changed;
// Windows auto-clears it if the process ever exits (no stuck-awake risk).
static void ApplyAwake() {
    SetThreadExecutionState(g_awakeOn
        ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)
        : ES_CONTINUOUS);
}
static ResultItem MakeAppRow(const AppEntry& app) {
    ResultItem r;
    r.kind = ResultKind::App;
    r.title = app.name;      // name only - never the file path
    r.sub = L"";
    r.action = app.target;
    r.color = kAppCol;
    r.isStore = app.isStore;
    return r;
}
static void PushAppRow(const AppEntry& app) {
    g_results.push_back(MakeAppRow(app));
}

// Case-insensitive "starts with" test against a literal or a stored name.
// BuildChips runs on every keystroke and used to allocate a temporary
// lowercase std::wstring per alias (and per quick link) just to compare a
// short prefix.
static bool PrefixMatchFold(const wchar_t* name, const std::wstring& qlower) {
    size_t n = qlower.size();
    size_t len = 0;
    while (name[len]) len++;
    return len >= n && _wcsnicmp(name, qlower.c_str(), n) == 0;
}

// Prefix-matched power + bang + quick-link chips (separate row above list).
static void BuildChips(const std::wstring& qlower) {
    g_chips.clear();
    if (qlower.size() < 2 || qlower[0] == L'!') return;
    for (int i = 0; i < 4 && (int)g_chips.size() < kMaxChips; i++) {
        for (int a = 0; a < 4 && kPowers[i].aliases[a]; a++) {
            if (PrefixMatchFold(kPowers[i].aliases[a], qlower)) {
                std::wstring pt = kPowers[i].title;
                if (g_confirmPower == i) pt += L"?"; // armed: second Enter runs it
                g_chips.push_back({ChipKind::Power, pt, kPowers[i].color, i});
                break;
            }
        }
    }
    static const wchar_t* awakeAliases[] =
        {L"awake", L"stay awake", L"stayawake", L"screen on", L"keep awake"};
    for (int a = 0; a < 5 && (int)g_chips.size() < kMaxChips; a++) {
        if (PrefixMatchFold(awakeAliases[a], qlower)) {
            g_chips.push_back({ChipKind::Awake,
                               g_awakeOn ? L"Awake: On" : L"Awake: Off",
                               kAwakeCol, 0});
            break;
        }
    }
    for (size_t i = 0; i < g_settings.bangs.size() && (int)g_chips.size() < kMaxChips; i++) {
        const Bang& b = g_settings.bangs[i];
        // Bang aliases are stored lowercased already: no fold needed here.
        for (const auto& a : b.aliases) {
            if (!a.empty() && a.size() >= qlower.size() &&
                a.compare(0, qlower.size(), qlower) == 0) {
                g_chips.push_back({ChipKind::Bang, b.name, b.color, (int)i});
                break;
            }
        }
    }
    for (size_t i = 0; i < g_settings.quicks.size() && (int)g_chips.size() < kMaxChips; i++) {
        if (PrefixMatchFold(g_settings.quicks[i].name.c_str(), qlower))
            g_chips.push_back({ChipKind::Quick, g_settings.quicks[i].name, kQuickCol, (int)i});
    }
}

// Content height for the band starting at `top` (grid tiles keep their own
// uniform metric). Heights come from RowH(), which is exactly what our
// WM_MEASUREITEM handler reports for the listbox, so the last row always fits
// at any DPI and scrolled or not — with no LB_GETITEMHEIGHT round-trip per
// row (each of those dispatches WM_MEASUREITEM back to us).
static int ContentHForTop(int top) {
    if (GridActive()) return GridContentH();
    int count = (int)g_results.size();
    int show = count > kMaxItems ? kMaxItems : count;
    if (top < 0) top = 0;
    int hgt = 0;
    for (int k = 0; k < show && top + k < count; k++)
        hgt += RowH((size_t)(top + k));
    return hgt;
}
// Resize the palette window (round region + chip row follow) to fit content.
// Only acts when the height actually changed: no per-keystroke blink.
static void FitPaletteWindow(int inputH, int listY, int contentH, int barH,
                             bool showChips) {
    // Bottom pad generously: the last row's descenders (e.g. "g") must clear
    // the rounded window edge at every DPI, never sit on the boundary.
    int h = listY + contentH + barH + Dpx(8);
    RECT wr; GetWindowRect(g_hwndPalette, &wr);
    if (h == wr.bottom - wr.top) return;
    SetWindowPos(g_hwndPalette, HWND_TOPMOST, wr.left, wr.top, kWidth, h,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    ApplyRoundCorners();
    RECT cr; GetClientRect(g_hwndPalette, &cr);
    if (showChips)
        SetWindowPos(g_hwndChipRow, NULL, kPadX, inputH, cr.right - kPadX * 2, kChipRowH,
                     SWP_NOZORDER | SWP_SHOWWINDOW);
    else
        ShowWindow(g_hwndChipRow, SW_HIDE);
}
// After a programmatic scroll the visible band starts at a different row
// (headers are slim, so bands differ in height): refit window + children so
// the new band never overflows its frame.
static void RefitListHeight() {
    if (GridActive() || !g_hwndList || !IsWindowVisible(g_hwndList)) return;
    if (!g_hwndPalette) return;
    int top = (int)SendMessageW(g_hwndList, LB_GETTOPINDEX, 0, 0);
    if (top < 0) top = 0;
    int inputH = CurrentInputH();
    bool showChips = !g_chips.empty();
    int listY = inputH + (showChips ? kChipRowH : 0);
    int contentH = ContentHForTop(top);
    bool showBar = InFileMode() && !g_fileDir.empty();
    int barH = showBar ? kBottomBarH : 0;
    FitPaletteWindow(inputH, listY, contentH, barH, showChips);
    RECT cr; GetClientRect(g_hwndPalette, &cr);
    SetWindowPos(g_hwndList, NULL, kPadX, listY, cr.right - kPadX * 2,
                 contentH, SWP_NOZORDER | SWP_SHOWWINDOW);
    if (showBar && g_hwndFileBar)
        SetWindowPos(g_hwndFileBar, NULL, kPadX, listY + contentH,
                     cr.right - kPadX * 2, barH, SWP_NOZORDER | SWP_SHOWWINDOW);
}

// Move a child only when its rectangle actually changes. RebuildListControl
// runs on every keystroke, and an unconditional SetWindowPos (even with
// identical geometry) still costs WM_WINDOWPOSCHANGING/CHANGED plus redraw
// scheduling on any overlapping sibling.
static void PlaceChild(HWND h, int x, int y, int w, int hh, bool show) {
    if (!h) return;
    if (!show) { ShowWindow(h, SW_HIDE); return; }
    // Compare the window rect (not the client rect) against the target so this
    // stays correct for any border style, then map to parent coordinates.
    RECT pr;
    GetWindowRect(h, &pr);
    MapWindowPoints(NULL, GetParent(h), (POINT*)&pr, 2);
    bool same = (pr.left == x && pr.top == y &&
                 pr.right - pr.left == w && pr.bottom - pr.top == hh);
    if (!same) {
        SetWindowPos(h, NULL, x, y, w, hh, SWP_NOZORDER | SWP_SHOWWINDOW);
    } else if (!IsWindowVisible(h)) {
        ShowWindow(h, SW_SHOW);
    }
}

static void RebuildListControl() {
    SendMessageW(g_hwndList, LB_RESETCONTENT, 0, 0);
    // Items are appended in g_results order, so listbox index == results index.
    // No LB_SETITEMDATA: storing i was redundant, and in a 1000-row folder it
    // doubled the per-keystroke message count on this rebuild.
    for (size_t i = 0; i < g_results.size(); i++)
        SendMessageW(g_hwndList, LB_ADDSTRING, 0, (LPARAM)L"");
    // Chips own the selection when they have focus; otherwise the first
    // selectable row (headers never take Enter).
    if (g_chipsFocus && !g_chips.empty())
        SendMessageW(g_hwndList, LB_SETCURSEL, (WPARAM)-1, 0);
    else {
        int first = FirstSelectable(g_results);
        SendMessageW(g_hwndList, LB_SETCURSEL, first, 0);
        EnsureVisible(first);
    }

    bool showChips = !g_chips.empty();
    bool showBar = InFileMode() && !g_fileDir.empty();
    bool gridOn = GridActive();
    size_t rows = g_results.size() > kMaxItems ? kMaxItems : g_results.size();
    int inputH = CurrentInputH();
    int listY = inputH + (showChips ? kChipRowH : 0);
    // Measured heights from the live top index (EnsureVisible above may have
    // scrolled a slim header out): the band always fits, scrolled or not.
    g_hoverRow = -1; // fresh results, fresh hover
    int top = (int)SendMessageW(g_hwndList, LB_GETTOPINDEX, 0, 0);
    int contentH = ContentHForTop(top);
    int barH = showBar ? kBottomBarH : 0;
    FitPaletteWindow(inputH, listY, contentH, barH, showChips);
    // List/grid visibility + geometry, every refresh (cheap, idempotent).
    {
        RECT cr; GetClientRect(g_hwndPalette, &cr);
        // The edit itself must track wrapped lines every refresh: word wrap
        // only becomes visible when the control is tall enough to show it.
        // Sized only on change (per-keystroke moves would flicker the caret).
        RECT er;
        GetWindowRect(g_hwndEdit, &er);
        if (er.bottom - er.top != inputH - Dpx(14))
            SetWindowPos(g_hwndEdit, NULL, kPadX, Dpx(9), cr.right - kPadX * 2,
                         inputH - Dpx(14), SWP_NOZORDER);
        if (gridOn) {
            if (IsWindowVisible(g_hwndList)) ShowWindow(g_hwndList, SW_HIDE);
            PlaceChild(g_hwndFileGrid, kPadX, listY, cr.right - kPadX * 2, contentH, true);
        } else {
            if (IsWindowVisible(g_hwndFileGrid)) ShowWindow(g_hwndFileGrid, SW_HIDE);
            PlaceChild(g_hwndList, kPadX, listY, cr.right - kPadX * 2, contentH, rows != 0);
        }
    }
    // Explorer bar docks under the content (positioned every refresh: cheap).
    {
        RECT cr; GetClientRect(g_hwndPalette, &cr);
        PlaceChild(g_hwndFileBar, kPadX, listY + contentH,
                   cr.right - kPadX * 2, barH, showBar);
    }
    // No background erase (controls paint every pixel themselves).
    if (gridOn) InvalidateRect(g_hwndFileGrid, NULL, FALSE);
    else InvalidateRect(g_hwndList, NULL, FALSE);
    if (showChips) InvalidateRect(g_hwndChipRow, NULL, FALSE);
    if (showBar) InvalidateRect(g_hwndFileBar, NULL, FALSE);
    // No scrollbar by construction (WS_VSCROLL removed): longer file lists
    // scroll via EnsureVisible on arrows + wheel. Vertical only.
}

// Domain-shaped input lives in bangs.h now (shared with the unit tests).

// Best system-settings pages for the query (name + keywords), capped.
static void MatchSysPages(const std::wstring& qlower, std::vector<ResultItem>& out,
                          size_t limit) {
    struct Hit { const SysPage* p; int score; };
    if (limit == 0) return;
    const std::vector<SysPageLower>& low = SysPagesLower();
    std::vector<Hit> hits;
    for (int i = 0; i < SysPageCount(); i++) {
        if ((size_t)i >= low.size()) break; // table/lowercase-list mismatch
        const SysPageLower& lp = low[(size_t)i];
        int s1 = FuzzyScore(lp.name, qlower);
        int s2 = FuzzyScore(lp.keywords, qlower);
        int s = s1 > s2 ? s1 : s2;
        if (s >= 0) hits.push_back({&kSysPages[i], s});
    }
    // Only the best `limit` rows are ever read, so sort just that prefix
    // instead of the whole match list.
    if (limit && hits.size() > limit) {
        std::partial_sort(hits.begin(), hits.begin() + (std::ptrdiff_t)limit,
                          hits.end(),
                          [](const Hit& a, const Hit& b) { return a.score > b.score; });
        hits.resize(limit);
    }
    for (size_t i = 0; i < hits.size() && out.size() < limit; i++) {
        ResultItem r{ResultKind::Setting, hits[i].p->name, L"",
                     hits[i].p->uri, kSetCol};
        out.push_back(std::move(r));
    }
}

static void UpdateResults() {
    // Any real refilter leaves history view (typing exits back to normal).
    g_histActive = false;
    g_results.clear();
    g_results.reserve(8);
    std::wstring raw = GetEditText(g_hwndEdit);
    std::wstring q = Trim(raw);

    // --- file-browse mode: drives / single-level dir only, no web fallback.
    if (InFileMode()) {
        g_chips.clear();
        g_chipsFocus = false;
        g_gridTopRow = 0; // fresh results start at the top
        UpdateFileResults(q);
        return;
    }

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

    // One lowercase copy per keystroke, shared by chips, app search and the
    // settings pages (each used to make its own copy). `q` keeps its original
    // case: URLs and bang terms must not be case-folded.
    std::wstring qlower = BangLower(q);

    // --- chips row (power + quick links), selected by default
    BuildChips(qlower);
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
            // term comes from ParseBangLeading on the raw query, so it still
            // needs its own lowercase copy for SearchAppsLower.
            for (auto& h : SearchAppsLower(g_apps, BangLower(term), 6)) {
                if (g_hidden.count(h.app->lower) == 0) PushAppRow(*h.app);
            }
        }
        RebuildListControl();
        return;
    }

    // --- normal: sectioned results (Calculator / Apps / Settings / Web)
    std::vector<ResultItem> calcRows;
    calcRows.swap(g_results); // whatever the calculator produced above

    std::vector<ResultItem> appRows;
    if (g_indexReady.load()) {
        for (auto& h : SearchAppsLower(g_apps, qlower, 4)) {
            if (g_hidden.count(h.app->lower) == 0) appRows.push_back(MakeAppRow(*h.app));
        }
    } else {
        // Loading indicator: a header, never selectable, so Enter during
        // indexing falls through to web search instead of swallowing input.
        ResultItem r{ResultKind::Header, L"Indexing apps...", L"",
                     L"", kHintCol};
        appRows.push_back(std::move(r));
    }

    std::vector<ResultItem> setRows;
    MatchSysPages(qlower, setRows, 3);

    std::vector<ResultItem> webRows;
    std::wstring directUrl; // URL-shaped input: direct open only, no dupe search row
    bool isUrl = IsUrlLikeLower(q, qlower, directUrl);
    if (isUrl) g_chipsFocus = false; // Enter opens the URL, chips stay on Tab
    if (isUrl) {
        ResultItem r{ResultKind::Web, q, L"", directUrl, kWebCol};
        webRows.push_back(std::move(r));
    } else {
        ResultItem web{ResultKind::Web, q, L"",
                       ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q)), kWebCol};
        webRows.push_back(std::move(web));
    }

    auto pushSection = [&](const wchar_t* h, std::vector<ResultItem>& rows) {
        if (rows.empty()) return;
        size_t room = g_results.size() >= 9 ? 0 : 9 - g_results.size();
        if (room < 2) return; // header + at least one row, else skip the section
        size_t take = rows.size() < room - 1 ? rows.size() : room - 1;
        ResultItem hh{ResultKind::Header, h, L"", L"", kHintCol};
        g_results.push_back(std::move(hh));
        for (size_t i = 0; i < take; i++) g_results.push_back(std::move(rows[i]));
    };
    pushSection(L"Calculator", calcRows);
    pushSection(L"Apps", appRows);
    pushSection(L"Settings", setRows);
    pushSection(isUrl ? L"URL" : L"Web", webRows);

    RebuildListControl();
}

static int CurrentSelection() {
    int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
    if (sel < 0) return -1; // nothing selected: caller runs the typed-query fallback
    // Listbox index == results index (items appended in g_results order).
    if (sel >= (int)g_results.size()) return -1;
    return sel;
}

static void ExecuteChip(int idx) {
    if (idx < 0 || idx >= (int)g_chips.size()) return;
    ChipItem c = g_chips[(size_t)idx]; // copy: HidePalette clears the vector
    if (c.kind != ChipKind::Power) g_confirmPower = -1; // only power confirms
    if (c.kind == ChipKind::Awake) {
        g_awakeOn = !g_awakeOn;
        ApplyAwake();
        g_settings.awakeOn = g_awakeOn;
        EnsureSettingsLists(); // palette shown => loaded, but guard pre-save anyway
        SaveOrWarn(L"keep awake");
        UpdateResults(); // refresh the chip title, stay open
        return;
    }
    if (c.kind == ChipKind::Power) {
        int nPowers = (int)(sizeof(kPowers) / sizeof(kPowers[0]));
        if (c.data < 0 || c.data >= nPowers) return;
        PowerOp op = kPowers[c.data].op;
        // Shutdown/restart need a deliberate second Enter; sleep/hibernate
        // wake with any keypress, so they stay instant.
        bool needsConfirm = (op == PowerOp::Shutdown || op == PowerOp::Restart);
        if (needsConfirm && g_confirmPower != (int)op) {
            g_confirmPower = (int)op; // arm: second Enter executes
            UpdateResults(); // repaint the chip as a question, stay open
            return;
        }
        g_confirmPower = -1;
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
    if ((UINT_PTR)OpenUrl(url, g_settings.browserPath) <= 32)
        NotifyLaunchFailed(L"Browser launch failed");
}

// Launch an app row (Store apps via explorer, classic via ShellExecute).
// Callers hide the palette first: never wait for the target app.
static void LaunchAppRow(const ResultItem& r) {
    if (r.action.empty()) { NotifyLaunchFailed(L"Couldn't open this app"); return; }
    if (r.isStore) {
        wchar_t winDir[MAX_PATH];
        HINSTANCE ok = NULL;
        if (GetWindowsDirectoryW(winDir, MAX_PATH)) {
            std::wstring exe = std::wstring(winDir) + L"\\explorer.exe";
            ok = ShellExecuteW(NULL, L"open", exe.c_str(), r.action.c_str(), NULL, SW_SHOWNORMAL);
        }
        if ((UINT_PTR)ok <= 32) NotifyLaunchFailed(L"Couldn't open this app");
    } else {
        if ((UINT_PTR)ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Couldn't open this app");
    }
}

static void ExecuteSelected(bool forceWeb) {
    // History recall: Enter acts per row kind (apps launch, files open,
    // folders drill and stay open, searches re-run). Shift is ignored here.
    if (g_histActive) {
        g_histActive = false;
        int hidx = EffectiveSelection(CurrentSelection(), g_results);
        if (hidx < 0) { UpdateResults(); return; }
        ResultItem hr = g_results[(size_t)hidx]; // copy: paths below rebuild
        HistoryItem rec; // re-entered history refreshes recency
        rec.kind = hr.kind; rec.title = hr.title;
        rec.action = hr.action; rec.isStore = hr.isStore;
        if (hr.kind == ResultKind::Folder && !hr.action.empty()) {
            HistoryRecord(rec);
            ActivateFileMode(hr.action); // drills, stays open
            return;
        }
        if (hr.kind == ResultKind::App && !hr.action.empty()) {
            HistoryRecord(rec);
            HidePalette(); // hide first: never wait for the target
            LaunchAppRow(hr);
            return;
        }
        if (hr.kind == ResultKind::Web) {
            if (!hr.action.empty()) {
                HistoryRecord(rec);
                HidePalette(); // hide first: never wait for the browser
                if ((UINT_PTR)OpenUrl(hr.action, g_settings.browserPath) <= 32)
                    NotifyLaunchFailed(L"Browser launch failed");
            } else if (!hr.title.empty()) {
                HistoryRecord(rec);
                std::wstring url = ExpandUrl(g_settings.engineUrl,
                                             UrlEncodeQuery(hr.title));
                HidePalette(); // hide first: never wait for the browser
                if ((UINT_PTR)OpenUrl(url, g_settings.browserPath) <= 32)
                    NotifyLaunchFailed(L"Browser launch failed");
            } else {
                UpdateResults();
            }
            return;
        }
        if (hr.kind == ResultKind::File && !hr.action.empty()) {
            HistoryRecord(rec);
            std::wstring full = hr.action;
            HidePalette(); // hide first: never wait for the target
            if ((UINT_PTR)ShellExecuteW(NULL, L"open", full.c_str(), NULL, NULL,
                                        SW_SHOWNORMAL) <= 32)
                NotifyLaunchFailed(L"Couldn't open this file");
            return;
        }
        UpdateResults();
        return;
    }
    std::wstring q = Trim(GetEditText(g_hwndEdit));
    if (g_navigating) {
        // Arrow-mirrored text is a preview, not input: web search below uses
        // what was actually typed, irrespective of the mirrored suggestion.
        std::wstring typed = Trim(g_origQuery);
        if (!typed.empty()) q = typed;
    }

    auto CopyText = [](const std::wstring& t) { CopyTextToClipboard(t); };

    // Bang-chip mode: the armed site always wins (Shift+Enter = engine instead).
    if (const Bang* b = ActiveBang()) {
        std::wstring url = forceWeb ? ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q))
            : (q.empty() ? BangHome(b->url) : ExpandUrl(b->url, UrlEncodeQuery(q)));
        HidePalette(); // hide first: never wait for the browser
        if ((UINT_PTR)OpenUrl(url, g_settings.browserPath) <= 32)
            NotifyLaunchFailed(L"Browser launch failed");
        return;
    }

    // File-browse mode: folders drill (stay open), files open. Never web-search.
    // Shift+Enter opens the relevant path in Explorer instead.
    if (InFileMode()) {
        ExecuteFileSelected(forceWeb);
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
        HistoryItem wh; // executed searches lead the merged history
        wh.kind = ResultKind::Web; wh.title = q; wh.action = L"";
        HistoryRecord(wh);
        HidePalette(); // hide first: never wait for the browser
        if ((UINT_PTR)OpenUrl(url, g_settings.browserPath) <= 32)
            NotifyLaunchFailed(L"Browser launch failed");
        return;
    }

    int idx = EffectiveSelection(CurrentSelection(), g_results);
    if (idx < 0) {
        // No actionable selection (none, or a section heading): normal
        // fallback runs the typed query as a web search.
        HistoryItem wh; // executed searches lead the merged history
        wh.kind = ResultKind::Web; wh.title = q; wh.action = L"";
        HistoryRecord(wh);
        std::wstring url = ExpandUrl(g_settings.engineUrl, UrlEncodeQuery(q));
        HidePalette();
        if ((UINT_PTR)OpenUrl(url, g_settings.browserPath) <= 32)
            NotifyLaunchFailed(L"Browser launch failed");
        return;
    }
    ResultItem r = g_results[(size_t)idx]; // copy: HidePalette clears the vector
    HidePalette(); // hide first: never wait for the target app
    switch (r.kind) {
    case ResultKind::App: {
        HistoryItem ah; // launched apps lead the merged history
        ah.kind = ResultKind::App; ah.title = r.title;
        ah.action = r.action; ah.isStore = r.isStore;
        HistoryRecord(ah);
        LaunchAppRow(r);
        break;
    }
    case ResultKind::Web:
    case ResultKind::Bang: {
        HistoryItem wh; // searches + direct links, re-runnable from history
        wh.kind = ResultKind::Web; wh.title = r.title; wh.action = r.action;
        HistoryRecord(wh);
        if ((UINT_PTR)OpenUrl(r.action, g_settings.browserPath) <= 32)
            NotifyLaunchFailed(L"Browser launch failed");
        break;
    }
    case ResultKind::Setting:
        if (!r.action.empty() &&
            (UINT_PTR)ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Couldn't open this settings page");
        break;
    case ResultKind::Calc:
        CopyText(r.action);
        break;
    case ResultKind::Folder:
        // Reached only outside file mode (should not happen): open in Explorer.
        if (!r.action.empty() &&
            (UINT_PTR)ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Couldn't open this folder");
        break;
    case ResultKind::File:
        if (!r.action.empty() &&
            (UINT_PTR)ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Couldn't open this file");
        break;
    case ResultKind::Explorer:
        // Reached only outside file mode (should not happen): open in Explorer.
        if (!r.action.empty() &&
            (UINT_PTR)ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Couldn't open this folder");
        break;
    }
}

// ------------------------------------------------- list navigation helpers
// Moving with arrows mirrors the selected row's text into the box WITHOUT
// refiltering: the list stays frozen until the user types or deletes.
static bool IsSelectable(size_t i) {
    return i < g_results.size() && g_results[i].kind != ResultKind::Header;
}
// Same, but no wrapping: -1 past either end (caller restores original text).
static int MoveSelNoWrap(int from, int dir) {
    int n = (int)g_results.size();
    if (n <= 0) return -1;
    int cur = (from < 0) ? (dir > 0 ? -1 : n) : from;
    for (int k = 0; k < n; k++) {
        cur += dir;
        if (cur < 0 || cur >= n) return -1;
        if (IsSelectable((size_t)cur)) return cur;
    }
    return -1;
}
static void EnsureNavStart() {
    if (!g_navigating) {
        g_origQuery = GetEditText(g_hwndEdit);
        g_navigating = true;
    }
}
static void RestoreOrigSel() {
    SendMessageW(g_hwndList, LB_SETCURSEL, (WPARAM)-1, 0);
    if (!g_navigating) return;
    if (GetEditText(g_hwndEdit) != g_origQuery) {
        g_changingEdit = true; // programmatic: no refilter, list stays frozen
        SetWindowTextW(g_hwndEdit, g_origQuery.c_str());
        SendMessageW(g_hwndEdit, EM_SETSEL,
                     (WPARAM)g_origQuery.size(), (LPARAM)g_origQuery.size());
        g_changingEdit = false;
    }
    g_navigating = false; // back at the typed text: fresh arrow cycling
}
static void SyncSelToBox(int listSel) {
    if (listSel < 0 || listSel >= (int)g_results.size()) return;
    const ResultItem& r = g_results[(size_t)listSel];
    std::wstring keep = GetEditText(g_hwndEdit);
    std::wstring t = keep;
    if (r.kind == ResultKind::App || r.kind == ResultKind::Setting)
        t = r.title;
    else if ((r.kind == ResultKind::Folder || r.kind == ResultKind::File) &&
             r.title != L"..")
        t = r.title; // file mode: mirror the name as the filter, list stays frozen
    if (t == keep) return;
    g_changingEdit = true; // programmatic: no refilter, list stays frozen
    SetWindowTextW(g_hwndEdit, t.c_str());
    SendMessageW(g_hwndEdit, EM_SETSEL, (WPARAM)t.size(), (LPARAM)t.size());
    g_changingEdit = false;
}

// No scrollbar anywhere (WS_VSCROLL removed): scrolling is driven by us.
// After any programmatic selection change, pin the view so the row shows.
static void EnsureVisible(int sel) {
    if (sel < 0 || !g_hwndList) return;
    if (GridActive() && g_hwndFileGrid) {
        // Grid rows, not list rows: pin the tile into the visible band.
        size_t base = (!g_results.empty() &&
                       g_results.front().kind == ResultKind::Header) ? 1 : 0;
        if ((size_t)sel < base || (size_t)sel >= g_results.size()) return;
        int cols = GridColsEff();
        if (cols < 1) cols = 1;
        int row = (sel - (int)base) / cols;
        int gr = GridRows();
        if (row < g_gridTopRow) g_gridTopRow = row;
        else if (row >= g_gridTopRow + gr)
            g_gridTopRow = row - gr + 1;
        InvalidateRect(g_hwndFileGrid, NULL, FALSE);
        return;
    }
    int top = (int)SendMessageW(g_hwndList, LB_GETTOPINDEX, 0, 0);
    int vis = VisibleRowCount();
    int newTop = top;
    if (sel < top)
        newTop = sel;
    else if (sel >= top + vis)
        newTop = sel - vis + 1;
    if (newTop != top) {
        SendMessageW(g_hwndList, LB_SETTOPINDEX, newTop, 0);
        // The scrolled band starts at a different row (slim headers shift
        // the math): refit so the new band never overflows its frame.
        RefitListHeight();
    }
}

// Multi-step arrow motion (shared by keys + wheel), no wrapping.
static void MoveSelSteps(int steps) {
    if (steps == 0 || g_results.empty()) return;
    EnsureNavStart();
    int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
    int dir = (steps > 0) ? +1 : -1;
    int to = sel;
    for (int i = 0, n = steps > 0 ? steps : -steps; i < n; i++) {
        int nx = MoveSelNoWrap(to, dir);
        if (nx < 0) break; // past either end: stop, list stays
        to = nx;
    }
    if (to != sel && to >= 0) {
        SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
        SyncSelToBox(to);
        EnsureVisible(to);
    }
    InvalidateRect(g_hwndList, NULL, FALSE);
    if (!g_chips.empty()) InvalidateRect(g_hwndChipRow, NULL, FALSE);
}

// Vertical mouse wheel: moves selection (3 rows per notch) only when the
// list overflows the visible area; otherwise ignored. Horizontal ignored.
static void WheelScrollList(int delta) {
    if (GridActive() && g_hwndFileGrid && IsWindowVisible(g_hwndFileGrid)) {
        // Grid band overflow: a full tile row per notch, ±1 on partials.
        size_t base = (!g_results.empty() &&
                       g_results.front().kind == ResultKind::Header) ? 1 : 0;
        size_t tiles = g_results.size() > base ? g_results.size() - base : 0;
        int cols = GridColsEff();
        if (cols < 1) cols = 1;
        int gr = GridRows();
        int need = tiles ? (int)((tiles + (size_t)cols - 1) / (size_t)cols) : 0;
        if (need <= gr) return; // nothing to scroll
        int notches = delta / WHEEL_DELTA;
        int move = -notches * cols; // wheel up = toward first row
        if (move == 0) move = (delta > 0) ? -1 : 1;
        MoveSelSteps(move);
        return;
    }
    if (!g_hwndList || !IsWindowVisible(g_hwndList)) return;
    int vis = VisibleRowCount();
    if ((int)g_results.size() <= vis) return; // nothing to scroll
    int notches = delta / WHEEL_DELTA;
    int move = -notches * 3; // wheel up = toward first row
    if (move == 0) move = (delta > 0) ? -1 : 1; // partial notch
    MoveSelSteps(move);
}

static LRESULT CALLBACK ListSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                     UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEWHEEL) {
        WheelScrollList(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    // Drag-out arming: default click selection first, then track moves past
    // the system drag threshold (files, folders, drives, classic app targets).
    if (m == WM_LBUTTONDOWN) {
        LRESULT r = DefSubclassProc(h, m, w, l);
        int idx = (int)SendMessageW(h, LB_ITEMFROMPOINT, 0, l);
        if (HIWORD(idx) == 0) {
            int row = LOWORD(idx); // listbox index == results index
            if (row < 0 || row >= (int)g_results.size()) row = -1;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ArmRowDrag(row, pt);
        } else {
            CancelRowDrag();
        }
        return r;
    }
    if (m == WM_MOUSEMOVE) {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        MoveRowDrag(h, pt);
        // Hover preview only: repaints the hot row, never moves selection
        // or disturbs the multi-set.
        int hov = ListRowFromPoint(h, l);
        if (hov != g_hoverRow) {
            int old = g_hoverRow;
            g_hoverRow = hov;
            auto invalidateRow = [&](int row) {
                if (row < 0 || !h) return;
                RECT r{};
                if (SendMessageW(h, LB_GETITEMRECT, row, (LPARAM)&r) != LB_ERR)
                    InvalidateRect(h, &r, FALSE);
            };
            if (hov >= 0) TrackHoverLeave(h);
            invalidateRow(old);
            invalidateRow(hov);
        }
        return DefSubclassProc(h, m, w, l);
    }
    if (m == WM_MOUSELEAVE) {
        if (g_hoverRow >= 0) {
            g_hoverRow = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    if (m == WM_LBUTTONUP) {
        CancelRowDrag();
    }
    return DefSubclassProc(h, m, w, l);
}

// ------------------------------------------------------------- edit subclass
// WM_CHAR swallowing kills the system beep: a single-line EDIT beeps for
// Enter/Esc/Tab chars it cannot insert.
static LRESULT CALLBACK EditSubclass(HWND h, UINT m, WPARAM w, LPARAM l,
                                    UINT_PTR, DWORD_PTR) {
    if (m == WM_CHAR) {
        if (w == 13 || w == 10 || w == 27 || w == 9) return 0;
        // File mode: Space confirms (drills/opens), so the trailing WM_CHAR
        // must not insert a literal space into the cleared box — that phantom
        // space is what used to cost an extra Backspace to go up.
        if (w == 32 && InFileMode()) return 0;
    }
    if (m == WM_MOUSEWHEEL) {
        WheelScrollList(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    if (m == WM_PASTE) {
        // Wrapping is display-only for one logical query: collapse pasted
        // line breaks to spaces so a multiline paste can't hide text or
        // force extra wrapped lines.
        if (!OpenClipboard(h)) return 0;
        HANDLE hd = GetClipboardData(CF_UNICODETEXT);
        std::wstring t;
        if (hd) {
            const wchar_t* p = (const wchar_t*)GlobalLock(hd);
            if (p) t = p;
            GlobalUnlock(hd);
        }
        CloseClipboard();
        for (auto& c : t)
            if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        // Collapse runs of spaces (keeps the box to one logical line).
        std::wstring flat;
        flat.reserve(t.size());
        bool sp = true; // skip leading spaces
        for (wchar_t c : t) {
            if (c == L' ') { if (!sp) { flat.push_back(c); sp = true; } }
            else { flat.push_back(c); sp = false; }
        }
        if (!flat.empty() && flat.back() == L' ') flat.pop_back();
        if (!flat.empty())
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)flat.c_str());
        return 0;
    }
    if (m == WM_KEYDOWN) {
        int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
        int count = (int)SendMessageW(g_hwndList, LB_GETCOUNT, 0, 0);
        bool haveChips = !g_chips.empty();
        switch (w) {
        case VK_ESCAPE: HidePalette(); return 0;
        case VK_APPS:
            KeyboardContextMenu(); // row/file menu, not the edit's own menu
            return 0;
        case VK_F10:
            // Shift+F10 is the Menu-key equivalent: same row menu.
            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                KeyboardContextMenu();
                return 0;
            }
            break;
        case VK_RETURN: {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            if (g_chipsFocus && haveChips && !shift) ExecuteChip(g_chipSel);
            else ExecuteSelected(shift);
            return 0;
        }
        case VK_TAB: {
            bool back = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            g_confirmPower = -1; // navigating disarms a pending confirm
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
        case VK_SPACE:
            // File mode: Space opens the top match like Enter (no separate
            // confirm keypress). Filters stay single-token by design.
            if (InFileMode()) {
                ExecuteSelected(false);
                return 0;
            }
            break;
        case VK_DOWN:
            g_confirmPower = -1; // navigating disarms a pending confirm
            if (g_chipsFocus && haveChips) {
                g_chipsFocus = false;
                EnsureNavStart();
                int to = MoveSelNoWrap(-1, +1);
                SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                if (to >= 0) { SyncSelToBox(to); EnsureVisible(to); }
                InvalidateRect(g_hwndChipRow, NULL, FALSE);
                InvalidateRect(g_hwndList, NULL, FALSE);
            } else if (count > 0) {
                EnsureNavStart();
                if (InFileMode() || g_histActive) {
                    // File mode and history never dead-end: clamp onto the
                    // nearest selectable row, so arrows always leave a live
                    // target for Enter.
                    int step = GridActive() ? GridColsEff() : 1;
                    int to = sel;
                    for (int i = 0; i < step; i++) {
                        int nx = ClampMoveSel(g_results, to, +1);
                        if (nx < 0 || nx == to) break;
                        to = nx;
                    }
                    SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                    SyncSelToBox(to);
                    EnsureVisible(to);
                } else {
                    // Grid mode jumps a full tile row; list mode steps one row.
                    int step = GridActive() ? GridColsEff() : 1;
                    int to = sel;
                    bool moved = false;
                    for (int i = 0; i < step; i++) {
                        int nx = MoveSelNoWrap(to, +1);
                        if (nx < 0) break;
                        to = nx;
                        moved = true;
                    }
                    if (moved) {
                        SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                        SyncSelToBox(to);
                        EnsureVisible(to);
                    } else {
                        RestoreOrigSel(); // past last: back to the typed text
                    }
                }
                InvalidateRect(g_hwndList, NULL, FALSE);
                if (haveChips) InvalidateRect(g_hwndChipRow, NULL, FALSE);
            }
            return 0;
        case VK_UP:
            // Empty box + Up = history recall in any mode (nothing to show:
            // fall through to normal navigation).
            if (!g_histActive && !g_navigating && GetWindowTextLengthW(h) == 0) {
                if (ShowHistory()) return 0;
            }
            g_confirmPower = -1; // navigating disarms a pending confirm
            if (!g_chipsFocus && (sel <= 0) && haveChips) {
                g_chipsFocus = true;
                EnsureNavStart();
                SendMessageW(g_hwndList, LB_SETCURSEL, (WPARAM)-1, 0);
                InvalidateRect(g_hwndChipRow, NULL, FALSE);
                InvalidateRect(g_hwndList, NULL, FALSE);
            } else if (count > 0) {
                EnsureNavStart();
                if (InFileMode() || g_histActive) {
                    // File mode and history never dead-end: clamp, never clear.
                    int step = GridActive() ? GridColsEff() : 1;
                    int to = sel;
                    for (int i = 0; i < step; i++) {
                        int nx = ClampMoveSel(g_results, to, -1);
                        if (nx < 0 || nx == to) break;
                        to = nx;
                    }
                    SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                    SyncSelToBox(to);
                    EnsureVisible(to);
                } else {
                    // Grid mode jumps a full tile row; list mode steps one row.
                    int step = GridActive() ? GridColsEff() : 1;
                    int to = sel;
                    bool moved = false;
                    for (int i = 0; i < step; i++) {
                        int nx = MoveSelNoWrap(to, -1);
                        if (nx < 0) break;
                        to = nx;
                        moved = true;
                    }
                    if (moved) {
                        SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                        SyncSelToBox(to);
                        EnsureVisible(to);
                    } else {
                        RestoreOrigSel(); // above first: back to the typed text
                    }
                }
                InvalidateRect(g_hwndList, NULL, FALSE);
                if (haveChips) InvalidateRect(g_hwndChipRow, NULL, FALSE);
            }
            return 0;
        case VK_LEFT:
        case VK_RIGHT:
            // Grid mode only: sideways tile step. List mode keeps caret keys.
            if (GridActive() && count > 0) {
                g_confirmPower = -1;
                EnsureNavStart();
                // Grid is file-mode-only: clamp, never clear (no dead spot).
                int to = ClampMoveSel(g_results, sel, (w == VK_RIGHT) ? +1 : -1);
                if (to >= 0) {
                    SendMessageW(g_hwndList, LB_SETCURSEL, to, 0);
                    SyncSelToBox(to);
                    EnsureVisible(to);
                }
                InvalidateRect(g_hwndList, NULL, FALSE);
                return 0; // consumed by the grid
            }
            break;
        case VK_BACK:
            if (InFileMode()) {
                // Go up when the box is empty OR holds an arrow-mirrored name:
                // only a user-typed filter gets normal letter deletion.
                bool empty = GetWindowTextLengthW(h) == 0;
                if (empty || g_navigating) {
                    if (!g_fileDir.empty()) {
                        g_fileDir = ParentDirOf(g_fileDir); // up one, drives at root
                        if (g_fileDir.empty()) RefreshDriveCache();
                    } else
                        DeactivateFileMode(); // drive list -> exit file mode
                    g_navigating = false;
                    g_changingEdit = true; // programmatic clear: no refilter
                    SetWindowTextW(h, L"");
                    SendMessageW(h, EM_SETSEL, 0, 0);
                    g_changingEdit = false;
                    UpdateResults();
                    return 0;
                }
                break; // typed filter: Backspace deletes a letter as usual
            }
            // Empty query + Backspace exits bang-chip mode.
            if (g_activeBang >= 0 && GetWindowTextLengthW(h) == 0) {
                DeactivateBang();
                UpdateResults();
                return 0;
            }
            break;
        case VK_F5:
            BuildIndexAsync(true); // manual rescan: full pass incl. PowerShell
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
        // Solid pill, white bold text, no outline. Click x exits bang/file mode.
        const Bang* b = ActiveBang();
        std::wstring text;
        COLORREF col = kHintCol;
        if (InFileMode()) { text = L"Files"; col = kFileCol; }
        else if (b) { text = b->name; col = b->color; }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        FillRect(dc, &r, g_brInput); // corners too: nothing peeks through
        FillPill(dc, r, ChipFill(col));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontChip);
        SetTextColor(dc, kChipText);
        RECT tr = r; tr.left += Dpx(10); tr.right -= Dpx(24);
        DrawTextW(dc, text.c_str(), -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT xr = r; xr.left = xr.right - Dpx(20);
        DrawTextW(dc, L"x", -1, &xr, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        RECT r; GetClientRect(hwnd, &r);
        int x = GET_X_LPARAM(l);
        if (x >= r.right - Dpx(24)) {
            if (InFileMode()) DeactivateFileMode(); // clicked the x
            else DeactivateBang();
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
    return (r.right - r.left) + Dpx(32);
}
// Chip pill rectangles are measured once per chip-set/size, not on every paint
// and every click. The chip row repaints on every keystroke while chips are
// visible, and each measurement was a GetDC plus one DT_CALCRECT per chip.
// Cache validity is derived from the live chip titles and the client width, so
// it can never go stale; the titles it was built for are kept for comparison.
static std::vector<std::wstring> g_chipLayoutTitles;
static int g_chipLayoutW = -1;
static int g_chipHintW = -1; // key-guide width, measured once per font/DPI

static void EnsureChipRects(const RECT& client) {
    if (g_chipLayoutW == client.right && g_chipLayoutTitles.size() == g_chips.size()) {
        bool same = true;
        for (size_t i = 0; i < g_chips.size(); i++)
            if (g_chipLayoutTitles[i] != g_chips[i].title) { same = false; break; }
        if (same) return;
    }
    g_chipRects.clear();
    int x = Dpx(12);
    int h = 28;
    int y = (client.bottom - client.top - h) / 2;
    HDC dc = GetDC(g_hwndChipRow);
    HGDIOBJ old = SelectObject(dc, g_fontChip);
    for (const auto& c : g_chips) {
        int w = ChipTextW(dc, c.title);
        RECT r{x, y, x + w, y + h};
        g_chipRects.push_back(r);
        x += w + Dpx(8);
    }
    SelectObject(dc, old);
    ReleaseDC(g_hwndChipRow, dc);
    g_chipLayoutTitles.clear();
    g_chipLayoutTitles.reserve(g_chips.size());
    for (const auto& c : g_chips) g_chipLayoutTitles.push_back(c.title);
    g_chipLayoutW = client.right;
}

// Chips + DPI change invalidates every cached measurement.
static void InvalidateChipCaches() {
    // Widths depend on g_fontChip, so a font rebuild invalidates them too
    // (WM_DPICHANGED rebuilds fonts, then calls this).
    g_chipLayoutW = -1;
    g_chipLayoutTitles.clear();
    g_chipHintW = -1;
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
        EnsureChipRects(cl);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontChip);
        PillPainter pills(dc); // one GDI+ Graphics for the whole pass
        for (size_t i = 0; i < g_chips.size() && i < g_chipRects.size(); i++) {
            const ChipItem& c = g_chips[i];
            const RECT& r = g_chipRects[i];
            bool sel = g_chipsFocus && (int)i == g_chipSel;
            COLORREF base = ChipFill(c.color);
            // Solid pills, white bold text; selection = full brightness.
            pills.Fill(r, sel ? base : Darken(base, 65));
            SetTextColor(dc, kChipText);
            RECT tr = r;
            DrawTextW(dc, c.title.c_str(), -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        }
        // Low-contrast key guide at the right end (only when it fits).
        if (!g_chipRects.empty()) {
            const wchar_t* hint = L"\u2191\u2193 move \u00B7 Enter open \u00B7 Shift+Enter web \u00B7 Esc close";
            SelectObject(dc, g_fontSub);
            if (g_chipHintW < 0) {
                RECT hr{0, 0, 0, 0};
                DrawTextW(dc, hint, -1, &hr, DT_SINGLELINE | DT_CALCRECT);
                g_chipHintW = hr.right - hr.left;
            }
            int hw = g_chipHintW;
            if (g_chipRects.back().right + Dpx(12) + hw < cl.right - Dpx(8)) {
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, kSub);
                RECT rr = cl; rr.left = rr.right - Dpx(8) - hw;
                DrawTextW(dc, hint, -1, &rr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        RECT cl; GetClientRect(hwnd, &cl);
        EnsureChipRects(cl);
        for (size_t i = 0; i < g_chipRects.size(); i++) {
            if (PtInRect(&g_chipRects[i], pt)) { ExecuteChip((int)i); return 0; }
        }
        SetFocus(g_hwndEdit);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ------------------------------------------------------- file bottom bar
// Minimalist Explorer shortcut: one slim bar docked under the list, visible
// in file mode (dir level) only. Click opens the current folder in Explorer.
static LRESULT CALLBACK FileBarProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cl; GetClientRect(hwnd, &cl);
        FillRect(dc, &cl, g_brBg);
        // hairline separator on top
        HBRUSH sep = CreateSolidBrush(kSep);
        RECT sr = cl; sr.bottom = sr.top + 1;
        FillRect(dc, &sr, sep);
        DeleteObject(sep);
        // amber tick marking the action
        HBRUSH tick = CreateSolidBrush(Darken(kFolderCol, 65));
        RECT br{cl.left + Dpx(14), cl.top + Dpx(8), cl.left + Dpx(16), cl.bottom - Dpx(8)};
        FillRect(dc, &br, tick);
        DeleteObject(tick);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontSub);
        SetTextColor(dc, kText);
        RECT tr = cl; tr.left += Dpx(34); tr.right -= Dpx(10);
        DrawTextW(dc, L"Enter/Space open \u00B7 Shift+Enter folder", -1, &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (InFileMode() && !g_fileDir.empty()) {
            std::wstring dir = g_fileDir; // copy: HidePalette clears file state
            HidePalette(); // hide first: never wait for Explorer
            if ((UINT_PTR)ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL) <= 32)
                NotifyLaunchFailed(L"Couldn't open this folder");
        }
        return 0;
    }
    case WM_MOUSEWHEEL: // wheel over the bar scrolls the list above it
        WheelScrollList(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// Hover fill between background and highlight. Both painters used to build
// this brush on every hover repaint; the globals up top cache it instead.
static void PaintHoverFill(HDC dc, const RECT& r) {
    COLORREF want = RGB((GetRValue(kBg) + GetRValue(kSel)) / 2,
                         (GetGValue(kBg) + GetGValue(kSel)) / 2,
                         (GetBValue(kBg) + GetBValue(kSel)) / 2);
    if (want != g_hoverBrushFor) {
        if (g_hoverBrush) DeleteObject(g_hoverBrush);
        g_hoverBrush = CreateSolidBrush(want);
        g_hoverBrushFor = want;
    }
    if (g_hoverBrush) FillRect(dc, r, g_hoverBrush);
}
// Drop the cached hover brush. Idempotent; safe even if it was never made.
static void ReleaseHoverBrush() {
    if (g_hoverBrush) { DeleteObject(g_hoverBrush); g_hoverBrush = NULL; }
    g_hoverBrushFor = (COLORREF)-1;
}

// Screen point near a result row for the keyboard menu key: selected tile
// center in grid mode, selected list-row center otherwise. Falls back to the
// palette center when the row is scrolled out of view or unknown.
static POINT RowScreenPoint(int resultsIdx) {
    POINT fb{0, 0};
    if (g_hwndPalette) {
        RECT wr;
        GetWindowRect(g_hwndPalette, &wr);
        fb.x = (wr.left + wr.right) / 2;
        fb.y = (wr.top + wr.bottom) / 2;
    }
    if (resultsIdx < 0 || resultsIdx >= (int)g_results.size()) return fb;
    if (GridActive() && g_hwndFileGrid) {
        size_t base = (!g_results.empty() &&
                       g_results.front().kind == ResultKind::Header) ? 1 : 0;
        if ((size_t)resultsIdx < base) return fb;
        int cols = GridColsEff();
        if (cols < 1) cols = 1;
        RECT gr;
        GetWindowRect(g_hwndFileGrid, &gr);
        int tw = (gr.right - gr.left) / cols;
        if (tw <= 0) return fb;
        int t = resultsIdx - (int)base - g_gridTopRow * cols;
        int c = t % cols, r = t / cols;
        if (r < 0 || r >= GridRows() || c < 0) return fb;
        POINT p{gr.left + c * tw + tw / 2,
                gr.top + kGridHeadH + r * kGridTileH + kGridTileH / 2};
        return p;
    }
    if (g_hwndList) {
        // Listbox items are added in g_results order: indices match.
        RECT ri{};
        if (SendMessageW(g_hwndList, LB_GETITEMRECT, resultsIdx, (LPARAM)&ri) != LB_ERR) {
            RECT lr, cl;
            GetWindowRect(g_hwndList, &lr);
            GetClientRect(g_hwndList, &cl);
            int cy = (ri.top + ri.bottom) / 2;
            if (cy >= 0 && cy <= cl.bottom - cl.top) {
                POINT p{lr.left + (lr.right - lr.left) / 2, lr.top + cy};
                return p;
            }
        }
    }
    return fb;
}

// ------------------------------------------------------- file grid window
// Fit a tile name into at most two wrapped lines with a real ellipsis.
// (DT_END_ELLIPSIS has no effect combined with DT_WORDBREAK, so overlong
// names would otherwise clip mid-glyph at the tile edge.)
//
// The binary search probes a truncated prefix + ellipsis. Building that probe
// as substr(0,n) + "..." allocated a std::wstring per probe, per tile, per
// repaint. Probing a reused buffer with an explicit character count allocates
// nothing after the first paint. All painting is single-threaded on the UI
// thread, so one shared scratch buffer per function is safe.
static const wchar_t kEllipsis = (wchar_t)0x2026;
static std::vector<wchar_t>& EllipsisScratch(size_t need) {
    static std::vector<wchar_t> buf;
    if (buf.size() < need + 1) buf.resize(need + 1);
    return buf;
}

static std::wstring FitTileText(HDC dc, const std::wstring& s, int w, int maxH) {
    if (s.empty() || w <= 0 || maxH <= 0) return s;
    {
        RECT r{0, 0, w, 0};
        DrawTextW(dc, s.c_str(), -1, &r,
                  DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_TOP);
        if (r.bottom - r.top <= maxH) return s;
    }
    std::vector<wchar_t>& buf = EllipsisScratch(s.size());
    std::wstring best = L"…";
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2; // >= 1 here: s is non-empty
        memcpy(buf.data(), s.c_str(), mid * sizeof(wchar_t));
        buf[mid] = kEllipsis;
        RECT r2{0, 0, w, 0};
        DrawTextW(dc, buf.data(), (int)(mid + 1), &r2,
                  DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_TOP);
        if (r2.bottom - r2.top <= maxH) { best.assign(buf.data(), mid + 1); lo = mid; }
        else hi = mid - 1;
    }
    return best;
}

// Single-line fit with a true trailing ellipsis (keeps the start).
// Used after FitTileText when everything fits on one line: guarantees the
// line ends with "..." instead of a hard mid-glyph cut at the tile edge.
static std::wstring FitOneLine(HDC dc, const std::wstring& s, int w) {
    if (s.empty() || w <= 0) return s;
    {
        RECT r{0, 0, 0, 0};
        DrawTextW(dc, s.c_str(), -1, &r,
                  DT_CALCRECT | DT_SINGLELINE | DT_LEFT | DT_TOP);
        if (r.right - r.left <= w) return s;
    }
    std::vector<wchar_t>& buf = EllipsisScratch(s.size());
    std::wstring best = L"…";
    size_t lo = 0, hi = s.size();
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        memcpy(buf.data(), s.c_str(), mid * sizeof(wchar_t));
        buf[mid] = kEllipsis;
        RECT r2{0, 0, 0, 0};
        DrawTextW(dc, buf.data(), (int)(mid + 1), &r2,
                  DT_CALCRECT | DT_SINGLELINE | DT_LEFT | DT_TOP);
        if (r2.right - r2.left <= w) { best.assign(buf.data(), mid + 1); lo = mid; }
        else hi = mid - 1;
    }
    return best;
}

// Explorer-like tiles, file grid-view only. Paints + hit-tests; selection
// lives in the hidden listbox (same indices), so nav/execute are untouched.
static int GridHitTest(int fx, int fy) {
    if (!g_hwndFileGrid || g_results.empty()) return -1;
    size_t base = (g_results.front().kind == ResultKind::Header) ? 1 : 0;
    RECT cl;
    if (!GetClientRect(g_hwndFileGrid, &cl)) return -1;
    int y = base ? kGridHeadH : 0;
    if (fy < y) return -1;
    int cols = GridColsEff();
    if (cols < 1) cols = 1;
    int tw = (cl.right - cl.left) / cols;
    if (tw <= 0) return -1;
    int c = fx / tw;
    int r = (fy - y) / kGridTileH;
    if (c < 0 || c >= cols || r < 0) return -1;
    size_t idx = base + (size_t)(g_gridTopRow + r) * (size_t)cols + (size_t)c;
    if (idx >= g_results.size()) return -1;
    if (g_results[idx].kind == ResultKind::Header) return -1;
    return (int)idx;
}

static LRESULT CALLBACK FileGridProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cl; GetClientRect(hwnd, &cl);
        FillRect(dc, &cl, g_brBg);
        int y = 0;
        size_t base = 0;
        if (!g_results.empty() && g_results.front().kind == ResultKind::Header) {
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, g_fontSub);
            SetTextColor(dc, kSub);
            RECT hr{cl.left + Dpx(14), 0, cl.right - Dpx(8), kGridHeadH};
            DrawTextW(dc, g_results.front().title.c_str(), -1, &hr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y = kGridHeadH;
            base = 1;
        }
        int cols = GridColsEff();
        if (cols < 1) cols = 1;
        int tw = (cl.right - cl.left) / cols;
        int sel = (int)SendMessageW(g_hwndList, LB_GETCURSEL, 0, 0);
        size_t start = base + (size_t)g_gridTopRow * (size_t)cols;
        // One line's height in g_fontSub: loop-invariant, but the old code
        // re-measured a literal "X" with DT_CALCRECT for every tile, every
        // repaint (and the grid repaints on every hover move).
        int oneLineH;
        {
            HGDIOBJ keep = SelectObject(dc, g_fontSub);
            RECT oneL{0, 0, 0, 0};
            DrawTextW(dc, L"X", -1, &oneL,
                      DT_CALCRECT | DT_SINGLELINE | DT_LEFT | DT_TOP);
            oneLineH = oneL.bottom - oneL.top;
            SelectObject(dc, keep);
        }
        for (size_t i = start; i < g_results.size(); i++) {
            size_t t = i - base;
            int r = (int)(t / (size_t)cols) - g_gridTopRow;
            if (r >= GridRows()) break;
            int c = (int)(t % (size_t)cols);
            // Inset tiles: a gutter between neighbors so labels can never
            // touch or overlap, at any column count.
            RECT tile{cl.left + c * tw + Dpx(2), y + r * kGridTileH,
                      cl.left + (c + 1) * tw - Dpx(2), y + (r + 1) * kGridTileH};
            const ResultItem& ri = g_results[i];
            if (ri.kind == ResultKind::Header) continue; // safety
            bool isCur = ((int)i == sel);
            if (isCur) FillRect(dc, &tile, g_brSel);
            // Hover preview: subtle fill, visual only.
            if (!isCur && (int)i == g_hoverRow) PaintHoverFill(dc, &tile);
            int gz = Dpx(32);
            int gx = tile.left + (tile.right - tile.left - gz) / 2;
            RECT gcell{gx, tile.top + Dpx(8), gx + gz, tile.top + Dpx(8) + gz};
            FileGlyph fg = GlyphForResult(ri.kind, ri.title, ri.action, ri.color);
            PaintGlyph(dc, gcell, fg, isCur, ri.title, g_fontGrid);
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, g_fontSub);
            SetTextColor(dc, isCur ? kText : kRowText);
            RECT nr{tile.left + Dpx(4), gcell.bottom + Dpx(4),
                    tile.right - Dpx(4), tile.bottom - Dpx(2)};
            // Pre-fitted to two lines with a true ellipsis: long names
            // shrink with "..." instead of clipping mid-glyph at the edge.
            std::wstring shown = FitTileText(dc, ri.title,
                                             nr.right - nr.left,
                                             nr.bottom - nr.top);
            // Width cap: a single rendered line that's still too wide ends
            // with a real "..." instead of overflowing (two-line results
            // already fit their width via wrapping). With both caps holding,
            // centered text can never clip either end.
            {
                RECT hr3{0, 0, nr.right - nr.left, 0};
                DrawTextW(dc, shown.c_str(), -1, &hr3,
                          DT_CALCRECT | DT_WORDBREAK | DT_LEFT | DT_TOP);
                if (hr3.bottom - hr3.top <= oneLineH)
                    shown = FitOneLine(dc, shown, nr.right - nr.left);
            }
            // Tile names are always centered.
            DrawTextW(dc, shown.c_str(), -1, &nr,
                      DT_CENTER | DT_TOP | DT_WORDBREAK);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int idx = GridHitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (idx >= 0) {
            EnsureNavStart();
            SendMessageW(g_hwndList, LB_SETCURSEL, idx, 0);
            SyncSelToBox(idx);
            EnsureVisible(idx);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        POINT dpt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ArmRowDrag(idx, dpt); // files/folders/drives arm a drag-out
        SetFocus(g_hwndEdit);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        // Only a tile double-click acts: blank-space double-clicks do
        // nothing (they must never fire the cursor's row). The hit tile
        // becomes the cursor first, so Enter-logic sees the right row.
        CancelRowDrag();
        int idx = GridHitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (idx < 0 || idx >= (int)g_results.size() ||
            g_results[(size_t)idx].kind == ResultKind::Header)
            return 0;
        EnsureNavStart();
        SendMessageW(g_hwndList, LB_SETCURSEL, idx, 0);
        ExecuteSelected(false);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT dpt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        MoveRowDrag(hwnd, dpt);
        // Hover preview only: never moves selection or the multi-set.
        int hov = GridHitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hov != g_hoverRow) {
            int old = g_hoverRow;
            g_hoverRow = hov;
            auto invalidateTile = [&](int idx) {
                if (idx < 0 || !hwnd) return;
                size_t base = (!g_results.empty() &&
                               g_results.front().kind == ResultKind::Header) ? 1 : 0;
                int y = base ? kGridHeadH : 0;
                RECT cl;
                if (!GetClientRect(hwnd, &cl)) return;
                int cols = GridColsEff();
                if (cols < 1) cols = 1;
                int tw = (cl.right - cl.left) / cols;
                if (tw <= 0) return;
                size_t t = (size_t)idx - base;
                int r = (int)(t / (size_t)cols) - g_gridTopRow;
                int c = (int)(t % (size_t)cols);
                if (r < 0 || r >= GridRows() || c < 0 || c >= cols) return;
                RECT tile{cl.left + c * tw + Dpx(2), y + r * kGridTileH,
                          cl.left + (c + 1) * tw - Dpx(2), y + (r + 1) * kGridTileH};
                InvalidateRect(hwnd, &tile, FALSE);
            };
            if (hov >= 0) TrackHoverLeave(hwnd);
            invalidateTile(old);
            invalidateTile(hov);
        }
        break;
    }
    case WM_MOUSELEAVE:
        if (g_hoverRow >= 0) {
            g_hoverRow = -1;
            // Invalidate every tile region that could have shown a hover fill.
            {
                RECT cl;
                if (GetClientRect(hwnd, &cl)) InvalidateRect(hwnd, &cl, FALSE);
            }
        }
        return 0;
    case WM_MOUSEWHEEL: // wheel over tiles moves through them
        WheelScrollList(GET_WHEEL_DELTA_WPARAM(w));
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ------------------------------------------------------- context menus
// Dark owner-drawn popup menus (same black/rounded language as the palette:
// kBg rows, kSel highlight, kText/kSub text, g_fontUI, category accent bar).
// One builder serves row menus, the palette menu, and the tray menu.
// Row target rides in g_menuRow; menu item data carries the g_menuItems index.
struct DarkMenuItem {
    std::wstring text;
    COLORREF accent = kMenuAcc;
    UINT wID = 0; // 0 = separator (never returned)
    bool separator = false;
    bool checked = false;
    bool disabled = false;
};
static std::vector<DarkMenuItem> g_menuItems;
static int g_menuW = 200;
static bool g_inMenu = false;  // suppress dismiss-while-menu-open
static bool g_dragging = false; // suppress dismiss-while-dragging
static int g_menuRow = -1;
static const UINT kMenuVerbBase = 3000;   // wID = base + (int)VerbId
static const UINT kMenuEngineBase = 3200; // wID = base + engine index
static void ShowPaletteMenu(int x, int y); // defined below ShowRowMenu

struct EnginePreset { const wchar_t* name; const wchar_t* url; };
static const EnginePreset kEnginePresets[] = {
    {L"DuckDuckGo", L"https://duckduckgo.com/?q=%s"},
    {L"Google", L"https://www.google.com/search?q=%s"},
    {L"Bing", L"https://www.bing.com/search?q=%s"},
    {L"Brave", L"https://search.brave.com/search?q=%s"},
};

static void SizeMenuItems() {
    g_menuW = Dpx(120);
    if (g_menuItems.empty() || !g_fontUI) return;
    HDC dc = GetDC(NULL);
    if (!dc) return;
    HGDIOBJ old = SelectObject(dc, g_fontUI);
    for (const auto& it : g_menuItems) {
        if (it.separator) continue;
        RECT r{0, 0, 0, 0};
        DrawTextW(dc, it.text.c_str(), -1, &r, DT_SINGLELINE | DT_CALCRECT);
        int w = (r.right - r.left) + kPadX * 2 + Dpx(40);
        if (w > g_menuW) g_menuW = w;
    }
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    if (g_menuW > Dpx(320)) g_menuW = Dpx(320);
}

static HMENU BuildMenuPopup(size_t begin, size_t end) {
    HMENU m = CreatePopupMenu();
    if (!m) return NULL;
    for (size_t i = begin; i < end && i < g_menuItems.size(); i++) {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STATE;
        mi.fType = MFT_OWNERDRAW; // separators too: system lines are light-themed
        mi.wID = g_menuItems[i].wID;
        mi.dwItemData = (ULONG_PTR)i;
        mi.fState = g_menuItems[i].disabled ? MFS_GRAYED : MFS_ENABLED;
        InsertMenuItemW(m, (UINT)(i - begin), TRUE, &mi);
    }
    return m;
}

static void DrawDarkMenuItem(const DRAWITEMSTRUCT* d) {
    size_t ix = (size_t)d->itemData;
    HDC dc = d->hDC;
    bool sel = (d->itemState & ODS_SELECTED) != 0;
    if (ix >= g_menuItems.size()) { FillRect(dc, &d->rcItem, g_brBg); return; }
    const DarkMenuItem& it = g_menuItems[ix];
    bool dis = it.disabled;
    FillRect(dc, &d->rcItem, (sel && !dis) ? g_brSel : g_brBg);
    if (it.separator) {
        int y = (d->rcItem.top + d->rcItem.bottom) / 2;
        GlyphLine(dc, d->rcItem.left + kPadX, y, d->rcItem.right - kPadX, y,
                  1, kSep);
        return;
    }
    // Category accent bar, like the result rows.
    int bx = d->rcItem.left + kPadX;
    HBRUSH bar = CreateSolidBrush(dis ? Darken(it.accent, 40) : it.accent);
    RECT br{bx, d->rcItem.top + Dpx(7), bx + Dpx(2), d->rcItem.bottom - Dpx(7)};
    FillRect(dc, &br, bar);
    DeleteObject(bar);
    // Check tick (engine choice, startup toggle): two strokes, no glyph font.
    int tx = bx + Dpx(10);
    if (it.checked) {
        COLORREF tc = dis ? kSub : it.accent;
        int cy = (d->rcItem.top + d->rcItem.bottom) / 2;
        GlyphLine(dc, tx, cy, tx + Dpx(4), cy + Dpx(4), 2, tc);
        GlyphLine(dc, tx + Dpx(4), cy + Dpx(4), tx + Dpx(10), cy - Dpx(4), 2, tc);
    }
    RECT tr = d->rcItem; tr.left = tx + Dpx(16);
    SetBkMode(dc, TRANSPARENT);
    if (g_fontUI) SelectObject(dc, g_fontUI);
    SetTextColor(dc, dis ? kSub : (sel ? kText : kRowText));
    DrawTextW(dc, it.text.c_str(), -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static int TrackDarkMenu(HMENU m, int x, int y) {
    if (!m || !g_hwndPalette) return 0;
    SetForegroundWindow(g_hwndPalette); // menu dismisses correctly
    g_inMenu = true;
    int cmd = (int)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                    x, y, g_hwndPalette, NULL);
    g_inMenu = false;
    PostMessageW(g_hwndPalette, WM_NULL, 0, 0); // restore focus hygiene
    return cmd;
}

// Row -> verb context (drive/removable resolved here, registry-free).
static VerbCtx CtxForRow(const ResultItem& r) {
    VerbCtx c;
    c.kind = r.kind;
    if (r.kind == ResultKind::App) {
        c.isStore = r.isStore;
        c.hasPath = !r.action.empty();
    } else if (r.kind == ResultKind::File) {
        c.glyph = FileGlyphFor(r.title, false).style;
        c.hasPath = !r.action.empty();
    } else if (r.kind == ResultKind::Folder) {
        c.hasPath = !r.action.empty();
        c.isDrive = IsDriveRootDir(r.action);
        if (c.isDrive) {
            UINT t = GetDriveTypeW(r.action.c_str());
            c.isRemovable = (t == DRIVE_REMOVABLE || t == DRIVE_CDROM);
        }
    }
    return c;
}

static bool ShellVerbOn(const std::wstring& path, const wchar_t* verb, bool hideFirst) {
    if (path.empty()) return false;
    if (hideFirst) HidePalette(); // hide first: never wait for the target
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_DEFAULT;
    sei.lpVerb = verb;
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

static void HideAppRow(const ResultItem& r) {
    EnsureSettingsLists(); // SaveSettings rewrites every section
    // g_hidden already holds trimmed+lowercased names, so membership is an O(1)
    // hash probe instead of an O(n) scan that lowercased every candidate.
    if (g_hidden.count(BangLower(BangTrim(r.title)))) {
        UpdateResults();
        return; // already hidden
    }
    g_settings.hiddenApps.push_back(r.title);
    SaveOrWarn(L"hidden apps");
    RebuildHiddenSet();
    RefreshAppsListPreservingChecks();
    UpdateResults(); // stay open, list refreshes without the app
}

static void ApplyEnginePreset(int idx) {
    if (idx < 0 || idx >= (int)(sizeof(kEnginePresets) / sizeof(kEnginePresets[0])))
        return;
    EnsureSettingsLists(); // SaveSettings rewrites every section
    g_settings.engineName = kEnginePresets[(size_t)idx].name;
    g_settings.engineUrl = kEnginePresets[(size_t)idx].url;
    SaveOrWarn(L"search engine");
    UpdateResults();
}

static void RunRowVerb(int rowIdx, VerbId verb) {
    if (rowIdx < 0 || rowIdx >= (int)g_results.size()) return;
    ResultItem r = g_results[(size_t)rowIdx]; // copy: some verbs rebuild results
    g_chipsFocus = false;
    switch (verb) {
    case VerbId::Open:
        SendMessageW(g_hwndList, LB_SETCURSEL, rowIdx, 0);
        ExecuteSelected(false);
        return;
    case VerbId::CopyPath:
        CopyTextToClipboard(r.action);
        return;
    case VerbId::CopyName:
        CopyTextToClipboard(r.title);
        return;
    case VerbId::CopyUrl:
        CopyTextToClipboard(r.action);
        return;
    case VerbId::CopyResult:
        CopyTextToClipboard(r.action);
        return;
    case VerbId::RunAsAdmin: {
        HidePalette();
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"runas";
        sei.lpFile = r.action.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei) && GetLastError() != ERROR_CANCELLED)
            NotifyLaunchFailed(L"Could not launch as administrator");
        return;
    }
    case VerbId::OpenLocation: {
        HidePalette();
        std::wstring params = L"/select,\"" + r.action + L"\"";
        if ((UINT_PTR)ShellExecuteW(NULL, L"open", L"explorer.exe", params.c_str(),
                                    NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Could not reveal the file");
        return;
    }
    case VerbId::OpenNewWindow:
        HidePalette();
        if ((UINT_PTR)ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL,
                                    SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Could not open the folder");
        return;
    case VerbId::OpenWith: {
        HidePalette();
        std::wstring args = L"shell32.dll,OpenAs_RunDLL \"" + r.action + L"\"";
        if ((UINT_PTR)ShellExecuteW(NULL, L"open", L"rundll32.exe", args.c_str(),
                                    NULL, SW_SHOWNORMAL) <= 32)
            NotifyLaunchFailed(L"Could not open the picker");
        return;
    }
    case VerbId::Print:
        if (!ShellVerbOn(r.action, L"print", true))
            NotifyLaunchFailed(L"Could not print");
        return;
    case VerbId::SetWallpaper:
        HidePalette();
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)r.action.c_str(),
                              SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        return;
    case VerbId::ExtractAll: {
        HidePalette();
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.lpVerb = L"ExtractAll";
        sei.lpFile = r.action.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) // no handler: just open the archive
            ShellExecuteW(NULL, L"open", r.action.c_str(), NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    case VerbId::Eject:
        HidePalette(); // Safely-Remove dialog; always present, no failure mode
        ShellExecuteW(NULL, L"open", L"rundll32.exe",
                      L"shell32.dll,Control_RunDLL hotplug.dll", NULL, SW_SHOWNORMAL);
        return;
    case VerbId::Properties: {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_INVOKEIDLIST;
        sei.lpVerb = L"properties";
        sei.lpFile = r.action.c_str();
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei); // modeless dialog: palette dismissal is automatic
        return;
    }
    case VerbId::HideApp:
        HideAppRow(r);
        return;
    case VerbId::ManageBangs:
    case VerbId::OpenSettings:
        OpenSettings();
        return;
    case VerbId::RefreshApps:
        g_indexReady.store(false);
        BuildIndexAsync(true);
        return;
    case VerbId::ToggleStartup:
        g_settings.runAtStartup = !g_settings.runAtStartup;
        EnsureSettingsLists();
        SaveOrWarn(L"run at startup");
        ApplyRunAtStartup(g_settings.runAtStartup);
        return;
    case VerbId::Quit:
        DestroyWindow(g_hwndPalette);
        return;
    default:
        return; // SwitchEngine is a submenu parent, never dispatched
    }
}

static void ShowRowMenu(int row, int x, int y) {
    if (row < 0 || row >= (int)g_results.size()) return;
    const ResultItem& rr = g_results[(size_t)row];
    auto verbs = VerbsFor(CtxForRow(rr));
    if (verbs.empty()) { /* fall through to palette menu below */ }
    else {
        // Highlight the target without mirroring it into the box:
        // right-click must never rewrite the query.
        SendMessageW(g_hwndList, LB_SETCURSEL, row, 0);
        InvalidateRect(g_hwndList, NULL, FALSE);
        if (!g_chips.empty()) InvalidateRect(g_hwndChipRow, NULL, FALSE);
        g_menuRow = row;
        g_menuItems.clear();
        size_t subAt = (size_t)-1;
        for (VerbId v : verbs) {
            DarkMenuItem it;
            it.text = VerbLabel(v);
            it.accent = rr.color;
            it.wID = kMenuVerbBase + (UINT)v;
            if (v == VerbId::SwitchEngine) subAt = g_menuItems.size();
            g_menuItems.push_back(std::move(it));
        }
        size_t parentEnd = g_menuItems.size(); // sub-items append after this
        HMENU hSub = NULL;
        if (subAt != (size_t)-1) {
            size_t base = g_menuItems.size();
            for (size_t i = 0; i < sizeof(kEnginePresets) / sizeof(kEnginePresets[0]); i++) {
                DarkMenuItem it;
                it.text = kEnginePresets[i].name;
                it.accent = kWebCol;
                it.wID = kMenuEngineBase + (UINT)i;
                it.checked = (g_settings.engineName == kEnginePresets[i].name);
                g_menuItems.push_back(std::move(it));
            }
            hSub = BuildMenuPopup(base, g_menuItems.size());
        }
        SizeMenuItems();
        HMENU hMenu = CreatePopupMenu();
        for (size_t i = 0; i < parentEnd; i++) {
            MENUITEMINFOW mi{};
            mi.cbSize = sizeof(mi);
            mi.fMask = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STATE;
            mi.fType = MFT_OWNERDRAW;
            mi.wID = g_menuItems[i].wID;
            mi.dwItemData = (ULONG_PTR)i;
            mi.fState = MFS_ENABLED;
            if (i == subAt && hSub) { mi.fMask |= MIIM_SUBMENU; mi.hSubMenu = hSub; }
            InsertMenuItemW(hMenu, (UINT)i, TRUE, &mi);
        }
        int cmd = TrackDarkMenu(hMenu, x, y);
        DestroyMenu(hMenu); // owned submenu goes with it
        if (cmd >= (int)kMenuEngineBase && cmd < (int)kMenuEngineBase + 16)
            ApplyEnginePreset(cmd - (int)kMenuEngineBase);
        else if (cmd >= (int)kMenuVerbBase && cmd < (int)kMenuVerbBase + 256)
            RunRowVerb(g_menuRow, (VerbId)(cmd - (int)kMenuVerbBase));
        g_menuRow = -1;
        return;
    }
    // No verbs (headers): palette menu instead.
    ShowPaletteMenu(x, y);
}

static void ShowPaletteMenu(int x, int y) {
    DarkMenuItem rf{L"Refresh apps", kAppCol, ID_TRAY_REFRESH};
    DarkMenuItem st{L"Settings...", kSetCol, ID_TRAY_SETTINGS};
    DarkMenuItem su{L"Run at startup", kCalcCol, ID_TRAY_STARTUP};
    su.checked = g_settings.runAtStartup;
    DarkMenuItem qu{L"Quit", kDanger, ID_TRAY_QUIT};
    g_menuRow = -1;
    g_menuItems.clear();
    g_menuItems.push_back(std::move(rf));
    g_menuItems.push_back(std::move(st));
    g_menuItems.push_back(std::move(su));
    g_menuItems.push_back(std::move(qu));
    SizeMenuItems();
    HMENU hMenu = BuildMenuPopup(0, g_menuItems.size());
    int cmd = TrackDarkMenu(hMenu, x, y);
    DestroyMenu(hMenu);
    if (cmd >= (int)ID_TRAY_SETTINGS && cmd <= (int)ID_TRAY_AWAKE)
        OnTrayCommand((UINT)cmd);
}

#include <objidl.h> // IDataObject for file drag-out
#include <oleidl.h> // IDropSource/IDropTarget, DoDragDrop, RegisterDragDrop

// ------------------------------------------------------- OLE drag and drop
// Lazy OleInitialize on the UI thread (first palette show): drag-out ships
// files as CF_HDROP, drop-in browses dropped folders / pastes file paths.
// Copy-only: the source is never deleted.
static bool g_oleInit = false;
static IDropTarget* g_dropTarget = nullptr;
static bool g_dragArmed = false;
static POINT g_dragPt{};
static int g_dragRow = -1;

// Preferred-drop-effect support: without it Explorer defaults same-volume
// drags to MOVE (the file vanishes from its folder). Advertising COPY keeps
// every palette drag a copy, everywhere.
static UINT g_cfPreferred = 0;
static UINT PreferredEffectFormat() {
    if (!g_cfPreferred)
        g_cfPreferred = RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return g_cfPreferred;
}
static void HdropFormatEtc(FORMATETC& f) {
    f.cfFormat = CF_HDROP;
    f.ptd = NULL;
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = -1;
    f.tymed = TYMED_HGLOBAL;
}
static void PreferredFormatEtc(FORMATETC& f) {
    f.cfFormat = (CLIPFORMAT)PreferredEffectFormat();
    f.ptd = NULL;
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = -1;
    f.tymed = TYMED_HGLOBAL;
}

class CEnumFmt : public IEnumFORMATETC {
    LONG m_ref = 1;
    FORMATETC m_fmt[2];
    ULONG m_at = 0;
public:
    CEnumFmt(const FORMATETC& a, const FORMATETC& b) { m_fmt[0] = a; m_fmt[1] = b; }
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) {
        if (!pp) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
            *pp = static_cast<IEnumFORMATETC*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP Next(ULONG celt, FORMATETC* out, ULONG* fetched) {
        if (!out) return E_INVALIDARG;
        ULONG n = 0;
        while (n < celt && m_at < 2) out[n++] = m_fmt[m_at++];
        if (fetched) *fetched = n;
        return (n == celt) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG celt) {
        m_at = (m_at + celt > 2) ? 2 : m_at + celt;
        return (m_at < 2 || celt == 0) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() { m_at = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumFORMATETC** pp) {
        if (!pp) return E_POINTER;
        CEnumFmt* c = new (std::nothrow) CEnumFmt(m_fmt[0], m_fmt[1]);
        if (!c) return E_OUTOFMEMORY;
        c->m_at = m_at;
        *pp = c;
        return S_OK;
    }
};

class HdropDataObject : public IDataObject {
    LONG m_ref = 1;
    std::wstring m_path;
public:
    explicit HdropDataObject(const std::wstring& p) : m_path(p) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) {
        if (!pp) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *pp = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP GetData(FORMATETC* f, STGMEDIUM* m) {
        if (!f || !m) return E_INVALIDARG;
        if (!(f->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
        if (g_cfPreferred && f->cfFormat == (CLIPFORMAT)g_cfPreferred) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
            if (!h) return E_OUTOFMEMORY;
            DWORD* p = (DWORD*)GlobalLock(h);
            if (!p) { GlobalFree(h); return E_OUTOFMEMORY; }
            *p = DROPEFFECT_COPY;
            GlobalUnlock(h);
            m->tymed = TYMED_HGLOBAL;
            m->hGlobal = h;
            m->pUnkForRelease = nullptr;
            return S_OK;
        }
        if (f->cfFormat != CF_HDROP)
            return DV_E_FORMATETC;
        size_t chars = m_path.size() + 2; // path + double null
        HGLOBAL h = GlobalAlloc(GHND | GMEM_SHARE, sizeof(DROPFILES) +
                                                     chars * sizeof(wchar_t));
        if (!h) return E_OUTOFMEMORY;
        DROPFILES* df = (DROPFILES*)GlobalLock(h);
        if (!df) { GlobalFree(h); return E_OUTOFMEMORY; }
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        wchar_t* dst = (wchar_t*)((BYTE*)df + sizeof(DROPFILES));
        memcpy(dst, m_path.c_str(), (m_path.size() + 1) * sizeof(wchar_t));
        dst[m_path.size() + 1] = L'\0';
        GlobalUnlock(h);
        m->tymed = TYMED_HGLOBAL;
        m->hGlobal = h;
        m->pUnkForRelease = nullptr;
        return S_OK;
    }
    STDMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) { return E_NOTIMPL; }
    STDMETHODIMP QueryGetData(FORMATETC* f) {
        if (!f) return E_INVALIDARG;
        if (!(f->tymed & TYMED_HGLOBAL)) return DV_E_FORMATETC;
        if (f->cfFormat == CF_HDROP) return S_OK;
        if (g_cfPreferred && f->cfFormat == (CLIPFORMAT)g_cfPreferred) return S_OK;
        return DV_E_FORMATETC;
    }
    STDMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) {
        if (!out) return E_INVALIDARG;
        return DATA_S_SAMEFORMATETC;
    }
    STDMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) { return E_NOTIMPL; }
    STDMETHODIMP EnumFormatEtc(DWORD dir, IEnumFORMATETC** pp) {
        if (!pp) return E_POINTER;
        if (dir != DATADIR_GET) return E_NOTIMPL;
        FORMATETC a{}, b{};
        PreferredFormatEtc(a);
        HdropFormatEtc(b);
        CEnumFmt* e = new (std::nothrow) CEnumFmt(a, b);
        if (!e) return E_OUTOFMEMORY;
        *pp = e;
        return S_OK;
    }
    STDMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    STDMETHODIMP DUnadvise(DWORD) { return OLE_E_ADVISENOTSUPPORTED; }
    STDMETHODIMP EnumDAdvise(IEnumSTATDATA**) { return OLE_E_ADVISENOTSUPPORTED; }
};

class DropSource : public IDropSource {
    LONG m_ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) {
        if (!pp) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *pp = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP QueryContinueDrag(BOOL esc, DWORD keyState) {
        if (esc) return DRAGDROP_S_CANCEL;
        if (!(keyState & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    STDMETHODIMP GiveFeedback(DWORD) { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

static bool HasHdrop(IDataObject* o) {
    if (!o) return false;
    FORMATETC f{};
    f.cfFormat = CF_HDROP;
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = -1;
    f.tymed = TYMED_HGLOBAL;
    return o->QueryGetData(&f) == S_OK;
}

static void DropPathsOnPalette(HDROP h) {
    if (!h || !g_hwndPalette || !IsWindowVisible(g_hwndPalette)) return;
    if (DragQueryFileW(h, 0xFFFFFFFF, NULL, 0) < 1) return;
    // Ask for the first path's length instead of always allocating 32K chars.
    UINT need = DragQueryFileW(h, 0, NULL, 0); // chars, excluding the NUL
    if (need == 0) return;
    std::vector<wchar_t> buf((size_t)need + 1);
    if (!DragQueryFileW(h, 0, buf.data(), (UINT)buf.size())) return;
    std::wstring path = buf.data();
    if (path.empty()) return;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ActivateFileMode(path); // browse the dropped folder
        return;
    }
    g_changingEdit = true; // paste the path as the query (no refilter yet)
    SetWindowTextW(g_hwndEdit, path.c_str());
    SendMessageW(g_hwndEdit, EM_SETSEL, (WPARAM)path.size(), (LPARAM)path.size());
    g_changingEdit = false;
    UpdateResults();
}

class PaletteDropTarget : public IDropTarget {
    LONG m_ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) {
        if (!pp) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *pp = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP DragEnter(IDataObject* o, DWORD, POINTL, DWORD* eff) {
        if (!eff) return E_INVALIDARG;
        *eff = HasHdrop(o) ? (*eff & DROPEFFECT_COPY) : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* eff) {
        if (!eff) return E_INVALIDARG;
        // Effect was validated on DragEnter; keep offering copy.
        *eff = DROPEFFECT_COPY;
        return S_OK;
    }
    STDMETHODIMP DragLeave() { return S_OK; }
    STDMETHODIMP Drop(IDataObject* o, DWORD, POINTL, DWORD* eff) {
        if (!eff) return E_INVALIDARG;
        *eff = DROPEFFECT_NONE;
        if (!HasHdrop(o)) return E_FAIL;
        FORMATETC f{};
        f.cfFormat = CF_HDROP;
        f.dwAspect = DVASPECT_CONTENT;
        f.lindex = -1;
        f.tymed = TYMED_HGLOBAL;
        STGMEDIUM m{};
        if (FAILED(o->GetData(&f, &m))) return E_FAIL;
        DropPathsOnPalette((HDROP)m.hGlobal);
        ReleaseStgMedium(&m);
        *eff = DROPEFFECT_COPY;
        return S_OK;
    }
};

static void EnsureOle() {
    if (g_oleInit) return;
    if (FAILED(OleInitialize(NULL))) return;
    g_oleInit = true;
    if (!g_dropTarget && g_hwndPalette) {
        PaletteDropTarget* t = new (std::nothrow) PaletteDropTarget();
        if (!t) return;
        if (FAILED(RegisterDragDrop(g_hwndPalette, t))) {
            t->Release();
            return;
        }
        g_dropTarget = t; // RegisterDragDrop holds its own reference
    }
}

// Only real filesystem paths start drags: files, folders, drive roots, and
// classic (non-Store) app targets (.lnk/.exe). Everything else ignores moves.
static bool DragPathForRow(int idx, std::wstring& out) {
    if (idx < 0 || idx >= (int)g_results.size()) return false;
    const ResultItem& r = g_results[(size_t)idx];
    if (r.action.empty()) return false;
    if (r.kind == ResultKind::File || r.kind == ResultKind::Folder) {
        out = r.action;
        return true;
    }
    if (r.kind == ResultKind::App && !r.isStore) {
        out = r.action;
        return true;
    }
    return false;
}

static void ArmRowDrag(int resultsIdx, POINT pt) {
    CancelRowDrag();
    std::wstring dummy;
    if (!DragPathForRow(resultsIdx, dummy)) return;
    g_dragArmed = true;
    g_dragPt = pt;
    g_dragRow = resultsIdx;
}

static void MoveRowDrag(HWND, POINT pt) {
    if (!g_dragArmed) return;
    if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) { CancelRowDrag(); return; }
    int dx = GetSystemMetrics(SM_CXDRAG), dy = GetSystemMetrics(SM_CYDRAG);
    int mx = pt.x - g_dragPt.x; if (mx < 0) mx = -mx;
    int my = pt.y - g_dragPt.y; if (my < 0) my = -my;
    if (mx < dx && my < dy) return;
    int row = g_dragRow;
    CancelRowDrag();
    std::wstring path;
    if (!DragPathForRow(row, path)) return;
    EnsureOle(); // belt-and-braces: ShowPalette already called it
    if (!g_oleInit) return;
    PreferredEffectFormat(); // register up front: targets may query before enumerating
    HdropDataObject* data = new (std::nothrow) HdropDataObject(path);
    DropSource* src = new (std::nothrow) DropSource();
    if (!data || !src) {
        if (data) data->Release();
        if (src) src->Release();
        return;
    }
    g_dragging = true; // modal loop activates other apps: don't dismiss
    DWORD eff = 0;
    DoDragDrop(data, src, DROPEFFECT_COPY, &eff);
    g_dragging = false;
    data->Release();
    src->Release();
}

static void CancelRowDrag() {
    g_dragArmed = false;
    g_dragRow = -1;
}

// Keyboard Menu key (VK_APPS / Shift+F10): act on the current selection like
// Explorer, never on hover, and never the edit box's own Cut/Copy/Paste
// menu (focus lives in the edit box, so the key is forwarded here from the
// edit subclass). Shared by the palette WM_CONTEXTMENU keyboard branch.
static void KeyboardContextMenu() {
    int sel = CurrentSelection();
    if (sel >= 0 && sel < (int)g_results.size() &&
        g_results[(size_t)sel].kind != ResultKind::Header) {
        POINT rp = RowScreenPoint(sel);
        ShowRowMenu(sel, rp.x, rp.y);
    } else {
        POINT pt{};
        GetCursorPos(&pt);
        ShowPaletteMenu(pt.x, pt.y);
    }
}

// ------------------------------------------------------------- palette wndproc
static LRESULT CALLBACK PaletteProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        // WS_CLIPSIBLINGS everywhere: the edit must never paint over the chip.
        // The edit is multiline word-wrap (no ES_AUTOHSCROLL): overflow wraps
        // to a second line and the input area grows (capped, then scrolls).
        // The listbox is OWNERDRAWVARIABLE so section headers stay slim.
        g_hwndEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_LEFT | ES_MULTILINE |
            ES_AUTOVSCROLL,
            kPadX, Dpx(9), kWidth - kPadX * 2, kInputH - Dpx(14),
            hwnd, (HMENU)1, g_hInst, NULL);
        SendMessageW(g_hwndEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
        if (g_fontInput) // fonts are lazy now: applied in EnsureAppFonts on first show
            SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)g_fontInput, TRUE);
        SendMessageW(g_hwndEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Dpx(10), Dpx(10)));
        SetWindowSubclass(g_hwndEdit, EditSubclass, 1, 0);
        // Belt-and-braces: wrapping needs exactly MULTILINE + AUTOVSCROLL
        // with no horizontal scroll mode. Reassert after creation so the
        // wrap path can never silently revert to a scrolling single line.
        {
            LONG_PTR st = GetWindowLongPtrW(g_hwndEdit, GWL_STYLE);
            st |= ES_MULTILINE | ES_AUTOVSCROLL;
            st &= ~(LONG_PTR)(ES_AUTOHSCROLL | WS_HSCROLL | WS_VSCROLL);
            SetWindowLongPtrW(g_hwndEdit, GWL_STYLE, st);
            SetWindowPos(g_hwndEdit, NULL, 0, 0, 0, 0,
                         SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        }

        g_hwndChip = CreateWindowExW(0, kChipClass, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            kPadX, Dpx(9), 10, kChipH,
            hwnd, (HMENU)3, g_hInst, NULL);

        g_hwndChipRow = CreateWindowExW(0, kChipRowClass, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            kPadX, kInputH, kWidth - kPadX * 2, kChipRowH,
            hwnd, (HMENU)4, g_hInst, NULL);

        g_hwndFileBar = CreateWindowExW(0, kFileBarClass, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            kPadX, kInputH, kWidth - kPadX * 2, kBottomBarH,
            hwnd, (HMENU)5, g_hInst, NULL);

        g_hwndFileGrid = CreateWindowExW(0, kFileGridClass, L"",
            WS_CHILD | WS_CLIPSIBLINGS,
            kPadX, kInputH, kWidth - kPadX * 2, kItemH,
            hwnd, (HMENU)6, g_hInst, NULL);

        g_hwndList = CreateWindowExW(0, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LBS_NOTIFY | LBS_OWNERDRAWVARIABLE |
            LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
            kPadX, kInputH, kWidth - kPadX * 2, kItemH,
            hwnd, (HMENU)2, g_hInst, NULL);
        SetWindowSubclass(g_hwndList, ListSubclass, 2, 0);
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
            g_navigating = false; // real typing restarts arrow cycling
            g_confirmPower = -1; // and disarms any pending power confirm
            // Bang-chip trigger: "alias<space>" with nothing else typed.
            // File trigger (":<space>") is reserved and wins over bangs.
            if (g_activeBang < 0 && !InFileMode()) {
                std::wstring editRaw = GetEditText(g_hwndEdit);
                size_t sp = editRaw.find(L' ');
                size_t tb = editRaw.find(L'\t');
                size_t p = sp;
                if (p == std::wstring::npos || (tb != std::wstring::npos && tb < p)) p = tb;
                if (p != std::wstring::npos) {
                    if (IsFileTrigger(editRaw)) {
                        std::wstring rest = Trim(editRaw.substr(p + 1));
                        ActivateFileMode(L""); // drive list; keeps any pasted filter
                        if (!rest.empty()) {
                            g_changingEdit = true;
                            SetWindowTextW(g_hwndEdit, rest.c_str());
                            SendMessageW(g_hwndEdit, EM_SETSEL,
                                         (WPARAM)rest.size(), (LPARAM)rest.size());
                            g_changingEdit = false;
                            UpdateResults();
                        }
                        break;
                    }
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
                // Only a row double-click acts: blank-space double-clicks
                // do nothing (they must never fire the cursor's row). The
                // hit row becomes the cursor first.
                DWORD pos = GetMessagePos();
                POINT p{(int)(short)LOWORD(pos), (int)(short)HIWORD(pos)};
                ScreenToClient(g_hwndList, &p);
                int row = ListRowFromPoint(g_hwndList, MAKELPARAM(p.x, p.y));
                if (row < 0 || g_results[(size_t)row].kind == ResultKind::Header)
                    break;
                g_chipsFocus = false;
                SendMessageW(g_hwndList, LB_SETCURSEL, row, 0);
                ExecuteSelected(false);
            } else if (code == LBN_SELCHANGE) {
                g_confirmPower = -1;
                if (g_chipsFocus) {
                    g_chipsFocus = false; // mouse took over the list
                    InvalidateRect(g_hwndChipRow, NULL, FALSE);
                } else if (!g_chips.empty()) {
                    InvalidateRect(g_hwndChipRow, NULL, FALSE);
                }
            }
        } else if (id >= ID_TRAY_SETTINGS && id <= ID_TRAY_AWAKE) {
            OnTrayCommand(id);
        }
        break;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)l;
        if (mi->CtlType == ODT_MENU) {
            // Dark popup menu (tray + context): separators slim, verbs compact.
            size_t ix = (size_t)mi->itemData;
            bool sep = ix < g_menuItems.size() && g_menuItems[ix].separator;
            mi->itemHeight = (UINT)(sep ? Dpx(9) : Dpx(30));
            mi->itemWidth = (UINT)g_menuW;
            return TRUE;
        }
        // Items are added in g_results order, so the listbox index matches
        // the results index at measure time. Headers stay slim.
        size_t id = (mi->itemID == (UINT)-1) ? (size_t)-1 : (size_t)mi->itemID;
        mi->itemHeight = (id < g_results.size() &&
                          g_results[id].kind == ResultKind::Header) ? kHeadH : kItemH;
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)l;
        if (d->CtlType == ODT_MENU) { DrawDarkMenuItem(d); return TRUE; }
        if (d->CtlID != 2) break;
        bool sel = (d->itemState & ODS_SELECTED) != 0;
        HDC dc = d->hDC;
        size_t ri = (d->itemData < (UINT)g_results.size()) ? d->itemData : 9999;
        bool isHead = ((int)ri < (int)g_results.size()) &&
                      g_results[ri].kind == ResultKind::Header;
        FillRect(dc, &d->rcItem, (!isHead && sel) ? g_brSel : g_brBg);
        // Hover preview: a half-step between background and highlight.
        // Visual only — selection and the multi-set are untouched.
        if (!isHead && !sel && (int)ri == g_hoverRow)
            PaintHoverFill(dc, &d->rcItem);
        if ((int)ri < (int)g_results.size()) {
            const ResultItem& r = g_results[ri];
            SetBkMode(dc, TRANSPARENT);
            if (isHead) { // section label: small gray, never highlights
                SelectObject(dc, g_fontSub);
                SetTextColor(dc, kSub);
                RECT tr = d->rcItem; tr.left += 14; tr.right -= Dpx(10);
                DrawTextW(dc, r.title.c_str(), -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                return TRUE;
            }
            // App rows keep the original v1 look (slim category bar);
            // every other kind paints its glyph.
            RECT tr = d->rcItem;
            if (r.kind == ResultKind::App) {
                HBRUSH bar = CreateSolidBrush(sel ? kAppCol : Darken(kAppCol, 65));
                RECT br = d->rcItem;
                br.left += Dpx(14); br.right = br.left + Dpx(2);
                br.top += Dpx(9); br.bottom -= Dpx(9);
                FillRect(dc, &br, bar);
                DeleteObject(bar);
                tr.left += Dpx(30);
            } else {
                FileGlyph fg = GlyphForResult(r.kind, r.title, r.action, r.color);
                int isz = Dpx(20);
                int icy = (d->rcItem.top + d->rcItem.bottom) / 2;
                RECT cell{d->rcItem.left + Dpx(14), icy - isz / 2,
                          d->rcItem.left + Dpx(14) + isz, icy + isz / 2};
                PaintGlyph(dc, cell, fg, sel, r.title, g_fontChip);
                tr.left += Dpx(44);
            }
            tr.right -= Dpx(10);
            SetTextColor(dc, sel ? kText : kRowText);
            SelectObject(dc, g_fontTitle);
            DrawTextW(dc, r.title.c_str(), -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        return TRUE;
    }
    case WM_ACTIVATE:
        // Click-outside-to-dismiss. Child focus keeps the top-level active,
        // so typing never hides. Settings is exempt. Open menus and modal
        // drag loops own their activation: never dismiss under them.
        if (g_inMenu || g_dragging) break;
        if (LOWORD(w) == WA_INACTIVE) {
            HWND now = (HWND)l;
            if (now != g_hwndSettings && !IsChild(g_hwndSettings, now))
                HidePalette();
        }
        break;
    case WM_ACTIVATEAPP:
        // App switch / click onto another app always dismisses.
        if (!w && IsWindowVisible(hwnd) && !g_inMenu && !g_dragging)
            HidePalette();
        break;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        UpdateLayoutMetrics();
        InvalidateChipCaches(); // cached chip rects + hint width are DPI-sized
        if (g_appFontsReady) { CreateAppFonts(); RefreshEditLineH(); } // lazy: nothing pre-first-show
        RECT* pr = (RECT*)l; // system-suggested rect for the new DPI
        SetWindowPos(hwnd, HWND_TOPMOST, pr->left, pr->top,
                     pr->right - pr->left, pr->bottom - pr->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(g_hwndEdit, WM_SETFONT, (WPARAM)g_fontInput, TRUE);
        if (g_hwndSettings) {
            ApplyUIFont(g_hwndSettings);
            InvalidateRect(g_hwndSettings, NULL, TRUE);
        }
        LayoutPaletteChildren();
        ApplyRoundCorners();
        if (g_activeBang >= 0) // re-measure the armed chip, keep the query
            ActivateBang(g_activeBang, GetEditText(g_hwndEdit));
        else if (InFileMode()) { // re-measure the Files chip, keep dir + filter
            std::wstring keep = GetEditText(g_hwndEdit);
            PositionFileChip();
            g_changingEdit = true;
            SetWindowTextW(g_hwndEdit, keep.c_str());
            SendMessageW(g_hwndEdit, EM_SETSEL, (WPARAM)keep.size(), (LPARAM)keep.size());
            g_changingEdit = false;
            UpdateResults();
        } else
            UpdateResults();
        break;
    }
    case WM_HOTKEY:
        if (g_hexOpen) return 0; // color-picker modal: don't toggle the palette
        if (w == HOTKEY_ID) {
            if (IsWindowVisible(hwnd)) HidePalette();
            else ShowPalette();
        }
        break;
    case WM_APP_SHOW:
        ShowPalette();
        break;
    case WM_CONTEXTMENU: {
        // Right-click anywhere on the palette: row menu on a result,
        // palette menu on headers and empty areas. (The edit box shows its
        // native Cut/Copy/Paste menu via its own proc and never reaches here.)
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (pt.x == -1 && pt.y == -1) {
            KeyboardContextMenu(); // Menu key: selection menu, not hover
            return 0;
        }
        POINT cl = pt;
        ScreenToClient(hwnd, &cl);
        int row = -1;
        if (GridActive() && g_hwndFileGrid && IsWindowVisible(g_hwndFileGrid)) {
            RECT gr; GetWindowRect(g_hwndFileGrid, &gr);
            MapWindowPoints(NULL, hwnd, (POINT*)&gr, 2);
            if (PtInRect(&gr, cl))
                row = GridHitTest(cl.x - gr.left, cl.y - gr.top);
        }
        if (row < 0 && g_hwndList && IsWindowVisible(g_hwndList)) {
            RECT lr; GetWindowRect(g_hwndList, &lr);
            MapWindowPoints(NULL, hwnd, (POINT*)&lr, 2);
            if (PtInRect(&lr, cl)) {
                LPARAM lp = MAKELPARAM(cl.x - lr.left, cl.y - lr.top);
                LRESULT hit = SendMessageW(g_hwndList, LB_ITEMFROMPOINT, 0, lp);
                if (HIWORD(hit) == 0) {
                    int r = LOWORD(hit); // listbox index == results index
                    if (r >= 0 && r < (int)g_results.size())
                        row = r;
                }
            }
        }
        if (row >= 0 && g_results[(size_t)row].kind != ResultKind::Header)
            ShowRowMenu(row, pt.x, pt.y);
        else
            ShowPaletteMenu(pt.x, pt.y);
        return 0;
    }
    case WM_APP_INDEX: { // background index published: adopt on the UI thread
        if (g_quitting.load()) { // shutting down: drop any in-flight index
            delete g_pendingIndex.exchange(nullptr);
            break;
        }
        auto* p = g_pendingIndex.exchange(nullptr);
        if (p) {
            g_apps.swap(*p);
            delete p;
            g_indexReady.store(true);
            SaveAppCache(AppCachePath(), g_apps); // persist for instant restart
        }
        RefreshAppsListPreservingChecks();
        if (IsWindowVisible(hwnd)) UpdateResults();
        break;
    }
    case WM_TIMER:
        if (w == IDT_DEFERRED_REFRESH) {
            KillTimer(hwnd, IDT_DEFERRED_REFRESH);
            g_startupDeferredArmed = false;
            // Single light refresh after boot settles. Skipped if the user
            // already triggered a scan (hotkey cache-miss / manual refresh).
            if (!g_quitting.load() && !g_indexing.load())
                BuildIndexAsync(false);
            break;
        }
        break;
    case WM_TRAYICON:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU)
            ShowTrayMenu(hwnd);
        else if (LOWORD(l) == WM_LBUTTONDBLCLK)
            ShowPalette();
        else if (LOWORD(l) == NIN_BALLOONUSERCLICK)
            ShowPalette(); // retry after a launch-failure balloon
        break;
    case WM_DESTROY:
        if (g_dropTarget) { // revoke while the window still exists
            RevokeDragDrop(hwnd);
            g_dropTarget->Release();
            g_dropTarget = nullptr;
        }
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ------------------------------------------------------------- tray
// Failure balloon: palette stays hidden (fast), the error surfaces here.
// Clicking the balloon reopens the palette for a retry.
static void NotifyLaunchFailed(const wchar_t* msg) {
    if (!g_hwndPalette) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwndPalette;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    wcsncpy_s(nid.szInfo, msg, _TRUNCATE);
    wcsncpy_s(nid.szInfoTitle, L"SneekPeek", _TRUNCATE);
    nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
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
    // Same dark owner-drawn language as the in-palette menus (the stock
    // system menu was light and off-theme). Identifiers stay ID_TRAY_* so
    // OnTrayCommand dispatch is unchanged.
    EnsureAppFonts(); // tray can open before the palette ever shows
    DarkMenuItem st{L"Settings...", kSetCol, ID_TRAY_SETTINGS};
    DarkMenuItem rf{L"Refresh apps", kAppCol, ID_TRAY_REFRESH};
    DarkMenuItem aw{L"Keep screen awake", kAwakeCol, ID_TRAY_AWAKE};
    aw.checked = g_awakeOn;
    DarkMenuItem su{L"Run at startup", kCalcCol, ID_TRAY_STARTUP};
    su.checked = g_settings.runAtStartup;
    DarkMenuItem sep; sep.separator = true;
    DarkMenuItem qu{L"Quit", kDanger, ID_TRAY_QUIT};
    g_menuRow = -1;
    g_menuItems.clear();
    g_menuItems.push_back(std::move(st));
    g_menuItems.push_back(std::move(rf));
    g_menuItems.push_back(std::move(aw));
    g_menuItems.push_back(std::move(su));
    g_menuItems.push_back(std::move(sep));
    g_menuItems.push_back(std::move(qu));
    SizeMenuItems();
    HMENU menu = BuildMenuPopup(0, g_menuItems.size());
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(hwnd); // required for the menu to dismiss correctly
    g_inMenu = true;
    int cmd = (int)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON |
                                    TPM_BOTTOMALIGN, pt.x, pt.y, hwnd, NULL);
    g_inMenu = false;
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd >= (int)ID_TRAY_SETTINGS && cmd <= (int)ID_TRAY_AWAKE)
        OnTrayCommand((UINT)cmd);
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
// Delete the selected table row (vector + listview) and persist.
static void DeleteSelectedTableRow(HWND lv) {
    int row = LVSelectedRow(lv);
    if (row < 0) return;
    if (lv == g_hwndBangsList && row < (int)g_settings.bangs.size())
        g_settings.bangs.erase(g_settings.bangs.begin() + row);
    else if (lv == g_hwndQuickList && row < (int)g_settings.quicks.size())
        g_settings.quicks.erase(g_settings.quicks.begin() + row);
    else return;
    ListView_DeleteItem(lv, row);
    SaveNow();
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
        ListView_SetCheckState(g_hwndAppsList, row, IsHiddenAppLower(g_apps[i].lower));
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
    const std::vector<HWND>* pages[6] = {&g_pageGeneral, &g_pageApps, &g_pageBangs, &g_pageQuick,
                                         &g_pageFiles, &g_pageTheme};
    for (int i = 0; i < 6; i++)
        for (HWND h : *pages[i])
            ShowWindow(h, (i == page) ? SW_SHOW : SW_HIDE);
}

static HWND NewCtrl(const wchar_t* cls, const wchar_t* text, DWORD style,
                    int x, int y, int w, int h, HWND parent, UINT id) {
    DWORD st = WS_CHILD | style;
    // All buttons owner-drawn (dark theme).
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
    if (FnDwmSetWindowAttribute f = DwmSetWindowAttribute()) {
        BOOL dark = TRUE;
        f(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    }
}

static void OpenSettings() {
    if (g_hwndSettings) { ShowWindow(g_hwndSettings, SW_SHOW); SetForegroundWindow(g_hwndSettings); return; }
    EnsureSettingsLists();
    EnsureAppFonts();
    EnsureComCtl(); // listview/tab classes needed below; deferred past boot
    // Normal window, not tool-palette owned: unowned so it takes a regular
    // z-position (minimizable, goes behind other windows). The topmost
    // palette would cover it, so it hides first (hotkey brings it back).
    if (g_hwndPalette && IsWindowVisible(g_hwndPalette)) HidePalette();
    int W = 600, H = 590;
    POINT pt{}; GetCursorPos(&pt); // open on the monitor being used
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    int mw = mi.rcWork.right - mi.rcWork.left;
    int mh = mi.rcWork.bottom - mi.rcWork.top;
    int x = mi.rcWork.left + (mw - W) / 2;
    int y = mi.rcWork.top + (mh - H) / 2;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    g_hwndSettings = CreateWindowExW(0, kSettingsClass, L"SneekPeek Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, W, H, NULL, NULL, g_hInst, NULL);
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
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
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

// Persist now; on failure tell the user, since these are all actions where
// they expect the change to survive a restart.
static void SaveOrWarn(const wchar_t* what) {
    if (SaveSettings(g_settings)) return;
    std::wstring msg = std::wstring(L"Couldn't save settings (") + what +
                       L").\nThe change applies now but may be lost on restart.";
    MessageBoxW(g_hwndPalette, msg.c_str(), L"SneekPeek",
                MB_OK | MB_ICONWARNING);
}

// Every change persists immediately: there is no Save button.
static void SaveNow() {
    EnsureSettingsLists(); // never persist defaults over real lists
    // A failed write must not claim success in the UI. g_settings stays the
    // source of truth, so the app keeps working; only persistence is lost.
    bool ok = SaveSettings(g_settings);
    RebuildHiddenSet();
    if (g_hwndSavedLabel) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t t[64];
        if (ok)
            swprintf_s(t, 64, L"Saved %02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        else
            swprintf_s(t, 64, L"Save failed %02d:%02d:%02d",
                       st.wHour, st.wMinute, st.wSecond);
        SetWindowTextW(g_hwndSavedLabel, t);
    }
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
    if (esel == 4) {
        std::wstring probe = eurl;
        size_t pp = probe.find(L"%s");
        if (pp != std::wstring::npos) probe.replace(pp, 2, L"test");
        if (!IsSafeWebUrl(probe)) {
            MessageBoxW(owner, L"Custom URL must be an https:// (or http://) address.",
                        L"SneekPeek", MB_ICONWARNING | MB_OK);
            SetWindowTextW(g_hwndEngineUrl, g_settings.engineUrl.c_str());
            return;
        }
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
        {
            std::wstring probe = b.url;
            size_t pp = probe.find(L"%s");
            if (pp != std::wstring::npos) probe.replace(pp, 2, L"test");
            if (!IsSafeWebUrl(probe)) {
                MessageBoxW(g_hwndSettings,
                            L"Bang URL must be https:// (or http://) - change reverted.",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
                return;
            }
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
        if (q.name.empty() || !IsSafeWebUrl(q.url)) {
            MessageBoxW(g_hwndSettings,
                        L"Need a name and an https:// (or http://) URL - change reverted.",
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

// Hex color editor for one theme field: a small modal dialog with a #RRGGBB
// box + live preview chip. Applies + saves instantly on OK, repaints live.
// (Replaces the system color dialog: hex-only, per the theme-file format.)
static const wchar_t* kHexDlgClass = L"SneekPeekHexEdit";
static bool g_hexAccepted = false;
// g_hexOpen declared above; modal loop guard (never PostQuitMessage here:
static COLORREF g_hexValue = RGB(0, 0, 0); // that would quit the whole app)
static HWND g_hexEdit = NULL;
static bool g_hexClassReady = false;

static LRESULT CALLBACK HexProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: {
        HDC dc = (HDC)w;
        RECT r; GetClientRect(hwnd, &r);
        FillRect(dc, &r, g_brBg);
        return 1;
    }
    case WM_PAINT: {
        // Live preview chip: the currently-typed color, dark when invalid.
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT chip{200, 36, 288, 60};
        wchar_t buf[32] = {0};
        if (g_hexEdit) GetWindowTextW(g_hexEdit, buf, 32);
        COLORREF c;
        bool ok = ParseColor(buf, c);
        HBRUSH b = CreateSolidBrush(ok ? c : kBg);
        HGDIOBJ oldB = SelectObject(dc, b);
        HPEN pen = CreatePen(PS_SOLID, 1, ok ? kBtnEdge : kDanger);
        HGDIOBJ oldP = SelectObject(dc, pen);
        RoundRect(dc, chip.left, chip.top, chip.right, chip.bottom, 6, 6);
        SelectObject(dc, oldB);
        SelectObject(dc, oldP);
        DeleteObject(b);
        DeleteObject(pen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        WORD id = LOWORD(w), code = HIWORD(w);
        if (id == 100 && code == EN_CHANGE) { // typing: refresh the preview
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (id == IDOK) {
            wchar_t buf[32] = {0};
            if (g_hexEdit) GetWindowTextW(g_hexEdit, buf, 32);
            COLORREF c;
            if (!ParseColor(buf, c)) {
                MessageBoxW(hwnd, L"Enter a hex color like #1C1C1E (6 hex digits).",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
                if (g_hexEdit) SetFocus(g_hexEdit);
                break;
            }
            g_hexValue = c;
            g_hexAccepted = true;
            DestroyWindow(hwnd);
        } else if (id == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        g_hexEdit = NULL;
        g_hexOpen = false; // ends the modal loop below
        break;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

static void PickThemeColor(HWND owner, int fieldIdx) {
    if (fieldIdx < 0 || fieldIdx >= ThemeFieldCount()) return;
    if (!g_hexClassReady) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = g_hInst;
        wc.lpszClassName = kHexDlgClass;
        wc.lpfnWndProc = HexProc;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = g_brBg;
        if (!RegisterClassExW(&wc)) return;
        g_hexClassReady = true;
    }
    COLORREF cur = g_settings.theme.*(kThemeFields[fieldIdx].member);
    g_hexAccepted = false;
    g_hexValue = cur;
    RECT orc; GetWindowRect(owner, &orc);
    int W = 312, H = 128;
    int x = orc.left + ((orc.right - orc.left) - W) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - H) / 2;
    HWND dlg = CreateWindowExW(0, kHexDlgClass, kThemeFields[fieldIdx].label,
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        x, y, W, H, owner, NULL, g_hInst, NULL);
    if (!dlg) return;
    HWND lab = CreateWindowW(L"STATIC", L"Hex color (#RRGGBB):",
        WS_CHILD | WS_VISIBLE, 12, 12, 276, 18, dlg, NULL, g_hInst, NULL);
    g_hexEdit = CreateWindowW(L"EDIT", SerializeColor(cur).c_str(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        12, 36, 180, 24, dlg, (HMENU)100, g_hInst, NULL);
    HWND ok = CreateWindowW(L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        120, 70, 84, 26, dlg, (HMENU)IDOK, g_hInst, NULL);
    HWND cancel = CreateWindowW(L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE, 212, 70, 84, 26, dlg, (HMENU)IDCANCEL, g_hInst, NULL);
    if (g_fontUI) {
        SendMessageW(lab, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
        SendMessageW(g_hexEdit, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
        SendMessageW(ok, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
        SendMessageW(cancel, WM_SETFONT, (WPARAM)g_fontUI, FALSE);
    }
    SendMessageW(g_hexEdit, EM_SETSEL, 0, -1);
    DarkTitleBar(dlg);
    EnableWindow(owner, FALSE); // modal to Settings
    ShowWindow(dlg, SW_SHOW);
    SetFocus(g_hexEdit);
    g_hexOpen = true;
    MSG msg;
    while (g_hexOpen && GetMessageW(&msg, NULL, 0, 0) > 0) {
        // Enter/Esc in the box map to OK/Cancel (plain windows don't do
        // this on their own); everything else gets dialog-manager keys.
        if (msg.message == WM_KEYDOWN && msg.hwnd == g_hexEdit) {
            if (msg.wParam == VK_RETURN) {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
                continue;
            }
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
                continue;
            }
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (!g_hexAccepted) return; // cancelled or invalid: nothing changes
    g_settings.theme.*(kThemeFields[fieldIdx].member) = g_hexValue;
    SaveNow(); // instant save, like the rest of Settings
    ApplyThemeFromSettings(); // live repaint: palette + Settings tables
    if (g_themeBtn[fieldIdx] && IsWindow(g_themeBtn[fieldIdx]))
        InvalidateRect(g_themeBtn[fieldIdx], NULL, FALSE);
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_fillingSettings = true;
        g_pageGeneral.clear(); g_pageApps.clear();
        g_pageBangs.clear(); g_pageQuick.clear(); g_pageFiles.clear();
        g_pageTheme.clear();
        for (int i = 0; i < 32; i++) g_themeBtn[i] = NULL;

        // Page tabs: owner-drawn buttons matching the app theme (no tab
        // control, so there is no light-themed rim anywhere in Settings).
        const wchar_t* tabs[] = {L"General", L"Hidden apps", L"Bangs", L"Quick links",
                                 L"Files", L"Theme"};
        const int tabX[] = {16, 110, 224, 308, 412, 496};
        const int tabW[] = {88, 108, 78, 98, 78, 78};
        for (int i = 0; i < 6; i++)
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
                WS_VISIBLE, 24, 214, 536, 44, hwnd, 0));
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
                L"Add appends an editable row - double-click a cell to edit - Del/Remove deletes:",
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
            page(g_pageBangs, NewCtrl(L"BUTTON", L"Remove",
                WS_VISIBLE, 134, 448, 100, 26, hwnd, IDC_BANG_DEL));
        }

        // ---- Quick links page (same in-place pattern, no query)
        {
            page(g_pageQuick, NewCtrl(L"STATIC",
                L"Type the name in the palette - Enter opens the fixed URL - Del/Remove deletes:",
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
            page(g_pageQuick, NewCtrl(L"BUTTON", L"Remove",
                WS_VISIBLE, 134, 448, 100, 26, hwnd, IDC_QUICK_DEL));
        }

        // ---- Files page (file browser presentation: view + grid size).
        // File mode only: list view and all other searches are unaffected.
        // Everything applies instantly, like the rest of Settings.
        {
            page(g_pageFiles, NewCtrl(L"STATIC", L"View:",
                WS_VISIBLE, 24, 52, 200, 20, hwnd, 0));
            page(g_pageFiles, NewCtrl(L"BUTTON", L"List",
                WS_VISIBLE, 24, 74, 120, 28, hwnd, IDC_VIEW_LIST));
            page(g_pageFiles, NewCtrl(L"BUTTON", L"Grid",
                WS_VISIBLE, 154, 74, 120, 28, hwnd, IDC_VIEW_GRID));
            page(g_pageFiles, NewCtrl(L"STATIC", L"Grid rows:",
                WS_VISIBLE, 24, 114, 200, 20, hwnd, 0));
            page(g_pageFiles, NewCtrl(L"BUTTON", L"-",
                WS_VISIBLE, 24, 136, 48, 26, hwnd, IDC_ROWS_DEC));
            g_hwndRowsVal = page(g_pageFiles, NewCtrl(L"STATIC", L"3",
                WS_VISIBLE | SS_CENTER, 78, 136, 60, 26, hwnd, 0));
            page(g_pageFiles, NewCtrl(L"BUTTON", L"+",
                WS_VISIBLE, 144, 136, 48, 26, hwnd, IDC_ROWS_INC));
            page(g_pageFiles, NewCtrl(L"STATIC", L"Grid columns:",
                WS_VISIBLE, 220, 114, 200, 20, hwnd, 0));
            page(g_pageFiles, NewCtrl(L"BUTTON", L"-",
                WS_VISIBLE, 220, 136, 48, 26, hwnd, IDC_COLS_DEC));
            g_hwndColsVal = page(g_pageFiles, NewCtrl(L"STATIC", L"5",
                WS_VISIBLE | SS_CENTER, 274, 136, 60, 26, hwnd, 0));
            page(g_pageFiles, NewCtrl(L"BUTTON", L"+",
                WS_VISIBLE, 340, 136, 48, 26, hwnd, IDC_COLS_INC));
            page(g_pageFiles, NewCtrl(L"STATIC",
                L"File browser grid only. List view and other searches are unaffected.\r\n"
                L"Changes apply instantly.",
                WS_VISIBLE, 24, 172, 536, 44, hwnd, 0));
            wchar_t rv[16], cv[16];
            swprintf_s(rv, 16, L"%d", g_settings.gridRows);
            swprintf_s(cv, 16, L"%d", g_settings.gridCols);
            SetWindowTextW(g_hwndRowsVal, rv);
            SetWindowTextW(g_hwndColsVal, cv);
        }

        // ---- Theme page (every UI color editable, applies instantly).
        // 3 columns x 10 rows of swatches, then file actions. Hex clicks
        // open a #RRGGBB box; Edit file round-trips through theme.ini.
        {
            page(g_pageTheme, NewCtrl(L"STATIC",
                L"Click a color to type a new hex value - everything applies instantly:",
                WS_VISIBLE, 24, 52, 536, 20, hwnd, 0));
            for (int i = 0; i < ThemeFieldCount(); i++) {
                int col = i % 3, row = i / 3;
                g_themeBtn[i] = page(g_pageTheme,
                    NewCtrl(L"BUTTON", kThemeFields[i].label,
                            WS_VISIBLE | WS_TABSTOP,
                            24 + col * 184, 74 + row * 31, 176, 27, hwnd,
                            (UINT)(IDC_THEME_BG + i)));
            }
            page(g_pageTheme, NewCtrl(L"BUTTON", L"Reset",
                WS_VISIBLE, 24, 394, 110, 28, hwnd, IDC_THEME_RESET));
            page(g_pageTheme, NewCtrl(L"BUTTON", L"Edit file",
                WS_VISIBLE, 140, 394, 110, 28, hwnd, IDC_THEME_EDITFILE));
            page(g_pageTheme, NewCtrl(L"BUTTON", L"Reload file",
                WS_VISIBLE, 256, 394, 110, 28, hwnd, IDC_THEME_RELOAD));
            page(g_pageTheme, NewCtrl(L"STATIC",
                L"Edit file exports the live theme to theme.ini and opens it;\r\n"
                L"Reload file applies your edits.",
                WS_VISIBLE, 376, 392, 200, 40, hwnd, 0));
        }

        // Shared bottom buttons (always visible).
        NewCtrl(L"BUTTON", L"Rescan apps", WS_VISIBLE,
                16, 512, 120, 30, hwnd, IDC_RESCAN);
        NewCtrl(L"BUTTON", L"Close", WS_VISIBLE,
                146, 512, 100, 30, hwnd, IDC_CLOSE);
        g_hwndSavedLabel = NewCtrl(L"STATIC", L"", WS_VISIBLE | SS_RIGHT,
                380, 518, 188, 20, hwnd, 0);

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
            HBRUSH bg = CreateSolidBrush(sel ? kBtnSel : kBg);
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
                HPEN pen = CreatePen(PS_SOLID, 1, kBtnEdge);
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
        // Theme swatches: live color chip + field name (not the generic
        // button paint, which would hide the color being edited).
        if (d->CtlID >= (UINT)IDC_THEME_BG &&
            (int)(d->CtlID - (UINT)IDC_THEME_BG) < ThemeFieldCount()) {
            int fi = (int)(d->CtlID - (UINT)IDC_THEME_BG);
            FillRect(dc, &d->rcItem, g_brBg);
            COLORREF col = g_settings.theme.*(kThemeFields[fi].member);
            RECT chip = d->rcItem;
            chip.left += Dpx(8); chip.right = chip.left + Dpx(28);
            chip.top += Dpx(5); chip.bottom -= Dpx(5);
            if (chip.right > chip.left && chip.bottom > chip.top) {
                HBRUSH cb = CreateSolidBrush(col);
                HGDIOBJ oldB = SelectObject(dc, cb);
                HPEN pen = CreatePen(PS_SOLID, 1, kBtnEdge);
                HGDIOBJ oldP = SelectObject(dc, pen);
                RoundRect(dc, chip.left, chip.top, chip.right, chip.bottom, 6, 6);
                SelectObject(dc, oldB);
                SelectObject(dc, oldP);
                DeleteObject(cb);
                DeleteObject(pen);
            }
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, g_fontUI);
            SetTextColor(dc, kText);
            RECT tr = d->rcItem; tr.left += Dpx(8) + Dpx(28) + Dpx(10);
            tr.right -= Dpx(6);
            DrawTextW(dc, kThemeFields[fi].label, -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
            bool pressed = (d->itemState & ODS_SELECTED) != 0;
            bool focus = (d->itemState & ODS_FOCUS) != 0;
            if (pressed || focus) {
                HPEN fp = CreatePen(PS_SOLID, 1, kAppCol);
                HGDIOBJ ofp = SelectObject(dc, fp);
                SelectObject(dc, GetStockObject(NULL_BRUSH));
                RoundRect(dc, d->rcItem.left, d->rcItem.top,
                          d->rcItem.right, d->rcItem.bottom, 8, 8);
                SelectObject(dc, ofp);
                DeleteObject(fp);
            }
            return TRUE;
        }
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_fontUI);
        bool pressed = (d->itemState & ODS_SELECTED) != 0;
        bool focus = (d->itemState & ODS_FOCUS) != 0;
        bool isPage = d->CtlID >= (UINT)IDC_PAGE_0 && d->CtlID <= (UINT)IDC_PAGE_5;
        bool pageSel = isPage && (int)(d->CtlID - (UINT)IDC_PAGE_0) == g_settingsPage;
        bool isView = d->CtlID == (UINT)IDC_VIEW_LIST || d->CtlID == (UINT)IDC_VIEW_GRID;
        bool viewSel = isView && ((d->CtlID == (UINT)IDC_VIEW_GRID) == g_settings.fileGrid);
        bool accent = pageSel || viewSel;
        FillRect(dc, &d->rcItem, g_brBg);
        COLORREF fill = accent ? (pressed ? kBtnSelHot : kBtnSel)
                               : (pressed ? kBtnFillHot : kBtnFill);
        COLORREF edge = accent ? kAppCol
                               : (focus ? kEdgeHot : kBtnEdge);
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
        if (id >= IDC_PAGE_0 && id <= IDC_PAGE_5) {
            ShowSettingsPage(id - IDC_PAGE_0);
            for (int i = IDC_PAGE_0; i <= IDC_PAGE_5; i++) {
                HWND b = GetDlgItem(hwnd, i);
                if (b) InvalidateRect(b, NULL, FALSE);
            }
        } else if (id == IDC_VIEW_LIST || id == IDC_VIEW_GRID) {
            g_settings.fileGrid = (id == IDC_VIEW_GRID);
            SaveNow();
            HWND bl = GetDlgItem(hwnd, IDC_VIEW_LIST);
            HWND bg = GetDlgItem(hwnd, IDC_VIEW_GRID);
            if (bl) InvalidateRect(bl, NULL, FALSE);
            if (bg) InvalidateRect(bg, NULL, FALSE);
            if (g_hwndPalette && IsWindowVisible(g_hwndPalette) && InFileMode())
                UpdateResults(); // re-layout list <-> grid live
        } else if (id == IDC_ROWS_DEC || id == IDC_ROWS_INC ||
                   id == IDC_COLS_DEC || id == IDC_COLS_INC) {
            if (id == IDC_ROWS_DEC) g_settings.gridRows = ClampGridRows(g_settings.gridRows - 1);
            else if (id == IDC_ROWS_INC) g_settings.gridRows = ClampGridRows(g_settings.gridRows + 1);
            else if (id == IDC_COLS_DEC) g_settings.gridCols = ClampGridCols(g_settings.gridCols - 1);
            else g_settings.gridCols = ClampGridCols(g_settings.gridCols + 1);
            wchar_t b[16];
            swprintf_s(b, 16, L"%d", g_settings.gridRows);
            SetWindowTextW(g_hwndRowsVal, b);
            swprintf_s(b, 16, L"%d", g_settings.gridCols);
            SetWindowTextW(g_hwndColsVal, b);
            SaveNow();
            if (g_hwndPalette && IsWindowVisible(g_hwndPalette) && InFileMode())
                UpdateResults(); // re-layout grid live
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
                    AddBrowserIfExists(g_browsers, ExeFileName(picked), picked);
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
                AddBrowserIfExists(g_browsers, ExeFileName(picked), picked);
                int count = (int)SendMessageW(g_hwndBrowserCombo, CB_GETCOUNT, 0, 0);
                SendMessageW(g_hwndBrowserCombo, CB_INSERTSTRING, count - 1,
                             (LPARAM)g_browsers.back().name.c_str());
                SendMessageW(g_hwndBrowserCombo, CB_SETCURSEL, count - 1, 0);
                SaveEngineBrowser(hwnd);
            }
        } else if (id == IDC_BANG_DEL) {
            DeleteSelectedTableRow(g_hwndBangsList);
        } else if (id == IDC_QUICK_DEL) {
            DeleteSelectedTableRow(g_hwndQuickList);
        } else if (id == IDC_BANG_ADD) {
            if ((int)g_settings.bangs.size() >= kMaxCustomEntries) {
                MessageBoxW(hwnd, L"Bang list is full (200 entries).",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
                break;
            }
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
            if ((int)g_settings.quicks.size() >= kMaxCustomEntries) {
                MessageBoxW(hwnd, L"Quick-link list is full (200 entries).",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
                break;
            }
            QuickLink q{L"New link", L"https://example.com"};
            g_settings.quicks.push_back(q);
            int row = LVAppendRow(g_hwndQuickList, q.name.c_str());
            LVSetCell(g_hwndQuickList, row, 1, q.url);
            SaveNow();
            ListView_SetItemState(g_hwndQuickList, row, LVIS_SELECTED, LVIS_SELECTED);
            ListView_EnsureVisible(g_hwndQuickList, row, FALSE);
            ListView_EditLabel(g_hwndQuickList, row);
        } else if (id >= IDC_THEME_BG &&
                   (int)(id - IDC_THEME_BG) < ThemeFieldCount()) {
            PickThemeColor(hwnd, (int)(id - IDC_THEME_BG));
        } else if (id == IDC_THEME_RESET) {
            g_settings.theme = DefaultTheme();
            SaveNow();
            ApplyThemeFromSettings();
            for (int i = 0; i < ThemeFieldCount(); i++)
                if (g_themeBtn[i] && IsWindow(g_themeBtn[i]))
                    InvalidateRect(g_themeBtn[i], NULL, FALSE);
        } else if (id == IDC_THEME_EDITFILE) {
            // Fresh export of the live theme, then open it for editing.
            ExportThemeFile(g_settings.theme);
            if ((UINT_PTR)ShellExecuteW(NULL, L"open",
                    ThemeIniPath().c_str(), NULL, NULL,
                    SW_SHOWNORMAL) <= 32)
                MessageBoxW(hwnd, L"Couldn't open theme.ini.",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
        } else if (id == IDC_THEME_RELOAD) {
            Theme t = g_settings.theme;
            if (!ImportThemeFile(t)) {
                MessageBoxW(hwnd, L"theme.ini not found - use Edit file first.",
                            L"SneekPeek", MB_ICONWARNING | MB_OK);
            } else {
                g_settings.theme = t;
                SaveNow(); // persist the file's colors into settings.ini
                ApplyThemeFromSettings();
                for (int i = 0; i < ThemeFieldCount(); i++)
                    if (g_themeBtn[i] && IsWindow(g_themeBtn[i]))
                        InvalidateRect(g_themeBtn[i], NULL, FALSE);
            }
        } else if (id == IDC_RESCAN) {
            g_indexReady.store(false);
            BuildIndexAsync(true); // manual rescan: full pass
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
        g_hwndSavedLabel = NULL;
        g_cellLv = 0; g_cellRow = -1; g_cellCol = 0;
        g_hwndEngineCombo = g_hwndEngineUrl = g_hwndBrowserCombo = NULL;
        g_hwndRowsVal = g_hwndColsVal = NULL;
        g_hwndAppsList = g_hwndBangsList = g_hwndQuickList = NULL;
        for (int i = 0; i < 32; i++) g_themeBtn[i] = NULL;
        g_pageGeneral.clear(); g_pageApps.clear();
        g_pageBangs.clear(); g_pageQuick.clear(); g_pageFiles.clear();
        g_pageTheme.clear();
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
        BuildIndexAsync(true); // manual refresh: full pass incl. PowerShell
        break;
    case ID_TRAY_AWAKE:
        g_awakeOn = !g_awakeOn;
        ApplyAwake();
        g_settings.awakeOn = g_awakeOn;
        EnsureSettingsLists();
        SaveOrWarn(L"keep awake");
        break;
    case ID_TRAY_STARTUP:
        g_settings.runAtStartup = !g_settings.runAtStartup;
        EnsureSettingsLists();
        SaveOrWarn(L"run at startup");
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
    // NOTE: GDI+ startup is lazy (EnsureGdiplus on first show) so gdiplus.dll
    // stays out of the logon path. comctl32 init is lazy too (OpenSettings).

    // Autostart launches with --tray (see ApplyRunAtStartup in settings.h).
    // A startup launch must do almost nothing: no scan, no font/GDI/DLL work.
    bool launchedAtStartup = (cmd != NULL &&
        (wcsstr(cmd, L"--tray") != NULL || wcsstr(cmd, L"/tray") != NULL));

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

    // Boot-cheap settings: General section only (~10 INI reads). Lists
    // (hidden/bangs/quicks) load on first interactive use via EnsureSettingsLists.
    LoadSettingsCore(g_settings);
    RebuildHiddenSet(); // empty until lists load; rebuilt by EnsureSettingsLists
    g_awakeOn = g_settings.awakeOn;
    if (g_awakeOn) ApplyAwake(); // restore the held-awake state silently
    // Disk cache serves instant results with zero scan cost at logon.
    bool cacheLoaded = LoadAppCache(AppCachePath(), g_apps);
    if (cacheLoaded)
        g_indexReady.store(true);

    // Fonts are lazy (EnsureAppFonts on first show); brushes are solid
    // colors, DPI-independent. DPI read itself is one cached query.
    {
        HDC screen = GetDC(NULL);
        g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
        ReleaseDC(NULL, screen);
    }
    UpdateLayoutMetrics();
    ApplyThemeFromSettings(); // theme colors from disk; builds the brushes

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

    WNDCLASSEXW wcBar{};
    wcBar.cbSize = sizeof(wcBar);
    wcBar.hInstance = hInst;
    wcBar.lpszClassName = kFileBarClass;
    wcBar.lpfnWndProc = FileBarProc;
    wcBar.hCursor = LoadCursorW(NULL, IDC_HAND);
    wcBar.hbrBackground = g_brBg;
    RegisterClassExW(&wcBar);

    WNDCLASSEXW wcGrid{};
    wcGrid.cbSize = sizeof(wcGrid);
    wcGrid.hInstance = hInst;
    wcGrid.lpszClassName = kFileGridClass;
    wcGrid.lpfnWndProc = FileGridProc;
    wcGrid.hCursor = LoadCursorW(NULL, IDC_HAND);
    wcGrid.hbrBackground = g_brBg;
    wcGrid.style = CS_DBLCLKS; // double-click drills/opens
    RegisterClassExW(&wcGrid);

    g_hwndPalette = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_COMPOSITED, // topmost, no taskbar, smooth
        kPaletteClass, L"SneekPeek",
        WS_POPUP | WS_CLIPCHILDREN, // borderless rounded region paints the shape
        0, 0, kWidth, kInputH + kItemH,
        NULL, NULL, hInst, NULL);
    if (!g_hwndPalette) return 1;
    // NOTE: no EnableRoundCorners here (dwmapi load deferred to first show).

    if (!RegisterHotKey(g_hwndPalette, HOTKEY_ID, MOD_CONTROL | MOD_NOREPEAT, VK_SPACE)) {
        MessageBoxW(NULL,
            L"Could not register Ctrl+Space.\nAnother app (IME, keyboard switcher, launcher) may own it.\n"
            L"SneekPeek will keep running - change the other app's hotkey, then restart SneekPeek.",
            L"SneekPeek", MB_OK | MB_ICONWARNING);
    }
    AddTrayIcon(g_hwndPalette);

    if (launchedAtStartup) {
        // Boot: never scan inside Windows' startup measurement window. Serve
        // from the disk cache (empty results are fine on first run) and run a
        // single light refresh after boot settles. A cold cache is populated
        // on the user's first Ctrl+Space instead of at logon.
        SetTimer(g_hwndPalette, IDT_DEFERRED_REFRESH, DEFERRED_REFRESH_MS, NULL);
        g_startupDeferredArmed = true;
    } else if (!cacheLoaded) {
        // Manual launch, cold cache: scan now so the first Ctrl+Space is
        // complete. Warm cache means the index is already ready.
        BuildIndexAsync(true);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        // Dialog-manager keys (Tab navigation) for Settings.
        if (g_hwndSettings && IsDialogMessageW(g_hwndSettings, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_quitting.store(true); // deferred startup work must stand down too
    if (g_hwndPalette) KillTimer(g_hwndPalette, IDT_DEFERRED_REFRESH);
    g_indexStop.store(true); // stop the indexer; it exits on its own at next check
    // Do not join at shutdown: ScanStartAppsPS can wait up to ~20s for a
    // stuck PowerShell child, which would make the process appear not
    // responding at quit. Process exit reclaims the thread; g_quitting +
    // g_indexStop make it stand down cleanly.
    if (g_historyDirty) SaveNow(); // persist any in-flight history entries
    delete g_pendingIndex.exchange(nullptr); // published but never consumed
    UnregisterHotKey(g_hwndPalette, HOTKEY_ID);
    if (g_oleInit) OleUninitialize(); // drop-target revoked in WM_DESTROY
    g_oleInit = false;
    SetThreadExecutionState(ES_CONTINUOUS); // release any Awake hold
    DeleteAppFonts(); // single source of truth for the six palette/UI fonts
    DeleteObject(g_brBg); DeleteObject(g_brInput); DeleteObject(g_brSel);
    ReleaseHoverBrush();
    ReleaseGlyphGdi(); // memoized glyph pens/brushes
    if (g_hIconBig) DestroyIcon(g_hIconBig);
    if (g_hIconSmall) DestroyIcon(g_hIconSmall);
    if (g_gdiplusStarted) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    CloseHandle(mutex);
    return (int)msg.wParam;
}
