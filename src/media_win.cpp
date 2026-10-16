// SneekPeekMedia window: built-in image viewer + video player. Zero chrome.
// WS_POPUP + WS_THICKFRAME (invisible: WM_NCCALCSIZE eats the frame, DWM
// extends glass over everything) + WS_EX_TOOLWINDOW (no taskbar button).
// Manual WM_NCHITTEST edge zones keep resizing working.
//
// Decoder policy: in-box OS components only (WIC for images, Media
// Foundation + D3D11 for video). No FFmpeg, no bundled codecs. All media
// DLLs are delay-loaded (MSVC): nothing maps until MediaOpen runs, so idle
// RSS stays 1-3 MB. The image path is CPU-only (WIC decode + StretchDIBits);
// video presents through its own DXGI swapchain with a D2D overlay second
// pass (GDI cannot paint over a D3D swapchain).
//
// Adaptive decode (NOT a CPU/GPU toggle): once the D3D11 device manager is
// handed to Media Foundation, MF picks hardware decode when the GPU offers
// it and falls back to software otherwise. Software decode costs 10-30x the
// CPU per frame plus surface-copy bandwidth, so there is deliberately no
// way to force it. MFStartup runs once per process on first video open and
// is never shut down; WIC/COM init per window.
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>   // SHFileOperationW (recycle-bin delete)
#include <dwmapi.h>   // MARGINS struct only (dwmapi.dll loaded dynamically)
#include <wincodec.h> // WIC image decode + scaler
#include <d3d11.h>    // video device (VIDEO_SUPPORT, enumerated adapters)
#include <dxgi.h>     // swapchain
#include <mfapi.h>    // MFStartup (process-lifetime, first video open)
#include <mfidl.h>    // MFT interface declarations
#include <mftransform.h> // MFT category GUIDs
#include <mfmediaengine.h> // IMFMediaEngine playback
#include <d2d1.h>     // control overlay (same swapchain, second pass)
#include <dwrite.h>   // overlay text
#include <oleauto.h>  // SysAllocString for SetSource
#include <new>
#include <cstdio>  // swprintf_s
#include <cstring> // memcpy
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include "media.h"
#include "files.h" // ListDirOneLevel: sibling prev/next reuses the one enumerator

namespace {

template <class T> void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

enum : UINT_PTR { IDT_LOAD = 101, IDT_PUMP, IDT_HIDE, IDT_WATCHDOG };

struct MediaCtx {
    HWND hwnd = NULL;
    HINSTANCE hInst = NULL;
    int dpi = 96;
    MediaKind kind = MediaKind::None;
    std::wstring path;
    std::wstring folder;
    bool closing = false;
    bool failed = false;
    std::wstring err;
    bool shown = false;
    bool comInit = false;
    // image state (CPU path only)
    std::vector<BYTE> dib;
    int dibW = 0, dibH = 0;       // decoded buffer size
    int imgW = 0, imgH = 0;       // native size (rotated)
    double zoom = 1.0, fitZoom = 1.0;
    double offX = 0.0, offY = 0.0; // content origin in client coords
    int turns = 0;
    bool panning = false;
    POINT panAt{};
    double panOX = 0.0, panOY = 0.0;
    // video state
    ID3D11Device* d3d = nullptr;
    ID3D11DeviceContext* dctx = nullptr;
    IDXGIFactory1* dxgi = nullptr;
    IDXGISwapChain* swap = nullptr;
    IMFDXGIDeviceManager* mgr = nullptr;
    IMFMediaEngine* engine = nullptr;
    IMFMediaEngineEx* engineEx = nullptr;
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    IDWriteTextFormat* tf = nullptr;
    ID2D1RenderTarget* overlay = nullptr;
    ID2D1SolidColorBrush* bWhite = nullptr;
    ID2D1SolidColorBrush* bDim = nullptr;
    ID2D1SolidColorBrush* bAccent = nullptr;
    ID2D1SolidColorBrush* bTrack = nullptr;
    double duration = 0.0;
    bool playing = false;
    bool sizeKnown = false; // native size arrived (late sizes self-correct)
    bool muted = false;
    double volume = 1.0;
    int rateIdx = 3; // 1.0x
    bool controls = true;
    bool scrubbing = false;
    bool wasPlaying = false;
    int lastShownSec = -1;
    double lastTime = -1.0;
    DWORD lastAdvanceTick = 0;
    // chrome state
    bool fullscreen = false;
    RECT savedRect{};
    // Safety net: windowless-failure paths delete the context without a
    // WM_DESTROY. The windowed teardown releases everything first (SafeRelease
    // nulls), so this is a no-op there. COM uninit stays explicit (ownership
    // differs per path), never here.
    ~MediaCtx() {
        SafeRelease(overlay);
        SafeRelease(bWhite);
        SafeRelease(bDim);
        SafeRelease(bAccent);
        SafeRelease(bTrack);
        SafeRelease(tf);
        SafeRelease(dw);
        SafeRelease(d2d);
        SafeRelease(engineEx);
        SafeRelease(engine);
        SafeRelease(mgr);
        SafeRelease(swap);
        SafeRelease(dxgi);
        SafeRelease(dctx);
        SafeRelease(d3d);
    }
};

MediaCtx* g_m = nullptr;
bool g_mfStarted = false;
int g_hwProbe = -1; // -1 unknown, 0 software, 1 hardware (process cache)

int DpxM(int px, int dpi) { return MulDiv(px, dpi, 96); }

// (MediaHwnd/MediaHardwareDecode/MediaDecodeLabel live at global scope
// below the namespace: media.h declares them with external linkage.)

// ---- process helpers ------------------------------------------------

bool MediaDllPresent(const wchar_t* name) {
    // Existence probe without executing entry points: keeps N-edition
    // machines (no mfplat.dll) on the graceful-failure path instead of a
    // delay-load fault.
    HMODULE m = LoadLibraryExW(name, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (m) FreeLibrary(m);
    return m != NULL;
}

void MediaExtendFrame(HWND hwnd) {
    if (HMODULE d = LoadLibraryW(L"dwmapi.dll")) {
        typedef HRESULT (WINAPI *Fn)(HWND, const MARGINS*);
        if (Fn f = (Fn)GetProcAddress(d, "DwmExtendFrameIntoClientArea")) {
            MARGINS m{-1, -1, -1, -1};
            f(hwnd, &m);
        }
        FreeLibrary(d);
    }
}

bool WorkAreaFor(HWND hwnd, RECT& out) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return false;
    out = mi.rcWork;
    return true;
}

// ---- images (WIC, CPU only) ------------------------------------------

// Decode at most maxW/maxH (aspect kept); native size used when smaller, so
// a full-size image is never materialised unless the view needs it.
bool DecodeImageAt(const std::wstring& path, int maxW, int maxH, int turns,
                   std::vector<BYTE>& bits, int& outW, int& outH) {
    bits.clear();
    outW = outH = 0;
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac))) || !fac)
        return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICBitmapFlipRotator* rot = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) && dec &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
        UINT nw = 0, nh = 0;
        if (SUCCEEDED(frame->GetSize(&nw, &nh)) && nw > 0 && nh > 0 &&
            nw <= 16384 && nh <= 16384) { // sanity: no OOM/overflow below
            double s = 1.0;
            if (maxW > 0 && maxH > 0) {
                double sx = (double)maxW / (double)nw;
                double sy = (double)maxH / (double)nh;
                s = sx < sy ? sx : sy;
                if (s > 1.0) s = 1.0; // never upscale at decode
            }
            UINT dw = (UINT)(nw * s + 0.5), dh = (UINT)(nh * s + 0.5);
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            IWICBitmapSource* src = frame;
            // Scaler failure must fall back to NATIVE dims, never to a size
            // mismatch: decoding native pixels into a smaller buffer would
            // over-read the heap and paint garbage.
            UINT sw = nw, sh = nh;
            if (SUCCEEDED(fac->CreateBitmapScaler(&scaler)) && scaler &&
                SUCCEEDED(scaler->Initialize(frame, dw, dh,
                                             WICBitmapInterpolationModeFant))) {
                src = scaler;
                sw = dw;
                sh = dh;
            }
            int tw = (int)(((turns & 1) ? sh : sw));
            int th = (int)(((turns & 1) ? sw : sh));
            if (turns & 3) {
                WICBitmapTransformOptions opt = WICBitmapTransformRotate0;
                int t = ((turns % 4) + 4) % 4;
                if (t == 1) opt = WICBitmapTransformRotate90;
                else if (t == 2) opt = WICBitmapTransformRotate180;
                else if (t == 3) opt = WICBitmapTransformRotate270;
                IWICBitmapFlipRotator* r2 = nullptr;
                if (SUCCEEDED(fac->CreateBitmapFlipRotator(&r2)) && r2 &&
                    SUCCEEDED(r2->Initialize(src, opt))) {
                    rot = r2;
                    src = rot;
                } else if (r2) {
                    r2->Release();
                }
            }
            if (SUCCEEDED(fac->CreateFormatConverter(&conv)) && conv &&
                SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppBGRA,
                                           WICBitmapDitherTypeNone, NULL, 0.0,
                                           WICBitmapPaletteTypeCustom))) {
                UINT stride = (UINT)tw * 4;
                bits.resize((size_t)stride * (size_t)th);
                if (SUCCEEDED(conv->CopyPixels(NULL, stride,
                                              (UINT)bits.size(), bits.data()))) {
                    outW = tw;
                    outH = th;
                    ok = true;
                } else {
                    bits.clear();
                }
            }
        }
    }
    SafeRelease(conv);
    SafeRelease(rot);
    SafeRelease(scaler);
    SafeRelease(frame);
    SafeRelease(dec);
    SafeRelease(fac);
    return ok;
}

