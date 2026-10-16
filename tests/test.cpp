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
#include "icons.h"
#include "minjson.h"
#include "settings.h"
#include "results.h"
#include "verbs.h"
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
        const char* s5 = "\"abc"; // unterminated: reject, don't keep partial
        const char* p5 = s5;
        CHECK(!DecodeJsonString(p5, s5 + strlen(s5), w));
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
        // file-mode trigger is ":" + space/tab (a "file" keyword would
        // hijack app searches like "file manager")
        CHECK(IsFileTrigger(L": "));
        CHECK(IsFileTrigger(L": c"));
        CHECK(IsFileTrigger(L":\tc"));
        CHECK(!IsFileTrigger(L":c"));
        CHECK(!IsFileTrigger(L"file "));
        CHECK(!IsFileTrigger(L"C:\\x"));
        CHECK(!IsFileTrigger(L"a:b c"));
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
        // extension -> glyph mapping (case-insensitive, last dot wins)
        CHECK(FileGlyphFor(L"setup.exe", false).style == GlyphStyle::Exe);
        CHECK(FileGlyphFor(L"a.TXT", false).style == GlyphStyle::DocLines);
        CHECK(FileGlyphFor(L"archive.tar.gz", false).style == GlyphStyle::Archive);
        CHECK(FileGlyphFor(L"song.MP3", false).style == GlyphStyle::Audio);
        CHECK(FileGlyphFor(L"noext", false).style == GlyphStyle::FilePage);
        CHECK(FileGlyphFor(L"file.", false).style == GlyphStyle::FilePage);
        CHECK(FileGlyphFor(L"backup.zip", true).style == GlyphStyle::Folder);
        // rows -> glyphs (drives detected by path, bang keeps its color)
        CHECK(GlyphForResult(ResultKind::Folder, L"Win 10 (C:)", L"C:\\", RGB(0, 0, 0)).style ==
              GlyphStyle::Drive);
        CHECK(GlyphForResult(ResultKind::Folder, L"..", L"", RGB(0, 0, 0)).style ==
              GlyphStyle::Folder);
        CHECK(GlyphForResult(ResultKind::File, L"x.mp3", L"E:\\x.mp3", RGB(0, 0, 0)).style ==
              GlyphStyle::Audio);
        CHECK(GlyphForResult(ResultKind::Bang, L"cats", L"https://x", RGB(1, 2, 3)).color ==
              RGB(1, 2, 3));
        CHECK(DriveTitle(L"C:\\", L"Win 10") == L"Win 10 (C:)");
        CHECK(DriveTitle(L"M:\\", L"Music") == L"Music (M:)");
        CHECK(DriveTitle(L"D:\\", L"") == L"D:\\");
        // fuzzy score: exact > prefix > substring > subsequence; -1 / 0 sentinels
        CHECK(FileFuzzyScore(L"all", L"all") == 100000);
        CHECK(FileFuzzyScore(L"all files (g:)", L"all") >
              FileFuzzyScore(L"installed (f:)", L"all"));
        CHECK(FileFuzzyScore(L"visual studio code", L"vsc") >= 0); // subsequence
        CHECK(FileFuzzyScore(L"readme", L"zzz") < 0);
        CHECK(FileFuzzyScore(L"x", L"") == 0);
        // filter is fuzzy-aware: subsequence matches pass, misses don't
        CHECK(FileMatchesFilter(L"visual studio code", L"vsc"));
        CHECK(FileMatchesFilter(L"all files (g:)", L"all"));
        CHECK(!FileMatchesFilter(L"readme", L"zzz"));
        CHECK(FileMatchesFilter(L"x", L""));
        // Explorer natural order: digits sort numerically, case-insensitive
        CHECK(StrCmpLogicalW(L"2", L"10") < 0);
        CHECK(StrCmpLogicalW(L"file10", L"file2") > 0);
        {
            std::vector<FileEntry> v = {
                {L"10.txt", false}, {L"2.txt", false},
            };
            SortFileEntries(v); // empty filter: pure Explorer order
            CHECK(v.size() == 2 && v[0].name == L"2.txt" && v[1].name == L"10.txt");
        }
        // drive rows resolve to the Drive glyph (regression pin)
        CHECK(IsDriveRootDir(L"C:\\"));
        CHECK(!IsDriveRootDir(L""));
        CHECK(!IsDriveRootDir(L"C:\\docs"));
        CHECK(GlyphForResult(ResultKind::Folder, L"Win 10 (C:)", L"C:\\",
                             RGB(0, 0, 0)).style == GlyphStyle::Drive);
        CHECK(GlyphForResult(ResultKind::Folder, L"docs", L"C:\\docs",
                             RGB(0, 0, 0)).style == GlyphStyle::Folder);
        CHECK(ClampGridRows(0) == 1);
        CHECK(ClampGridRows(3) == 3);
        CHECK(ClampGridRows(9) == 4);
        CHECK(ClampGridCols(1) == 2);
        CHECK(ClampGridCols(5) == 5);
        CHECK(ClampGridCols(99) == 7);
        // ranked sort: "all" puts ALL FILES before instALLEd
        {
            std::vector<FileEntry> v = {
                {L"INSTALLED", true}, {L"ALL FILES", true},
            };
            SortFileEntries(v, L"all");
            CHECK(v.size() == 2 && v[0].name == L"ALL FILES" && v[1].name == L"INSTALLED");
        }
        // folders still first; prefix files before substring files
        {
            std::vector<FileEntry> v = {
                {L"mydocs.txt", false}, {L"Documents", true}, {L"ado.doc", false},
            };
            SortFileEntries(v, L"doc");
            CHECK(v.size() == 3 && v[0].name == L"Documents");
            CHECK(v[1].name == L"mydocs.txt" && v[2].name == L"ado.doc");
        }
        // shared ordering: tier, earliest occurrence, then Explorer order
        CHECK(RankedNameLess(L"all files (g:)", L"installed (f:)", L"all",
                             L"ALL FILES (G:)", L"INSTALLED (F:)"));
        CHECK(!RankedNameLess(L"installed (f:)", L"all files (g:)", L"all",
                              L"INSTALLED (F:)", L"ALL FILES (G:)"));
        CHECK(RankedNameLess(L"c++ projects", L"c file dev c++", L"++",
                             L"c++ projects", L"c file dev c++"));
        CHECK(!RankedNameLess(L"c file dev c++", L"c++ projects", L"++",
                              L"c file dev c++", L"c++ projects"));
        // drive sort is headless-testable: "all" puts ALL FILES first ...
        {
            std::vector<DriveInfo> d = {
                {L"F:\\", L"INSTALLED", L"INSTALLED (F:)"},
                {L"G:\\", L"ALL FILES", L"ALL FILES (G:)"},
            };
            SortDrivesByRank(d, L"all");
            CHECK(d.size() == 2 && d[0].root == L"G:\\" && d[1].root == L"F:\\");
        }
        // ... and an empty filter preserves Explorer letter order
        {
            std::vector<DriveInfo> d = {
                {L"F:\\", L"INSTALLED", L"INSTALLED (F:)"},
                {L"G:\\", L"ALL FILES", L"ALL FILES (G:)"},
            };
            SortDrivesByRank(d, L"");
            CHECK(d.size() == 2 && d[0].root == L"F:\\" && d[1].root == L"G:\\");
        }
        // context-menu verbs: pure mapping per area and file type
        {
            auto has = [](const std::vector<VerbId>& vv, VerbId id) {
                for (auto x : vv) if (x == id) return true;
                return false;
            };
            VerbCtx app; app.kind = ResultKind::App; app.hasPath = true;
            auto va = VerbsFor(app);
            CHECK(has(va, VerbId::Open) && has(va, VerbId::RunAsAdmin));
            CHECK(has(va, VerbId::HideApp) && !has(va, VerbId::Eject));
            VerbCtx store = app; store.isStore = true;
            auto vs = VerbsFor(store);
            CHECK(has(vs, VerbId::Open) && !has(vs, VerbId::RunAsAdmin) &&
                  !has(vs, VerbId::CopyPath));
            VerbCtx drv; drv.kind = ResultKind::Folder; drv.isDrive = true;
            drv.hasPath = true;
            CHECK(!has(VerbsFor(drv), VerbId::Eject));
            drv.isRemovable = true;
            CHECK(has(VerbsFor(drv), VerbId::Eject));
            VerbCtx exe; exe.kind = ResultKind::File; exe.glyph = GlyphStyle::Exe;
            exe.hasPath = true;
            CHECK(has(VerbsFor(exe), VerbId::RunAsAdmin));
            VerbCtx pic = exe; pic.glyph = GlyphStyle::Picture;
            CHECK(has(VerbsFor(pic), VerbId::SetWallpaper) &&
                  !has(VerbsFor(pic), VerbId::RunAsAdmin));
            VerbCtx au = exe; au.glyph = GlyphStyle::Audio;
            auto vau = VerbsFor(au);
            CHECK(has(vau, VerbId::Open) && has(vau, VerbId::OpenWith) &&
                  !has(vau, VerbId::SetWallpaper));
            VerbCtx doc = exe; doc.glyph = GlyphStyle::Pdf;
            CHECK(has(VerbsFor(doc), VerbId::Print));
            VerbCtx web; web.kind = ResultKind::Web;
            CHECK(has(VerbsFor(web), VerbId::SwitchEngine));
            VerbCtx calc; calc.kind = ResultKind::Calc;
            auto vc = VerbsFor(calc);
            CHECK(vc.size() == 1 && vc[0] == VerbId::CopyResult);
            VerbCtx head; head.kind = ResultKind::Header;
            CHECK(VerbsFor(head).empty());
            CHECK(VerbLabel(VerbId::Open)[0] != L'\0' &&
                  VerbLabel(VerbId::Quit)[0] != L'\0');
        }
        // app search already ranks by position (regression pin)
        CHECK(FuzzyScore(L"c++ projects", L"++") > FuzzyScore(L"c file dev c++", L"++"));
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
            frow(ResultKind::Explorer, L"Open in Explorer"),
            frow(ResultKind::Folder, L"docs"),
            frow(ResultKind::File, L"a.txt"),
        };
        CHECK(EffectiveSelection(1, fl) == 1);
        CHECK(EffectiveSelection(2, fl) == 2);
        CHECK(EffectiveSelection(3, fl) == 3);
        CHECK(FirstSelectable(fl) == 1);
    }
    // single-level dir scan on a temp folder (no recursion, names only)
    {
        wchar_t tmp[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, tmp) != 0);
        std::wstring root = EnsureTrailingSlash(tmp) + L"sneekpeek_filetest\\";
        // Best-effort pre-clean: a crashed prior run must not break the count.
        {
            std::wstring stale1 = root + L"a.txt";
            std::wstring stale2 = root + L"hidden.txt";
            std::wstring staleSub = root + L"sub\\";
            std::wstring stale3 = staleSub + L"nested.txt";
            SetFileAttributesW(stale2.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(stale3.c_str());
            DeleteFileW(stale1.c_str());
            DeleteFileW(stale2.c_str());
            RemoveDirectoryW(staleSub.c_str());
            RemoveDirectoryW(root.c_str());
        }
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
        // hidden entries stay out of the list (and the count)
        std::wstring hidden = root + L"hidden.txt";
        hf = CreateFileW(hidden.c_str(), GENERIC_WRITE, 0, NULL,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) CloseHandle(hf);
        SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN);
        std::vector<FileEntry> out;
        size_t total = 0;
        bool trunc = false;
        CHECK(ListDirOneLevel(root, out, total, trunc));
        CHECK(total == 2 && !trunc);
        bool sawSub = false, sawFile = false, sawNested = false, sawHidden = false;
        for (const auto& e : out) {
            if (e.name == L"sub" && e.isDir) sawSub = true;
            if (e.name == L"a.txt" && !e.isDir) sawFile = true;
            if (e.name == L"nested.txt") sawNested = true;
            if (e.name == L"hidden.txt") sawHidden = true;
        }
        CHECK(sawSub && sawFile && !sawNested && !sawHidden);
        size_t badTotal = 0;
        bool badTrunc = false;
        CHECK(!ListDirOneLevel(L"Z:\\no\\such\\dir\\", out, badTotal, badTrunc));
        SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(nested.c_str());
        DeleteFileW(hidden.c_str());
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
    // theme.ini line parsing + extended defaults
    {
        CHECK(ThemeFieldCount() == 29);
        Theme d = DefaultTheme();
        CHECK(d.rowtext == RGB(218, 218, 226) && d.danger == RGB(255, 90, 90));
        CHECK(d.btnsel == RGB(29, 65, 115) && d.sep == RGB(30, 30, 32));
        CHECK(d.powdown == RGB(255, 90, 90) && d.powhib == RGB(160, 110, 255));
        CHECK(d.chiptext == RGB(255, 255, 255) && d.menuacc == RGB(150, 150, 155));
        std::wstring key;
        COLORREF c;
        CHECK(ParseThemeLine(L"Bg = #000000", key, c) && key == L"Bg" &&
              c == RGB(0, 0, 0));
        CHECK(ParseThemeLine(L"  text=#F0F0F0  ", key, c) && key == L"Text");
        CHECK(ParseThemeLine(L"powhib=160110FF", key, c) && key == L"PowHib");
        CHECK(!ParseThemeLine(L"# a comment", key, c));
        CHECK(!ParseThemeLine(L"", key, c));
        CHECK(!ParseThemeLine(L"NoEqualsHere", key, c));
        CHECK(!ParseThemeLine(L"Nope = #000000", key, c)); // unknown key
        CHECK(!ParseThemeLine(L"Bg = notacolor", key, c)); // bad color
        CHECK(!ParseThemeLine(L"Bg = ", key, c));
        CHECK(!ParseThemeLine(L" = #000000", key, c));
    }
    // theme defaults + auto-fit grid columns + multi-select membership
    {
        Theme d = DefaultTheme();
        CHECK(d.bg == RGB(0, 0, 0) && d.text == RGB(240, 240, 240));
        CHECK(d.sel == RGB(28, 28, 30) && d.folder == RGB(251, 191, 36));
        CHECK(d == DefaultTheme() && !(d != DefaultTheme()));
        Theme other = d;
        other.text = RGB(0, 0, 0);
        CHECK(other != d);
        // auto-widen: setting is a maximum, narrow clients drop columns
        CHECK(EffectiveGridCols(496, 5, 110) == 4);
        CHECK(EffectiveGridCols(496, 2, 110) == 2);
        CHECK(EffectiveGridCols(600, 5, 110) == 5);
        CHECK(EffectiveGridCols(100, 5, 110) == 1);
        CHECK(EffectiveGridCols(0, 5, 110) == 5);
        CHECK(EffectiveGridCols(496, 0, 110) == 1);
        // multi-select membership: files/folders/drives yes, ".."/headers/apps no
        ResultItem f{ResultKind::File, L"a.txt", L"", L"C:\\a.txt", RGB(0, 0, 0)};
        ResultItem d2{ResultKind::Folder, L"docs", L"", L"C:\\docs", RGB(0, 0, 0)};
        ResultItem drv{ResultKind::Folder, L"Win (C:)", L"", L"C:\\", RGB(0, 0, 0)};
        ResultItem up{ResultKind::Folder, L"..", L"", L"C:\\", RGB(0, 0, 0)};
        ResultItem hd{ResultKind::Header, L"C:\\ - 3 items", L"", L"", RGB(0, 0, 0)};
        ResultItem ap{ResultKind::App, L"Notepad", L"", L"C:\\n.lnk", RGB(0, 0, 0)};
        CHECK(MultiKeySelectable(f) && MultiKeySelectable(d2) && MultiKeySelectable(drv));
        CHECK(!MultiKeySelectable(up) && !MultiKeySelectable(hd) && !MultiKeySelectable(ap));
        std::vector<ResultItem> items = {hd, f, d2, up, drv};
        std::set<std::wstring> sel;
        MultiToggle(sel, f.action);
        CHECK(sel.size() == 1);
        MultiToggle(sel, f.action); // toggles off
        CHECK(sel.empty());
        MultiRangeAdd(sel, items, 1, 4); // file + folder + drive, skips ".."
        CHECK(sel.size() == 3 && sel.count(L"C:\\a.txt") && sel.count(L"C:\\docs") &&
              sel.count(L"C:\\"));
        auto rows = MultiMemberRows(sel, items);
        CHECK(rows.size() == 3 && rows[0] == 1 && rows[1] == 2 && rows[2] == 4);
        MultiRangeAdd(sel, items, 99, -5); // clamped to the same range
        CHECK(sel.size() == 3);
        std::set<std::wstring> empty;
        CHECK(MultiMemberRows(empty, items).empty());
        // clamped file-mode motion: skips headers, never lands on nothing
        // (".." is a real Folder row, so arrows stop on it like before)
        CHECK(LastSelectable(items) == 4);
        CHECK(ClampMoveSel(items, 1, +1) == 2);  // file -> folder
        CHECK(ClampMoveSel(items, 2, +1) == 3);  // folder -> ".."
        CHECK(ClampMoveSel(items, 3, +1) == 4);  // ".." -> drive
        CHECK(ClampMoveSel(items, 4, +1) == 4);  // past last: stays (no dead spot)
        CHECK(ClampMoveSel(items, 1, -1) == 1);  // above first pickable: stays
        CHECK(ClampMoveSel(items, 4, -1) == 3);  // drive -> ".."
        CHECK(ClampMoveSel(items, -1, +1) == 1); // resyncs to first pickable
        CHECK(ClampMoveSel(items, -1, -1) == 4); // resyncs to last pickable
        std::vector<ResultItem> headsOnly = {hd};
        CHECK(ClampMoveSel(headsOnly, 0, +1) == -1); // nothing selectable
        CHECK(LastSelectable(headsOnly) == -1);
    }
    // history: typed entries, move-to-front, dedupe, cap, line round-trip
    {
        std::vector<HistoryItem> h;
        HistoryItem empty;
        HistoryAdd(h, empty);
        CHECK(h.empty());
        HistoryItem f{ResultKind::File, L"a.txt", L"C:\\a.txt", false};
        HistoryItem d{ResultKind::Folder, L"docs", L"D:\\docs\\", false};
        HistoryAdd(h, f);
        HistoryAdd(h, d);
        CHECK(h.size() == 2 && h[0].title == L"docs" && h[1].title == L"a.txt");
        HistoryItem f2{ResultKind::File, L"A.TXT", L"c:\\A.TXT", false};
        HistoryAdd(h, f2); // same target, other case: moves to front
        CHECK(h.size() == 2 && h[0].action == L"c:\\A.TXT");
        // kinds don't collide: same path as file vs app stays separate
        HistoryItem a{ResultKind::App, L"a.txt", L"C:\\a.txt", false};
        HistoryAdd(h, a);
        CHECK(h.size() == 3 && h[0].kind == ResultKind::App);
        // engine searches dedupe on the query, not the empty action
        HistoryItem w1{ResultKind::Web, L"cats", L"", false};
        HistoryItem w2{ResultKind::Web, L"Cats", L"", false};
        HistoryAdd(h, w1);
        HistoryAdd(h, w2);
        CHECK(h.size() == 4 && h[0].title == L"Cats");
        for (int i = 0; i < 30; i++) {
            wchar_t b[32];
            swprintf_s(b, 32, L"C:\\f%d.txt", i);
            HistoryItem fi{ResultKind::File, b, b, false};
            HistoryAdd(h, fi);
        }
        CHECK(h.size() == kMaxHistory && h[0].title == L"C:\\f29.txt");
        // line format round-trips, legacy bare lines load as files
        CHECK(SerializeHistoryItem(a) == L"A|a.txt|C:\\a.txt|0");
        HistoryItem st{ResultKind::App, L"Cam", L"shell:x", true};
        CHECK(SerializeHistoryItem(st) == L"A|Cam|shell:x|1");
        CHECK(SerializeHistoryItem(w1) == L"W|cats|");
        CHECK(SerializeHistoryItem(d) == L"D|D:\\docs\\");
        CHECK(SerializeHistoryItem(f) == L"F|C:\\a.txt");
        HistoryItem p;
        CHECK(ParseHistoryLine(L"A|Notepad|C:\\n.lnk|0", p) &&
              p.kind == ResultKind::App && p.title == L"Notepad" && !p.isStore);
        CHECK(ParseHistoryLine(L"A|Cam|shell:x|1", p) && p.isStore);
        CHECK(ParseHistoryLine(L"W|cats|", p) && p.kind == ResultKind::Web &&
              p.title == L"cats" && p.action.empty());
        CHECK(ParseHistoryLine(L"W|q|https://x.y/?q=%s", p) &&
              p.action == L"https://x.y/?q=%s");
        CHECK(ParseHistoryLine(L"D|D:\\docs\\", p) && p.kind == ResultKind::Folder);
        CHECK(ParseHistoryLine(L"C:\\a.txt", p) && p.kind == ResultKind::File &&
              p.title == L"a.txt"); // legacy bare line
        CHECK(!ParseHistoryLine(L"", p) && !ParseHistoryLine(L"X|y", p));
        CHECK(!ParseHistoryLine(L"A|only", p) && !ParseHistoryLine(L"A|n|t|x", p));
        // web rows still route to the Web glyph (search icon rework pin)
        CHECK(GlyphForResult(ResultKind::Web, L"q", L"http://x",
                             RGB(46, 224, 158)).style == GlyphStyle::Web);
    }
    if (g_fail == 0) printf("ALL TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
