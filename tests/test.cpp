// SneekPeek unit tests: pure logic only (calc, fuzzy, bangs, URL detect).
// Built by CMake (SneekPeekTests) and run by CI via ctest.
// Returns nonzero on any failure; prints failing expressions.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include "calc.h"
#include "bangs.h"
#include "files.h"
#include "minjson.h"
#include "settings.h"
#include "results.h"
#include "app_index.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); g_fail++; } \
} while (0)

static bool CalcEq(const wchar_t* expr, double want) {
    double v = 0;
    if (!TryCalc(expr, v)) return false;
    return fabs(v - want) < 1e-9;
}

int main() {
    // calculator: precedence, parens, unary, power associativity
    CHECK(CalcEq(L"12*8", 96));
    CHECK(CalcEq(L"2+3*4", 14));
    CHECK(CalcEq(L"(2+3)*4", 20));
    CHECK(CalcEq(L"-2^2", -4));     // ^ binds tighter than unary minus
    CHECK(CalcEq(L"2^3^2", 512));   // right-associative
    CHECK(CalcEq(L"2^-3", 0.125));
    CHECK(CalcEq(L"0.5+0.5", 1));
    CHECK(CalcEq(L" 12 * 8 ", 96));
    CHECK(CalcEq(L"10%3", 1));
    // calculator: undefined operations are errors, not silent 0
    double v = 0;
    CHECK(!TryCalc(L"1/0", v));
    CHECK(!TryCalc(L"5%0", v));
    CHECK(!TryCalc(L"0^(-1)", v));  // infinity
    CHECK(!TryCalc(L"hello", v));
    CHECK(!TryCalc(L"", v));
    CHECK(!TryCalc(L"12", v));      // bare number is not an expression
    // calculator: input-length and nesting-depth limits
    {
        std::wstring big;
        for (int i = 0; i < 150; i++) big += L"1+";
        big += L"1"; // >256 chars
        CHECK(big.size() > 256 && !TryCalc(big, v));
        std::wstring deep(70, L'(');
        deep += L"1";
        deep += std::wstring(70, L')');
        CHECK(!TryCalc(deep, v)); // too deep
        std::wstring ok10(10, L'(');
        ok10 += L"1";
        ok10 += std::wstring(10, L')');
        ok10 += L"+0";
        CHECK(CalcEq(ok10.c_str(), 1)); // shallow nesting still fine
    }
    // fuzzy search ranking
    std::vector<AppEntry> apps = {
        {L"Visual Studio Code", L"C:\\vscode.lnk", L"visual studio code", false},
        {L"Code", L"C:\\code.lnk", L"code", false},
        {L"Control Panel", L"C:\\cp.lnk", L"control panel", false},
    };
    auto r1 = SearchApps(apps, L"code", 7);
    CHECK(!r1.empty() && r1[0].app->name == L"Code"); // exact/short wins
    auto r2 = SearchApps(apps, L"vsc", 7);
    CHECK(!r2.empty() && r2[0].app->name == L"Visual Studio Code");
    auto r3 = SearchApps(apps, L"zzzqqq", 7);
    CHECK(r3.empty());
    CHECK(FuzzyScore(L"hello", L"xyz") < 0);
    CHECK(FuzzyScore(L"code", L"code") > FuzzyScore(L"visual studio code", L"code"));
    // bangs
    {
        auto bangs = DefaultBangs();
        int idx = -1;
        std::wstring term;
        CHECK(ParseBangLeading(L"!g cats", bangs, idx, term));
        CHECK(idx >= 0 && bangs[(size_t)idx].name == L"Google" && term == L"cats");
        CHECK(!ParseBangLeading(L"hello", bangs, idx, term));
        int g = FindBangByAlias(bangs, L"yt");
        CHECK(g >= 0 && bangs[(size_t)g].name == L"YouTube");
        CHECK(FindBangByAlias(bangs, L"nope") < 0);
        Bang b;
        CHECK(ParseBangLine(L"yt, youtube | YouTube | https://www.youtube.com/results?search_query=%s | #FF0033", b));
        CHECK(b.aliases.size() == 2 && b.name == L"YouTube");
        CHECK(!ParseBangLine(L"bad line without pipes", b));
        CHECK(!ParseBangLine(L"x | Y | https://example.com/novar", b)); // %s required
        COLORREF c = 0;
        CHECK(ParseColor(L"#FF0033", c) && c == RGB(255, 0, 51));
        CHECK(!ParseColor(L"notacolor", c));
        CHECK(SerializeColor(RGB(255, 0, 51)) == L"#FF0033");
    }
    // Enter-key decision table: headers are never actionable.
    {
        auto row = [](ResultKind k, const wchar_t* t) {
            ResultItem r;
            r.kind = k;
            r.title = t;
            r.color = RGB(0, 0, 0);
            return r;
        };
        std::vector<ResultItem> list = {
            row(ResultKind::Header, L"Apps"),
            row(ResultKind::App, L"Notepad"),
            row(ResultKind::Header, L"Web"),
            row(ResultKind::Web, L"notepad"),
        };
        CHECK(EffectiveSelection(1, list) == 1);   // app row launches
        CHECK(EffectiveSelection(3, list) == 3);   // web row searches
        CHECK(EffectiveSelection(0, list) == -1);  // heading -> fallback
        CHECK(EffectiveSelection(2, list) == -1);  // heading -> fallback
        CHECK(EffectiveSelection(-1, list) == -1); // nothing -> fallback
        CHECK(EffectiveSelection(99, list) == -1); // out of range -> fallback
        CHECK(FirstSelectable(list) == 1);         // skips the heading
        std::vector<ResultItem> headsOnly = {
            row(ResultKind::Header, L"Apps"),
            row(ResultKind::Header, L"Web"),
        };
        CHECK(FirstSelectable(headsOnly) == -1);
        std::vector<ResultItem> empty;
        CHECK(FirstSelectable(empty) == -1);
        CHECK(EffectiveSelection(0, empty) == -1);
    }
    // JSON string decoding incl. UTF-16 surrogate pairs
    {
        const char* s1 = "\"plain\"";
        const char* p1 = s1;
        std::wstring w;
        CHECK(DecodeJsonString(p1, s1 + strlen(s1), w) && w == L"plain");
        const char* s2 = "\"A\\u00e9\\ud83d\\ude00\""; // e-acute + grinning face
        const char* p2 = s2;
        CHECK(DecodeJsonString(p2, s2 + strlen(s2), w));
        CHECK(w.size() == 4 && w[0] == L'A' && w[1] == 0xE9 &&
              w[2] == 0xD83D && w[3] == 0xDE00);
        const char* s3 = "\"\\ud83d\""; // lone high surrogate: reject
        const char* p3 = s3;
        CHECK(!DecodeJsonString(p3, s3 + strlen(s3), w));
        const char* s4 = "\"x\\ude00\""; // lone low surrogate: reject
        const char* p4 = s4;
        CHECK(!DecodeJsonString(p4, s4 + strlen(s4), w));
    }
    // settings list cap is consistent everywhere
    CHECK(ClampCustomCount(-5) == 0);
    CHECK(ClampCustomCount(0) == 0);
    CHECK(ClampCustomCount(199) == 199);
    CHECK(ClampCustomCount(500) == kMaxCustomEntries);
    // URL detection
    {
        std::wstring u;
        CHECK(IsUrlLike(L"google.com", u) && u == L"https://google.com");
        CHECK(IsUrlLike(L"https://google.com/search?q=x", u));
        CHECK(IsUrlLike(L"localhost:3000", u));
        CHECK(!IsUrlLike(L"hello world", u));
        CHECK(!IsUrlLike(L"code", u));
        CHECK(!IsUrlLike(L"", u));
        CHECK(!IsUrlLike(L".com", u));
    }
    // safe web schemes only (launch gate)
    {
        CHECK(IsSafeWebUrl(L"https://example.com/?q=x"));
        CHECK(IsSafeWebUrl(L"http://example.com/"));
        CHECK(IsSafeWebUrl(L"HTTPS://EXAMPLE.COM/"));
        CHECK(!IsSafeWebUrl(L""));
        CHECK(!IsSafeWebUrl(L"notaurl"));
        CHECK(!IsSafeWebUrl(L"file:///C:/secret.txt"));
        CHECK(!IsSafeWebUrl(L"FILE:///C:/secret.txt"));
        CHECK(!IsSafeWebUrl(L"javascript:alert(1)"));
        CHECK(!IsSafeWebUrl(L"data:text/html,hi"));
        CHECK(!IsSafeWebUrl(L"https://example.com/\"quoted\""));
        CHECK(!IsSafeWebUrl(L"https://example.com/a\tb"));
    }
    // file-browse helpers: names + count only, folders first, current path only
    {
        CHECK(FileLower(L"C:\\Foo") == L"c:\\foo");
        CHECK(FileMatchesFilter(L"notepad.exe", L""));
        CHECK(FileMatchesFilter(L"notepad.exe", L"note"));
        CHECK(FileMatchesFilter(L"notepad.exe", L"pad"));
        CHECK(!FileMatchesFilter(L"note", L"notepad.exe"));
        CHECK(!FileMatchesFilter(L"readme.txt", L"zzz"));
        std::vector<FileEntry> v = {
            {L"b.txt", false}, {L"A", true}, {L"a.txt", false}, {L"B", true},
        };
        SortFileEntries(v);
        CHECK(v.size() == 4 && v[0].name == L"A" && v[0].isDir);
        CHECK(v[1].name == L"B" && v[1].isDir);
        CHECK(v[2].name == L"a.txt" && !v[2].isDir);
        CHECK(v[3].name == L"b.txt" && !v[3].isDir);
        CHECK(EnsureTrailingSlash(L"C:\\a") == L"C:\\a\\");
        CHECK(EnsureTrailingSlash(L"C:\\a\\") == L"C:\\a\\");
        CHECK(EnsureTrailingSlash(L"") == L"");
        CHECK(JoinFilePath(L"C:\\a\\", L"b.txt") == L"C:\\a\\b.txt");
        CHECK(JoinFilePath(L"C:\\a", L"b.txt") == L"C:\\a\\b.txt");
        CHECK(ParentDirOf(L"C:\\") == L"");
        CHECK(ParentDirOf(L"C:\\a\\") == L"C:\\");
        CHECK(ParentDirOf(L"C:\\a\\b\\") == L"C:\\a\\");
        CHECK(ParentDirOf(L"") == L"");
        CHECK(FormatFileCount(0) == L"0 items");
        CHECK(FormatFileCount(1) == L"1 item");
        CHECK(FormatFileCount(42) == L"42 items");
        // file rows are actionable like app rows (headers never are)
        auto frow = [](ResultKind k, const wchar_t* t) {
            ResultItem r;
            r.kind = k;
            r.title = t;
            r.color = RGB(0, 0, 0);
            return r;
        };
        std::vector<ResultItem> fl = {
            frow(ResultKind::Header, L"C:\\ - 2 items"),
            frow(ResultKind::Folder, L"docs"),
            frow(ResultKind::File, L"a.txt"),
        };
        CHECK(EffectiveSelection(1, fl) == 1);
        CHECK(EffectiveSelection(2, fl) == 2);
        CHECK(FirstSelectable(fl) == 1);
    }
    // single-level dir scan on a temp folder (no recursion, names only)
    {
        wchar_t tmp[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, tmp) != 0);
        std::wstring root = EnsureTrailingSlash(tmp) + L"sneekpeek_filetest\\";
        CreateDirectoryW(root.c_str(), NULL);
        std::wstring sub = root + L"sub\\";
        CreateDirectoryW(sub.c_str(), NULL);
        std::wstring f1 = root + L"a.txt";
        HANDLE hf = CreateFileW(f1.c_str(), GENERIC_WRITE, 0, NULL,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
        // nested file must NOT leak into the top-level scan
        std::wstring nested = sub + L"nested.txt";
        hf = CreateFileW(nested.c_str(), GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
        std::vector<FileEntry> out;
        size_t total = 0;
        bool trunc = false;
        CHECK(ListDirOneLevel(root, out, total, trunc));
        CHECK(total == 2 && !trunc);
        bool sawSub = false, sawFile = false, sawNested = false;
        for (const auto& e : out) {
            if (e.name == L"sub" && e.isDir) sawSub = true;
            if (e.name == L"a.txt" && !e.isDir) sawFile = true;
            if (e.name == L"nested.txt") sawNested = true;
        }
        CHECK(sawSub && sawFile && !sawNested);
        size_t badTotal = 0;
        bool badTrunc = false;
        CHECK(!ListDirOneLevel(L"Z:\\no\\such\\dir\\", out, badTotal, badTrunc));
        DeleteFileW(nested.c_str());
        DeleteFileW(f1.c_str());
        RemoveDirectoryW(sub.c_str());
        RemoveDirectoryW(root.c_str());
    }
    // child environment blocks: sorted, deduplicated, double-terminated
    {
        std::wstring block = BuildChildEnv(L"SNEEKPEEK_APPS_JSON", L"C:\\t\\o.json");
        std::vector<std::wstring> vars;
        for (const wchar_t* p = block.c_str(); *p; p += wcslen(p) + 1)
            vars.push_back(p);
        CHECK(block.size() >= 2 && block[block.size() - 1] == L'\0' &&
              block[block.size() - 2] == L'\0');
        int ours = 0;
        for (const auto& v : vars) {
            if (v.compare(0, 20, L"SNEEKPEEK_APPS_JSON=") == 0) {
                ours++;
                CHECK(v == L"SNEEKPEEK_APPS_JSON=C:\\t\\o.json");
            }
        }
        CHECK(ours == 1); // exactly one, even if the parent already had one
        for (size_t i = 1; i < vars.size(); i++)
            CHECK(_wcsicmp(vars[i - 1].c_str(), vars[i].c_str()) <= 0);
    }
    // app index cache round-trip (+ corrupt rejection)
    {
        wchar_t tmp[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, tmp) != 0);
        std::wstring f = std::wstring(tmp) + L"sneekpeek_test_cache.bin";
        std::vector<AppEntry> a = {
            {L"Camera", L"shell:AppsFolder\\Microsoft.WindowsCamera_8wekyb3d8bbwe!App", L"camera", true},
            {L"Notepad", L"C:\\x\\notepad.lnk", L"notepad", false},
        };
        CHECK(SaveAppCache(f, a));
        std::vector<AppEntry> b;
        CHECK(LoadAppCache(f, b));
        CHECK(b.size() == 2 && b[0].name == L"Camera" && b[0].isStore &&
              b[0].lower == L"camera" && b[1].target == L"C:\\x\\notepad.lnk" &&
              !b[1].isStore);
        DeleteFileW(f.c_str());
        HANDLE hf = CreateFileW(f.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w = 0;
        const char junk[20] = {0}; // bad magic
        if (hf != INVALID_HANDLE_VALUE) {
            WriteFile(hf, junk, sizeof(junk), &w, NULL);
            CloseHandle(hf);
        }
        std::vector<AppEntry> c;
        CHECK(!LoadAppCache(f, c) && c.empty());
        CHECK(!LoadAppCache(L"Z:\\no\\such\\file.bin", c));
        DeleteFileW(f.c_str());
    }
    if (g_fail == 0) printf("ALL TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