// Sibling media paths in one folder, Explorer logical order. Reuses the
// single-level enumerator (files.h) — no second directory walker.
std::vector<std::wstring> MediaSiblings(const std::wstring& folder, MediaKind kind) {
    std::vector<std::wstring> out;
    std::vector<FileEntry> entries;
    size_t total = 0;
    bool trunc = false;
    if (!ListDirOneLevel(folder, entries, total, trunc)) return out;
    for (const auto& e : entries) {
        if (e.isDir) continue;
        std::wstring full = JoinFilePath(folder, e.name);
        if (ClassifyMedia(full) == kind) out.push_back(full);
    }
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return RankedNameLess(FileLower(a), FileLower(b), L"", a, b);
    });
    return out;
}

int SiblingStep(const std::vector<std::wstring>& sibs, const std::wstring& cur, int delta) {
    if (sibs.empty()) return -1;
    for (size_t i = 0; i < sibs.size(); i++) {
        if (_wcsicmp(sibs[i].c_str(), cur.c_str()) == 0) {
            int n = (int)sibs.size();
            int to = ((int)i + delta) % n;
            if (to < 0) to += n;
            return to;
        }
    }
    return -1;
}

} // namespace

// ---- open / close lifecycle (bridge implementation) -------------------
// External linkage to match media.h (main.cpp calls these). Everything
// they touch lives in this TU.

HWND MediaHwnd() { return (g_m && g_m->hwnd) ? g_m->hwnd : NULL; }

int MediaHardwareDecode() {
    // Answers "can THIS GPU decode video" by asking D3D11 itself for decoder
    // profiles, adapter by adapter (same order as playback). This deliberately
    // avoids MFTEnumEx: it reports registered MFTs, not what the active
    // adapter supports, and its profile of the hybrid-GPU case is murky.
    // Runs only on first Settings open (cached); never at boot or idle.
    if (g_hwProbe >= 0) return g_hwProbe;
    g_hwProbe = 0; // guilty until proven innocent
    if (!MediaDllPresent(L"d3d11.dll")) return g_hwProbe;
    bool uninit = false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
                                      COINIT_DISABLE_OLE1DDE);
    if (hr == S_OK) uninit = true;
    else if (hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return g_hwProbe;
    IDXGIFactory1* dxgi = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi))) && dxgi) {
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                                   D3D_FEATURE_LEVEL_11_0};
        for (UINT i = 0;; i++) {
            IDXGIAdapter1* ad = nullptr;
            if (FAILED(dxgi->EnumAdapters1(i, &ad))) break;
            ID3D11Device* dev = nullptr;
            HRESULT hr2 = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels, 2, D3D11_SDK_VERSION, &dev, NULL, NULL);
            SafeRelease(ad);
            if (SUCCEEDED(hr2) && dev) {
                ID3D11VideoDevice* vd = nullptr;
                if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&vd))) && vd) {
                    if (vd->GetVideoDecoderProfileCount() > 0)
                        g_hwProbe = 1;
                    SafeRelease(vd);
                }
                SafeRelease(dev);
                if (g_hwProbe > 0) break;
            }
        }
        SafeRelease(dxgi);
    }
    if (uninit) CoUninitialize();
    return g_hwProbe;
}

std::wstring MediaDecodeLabel() {
    int hw = MediaHardwareDecode();
    if (hw > 0) return L"Video decode: Hardware (GPU)";
    if (hw == 0) return L"Video decode: Software (CPU)";
    return L"Video decode: Unknown";
}

static void MediaFail(const std::wstring& err) {
    // Failure before first frame: never show a black window. If the window
    // exists, destroy it (WM_DESTROY runs the identical restore path);
    // otherwise report straight back to the palette.
    if (g_m && g_m->hwnd && IsWindow(g_m->hwnd)) {
        g_m->failed = true;
        g_m->err = err;
        DestroyWindow(g_m->hwnd);
        return;
    }
    std::wstring e = err;
    if (g_m) {
        // Tear down COM init owned by this open (no window was created).
        if (g_m->comInit) CoUninitialize();
        delete g_m;
        g_m = nullptr;
    }
    MediaOnClosed(true, e);
}

static bool OpenImage(const std::wstring& path);
static bool OpenVideo(const std::wstring& path);

// All-or-nothing gate: a missing media DLL must degrade to an honest
// message, never a delay-load fault (which would terminate the process).
static bool MediaStackPresent(MediaKind mk, std::wstring& err) {
    auto need = [&](const wchar_t* dll, const wchar_t* what) {
        if (MediaDllPresent(dll)) return true;
        err = std::wstring(L"Couldn't open this file (") + what +
              L" missing on this Windows build).";
        return false;
    };
    if (!need(L"windowscodecs.dll", L"image support")) return false;
    if (mk == MediaKind::Video) {
        if (!need(L"mfplat.dll", L"Media Feature Pack")) return false;
        if (!need(L"d3d11.dll", L"video device support")) return false;
        if (!need(L"dxgi.dll", L"video device support")) return false;
        if (!need(L"d2d1.dll", L"video overlay support")) return false;
        if (!need(L"dwrite.dll", L"video overlay support")) return false;
    }
    return true;
}

