#pragma once
// SneekPeek — settings stored in %APPDATA%\SneekPeek\settings.ini
// via Get/WritePrivateProfileString (OS-cached, zero background cost).
#include <windows.h>
#include <shlobj.h>
#include <string>

struct Settings {
    // Web engine template, must contain %s
    std::wstring engineUrl = L"https://duckduckgo.com/?q=%s";
    std::wstring engineName = L"DuckDuckGo";
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

inline void LoadSettings(Settings& s) {
    std::wstring ini = SettingsPath();
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"General", L"EngineUrl", s.engineUrl.c_str(),
                             buf, 1024, ini.c_str());
    if (buf[0]) s.engineUrl = buf;
    GetPrivateProfileStringW(L"General", L"EngineName", s.engineName.c_str(),
                             buf, 1024, ini.c_str());
    if (buf[0]) s.engineName = buf;
    s.includePathExes = GetPrivateProfileIntW(L"General", L"IncludePathExes", 0, ini.c_str()) != 0;
    s.runAtStartup = GetPrivateProfileIntW(L"General", L"RunAtStartup", 0, ini.c_str()) != 0;
}

inline void SaveSettings(const Settings& s) {
    std::wstring ini = SettingsPath();
    WritePrivateProfileStringW(L"General", L"EngineUrl", s.engineUrl.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"General", L"EngineName", s.engineName.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"General", L"IncludePathExes", s.includePathExes ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"RunAtStartup", s.runAtStartup ? L"1" : L"0", ini.c_str());
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
