// SneekPeek unit tests: pure logic only (calc, fuzzy, bangs, URL detect).
// Built by CMake (SneekPeekTests) and run by CI via ctest.
// Returns nonzero on any failure; prints failing expressions.
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#include "calc.h"
#include "bangs.h"
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
    if (g_fail == 0) printf("ALL TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