bool MediaOpen(HINSTANCE hInst, HWND owner, int dpi, const std::wstring& path,
               std::wstring& err) {
    (void)owner; // top-level window; kept in the signature for future use
    if (g_m) {
        err = L"A media window is already open";
        return false;
    }
    MediaKind mk = ClassifyMedia(path);
    if (mk != MediaKind::Image && mk != MediaKind::Video) {
        err = L"This format isn't supported on this Windows build. "
              L"Ctrl+Enter opens the default app.";
        return false;
    }
    if (!MediaStackPresent(mk, err)) return false;
    MediaCtx* m = new (std::nothrow) MediaCtx();
    if (!m) {
        err = L"Out of memory";
        return false;
    }
    g_m = m;
    m->hInst = hInst;
    m->dpi = dpi > 0 ? dpi : 96;
    m->kind = mk;
    m->path = path;
    size_t sep = path.find_last_of(L"\\/");
    m->folder = (sep == std::wstring::npos) ? L"" : path.substr(0, sep + 1);
    m->rateIdx = MediaSpeedDefaultIndex();
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
                                      COINIT_DISABLE_OLE1DDE);
    if (hr == S_OK) m->comInit = true;
    else if (hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        err = L"Couldn't start media playback";
        delete g_m;
        g_m = nullptr;
        return false;
    }
    bool ok = (mk == MediaKind::Image) ? OpenImage(path) : OpenVideo(path);
    if (!ok) {
        if (g_m) {
            // Windowless failure: report back without a window (a windowed
            // failure already restored via MediaOnClosed in WM_DESTROY).
            err = m->err.empty() ? L"Couldn't open this file" : m->err;
            if (m->comInit) CoUninitialize();
            delete g_m;
            g_m = nullptr;
        }
        return false;
    }
    return true;
}

// ---- image path ---------------------------------------------------------

namespace {

void ImageCenter(MediaCtx* m) {
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    int vw = cl.right - cl.left, vh = cl.bottom - cl.top;
    int cw = (int)(m->dibW * m->zoom + 0.5);
    int ch = (int)(m->dibH * m->zoom + 0.5);
    m->offX = (vw - cw) / 2.0;
    m->offY = (vh - ch) / 2.0;
    MediaClampPan(m->offX, m->offY, cw, ch, vw, vh);
}

// Decode enough pixels for the current view (never the full image unless
// the zoom genuinely needs it).
bool ImageEnsureRes(MediaCtx* m) {
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    int vw = cl.right - cl.left, vh = cl.bottom - cl.top;
    if (vw <= 0 || vh <= 0 || m->zoom <= 0.0) return true;
    int needW = (int)(vw / m->zoom + 0.5) + 2;
    int needH = (int)(vh / m->zoom + 0.5) + 2;
    if (needW <= m->dibW && needH <= m->dibH) return true;
    int boxW = needW > m->dibW ? needW : m->dibW;
    int boxH = needH > m->dibH ? needH : m->dibH;
    if (boxW > m->imgW) boxW = m->imgW;
    if (boxH > m->imgH) boxH = m->imgH;
    std::vector<BYTE> bits;
    int w = 0, h = 0;
    if (!DecodeImageAt(m->path, boxW, boxH, m->turns, bits, w, h)) return false;
    m->dib.swap(bits);
    m->dibW = w;
    m->dibH = h;
    return true;
}

void ImageZoomAt(MediaCtx* m, int cx, int cy, double factor) {
    double nz = MediaClampZoom(m->zoom * factor);
    if (nz == m->zoom) return;
    // Anchor the content point under the cursor.
    double wx = (cx - m->offX) / m->zoom;
    double wy = (cy - m->offY) / m->zoom;
    m->zoom = nz;
    if (!ImageEnsureRes(m)) return;
    m->offX = cx - wx * nz;
    m->offY = cy - wy * nz;
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    MediaClampPan(m->offX, m->offY, (int)(m->dibW * nz + 0.5),
                  (int)(m->dibH * nz + 0.5), cl.right - cl.left, cl.bottom - cl.top);
    InvalidateRect(m->hwnd, NULL, FALSE);
}

void ImagePaint(MediaCtx* m, HDC dc) {
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(dc, &cl, bg);
    if (m->dib.empty() || m->dibW <= 0 || m->dibH <= 0 || m->zoom <= 0.0) return;
    // Buffer/size mismatch (should be impossible): skip the blit rather than
    // painting garbage.
    if (m->dib.size() < (size_t)m->dibW * (size_t)m->dibH * 4) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m->dibW;
    bmi.bmiHeader.biHeight = -m->dibH; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    int dw = (int)(m->dibW * m->zoom + 0.5);
    int dh = (int)(m->dibH * m->zoom + 0.5);
    int dx = (int)(m->offX + 0.5), dy = (int)(m->offY + 0.5);
    SetStretchBltMode(dc, HALFTONE);
    if (StretchDIBits(dc, dx, dy, dw, dh, 0, 0, m->dibW, m->dibH,
                      m->dib.data(), &bmi, DIB_RGB_COLORS, SRCCOPY) == 0) {
        // GDI refused the blit: an honest error beats a mystery blank window.
        // WM_DESTROY reports these via the identical restore path.
        m->failed = true;
        m->err = L"This image couldn't be drawn on this PC.";
        PostMessageW(m->hwnd, WM_CLOSE, 0, 0);
    }
}

// Native pixel size via header read only (GetSize parses headers, never
// decodes pixels). Lets zoom-in re-decode up to true native resolution even
// when the first decode was work-area clamped.
bool ImageNativeSize(const std::wstring& path, int& w, int& h) {
    w = h = 0;
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&fac))) || !fac)
        return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    if (SUCCEEDED(fac->CreateDecoderFromFilename(path.c_str(), NULL, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) &&
        dec && SUCCEEDED(dec->GetFrame(0, &frame)) && frame) {
        UINT nw = 0, nh = 0;
        if (SUCCEEDED(frame->GetSize(&nw, &nh)) && nw > 0 && nh > 0 &&
            nw < 100000 && nh < 100000) {
            w = (int)nw;
            h = (int)nh;
            ok = true;
        }
    }
    SafeRelease(frame);
    SafeRelease(dec);
    SafeRelease(fac);
    return ok;
}

bool LoadImagePath(MediaCtx* m, const std::wstring& path) {
    RECT area{0, 0, 1024, 768};
    WorkAreaFor(m->hwnd, area);
    int aw = area.right - area.left, ah = area.bottom - area.top;
    std::vector<BYTE> bits;
    int w = 0, h = 0;
    if (!DecodeImageAt(path, aw, ah, 0, bits, w, h) || w <= 0 || h <= 0)
        return false;
    m->path = path;
    size_t sep = path.find_last_of(L"\\/");
    m->folder = (sep == std::wstring::npos) ? L"" : path.substr(0, sep + 1);
    m->dib.swap(bits);
    m->dibW = w;
    m->dibH = h;
    int nw = 0, nh = 0; // true native size: zoom-in may exceed this decode
    if (ImageNativeSize(path, nw, nh)) { m->imgW = nw; m->imgH = nh; }
    else { m->imgW = w; m->imgH = h; }
    m->turns = 0;
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    m->fitZoom = MediaFitScale(w, h, cl.right - cl.left, cl.bottom - cl.top);
    m->zoom = m->fitZoom;
    ImageCenter(m);
    return true;
}

} // namespace

