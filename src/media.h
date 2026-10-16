#pragma once
// SneekPeek - built-in image viewer + video player: pure decision logic.
// Header-only, no Win32 dependency beyond windows.h types (RECT, HMONITOR,
// HWND in declarations only), so everything here is unit-tested headlessly.
// Thin Win32/COM rendering lives in media_win.cpp (untested by convention);
// palette wiring lives in main.cpp.
//
// Decoder policy (do not "fix" by adding FFmpeg): Windows already ships the
// codecs. Images decode via WIC, video plays via Media Foundation with a
// D3D11 device manager (hardware when present, software fallback automatic).
// A from-scratch player would mean H.264/H.265/AV1 + AAC decode: 30-80 MB
// plus LGPL/GPL licensing, against every constraint this product has.
#include <windows.h> // RECT, HMONITOR, HWND (types only, no calls)
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

enum class MediaKind { None, Image, Video, Unsupported };

// Lowercase extension without the dot ("" when none). Trailing spaces/dots
// are stripped first: a name like "photo.jpg " (legal from zips/macOS)
// must still classify as jpeg, or it falls through to the default app.
// Pure, unit-tested.
inline std::wstring MediaExt(const std::wstring& path) {
    std::wstring p = path;
    while (!p.empty() && (p.back() == L' ' || p.back() == L'.')) p.pop_back();
    size_t sep = p.find_last_of(L"\\/");
    size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    if (sep != std::wstring::npos && dot < sep) return L"";
    if (dot + 1 >= p.size()) return L"";
    std::wstring e = p.substr(dot + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

// Extension -> kind. The Image set is exactly what WIC decodes on
// Windows 10 19045 (no HEIC/WebP codec present there); the Video set is
// containers Media Foundation accepts (missing codecs fail gracefully at
// playback time, never at startup). Pure, unit-tested.
inline MediaKind ClassifyMedia(const std::wstring& path) {
    std::wstring e = MediaExt(path);
    if (e.empty()) return MediaKind::None;
    if (e == L"heic" || e == L"heif" || e == L"avif" || e == L"webp")
        return MediaKind::Unsupported; // no in-box codec on Win10 19045
    if (e == L"jpg" || e == L"jpeg" || e == L"png" || e == L"bmp" ||
        e == L"gif" || e == L"tif" || e == L"tiff" || e == L"ico")
        return MediaKind::Image;
    if (e == L"mp4" || e == L"m4v" || e == L"mov" || e == L"mkv" ||
        e == L"avi" || e == L"wmv" || e == L"asf" || e == L"mpg" ||
        e == L"mpeg" || e == L"mpe" || e == L"3gp" || e == L"3g2" ||
        e == L"webm" || e == L"flv" || e == L"m2ts")
        return MediaKind::Video;
    return MediaKind::None;
}

// ------------------------------------------------------- palette snapshot
// Captured BEFORE HidePalette() destroys file state (files.h: state is one
// g_fileDir string, cleared on hide), restored when the media window closes.
struct PaletteSnapshot {
    bool valid = false;
    bool fileActive = false;
    std::wstring fileDir;   // read BEFORE HidePalette() clears it
    std::wstring query;     // edit-control text
    std::wstring selAction; // selected row's action (match by this, not index)
    int selIndex = -1;      // fast path only; -1 when nothing selected
    int gridTopRow = 0;     // file-grid scroll position
    int listTop = 0;        // listbox top index
    RECT rect{};            // palette window position
    HMONITOR monitor = NULL;
};

// Row lookup by action string: immune to result reordering (index of the
// first match, or -1). The deleted-file case resolves to -1 and the caller
// falls back to FirstSelectable, so the row is simply absent. Pure.
inline int MediaFindRowByAction(const std::vector<std::wstring>& actions,
                                const std::wstring& action) {
    if (action.empty()) return -1;
    for (size_t i = 0; i < actions.size(); i++)
        if (actions[i] == action) return (int)i;
    return -1;
}

// Restore selection: action match wins; anything else (-1) lets the caller
// pick its normal default. Pure, unit-tested.
inline int MediaResolveSelection(const std::vector<std::wstring>& actions,
                                 const std::wstring& selAction, int /*selIndex*/) {
    return MediaFindRowByAction(actions, selAction);
}

// ------------------------------------------------------- image view math
inline double MediaClampZoom(double z) {
    if (!(z > 0.0)) return 1.0; // NaN/zero/negative: safe default
    if (z < 0.05) return 0.05;
    if (z > 32.0) return 32.0;
    return z;
}

// Scale that fits imgWxH inside viewWxH (aspect preserved). Degenerate
// inputs yield 1.0, never 0/divide-by-zero. Pure, unit-tested.
inline double MediaFitScale(int imgW, int imgH, int viewW, int viewH) {
    if (imgW <= 0 || imgH <= 0 || viewW <= 0 || viewH <= 0) return 1.0;
    double s = (double)viewW / (double)imgW;
    double t = (double)viewH / (double)imgH;
    return s < t ? s : t;
}

// Native window size for an image: native pixels clamped to the work area
// (aspect preserved), so small images show 1:1 and large ones fit with no
// letterbox bars. Pure, unit-tested.
inline void MediaFitWindow(int imgW, int imgH, int areaW, int areaH,
                           int& outW, int& outH) {
    if (imgW <= 0 || imgH <= 0 || areaW <= 0 || areaH <= 0) {
        outW = 640; outH = 480;
        return;
    }
    double s = MediaFitScale(imgW, imgH, areaW, areaH);
    if (s > 1.0) s = 1.0; // never upscale for the initial window
    outW = (int)(imgW * s + 0.5);
    outH = (int)(imgH * s + 0.5);
    if (outW < 1) outW = 1;
    if (outH < 1) outH = 1;
}

// Clamp pan offsets: content smaller than the view centers, larger content
// clamps to [view - content, 0] on each axis. Pure, unit-tested.
inline void MediaClampPan(double& ox, double& oy,
                          int contentW, int contentH, int viewW, int viewH) {
    ox = (contentW <= viewW) ? (viewW - contentW) / 2.0
                             : (ox < viewW - contentW ? viewW - contentW
                                                      : (ox > 0.0 ? 0.0 : ox));
    oy = (contentH <= viewH) ? (viewH - contentH) / 2.0
                             : (oy < viewH - contentH ? viewH - contentH
                                                      : (oy > 0.0 ? 0.0 : oy));
}

// Display size after quarter-turn rotation (odd turns swap axes).
inline void MediaRotatedSize(int w, int h, int quarterTurns, int& outW, int& outH) {
    int t = ((quarterTurns % 4) + 4) % 4;
    if (t == 1 || t == 3) { outW = h; outH = w; return; }
    outW = w; outH = h;
}

// ------------------------------------------------------- time + speed
// m:ss below one hour ("0:05", "59:59"), h:mm:ss at/above ("1:00:00").
// Negative/NaN/infinite input yields "0:00", never garbage. Pure.
inline std::wstring MediaFormatTime(double seconds) {
    if (!(seconds > 0.0)) return L"0:00"; // covers NaN, negative, +inf check below
    if (seconds > 365 * 86400.0) return L"0:00"; // absurd: fail safe, not wrap
    long long total = (long long)seconds; // floor toward display
    long long h = total / 3600;
    long long m = (total % 3600) / 60;
    long long s = total % 60;
    wchar_t b[32];
    if (h > 0) swprintf_s(b, 32, L"%lld:%02lld:%02lld", h, m, s);
    else swprintf_s(b, 32, L"%lld:%02lld", m, s);
    return b;
}

inline std::wstring MediaFormatTimePair(double cur, double dur) {
    return MediaFormatTime(cur) + L" / " + MediaFormatTime(dur);
}

inline constexpr double kMediaSpeeds[] = {
    0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, 4.0
};
inline int MediaSpeedCount() {
    return (int)(sizeof(kMediaSpeeds) / sizeof(kMediaSpeeds[0]));
}
inline int MediaSpeedDefaultIndex() { return 3; } // 1.0x
inline int MediaSpeedClamp(int i) {
    if (i < 0) return 0;
    if (i >= MediaSpeedCount()) return MediaSpeedCount() - 1;
    return i;
}
inline std::wstring MediaFormatSpeed(double s) {
    wchar_t b[16];
    swprintf_s(b, 16, L"%gx", s);
    return b;
}
inline std::wstring MediaVolumeLabel(bool muted, double vol) {
    if (muted) return L"Muted";
    if (!(vol > 0.0)) return L"0%";
    if (vol > 1.0) vol = 1.0;
    wchar_t b[16];
    swprintf_s(b, 16, L"%d%%", (int)(vol * 100.0 + 0.5));
    return b;
}

// ------------------------------------------------------- host bridge
// Implemented by main.cpp / media_win.cpp (NOT unit-tested by convention;
// declarations here are never ODR-used by the test build).
static const wchar_t* kMediaClass = L"SneekPeekMedia";
// Open path in the media window. Returns false + err when nothing was
// shown (decode/playback failure before first frame): the caller restores
// the palette and balloons err. Never leaves a black/hung window.
bool MediaOpen(HINSTANCE hInst, HWND owner, int dpi, const std::wstring& path,
               std::wstring& err);
HWND MediaHwnd();
LRESULT CALLBACK MediaWndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l);
// Called by the media window on EVERY close path (Esc, click-outside,
// delete-then-empty, playback error): exactly once per open.
void MediaOnClosed(bool failed, const std::wstring& err);
// Hardware video-decode probe (MFT_ENUM_FLAG_HARDWARE): 1 = GPU decoder
// present, 0 = software only, -1 = unknown. Cached after first call.
int MediaHardwareDecode();
// Human-readable diagnostic for Settings -> General.
std::wstring MediaDecodeLabel();
