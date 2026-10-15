#pragma once
// SneekPeek - app index: Start Menu (.lnk, exe-targets only) + Microsoft Store
// (UWP) apps + optional PATH scan. Lazy one-time scan, ~100KB RAM, 0% idle CPU.

#include <atomic>
#include <string>
#include <vector>

struct AppEntry {
    std::wstring name;    // display name ("Visual Studio Code", "Camera")
    std::wstring target;  // .lnk path, or "shell:AppsFolder\<AUMID>" for Store
    std::wstring lower;   // cached lowercase name (hot-path searches, no allocs)
    bool isStore = false; // Store apps launch via explorer.exe + target
};

struct ScoredApp {
    const AppEntry* app;
    int score;
};

// Scan Start Menu + Store apps + optional PATH exes. Runs once, on first hotkey.
// Initializes COM on the calling thread (needed for .lnk resolve + Store enum).
// `stop`, when set mid-scan, abandons remaining phases promptly.
// `skipHeavy`, when true, runs the light pass only (.lnk + primary Store
// enum): the PowerShell Get-StartApps phase and the fallback Store enum are
// skipped. Used for the automatic post-boot refresh so logon stays cheap;
// manual Refresh / F5 / cache-miss scans run the full pass.
void BuildAppIndex(std::vector<AppEntry>& out, bool includePathExes,
                   const std::atomic<bool>* stop = nullptr,
                   bool skipHeavy = false);

// Build a double-null-terminated Unicode child environment block: the parent
// block plus name=value (replacing any same-named entry), sorted
// case-insensitively as CreateProcessW documents. Pure logic, unit-tested.
std::wstring BuildChildEnv(const std::wstring& name, const std::wstring& value);

// Fuzzy score: exact > prefix > word-boundary > substring > subsequence
// (with consecutive / boundary bonuses). Returns -1 = no match.
int FuzzyScore(const std::wstring& nameLower, const std::wstring& queryLower);

// Filter + rank. Returns pointers into `apps`, max `limit` entries.
std::vector<ScoredApp> SearchApps(const std::vector<AppEntry>& apps,
                                 const std::wstring& query,
                                 size_t limit = 7);

// Persisted index cache (%APPDATA%\\SneekPeek\\apps.cache): versioned binary,
// exact-size validated on load. lower[] is recomputed, not stored.
bool SaveAppCache(const std::wstring& path, const std::vector<AppEntry>& apps);
bool LoadAppCache(const std::wstring& path, std::vector<AppEntry>& out);

std::wstring ToLower(std::wstring s);
