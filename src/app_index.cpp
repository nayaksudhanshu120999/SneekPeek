#include "app_index.h"
#include "minjson.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <algorithm>

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
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
static bool ResolveLnk(const std::wstring& lnk, std::wstring& target) {
    IShellLinkW* sl = NULL;
    if (CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                         IID_IShellLinkW, (void**)&sl) != S_OK)
        return false;
    IPersistFile* pf = NULL;
    bool ok = false;
    if (sl->QueryInterface(IID_IPersistFile, (void**)&pf) == S_OK) {
        if (pf->Load(lnk.c_str(), STGM_READ) == S_OK) {
            wchar_t path[1024] = {0};
            WIN32_FIND_DATAW fd{};
            if (sl->GetPath(path, 1024, &fd, SLGP_UNCPRIORITY) == S_OK && path[0]) {
                target = ExpandEnvLocal(path);
                ok = true;
            }
        }
        pf->Release();
    }
    sl->Release();
    return ok;
}

// Only executable targets belong in the index: drops .txt/.chm/.pdf links etc.
static bool IsRunnableTarget(const std::wstring& target) {
    size_t dot = target.find_last_of(L'.');
    size_t sep = target.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep))
        return true; // no extension: keep (some tools are extensionless)
    std::wstring ext = ToLower(target.substr(dot + 1));
    return ext == L"exe" || ext == L"msc" || ext == L"bat" ||
           ext == L"cmd" || ext == L"com" || ext == L"pif" ||
           ext == L"scr" || ext == L"cpl";
}

static bool StartsWithUninstall(const std::wstring& name) {
    std::wstring n = ToLower(name);
    return n.compare(0, 9, L"uninstall") == 0;
}

static void ScanLnkDir(const std::wstring& root, std::vector<AppEntry>& out) {
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
                if (ResolveLnk(full, target) && !IsRunnableTarget(target))
                    continue; // .lnk to a doc/help file: drop
            }
            AppEntry e;
            e.name = base;
            e.lower = ToLower(base);
            e.target = full;
            e.isStore = false;
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
            out.name = name;
            out.lower = ToLower(name);
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
            e.lower = ToLower(e.name);
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

// Tertiary source: Get-StartApps lists desktop + Store apps exactly like
// Start Menu search does (Camera, ...). One hidden PowerShell run, one-time
// cost on the background indexer thread; dedupe merges overlaps.
static void ScanStartAppsPS(std::vector<AppEntry>& out) {
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
    // block, never interpolated into script (quote-proof by construction).
    std::wstring cmd = L"\"" + psExe + L"\" -NoProfile -NonInteractive -Command \"Get-StartApps | Select-Object Name,AppID | ConvertTo-Json -Compress | Out-File -Encoding utf8 $env:SNEEKPEEK_APPS_JSON\"";
    std::wstring envBlock;
    if (LPWCH parent = GetEnvironmentStringsW()) {
        for (LPWCH p = parent; *p; p += wcslen(p) + 1)
            envBlock.append(p, wcslen(p) + 1);
        FreeEnvironmentStringsW(parent);
    }
    std::wstring var = L"SNEEKPEEK_APPS_JSON=" + file;
    envBlock.append(var.c_str(), var.size() + 1);
    envBlock.push_back(L'\0');
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
    bool done = WaitForSingleObject(pi.hProcess, 20000) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(pi.hProcess, 1); // stuck helper: kill, don't leak
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
    if (!done) return;
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
        AppEntry e;
        e.name = name;
        e.lower = ToLower(name);
        if (appid.find(L'!') != std::wstring::npos) {
            e.target = L"shell:AppsFolder\\" + appid; // Store app
            e.isStore = true;
        } else {
            e.target = appid; // classic app: full .lnk path
            e.isStore = false;
        }
        out.push_back(std::move(e));
    }
}

void BuildAppIndex(std::vector<AppEntry>& out, bool includePathExes) {
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
    ScanStoreAppsFallback(out); // second COM route; dedupe merges overlaps
    ScanStartAppsPS(out);       // PowerShell Start list; dedupe merges
    if (includePathExes)
        ScanPathExes(out);

    if (com) CoUninitialize();

    // Sort by name; .lnk wins ties over Store/PATH dupes. Then de-dupe.
    std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) {
        if (a.lower != b.lower) return a.lower < b.lower;
        return (a.isStore ? 1 : 0) < (b.isStore ? 1 : 0);
    });
    std::vector<AppEntry> uniq;
    uniq.reserve(out.size());
    std::wstring prev;
    bool first = true;
    for (auto& e : out) {
        if (!first && e.lower == prev) continue;
        prev = e.lower;
        uniq.push_back(std::move(e));
        first = false;
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

std::vector<ScoredApp> SearchApps(const std::vector<AppEntry>& apps,
                                 const std::wstring& query,
                                 size_t limit) {
    std::wstring q = ToLower(query);
    size_t a = q.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return {};
    size_t b = q.find_last_not_of(L" \t");
    q = q.substr(a, b - a + 1);
    if (q.empty()) return {};

    std::vector<ScoredApp> hits;
    hits.reserve(32);
    for (const auto& app : apps) {
        int s = FuzzyScore(app.lower, q);
        if (s >= 0) hits.push_back({&app, s});
    }
    std::sort(hits.begin(), hits.end(),
              [](const ScoredApp& x, const ScoredApp& y) { return x.score > y.score; });
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}
