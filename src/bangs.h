#pragma once
// SneekPeek - user-configurable !bang table with per-site color + aliases.
// Header-only. All UI strings stay plain ASCII (no mojibake, ever).
#include <windows.h> // COLORREF, RGB
#include <cwctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct Bang {
    std::vector<std::wstring> aliases; // lowercase match keys, e.g. {"yt","youtube"}
    std::wstring name;                 // display name, e.g. "YouTube"
    std::wstring url;                  // must contain %s
    COLORREF color;                    // chip + row accent color
};

inline std::wstring BangTrim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::wstring BangLower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// #RRGGBB (leading # optional) -> COLORREF. False when malformed.
inline bool ParseColor(const std::wstring& text, COLORREF& out) {
    std::wstring h = BangTrim(text);
    if (!h.empty() && h[0] == L'#') h.erase(h.begin());
    if (h.size() != 6) return false;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(h.c_str(), &end, 16);
    if (!end || *end != L'\0') return false;
    out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

inline std::wstring SerializeColor(COLORREF c) {
    wchar_t b[16];
    swprintf_s(b, 16, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return b;
}

// Tasteful random pick for new bangs (user may leave the color empty).
inline COLORREF RandomBangColor() {
    static const COLORREF palette[] = {
        RGB(96, 165, 250), RGB(52, 211, 153), RGB(251, 191, 36),
        RGB(248, 113, 113), RGB(192, 132, 252), RGB(45, 212, 191),
        RGB(251, 146, 60), RGB(244, 114, 182), RGB(163, 230, 53),
        RGB(94, 234, 212), RGB(253, 224, 71), RGB(252, 165, 165),
    };
    return palette[rand() % 12];
}

inline std::vector<Bang> DefaultBangs() {
    return {
        {{L"g", L"google"},      L"Google",       L"https://www.google.com/search?q=%s",               RGB(66, 133, 244)},
        {{L"d", L"ddg", L"duckduckgo"}, L"DuckDuckGo", L"https://duckduckgo.com/?q=%s",                RGB(222, 88, 51)},
        {{L"yt", L"youtube"},    L"YouTube",      L"https://www.youtube.com/results?search_query=%s",  RGB(255, 0, 51)},
        {{L"w", L"wiki", L"wikipedia"}, L"Wikipedia", L"https://en.wikipedia.org/wiki/Special:Search?search=%s", RGB(168, 176, 188)},
        {{L"gh", L"github"},     L"GitHub",       L"https://github.com/search?q=%s&type=repositories", RGB(88, 166, 255)},
        {{L"so", L"stackoverflow"}, L"Stack Overflow", L"https://stackoverflow.com/search?q=%s",        RGB(244, 128, 36)},
        {{L"r", L"reddit"},      L"Reddit",       L"https://www.reddit.com/search/?q=%s",               RGB(255, 69, 0)},
        {{L"m", L"maps"},        L"Maps",         L"https://www.google.com/maps/search/%s",            RGB(52, 168, 83)},
        {{L"t", L"translate"},   L"Translate",    L"https://translate.google.com/?sl=auto&tl=en&text=%s&op=translate", RGB(26, 115, 232)},
        {{L"a", L"amazon"},      L"Amazon",       L"https://www.amazon.com/s?k=%s",                     RGB(255, 153, 0)},
        {{L"x", L"twitter"},     L"X",            L"https://x.com/search?q=%s",                         RGB(231, 233, 234)},
    };
}

// -1 when no alias matches (alias must already be lowercased by the caller).
inline int FindBangByAlias(const std::vector<Bang>& bangs, const std::wstring& aliasLower) {
    for (size_t i = 0; i < bangs.size(); i++)
        for (const auto& a : bangs[i].aliases)
            if (a == aliasLower) return (int)i;
    return -1;
}

// Leading "!alias term" form. Returns false when the query has no '!' prefix.
inline bool ParseBangLeading(const std::wstring& query, const std::vector<Bang>& bangs,
                             int& outIdx, std::wstring& outTerm) {
    size_t i = 0;
    while (i < query.size() && iswspace(query[i])) i++;
    if (i >= query.size() || query[i] != L'!') return false;
    size_t j = query.find_first_of(L" \t", i + 1);
    std::wstring alias;
    if (j == std::wstring::npos) { alias = query.substr(i + 1); outTerm.clear(); }
    else {
        alias = query.substr(i + 1, j - (i + 1));
        size_t k = query.find_first_not_of(L" \t", j);
        outTerm = (k == std::wstring::npos) ? L"" : query.substr(k);
    }
    outIdx = FindBangByAlias(bangs, BangLower(alias));
    return true;
}

// One settings line:  alias, alias | Name | url-with-%s | #RRGGBB
// The color part is optional (defaults to gray).
inline bool ParseBangLine(const std::wstring& line, Bang& out) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        size_t p = line.find(L'|', start);
        parts.push_back(BangTrim(line.substr(start, p == std::wstring::npos ? p : p - start)));
        if (p == std::wstring::npos) break;
        start = p + 1;
    }
    if (parts.size() < 3) return false;
    Bang b;
    size_t a0 = 0;
    for (;;) {
        size_t p = parts[0].find(L',', a0);
        std::wstring a = BangLower(BangTrim(parts[0].substr(a0, p == std::wstring::npos ? p : p - a0)));
        if (!a.empty()) b.aliases.push_back(a);
        if (p == std::wstring::npos) break;
        a0 = p + 1;
    }
    b.name = parts[1];
    b.url = parts[2];
    b.color = RGB(142, 142, 147);
    if (parts.size() >= 4 && !parts[3].empty()) {
        COLORREF c;
        if (ParseColor(parts[3], c)) b.color = c;
    }
    if (b.aliases.empty() || b.name.empty()) return false;
    if (b.url.find(L"%s") == std::wstring::npos) return false;
    out = std::move(b);
    return true;
}

