#pragma once
// SneekPeek - context-menu verbs: pure mapping from row/file context to the
// verb list, so menus are unit-testable without popping UI. Execution lives
// in main.cpp (shell verbs, clipboard, settings writes).
//
// Menu language (kept in sync with main.cpp's dark owner-drawn menu):
// destructive verbs (Delete/Rename/Format) are deliberately absent — a
// launcher opens, reveals, and copies; Explorer owns destruction.
#include <string>
#include <vector>
#include "results.h"
#include "icons.h"

enum class VerbId {
    Open,           // launch / drill in
    OpenNewWindow,  // folders: separate Explorer window
    RunAsAdmin,     // exe/scripts via the "runas" shell verb
    OpenLocation,   // explorer.exe /select — works for every real path
    OpenWith,       // OpenAs_RunDLL dialog
    Print,          // shell "print" verb (documents)
    SetWallpaper,   // images only (SPI_SETDESKWALLPAPER)
    ExtractAll,     // archives (shell verb, falls back to Open)
    Eject,          // removable/optical drives (Safely-Remove dialog)
    CopyPath,       // full filesystem path / URL / URI as applicable
    CopyName,       // display name only
    CopyUrl,        // web/bang target URL, settings URI
    CopyResult,     // calculator result text
    Properties,     // shell properties dialog for real paths
    HideApp,        // apps: add to hiddenApps (never shown again)
    ManageBangs,    // open Settings (bangs are edited there)
    OpenSettings,
    RefreshApps,    // full background rescan
    ToggleStartup,  // Run-at-startup check item
    Quit,
    SwitchEngine,   // submenu parent: web search engine presets
    Count
};

struct VerbCtx {
    ResultKind kind = ResultKind::Web;
    GlyphStyle glyph = GlyphStyle::FilePage; // File rows only
    bool isStore = false;     // App rows: Store apps have no file path
    bool isDrive = false;     // Folder rows at drive roots ("X:\\")
    bool isRemovable = false; // drives: optical/removable -> Eject offered
    bool hasPath = false;     // row carries a real filesystem path
};

// Display label for a verb. Engine names and toggle states are composed by
// the caller (they carry runtime data); everything else is static here.
inline const wchar_t* VerbLabel(VerbId v) {
    switch (v) {
    case VerbId::Open: return L"Open";
    case VerbId::OpenNewWindow: return L"Open in new window";
    case VerbId::RunAsAdmin: return L"Run as administrator";
    case VerbId::OpenLocation: return L"Open file location";
    case VerbId::OpenWith: return L"Open with...";
    case VerbId::Print: return L"Print";
    case VerbId::SetWallpaper: return L"Set as desktop background";
    case VerbId::ExtractAll: return L"Extract all...";
    case VerbId::Eject: return L"Eject";
    case VerbId::CopyPath: return L"Copy path";
    case VerbId::CopyName: return L"Copy name";
    case VerbId::CopyUrl: return L"Copy link";
    case VerbId::CopyResult: return L"Copy result";
    case VerbId::Properties: return L"Properties";
    case VerbId::HideApp: return L"Hide from suggestions";
    case VerbId::ManageBangs: return L"Manage bangs...";
    case VerbId::OpenSettings: return L"Settings...";
    case VerbId::RefreshApps: return L"Refresh apps";
    case VerbId::ToggleStartup: return L"Run at startup";
    case VerbId::Quit: return L"Quit";
    case VerbId::SwitchEngine: return L"Search engine";
    default: return L"";
    }
}

inline std::vector<VerbId> VerbsFor(const VerbCtx& c) {
    std::vector<VerbId> v;
    switch (c.kind) {
    case ResultKind::App:
        v.push_back(VerbId::Open);
        if (!c.isStore && c.hasPath) {
            v.push_back(VerbId::RunAsAdmin);
            v.push_back(VerbId::OpenLocation);
            v.push_back(VerbId::CopyPath);
        }
        v.push_back(VerbId::HideApp);
        v.push_back(VerbId::CopyName);
        return v;
    case ResultKind::Web:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::CopyUrl);
        v.push_back(VerbId::SwitchEngine);
        return v;
    case ResultKind::Bang:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::CopyUrl);
        v.push_back(VerbId::ManageBangs);
        return v;
    case ResultKind::Calc:
        v.push_back(VerbId::CopyResult);
        return v;
    case ResultKind::Setting:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::CopyUrl);
        return v;
    case ResultKind::Explorer:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::CopyPath);
        return v;
    case ResultKind::Header:
        return v; // empty: caller shows the palette menu instead
    case ResultKind::Folder:
        v.push_back(VerbId::Open);
        if (c.isDrive) {
            if (c.isRemovable) v.push_back(VerbId::Eject);
        } else {
            v.push_back(VerbId::OpenNewWindow);
        }
        v.push_back(VerbId::CopyPath);
        v.push_back(VerbId::Properties);
        return v;
    case ResultKind::File:
        break; // dispatched on glyph below
    }
    // Files, grouped by glyph family (mirrors the kExtTable groupings).
    switch (c.glyph) {
    case GlyphStyle::Exe:
    case GlyphStyle::Terminal:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::RunAsAdmin);
        v.push_back(VerbId::OpenLocation);
        v.push_back(VerbId::CopyPath);
        v.push_back(VerbId::Properties);
        return v;
    case GlyphStyle::Shortcut:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::OpenLocation);
        v.push_back(VerbId::CopyPath);
        v.push_back(VerbId::Properties);
        return v;
    case GlyphStyle::Picture:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::OpenWith);
        v.push_back(VerbId::SetWallpaper);
        v.push_back(VerbId::CopyPath);
        v.push_back(VerbId::Properties);
        return v;
    case GlyphStyle::Audio:
    case GlyphStyle::Video:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::OpenWith);
        v.push_back(VerbId::CopyPath);
        v.push_back(VerbId::Properties);
        return v;
    case GlyphStyle::Archive:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::ExtractAll);
        v.push_back(VerbId::CopyPath);
        v.push_back(VerbId::Properties);
        return v;
    case GlyphStyle::DocLines:
    case GlyphStyle::Pdf:
    case GlyphStyle::DocWord:
    case GlyphStyle::Sheet:
        v.push_back(VerbId::Open);
        v.push_back(VerbId::OpenWith);
        v.push_back(VerbId::Print);
        v.push_back(VerbId::CopyPath);
        v.push_back(VerbId::Properties);
        return v;
    default: // Slide, Book, FontTag, Database, Code, Disc, FilePage, ...
        v.push_back(VerbId::Open);
        v.push_back(VerbId::OpenWith);
        v.push_back(VerbId::CopyPath);
        v.push_back(VerbId::Properties);
        return v;
    }
}
