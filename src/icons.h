#pragma once
// SneekPeek - predefined kind/extension glyphs, drawn not loaded.
// Header-only. No icon extraction anywhere (no SHGetFileInfo, no ExtractIcon,
// no image files): every glyph is GDI shapes painted in WM_DRAWITEM,
// so cost is ~9 visible rows x a handful of calls per repaint — no disk I/O,
// no cache, no RAM growth. The language is rounded 3D: soft drop shade,
// saturated body, light top gloss, bright crest line. Mapping helpers are
// pure and unit-tested.
#include <windows.h>
#include <string>
#include "results.h"
#include "files.h"

enum class GlyphStyle {
    Web, Bang, Calc, Setting, Folder, Drive, FilePage,
    Exe, Terminal, Shortcut, DocLines, Pdf, DocWord, Sheet, Slide,
    Book, Picture, Audio, Video, Archive, Disc, Code, FontTag, Database,
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
inline void GlyphArc(HDC dc, int l, int t, int r, int b,
                     int x1, int y1, int x2, int y2, int w, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ o = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Arc(dc, l, t, r, b, x1, y1, x2, y2);
    SelectObject(dc, ob);
    SelectObject(dc, o);
    DeleteObject(p);
}

// ------------------------------------------------------- rounded 3D language
// All 3D comes from lighter-toward-top shading (drop shadows would vanish on
// the black rows): each solid shape gets a dark drop shade, a saturated body,
// a light top gloss and a bright crest line. Geometry derives from the cell
// so DPI scaling follows automatically. Callers precompute lite/base/dark.
inline COLORREF GlyphLighten(COLORREF c, int amt) {
    int r = GetRValue(c) + amt; if (r > 255) r = 255;
    int g = GetGValue(c) + amt; if (g > 255) g = 255;
    int b = GetBValue(c) + amt; if (b > 255) b = 255;
    return RGB(r, g, b);
}
// Glossy rounded tile: (l,t,r,b) body rect, rad corner diameter.
inline void GlyphTile3D(HDC dc, int l, int t, int r, int b, int rad,
                        COLORREF lite, COLORREF base, COLORREF dark) {
    GlyphFillRound(dc, l + 1, t + 2, r + 1, b + 1, rad, dark); // drop shade
    GlyphFillRound(dc, l, t, r, b, rad, base);                 // body
    int mid = t + (b - t) * 42 / 100;
    if (mid > t + 2)
        GlyphFillRound(dc, l + 2, t + 1, r - 2, mid, rad > 2 ? rad - 2 : 1, lite);
    if (r - l > 10)
        GlyphLine(dc, l + 4, t + 1, r - 4, t + 1, 1, GlyphLighten(base, 110));
}
// Bright top-left arc highlight for rings and rims (globe, disc, button).
inline void GlyphHiArc(HDC dc, int l, int t, int r, int b, COLORREF base) {
    int cx = (l + r) / 2, cy = (t + b) / 2;
    GlyphArc(dc, l, t, r, b, l, cy, cx, t, 2, GlyphLighten(base, 110));
}

// ------------------------------------------------------- the painter
// cell is a ~20px square. title supplies the Bang letter.
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
    case GlyphStyle::Folder: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphFillRound(dc, x + 1, y + 4, x + 8, y + 7, 2, dark); // tab shade
        GlyphFillRound(dc, x + 1, y + 3, x + 8, y + 6, 2, base); // tab
        GlyphTile3D(dc, x + 1, y + 5, x + s - 2, y + s - 3, 3, lite, base, dark);
        break;
    }
    case GlyphStyle::Drive: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphTile3D(dc, x + 1, y + 7, x + s - 2, y + s - 4, 3, lite, base, dark);
        GlyphLine(dc, x + 4, y + s - 8, x + s - 6, y + s - 8, 2, dark); // slot
        GlyphLine(dc, x + 4, y + s - 9, x + s - 6, y + s - 9, 1, lite); // slot sheen
        GlyphFillEllipse(dc, x + s - 6, y + 9, x + s - 4, y + 11, W); // LED
        break;
    }
    case GlyphStyle::FilePage:
    case GlyphStyle::DocLines: {
        POINT sh[5] = {{x + 6, y + 3}, {x + 13, y + 3}, {x + 16, y + 6},
                       {x + 16, y + s - 1}, {x + 6, y + s - 1}};
        GlyphFillPoly(dc, sh, 5, RGB(18, 18, 22)); // drop shade
        POINT pg[5] = {{x + 5, y + 2}, {x + 12, y + 2}, {x + 15, y + 5},
                       {x + 15, y + s - 2}, {x + 5, y + s - 2}};
        GlyphFillPoly(dc, pg, 5, base);
        GlyphLine(dc, x + 6, y + 3, x + 11, y + 3, 1, GlyphLighten(base, 110));
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
        POINT sh[5] = {{x + 6, y + 3}, {x + 13, y + 3}, {x + 16, y + 6},
                       {x + 16, y + s - 1}, {x + 6, y + s - 1}};
        GlyphFillPoly(dc, sh, 5, RGB(18, 18, 22)); // drop shade
        POINT pg[5] = {{x + 5, y + 2}, {x + 12, y + 2}, {x + 15, y + 5},
                       {x + 15, y + s - 2}, {x + 5, y + s - 2}};
        GlyphFillPoly(dc, pg, 5, PAGE);
        GlyphLine(dc, x + 6, y + 3, x + 11, y + 3, 1, W); // top crest
        POINT fd[3] = {{x + 12, y + 2}, {x + 15, y + 5}, {x + 12, y + 5}};
        GlyphFillPoly(dc, fd, 3, FOLD);
        GlyphLine(dc, x + 7, y + 8, x + 13, y + 8, 1, FOLD);
        GlyphFillRect(dc, x + 5, y + s - 6, x + 15, y + s - 2, base); // band
        GlyphLine(dc, x + 6, y + s - 5, x + 14, y + s - 5, 1, GlyphLighten(base, 90));
        break;
    }
    case GlyphStyle::Picture: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphTile3D(dc, x + 3, y + 4, x + s - 4, y + s - 5, 3, lite, base, dark);
        GlyphFillEllipse(dc, x + 11, y + 6, x + 15, y + 10, lite); // sun glow
        GlyphFillRect(dc, x + 12, y + 7, x + 14, y + 9, W); // sun core
        POINT m[5] = {{x + 4, y + s - 5}, {x + 9, y + 9},
                      {x + 12, y + 12}, {x + 14, y + 10}, {x + s - 4, y + s - 5}};
        GlyphFillPoly(dc, m, 5, dark); // mountains
        GlyphLine(dc, x + 5, y + 12, x + 8, y + 9, 1, lite); // ridge light
        break;
    }
    case GlyphStyle::Audio:
        GlyphLine(dc, x + 10, y + 5, x + 10, y + 14, 2, dark); // depth offsets
        GlyphLine(dc, x + 10, y + 5, x + 15, y + 7, 2, dark);
        GlyphFillEllipse(dc, x + 5, y + 13, x + 11, y + 18, dark);
        GlyphLine(dc, x + 9, y + 4, x + 9, y + 13, 2, base); // stem
        GlyphLine(dc, x + 9, y + 4, x + 14, y + 6, 2, base); // flag
        GlyphFillEllipse(dc, x + 4, y + 12, x + 10, y + 17, base); // head
        GlyphFillEllipse(dc, x + 5, y + 13, x + 7, y + 15, GlyphLighten(base, 120));
        break;
    case GlyphStyle::Video: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphTile3D(dc, x + 2, y + 5, x + s - 4, y + s - 6, 6, lite, base, dark);
        POINT sh[3] = {{x + 9, y + 9}, {x + 9, y + s - 9}, {x + 14, cy}};
        GlyphFillPoly(dc, sh, 3, dark);
        POINT p[3] = {{x + 8, y + 8}, {x + 8, y + s - 8}, {x + 13, cy}};
        GlyphFillPoly(dc, p, 3, W);
        break;
    }
    case GlyphStyle::Archive: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphTile3D(dc, x + 4, y + 7, x + s - 5, y + s - 4, 2, lite, base, dark);
        GlyphLine(dc, cx - 1, y + 7, cx - 1, y + s - 4, 2, dark); // strap
        GlyphLine(dc, cx + 1, y + 7, cx + 1, y + s - 4, 1, lite); // strap sheen
        break;
    }
    case GlyphStyle::Disc: {
        COLORREF lite = GlyphLighten(base, 90);
        GlyphFrameEllipse(dc, x + 2, y + 2, x + s - 2, y + s - 2, 2, base);
        GlyphHiArc(dc, x + 2, y + 2, x + s - 2, y + s - 2, base);
        GlyphFrameEllipse(dc, x + 8, y + 8, x + s - 8, y + s - 8, 1, base);
        GlyphFillEllipse(dc, cx - 1, cy - 1, cx + 1, cy + 1, lite); // hub glint
        break;
    }
    case GlyphStyle::Code: {
        COLORREF halo = GlyphLighten(base, 30);
        GlyphLine(dc, x + 7, y + 6, x + 3, y + 10, 3, halo); // glow pass
        GlyphLine(dc, x + 3, y + 10, x + 7, y + 14, 3, halo);
        GlyphLine(dc, x + 11, y + 5, x + 9, y + 15, 3, halo);
        GlyphLine(dc, x + 13, y + 6, x + 17, y + 10, 3, halo);
        GlyphLine(dc, x + 17, y + 10, x + 13, y + 14, 3, halo);
        GlyphLine(dc, x + 7, y + 6, x + 3, y + 10, 2, base); // core <
        GlyphLine(dc, x + 3, y + 10, x + 7, y + 14, 2, base);
        GlyphLine(dc, x + 11, y + 5, x + 9, y + 15, 2, base); // core /
        GlyphLine(dc, x + 13, y + 6, x + 17, y + 10, 2, base); // core >
        GlyphLine(dc, x + 17, y + 10, x + 13, y + 14, 2, base);
        break;
    }
    case GlyphStyle::Exe: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphTile3D(dc, x + 2, y + 2, x + s - 4, y + s - 4, 5, lite, base, dark);
        POINT sh[3] = {{x + 9, y + 7}, {x + 9, y + 15}, {x + 15, y + 11}};
        GlyphFillPoly(dc, sh, 3, dark);
        POINT p[3] = {{x + 8, y + 6}, {x + 8, y + 14}, {x + 14, y + 10}};
        GlyphFillPoly(dc, p, 3, W);
        break;
    }
    case GlyphStyle::Terminal: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphTile3D(dc, x + 2, y + 2, x + s - 4, y + s - 4, 5, lite, base, dark);
        GlyphLine(dc, x + 5, y + 7, x + 9, y + 10, 2, W); // >
        GlyphLine(dc, x + 9, y + 10, x + 5, y + 13, 2, W);
        GlyphLine(dc, x + 11, y + 13, x + 15, y + 13, 2, W); // _
        GlyphFillEllipse(dc, x + 11, y + 12, x + 13, y + 14, GlyphLighten(base, 120));
        break;
    }
    case GlyphStyle::Shortcut: {
        COLORREF lite = GlyphLighten(base, 45);
        POINT sh[5] = {{x + 4, y + 3}, {x + 10, y + 3}, {x + 12, y + 5},
                       {x + 12, y + 13}, {x + 4, y + 13}};
        GlyphFillPoly(dc, sh, 5, RGB(18, 18, 22)); // page shade
        POINT pg[5] = {{x + 3, y + 2}, {x + 9, y + 2}, {x + 11, y + 4},
                       {x + 11, y + 12}, {x + 3, y + 12}};
        GlyphFillPoly(dc, pg, 5, PAGE);
        GlyphLine(dc, x + 4, y + 3, x + 8, y + 3, 1, W); // page crest
        GlyphTile3D(dc, x + 8, y + 10, x + s - 2, y + s - 2, 4, lite, base, dark);
        GlyphLine(dc, x + 10, y + 14, x + 15, y + 14, 2, W); // arrow
        GlyphLine(dc, x + 13, y + 12, x + 15, y + 14, 2, W);
        GlyphLine(dc, x + 15, y + 14, x + 13, y + 16, 2, W);
        break;
    }
    case GlyphStyle::Calc:
        GlyphFrameEllipse(dc, x + 3, y + 3, x + s - 3, y + s - 3, 2, base);
        GlyphHiArc(dc, x + 3, y + 3, x + s - 3, y + s - 3, base);
        GlyphLine(dc, x + 7, y + 8, x + 13, y + 8, 2, base); // =
        GlyphLine(dc, x + 7, y + 9, x + 13, y + 9, 1, GlyphLighten(base, 110));
        GlyphLine(dc, x + 7, y + 12, x + 13, y + 12, 2, base);
        break;
    case GlyphStyle::Setting: {
        COLORREF lite = GlyphLighten(base, 60);
        GlyphLine(dc, x + 3, y + 7, x + s - 3, y + 7, 1, GlyphDim(W, 65)); // rails
        GlyphLine(dc, x + 3, y + 13, x + s - 3, y + 13, 1, GlyphDim(W, 65));
        GlyphFillEllipse(dc, x + 5, y + 5, x + 9, y + 9, base); // knobs
        GlyphFillEllipse(dc, x + 5, y + 5, x + 7, y + 7, lite);
        GlyphFillEllipse(dc, x + 11, y + 11, x + 15, y + 15, base);
        GlyphFillEllipse(dc, x + 11, y + 11, x + 13, y + 13, lite);
        break;
    }
    case GlyphStyle::Web: {
        // Search magnifier: ring lens + handle. Reads as "search" at a
        // glance; the single definition serves list rows, tiles, and
        // history alike. Geometry derives from s so DPI follows.
        int B = s * 12 / 20; // lens diameter
        if (B < 8) B = 8;
        int lw = s >= 26 ? 3 : 2; // ring width
        int lx = x + 2, ly = y + 1; // lens box top-left
        GlyphFrameEllipse(dc, lx + 1, ly + 2, lx + B + 1, ly + B + 2, lw, dark);
        GlyphFrameEllipse(dc, lx, ly, lx + B, ly + B, lw, base); // ring
        GlyphHiArc(dc, lx, ly, lx + B, ly + B, base); // gloss
        GlyphFillEllipse(dc, lx + B / 4, ly + B / 4 - 1,
                         lx + B / 4 + 2, ly + B / 4 + 1, W); // specular dot
        int hx = lx + B - 2, hy = ly + B - 2; // handle root at the ring
        GlyphLine(dc, hx + 1, hy + 1, x + s - 2, y + s - 1, lw + 1, dark);
        GlyphLine(dc, hx, hy, x + s - 3, y + s - 2, lw, base); // handle
        break;
    }
    case GlyphStyle::Bang: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphTile3D(dc, x + 1, y + 1, x + s - 2, y + s - 2, 5, lite, base, dark);
        if (tagFont) {
            wchar_t b[2] = {title.empty() ? L'?' : title[0], 0};
            HGDIOBJ old = SelectObject(dc, tagFont);
            int ob = SetBkMode(dc, TRANSPARENT);
            RECT tr{cell.left + 1, cell.top + 1, cell.right + 1, cell.bottom + 1};
            COLORREF ot = SetTextColor(dc, dark); // letter drop shade
            DrawTextW(dc, b, 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            tr.left--; tr.top--; tr.right--; tr.bottom--;
            SetTextColor(dc, W);
            DrawTextW(dc, b, 1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetBkMode(dc, ob);
            SetTextColor(dc, ot);
            SelectObject(dc, old);
        }
        break;
    }
    case GlyphStyle::FontTag: {
        POINT sh[5] = {{x + 6, y + 3}, {x + 13, y + 3}, {x + 16, y + 6},
                       {x + 16, y + s - 1}, {x + 6, y + s - 1}};
        GlyphFillPoly(dc, sh, 5, RGB(18, 18, 22)); // drop shade
        POINT pg[5] = {{x + 5, y + 2}, {x + 12, y + 2}, {x + 15, y + 5},
                       {x + 15, y + s - 2}, {x + 5, y + s - 2}};
        GlyphFillPoly(dc, pg, 5, PAGE);
        GlyphLine(dc, x + 6, y + 3, x + 11, y + 3, 1, W); // top crest
        GlyphLine(dc, x + 7, y + 15, x + 10, y + 8, 2, base); // A
        GlyphLine(dc, x + 10, y + 8, x + 13, y + 15, 2, base);
        GlyphLine(dc, x + 8, y + 13, x + 12, y + 13, 2, base);
        GlyphLine(dc, x + 8, y + 14, x + 12, y + 14, 1, GlyphLighten(base, 60));
        break;
    }
    case GlyphStyle::Database: {
        COLORREF lite = GlyphLighten(base, 45);
        GlyphFillRound(dc, x + 6, y + 7, x + 16, y + s - 2, 3, dark); // body shade
        GlyphFillRound(dc, x + 5, y + 6, x + 15, y + s - 3, 3, base); // body
        GlyphLine(dc, x + 6, y + 7, x + 14, y + 7, 1, lite); // body sheen
        GlyphFillEllipse(dc, x + 5, y + 3, x + 15, y + 8, lite); // rim light
        GlyphFillEllipse(dc, x + 6, y + 4, x + 14, y + 8, dark); // opening
        GlyphHiArc(dc, x + 5, y + 3, x + 15, y + 8, base);
        break;
    }
    }
}
