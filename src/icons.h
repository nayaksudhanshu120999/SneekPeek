#pragma once
// SneekPeek - predefined kind/extension glyphs, drawn not loaded.
// Header-only. No icon extraction anywhere (no SHGetFileInfo, no ExtractIcon,
// no image files): every glyph is a few GDI shapes painted in WM_DRAWITEM,
// so cost is ~9 visible rows x a handful of calls per repaint — no disk I/O,
// no cache, no RAM growth. Mapping helpers are pure and unit-tested.
#include <windows.h>
#include <string>
#include "results.h"
#include "files.h"

enum class GlyphStyle {
    Web, Bang, Calc, Setting, Folder, Drive, FilePage,
    Exe, Terminal, Shortcut, DocLines, Pdf, DocWord, Sheet, Slide,
    Book, Picture, Audio, Video, Archive, Disc, Code, FontTag, Database,
    AppTile,
};

struct FileGlyph {
    GlyphStyle style;
    COLORREF color; // full-strength accent; paint dims it when unselected
};

inline COLORREF GlyphDim(COLORREF c, int pct) {
    return RGB(GetRValue(c) * pct / 100,
               GetGValue(c) * pct / 100,
               GetBValue(c) * pct / 100);
}

// The one vibrant palette. Every chip/tile color in the app comes from here
// (app letter-tiles hash into it; new bangs roll from it) — never an ad-hoc
// RGB(). High saturation tuned to pop on the black rows.
inline const COLORREF kVibrantCols[] = {
    RGB(255, 90, 90), RGB(255, 140, 40), RGB(255, 180, 40), RGB(240, 210, 50),
    RGB(126, 217, 87), RGB(50, 215, 110), RGB(45, 210, 180), RGB(60, 200, 255),
    RGB(80, 140, 255), RGB(120, 127, 240), RGB(160, 110, 255), RGB(255, 95, 170),
};
inline constexpr size_t kVibrantCount = sizeof(kVibrantCols) / sizeof(kVibrantCols[0]);

// Deterministic tile color: same app name -> same color, every launch.
// djb2 over the raw title (stable for identical strings, no locale issues).
inline COLORREF AppTileColor(const std::wstring& name) {
    unsigned long h = 5381;
    for (wchar_t c : name) h = ((h << 5) + h) + (unsigned long)c;
    return kVibrantCols[h % kVibrantCount];
}