static bool OpenImage(const std::wstring& path) {
    MediaCtx* m = g_m;
    RECT area{0, 0, 1024, 768};
    WorkAreaFor(m->hwnd, area); // hwnd NULL: primary monitor work area
    int aw = area.right - area.left, ah = area.bottom - area.top;
    std::vector<BYTE> bits;
    int w = 0, h = 0;
    if (!DecodeImageAt(path, aw, ah, 0, bits, w, h) || w <= 0 || h <= 0) {
        m->err = L"This image couldn't be decoded on this Windows build.";
        return false;
    }
    int ww = 0, wh = 0;
    // Initial window covers ~70% of the work area, centered (never
    // edge-to-edge); small images still open at native 1:1.
    MediaFitWindow(w, h, (aw * 7) / 10, (ah * 7) / 10, ww, wh);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMediaClass, L"SneekPeek",
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
                                (area.left + area.right - ww) / 2,
                                (area.top + area.bottom - wh) / 2, ww, wh,
                                NULL, NULL, m->hInst, NULL);
    if (!hwnd) {
        m->err = L"Couldn't open this file";
        return false;
    }
    m->hwnd = hwnd;
    m->dib.swap(bits);
    m->dibW = w;
    m->dibH = h;
    int nw = 0, nh = 0; // true native size (see LoadImagePath)
    if (ImageNativeSize(path, nw, nh)) { m->imgW = nw; m->imgH = nh; }
    else { m->imgW = w; m->imgH = h; }
    m->turns = 0;
    RECT cl{};
    GetClientRect(hwnd, &cl);
    m->fitZoom = MediaFitScale(w, h, cl.right - cl.left, cl.bottom - cl.top);
    m->zoom = m->fitZoom;
    ImageCenter(m);
    MediaExtendFrame(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd); // force the first paint now: never show an empty frame
    m->shown = true;
    return true;
}

// ---- video path (Media Foundation + D3D11) ------------------------------

namespace {

void VideoRevokeSwap(MediaCtx* m) {
    // Outstanding surface refs must be gone before ResizeBuffers: surfaces
    // are acquired per present and released right after, so nothing to do
    // here beyond the overlay target (bound to the old buffers).
    SafeRelease(m->overlay);
    SafeRelease(m->bWhite);
    SafeRelease(m->bDim);
    SafeRelease(m->bAccent);
    SafeRelease(m->bTrack);
}

bool VideoCreateSwap(MediaCtx* m) {
    if (!m->dxgi || !m->d3d || !m->hwnd) return false;
    SafeRelease(m->swap);
    VideoRevokeSwap(m);
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    int w = cl.right - cl.left, h = cl.bottom - cl.top;
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = (UINT)w;
    sd.BufferDesc.Height = (UINT)h;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m->hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    return SUCCEEDED(m->dxgi->CreateSwapChain(m->d3d, &sd, &m->swap)) && m->swap;
}

// Letterboxed destination rect preserving the video aspect.
RECT VideoDstRect(MediaCtx* m, int vw, int vh) {
    RECT r{0, 0, vw, vh};
    DWORD nw = 0, nh = 0;
    if (m->engine && SUCCEEDED(m->engine->GetNativeVideoSize(&nw, &nh)) &&
        nw > 0 && nh > 0 && vw > 0 && vh > 0) {
        double s = (double)vw / (double)nw;
        double t = (double)vh / (double)nh;
        double z = s < t ? s : t;
        int dw = (int)(nw * z + 0.5), dh = (int)(nh * z + 0.5);
        r.left = (vw - dw) / 2;
        r.top = (vh - dh) / 2;
        r.right = r.left + dw;
        r.bottom = r.top + dh;
    }
    return r;
}

bool VideoEnsureOverlay(MediaCtx* m) {
    if (m->overlay) return true;
    if (!m->d2d || !m->dw || !m->swap) return false;
    IDXGISurface* surf = nullptr;
    if (FAILED(m->swap->GetBuffer(0, IID_PPV_ARGS(&surf))) || !surf) return false;
    bool ok = false;
    D2D1_RENDER_TARGET_PROPERTIES props{};
    props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    if (SUCCEEDED(m->d2d->CreateDxgiSurfaceRenderTarget(surf, &props, &m->overlay)) &&
        m->overlay) {
        D2D1_COLOR_F white{1.0f, 1.0f, 1.0f, 1.0f};
        D2D1_COLOR_F dim{1.0f, 1.0f, 1.0f, 0.55f};
        D2D1_COLOR_F accent{0.38f, 0.65f, 0.98f, 1.0f};
        D2D1_COLOR_F track{1.0f, 1.0f, 1.0f, 0.22f};
        ok = SUCCEEDED(m->overlay->CreateSolidColorBrush(white, &m->bWhite)) &&
             SUCCEEDED(m->overlay->CreateSolidColorBrush(dim, &m->bDim)) &&
             SUCCEEDED(m->overlay->CreateSolidColorBrush(accent, &m->bAccent)) &&
             SUCCEEDED(m->overlay->CreateSolidColorBrush(track, &m->bTrack));
        if (!m->tf) {
            m->dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    13.0f, L"", &m->tf);
        }
        if (!ok) VideoRevokeSwap(m);
    }
    SafeRelease(surf);
    return ok && m->overlay;
}

void VideoDrawControls(MediaCtx* m) {
    if (!VideoEnsureOverlay(m)) return;
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    int W = cl.right - cl.left, H = cl.bottom - cl.top;
    if (W <= 0 || H <= 0) return;
    m->overlay->BeginDraw();
    D2D1_COLOR_F band{0.0f, 0.0f, 0.0f, 0.55f};
    ID2D1SolidColorBrush* bBand = nullptr;
    m->overlay->CreateSolidColorBrush(band, &bBand);
    float top = (float)(H - 64);
    if (bBand) {
        D2D1_RECT_F br{0.0f, top, (float)W, (float)H};
        m->overlay->FillRectangle(&br, bBand);
        bBand->Release();
    }
    double cur = m->engine ? m->engine->GetCurrentTime() : 0.0;
    double frac = 0.0;
    if (m->duration > 0.0 && cur >= 0.0) {
        frac = cur / m->duration;
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
    }
    float x0 = 16.0f, x1 = (float)W - 16.0f;
    float by = (float)H - 40.0f;
    D2D1_RECT_F track{x0, by - 3.0f, x1, by + 3.0f};
    m->overlay->FillRectangle(&track, m->bTrack);
    D2D1_RECT_F fill{x0, by - 3.0f, x0 + (float)(frac * (x1 - x0)), by + 3.0f};
    if (fill.right > fill.left) m->overlay->FillRectangle(&fill, m->bAccent);
    D2D1_ELLIPSE knob{{x0 + (float)(frac * (x1 - x0)), by}, 5.0f, 5.0f};
    m->overlay->FillEllipse(&knob, m->bWhite);
    if (m->tf) {
        std::wstring left = MediaFormatSpeed(kMediaSpeeds[m->rateIdx]) + L"  " +
                            MediaVolumeLabel(m->muted, m->volume);
        D2D1_RECT_F lr{x0, (float)H - 26.0f, x0 + 220.0f, (float)H - 6.0f};
        m->overlay->DrawText(left.c_str(), (UINT32)left.size(), m->tf, &lr, m->bDim);
        std::wstring right = MediaFormatTimePair(cur < 0.0 ? 0.0 : cur,
                                                m->duration < 0.0 ? 0.0 : m->duration);
        D2D1_RECT_F rr{x1 - 220.0f, (float)H - 26.0f, x1, (float)H - 6.0f};
        m->overlay->DrawText(right.c_str(), (UINT32)right.size(), m->tf, &rr, m->bDim);
    }
    m->overlay->EndDraw();
}

