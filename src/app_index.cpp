#include "app_index.h"
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>

std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static void ScanLnkDir(const std::wstring& root, std::vector<AppEntry>& out) {
    // Iterative stack-based recursive scan (no recursion, tiny stack use).
    std::vector<std::wstring> stack;
    stack.push_back(root);
    wchar_t buf[MAX_PATH];

    while (!stack.empty()) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();

        std::wstring pattern = dir + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.') {
                // Skip "." / ".." but allow ".hidden" files? Keep simple: skip dot-dirs.
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (fd.cFileName[1] == L'\0' ||
                        (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))
                        continue;
                }
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(dir + L"\\" + fd.cFileName);
            } else {
                const wchar_t* ext = PathFindExtensionW(fd.cFileName);
                if (_wcsicmp(ext, L".lnk") == 0 || _wcsicmp(ext, L".url") == 0) {
                    AppEntry e;
                    e.target = dir + L"\\" + fd.cFileName;
                    // Display name = filename without extension.
                    std::wstring n = fd.cFileName;
                    n.resize(ext - fd.cFileName);
                    e.name = n;
                    out.push_back(std::move(e));
                } else if (_wcsicmp(ext, L".exe") == 0) {
                    // Rare inside start-menu dirs, but handle it.
                    AppEntry e;
                    e.target = dir + L"\\" + fd.cFileName;
                    std::wstring n = fd.cFileName;
                    n.resize(ext - fd.cFileName);
                    e.name = n;
                    out.push_back(std::move(e));
                }
            }
            // Hard cap: stay light even on pathological machines.
            if (out.size() >= 4000) { FindClose(h); return; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        (void)buf;
    }
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
        // Skip system32 churn? No - keep, but cap files per dir.
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
            out.push_back(std::move(e));
            if (++perDir > 150 || out.size() >= 5000) break;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        dirsScanned++;
        if (out.size() >= 5000) return;
    }
}

void BuildAppIndex(std::vector<AppEntry>& out, bool includePathExes) {
    out.clear();
    out.reserve(600);

    wchar_t buf[MAX_PATH];

    // 1) Per-user Start Menu
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, buf)))
        ScanLnkDir(buf, out);
    // 2) All-users Start Menu
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, buf)))
        ScanLnkDir(buf, out);
    // 3) Optional PATH scan (off by default - costs RAM)
    if (includePathExes)
        ScanPathExes(out);

    // De-dupe by lowercase name, keep first (user dir wins).
    std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) {
        return ToLower(a.name) < ToLower(b.name);
    });
    std::vector<AppEntry> uniq;
    uniq.reserve(out.size());
    std::wstring prev;
    bool first = true;
    for (auto& e : out) {
        std::wstring k = ToLower(e.name);
        if (!first && k == prev) continue; // dupe name - drop (keeps memory flat)
        uniq.push_back(std::move(e));
        prev = k;
        first = false;
    }
    out.swap(uniq);
}

int FuzzyScore(const std::wstring& nameLower, const std::wstring& queryLower) {
    if (queryLower.empty()) return 0;
    if (nameLower.empty()) return -1;

    // 1) Prefix match - best.
    if (nameLower.compare(0, queryLower.size(), queryLower) == 0)
        return 10000 - (int)nameLower.size(); // shorter names rank higher

    // 2) Word-prefix match ("Visual Studio Code" vs "code").
    size_t pos = nameLower.find(queryLower);
    if (pos != std::wstring::npos) {
        bool atWordStart = (pos == 0 || nameLower[pos - 1] == L' ' ||
                            nameLower[pos - 1] == L'-' || nameLower[pos - 1] == L'_');
        return (atWordStart ? 5000 : 3000) - (int)pos - (int)nameLower.size() / 4;
    }

    // 3) Subsequence (fuzzy initials "vsc" -> "Visual Studio Code").
    size_t qi = 0;
    int gaps = 0;
    size_t lastHit = 0;
    for (size_t i = 0; i < nameLower.size() && qi < queryLower.size(); ++i) {
        if (nameLower[i] == queryLower[qi]) {
            if (qi > 0) gaps += (int)(i - lastHit - 1);
            lastHit = i;
            qi++;
        }
    }
    if (qi == queryLower.size())
        return 1000 - gaps * 10 - (int)nameLower.size() / 8;

    return -1;
}

std::vector<ScoredApp> SearchApps(const std::vector<AppEntry>& apps,
                                 const std::wstring& query,
                                 size_t limit) {
    std::wstring q = ToLower(query);
    // trim
    size_t a = q.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return {};
    size_t b = q.find_last_not_of(L" \t");
    q = q.substr(a, b - a + 1);
    if (q.empty()) return {};

    std::vector<ScoredApp> hits;
    hits.reserve(32);
    for (const auto& app : apps) {
        int s = FuzzyScore(ToLower(app.name), q);
        if (s >= 0) hits.push_back({&app, s});
    }
    std::sort(hits.begin(), hits.end(),
              [](const ScoredApp& x, const ScoredApp& y) { return x.score > y.score; });
    if (hits.size() > limit) hits.resize(limit);
    return hits;
}
