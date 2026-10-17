#pragma once
// SneekPeek - single-level file browser (names + count only, no recursion).
// Header-only. Pure helpers are unit-tested; Win32 wrappers stay thin so the
// palette never pays for more than one FindFirstFileW per keystroke.
//
// Cost model: one directory scan per UpdateResults(), names only (isDir comes
// free inside WIN32_FIND_DATAW). No sizes, dates, icons, subfolder or parent
// scans, no watchers, no cache. State is one g_fileDir string, cleared on hide.
#include <windows.h>
#include <shlwapi.h> // StrCmpLogicalW: Explorer's natural sort order
#include <cwctype>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>
#include <utility>

#include "app_index.h" // FuzzyScore: one match engine for apps and files

struct FileEntry {
    std::wstring name; // file or folder name only, never a path
    bool isDir = false;
};

// Cap a single scan so a pathological folder (100k entries) can't balloon RAM.
// Normal folders are fully listed; beyond the cap we report "10000+ items".
inline constexpr size_t kMaxFileScan = 10000;
// Display cap for the suggestion list: filtering always sees every scanned
// name, but at most this many rows are pushed per refresh so a huge folder
// (e.g. System32) can't stall a keystroke with 10k listbox inserts.
inline constexpr size_t kMaxFileList = 1000;

inline std::wstring FileLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// File-mode trigger: a lone ":" token followed by space/tab (": ", ": query").
// A keyword trigger ("file") collided with app searches like "file manager",
// so the trigger is punctuation-only. Pure, unit-tested.
inline bool IsFileTrigger(const std::wstring& raw) {
    size_t sp = raw.find(L' ');
    size_t tb = raw.find(L'\t');
    size_t p = sp;
    if (p == std::wstring::npos || (tb != std::wstring::npos && tb < p)) p = tb;
    if (p == std::wstring::npos) return false;
    return raw.substr(0, p) == L":";
}

// Fuzzy match quality, shared with app search: exact (100000) > prefix
// (50000-) > word-boundary/substring > subsequence, -1 = no match,
// 0 = neutral (empty filter). Higher is better. Pure, unit-tested.
inline int FileFuzzyScore(const std::wstring& nameLower, const std::wstring& filterLower) {
    return FuzzyScore(nameLower, filterLower);
}

// Coarse tier so ranking stays predictable: exact (1) beats contiguous
// substring (2) beats fuzzy subsequence (3); -1 never matches. Note that
// FuzzyScore's prefix bonus (50000 - nameLen) sits just below the tier-1
// threshold, so a prefix is tier 2 with pos 0 -- which still outranks every
// other substring because position is compared within the tier.
// Within a tier, earliest occurrence wins (keeps "mydocs" ahead of "ado.doc"
// for "doc"), then Explorer's natural order. Pure, unit-tested.
//
// posOut also reports the first literal occurrence of the filter (npos for a
// pure subsequence match, 0 for exact/prefix). RankedNameLess needs both tier
// and position, and re-running find() there doubled the scan work on every
// comparison.
inline int FileScoreTierFind(const std::wstring& nameLower,
                             const std::wstring& filterLower, size_t& posOut) {
    posOut = std::wstring::npos;
    if (filterLower.empty()) return 0;
    int s = FileFuzzyScore(nameLower, filterLower);
    if (s < 0) return -1;
    if (s >= 50000) { posOut = 0; return 1; } // exact match only: starts at 0
    posOut = nameLower.find(filterLower);
    return posOut != std::wstring::npos ? 2 : 3;
}
inline int FileScoreTier(const std::wstring& nameLower, const std::wstring& filterLower) {
    size_t ignore;
    return FileScoreTierFind(nameLower, filterLower, ignore);
}

// Substring filter, now fuzzy-aware: a subsequence match counts (so "vsc"
// finds "Visual Studio Code"). Empty filter matches all. Pure, unit-tested.
inline bool FileMatchesFilter(const std::wstring& nameLower, const std::wstring& filterLower) {
    if (filterLower.empty()) return true;
    return FileFuzzyScore(nameLower, filterLower) >= 0;
}

// Shared ordering for every ranked name list in the app: best tier first,
// then earliest occurrence, then Explorer's natural logical order
// (StrCmpLogicalW: digits sort numerically, case-insensitive). Empty filter
// = pure Explorer order.
inline bool RankedNameLess(const std::wstring& aLower, const std::wstring& bLower,
                           const std::wstring& filterLower,
                           const std::wstring& aOrig, const std::wstring& bOrig) {
    if (!filterLower.empty()) {
        size_t pa, pb;
        int ra = FileScoreTierFind(aLower, filterLower, pa);
        int rb = FileScoreTierFind(bLower, filterLower, pb);
        if (ra != rb) {
            if (ra < 0) return false;
            if (rb < 0) return true;
            return ra < rb;
        }
        if (pa != pb) return pa < pb;
    }
    int c = StrCmpLogicalW(aOrig.c_str(), bOrig.c_str());
    if (c != 0) return c < 0;
    return aOrig < bOrig;
}

// Folders first, then best fuzzy match first, then Explorer natural order.
// Empty filter keeps the classic folders-first Explorer order.
inline void SortFileEntries(std::vector<FileEntry>& v, const std::wstring& filterLower = L"") {
    std::vector<std::wstring> low(v.size());
    for (size_t i = 0; i < v.size(); i++) low[i] = FileLower(v[i].name);
    std::vector<size_t> idx(v.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        if (v[a].isDir != v[b].isDir) return v[a].isDir > v[b].isDir;
        return RankedNameLess(low[a], low[b], filterLower, v[a].name, v[b].name);
    });
    std::vector<FileEntry> sorted;
    sorted.reserve(v.size());
    for (size_t i = 0; i < idx.size(); i++) sorted.push_back(std::move(v[idx[i]]));
    v.swap(sorted);
}