// Tile letters: first alphanumeric char + first alphanumeric char of the next
// word ("Visual Studio Code" -> "VS", "Code" -> "C", "7-Zip" -> "7Z").
inline std::wstring AppTileLetters(const std::wstring& name) {
    std::wstring out;
    bool wordStart = true;
    for (wchar_t c : name) {
        bool alnum = (c >= L'0' && c <= L'9') ||
                     (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
        if (alnum && wordStart && out.size() < 2) {
            wchar_t u = c;
            if (u >= L'a' && u <= L'z') u = (wchar_t)(u - L'a' + L'A');
            out.push_back(u);
        }
        wordStart = !alnum;
    }
    if (out.empty()) out = L"?";
    return out;
}

// Extension -> glyph. Directories never reach the table: callers pass isDir
// (a folder named "backup.zip" still shows a folder).
struct ExtEntry {
    const wchar_t* ext; // lowercase, no dot
    GlyphStyle style;
    COLORREF color;
};

inline const ExtEntry kExtTable[] = {
    {L"exe", GlyphStyle::Exe, RGB(251, 146, 60)},
    {L"msi", GlyphStyle::Exe, RGB(251, 146, 60)},
    {L"com", GlyphStyle::Exe, RGB(251, 146, 60)},
    {L"scr", GlyphStyle::Exe, RGB(251, 146, 60)},
    {L"cpl", GlyphStyle::Exe, RGB(251, 146, 60)},
    {L"msc", GlyphStyle::Exe, RGB(251, 146, 60)},
    {L"bat", GlyphStyle::Terminal, RGB(148, 163, 184)},
    {L"cmd", GlyphStyle::Terminal, RGB(148, 163, 184)},
    {L"ps1", GlyphStyle::Terminal, RGB(148, 163, 184)},
    {L"sh", GlyphStyle::Terminal, RGB(148, 163, 184)},
    {L"lnk", GlyphStyle::Shortcut, RGB(96, 165, 250)},
    {L"url", GlyphStyle::Shortcut, RGB(96, 165, 250)},
    {L"txt", GlyphStyle::DocLines, RGB(147, 197, 253)},
    {L"md", GlyphStyle::DocLines, RGB(147, 197, 253)},
    {L"log", GlyphStyle::DocLines, RGB(147, 197, 253)},
    {L"rtf", GlyphStyle::DocLines, RGB(147, 197, 253)},
    {L"odt", GlyphStyle::DocLines, RGB(147, 197, 253)},
    {L"tex", GlyphStyle::DocLines, RGB(147, 197, 253)},
    {L"pdf", GlyphStyle::Pdf, RGB(248, 113, 113)},
    {L"doc", GlyphStyle::DocWord, RGB(96, 165, 250)},
    {L"docx", GlyphStyle::DocWord, RGB(96, 165, 250)},
    {L"xls", GlyphStyle::Sheet, RGB(45, 212, 191)},
    {L"xlsx", GlyphStyle::Sheet, RGB(45, 212, 191)},
    {L"csv", GlyphStyle::Sheet, RGB(45, 212, 191)},
    {L"ppt", GlyphStyle::Slide, RGB(251, 191, 36)},
    {L"pptx", GlyphStyle::Slide, RGB(251, 191, 36)},
    {L"odp", GlyphStyle::Slide, RGB(251, 191, 36)},
    {L"epub", GlyphStyle::Book, RGB(180, 140, 90)},
    {L"mobi", GlyphStyle::Book, RGB(180, 140, 90)},
    {L"png", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"jpg", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"jpeg", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"gif", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"bmp", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"webp", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"svg", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"ico", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"tif", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"tiff", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"heic", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"psd", GlyphStyle::Picture, RGB(192, 132, 252)},
    {L"mp3", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"wav", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"flac", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"ogg", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"m4a", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"aac", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"opus", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"wma", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"mid", GlyphStyle::Audio, RGB(244, 114, 182)},
    {L"mp4", GlyphStyle::Video, RGB(248, 113, 113)},
    {L"mkv", GlyphStyle::Video, RGB(248, 113, 113)},
    {L"avi", GlyphStyle::Video, RGB(248, 113, 113)},
    {L"mov", GlyphStyle::Video, RGB(248, 113, 113)},
    {L"wmv", GlyphStyle::Video, RGB(248, 113, 113)},
    {L"flv", GlyphStyle::Video, RGB(248, 113, 113)},
    {L"webm", GlyphStyle::Video, RGB(248, 113, 113)},
    {L"3gp", GlyphStyle::Video, RGB(248, 113, 113)},
    {L"zip", GlyphStyle::Archive, RGB(205, 150, 70)},
    {L"rar", GlyphStyle::Archive, RGB(205, 150, 70)},
    {L"7z", GlyphStyle::Archive, RGB(205, 150, 70)},
    {L"tar", GlyphStyle::Archive, RGB(205, 150, 70)},
    {L"gz", GlyphStyle::Archive, RGB(205, 150, 70)},
    {L"bz2", GlyphStyle::Archive, RGB(205, 150, 70)},
    {L"xz", GlyphStyle::Archive, RGB(205, 150, 70)},
    {L"cab", GlyphStyle::Archive, RGB(205, 150, 70)},
    {L"iso", GlyphStyle::Disc, RGB(200, 200, 205)},
    {L"img", GlyphStyle::Disc, RGB(200, 200, 205)},
    {L"bin", GlyphStyle::Disc, RGB(200, 200, 205)},
    {L"vhd", GlyphStyle::Disc, RGB(200, 200, 205)},
    {L"c", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"cpp", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"h", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"hpp", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"cs", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"java", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"py", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"js", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"ts", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"go", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"rs", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"rb", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"php", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"swift", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"kt", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"sql", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"xml", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"yml", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"yaml", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"toml", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"json", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"html", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"css", GlyphStyle::Code, RGB(74, 222, 128)},
    {L"ttf", GlyphStyle::FontTag, RGB(250, 204, 21)},
    {L"otf", GlyphStyle::FontTag, RGB(250, 204, 21)},
    {L"woff", GlyphStyle::FontTag, RGB(250, 204, 21)},
    {L"db", GlyphStyle::Database, RGB(129, 140, 248)},
    {L"sqlite", GlyphStyle::Database, RGB(129, 140, 248)},
    {L"mdb", GlyphStyle::Database, RGB(129, 140, 248)},
};

inline FileGlyph FileGlyphFor(const std::wstring& name, bool isDir) {
    if (isDir) return {GlyphStyle::Folder, RGB(251, 191, 36)};
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size())
        return {GlyphStyle::FilePage, RGB(96, 165, 250)};
    std::wstring ext = FileLower(name.substr(dot + 1));
    for (size_t i = 0; i < sizeof(kExtTable) / sizeof(kExtTable[0]); i++)
        if (ext == kExtTable[i].ext) return {kExtTable[i].style, kExtTable[i].color};
    return {GlyphStyle::FilePage, RGB(96, 165, 250)};
}

