#pragma once
// SneekPeek - settings in %APPDATA%\SneekPeek\settings.ini (OS-cached, zero
// background cost) + installed-browser detection + URL opening helper.
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar> // _wcsicmp in HistoryKeyEq / ParseThemeLine
#include <string>
#include <vector>
#include "bangs.h"
#include "results.h"

// ------------------------------------------------- quick links (name -> URL)
// Declared before Settings: the struct stores a vector of these.
struct QuickLink {
    std::wstring name;
    std::wstring url; // fixed URL, no %s / no query
};

// Shared cap for custom lists (bangs, quick links): load, save, and the UI
// all enforce the same limit, so entries can never silently disappear.
inline constexpr int kMaxCustomEntries = 200;

inline int ClampCustomCount(int n) {
    if (n < 0) return 0;
    return n > kMaxCustomEntries ? kMaxCustomEntries : n;
}

// File grid dimensions (file mode only): rows 1-4, columns 2-7.
inline int ClampGridRows(int n) {
    if (n < 1) return 1;
    return n > 4 ? 4 : n;
}
inline int ClampGridCols(int n) {
    if (n < 2) return 2;
    return n > 7 ? 7 : n;
}

// Effective grid columns: the Settings value is a maximum, auto-widened down
// when tiles would get too narrow for their labels (drive names like
// "APPLICATION (E:)" must never clip mid-glyph). Pure, unit-tested.
inline int EffectiveGridCols(int clientW, int wantCols, int minTileW) {
    if (wantCols < 1) wantCols = 1;
    if (clientW <= 0 || minTileW <= 0) return wantCols;
    int fit = clientW / minTileW;
    if (fit < 1) fit = 1;
    return fit < wantCols ? fit : wantCols;
}

// ------------------------------------------------- palette theme (Settings > Theme)
// Every UI color is user-editable: background, fields, text, all accents,
// buttons, edges, separators, even the power-chip colors. ONE descriptor
// table drives everything (defaults, settings.ini, theme.ini, swatches), so
// a color can never be added in one place and forgotten in another.
struct Theme {
    COLORREF bg, input, sel, text, rowtext, sub;
    COLORREF app, web, calc, hint, set, awake, file, folder, quick;
    COLORREF btnfill, btnfillhot, btnedge, edgehot, btnsel, btnselhot;
    COLORREF sep, menuacc, chiptext, danger;
    COLORREF powdown, powreboot, powsleep, powhib;
};

struct ThemeField {
    const wchar_t* key;   // INI + theme.ini name
    const wchar_t* label; // swatch button text (short: 176px buttons)
    COLORREF Theme::*member;
    COLORREF def;
};

