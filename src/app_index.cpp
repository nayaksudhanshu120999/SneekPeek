#include "app_index.h"
#include "minjson.h"
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <cwctype> // towlower in ToLower
#include <cwchar>   // _wcsnicmp in IsRunnableTarget / StartsWithUninstall
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <algorithm>

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) c = (wchar_t)towlower(c);
    return out;
}

// Same transform, in place: no second buffer, and callers that only need the
// folded form stop allocating a temporary copy per item.
void ToLowerInPlace(std::wstring& s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
}

static std::wstring ExpandEnvLocal(const std::wstring& s) {
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), NULL, 0);
    if (n == 0 || n > 32768) return s;
    std::wstring out(n, L'\0');
    DWORD w = ExpandEnvironmentStringsW(s.c_str(), out.data(), n);
    if (w == 0 || w > n) return s;
    out.resize(w > 0 ? w - 1 : 0);
    return out;
}

// Resolve a .lnk to its target path. False when unresolvable (special shell
// links) - callers keep those; they are usually real system tools.
//
// The shell-link COM object is created once per scan and re-Load()ed per link.
// CoCreateInstance + QueryInterface per file meant hundreds of object
// creations for a typical Start Menu.
class LnkResolver {
    IShellLinkW* m_sl;
    IPersistFile* m_pf;
public:
    LnkResolver() : m_sl(NULL), m_pf(NULL) {
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                       IID_IShellLinkW, (void**)&m_sl)) && m_sl) {
            if (FAILED(m_sl->QueryInterface(IID_IPersistFile, (void**)&m_pf))) {
                m_sl->Release();
                m_sl = NULL;
            }
        }
    }
    ~LnkResolver() {
        if (m_pf) m_pf->Release();
        if (m_sl) m_sl->Release();
    }
    bool Resolve(const std::wstring& lnk, std::wstring& target) {
        if (!m_pf) return false;
        if (FAILED(m_pf->Load(lnk.c_str(), STGM_READ))) return false;
        // MAX_PATH-sized buffer, plus a long-path probe: a deep .lnk target can
        // exceed 1023 chars, and GetPath returning S_OK with a truncated path
        // would yield a wrong extension (and so a wrong runnable/not decision).
        wchar_t path[1024] = {0};
        WIN32_FIND_DATAW fd{};
        HRESULT hr = m_sl->GetPath(path, 1024, &fd, SLGP_UNCPRIORITY);
        if (hr == S_OK && path[0]) {
            // Expand only when the target actually holds an env reference.
            // Callers use the result solely for its extension, so the expansion
            // (two ExpandEnvironmentStringsW passes plus an allocation) was
            // thrown away for essentially every link.
            target = (wcschr(path, L'%') != NULL) ? ExpandEnvLocal(path) : path;
            return true;
        }
        if (hr != S_FALSE) return false; // S_OK-but-empty: unresolvable link
        // S_FALSE = buffer too small: retry once at a larger size.
        std::vector<wchar_t> big(32768, L'\0');
        if (FAILED(m_sl->GetPath(big.data(), (int)big.size(), &fd,
                                 SLGP_UNCPRIORITY)) || !big[0])
            return false;
        target = (wcschr(big.data(), L'%') != NULL) ? ExpandEnvLocal(big.data())
                                                    : big.data();
        return true;
    }
};

// Only executable targets belong in the index: drops .txt/.chm/.pdf links etc.
static bool IsRunnableTarget(const std::wstring& target) {
    size_t dot = target.find_last_of(L'.');
    size_t sep = target.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep))
        return true; // no extension: keep (some tools are extensionless)
    // Compare case-insensitively in place, without materializing the extension.
    size_t n = target.size() - (dot + 1);
    const wchar_t* ext = target.c_str() + dot + 1;
    auto is = [&](const wchar_t* want) {
        size_t wl = 0;
        while (want[wl]) wl++;
        return wl == n && _wcsnicmp(ext, want, n) == 0;
    };
    return is(L"exe") || is(L"msc") || is(L"bat") || is(L"cmd") ||
           is(L"com") || is(L"pif") || is(L"scr") || is(L"cpl");
}

