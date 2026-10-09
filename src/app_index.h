#pragma once
// SneekPeek - app index: Start Menu (.lnk, exe-targets only) + Microsoft Store
// (UWP) apps + optional PATH scan. Lazy one-time scan, ~100KB RAM, 0% idle CPU.

#include <string>
#include <vector>

struct AppEntry {
    std::wstring name;    // display name ("Visual Studio Code", "Camera")
    std::wstring target;  // .lnk path, or "shell:AppsFolder\<AUMID>" for Store
    bool isStore = false; // Store apps launch via explorer.exe + target
};

struct ScoredApp {
    const AppEntry* app;
    int score;
};

// Scan Start Menu + Store apps + optional PATH exes. Runs once, on first hotkey.
// Initializes COM on the calling thread (needed for .lnk resolve + Store enum).
void BuildAppIndex(std::vector<AppEntry>& out, bool includePathExes);

// Fuzzy score: exact > prefix > word-boundary > substring > subsequence
// (with consecutive / boundary bonuses). Returns -1 = no match.
int FuzzyScore(const std::wstring& nameLower, const std::wstring& queryLower);

// Filter + rank. Returns pointers into `apps`, max `limit` entries.
std::vector<ScoredApp> SearchApps(const std::vector<AppEntry>& apps,
                                 const std::wstring& query,
                                 size_t limit = 7);

std::wstring ToLower(std::wstring s);