inline const ThemeField kThemeFields[] = {
    {L"Bg",        L"Background",   &Theme::bg,        RGB(0, 0, 0)},
    {L"Input",     L"Search field", &Theme::input,     RGB(0, 0, 0)},
    {L"Sel",       L"Highlight",    &Theme::sel,       RGB(28, 28, 30)},
    {L"Text",      L"Text",         &Theme::text,      RGB(240, 240, 240)},
    {L"RowText",   L"Row text",     &Theme::rowtext,   RGB(218, 218, 226)},
    {L"Sub",       L"Secondary",    &Theme::sub,       RGB(130, 130, 135)},
    {L"App",       L"Apps",         &Theme::app,       RGB(96, 165, 250)},
    {L"Web",       L"Web",          &Theme::web,       RGB(46, 224, 158)},
    {L"Calc",      L"Calculator",   &Theme::calc,      RGB(251, 191, 36)},
    {L"Hint",      L"Hint",         &Theme::hint,      RGB(110, 110, 115)},
    {L"Set",       L"Settings",     &Theme::set,       RGB(45, 212, 191)},
    {L"Awake",     L"Awake",        &Theme::awake,     RGB(45, 212, 191)},
    {L"File",      L"Files",        &Theme::file,      RGB(96, 165, 250)},
    {L"Folder",    L"Folders",      &Theme::folder,    RGB(251, 191, 36)},
    {L"Quick",     L"Quick links",  &Theme::quick,     RGB(255, 95, 170)},
    {L"BtnFill",   L"Button",       &Theme::btnfill,   RGB(26, 26, 28)},
    {L"BtnFillHot",L"Button hot",   &Theme::btnfillhot,RGB(52, 52, 56)},
    {L"BtnEdge",   L"Button edge",  &Theme::btnedge,   RGB(66, 66, 72)},
    {L"EdgeHot",   L"Edge hot",     &Theme::edgehot,   RGB(130, 130, 140)},
    {L"BtnSel",    L"Accent",       &Theme::btnsel,    RGB(29, 65, 115)},
    {L"BtnSelHot", L"Accent hot",   &Theme::btnselhot, RGB(38, 82, 140)},
    {L"Sep",       L"Separator",    &Theme::sep,       RGB(30, 30, 32)},
    {L"MenuAcc",   L"Menu accent",  &Theme::menuacc,   RGB(150, 150, 155)},
    {L"ChipText",  L"Chip text",    &Theme::chiptext,  RGB(255, 255, 255)},
    {L"Danger",    L"Danger",       &Theme::danger,    RGB(255, 90, 90)},
    {L"PowDown",   L"Shut down",    &Theme::powdown,   RGB(255, 90, 90)},
    {L"PowReboot", L"Restart",      &Theme::powreboot, RGB(251, 191, 36)},
    {L"PowSleep",  L"Sleep",        &Theme::powsleep,  RGB(80, 140, 255)},
    {L"PowHib",    L"Hibernate",    &Theme::powhib,    RGB(160, 110, 255)},
};

inline int ThemeFieldCount() {
    return (int)(sizeof(kThemeFields) / sizeof(kThemeFields[0]));
}

inline Theme DefaultTheme() {
    Theme t{};
    for (int i = 0; i < ThemeFieldCount(); i++)
        t.*(kThemeFields[i].member) = kThemeFields[i].def;
    return t;
}

inline bool operator==(const Theme& a, const Theme& b) {
    for (int i = 0; i < ThemeFieldCount(); i++)
        if (a.*(kThemeFields[i].member) != b.*(kThemeFields[i].member)) return false;
    return true;
}
inline bool operator!=(const Theme& a, const Theme& b) { return !(a == b); }