// One presented frame: video blit, then the D2D control pass, then flip.
// Overlay surfaces are acquired per present so ResizeBuffers is always safe.
bool VideoPresent(MediaCtx* m, bool withOverlay) {
    if (!m->swap || !m->engine) return false;
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    int W = cl.right - cl.left, H = cl.bottom - cl.top;
    if (W <= 0 || H <= 0) return true;
    IDXGISurface* surf = nullptr;
    if (FAILED(m->swap->GetBuffer(0, IID_PPV_ARGS(&surf))) || !surf) return false;
    RECT dst = VideoDstRect(m, W, H);
    MFARGB black{255, 0, 0, 0}; // MFARGB channels are bytes, not floats
    HRESULT hr = m->engine->TransferVideoFrame(surf, NULL, &dst, &black);
    SafeRelease(surf);
    if (FAILED(hr)) return false;
    if (withOverlay && m->controls) VideoDrawControls(m);
    return SUCCEEDED(m->swap->Present(1, 0));
}

void VideoShowControls(MediaCtx* m, bool show) {
    if (m->controls == show) {
        if (show) {
            KillTimer(m->hwnd, IDT_HIDE);
            if (m->playing && !m->scrubbing)
                SetTimer(m->hwnd, IDT_HIDE, 2500, NULL);
        }
        return;
    }
    m->controls = show;
    if (show) {
        KillTimer(m->hwnd, IDT_HIDE);
        if (m->playing && !m->scrubbing) SetTimer(m->hwnd, IDT_HIDE, 2500, NULL);
        m->lastShownSec = -1; // force label refresh
        VideoPresent(m, true);
    } else {
        KillTimer(m->hwnd, IDT_HIDE);
        VideoPresent(m, false);
    }
}

void VideoSetPlaying(MediaCtx* m, bool play) {
    if (!m->engine || m->playing == play) return;
    if (play) {
        if (FAILED(m->engine->Play())) return;
        m->playing = true;
        m->lastTime = -1.0;
        m->lastAdvanceTick = GetTickCount();
        SetTimer(m->hwnd, IDT_PUMP, 16, NULL);
        VideoShowControls(m, true);
    } else {
        m->engine->Pause();
        m->playing = false;
        KillTimer(m->hwnd, IDT_PUMP);
        VideoShowControls(m, true); // forced visible while paused
        VideoPresent(m, true);
    }
}

// Poll the engine: errors, duration, first frame, end-of-stream, stalls.
// Deterministic for local files; no reliance on callback plumbing.
// 100 ms loader: duration, readiness, first frame. Runs only until shown;
// the watchdog bounds the whole wait, so a stall can never hang silently.
void VideoPollLoad(MediaCtx* m) {
    if (!m->engine || m->closing || m->shown) return;
    if (m->duration <= 0.0) {
        double d = m->engine->GetDuration();
        if (d > 0.0) m->duration = d;
    }
    // Ready-state values mirror HTML (HAVE_CURRENT_DATA = 2); the watchdog
    // bounds any stall regardless.
    int rs = (int)m->engine->GetReadyState();
    if (rs < 2) return; // still loading
    DWORD nw = 0, nh = 0;
    bool sized = SUCCEEDED(m->engine->GetNativeVideoSize(&nw, &nh)) &&
                 nw > 0 && nh > 0;
    RECT area{0, 0, 960, 540};
    WorkAreaFor(m->hwnd, area);
    int aw = area.right - area.left, ah = area.bottom - area.top;
    // Initial window covers ~70% of the work area (never edge-to-edge).
    int ww = 960, wh = 540;
    if (sized) {
        MediaFitWindow((int)nw, (int)nh, (aw * 7) / 10, (ah * 7) / 10, ww, wh);
        m->sizeKnown = true;
    } else {
        // Size unknown yet: show at a default size and play anyway. The
        // letterbox rect corrects itself once the size arrives (no resize,
        // no failure just for reporting late).
        if (ww > aw) ww = aw;
        if (wh > ah) wh = ah;
    }
    if (!VideoCreateSwap(m)) {
        MediaFail(L"This video couldn't be played on this Windows build.");
        return;
    }
    SetWindowPos(m->hwnd, HWND_TOP,
                 (area.left + area.right - ww) / 2,
                 (area.top + area.bottom - wh) / 2, ww, wh,
                 SWP_SHOWWINDOW);
    SetForegroundWindow(m->hwnd);
    m->shown = true;
    KillTimer(m->hwnd, IDT_WATCHDOG);
    VideoSetPlaying(m, true);
    if (!m->playing)
        MediaFail(L"This video couldn't be started on this Windows build.");
}

// 16 ms pump while playing: present EVERY tick (the old code only presented
// on second changes, freezing the picture), plus end-of-stream and stall
// bookkeeping. Pausing kills this timer; nothing runs while paused.
void VideoPumpFrame(MediaCtx* m) {
    if (!m->engine || m->closing || !m->playing) return;
    if (!m->sizeKnown) {
        DWORD nw = 0, nh = 0; // late arrival: dst rect self-corrects, no resize
        if (SUCCEEDED(m->engine->GetNativeVideoSize(&nw, &nh)) && nw > 0 && nh > 0)
            m->sizeKnown = true;
    }
    VideoPresent(m, m->controls);
    double cur = m->engine->GetCurrentTime();
    if (m->duration > 0.0 && cur >= m->duration - 0.05) {
        // End of stream: hold the last frame, show controls.
        m->engine->Pause();
        m->playing = false;
        KillTimer(m->hwnd, IDT_PUMP);
        VideoShowControls(m, true);
        VideoPresent(m, true);
        return;
    }
    DWORD now = GetTickCount();
    if (m->lastTime >= 0.0 && cur <= m->lastTime + 1e-3 && cur < m->duration - 0.5) {
        if (now - m->lastAdvanceTick > 3000) {
            MediaFail(L"Video playback stopped progressing.");
            return;
        }
    } else {
        m->lastTime = cur;
        m->lastAdvanceTick = now;
    }
    int sec = (int)cur;
    if (sec != m->lastShownSec) m->lastShownSec = sec; // labels read live
}

void VideoSeekTo(MediaCtx* m, double t) {
    if (!m->engine || m->duration <= 0.0) return;
    if (t < 0.0) t = 0.0;
    if (t > m->duration) t = m->duration;
    if (FAILED(m->engine->SetCurrentTime(t))) return;
    m->lastTime = -1.0;
    m->lastAdvanceTick = GetTickCount();
    VideoPresent(m, true);
}

void VideoFrameStep(MediaCtx* m, bool fwd) {
    if (!m->engine) return;
    if (m->playing) VideoSetPlaying(m, false);
    if (m->engineEx) {
        if (SUCCEEDED(m->engineEx->FrameStep(fwd ? TRUE : FALSE))) {
            VideoPresent(m, true);
            return;
        }
    }
    double cur = m->engine->GetCurrentTime(); // fallback: ~one frame at 30 fps
    VideoSeekTo(m, cur + (fwd ? 1.0 / 30.0 : -1.0 / 30.0));
}