static bool StartsWithUninstall(const std::wstring& name) {
    return name.size() >= 9 &&
           _wcsnicmp(name.c_str(), L"uninstall", 9) == 0;
}

// True for direct filesystem paths ("C:\...", "\\server\..."): the only
// StartApps AppIDs launchable without the shell:AppsFolder prefix.
static bool IsAbsoluteFilePath(const std::wstring& p) {
    if (p.size() >= 3 && ((p[0] >= L'A' && p[0] <= L'Z') ||
                           (p[0] >= L'a' && p[0] <= L'z')) &&
        p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'))
        return true;
    if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') return true;
    return false;
}

static void ScanLnkDir(const std::wstring& root, std::vector<AppEntry>& out) {
    // One COM shell-link for the whole directory walk (see LnkResolver).
    LnkResolver lnk;
    std::vector<std::wstring> stack;
    stack.push_back(root);

    while (!stack.empty()) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();

        std::wstring pattern = dir + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    continue; // junctions/symlinks: never recurse (loop risk)
                if (fd.cFileName[0] == L'.' &&
                    (fd.cFileName[1] == L'\0' ||
                     (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
                    continue;
                if (out.size() < 4000)
                    stack.push_back(dir + L"\\" + fd.cFileName);
                continue;
            }
            const wchar_t* ext = PathFindExtensionW(fd.cFileName);
            bool isLnk = _wcsicmp(ext, L".lnk") == 0;
            bool isExe = _wcsicmp(ext, L".exe") == 0;
            if (!isLnk && !isExe) continue; // no .url/docs/pics: apps only
            if (out.size() >= 4000) break;

            std::wstring base(fd.cFileName, ext - fd.cFileName);
            if (base.empty() || StartsWithUninstall(base)) continue;

            std::wstring full = dir + L"\\" + fd.cFileName;
            if (isLnk) {
                std::wstring target;
                if (lnk.Resolve(full, target) && !IsRunnableTarget(target))
                    continue; // .lnk to a doc/help file: drop
            }
            AppEntry e;
            e.target = full;
            e.isStore = false;
            e.lower = base;   // fold the copy we keep, not the original
            ToLowerInPlace(e.lower);
            e.name = std::move(base);
            out.push_back(std::move(e));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

// AppUserModelId property key {9F4C2855-9F79-4B39-A8D0-E1D42DE3D5F3},5.
// Hardcoded so no extra libs/headers are needed for one GUID.
static const PROPERTYKEY kKeyAumid = {
    {0x9F4C2855, 0x9F79, 0x4B39,
     {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE3, 0xD5, 0xF3}},
    5
};

static bool ItemToStoreApp(IShellItem* it, AppEntry& out) {
    LPWSTR name = NULL;
    if (it->GetDisplayName(SIGDN_NORMALDISPLAY, &name) != S_OK || !name || !*name) {
        if (name) CoTaskMemFree(name);
        return false;
    }
    bool ok = false;
    IShellItem2* i2 = NULL;
    if (it->QueryInterface(IID_IShellItem2, (void**)&i2) == S_OK) {
        LPWSTR aumid = NULL;
        if (i2->GetString(kKeyAumid, &aumid) == S_OK && aumid && *aumid) {
            // `name` is COM-allocated and freed below, so it must be copied twice here:
            // once for display, once folded for search. Fold the second copy in
            // place rather than running the whole name through a third buffer.
            out.name = name;
            out.lower = name;
            ToLowerInPlace(out.lower);
            out.target = std::wstring(L"shell:AppsFolder\\") + aumid;
            out.isStore = true;
            ok = true;
        }
        if (aumid) CoTaskMemFree(aumid);
        i2->Release();
    }
    CoTaskMemFree(name);
    return ok;
}

// Fallback path via IShellFolder::EnumObjects: same apps, different COM
// route, for machines where the ShellItem enumerator comes up empty.
static void ScanStoreAppsFallback(std::vector<AppEntry>& out) {
    PIDLIST_ABSOLUTE pidl = NULL;
    if (SHGetKnownFolderIDList(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, NULL, &pidl) != S_OK)
        return;
    IShellFolder* desktop = NULL;
    if (SHGetDesktopFolder(&desktop) == S_OK) {
        IShellFolder* appsF = NULL;
        if (desktop->BindToObject(pidl, NULL, IID_PPV_ARGS(&appsF)) == S_OK) {
            IEnumIDList* en = NULL;
            if (appsF->EnumObjects(NULL, SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN,
                                   &en) == S_OK) {
                for (;;) {
                    LPITEMIDLIST child = NULL;
                    if (en->Next(1, &child, NULL) != S_OK || !child) break;
                    LPITEMIDLIST full = ILCombine(pidl, child);
                    if (full) {
                        IShellItem* it = NULL;
                        if (SHCreateItemFromIDList(full, IID_PPV_ARGS(&it)) == S_OK) {
                            AppEntry e;
                            if (ItemToStoreApp(it, e)) out.push_back(std::move(e));
                            it->Release();
                        }
                        CoTaskMemFree(full);
                    }
                    CoTaskMemFree(child);
                    if (out.size() >= 6000) break;
                }
                en->Release();
            }
            appsF->Release();
        }
        desktop->Release();
    }
    CoTaskMemFree(pidl);
}

// Microsoft Store (UWP) apps via shell:AppsFolder - the only complete,
// launchable enumeration (Camera, Dolby Audio, ...).
static void ScanStoreApps(std::vector<AppEntry>& out) {
    PIDLIST_ABSOLUTE pidl = NULL;
    if (SHGetKnownFolderIDList(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, NULL, &pidl) != S_OK)
        return;
    IShellItem* folder = NULL;
    if (SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&folder)) == S_OK) {
        IEnumShellItems* en = NULL;
        if (SUCCEEDED(folder->BindToHandler(NULL, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
            for (;;) {
                IShellItem* it = NULL;
                ULONG n = 0;
                if (en->Next(1, &it, &n) != S_OK || n != 1 || !it) break;
                AppEntry e;
                if (ItemToStoreApp(it, e)) out.push_back(std::move(e));
                it->Release();
                if (out.size() >= 6000) break;
            }
            en->Release();
        }
        folder->Release();
    }
    CoTaskMemFree(pidl);
}

static void ScanPathExes(std::vector<AppEntry>& out) {
    wchar_t pathEnv[32767];
    DWORD n = GetEnvironmentVariableW(L"PATH", pathEnv, 32767);
    if (n == 0 || n >= 32767) return;
    std::wstring env(pathEnv, n);
    size_t start = 0;
    int dirsScanned = 0;
    while (start <= env.size() && dirsScanned < 40) {
        size_t end = env.find(L';', start);
        if (end == std::wstring::npos) end = env.size();
        std::wstring dir = env.substr(start, end - start);
        start = end + 1;
        if (dir.empty() || dir.size() > MAX_PATH) continue;
        std::wstring pattern = dir + L"\\*.exe";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        int perDir = 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            AppEntry e;
            e.target = dir + L"\\" + fd.cFileName;
            std::wstring fn = fd.cFileName;
            auto dot = fn.rfind(L'.');
            e.name = (dot == std::wstring::npos) ? fn : fn.substr(0, dot);
            e.lower = e.name;   // fold the copy we keep
            ToLowerInPlace(e.lower);
            e.isStore = false;
            out.push_back(std::move(e));
            if (++perDir > 150 || out.size() >= 6000) break;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        dirsScanned++;
        if (out.size() >= 6000) return;
    }
}

// JSON string decoding lives in minjson.h (shared with the unit tests).

std::wstring BuildChildEnv(const std::wstring& name, const std::wstring& value) {
    std::vector<std::wstring> vars;
    if (LPWCH parent = GetEnvironmentStringsW()) {
        for (LPWCH p = parent; *p; p += wcslen(p) + 1)
            vars.emplace_back(p);
        FreeEnvironmentStringsW(parent);
    }
    std::wstring prefix = name + L"=";
    vars.erase(std::remove_if(vars.begin(), vars.end(),
        [&](const std::wstring& v) {
            return v.size() >= prefix.size() &&
                   _wcsnicmp(v.c_str(), prefix.c_str(), prefix.size()) == 0;
        }), vars.end());
    vars.push_back(prefix + value);
    std::sort(vars.begin(), vars.end(), [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::wstring block;
    for (const auto& v : vars)
        block.append(v.c_str(), v.size() + 1);
    block.push_back(L'\0');
    return block;
}

// Tertiary source: Get-StartApps lists desktop + Store apps exactly like
// Start Menu search does (Camera, ...). One hidden PowerShell run, one-time
// cost on the background indexer thread; dedupe merges overlaps.
static void ScanStartAppsPS(std::vector<AppEntry>& out, const std::atomic<bool>* stop) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return;
    std::wstring file = std::wstring(tmp) + L"SneekPeek_apps_" +
        std::to_wstring(GetCurrentProcessId()) + L".json";
    // Explicit system PowerShell; no -ExecutionPolicy flag (-Command strings
    // are not subject to execution policy).
    wchar_t sysDir[MAX_PATH];
    std::wstring psExe = L"powershell.exe";
    if (GetSystemDirectoryW(sysDir, MAX_PATH))
        psExe = std::wstring(sysDir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    // Fixed script text: the output path travels in a private environment
    // block (sorted, deduplicated), never interpolated into script.
    std::wstring cmd = L"\"" + psExe + L"\" -NoProfile -NonInteractive -Command \"Get-StartApps | Select-Object Name,AppID | ConvertTo-Json -Compress | Out-File -Encoding utf8 $env:SNEEKPEEK_APPS_JSON\"";
    std::wstring envBlock = BuildChildEnv(L"SNEEKPEEK_APPS_JSON", file);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdLine(cmd.begin(), cmd.end());
    cmdLine.push_back(L'\0');
    if (!CreateProcessW(NULL, cmdLine.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        (LPVOID)envBlock.data(), NULL, &si, &pi))
        return;
    bool done = false;
    for (int i = 0; i < 20; i++) {
        if (WaitForSingleObject(pi.hProcess, 1000) == WAIT_OBJECT_0) { done = true; break; }
        if (stop && stop->load()) break; // shutting down: abandon the helper
    }
    if (!done) {
        TerminateProcess(pi.hProcess, 1); // stuck helper: kill, don't leak
        WaitForSingleObject(pi.hProcess, 5000); // let termination complete
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::string data;
    if (done) {
        HANDLE hf = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            char buf[8192];
            DWORD rd = 0;
            while (ReadFile(hf, buf, sizeof(buf), &rd, NULL) && rd) {
                data.append(buf, rd);
                if (data.size() > 1048576) break;
            }
            CloseHandle(hf);
        }
    }
    DeleteFileW(file.c_str()); // every exit path cleans the temp file
    if (!done) {
        // Best effort already made above (wait + terminate + wait); TEMP
        // self-cleans anything left behind. One retry for slow filesystems:
        Sleep(100);
        DeleteFileW(file.c_str());
        return;
    }
    if (data.size() > 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        data.erase(0, 3); // PS5.1 UTF-8 BOM

    size_t pos = 0;
    while (pos < data.size() && out.size() < 6000) {
        size_t kn = data.find("\"Name\"", pos);
        if (kn == std::string::npos) break;
        size_t q1 = data.find('"', kn + 6);
        if (q1 == std::string::npos) break;
        const char* pp = data.c_str() + q1;
        const char* end = data.c_str() + data.size();
        std::wstring name;
        if (!DecodeJsonString(pp, end, name)) { pos = q1 + 1; continue; }
        size_t ka = data.find("\"AppID\"", (size_t)(pp - data.c_str()));
        if (ka == std::string::npos) { pos = q1 + 1; continue; }
        size_t q2 = data.find('"', ka + 7);
        if (q2 == std::string::npos) break;
        pp = data.c_str() + q2;
        std::wstring appid;
        if (!DecodeJsonString(pp, end, appid)) { pos = q2 + 1; continue; }
        pos = (size_t)(pp - data.c_str());
        if (name.empty() || appid.empty()) continue;
        if (StartsWithUninstall(name)) continue;
        // Docs/help/URLs are not apps (mirrors the .lnk scan's policy).
        if (appid.compare(0, 7, L"file://") == 0 ||
            appid.compare(0, 5, L"file:") == 0)
            continue;
        AppEntry e;
        e.name = name;
        e.lower = name;   // fold the copy we keep
        ToLowerInPlace(e.lower);
        if (appid.find(L'!') != std::wstring::npos) {
            e.target = L"shell:AppsFolder\\" + appid; // Store app
            e.isStore = true;
        } else if (IsAbsoluteFilePath(appid)) {
            // Direct path (e.g. "F:\7-Zip\7zFM.exe"): launch as-is, but
            // drop non-runnable files (.txt/.chm/.url/...) like ScanLnkDir.
            size_t dot = appid.find_last_of(L'.');
            size_t sep = appid.find_last_of(L"\\/");
            if (dot != std::wstring::npos &&
                (sep == std::wstring::npos || dot > sep)) {
                // Same runnable-extension policy as ScanLnkDir, plus .lnk
                // (Start-apps can hand back a link to a link).
                std::wstring ext = appid.substr(dot + 1);
                ToLowerInPlace(ext);
                if (!(ext == L"exe" || ext == L"msc" || ext == L"bat" ||
                      ext == L"cmd" || ext == L"com" || ext == L"pif" ||
                      ext == L"scr" || ext == L"cpl" || ext == L"lnk"))
                    continue; // doc/help/url file, not an app
            }
            e.target = appid;
            e.isStore = false;
        } else {
            // Bare Start-menu ID ("Microsoft.Windows.Explorer",
            // "{GUID}\notepad.exe", "MSEdge", ...): only launchable via
            // shell:AppsFolder, exactly like Store apps.
            e.target = L"shell:AppsFolder\\" + appid;
            e.isStore = true;
        }
        out.push_back(std::move(e));
    }
}

void BuildAppIndex(std::vector<AppEntry>& out, bool includePathExes,
                   const std::atomic<bool>* stop, bool skipHeavy) {
    out.clear();
    out.reserve(900);

    // COM for .lnk resolve + Store enumeration (this runs on a worker thread).
    bool com = SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
                                              COINIT_DISABLE_OLE1DDE));

    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, buf)))
        ScanLnkDir(buf, out);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, buf)))
        ScanLnkDir(buf, out);
    ScanStoreApps(out);
    if (stop && stop->load()) { if (com) CoUninitialize(); return; }
    if (!skipHeavy) {
        ScanStoreAppsFallback(out); // second COM route; dedupe merges overlaps
        if (stop && stop->load()) { if (com) CoUninitialize(); return; }
        ScanStartAppsPS(out, stop); // PowerShell Start list; dedupe merges
        if (stop && stop->load()) { if (com) CoUninitialize(); return; }
    }
    if (includePathExes)
        ScanPathExes(out);

    if (com) CoUninitialize();

    // Sort by name; .lnk wins ties over Store/PATH dupes. Then de-dupe.
    // One compare() instead of != followed by <: the old form scanned every
    // name twice per sort comparison (thousands of entries at most).
    std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) {
        int c = a.lower.compare(b.lower);
        if (c != 0) return c < 0;
        return (a.isStore ? 1 : 0) < (b.isStore ? 1 : 0);
    });
    std::vector<AppEntry> uniq;
    uniq.reserve(out.size());
    // Compare against the last kept entry instead of copying into `prev`,
    // which allocated one string per unique app (~6000 on a big machine).
    for (auto& e : out) {
        if (!uniq.empty() && e.lower == uniq.back().lower) continue;
        uniq.push_back(std::move(e));
    }
    out.swap(uniq);
}