// ------------------------------------------------- Up-on-empty history
// One merged recency list across apps, files/folders, and web searches.
// Move-to-front, capped, pure logic (unit-tested); persistence is [History]
// Count + ItemN, one typed line each. Bare legacy lines load as files.
struct HistoryItem {
    ResultKind kind = ResultKind::File; // App, Web, File, or Folder
    std::wstring title;   // display name only (app/query/base name)
    std::wstring action;  // app target | file path | URL ("" = engine-search title)
    bool isStore = false; // apps only
};
inline constexpr size_t kMaxHistory = 20;
// Dedupe identity: same kind + same target (engine searches match on title).
// Compared field by field rather than via a serialized key string: HistoryAdd
// used to rebuild every existing entry's key inside its scan loop, so one
// insert allocated up to kMaxHistory strings. App/file/web targets are short
// and this runs on every launch, search and folder drill.
inline bool HistoryKeyEq(const HistoryItem& a, const HistoryItem& b) {
    auto tag = [](ResultKind k) {
        return (k == ResultKind::App) ? L'A' : (k == ResultKind::Web) ? L'W' : L'F';
    };
    if (tag(a.kind) != tag(b.kind)) return false;
    if (_wcsicmp(a.action.c_str(), b.action.c_str()) != 0) return false;
    // Engine searches carry no target, so they dedupe on the query instead.
    if (a.kind == ResultKind::Web && a.action.empty() && b.action.empty())
        return _wcsicmp(a.title.c_str(), b.title.c_str()) == 0;
    return true;
}
inline void HistoryAdd(std::vector<HistoryItem>& h, const HistoryItem& item) {
    if (item.title.empty() && item.action.empty()) return;
    if (item.kind != ResultKind::App && item.kind != ResultKind::Web &&
        item.kind != ResultKind::File && item.kind != ResultKind::Folder)
        return;
    for (auto it = h.begin(); it != h.end(); ++it) {
        if (HistoryKeyEq(*it, item)) {
            h.erase(it);
            break;
        }
    }
    h.insert(h.begin(), item);
    if (h.size() > kMaxHistory) h.resize(kMaxHistory);
}
// "C:\a\b.txt" -> "b.txt"; drive roots ("C:\") keep full text.
inline std::wstring HistoryBaseName(const std::wstring& path) {
    size_t s = path.find_last_of(L"\\/");
    std::wstring t = (s == std::wstring::npos) ? path : path.substr(s + 1);
    return t.empty() ? path : t;
}
// A|name|target|0or1, W|title|url-or-empty, F|path, D|dirpath.
// Bare lines load as files (legacy format).
inline std::wstring SerializeHistoryItem(const HistoryItem& h) {
    if (h.kind == ResultKind::App)
        return L"A|" + h.title + L"|" + h.action + (h.isStore ? L"|1" : L"|0");
    if (h.kind == ResultKind::Web)
        return L"W|" + h.title + L"|" + h.action;
    if (h.kind == ResultKind::Folder)
        return L"D|" + h.action;
    return L"F|" + h.action;
}
inline bool ParseHistoryLine(const std::wstring& line, HistoryItem& out) {
    if (line.empty()) return false;
    if (line.find(L'|') == std::wstring::npos) { // legacy bare path
        HistoryItem h;
        h.kind = ResultKind::File;
        h.action = line;
        h.title = HistoryBaseName(line);
        if (h.action.empty() || h.title.empty()) return false;
        out = std::move(h);
        return true;
    }
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        size_t p = line.find(L'|', start);
        parts.push_back(line.substr(start, p == std::wstring::npos ? p : p - start));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    if (parts.empty()) return false;
    if (parts[0] == L"A" && parts.size() >= 4 &&
        (parts.back() == L"0" || parts.back() == L"1")) {
        HistoryItem h;
        h.kind = ResultKind::App;
        h.isStore = parts.back() == L"1";
        h.action = parts[parts.size() - 2];
        h.title = parts[1];
        for (size_t i = 2; i + 2 < parts.size(); i++) h.title += L"|" + parts[i];
        if (h.title.empty() || h.action.empty()) return false;
        out = std::move(h);
        return true;
    }
    if (parts[0] == L"W" && parts.size() >= 2) {
        HistoryItem h;
        h.kind = ResultKind::Web;
        h.title = parts[1];
        h.action = L"";
        for (size_t i = 2; i < parts.size(); i++) h.action += (i > 2 ? L"|" : L"") + parts[i];
        if (h.title.empty()) return false;
        out = std::move(h);
        return true;
    }
    if (parts[0] == L"F" && parts.size() >= 2) {
        HistoryItem h;
        h.kind = ResultKind::File;
        h.action = parts[1];
        for (size_t i = 2; i < parts.size(); i++) h.action += L"|" + parts[i];
        h.title = HistoryBaseName(h.action);
        if (h.action.empty() || h.title.empty()) return false;
        out = std::move(h);
        return true;
    }
    if (parts[0] == L"D" && parts.size() >= 2) {
        HistoryItem h;
        h.kind = ResultKind::Folder;
        h.action = parts[1];
        for (size_t i = 2; i < parts.size(); i++) h.action += L"|" + parts[i];
        h.title = HistoryBaseName(h.action);
        if (h.action.empty() || h.title.empty()) return false;
        out = std::move(h);
        return true;
    }
    return false;
}

inline bool ParseQuickLine(const std::wstring& line, QuickLink& out) {
    size_t p = line.find(L'|');
    if (p == std::wstring::npos) return false;
    QuickLink q{BangTrim(line.substr(0, p)), BangTrim(line.substr(p + 1))};
    if (q.name.empty()) return false;
    if (q.url.find(L"://") == std::wstring::npos) return false;
    out = std::move(q);
    return true;
}