void VideoSetRate(MediaCtx* m, int idx) {
    idx = MediaSpeedClamp(idx);
    if (!m->engine || idx == m->rateIdx) return;
    if (SUCCEEDED(m->engine->SetPlaybackRate(kMediaSpeeds[idx]))) {
        m->rateIdx = idx;
        m->lastShownSec = -1;
        if (m->controls) VideoPresent(m, true);
    }
}

void VideoSetVolume(MediaCtx* m, double v, bool mute) {
    if (!m->engine) return;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    m->engine->SetVolume(v);
    m->engine->SetMuted(mute ? TRUE : FALSE);
    m->volume = v;
    m->muted = mute;
    m->lastShownSec = -1;
    if (m->controls) VideoPresent(m, true);
    else VideoShowControls(m, true);
}

} // namespace

static bool OpenVideo(const std::wstring& path) {
    MediaCtx* m = g_m;
    // Hybrid-GPU safe: first adapter that supports video wins. Never assume
    // adapter 0 (on battery Windows may park the discrete GPU; MF then uses
    // Intel QuickSync — no special handling needed).
    IDXGIFactory1* dxgi = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxgi))) || !dxgi) {
        m->err = L"Couldn't start video playback on this PC.";
        return false;
    }
    m->dxgi = dxgi;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                               D3D_FEATURE_LEVEL_11_0};
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* ad = nullptr;
        if (FAILED(dxgi->EnumAdapters1(i, &ad))) break;
        HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       levels, 2, D3D11_SDK_VERSION,
                                       &dev, NULL, &ctx);
        SafeRelease(ad);
        if (SUCCEEDED(hr) && dev) break;
        SafeRelease(dev);
        SafeRelease(ctx);
    }
    if (!dev) {
        m->err = L"Couldn't start video playback on this PC (no video-capable GPU).";
        return false;
    }
    m->d3d = dev;
    m->dctx = ctx;
    IMFDXGIDeviceManager* mgr = nullptr;
    UINT token = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&token, &mgr)) || !mgr) {
        m->err = L"Couldn't start video playback on this PC.";
        return false;
    }
    m->mgr = mgr;
    if (FAILED(mgr->ResetDevice(dev, token))) {
        m->err = L"Couldn't start video playback on this PC.";
        return false;
    }
    if (!g_mfStarted) {
        // Process-lifetime MF: started on first video open (never at boot),
        // never shut down (reopen cost would return for zero benefit).
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
            m->err = L"Couldn't start video playback on this Windows build.";
            return false;
        }
        g_mfStarted = true;
    }
    IMFAttributes* attr = nullptr;
    IMFMediaEngineClassFactory* fac = nullptr;
    IMFMediaEngine* eng = nullptr;
    bool ok = false;
    if (SUCCEEDED(MFCreateAttributes(&attr, 2)) && attr) {
        attr->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, mgr);
        attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                        DXGI_FORMAT_B8G8R8A8_UNORM);
        if (SUCCEEDED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, NULL,
                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
            fac &&
            SUCCEEDED(fac->CreateInstance(0, attr, &eng)) && eng) {
            m->engine = eng; // ownership transferred
            eng->QueryInterface(IID_PPV_ARGS(&m->engineEx)); // FrameStep; may stay NULL
            ok = true;
        }
    }
    SafeRelease(fac);
    SafeRelease(attr);
    if (!ok) {
        m->err = L"This video couldn't be played on this Windows build "
                 L"(missing codec — try Ctrl+Enter).";
        return false;
    }
    // D2D/DWrite factories for the control overlay (created with the video
    // path only, so images never map these DLLs).
    // Explicit-IID factory form (no options struct needed).
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory), (void**)&m->d2d)) ||
        !m->d2d) {
        m->err = L"Couldn't start video playback on this PC.";
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)&m->dw)) ||
        !m->dw) {
        m->err = L"Couldn't start video playback on this PC.";
        return false;
    }
    if (FAILED(m->dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &m->tf)) ||
        !m->tf) {
        m->err = L"Couldn't start video playback on this PC.";
        return false;
    }
    // Hidden window first: it only ever appears with a frame ready, so a
    // failure can never leave a black window behind.
    RECT area{0, 0, 960, 540};
    WorkAreaFor(NULL, area);
    int ww = 960, wh = 540;
    if (ww > area.right - area.left) ww = area.right - area.left;
    if (wh > area.bottom - area.top) wh = area.bottom - area.top;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMediaClass, L"SneekPeek",
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
                                (area.left + area.right - ww) / 2,
                                (area.top + area.bottom - wh) / 2, ww, wh,
                                NULL, NULL, m->hInst, NULL);
    if (!hwnd) {
        m->err = L"Couldn't open this file";
        return false;
    }
    m->hwnd = hwnd;
    MediaExtendFrame(hwnd);
    BSTR url = SysAllocString(path.c_str());
    HRESULT hr = url ? m->engine->SetSource(url) : E_OUTOFMEMORY;
    if (url) SysFreeString(url);
    if (FAILED(hr)) {
        // Window exists but nothing was shown: destroy it — WM_DESTROY runs
        // the identical restore path, so MediaOpen just reports failure.
        m->err = L"This video couldn't be played on this Windows build "
                 L"(missing codec — try Ctrl+Enter).";
        DestroyWindow(hwnd);
        return false;
    }
    m->volume = 1.0;
    m->engine->SetVolume(1.0);
    m->engine->SetMuted(FALSE);
    SetTimer(hwnd, IDT_LOAD, 100, NULL);    // state poll until first frame
    SetTimer(hwnd, IDT_WATCHDOG, 8000, NULL); // bounds stalls before first frame
    return true;
}

// ---- window procedure -----------------------------------------------------

