#pragma once
// SneekPeek - settings in %APPDATA%\SneekPeek\settings.ini (OS-cached, zero
// background cost) + installed-browser detection + URL opening helper.
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include "bangs.h"

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
    bool onlineOn = true;          // live web suggestions (sends text to DuckDuckGo)
    bool includePathExes = false;
    bool runAtStartup = false;
};

inline std::wstring SettingsPath() {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appdata)))
        return L"settings.ini";
    std::wstring dir = std::wstring(appdata) + L"\\SneekPeek";
    CreateDirectoryW(dir.c_str(), NULL);
    return dir + L"\\settings.ini";
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

inline std::wstring JoinPipe(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += L"|";
        s += v[i];
    }
    return s;
}

inline void LoadSettings(Settings& s) {
    s.bangs = DefaultBangs();
    std::wstring ini = SettingsPath();
    wchar_t buf[4096];
    GetPrivateProfileStringW(L"General", L"EngineUrl", s.engineUrl.c_str(),
                             buf, 4096, ini.c_str());
    if (buf[0]) s.engineUrl = buf;
    GetPrivateProfileStringW(L"General", L"EngineName", s.engineName.c_str(),
                             buf, 4096, ini.c_str());
    if (buf[0]) s.engineName = buf;
    GetPrivateProfileStringW(L"General", L"BrowserPath", L"", buf, 4096, ini.c_str());
    s.browserPath = buf;
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
    s.includePathExes = GetPrivateProfileIntW(L"General", L"IncludePathExes", 0, ini.c_str()) != 0;
    s.runAtStartup = GetPrivateProfileIntW(L"General", L"RunAtStartup", 0, ini.c_str()) != 0;
    s.awakeOn = GetPrivateProfileIntW(L"General", L"Awake", 0, ini.c_str()) != 0;
    s.onlineOn = GetPrivateProfileIntW(L"General", L"OnlineSug", 1, ini.c_str()) != 0;

    int count = GetPrivateProfileIntW(L"Bangs", L"Count", 0, ini.c_str());
    count = ClampCustomCount(count); // clamp, never drop all
    if (count > 0) {
        std::vector<Bang> custom;
        wchar_t key[32];
        for (int i = 0; i < count; i++) {
            swprintf_s(key, 32, L"Bang%d", i);
            GetPrivateProfileStringW(L"Bangs", key, L"", buf, 4096, ini.c_str());
            if (!buf[0]) continue;
            Bang b;
            if (ParseBangLine(buf, b)) custom.push_back(std::move(b));
        }
        if (!custom.empty()) s.bangs = std::move(custom);
    }

    // -1 = never configured: seed one example so the feature is discoverable.
    int qcount = GetPrivateProfileIntW(L"Quick", L"Count", -1, ini.c_str());
    if (qcount < 0) {
        s.quicks = {{L"Gmail", L"https://mail.google.com"}};
    } else if (qcount > 0) {
        qcount = ClampCustomCount(qcount);
        wchar_t key[32];
        for (int i = 0; i < qcount; i++) {
            swprintf_s(key, 32, L"Quick%d", i);
            GetPrivateProfileStringW(L"Quick", key, L"", buf, 4096, ini.c_str());
            if (!buf[0]) continue;
            QuickLink q;
            if (ParseQuickLine(buf, q)) s.quicks.push_back(std::move(q));
        }
    }
}

inline void SaveSettings(const Settings& s) {
    std::wstring ini = SettingsPath();
    WritePrivateProfileStringW(L"General", L"EngineUrl", s.engineUrl.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"General", L"EngineName", s.engineName.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"General", L"BrowserPath", s.browserPath.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"General", L"HiddenApps", JoinPipe(s.hiddenApps).c_str(), ini.c_str());
    {
        wchar_t hc[16];
        size_t hn = s.hiddenApps.size() < 2000 ? s.hiddenApps.size() : 2000;
        swprintf_s(hc, 16, L"%d", (int)hn);
        WritePrivateProfileStringW(L"General", L"HiddenCount", hc, ini.c_str());
        wchar_t key[32];
        for (size_t i = 0; i < hn; i++) {
            swprintf_s(key, 32, L"Hidden%d", (int)i);
            WritePrivateProfileStringW(L"General", key, s.hiddenApps[i].c_str(), ini.c_str());
        }
    }
    WritePrivateProfileStringW(L"General", L"IncludePathExes", s.includePathExes ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"RunAtStartup", s.runAtStartup ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"Awake", s.awakeOn ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"OnlineSug", s.onlineOn ? L"1" : L"0", ini.c_str());
    size_t nbangs = s.bangs.size() < (size_t)kMaxCustomEntries
        ? s.bangs.size() : (size_t)kMaxCustomEntries;
    wchar_t count[16];
    swprintf_s(count, 16, L"%d", (int)nbangs);
    WritePrivateProfileStringW(L"Bangs", L"Count", count, ini.c_str());
    wchar_t key[32];
    for (size_t i = 0; i < nbangs; i++) {
        swprintf_s(key, 32, L"Bang%d", (int)i);
        WritePrivateProfileStringW(L"Bangs", key, SerializeBang(s.bangs[i]).c_str(), ini.c_str());
    }
    size_t nquick = s.quicks.size() < (size_t)kMaxCustomEntries
        ? s.quicks.size() : (size_t)kMaxCustomEntries;
    wchar_t qcount[16];
    swprintf_s(qcount, 16, L"%d", (int)nquick);
    WritePrivateProfileStringW(L"Quick", L"Count", qcount, ini.c_str());
    for (size_t i = 0; i < nquick; i++) {
        swprintf_s(key, 32, L"Quick%d", (int)i);
        std::wstring line = s.quicks[i].name + L" | " + s.quicks[i].url;
        WritePrivateProfileStringW(L"Quick", key, line.c_str(), ini.c_str());
    }
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
    if (browserPath.empty()) {
        return ShellExecuteW(NULL, L"open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
    } else {
        std::wstring params = L"\"" + url + L"\"";
        return ShellExecuteW(NULL, L"open", browserPath.c_str(), params.c_str(), NULL, SW_SHOWNORMAL);
    }
}