struct Settings {
    std::wstring engineUrl = L"https://duckduckgo.com/?q=%s";
    std::wstring engineName = L"DuckDuckGo";
    std::wstring browserPath; // empty = system default browser
    std::vector<std::wstring> hiddenApps; // exact app names (case-insensitive) never shown
    std::vector<Bang> bangs;
    std::vector<QuickLink> quicks; // name -> fixed URL launched on Enter (no query)
    bool awakeOn = false;          // keep display+system awake (tray + Awake chip)
    bool fileGrid = true;          // file browser presentation: grid vs list
    int gridRows = 3;              // grid tile rows, 1-4 (file grid only)
    int gridCols = 5;              // grid tile columns, 2-7 (file grid only)
    bool includePathExes = false;
    bool runAtStartup = false;
    Theme theme = DefaultTheme();
    std::vector<HistoryItem> history; // merged recency: apps, files, searches
};

// Resolved once per process: every load/save/cache/theme path needs this, and
// each call used to cost a SHGetFolderPathW + a CreateDirectoryW syscall.
inline const std::wstring& SettingsPath() {
    static const std::wstring path = [] {
        wchar_t appdata[MAX_PATH];
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata)))
            return std::wstring(L"settings.ini");
        std::wstring dir = std::wstring(appdata) + L"\\SneekPeek";
        CreateDirectoryW(dir.c_str(), NULL);
        return dir + L"\\settings.ini";
    }();
    return path;
}

// Directory holding settings.ini, apps.cache and theme.ini. SettingsPath()
// resolves (and creates) it once per process.
inline std::wstring SettingsDir() {
    const std::wstring& ini = SettingsPath();
    size_t p = ini.rfind(L'\\');
    return (p == std::wstring::npos) ? std::wstring(L"") : ini.substr(0, p + 1);
}
inline std::wstring AppCachePath() {
    return SettingsDir() + L"apps.cache";
}

// Split on '|' (single-line INI value <-> lines).
inline std::vector<std::wstring> SplitPipe(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(L'|', start);
        std::wstring t = BangTrim(s.substr(start, p == std::wstring::npos ? p : p - start));
        if (!t.empty()) out.push_back(t);
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    return out;
}

// Boot-cheap core: General section only (a handful of INI reads) plus
// in-memory defaults for the lists, so the palette works before the full
// lists are loaded. Call LoadSettingsLists() (via EnsureSettingsLists in
// main.cpp) on first interactive use.
//
// `ini` defaults to the real %APPDATA% path. It is a parameter only so the
// unit tests can point the loader at a scratch file; production callers always
// take the default.
inline void LoadSettingsCore(Settings& s, const std::wstring& ini = SettingsPath()) {
    s.bangs = DefaultBangs();
    // Seed one example so the feature is discoverable until lists load.
    s.quicks = {{L"Gmail", L"https://mail.google.com"}};
    s.hiddenApps.clear();
    wchar_t buf[4096];
    GetPrivateProfileStringW(L"General", L"EngineUrl", s.engineUrl.c_str(),
                             buf, 4096, ini.c_str());
    if (buf[0]) s.engineUrl = buf;
    GetPrivateProfileStringW(L"General", L"EngineName", s.engineName.c_str(),
                             buf, 4096, ini.c_str());
    if (buf[0]) s.engineName = buf;
    GetPrivateProfileStringW(L"General", L"BrowserPath", L"", buf, 4096, ini.c_str());
    s.browserPath = buf;
    s.includePathExes = GetPrivateProfileIntW(L"General", L"IncludePathExes", 0, ini.c_str()) != 0;
    s.runAtStartup = GetPrivateProfileIntW(L"General", L"RunAtStartup", 0, ini.c_str()) != 0;
    s.awakeOn = GetPrivateProfileIntW(L"General", L"Awake", 0, ini.c_str()) != 0;
    s.fileGrid = GetPrivateProfileIntW(L"General", L"FileGrid", 1, ini.c_str()) != 0;
    s.gridRows = ClampGridRows(GetPrivateProfileIntW(L"General", L"GridRows", 3, ini.c_str()));
    s.gridCols = ClampGridCols(GetPrivateProfileIntW(L"General", L"GridCols", 5, ini.c_str()));
    // Theme: every key falls back to its default when missing or malformed,
    // so a hand-edited or older INI can never produce an unreadable palette.
    for (int i = 0; i < ThemeFieldCount(); i++) {
        GetPrivateProfileStringW(L"Theme", kThemeFields[i].key,
                                 SerializeColor(kThemeFields[i].def).c_str(),
                                 buf, 4096, ini.c_str());
        COLORREF c;
        s.theme.*(kThemeFields[i].member) =
            (buf[0] && ParseColor(buf, c)) ? c : kThemeFields[i].def;
    }
}