static bool IsWordBoundary(wchar_t prev) {
    return prev == L' ' || prev == L'-' || prev == L'_' || prev == L'.' ||
           prev == L'/' || prev == L'\\' || prev == L'(' || prev == L'[';
}

int FuzzyScore(const std::wstring& nameLower, const std::wstring& queryLower) {
    if (queryLower.empty()) return 0;
    if (nameLower.empty() || queryLower.size() > nameLower.size()) return -1;

    // 1) Exact + prefix: strongest signals.
    if (nameLower == queryLower) return 100000;
    if (nameLower.compare(0, queryLower.size(), queryLower) == 0)
        return 50000 - (int)nameLower.size();

    // 2) Contiguous substring, word-boundary hits rank higher.
    size_t pos = nameLower.find(queryLower);
    if (pos != std::wstring::npos) {
        int base = (pos == 0 || IsWordBoundary(nameLower[pos - 1])) ? 20000 : 8000;
        return base - (int)pos * 8 - (int)nameLower.size() / 4;
    }

    // 3) Fuzzy subsequence with bonuses: word starts, camel humps,
    //    consecutive runs; gaps and long names penalized.
    int total = 0;
    int gaps = 0;
    size_t ni = 0;
    int consec = 0;
    for (size_t qi = 0; qi < queryLower.size(); qi++) {
        size_t f = nameLower.find(queryLower[qi], ni);
        if (f == std::wstring::npos) return -1;
        if (qi == 0 && f == 0) {
            total += 30; // matches the very first character
            consec = 1;
        } else if (f == 0 || IsWordBoundary(nameLower[f - 1])) {
            total += 16;
            consec = 1;
        } else if (f == ni) {
            total += 10 + consec * 2; // consecutive run bonus grows
            consec++;
        } else {
            total += 1;
            consec = 0;
            gaps += (int)(f - ni);
        }
        ni = f + 1;
    }
    total -= gaps * 2 + (int)nameLower.size() / 8;
    return total > 0 ? total : 1; // every full subsequence still matches, weakly
}

