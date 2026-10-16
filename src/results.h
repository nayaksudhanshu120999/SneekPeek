#pragma once
// SneekPeek - result model shared by the palette and the unit tests.
// Pure data + pure selection rules (no windows, no controls), so the
// Enter-key decision table is testable headlessly:
//   app row      -> launch it
//   calc row     -> copy the result
//   URL row      -> open it
//   web row      -> engine search
//   setting row  -> open the page
//   folder row   -> drill into that path (file mode, stays open)
//   file row     -> open the file directly
//   explorer row -> open the path in Explorer (file mode)
//   header/none  -> typed-query fallback (web search, or hide if empty;
//                   file mode instead stays open with no fallback)
#include <windows.h> // COLORREF
#include <set>
#include <string>
#include <vector>

enum class ResultKind { App, Web, Bang, Calc, Setting, Header, Folder, File, Explorer };

struct ResultItem {
    ResultKind kind;
    std::wstring title;   // single line (headers, names, queries)
    std::wstring sub;     // reserved, currently unused
    std::wstring action;  // lnk path | shell:AppsFolder target | url | ms-settings: | calc text | full file path
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

// ------------------------------------------------- file-mode multi-selection
// The selection set holds row action paths (full paths / drive roots), so it
// survives refiltering within one folder. Pure helpers: the palette owns the
// set, these only decide membership (unit-tested).
// A row can join the selection: real file/folder/drive rows only. ".."
// navigates up and headers are labels, so neither is ever selectable.
inline bool MultiKeySelectable(const ResultItem& r) {
    if (r.kind != ResultKind::Folder && r.kind != ResultKind::File) return false;
    if (r.title == L"..") return false;
    return !r.action.empty();
}
inline void MultiToggle(std::set<std::wstring>& sel, const std::wstring& key) {
    auto it = sel.find(key);
    if (it == sel.end()) sel.insert(key);
    else sel.erase(it);
}
// Shift-range: add every selectable row between two result indices.
inline void MultiRangeAdd(std::set<std::wstring>& sel,
                          const std::vector<ResultItem>& items, int from, int to) {
    int n = (int)items.size();
    if (n <= 0) return;
    if (from < 0) from = 0;
    if (to < 0) to = 0;
    if (from >= n) from = n - 1;
    if (to >= n) to = n - 1;
    int a = from < to ? from : to, b = from < to ? to : from;
    for (int i = a; i <= b; i++)
        if (MultiKeySelectable(items[i])) sel.insert(items[i].action);
}
// Indices of the current rows that are members of the set (open/copy targets).
inline std::vector<int> MultiMemberRows(const std::set<std::wstring>& sel,
                                        const std::vector<ResultItem>& items) {
    std::vector<int> out;
    for (size_t i = 0; i < items.size(); i++)
        if (MultiKeySelectable(items[i]) && sel.count(items[i].action))
            out.push_back((int)i);
    return out;
}