// Deferred lists: hidden apps + bangs + quick links (dozens of INI reads).
// Runs once on first palette show / Settings open / pre-save, never at boot.
// `ini` defaults to the real %APPDATA% path; see LoadSettingsCore.
inline void LoadSettingsLists(Settings& s, const std::wstring& ini = SettingsPath()) {
    wchar_t buf[4096];
    int hcount = GetPrivateProfileIntW(L"General", L"HiddenCount", -1, ini.c_str());
    if (hcount >= 0) {
        s.hiddenApps.clear();
        wchar_t key[32];
        for (int i = 0; i < hcount && i < 2000; i++) {
            swprintf_s(key, 32, L"Hidden%d", i);
            GetPrivateProfileStringW(L"General", key, L"", buf, 4096, ini.c_str());
            if (buf[0]) s.hiddenApps.push_back(buf);
        }
    } else {
        GetPrivateProfileStringW(L"General", L"HiddenApps", L"", buf, 4096, ini.c_str());
        s.hiddenApps = SplitPipe(buf); // legacy single-value format
    }

    // -1 (key absent) = never configured: keep the defaults seeded by
    // LoadSettingsCore. 0 = the user deleted every bang, which must stay
    // deleted. Reading the default as 0 conflated those two cases, so deleting
    // the last bang resurrected all 11 defaults on the next launch. This now
    // mirrors the [Quick] block below exactly.
    //
    // The clamp must stay INSIDE the count>0 arm: ClampCustomCount maps -1 to 0,
    // so clamping first would make "key absent" indistinguishable from
    // "explicitly 0" and wipe the defaults on every first run.
    int count = GetPrivateProfileIntW(L"Bangs", L"Count", -1, ini.c_str());
    if (count > 0) {
        count = ClampCustomCount(count);
        std::vector<Bang> custom;
        wchar_t key[32];
        for (int i = 0; i < count; i++) {
            swprintf_s(key, 32, L"Bang%d", i);
            GetPrivateProfileStringW(L"Bangs", key, L"", buf, 4096, ini.c_str());
            if (!buf[0]) continue;
            Bang b;
            if (ParseBangLine(buf, b)) custom.push_back(std::move(b));
        }
        // A Count>0 means the user has a custom list, so keep what parsed even if
        // some lines were rejected. Falling back to the defaults when the
        // custom list comes back empty would silently resurrect bangs the user
        // deleted - the old code did exactly that.
        s.bangs = std::move(custom);
    } else if (count == 0) {
        s.bangs.clear(); // explicitly emptied: stay empty
    }

    // -1 = never configured: keep the seeded example from LoadSettingsCore.
    int qcount = GetPrivateProfileIntW(L"Quick", L"Count", -1, ini.c_str());
    if (qcount > 0) {
        qcount = ClampCustomCount(qcount);
        s.quicks.clear();
        wchar_t key[32];
        for (int i = 0; i < qcount; i++) {
            swprintf_s(key, 32, L"Quick%d", (int)i);
            GetPrivateProfileStringW(L"Quick", key, L"", buf, 4096, ini.c_str());
            if (!buf[0]) continue;
            QuickLink q;
            if (ParseQuickLine(buf, q)) s.quicks.push_back(std::move(q));
        }
    } else if (qcount == 0) {
        s.quicks.clear();
    }

    // Up-on-empty history: typed lines, newest first. Bad lines are skipped
    // so a hand-edited INI can never show dead rows; bare legacy lines load
    // as files.
    s.history.clear();
    int histCount = GetPrivateProfileIntW(L"History", L"Count", 0, ini.c_str());
    if (histCount < 0) histCount = 0;
    if (histCount > (int)kMaxHistory) histCount = (int)kMaxHistory;
    {
        wchar_t key[32];
        for (int i = 0; i < histCount && s.history.size() < kMaxHistory; i++) {
            swprintf_s(key, 32, L"Item%d", i);
            GetPrivateProfileStringW(L"History", key, L"", buf, 4096, ini.c_str());
            if (!buf[0]) continue;
            HistoryItem h;
            if (ParseHistoryLine(buf, h)) s.history.push_back(std::move(h));
        }
    }
}