// Filter + rank against an ALREADY lowercased, trimmed query. The palette
// lowercases the query once per keystroke and reuses it for apps, settings
// pages and chips, so this variant skips that copy.
std::vector<ScoredApp> SearchAppsLower(const std::vector<AppEntry>& apps,
                                       const std::wstring& q,
                                       size_t limit) {
    if (q.empty() || limit == 0) return {};
    std::vector<ScoredApp> hits;
    hits.reserve(32);
    for (const auto& app : apps) {
        int s = FuzzyScore(app.lower, q);
        if (s >= 0) hits.push_back({&app, s});
    }
    // Results are ALWAYS returned best-first, so the sort must run even when
    // everything already fits under the limit. A 1-2 char query matches most
    // of the index as a weak subsequence, so only the prefix of that size is
    // ranked rather than the whole match list.
    auto better = [](const ScoredApp& x, const ScoredApp& y) {
        return x.score > y.score;
    };
    if (hits.size() > limit) {
        std::partial_sort(hits.begin(), hits.begin() + (std::ptrdiff_t)limit,
                          hits.end(), better);
        hits.resize(limit);
    } else {
        std::sort(hits.begin(), hits.end(), better);
    }
    return hits;
}

std::vector<ScoredApp> SearchApps(const std::vector<AppEntry>& apps,
                                 const std::wstring& query,
                                 size_t limit) {
    std::wstring q = ToLower(query);
    size_t a = q.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return {};
    size_t b = q.find_last_not_of(L" \t");
    q = q.substr(a, b - a + 1);
    return SearchAppsLower(apps, q, limit);
}