// Pure path helpers (no filesystem access), unit-tested.
inline std::wstring EnsureTrailingSlash(const std::wstring& dir) {
    if (dir.empty()) return dir;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir;
    return dir + L"\\";
}

inline std::wstring JoinFilePath(const std::wstring& dir, const std::wstring& name) {
    return EnsureTrailingSlash(dir) + name;
}

// Parent of a trailing-slash dir. "C:\\" -> "" (= drive list level).
// "C:\\a\\b\\" -> "C:\\a\\". "" stays "".
inline std::wstring ParentDirOf(const std::wstring& dir) {
    if (dir.empty()) return L"";
    std::wstring t = dir;
    while (!t.empty() && (t.back() == L'\\' || t.back() == L'/')) t.pop_back();
    if (t.empty()) return L"";
    // Drive root like "C:" -> drive-list level.
    if (t.size() == 2 && t[1] == L':' &&
        ((t[0] >= L'A' && t[0] <= L'Z') || (t[0] >= L'a' && t[0] <= L'z')))
        return L"";
    size_t p = t.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    return t.substr(0, p + 1);
}

inline bool IsDriveRootDir(const std::wstring& dir) {
    return ParentDirOf(EnsureTrailingSlash(dir)).empty() && !dir.empty();
}

// "3 items" / "1 item" / "0 items". Pure, unit-tested.
inline std::wstring FormatFileCount(size_t n) {
    wchar_t b[64];
    if (n == 1) swprintf_s(b, 64, L"1 item");
    else swprintf_s(b, 64, L"%u items", (unsigned)n);
    return b;
}

// ------------------------------------------------------- drives
// Drive rows are the bare letter ("C:\\"), never the volume name.
//
// This used to read the label via GetVolumeInformationW and show
// "Win 10 (C:)". That call blocks for as long as the OS retries a volume that
// isn't answering: an empty optical drive or an offline network share froze the
// palette for seconds on the ": " that opens file mode, and again on every
// return to the drive list. Dropping labels removes the only volume I/O on the
// file-mode path, so entering and leaving drives is now instant regardless of
// what is or isn't mounted.
struct DriveInfo {
    std::wstring root;  // "C:\\" (navigation target)
    std::wstring title; // "C:\\" (row text; kept separate for the sort keys)
};

// Drive list ordering: empty filter keeps Explorer letter order (the
// enumeration order of GetLogicalDrivesWin32). With a filter, best fuzzy
// score first; full ties keep letter order (stable_sort).
// Headless-testable; the palette sorts DriveInfos before building rows.
//
// `low` holds the lowercased titles parallel to `v`. The caller already
// lowercased them to run the filter, so passing them in avoids redoing that
// per comparison; if it is empty they are derived here. Sorting runs over an
// index permutation so the comparator itself never allocates.
inline void SortDrivesByRank(std::vector<DriveInfo>& v,
                             const std::vector<std::wstring>& low,
                             const std::wstring& filterLower) {
    if (filterLower.empty()) return;
    std::vector<std::wstring> owned;
    if (low.size() != v.size()) {
        owned.resize(v.size());
        for (size_t i = 0; i < v.size(); i++) owned[i] = FileLower(v[i].title);
    }
    const std::vector<std::wstring>& t =
        (low.size() == v.size()) ? low : owned;
    std::vector<size_t> idx(v.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t x, size_t y) {
        size_t pa, pb;
        int ra = FileScoreTierFind(t[x], filterLower, pa);
        int rb = FileScoreTierFind(t[y], filterLower, pb);
        if (ra != rb) {
            if (ra < 0) return false;
            if (rb < 0) return true;
            return ra < rb;
        }
        if (pa != pb) return pa < pb;
        return false; // tie: keep Explorer letter order
    });
    std::vector<DriveInfo> out;
    out.reserve(v.size());
    for (size_t i = 0; i < idx.size(); i++) out.push_back(std::move(v[idx[i]]));
    v.swap(out);
}

// ------------------------------------------------------- Win32 enumeration
// Drive letters only ("C:\\"). No volume I/O at all: reading a label blocks on
// volumes that do not answer (empty optical drive, offline share), so nothing
// here touches the disk and the drive list is always instant.
inline std::vector<std::wstring> GetLogicalDrivesWin32() {
    std::vector<std::wstring> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (mask & (1u << i)) {
            std::wstring d;
            d.push_back((wchar_t)(L'A' + i));
            d += L":\\";
            out.push_back(d);
        }
    }
    return out;
}

// Single-level scan of `dir` only. Skips "." / ".." and hidden entries.
// Never recurses.
// out holds up to kMaxFileScan names; total is the true count (capped flag via
// truncated=true when the dir holds more than the cap). False = dir unreadable.
inline bool ListDirOneLevel(const std::wstring& dir, std::vector<FileEntry>& out,
                            size_t& total, bool& truncated) {
    out.clear();
    total = 0;
    truncated = false;
    if (dir.empty()) return false;
    // Reserve up front: growth would otherwise reallocate and copy every name
    // already collected (this runs once per keystroke while browsing).
    out.reserve(256);
    std::wstring pattern = EnsureTrailingSlash(dir) + L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' ||
             (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0')))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)
            continue; // hidden files/folders stay out of the list (and count)
        total++;
        if (out.size() < kMaxFileScan) {
            out.emplace_back(FileEntry{
                std::wstring(fd.cFileName),
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
        } else {
            truncated = true;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}