// Row -> glyph. Drives are Folder rows whose action is a drive root;
// bang tiles reuse the row's per-site color (letter drawn from the title).
inline FileGlyph GlyphForResult(ResultKind kind, const std::wstring& title,
                                const std::wstring& action, COLORREF bangColor) {
    switch (kind) {
    case ResultKind::App: return {GlyphStyle::AppTile, AppTileColor(title)};
    case ResultKind::Web: return {GlyphStyle::Web, RGB(46, 224, 158)};
    case ResultKind::Bang: return {GlyphStyle::Bang, bangColor};
    case ResultKind::Calc: return {GlyphStyle::Calc, RGB(251, 191, 36)};
    case ResultKind::Setting: return {GlyphStyle::Setting, RGB(45, 212, 191)};
    case ResultKind::Folder:
        if (IsDriveRootDir(action)) return {GlyphStyle::Drive, RGB(251, 191, 36)};
        return {GlyphStyle::Folder, RGB(251, 191, 36)};
    case ResultKind::File: return FileGlyphFor(title, false);
    case ResultKind::Explorer: return {GlyphStyle::Folder, RGB(251, 191, 36)};
    default: return {GlyphStyle::FilePage, RGB(150, 150, 155)};
    }
}

// ------------------------------------------------------- tiny GDI helpers
inline void GlyphLine(HDC dc, int x1, int y1, int x2, int y2, int w, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ o = SelectObject(dc, p);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    SelectObject(dc, o);
    DeleteObject(p);
}
inline void GlyphFillRect(HDC dc, int l, int t, int r, int b, COLORREF c) {
    HBRUSH h = CreateSolidBrush(c);
    RECT rc{l, t, r, b};
    FillRect(dc, &rc, h);
    DeleteObject(h);
}
inline void GlyphFrameRect(HDC dc, int l, int t, int r, int b, int w, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ o = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, l, t, r, b);
    SelectObject(dc, ob);
    SelectObject(dc, o);
    DeleteObject(p);
}
inline void GlyphFillRound(HDC dc, int l, int t, int r, int b, int e, COLORREF c) {
    HBRUSH h = CreateSolidBrush(c);
    HGDIOBJ o = SelectObject(dc, h);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, l, t, r, b, e, e);
    SelectObject(dc, op);
    SelectObject(dc, o);
    DeleteObject(h);
}
inline void GlyphFillEllipse(HDC dc, int l, int t, int r, int b, COLORREF c) {
    HBRUSH h = CreateSolidBrush(c);
    HGDIOBJ o = SelectObject(dc, h);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, l, t, r, b);
    SelectObject(dc, op);
    SelectObject(dc, o);
    DeleteObject(h);
}
inline void GlyphFrameEllipse(HDC dc, int l, int t, int r, int b, int w, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ o = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, l, t, r, b);
    SelectObject(dc, ob);
    SelectObject(dc, o);
    DeleteObject(p);
}
inline void GlyphFillPoly(HDC dc, POINT* pts, int n, COLORREF c) {
    HBRUSH h = CreateSolidBrush(c);
    HGDIOBJ o = SelectObject(dc, h);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, pts, n);
    SelectObject(dc, op);
    SelectObject(dc, o);
    DeleteObject(h);
}