// ------------------------------------------------------- persisted index cache
// Format: magic u32 ("SPIC" LE), version u32, count u32, then per entry:
// nameLen u32 + UTF-16 chars, targetLen u32 + UTF-16 chars, flags u32.
// lower[] is recomputed on load, never stored.
static const uint32_t kAppCacheMagic = 0x43495053;
static const uint32_t kAppCacheVersion = 2; // v2: StartApps IDs launch via shell:AppsFolder

static void WriteU32(std::string& b, uint32_t v) {
    b.push_back((char)(v & 0xFF));
    b.push_back((char)((v >> 8) & 0xFF));
    b.push_back((char)((v >> 16) & 0xFF));
    b.push_back((char)((v >> 24) & 0xFF));
}
static void WriteWStr(std::string& b, const std::wstring& s) {
    WriteU32(b, (uint32_t)s.size());
    b.append((const char*)s.data(), s.size() * sizeof(wchar_t));
}
bool SaveAppCache(const std::wstring& path, const std::vector<AppEntry>& apps) {
    std::string b;
    WriteU32(b, kAppCacheMagic);
    WriteU32(b, kAppCacheVersion);
    size_t n = apps.size() < 20000 ? apps.size() : 20000;
    WriteU32(b, (uint32_t)n);
    for (size_t i = 0; i < n; i++) {
        WriteWStr(b, apps[i].name);
        WriteWStr(b, apps[i].target);
        WriteU32(b, apps[i].isStore ? 1u : 0u);
    }
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(hf, b.data(), (DWORD)b.size(), &wrote, NULL);
    CloseHandle(hf);
    return ok && wrote == b.size();
}
static bool ReadU32(const char*& p, const char* end, uint32_t& v) {
    if ((size_t)(end - p) < 4) return false;
    const unsigned char* u = (const unsigned char*)p;
    v = (uint32_t)u[0] | ((uint32_t)u[1] << 8) |
        ((uint32_t)u[2] << 16) | ((uint32_t)u[3] << 24);
    p += 4;
    return true;
}
static bool ReadWStr(const char*& p, const char* end, std::wstring& s) {
    uint32_t n = 0;
    if (!ReadU32(p, end, n) || n > 32768) return false;
    if ((size_t)(end - p) < (size_t)n * sizeof(wchar_t)) return false;
    s.assign((const wchar_t*)p, n);
    p += n * sizeof(wchar_t);
    return true;
}
bool LoadAppCache(const std::wstring& path, std::vector<AppEntry>& out) {
    out.clear();
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, 0, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;
    std::string data;
    char buf[8192];
    DWORD rd = 0;
    while (ReadFile(hf, buf, sizeof(buf), &rd, NULL) && rd) {
        data.append(buf, rd);
        if (data.size() > 8 * 1024 * 1024) break;
    }
    CloseHandle(hf);
    const char* p = data.c_str();
    const char* end = p + data.size();
    uint32_t magic = 0, ver = 0, count = 0;
    if (!ReadU32(p, end, magic) || magic != kAppCacheMagic) return false;
    if (!ReadU32(p, end, ver) || ver != kAppCacheVersion) return false;
    if (!ReadU32(p, end, count) || count > 20000) return false;
    std::vector<AppEntry> fresh;
    fresh.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        AppEntry e;
        uint32_t flags = 0;
        if (!ReadWStr(p, end, e.name) || !ReadWStr(p, end, e.target) ||
            !ReadU32(p, end, flags))
            return false;
        e.lower = e.name;   // recomputed, never stored: fold the copy we keep
        ToLowerInPlace(e.lower);
        e.isStore = (flags & 1u) != 0;
        fresh.push_back(std::move(e));
    }
    if (p != end) return false; // trailing garbage: reject
    out.swap(fresh);
    return true;
}
