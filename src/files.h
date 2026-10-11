#pragma once
// SneekPeek - single-level file browser (names + count only, no recursion).
// Header-only. Pure helpers are unit-tested; Win32 wrappers stay thin so the
// palette never pays for more than one FindFirstFileW per keystroke.
//
// Cost model: one directory scan per UpdateResults(), names only (isDir comes
// free inside WIN32_FIND_DATAW). No sizes, dates, icons, subfolder or parent
// scans, no watchers, no cache. State is one g_fileDir string, cleared on hide.
#include <windows.h>
#include <cwctype>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <algorithm>

struct FileEntry {
    std::wstring name; // file or folder name only, never a path
    bool isDir = false;
};

// Cap a single scan so a pathological folder (100k entries) can't balloon RAM.
// Normal folders are fully listed; beyond the cap we report "10000+ items".
inline constexpr size_t kMaxFileScan = 10000;
// Rows shown under one header (header + rows fit the 9-row palette).
inline constexpr size_t kMaxFileRows = 8;

inline std::wstring FileLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Substring filter, case-insensitive via pre-lowered inputs. Empty filter matches all.
inline bool FileMatchesFilter(const std::wstring& nameLower, const std::wstring& filterLower) {
    if (filterLower.empty()) return true;
    if (nameLower.size() < filterLower.size()) return false;
    return nameLower.find(filterLower) != std::wstring::npos;
}

// Folders first, then case-insensitive alphabetical. Pure, unit-tested.
inline void SortFileEntries(std::vector<FileEntry>& v) {
    std::sort(v.begin(), v.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        int c = _wcsicmp(a.name.c_str(), b.name.c_str());
        if (c != 0) return c < 0;
        return a.name < b.name;
    });
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

// ------------------------------------------------------- drive labels
// Volume name for one drive root ("C:\\" -> "Win 10", "" when unreadable).
// Called only on file-mode entry / return-to-drives, never per keystroke,
// so slow volumes (empty DVD, offline network) can't stall filtering.
struct DriveInfo {
    std::wstring root;  // "C:\\" (navigation target)
    std::wstring label; // "Win 10" (may be empty)
    std::wstring title; // "Win 10 (C:)" or "C:\\" (row text)
};

// Explorer-style row text. Pure, unit-tested.
inline std::wstring DriveTitle(const std::wstring& root, const std::wstring& label) {
    if (label.empty()) return root;
    std::wstring letter = root.size() >= 2 ? root.substr(0, 2) : root; // "C:"
    return label + L" (" + letter + L")";
}

inline std::wstring GetDriveLabel(const std::wstring& root) {
    wchar_t vol[MAX_PATH + 1] = {0};
    if (GetVolumeInformationW(root.c_str(), vol, MAX_PATH + 1,
                              NULL, NULL, NULL, NULL, 0))
        return vol;
    return L"";
}

// ------------------------------------------------------- Win32 enumeration
// Drive letters only ("C:\\"), no volume labels (those block on I/O).
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
            FileEntry e;
            e.name = fd.cFileName;
            e.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            out.push_back(std::move(e));
        } else {
            truncated = true;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}
