#pragma once
// SneekPeek — web + bang resolution. Zero heap churn in idle; only runs on Enter.
#include <windows.h>
#include <cwctype>
#include <string>
#include <vector>

struct Bang {
    const wchar_t* key;  // L"g"
    const wchar_t* url;  // L"https://www.google.com/search?q=%s"
    const wchar_t* label;// L"Google"
};

// Built-in bangs. User can override the *default engine* in Settings;
// bangs stay fixed (tiny, no config-file parsing cost at runtime).
inline const Bang kBangs[] = {
    {L"g",   L"https://www.google.com/search?q=%s",          L"Google"},
    {L"d",   L"https://duckduckgo.com/?q=%s",               L"DuckDuckGo"},
    {L"ddg", L"https://duckduckgo.com/?q=%s",               L"DuckDuckGo"},
    {L"yt",  L"https://www.youtube.com/results?search_query=%s", L"YouTube"},
    {L"w",   L"https://en.wikipedia.org/wiki/Special:Search?search=%s", L"Wikipedia"},
    {L"gh",  L"https://github.com/search?q=%s&type=repositories", L"GitHub"},
    {L"so",  L"https://stackoverflow.com/search?q=%s",      L"Stack Overflow"},
    {L"r",   L"https://www.reddit.com/search/?q=%s",        L"Reddit"},
    {L"m",   L"https://www.google.com/maps/search/%s",      L"Maps"},
    {L"t",   L"https://translate.google.com/?sl=auto&tl=en&text=%s&op=translate", L"Translate"},
    {L"a",   L"https://www.amazon.com/s?k=%s",              L"Amazon"},
    {L"x",   L"https://x.com/search?q=%s",                 L"X"},
};

// Try parse leading "!bang term". Returns true if input starts with '!'.
inline bool ParseBang(const std::wstring& query,
                      std::wstring& outBang, std::wstring& outTerm) {
    size_t i = 0;
    while (i < query.size() && iswspace(query[i])) i++;
    if (i >= query.size() || query[i] != L'!') return false;
    size_t j = query.find_first_of(L" \t", i + 1);
    if (j == std::wstring::npos) { outBang = query.substr(i + 1); outTerm.clear(); return true; }
    outBang = query.substr(i + 1, j - (i + 1));
    size_t k = query.find_first_not_of(L" \t", j);
    outTerm = (k == std::wstring::npos) ? L"" : query.substr(k);
    for (auto& c : outBang) c = (wchar_t)towlower(c);
    return true;
}

inline const Bang* FindBang(const std::wstring& key) {
    for (const auto& b : kBangs)
        if (key == b.key) return &b;
    return nullptr;
}

// Percent-encode a wide query as UTF-8 for URLs. Space -> '+' (query style).
inline std::wstring UrlEncodeQuery(const std::wstring& in) {
    if (in.empty()) return L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, in.c_str(), (int)in.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return L"";
    std::string utf8(n, '\0');
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
