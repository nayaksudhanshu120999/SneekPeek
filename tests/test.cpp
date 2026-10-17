// SneekPeek unit tests: pure logic only (calc, fuzzy, bangs, URL detect).
// Built by CMake (SneekPeekTests) and run by CI via ctest.
// Returns nonzero on any failure; prints failing expressions.
#include <cstdio>
#include <cstring>
#include <cwchar>  // wcscmp
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
        // The Drive glyph keys off the row's action ("C:\"), not its title, so
        // it still resolves now that titles are bare letters.
        CHECK(GlyphForResult(ResultKind::Folder, L"C:\\", L"C:\\", RGB(0, 0, 0)).style ==
              GlyphStyle::Drive);
        CHECK(GlyphForResult(ResultKind::Folder, L"..", L"", RGB(0, 0, 0)).style ==
              GlyphStyle::Folder);
        CHECK(GlyphForResult(ResultKind::File, L"x.mp3", L"E:\\x.mp3", RGB(0, 0, 0)).style ==
              GlyphStyle::Audio);
        CHECK(GlyphForResult(ResultKind::Bang, L"cats", L"https://x", RGB(1, 2, 3)).color ==
              RGB(1, 2, 3));
        // Drive rows are the bare letter: volume labels were dropped because
        // reading them blocks on volumes that don't answer (empty optical
        // drive, offline share), freezing file mode. No DriveTitle helper here.
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
        CHECK(GlyphForResult(ResultKind::Folder, L"C:\\", L"C:\\",
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
        // (plain names; drive rows are bare letters now, but the comparator is
        //  generic over any name list, so keep a mixed-case pair here)
        CHECK(RankedNameLess(L"all files", L"installed", L"all",
                             L"ALL FILES", L"INSTALLED"));
        CHECK(!RankedNameLess(L"installed", L"all files", L"all",
                              L"INSTALLED", L"ALL FILES"));
        CHECK(RankedNameLess(L"c++ projects", L"c file dev c++", L"++",
                             L"c++ projects", L"c file dev c++"));
        CHECK(!RankedNameLess(L"c file dev c++", L"c++ projects", L"++",
                              L"c file dev c++", L"c++ projects"));
        // drive sort is headless-testable: filtering ranks the matching letters ...
        {
            std::vector<DriveInfo> d = {{L"F:\\", L"F:\\"}, {L"G:\\", L"G:\\"}};
            SortDrivesByRank(d, {}, L"g");
            CHECK(d.size() == 2 && d[0].root == L"G:\\" && d[1].root == L"F:\\");
        }
        // ... and an empty filter preserves Explorer letter order
        {
            std::vector<DriveInfo> d = {{L"F:\\", L"F:\\"}, {L"G:\\", L"G:\\"}};
            SortDrivesByRank(d, {}, L"");
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
        // Lowercase key is canonicalised to "PowHib", and a bare 6-digit hex
        // value (no '#') with lowercase digits parses to the same color as the
        // default above. Was "160110FF", which is 8 digits -- ParseColor
        // requires exactly 6, so this assertion could never have passed.
        CHECK(ParseThemeLine(L"powhib=a06eff", key, c) && key == L"PowHib" &&
              c == RGB(160, 110, 255));
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
        ResultItem drv{ResultKind::Folder, L"C:\\", L"", L"C:\\", RGB(0, 0, 0)};
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
    // INI round-trip through the real profile API. SaveSettings now serializes
    // the whole file and writes it once instead of calling
    // WritePrivateProfileStringW per key, so the quoting and the UTF-16 BOM are
    // ours to get right. Exercised against an actual temp file, not a mock.
    {
        wchar_t tmp[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, tmp) != 0);
        std::wstring ini = std::wstring(tmp) + L"sneekpeek_ini_roundtrip.ini";
        DeleteFileW(ini.c_str());

        Settings s;
        s.engineUrl = L"https://example.com/?q=%s";
        s.engineName = L"Example";
        s.browserPath = L"C:\\Program Files\\Some Browser\\browser.exe";
        s.hiddenApps = {L"Notepad", L"Weird ; Name", L" leading and trailing ",
                        L"tab\tinside"};
        s.includePathExes = true;
        s.runAtStartup = true;
        s.awakeOn = true;
        s.fileGrid = false;
        s.gridRows = 4;
        s.gridCols = 7;
        s.theme = DefaultTheme();
        s.theme.bg = RGB(1, 2, 3);
        s.theme.text = RGB(250, 251, 252);
        s.bangs = DefaultBangs();
        s.quicks = {{L"Gmail", L"https://mail.google.com"},
                    {L"Has ; Semicolon", L"https://example.com/a;b"}};
        s.history = {{ResultKind::App, L"Cam",
                      L"shell:AppsFolder\\X!App", true},
                     {ResultKind::Web, L"a ; query", L"", false},
                     // File/Folder rows serialize ONLY the path; the title is
                     // re-derived from it by HistoryBaseName on load. So the
                     // title here must be the basename of the action, which is
                     // what the app stores in the first place.
                     {ResultKind::File, L"a;b.txt", L"C:\\a;b.txt", false}};

        // Serialize once, then read every section back through the profile API.
        IniBuf b;
        b.open(L"General");
        b.put(L"EngineUrl", s.engineUrl);
        b.put(L"BrowserPath", s.browserPath);
        b.putInt(L"HiddenCount", (int)s.hiddenApps.size());
        wchar_t key[32];
        for (size_t i = 0; i < s.hiddenApps.size(); i++) {
            swprintf_s(key, 32, L"Hidden%d", (int)i);
            b.put(key, s.hiddenApps[i]);
        }
        b.open(L"Theme");
        b.put(L"Bg", SerializeColor(s.theme.bg));
        b.put(L"Text", SerializeColor(s.theme.text));
        b.open(L"Bangs");
        b.putInt(L"Count", (int)s.bangs.size());
        for (size_t i = 0; i < s.bangs.size(); i++) {
            swprintf_s(key, 32, L"Bang%d", (int)i);
            b.put(key, SerializeBang(s.bangs[i]));
        }
        b.open(L"Quick");
        b.putInt(L"Count", (int)s.quicks.size());
        for (size_t i = 0; i < s.quicks.size(); i++) {
            swprintf_s(key, 32, L"Quick%d", (int)i);
            b.put(key, s.quicks[i].name + L" | " + s.quicks[i].url);
        }
        b.open(L"History");
        b.putInt(L"Count", (int)s.history.size());
        for (size_t i = 0; i < s.history.size(); i++) {
            swprintf_s(key, 32, L"Item%d", (int)i);
            b.put(key, SerializeHistoryItem(s.history[i]));
        }
        CHECK(WriteTextFileAtomic(ini, b.text));

        wchar_t buf[4096];
        GetPrivateProfileStringW(L"General", L"EngineUrl", L"?", buf, 4096, ini.c_str());
        CHECK(wcscmp(buf, L"https://example.com/?q=%s") == 0);
        GetPrivateProfileStringW(L"General", L"BrowserPath", L"?", buf, 4096, ini.c_str());
        CHECK(wcscmp(buf, L"C:\\Program Files\\Some Browser\\browser.exe") == 0);
        // Plain value, ';' inside (must be quoted and survive), leading/trailing
        // space (must be quoted and survive), embedded tab.
        for (size_t i = 0; i < s.hiddenApps.size(); i++) {
            swprintf_s(key, 32, L"Hidden%d", (int)i);
            GetPrivateProfileStringW(L"General", key, L"?", buf, 4096, ini.c_str());
            CHECK(wcscmp(buf, s.hiddenApps[i].c_str()) == 0);
        }
        GetPrivateProfileStringW(L"Theme", L"Bg", L"?", buf, 4096, ini.c_str());
        COLORREF c;
        CHECK(ParseColor(buf, c) && c == RGB(1, 2, 3));
        // Bangs and quick links must survive serialization and re-parse to the
        // same alias/name/url triple the loaders expect.
        int bc = GetPrivateProfileIntW(L"Bangs", L"Count", 0, ini.c_str());
        CHECK(bc == (int)s.bangs.size());
        for (int i = 0; i < bc; i++) {
            swprintf_s(key, 32, L"Bang%d", i);
            GetPrivateProfileStringW(L"Bangs", key, L"", buf, 4096, ini.c_str());
            Bang pb;
            CHECK(ParseBangLine(buf, pb));
            CHECK(pb.name == s.bangs[(size_t)i].name &&
                  pb.url == s.bangs[(size_t)i].url &&
                  pb.aliases.size() == s.bangs[(size_t)i].aliases.size());
        }
        int qc = GetPrivateProfileIntW(L"Quick", L"Count", 0, ini.c_str());
        CHECK(qc == (int)s.quicks.size());
        for (int i = 0; i < qc; i++) {
            swprintf_s(key, 32, L"Quick%d", i);
            GetPrivateProfileStringW(L"Quick", key, L"", buf, 4096, ini.c_str());
            QuickLink pq;
            CHECK(ParseQuickLine(buf, pq));
            CHECK(pq.name == s.quicks[(size_t)i].name &&
                  pq.url == s.quicks[(size_t)i].url);
        }
        int hc = GetPrivateProfileIntW(L"History", L"Count", 0, ini.c_str());
        CHECK(hc == (int)s.history.size());
        for (int i = 0; i < hc; i++) {
            swprintf_s(key, 32, L"Item%d", i);
            GetPrivateProfileStringW(L"History", key, L"", buf, 4096, ini.c_str());
            HistoryItem ph;
            CHECK(ParseHistoryLine(buf, ph));
            CHECK(ph.kind == s.history[(size_t)i].kind &&
                  ph.title == s.history[(size_t)i].title &&
                  ph.action == s.history[(size_t)i].action);
        }
        // ...except that a File row's title is derived, never stored, so a
        // title that disagrees with its own basename is replaced on load.
        {
            HistoryItem ph;
            CHECK(ParseHistoryLine(L"F|C:\\a;b.txt", ph) &&
                  ph.kind == ResultKind::File &&
                  ph.action == L"C:\\a;b.txt" &&
                  ph.title == L"a;b.txt");
        }
        // Non-ASCII must survive the UTF-16 BOM (otherwise it reads as mojibake).
        {
            IniBuf u;
            u.open(L"General");
            u.put(L"EngineName", L"Su\x00E9\x0301ch \u4F60\u597D");
            CHECK(WriteTextFileAtomic(ini, u.text));
            GetPrivateProfileStringW(L"General", L"EngineName", L"?", buf, 4096,
                                     ini.c_str());
            CHECK(wcscmp(buf, L"Su\x00E9\x0301ch \u4F60\u597D") == 0);
        }
        // The legacy pipe-joined key is no longer written: the sentinel
        // default comes back, proving the key is absent from the file.
        GetPrivateProfileStringW(L"General", L"HiddenApps", L"?absent?", buf, 4096,
                                 ini.c_str());
        CHECK(wcscmp(buf, L"?absent?") == 0);
        // Atomic write must not leave the temp file behind.
        std::wstring tmpIni = ini + L".tmp";
        CHECK(GetFileAttributesW(tmpIni.c_str()) == INVALID_FILE_ATTRIBUTES);
        DeleteFileW(ini.c_str());
    }
    // Full SaveSettings -> LoadSettings round-trip against a real %APPDATA%
    // file: the loaders and the new one-shot writer must agree on every
    // section, including values the INI quoting has to protect.
    {
        // Point the cached SettingsPath at a scratch directory so the test can
        // never touch a real user profile.
        wchar_t tmp[MAX_PATH];
        CHECK(GetTempPathW(MAX_PATH, tmp) != 0);
        std::wstring dir = EnsureTrailingSlash(tmp) + L"sneekpeek_saveround\\";
        CreateDirectoryW(dir.c_str(), NULL);
        std::wstring real = dir + L"settings.ini";
        DeleteFileW(real.c_str());

        Settings s;
        s.engineUrl = L"https://example.com/?q=%s";
        s.engineName = L"Example ; Engine";
        s.browserPath = L"C:\\Some Dir\\browser.exe";
        s.hiddenApps = {L"Notepad", L"Trailing Space ", L"semi;colon"};
        s.includePathExes = true;
        s.runAtStartup = true;
        s.awakeOn = true;
        s.fileGrid = false;
        s.gridRows = 2;
        s.gridCols = 6;
        s.theme = DefaultTheme();
        s.theme.bg = RGB(10, 20, 30);
        s.bangs = DefaultBangs();
        s.quicks = {{L"Gmail", L"https://mail.google.com"}};
        s.history = {{ResultKind::App, L"Cam", L"shell:AppsFolder\\X!App", true},
                     {ResultKind::Web, L"q ; u", L"", false},
                     {ResultKind::Folder, L"docs", L"C:\\docs", false}};

        // Serialize with the same writer SaveSettings uses, into the scratch
        // dir, then load it back with the real loader.
        IniBuf b;
        b.open(L"General");
        b.put(L"EngineUrl", s.engineUrl);
        b.put(L"EngineName", s.engineName);
        b.put(L"BrowserPath", s.browserPath);
        b.putInt(L"HiddenCount", (int)s.hiddenApps.size());
        wchar_t key[32];
        for (size_t i = 0; i < s.hiddenApps.size(); i++) {
            swprintf_s(key, 32, L"Hidden%d", (int)i);
            b.put(key, s.hiddenApps[i]);
        }
        b.put(L"IncludePathExes", L"1");
        b.put(L"RunAtStartup", L"1");
        b.put(L"Awake", L"1");
        b.put(L"FileGrid", L"0");
        b.putInt(L"GridRows", s.gridRows);
        b.putInt(L"GridCols", s.gridCols);
        b.open(L"Theme");
        for (int i = 0; i < ThemeFieldCount(); i++)
            b.put(kThemeFields[i].key,
                  SerializeColor(s.theme.*(kThemeFields[i].member)));
        b.open(L"Bangs");
        b.putInt(L"Count", (int)s.bangs.size());
        for (size_t i = 0; i < s.bangs.size(); i++) {
            swprintf_s(key, 32, L"Bang%d", (int)i);
            b.put(key, SerializeBang(s.bangs[i]));
        }
        b.open(L"Quick");
        b.putInt(L"Count", (int)s.quicks.size());
        for (size_t i = 0; i < s.quicks.size(); i++) {
            swprintf_s(key, 32, L"Quick%d", (int)i);
            b.put(key, s.quicks[i].name + L" | " + s.quicks[i].url);
        }
        b.open(L"History");
        b.putInt(L"Count", (int)s.history.size());
        for (size_t i = 0; i < s.history.size(); i++) {
            swprintf_s(key, 32, L"Item%d", (int)i);
            b.put(key, SerializeHistoryItem(s.history[i]));
        }
        CHECK(WriteTextFileAtomic(real, b.text));

        // Load it back the way the app does.
        Settings back;
        LoadSettingsCore(back, real);
        LoadSettingsLists(back, real);
        CHECK(back.engineUrl == s.engineUrl);
        CHECK(back.engineName == s.engineName);
        CHECK(back.browserPath == s.browserPath);
        CHECK(back.includePathExes && back.runAtStartup && back.awakeOn);
        CHECK(!back.fileGrid);
        CHECK(back.gridRows == s.gridRows && back.gridCols == s.gridCols);
        CHECK(back.theme == s.theme);           // every one of the 29 fields
        CHECK(back.hiddenApps.size() == s.hiddenApps.size());
        for (size_t i = 0; i < back.hiddenApps.size() && i < s.hiddenApps.size(); i++)
            CHECK(back.hiddenApps[i] == s.hiddenApps[i]);
        CHECK(back.bangs.size() == s.bangs.size());
        CHECK(back.quicks.size() == s.quicks.size());
        CHECK(back.quicks[0].name == s.quicks[0].name &&
              back.quicks[0].url == s.quicks[0].url);
        CHECK(back.history.size() == s.history.size());
        for (size_t i = 0; i < back.history.size() && i < s.history.size(); i++) {
            CHECK(back.history[i].kind == s.history[i].kind);
            CHECK(back.history[i].title == s.history[i].title);
            CHECK(back.history[i].action == s.history[i].action);
            CHECK(back.history[i].isStore == s.history[i].isStore);
        }
        // A Bangs Count>0 whose every line is malformed must yield an EMPTY
        // list, not the defaults: the defaults here would resurrect bangs the
        // user deliberately deleted.
        {
            IniBuf bad;
            bad.open(L"Bangs");
            bad.putInt(L"Count", 2);
            bad.put(L"Bang0", L"this is not a valid bang line");
            bad.put(L"Bang1", L"also|bad|novars%s");
            CHECK(WriteTextFileAtomic(real, bad.text));
            Settings loaded;
            LoadSettingsCore(loaded, real);   // seeds the defaults...
            LoadSettingsLists(loaded, real);
            CHECK(loaded.bangs.empty()); // ...which the explicit list must win over
            // Sanity: a well-formed list of the same shape does load.
            IniBuf good;
            good.open(L"Bangs");
            good.putInt(L"Count", 1);
            good.put(L"Bang0", L"x, xx | X | https://example.com/?q=%s");
            CHECK(WriteTextFileAtomic(real, good.text));
            Settings loaded2;
            LoadSettingsCore(loaded2, real);
            LoadSettingsLists(loaded2, real);
            CHECK(loaded2.bangs.size() == 1 &&
                  loaded2.bangs[0].name == L"X" &&
                  loaded2.bangs[0].aliases.size() == 2);
        }
        // Deleting EVERY bang must stay deleted. Count=0 is written by
        // SaveSettings in that case; the loader must honour it instead of
        // falling back to the seeded defaults. (Reading Count with a 0 default
        // made "absent" and "explicitly 0" indistinguishable, so clearing the
        // list resurrected all 11 defaults on the next launch.)
        {
            IniBuf none;
            none.open(L"Bangs");
            none.putInt(L"Count", 0); // what SaveSettings writes for an empty list
            CHECK(WriteTextFileAtomic(real, none.text));
            Settings cleared;
            LoadSettingsCore(cleared, real);   // seeds DefaultBangs()...
            CHECK(!cleared.bangs.empty());
            LoadSettingsLists(cleared, real);
            CHECK(cleared.bangs.empty()); // ...which Count=0 must override

            // No [Bangs] section at all (first run / hand-written INI): the
            // defaults are the right answer, so the two cases must not collide.
            // This is the case that catches a clamp placed BEFORE the count
            // test: ClampCustomCount(-1) == 0, so an early clamp turns "key
            // absent" into "explicitly 0" and wipes the defaults on every
            // first run -- trading the original bug for a worse one.
            IniBuf absent;
            absent.open(L"General");
            absent.put(L"EngineUrl", L"https://example.com/?q=%s");
            CHECK(WriteTextFileAtomic(real, absent.text));
            Settings fresh;
            LoadSettingsCore(fresh, real);
            LoadSettingsLists(fresh, real);
            CHECK(fresh.bangs.size() == DefaultBangs().size());
            CHECK(!fresh.bangs.empty());
        }
        // Same three cases for quick links, which already had the right shape
        // (Count default -1 + an explicit clear). Guards the asymmetry that let
        // the bang bug exist: the two lists must now behave identically.
        {
            IniBuf none;
            none.open(L"Quick");
            none.putInt(L"Count", 0);
            CHECK(WriteTextFileAtomic(real, none.text));
            Settings cleared;
            LoadSettingsCore(cleared, real);   // seeds the Gmail example...
            LoadSettingsLists(cleared, real);
            CHECK(cleared.quicks.empty());

            IniBuf absent;
            absent.open(L"General");
            absent.put(L"EngineUrl", L"https://example.com/?q=%s");
            CHECK(WriteTextFileAtomic(real, absent.text));
            Settings fresh;
            LoadSettingsCore(fresh, real);
            LoadSettingsLists(fresh, real);
            CHECK(fresh.quicks.size() == 1 && fresh.quicks[0].name == L"Gmail");
        }
        DeleteFileW(real.c_str());
        RemoveDirectoryW(dir.c_str());
    }
    // Drive ranking takes the caller's lowercased titles (computed once for the
    // filter) so the comparator never allocates. Order must be identical to
    // ranking with no pre-lowered vector, which the palette no longer passes.
    {
        auto mk = [] {
            return std::vector<DriveInfo>{
                {L"F:\\", L"F:\\"},
                {L"G:\\", L"G:\\"},
                {L"H:\\", L"H:\\"},
            };
        };
        // no pre-lowered vector: helper must derive them itself
        auto a = mk();
        SortDrivesByRank(a, {}, L"g");
        // with pre-lowered titles: must produce exactly the same order
        auto b = mk();
        std::vector<std::wstring> low;
        for (const auto& x : b) low.push_back(FileLower(x.title));
        SortDrivesByRank(b, low, L"g");
        CHECK(a.size() == 3 && b.size() == 3);
        for (size_t i = 0; i < a.size() && i < b.size(); i++)
            CHECK(a[i].root == b[i].root && a[i].title == b[i].title);
        CHECK(a[0].root == L"G:\\" && a[1].root == L"F:\\" && a[2].root == L"H:\\");
        // empty filter: early-out, order untouched
        SortDrivesByRank(b, low, L"");
        CHECK(b[0].root == L"G:\\" && b[1].root == L"F:\\" && b[2].root == L"H:\\");
        // a filter that matches nothing keeps the input order
        std::vector<DriveInfo> e = {{L"F:\\", L"F:\\"}, {L"G:\\", L"G:\\"}};
        SortDrivesByRank(e, {}, L"zzz");
        CHECK(e.size() == 2 && e[0].root == L"F:\\" && e[1].root == L"G:\\");
        // single entry and empty vector must not crash
        std::vector<DriveInfo> one = {{L"C:\\", L"C:\\"}};
        SortDrivesByRank(one, {}, L"c");
        CHECK(one.size() == 1 && one[0].root == L"C:\\");
        std::vector<DriveInfo> none;
        SortDrivesByRank(none, {}, L"c");
        CHECK(none.empty());
        // mismatched pre-lowered vector must be ignored, not trusted blindly
        std::vector<DriveInfo> m = mk();
        std::vector<std::wstring> wrong = {L"zzz"};
        SortDrivesByRank(m, wrong, L"g");
        CHECK(m.size() == 3 && m[0].root == L"G:\\" && m[1].root == L"F:\\");
        // Drive rows carry no label, so title must equal root.
        for (const auto& r : GetLogicalDrivesWin32())
            CHECK(!r.empty() && r.size() == 3 && r[1] == L':' && r[2] == L'\\');
    }
    // FileScoreTierFind must agree with FileScoreTier and report the position
    // RankedNameLess now relies on (0 for prefix, npos for subsequence-only).
    {
        size_t pos = 12345;
        CHECK(FileScoreTierFind(L"all files", L"", pos) == 0 && pos == (size_t)-1);
        CHECK(FileScoreTierFind(L"readme.txt", L"zzz", pos) == -1);
        // Tier 1 is EXACT match only: a prefix scores 50000 - nameLen, which is
        // always below the tier-1 threshold, so "readme.txt"/"read" lands in 2
        // (with pos 0, which is what keeps prefixes ahead of other substrings).
        CHECK(FileScoreTierFind(L"readme", L"readme", pos) == 1 && pos == 0);
        CHECK(FileScoreTierFind(L"readme.txt", L"read", pos) == 2 && pos == 0);
        CHECK(FileScoreTierFind(L"mydocs.txt", L"doc", pos) == 2 && pos == 2);
        CHECK(FileScoreTierFind(L"ado.doc", L"doc", pos) == 2 && pos == 4);
        // subsequence-only: no literal occurrence, so tier 3 and pos stays npos
        CHECK(FileScoreTierFind(L"installed", L"ald", pos) == 3 &&
              pos == (size_t)-1);
        // tier wrapper agrees
        CHECK(FileScoreTier(L"mydocs.txt", L"doc") == 2);
        CHECK(FileScoreTier(L"readme", L"readme") == 1);
    }
    // ToLower / ToLowerInPlace must agree, and neither may disturb its input.
    // ASCII only: the app never calls setlocale, so towlower runs in the "C"
    // locale and leaves non-ASCII alone (matching the pre-existing behaviour).
    {
        std::wstring src = L"MiXeD Case 123";
        std::wstring byValue = src;
        std::wstring folded = ToLower(src);
        CHECK(folded == L"mixed case 123");   // ASCII folded
        CHECK(wcscmp(src.c_str(), L"MiXeD Case 123") == 0); // source untouched
        CHECK(byValue == L"MiXeD Case 123");  // caller's copy untouched
        ToLowerInPlace(byValue);
        CHECK(byValue == folded);             // same result, in place
        CHECK(ToLower(L"").empty());
        std::wstring e; ToLowerInPlace(e); CHECK(e.empty());
    }
    // SearchAppsLower must behave exactly like SearchApps on the same input.
    {
        std::vector<AppEntry> apps = {
            {L"Visual Studio Code", L"C:\\vscode.lnk", L"visual studio code", false},
            {L"Code", L"C:\\code.lnk", L"code", false},
            {L"Control Panel", L"C:\\cp.lnk", L"control panel", false},
        };
        for (size_t lim = 1; lim <= 5; lim++) {
            auto mixed = SearchApps(apps, L"CoDe", lim);
            auto lowerOnly = SearchAppsLower(apps, L"code", lim);
            CHECK(mixed.size() == lowerOnly.size());
            for (size_t i = 0; i < mixed.size() && i < lowerOnly.size(); i++)
                CHECK(mixed[i].app == lowerOnly[i].app &&
                      mixed[i].score == lowerOnly[i].score);
        }
        // limits truncate, and a larger limit is a prefix-extension of a smaller
        CHECK(SearchAppsLower(apps, L"c", 1).size() == 1);
        CHECK(SearchAppsLower(apps, L"c", 5).size() >= 1);
        // empty query yields nothing (no accidental "match everything")
        CHECK(SearchAppsLower(apps, L"", 7).empty());
        // a query longer than every name matches nothing
        CHECK(SearchAppsLower(apps, L"waytoolongquery", 7).empty());
        // limit 0 must not crash and must yield nothing
        CHECK(SearchAppsLower(apps, L"c", 0).empty());
        // Whitespace-only queries: SearchApps trims first, so it finds nothing.
        CHECK(SearchApps(apps, L"   ", 5).empty());
        // SearchAppsLower does NOT trim -- that is the contract, so the palette
        // can lowercase+trim once and share the query. Untrimmed blanks are
        // real characters, so a two-space query is a (degenerate) subsequence
        // match against a two-word name. Asserted to pin that down, because
        // silently trimming here would be an extra per-keystroke allocation.
        CHECK(!SearchAppsLower(apps, L"  ", 5).empty());
        // trimming: SearchApps strips surrounding space, SearchAppsLower
        // expects it already stripped - same result for the same text
        auto padded = SearchApps(apps, L"  code  ", 5);
        auto bare = SearchAppsLower(apps, L"code", 5);
        CHECK(padded.size() == bare.size());
        for (size_t i = 0; i < padded.size() && i < bare.size(); i++)
            CHECK(padded[i].app == bare[i].app);
    }
    // Extension matching moved from a lowercased copy to an in-place compare:
    // every case combination and the non-matching paths must still resolve.
    {
        struct Case { const wchar_t* name; GlyphStyle want; };
        const Case cases[] = {
            {L"setup.exe", GlyphStyle::Exe},
            {L"setup.EXE", GlyphStyle::Exe},
            {L"setup.ExE", GlyphStyle::Exe},
            {L"a.TXT", GlyphStyle::DocLines},
            {L"A.Txt", GlyphStyle::DocLines},
            {L"archive.tar.gz", GlyphStyle::Archive},
            {L"ARCHIVE.TAR.GZ", GlyphStyle::Archive},
            {L"song.MP3", GlyphStyle::Audio},
            {L"noext", GlyphStyle::FilePage},  // no dot
            {L"file.", GlyphStyle::FilePage},  // trailing dot: no extension
            {L".exe", GlyphStyle::Exe},       // dotfile: extension is still "exe"
            {L"weird.", GlyphStyle::FilePage},
            {L"file.c", GlyphStyle::Code},    // single-char extension
            {L"FILE.C", GlyphStyle::Code},
            {L"x", GlyphStyle::FilePage},     // single char, no dot
            {L"exe", GlyphStyle::FilePage},   // no dot at all
        };
        for (const auto& c : cases)
            CHECK(FileGlyphFor(c.name, false).style == c.want);
        // directories always win over any extension
        CHECK(FileGlyphFor(L"backup.zip", true).style == GlyphStyle::Folder);
        CHECK(FileGlyphFor(L"noext", true).style == GlyphStyle::Folder);
    }
    if (g_fail == 0) printf("ALL TESTS PASSED\n");
    return g_fail ? 1 : 0;
}