inline void LoadSettings(Settings& s) {
    LoadSettingsCore(s);
    LoadSettingsLists(s);
}

// ------------------------------------------------- one-shot INI serialization
// WritePrivateProfileStringW rewrites (and relocks) the WHOLE file on every
// single key, so the old SaveSettings turned one save into hundreds of
// read-modify-write cycles of settings.ini (~2000 with a long hidden list).
// We own the whole schema, so we serialize it in memory and write it once.
//
// Quoting mirrors the profile API's own rule: a value is wrapped in quotes when
// it has leading/trailing whitespace or contains ';' (the only INI comment
// character on Windows). '#' is not special in Windows INI files, and embedded
// quotes are deliberately left unquoted because Windows has no escape for them.
inline std::wstring IniQuote(const std::wstring& v) {
    bool ws = !v.empty() && (v.front() == L' ' || v.front() == L'\t' ||
                             v.back() == L' ' || v.back() == L'\t');
    if (!ws && v.find(L';') != std::wstring::npos) ws = true;
    return ws ? (L"\"" + v + L"\"") : v;
}

struct IniBuf {
    std::wstring text;
    bool haveSection = false;
    void open(const wchar_t* s) {
        if (haveSection) text += L"\r\n";
        haveSection = true;
        text += L"[";
        text += s;
        text += L"]\r\n";
    }
    void put(const wchar_t* key, const std::wstring& val) {
        if (!haveSection) open(L"General");
        text += key;
        text += L"=";
        text += IniQuote(val);
        text += L"\r\n";
    }
    void putInt(const wchar_t* key, int n) {
        wchar_t b[16];
        swprintf_s(b, 16, L"%d", n);
        put(key, b);
    }
};