// ------------------------------------------------------- the painter
// cell is a ~20px square. title supplies the Bang letter / app tile initials.
// tagFont supplies those letters (main palette chip font).
inline void PaintGlyph(HDC dc, const RECT& cell, const FileGlyph& g,
                       bool sel, const std::wstring& title, HFONT tagFont) {
    const COLORREF W = RGB(255, 255, 255);
    const COLORREF PAGE = RGB(226, 230, 238);
    const COLORREF FOLD = RGB(170, 175, 185);
    COLORREF base = sel ? g.color : GlyphDim(g.color, 80);
    COLORREF dark = GlyphDim(g.color, 50);
    int x = cell.left, y = cell.top, s = cell.right - cell.left;
    if (s < 12) return;
    int cx = x + s / 2, cy = y + s / 2;
    switch (g.style) {
    case GlyphStyle::Folder:
        GlyphFillRect(dc, x + 1, y + 3, x + 8, y + 6, base); // tab
        GlyphFillRect(dc, x + 1, y + 5, x + s - 1, y + s - 2, base); // body
        break;
    case GlyphStyle::Drive:
        GlyphFillRect(dc, x + 1, y + 7, x + s - 1, y + s - 3, base);
        GlyphLine(dc, x + 4, y + s - 7, x + s - 6, y + s - 7, 2, dark); // slot
        GlyphFillRect(dc, x + s - 5, y + 9, x + s - 3, y + 11, W); // LED
        break;
    case GlyphStyle::FilePage:
    case GlyphStyle::DocLines: {
        POINT pg[5] = {{x + 5, y + 2}, {x + 12, y + 2}, {x + 15, y + 5},
                       {x + 15, y + s - 2}, {x + 5, y + s - 2}};
        GlyphFillPoly(dc, pg, 5, base);
        POINT fd[3] = {{x + 12, y + 2}, {x + 15, y + 5}, {x + 12, y + 5}};
        GlyphFillPoly(dc, fd, 3, dark);
        GlyphLine(dc, x + 7, y + 9, x + 13, y + 9, 1, dark);
        GlyphLine(dc, x + 7, y + 12, x + 13, y + 12, 1, dark);
        if (g.style == GlyphStyle::DocLines)
            GlyphLine(dc, x + 7, y + 15, x + 13, y + 15, 1, dark);
        break;
    }
    case GlyphStyle::Pdf:
    case GlyphStyle::DocWord:
    case GlyphStyle::Sheet:
    case GlyphStyle::Slide:
    case GlyphStyle::Book: {
        POINT pg[5] = {{x + 5, y + 2}, {x + 12, y + 2}, {x + 15, y + 5},
                       {x + 15, y + s - 2}, {x + 5, y + s - 2}};
        GlyphFillPoly(dc, pg, 5, PAGE);
        POINT fd[3] = {{x + 12, y + 2}, {x + 15, y + 5}, {x + 12, y + 5}};
        GlyphFillPoly(dc, fd, 3, FOLD);
        GlyphLine(dc, x + 7, y + 8, x + 13, y + 8, 1, FOLD);
        GlyphFillRect(dc, x + 5, y + s - 6, x + 15, y + s - 2, base); // band
        break;
    }
    case GlyphStyle::Picture:
        GlyphFillRect(dc, x + 3, y + 4, x + s - 3, y + s - 4, base);
        GlyphFillRect(dc, x + 12, y + 7, x + 14, y + 9, W); // sun
        {
            POINT m[5] = {{x + 3, y + s - 4}, {x + 9, y + 9},
                          {x + 12, y + 12}, {x + 14, y + 10}, {x + s - 3, y + s - 4}};
            GlyphFillPoly(dc, m, 5, dark); // mountains
        }
        break;
    case GlyphStyle::Audio:
        GlyphLine(dc, x + 9, y + 4, x + 9, y + 13, 2, base); // stem
        GlyphLine(dc, x + 9, y + 4, x + 14, y + 6, 2, base); // flag
        GlyphFillEllipse(dc, x + 4, y + 12, x + 10, y + 17, base); // head
        break;
    case GlyphStyle::Video: {
        GlyphFillRound(dc, x + 2, y + 5, x + s - 2, y + s - 5, 6, base);
        POINT p[3] = {{x + 8, y + 8}, {x + 8, y + s - 8}, {x + 13, cy}};
        GlyphFillPoly(dc, p, 3, W);
        break;
    }
    case GlyphStyle::Archive:
        GlyphFillRect(dc, x + 4, y + 7, x + s - 4, y + s - 3, base);
        GlyphLine(dc, x + 4, y + 9, x + s - 4, y + 9, 1, dark); // lid
        GlyphLine(dc, cx, y + 7, cx, y + s - 3, 2, dark); // strap
        break;
    case GlyphStyle::Disc:
        GlyphFrameEllipse(dc, x + 2, y + 2, x + s - 2, y + s - 2, 2, base);
        GlyphFrameEllipse(dc, x + 8, y + 8, x + s - 8, y + s - 8, 1, base);
        break;
    case GlyphStyle::Code:
        GlyphLine(dc, x + 7, y + 6, x + 3, y + 10, 2, base); // <
        GlyphLine(dc, x + 3, y + 10, x + 7, y + 14, 2, base);
        GlyphLine(dc, x + 11, y + 5, x + 9, y + 15, 2, base); // /
        GlyphLine(dc, x + 13, y + 6, x + 17, y + 10, 2, base); // >
        GlyphLine(dc, x + 17, y + 10, x + 13, y + 14, 2, base);
        break;
    case GlyphStyle::Exe: {
        GlyphFillRound(dc, x + 2, y + 2, x + s - 2, y + s - 2, 5, base);
        POINT p[3] = {{x + 8, y + 6}, {x + 8, y + 14}, {x + 14, y + 10}};
        GlyphFillPoly(dc, p, 3, W);
        break;
    }
    case GlyphStyle::Terminal:
        GlyphFillRound(dc, x + 2, y + 2, x + s - 2, y + s - 2, 5, base);
        GlyphLine(dc, x + 5, y + 7, x + 9, y + 10, 2, W); // >
        GlyphLine(dc, x + 9, y + 10, x + 5, y + 13, 2, W);
        GlyphLine(dc, x + 11, y + 13, x + 15, y + 13, 2, W); // _
        break;
    case GlyphStyle::Shortcut: {
        POINT pg[5] = {{x + 3, y + 2}, {x + 9, y + 2}, {x + 11, y + 4},
                       {x + 11, y + 12}, {x + 3, y + 12}};
        GlyphFillPoly(dc, pg, 5, PAGE);
        GlyphFillRound(dc, x + 8, y + 10, x + s - 1, y + s - 1, 4, base);
        GlyphLine(dc, x + 10, y + 14, x + 15, y + 14, 2, W); // arrow
        GlyphLine(dc, x + 13, y + 12, x + 15, y + 14, 2, W);
        GlyphLine(dc, x + 15, y + 14, x + 13, y + 16, 2, W);
        break;
    }
    case GlyphStyle::Calc:
        GlyphFrameEllipse(dc, x + 3, y + 3, x + s - 3, y + s - 3, 2, base);
        GlyphLine(dc, x + 7, y + 8, x + 13, y + 8, 2, base); // =
        GlyphLine(dc, x + 7, y + 12, x + 13, y + 12, 2, base);
        break;
    case GlyphStyle::Setting:
        GlyphLine(dc, x + 3, y + 7, x + s - 3, y + 7, 1, GlyphDim(W, 65)); // rails
        GlyphLine(dc, x + 3, y + 13, x + s - 3, y + 13, 1, GlyphDim(W, 65));
        GlyphFillRect(dc, x + 6, y + 6, x + 8, y + 8, base); // knobs
        GlyphFillRect(dc, x + 12, y + 12, x + 14, y + 14, base);
        break;
    case GlyphStyle::AppTile: {
        GlyphFillRound(dc, x + 1, y + 1, x + s - 1, y + s - 1, 5, base);
        if (tagFont) {
            std::wstring initials = AppTileLetters(title);
            HGDIOBJ old = SelectObject(dc, tagFont);
            COLORREF ot = SetTextColor(dc, W);
            int ob = SetBkMode(dc, TRANSPARENT);
            RECT tr{cell.left, cell.top, cell.right, cell.bottom};
            DrawTextW(dc, initials.c_str(), (int)initials.size(), &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SetBkMode(dc, ob);
            SetTextColor(dc, ot);
            SelectObject(dc, old);
        }
        break;
    }
    case GlyphStyle::Web:
        GlyphFrameEllipse(dc, x + 3, y + 3, x + s - 3, y + s - 3, 2, base);
        GlyphFrameEllipse(dc, x + 7, y + 3, x + s - 7, y + s - 3, 1, base); // meridian
        GlyphLine(dc, x + 3, cy, x + s - 3, cy, 1, base); // equator
        break;
    case GlyphStyle::Bang: {
        GlyphFillRound(dc, x + 1, y + 1, x + s - 1, y + s - 1, 5, base);
        if (tagFont) {
            wchar_t b[2] = {title.empty() ? L'?' : title[0], 0};
            HGDIOBJ old = SelectObject(dc, tagFont);
            COLORREF ot = SetTextColor(dc, W);
            int ob = SetBkMode(dc, TRANSPARENT);
            RECT tr{cell.left, cell.top, cell.right, cell.bottom};
            DrawTextW(dc, b, 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetBkMode(dc, ob);
            SetTextColor(dc, ot);
            SelectObject(dc, old);
        }
        break;
    }
    case GlyphStyle::FontTag: {
        POINT pg[5] = {{x + 5, y + 2}, {x + 12, y + 2}, {x + 15, y + 5},
                       {x + 15, y + s - 2}, {x + 5, y + s - 2}};
        GlyphFillPoly(dc, pg, 5, PAGE);
        GlyphLine(dc, x + 7, y + 15, x + 10, y + 8, 2, base); // A
        GlyphLine(dc, x + 10, y + 8, x + 13, y + 15, 2, base);
        GlyphLine(dc, x + 8, y + 13, x + 12, y + 13, 2, base);
        break;
    }
    case GlyphStyle::Database:
        GlyphFillRect(dc, x + 5, y + 6, x + 15, y + s - 3, base); // body
        GlyphFillEllipse(dc, x + 5, y + 3, x + 15, y + 8, base); // top rim
        GlyphFrameEllipse(dc, x + 5, y + 3, x + 15, y + 8, 1, dark);
        break;
    }
}