namespace {

void MediaRequestClose() {
    if (!g_m || g_m->closing) return;
    g_m->closing = true;
    if (g_m->hwnd && IsWindow(g_m->hwnd)) {
        KillTimer(g_m->hwnd, IDT_LOAD);
        KillTimer(g_m->hwnd, IDT_PUMP);
        KillTimer(g_m->hwnd, IDT_HIDE);
        KillTimer(g_m->hwnd, IDT_WATCHDOG);
        DestroyWindow(g_m->hwnd);
    }
}

void MediaToggleFullscreen(MediaCtx* m) {
    HMONITOR mon = MonitorFromWindow(m->hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    if (!m->fullscreen) {
        GetWindowRect(m->hwnd, &m->savedRect);
        m->fullscreen = true;
        SetWindowPos(m->hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_SHOWWINDOW);
    } else {
        m->fullscreen = false;
        SetWindowPos(m->hwnd, HWND_TOP, m->savedRect.left, m->savedRect.top,
                     m->savedRect.right - m->savedRect.left,
                     m->savedRect.bottom - m->savedRect.top, SWP_SHOWWINDOW);
    }
}

bool MediaActivateLost(HWND hwnd, LPARAM l) {
    // Owned popups (the delete confirm) keep activation in-family: only an
    // outside window closes the viewer.
    HWND to = (HWND)l;
    if (to && (to == hwnd || IsChild(hwnd, to) || GetWindow(to, GW_OWNER) == hwnd))
        return false;
    return true;
}

bool MediaLoadPath(MediaCtx* m, const std::wstring& path) {
    // Sibling hopping reuses the live window (no destroy/recreate flicker).
    KillTimer(m->hwnd, IDT_LOAD);
    KillTimer(m->hwnd, IDT_PUMP);
    KillTimer(m->hwnd, IDT_WATCHDOG);
    if (m->kind == MediaKind::Image) {
        m->turns = 0;
        if (!LoadImagePath(m, path)) {
            MediaFail(L"This image couldn't be decoded on this Windows build.");
            return false;
        }
        InvalidateRect(m->hwnd, NULL, FALSE);
        return true;
    }
    if (m->engine) {
        m->engine->Pause();
        m->playing = false;
        m->duration = 0.0;
        m->lastTime = -1.0;
        m->lastShownSec = -1;
        BSTR url = SysAllocString(path.c_str());
        HRESULT hr = url ? m->engine->SetSource(url) : E_OUTOFMEMORY;
        if (url) SysFreeString(url);
        if (FAILED(hr)) {
            MediaFail(L"This video couldn't be played on this Windows build "
                      L"(missing codec — try Ctrl+Enter).");
            return false;
        }
        m->path = path;
        size_t sep = path.find_last_of(L"\\/");
        m->folder = (sep == std::wstring::npos) ? L"" : path.substr(0, sep + 1);
        m->shown = false;
        ShowWindow(m->hwnd, SW_HIDE);
        SetTimer(m->hwnd, IDT_LOAD, 100, NULL);
        SetTimer(m->hwnd, IDT_WATCHDOG, 8000, NULL);
        return true;
    }
    return false;
}

int MediaHop(MediaCtx* m, int delta) {
    auto sibs = MediaSiblings(m->folder, m->kind);
    int to = SiblingStep(sibs, m->path, delta);
    if (to >= 0) MediaLoadPath(m, sibs[(size_t)to]);
    return to;
}

void MediaDeleteCurrent(MediaCtx* m) {
    if (m->kind != MediaKind::Image || m->path.empty()) return;
    wchar_t msg[512];
    const wchar_t* name = m->path.c_str();
    size_t sep = m->path.find_last_of(L"\\/");
    if (sep != std::wstring::npos) name = m->path.c_str() + sep + 1;
    swprintf_s(msg, 512, L"Move '%s' to the Recycle Bin?", name);
    if (MessageBoxW(m->hwnd, msg, L"SneekPeek", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    std::vector<wchar_t> from(m->path.size() + 2, L'\0');
    memcpy(from.data(), m->path.c_str(), m->path.size() * sizeof(wchar_t));
    SHFILEOPSTRUCTW fo{};
    fo.wFunc = FO_DELETE;
    fo.pFrom = from.data();
    fo.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperationW(&fo) != 0 || fo.fAnyOperationsAborted) return;
    // The palette refresh on restore drops the row naturally (matched by
    // action, now absent). Advance first so something stays on screen.
    if (MediaHop(m, +1) < 0) MediaRequestClose();
}

bool MediaScrubZone(MediaCtx* m, int y, RECT& bar) {
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    bar.left = 16;
    bar.right = cl.right - cl.left - 16;
    bar.top = cl.bottom - cl.top - 48;
    bar.bottom = cl.bottom - cl.top - 24;
    POINT p{0, y};
    return PtInRect(&bar, p);
}

void MediaScrubTo(MediaCtx* m, int x) {
    RECT bar{};
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    bar.left = 16;
    bar.right = cl.right - cl.left - 16;
    if (bar.right <= bar.left || m->duration <= 0.0) return;
    double frac = (double)(x - bar.left) / (double)(bar.right - bar.left);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    VideoSeekTo(m, frac * m->duration);
}

} // namespace

LRESULT CALLBACK MediaWndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    MediaCtx* mc = g_m;
    if (!mc || mc->hwnd != hwnd) return DefWindowProcW(hwnd, m, w, l);
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) return 0; // frameless: erase the entire non-client area
        return 0;
    case WM_NCHITTEST: {
        // Invisible-but-functional resize border (8px zones).
        RECT wr;
        GetWindowRect(hwnd, &wr);
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        const int z = 8;
        bool L = x < wr.left + z, R = x >= wr.right - z;
        bool T = y < wr.top + z, B = y >= wr.bottom - z;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT;
        if (R) return HTRIGHT;
        if (T) return HTTOP;
        if (B) return HTBOTTOM;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize.x = DpxM(320, mc->dpi);
        mm->ptMinTrackSize.y = DpxM(200, mc->dpi);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // every pixel painted in WM_PAINT / Present
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (mc->kind == MediaKind::Image) ImagePaint(mc, dc);
        else {
            RECT cl{};
            GetClientRect(hwnd, &cl);
            HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(dc, &cl, bg); // pre-first-frame: black, never garbage
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (mc->kind == MediaKind::Image) {
            MediaClampPan(mc->offX, mc->offY, (int)(mc->dibW * mc->zoom + 0.5),
                          (int)(mc->dibH * mc->zoom + 0.5),
                          LOWORD(l), HIWORD(l));
            ImageEnsureRes(mc); // grown window may deserve more pixels
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (mc->swap && LOWORD(l) > 0 && HIWORD(l) > 0) {
            VideoRevokeSwap(mc);
            mc->swap->ResizeBuffers(0, LOWORD(l), HIWORD(l),
                                    DXGI_FORMAT_UNKNOWN, 0);
            VideoPresent(mc, mc->controls);
        }
        return 0;
    case WM_DPICHANGED:
        mc->dpi = HIWORD(w);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_KEYDOWN:
        if (mc->kind == MediaKind::Image) {
            switch (w) {
            case VK_ESCAPE: MediaRequestClose(); return 0;
            case VK_LEFT:
            case VK_RIGHT: {
                int d = (w == VK_RIGHT) ? +1 : -1;
                if (MediaHop(mc, d) < 0) MediaRequestClose();
                return 0;
            }
            case VK_ADD:
            case VK_OEM_PLUS: {
                RECT cl{};
                GetClientRect(hwnd, &cl);
                ImageZoomAt(mc, (cl.right - cl.left) / 2, (cl.bottom - cl.top) / 2, 1.25);
                return 0;
            }
            case VK_SUBTRACT:
            case VK_OEM_MINUS: {
                RECT cl{};
                GetClientRect(hwnd, &cl);
                ImageZoomAt(mc, (cl.right - cl.left) / 2, (cl.bottom - cl.top) / 2, 0.8);
                return 0;
            }
            case '0':
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                    mc->zoom = mc->fitZoom;
                    ImageCenter(mc);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                break;
            case '1':
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                    mc->zoom = 1.0;
                    ImageEnsureRes(mc);
                    ImageCenter(mc);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                break;
            case 'R': {
                mc->turns = (mc->turns + 1) % 4;
                std::vector<BYTE> bits;
                int tw = 0, th = 0;
                RECT cl{};
                GetClientRect(hwnd, &cl);
                int bw = cl.right - cl.left, bh = cl.bottom - cl.top;
                if (DecodeImageAt(mc->path, bw > 0 ? bw : 64, bh > 0 ? bh : 64,
                                  mc->turns, bits, tw, th) &&
                    tw > 0 && th > 0) {
                    mc->dib.swap(bits);
                    mc->dibW = tw;
                    mc->dibH = th;
                    mc->fitZoom = MediaFitScale(tw, th, bw, bh);
                    mc->zoom = mc->fitZoom;
                    ImageCenter(mc);
                    InvalidateRect(hwnd, NULL, FALSE);
                } else {
                    mc->turns = (mc->turns + 3) % 4; // rotate failed: revert
                }
                return 0;
            }
            case 'F': MediaToggleFullscreen(mc); return 0;
            case VK_DELETE: MediaDeleteCurrent(mc); return 0;
            case VK_RETURN: {
                std::wstring p = mc->path;
                if ((UINT_PTR)ShellExecuteW(NULL, L"open", p.c_str(), NULL, NULL,
                                            SW_SHOWNORMAL) > 32)
                    MediaRequestClose(); // default app takes it from here
                return 0;
            }
            }
        } else {
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            switch (w) {
            case VK_ESCAPE: MediaRequestClose(); return 0; // closes immediately
            case VK_SPACE: VideoSetPlaying(mc, !mc->playing); return 0;
            case VK_LEFT:
            case VK_RIGHT: {
                int dir = (w == VK_RIGHT) ? +1 : -1;
                if (shift) {
                    VideoSetRate(mc, mc->rateIdx + dir);
                } else if (!mc->playing) {
                    VideoFrameStep(mc, dir > 0);
                } else {
                    double cur = mc->engine ? mc->engine->GetCurrentTime() : 0.0;
                    VideoSeekTo(mc, cur + dir * 5.0);
                }
                VideoShowControls(mc, true);
                return 0;
            }
            case VK_PRIOR:
                if (MediaHop(mc, -1) < 0) MediaRequestClose();
                return 0;
            case VK_NEXT:
                if (MediaHop(mc, +1) < 0) MediaRequestClose();
                return 0;
            case 'M':
                VideoSetVolume(mc, mc->volume, !mc->muted);
                VideoShowControls(mc, true);
                return 0;
            case VK_UP:
                VideoSetVolume(mc, mc->volume + 0.1, false);
                VideoShowControls(mc, true);
                return 0;
            case VK_DOWN:
                VideoSetVolume(mc, mc->volume - 0.1, false);
                VideoShowControls(mc, true);
                return 0;
            case 'F': MediaToggleFullscreen(mc); return 0;
            case VK_RETURN: {
                std::wstring p = mc->path;
                if ((UINT_PTR)ShellExecuteW(NULL, L"open", p.c_str(), NULL, NULL,
                                            SW_SHOWNORMAL) > 32)
                    MediaRequestClose(); // default app takes it from here
                return 0;
            }
            }
            VideoShowControls(mc, true); // any other key wakes the controls
        }
        break;
    case WM_LBUTTONDOWN: {
        if (mc->kind == MediaKind::Image) {
            RECT cl{};
            GetClientRect(hwnd, &cl);
            int cw = (int)(mc->dibW * mc->zoom + 0.5);
            int ch = (int)(mc->dibH * mc->zoom + 0.5);
            if (cw > cl.right - cl.left || ch > cl.bottom - cl.top) {
                mc->panning = true;
                mc->panAt.x = GET_X_LPARAM(l);
                mc->panAt.y = GET_Y_LPARAM(l);
                mc->panOX = mc->offX;
                mc->panOY = mc->offY;
                SetCapture(hwnd);
            }
        } else {
            // Clicking the bar seeks (drag = scrub); plain clicks do nothing
            // so double-click fullscreen never toggles playback by accident.
            RECT bar{};
            if (MediaScrubZone(mc, GET_Y_LPARAM(l), bar)) {
                mc->scrubbing = true;
                mc->wasPlaying = mc->playing;
                if (mc->playing) VideoSetPlaying(mc, false);
                SetCapture(hwnd);
                MediaScrubTo(mc, GET_X_LPARAM(l));
                VideoShowControls(mc, true);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (mc->panning) {
            mc->panning = false;
            ReleaseCapture();
        }
        if (mc->scrubbing) {
            mc->scrubbing = false;
            ReleaseCapture();
            if (mc->wasPlaying) VideoSetPlaying(mc, true);
            VideoShowControls(mc, true);
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (mc->kind == MediaKind::Video) {
            MediaToggleFullscreen(mc);
        } else {
            mc->zoom = mc->fitZoom; // double-click: back to fit
            ImageCenter(mc);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (mc->panning) {
            mc->offX = mc->panOX + (GET_X_LPARAM(l) - mc->panAt.x);
            mc->offY = mc->panOY + (GET_Y_LPARAM(l) - mc->panAt.y);
            RECT cl{};
            GetClientRect(hwnd, &cl);
            MediaClampPan(mc->offX, mc->offY,
                          (int)(mc->dibW * mc->zoom + 0.5),
                          (int)(mc->dibH * mc->zoom + 0.5),
                          cl.right - cl.left, cl.bottom - cl.top);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (mc->scrubbing) {
            MediaScrubTo(mc, GET_X_LPARAM(l));
        } else if (mc->kind == MediaKind::Video) {
            VideoShowControls(mc, true); // wakes + restarts the hide timer
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (mc->kind == MediaKind::Image) {
            int d = GET_WHEEL_DELTA_WPARAM(w);
            double f = (d != 0) ? pow(1.15, (double)d / (double)WHEEL_DELTA) : 1.0;
            POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(hwnd, &pt);
            ImageZoomAt(mc, pt.x, pt.y, f);
        } else {
            int d = GET_WHEEL_DELTA_WPARAM(w);
            if (d != 0) VideoSetVolume(mc, mc->volume + (d > 0 ? 0.1 : -0.1), false);
            VideoShowControls(mc, true);
        }
        return 0;
    case WM_TIMER:
        if (w == IDT_LOAD) {
            VideoPollLoad(mc); // pre-play polling only; safe post-close
            return 0;
        }
        if (w == IDT_PUMP) {
            VideoPumpFrame(mc); // present every tick while playing
            return 0;
        }
        if (w == IDT_HIDE) {
            KillTimer(hwnd, IDT_HIDE);
            if (mc->playing && !mc->scrubbing) VideoShowControls(mc, false);
            return 0;
        }
        if (w == IDT_WATCHDOG) {
            KillTimer(hwnd, IDT_WATCHDOG);
            if (!mc->shown)
                MediaFail(L"This video is taking too long to start "
                          L"(missing codec?).");
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && MediaActivateLost(hwnd, l))
            MediaRequestClose(); // click outside closes (owned dialogs exempt)
        break;
    case WM_ACTIVATEAPP:
        if (!w) MediaRequestClose();
        break;
    case WM_CLOSE:
        MediaRequestClose();
        return 0;
    case WM_DESTROY: {
        KillTimer(hwnd, IDT_LOAD);
        KillTimer(hwnd, IDT_PUMP);
        KillTimer(hwnd, IDT_HIDE);
        KillTimer(hwnd, IDT_WATCHDOG);
        SafeRelease(mc->overlay);
        SafeRelease(mc->bWhite);
        SafeRelease(mc->bDim);
        SafeRelease(mc->bAccent);
        SafeRelease(mc->bTrack);
        SafeRelease(mc->tf);
        SafeRelease(mc->dw);
        SafeRelease(mc->d2d);
        SafeRelease(mc->engineEx);
        SafeRelease(mc->engine);
        SafeRelease(mc->mgr);
        SafeRelease(mc->swap);
        SafeRelease(mc->dxgi);
        SafeRelease(mc->dctx);
        SafeRelease(mc->d3d);
        if (mc->comInit) CoUninitialize();
        bool failed = mc->failed;
        std::wstring err = mc->err;
        delete mc;
        g_m = nullptr;
        MediaOnClosed(failed, err); // identical path for every close reason
        return 0;
    }
    }
    return DefWindowProcW(hwnd, m, w, l);
}