// Write atomically via a temp file + rename: a crash mid-write can no longer
// leave a half-written settings.ini (the per-key API could).
inline bool WriteTextFileAtomic(const std::wstring& path, const std::wstring& body) {
    std::wstring tmp = path + L".tmp";
    // UTF-16LE BOM: without it the profile API treats the file as ANSI and
    // non-ASCII app names / queries come back as mojibake.
    std::wstring payload;
    payload.reserve(body.size() + 1);
    payload.push_back((wchar_t)0xFEFF);
    payload += body;

    HANDLE hf = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;
    const char* p = (const char*)payload.data();
    DWORD left = (DWORD)(payload.size() * sizeof(wchar_t));
    bool ok = true;
    while (left) {
        DWORD n = 0;
        if (!WriteFile(hf, p, left, &n, NULL) || n == 0) { ok = false; break; }
        p += n;
        left -= n;
    }
    CloseHandle(hf);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// Returns false if the file could not be written (read-only profile, disk
// full, locked by a sync client). Callers treat Settings as the source of
// truth regardless, so a failed write only costs persistence, never state.
inline bool SaveSettings(const Settings& s) {
    IniBuf ini;

    ini.open(L"General");
    ini.put(L"EngineUrl", s.engineUrl);
    ini.put(L"EngineName", s.engineName);
    ini.put(L"BrowserPath", s.browserPath);
    // Legacy pipe-joined HiddenApps is intentionally not written: it is only
    // ever read as a fallback when HiddenCount is absent, and we always write
    // HiddenCount. Keeping it meant serializing the whole list twice.
    {
        size_t hn = s.hiddenApps.size() < 2000 ? s.hiddenApps.size() : 2000;
        ini.putInt(L"HiddenCount", (int)hn);
        wchar_t key[32];
        for (size_t i = 0; i < hn; i++) {
            swprintf_s(key, 32, L"Hidden%d", (int)i);
            ini.put(key, s.hiddenApps[i]);
        }
    }
    ini.put(L"IncludePathExes", s.includePathExes ? L"1" : L"0");
    ini.put(L"RunAtStartup", s.runAtStartup ? L"1" : L"0");
    ini.put(L"Awake", s.awakeOn ? L"1" : L"0");
    ini.put(L"FileGrid", s.fileGrid ? L"1" : L"0");
    ini.putInt(L"GridRows", s.gridRows);
    ini.putInt(L"GridCols", s.gridCols);

    ini.open(L"Theme");
    for (int i = 0; i < ThemeFieldCount(); i++)
        ini.put(kThemeFields[i].key,
                SerializeColor(s.theme.*(kThemeFields[i].member)));

    ini.open(L"Bangs");
    size_t nbangs = s.bangs.size() < (size_t)kMaxCustomEntries
        ? s.bangs.size() : (size_t)kMaxCustomEntries;
    ini.putInt(L"Count", (int)nbangs);
    {
        wchar_t key[32];
        for (size_t i = 0; i < nbangs; i++) {
            swprintf_s(key, 32, L"Bang%d", (int)i);
            ini.put(key, SerializeBang(s.bangs[i]));
        }
    }

    ini.open(L"Quick");
    size_t nquick = s.quicks.size() < (size_t)kMaxCustomEntries
        ? s.quicks.size() : (size_t)kMaxCustomEntries;
    ini.putInt(L"Count", (int)nquick);
    {
        wchar_t key[32];
        for (size_t i = 0; i < nquick; i++) {
            swprintf_s(key, 32, L"Quick%d", (int)i);
            ini.put(key, s.quicks[i].name + L" | " + s.quicks[i].url);
        }
    }

    ini.open(L"History");
    size_t nhist = s.history.size() < kMaxHistory ? s.history.size() : kMaxHistory;
    ini.putInt(L"Count", (int)nhist);
    {
        wchar_t key[32];
        for (size_t i = 0; i < nhist; i++) {
            swprintf_s(key, 32, L"Item%d", (int)i);
            ini.put(key, SerializeHistoryItem(s.history[i]));
        }
    }

    return WriteTextFileAtomic(SettingsPath(), ini.text); // resolved once
}

// ------------------------------------------------- theme.ini (plain-text theme file)
// %APPDATA%\SneekPeek\theme.ini: commented "#RRGGBB per line" file for people
// who'd rather edit colors in Notepad than click swatches. settings.ini stays
// the store: "Edit file" exports the live theme, "Reload" imports + saves.
inline std::wstring ThemeIniPath() {
    return SettingsDir() + L"theme.ini";
}

// One theme.ini line: "Key = #RRGGBB" (spacing/case tolerant). False for
// blanks, #-comments, unknown keys, and malformed colors. Unit-tested.
inline bool ParseThemeLine(const std::wstring& line, std::wstring& key, COLORREF& color) {
    std::wstring t = BangTrim(line);
    if (t.empty() || t[0] == L'#' || t[0] == L';') return false;
    size_t eq = t.find(L'=');
    if (eq == std::wstring::npos) return false;
    std::wstring k = BangTrim(t.substr(0, eq));
    std::wstring v = BangTrim(t.substr(eq + 1));
    if (k.empty() || v.empty()) return false;
    for (int i = 0; i < ThemeFieldCount(); i++) {
        if (_wcsicmp(k.c_str(), kThemeFields[i].key) == 0) {
            if (!ParseColor(v, color)) return false;
            key = kThemeFields[i].key;
            return true;
        }
    }
    return false; // unknown key: never fail the whole file for one line
}

// Export the live theme with comments (always rewrites: the file is a fresh
// export of what Settings holds, so the two can never silently disagree).
inline void ExportThemeFile(const Theme& t) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, ThemeIniPath().c_str(), L"w,ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"# SneekPeek theme - #RRGGBB per line, # starts a comment.\n");
    fwprintf(f, L"# Exported from Settings; edit, save, then press Reload.\n");
    fwprintf(f, L"# Unknown keys and bad lines are ignored on reload.\n");
    for (int i = 0; i < ThemeFieldCount(); i++)
        fwprintf(f, L"%-10s = %s\n", kThemeFields[i].key,
                 SerializeColor(t.*(kThemeFields[i].member)).c_str());
    fclose(f);
}