inline std::wstring SerializeBang(const Bang& b) {
    std::wstring s;
    for (size_t i = 0; i < b.aliases.size(); i++) {
        if (i) s += L", ";
        s += b.aliases[i];
    }
    wchar_t col[16];
    swprintf_s(col, 16, L"#%02X%02X%02X", GetRValue(b.color), GetGValue(b.color), GetBValue(b.color));
    return s + L" | " + b.name + L" | " + b.url + L" | " + col;
}

// Percent-encode a wide query as UTF-8 for URLs. Space -> '+' (query style).
inline std::wstring UrlEncodeQuery(const std::wstring& in) {
    if (in.empty()) return L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, in.c_str(), (int)in.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return L"";
    std::string utf8((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, in.c_str(), (int)in.size(), utf8.data(), n, NULL, NULL);
    static const wchar_t* HEX = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(utf8.size() + 8);
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back((wchar_t)c);
        else if (c == ' ')
            out.push_back(L'+');
        else {
            out.push_back(L'%');
            out.push_back(HEX[c >> 4]);
            out.push_back(HEX[c & 15]);
        }
    }
    return out;
}

inline std::wstring ExpandUrl(const std::wstring& tmpl, const std::wstring& encodedQuery) {
    std::wstring url = tmpl;
    size_t p = url.find(L"%s");
    if (p != std::wstring::npos) url.replace(p, 2, encodedQuery);
    else url += encodedQuery;
    return url;
}

// Domain-shaped input (google.com, not "weather today") opens directly,
// like a browser address bar. No spaces; scheme optional (https assumed).
// Pure string logic (uses BangLower above) so unit tests cover it.
inline bool IsUrlLike(const std::wstring& q, std::wstring& outUrl) {
    if (q.empty()) return false;
    if (q.find_first_of(L" \t\r\n") != std::wstring::npos) return false;
    std::wstring low = BangLower(q);
    if (low.compare(0, 7, L"http://") == 0 || low.compare(0, 8, L"https://") == 0) {
        outUrl = q;
        return true;
    }
    for (wchar_t c : q) {
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'0' && c <= L'9') ||
                  c == L'-' || c == L'.' || c == L'_' || c == L'~' || c == L':' ||
                  c == L'/' || c == L'?' || c == L'#' || c == L'@' || c == L'!' ||
                  c == L'$' || c == L'&' || c == L'\'' || c == L'(' || c == L')' ||
                  c == L',' || c == L';' || c == L'=' || c == L'%';
        if (!ok) return false;
    }
    bool hasDot = q.find(L'.') != std::wstring::npos;
    bool hasPort = q.find(L':') != std::wstring::npos; // localhost:3000
    if (!hasDot && !hasPort) return false;
    if (q.front() == L'.' || q.front() == L'-' || q.front() == L'/' ||
        q.back() == L'.' || q.back() == L'-' || q.back() == L'/')
        return false;
    if (hasDot) {
        std::wstring tail = q.substr(q.find_last_of(L'.') + 1);
        size_t cut = tail.find_first_of(L"/?#:");
        if (cut != std::wstring::npos) tail.resize(cut);
        if (tail.empty()) return false;
    }
    outUrl = L"https://" + q;
    return true;
}

// Homepage fallback for empty bang queries: template minus %s and dangling ? & = /.
inline std::wstring BangHome(const std::wstring& tmpl) {
    std::wstring u = tmpl;
    size_t p = u.find(L"%s");
    if (p != std::wstring::npos) u.erase(p);
    while (!u.empty()) {
        wchar_t c = u.back();
        if (c == L'?' || c == L'&' || c == L'=' || c == L'/' || c == L'+' || c == L'#' || c == L'.')
            u.pop_back();
        else break;
    }
    return u;
}
