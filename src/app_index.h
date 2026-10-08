#pragma once
// SneekPeek — tiny app index (Start Menu .lnk + optional PATH scan)
// Design goals: <300KB source, lazy one-time scan, ~100KB RAM, 0% idle CPU.

#include <string>
#include <vector>

struct AppEntry {
    std::wstring name;   // "Visual Studio Code" (from .lnk filename)
    std::wstring target; // full path to .lnk (ShellExecute resolves it)
};

struct ScoredApp {
    const AppEntry* app;
    int score;
};

// Scan two Start Menu trees + optional PATH exes. Runs once, on first hotkey.
void BuildAppIndex(std::vector<AppEntry>& out, bool includePathExes);

// Fuzzy score. Returns -1 = no match, higher = better.
int FuzzyScore(const std::wstring& nameLower, const std::wstring& queryLower);

// Filter + rank. Returns pointers into `apps`, max `limit` entries.
std::vector<ScoredApp> SearchApps(const std::vector<AppEntry>& apps,
                                 const std::wstring& query,
                                 size_t limit = 7);

std::wstring ToLower(std::wstring s);