// Import theme.ini over t (missing/bad lines keep current values).
// False when the file doesn't exist (caller says "use Edit file first").
inline bool ImportThemeFile(Theme& t) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, ThemeIniPath().c_str(), L"r,ccs=UTF-8") != 0 || !f) return false;
    wchar_t buf[512];
    while (fgetws(buf, 512, f)) {
        std::wstring key;
        COLORREF c;
        if (!ParseThemeLine(buf, key, c)) continue;
        for (int i = 0; i < ThemeFieldCount(); i++) {
            if (key == kThemeFields[i].key) {
                t.*(kThemeFields[i].member) = c;
                break;
            }
        }
    }
    fclose(f);
    return true;
}

inline void ApplyRunAtStartup(bool enable) {
    HKEY h = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
        RegSetValueExW(h, L"SneekPeek", 0, REG_SZ,
                       (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(h, L"SneekPeek");
    }
    RegCloseKey(h);
}

// ------------------------------------------------- browsers (not system default)
struct BrowserOpt {
    std::wstring name;
    std::wstring path; // full path to exe
};

inline std::wstring ExpandEnv(const std::wstring& s) {
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), NULL, 0);
    if (n == 0 || n > 32768) return s;
    std::wstring out(n, L'\0');
    DWORD w = ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    if (w == 0 || w > n) return s;
    out.resize(w > 0 ? w - 1 : 0);
    return out;
}

inline void AddBrowserIfExists(std::vector<BrowserOpt>& out,
                               const std::wstring& name, const std::wstring& path) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    for (const auto& b : out)
        if (_wcsicmp(b.path.c_str(), path.c_str()) == 0) return;
    out.push_back({name, path});
}

inline std::vector<BrowserOpt> DetectBrowsers() {
    std::vector<BrowserOpt> out;
    const wchar_t* exes[] = {L"chrome.exe", L"msedge.exe", L"firefox.exe",
                             L"brave.exe", L"opera.exe", L"vivaldi.exe"};
    const wchar_t* names[] = {L"Chrome", L"Edge", L"Firefox",
                              L"Brave", L"Opera", L"Vivaldi"};
    for (int i = 0; i < 6; i++) {
        std::wstring sub = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exes[i];
        wchar_t buf[MAX_PATH]; DWORD sz = sizeof(buf);
        LONG r = RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), NULL,
                              RRF_RT_REG_SZ, NULL, buf, &sz);
        if (r != ERROR_SUCCESS) {
            sz = sizeof(buf);
            r = RegGetValueW(HKEY_LOCAL_MACHINE, sub.c_str(), NULL,
                             RRF_RT_REG_SZ, NULL, buf, &sz);
        }
        if (r == ERROR_SUCCESS) AddBrowserIfExists(out, names[i], buf);
    }
    const wchar_t* known[] = {
        L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe",
        L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe",
        L"%ProgramFiles%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
        L"%ProgramFiles(x86)%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
        L"%ProgramFiles%\\Mozilla Firefox\\firefox.exe",
        L"%ProgramFiles(x86)%\\Mozilla Firefox\\firefox.exe",
    };
    const wchar_t* knownNames[] = {L"Chrome", L"Chrome", L"Edge", L"Edge",
                                   L"Brave", L"Brave", L"Firefox", L"Firefox"};
    for (int i = 0; i < 8; i++)
        AddBrowserIfExists(out, knownNames[i], ExpandEnv(known[i]));
    return out;
}

// Open a URL in the chosen browser (empty path = system default handler).
// Returns the ShellExecute result (>32 = launched); empty URL is a no-op success.
inline HINSTANCE OpenUrl(const std::wstring& url, const std::wstring& browserPath) {
    if (url.empty()) return (HINSTANCE)33;
    if (!IsSafeWebUrl(url)) return (HINSTANCE)0; // blocked scheme: caller balloons
    if (browserPath.empty()) {
        return ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else {
        std::wstring params = L"\"" + url + L"\"";
        return ShellExecuteW(NULL, L"open", browserPath.c_str(), params.c_str(), NULL, SW_SHOWNORMAL);
    }
}
