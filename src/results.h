#pragma once
// SneekPeek - result model shared by the palette and the unit tests.
// Pure data + pure selection rules (no windows, no controls), so the
// Enter-key decision table is testable headlessly:
//   app row      -> launch it
//   calc row     -> copy the result
//   URL row      -> open it
//   web row      -> engine search
//   setting row  -> open the page
//   header/none  -> typed-query fallback (web search, or hide if empty)
#include <windows.h> // COLORREF
#include <string>
#include <vector>

enum class ResultKind { App, Web, Bang, Calc, Setting, Header, Online };

struct ResultItem {
    ResultKind kind;
    std::wstring title;   // single line (headers, names, queries)
    std::wstring sub;     // reserved, currently unused
    std::wstring action;  // lnk path | shell:AppsFolder target | url | ms-settings: | calc text
    COLORREF color;       // category accent color
    bool isStore = false; // Store app: launch via explorer.exe
};

// Actionable index, or -1 for the typed-query fallback. Headers are never
// actionable, so Enter can never silently land on one.
inline int EffectiveSelection(int idx, const std::vector<ResultItem>& results) {
    if (idx < 0 || idx >= (int)results.size()) return -1;
    if (results[(size_t)idx].kind == ResultKind::Header) return -1;
    return idx;
}

// First actionable row, or -1 when there is none (empty list / headers only).
inline int FirstSelectable(const std::vector<ResultItem>& results) {
    for (size_t i = 0; i < results.size(); i++)
        if (results[i].kind != ResultKind::Header) return (int)i;
    return -1;
}
