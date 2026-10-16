// SneekPeekMedia window: built-in image viewer + video player. Zero chrome.
// WS_POPUP + WS_THICKFRAME (invisible: WM_NCCALCSIZE eats the frame, DWM
// extends glass over everything) + WS_EX_TOOLWINDOW (no taskbar button).
// Manual WM_NCHITTEST edge zones keep resizing working.
//
// Decoder policy: in-box OS components plus ONE vendored header (stb_image,
// public domain, for photos). No FFmpeg, no bundled codecs, no shipped DLLs.
// Images decode via stb_image from memory (Unicode-safe file read, RGBA,
// swizzled once to BGRA for GDI). Video plays through Media Foundation:
// Source Resolver -> partial topology (auto-inserted decoders) -> EVR video
// + audio renderer, driven by deterministic session events. Hardware decode
// engages automatically where the GPU offers it; there is deliberately no
// CPU/GPU toggle (software decode costs 10-30x the CPU per frame). The
// control bar is a plain GDI child window in the palette's visual language,
// not a composited overlay. MFStartup runs once per process on first video
// open and is never shut down; nothing media-related maps before first use,
// so idle RSS stays 1-3 MB.
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h> // SHFileOperationW (recycle-bin delete)
#include <dwmapi.h> // MARGINS struct only (dwmapi.dll loaded dynamically)
#include <mfapi.h>  // MFStartup, session, MFGetService
#include <mfidl.h>  // session/topology interfaces
#include <evr.h>    // video + audio renderer activates
#include <new>
#include <cstdio>  // swprintf_s
#include <cstring> // memcpy
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include "media.h"
#include "files.h" // ListDirOneLevel: sibling prev/next reuses the one enumerator

#ifdef _MSC_VER
__pragma(warning(push, 0))
#endif
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#ifdef _MSC_VER
__pragma(warning(pop))
#endif

namespace {

template <class T> void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

enum : UINT_PTR { IDT_UI = 101, IDT_HIDE, IDT_WATCHDOG };
enum : UINT { WM_APP_SES = WM_APP + 32 }; // session event arrived
static const int kBarH = 64;              // control bar height (video only)
static const wchar_t* kMediaBarClass = L"SneekPeekMediaBar";

struct MediaCtx {
    HWND hwnd = NULL;
    HWND hBar = NULL;
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
    // image state (stb RGBA buffer, swizzled once to BGRA for GDI)
    unsigned char* px = nullptr;
    int pxW = 0, pxH = 0; // buffer dims == native dims (full decode)
    int imgW = 0, imgH = 0;
    double zoom = 1.0, fitZoom = 1.0;
    double offX = 0.0, offY = 0.0; // content origin in client coords
    int turns = 0;
    bool panning = false;
    POINT panAt{};
    double panOX = 0.0, panOY = 0.0;
    // video state (session)
    IMFMediaSession* session = nullptr;
    IMFMediaSource* source = nullptr;
    IMFPresentationDescriptor* pd = nullptr;
    IMFTopology* topo = nullptr;
    IMFPresentationClock* clock = nullptr;
    IMFRateControl* rate = nullptr;
    IMFSimpleAudioVolume* audio = nullptr;
    SessionEvents* events = nullptr;
    double duration = 0.0; // seconds, 0 = unknown
    double segPos = 0.0;  // content position at segClock
    LONGLONG segClock = 0;
    bool playing = false;
    bool muted = false;
    double volume = 1.0;
    int rateIdx = 3; // 1.0x
    bool controls = true;
    bool scrubbing = false;
    bool wasPlaying = false;
    int lastShownSec = -1;
    double lastTime = -1.0;
    DWORD lastAdvanceTick = 0;
    HFONT barFont = NULL;
    // chrome state
    bool fullscreen = false;
    RECT savedRect{};
    // Safety net: windowless-failure paths delete the context without a
    // WM_DESTROY. The windowed teardown releases everything first
    // (SafeRelease nulls), so this is a no-op there. COM uninit stays
    // explicit (ownership differs per path), never here.
    ~MediaCtx() {
        if (px) {
            stbi_image_free(px);
            px = nullptr;
        }
        if (barFont) {
            DeleteObject(barFont);
            barFont = NULL;
        }
        SafeRelease(events);
        SafeRelease(audio);
        SafeRelease(rate);
        SafeRelease(clock);
        SafeRelease(topo);
        SafeRelease(pd);
        SafeRelease(source);
        SafeRelease(session);
    }
};

MediaCtx* g_m = nullptr;
bool g_mfStarted = false;
int g_hwProbe = -1; // -1 unknown, 0 software, 1 hardware (process cache)

int DpxM(int px, int dpi) { return MulDiv(px, dpi, 96); }

bool MediaDllPresent(const wchar_t* name) {
    // Existence probe without executing entry points: a missing media DLL
    // degrades to an honest message, never a delay-load fault.
    HMODULE m = LoadLibraryExW(name, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (m) FreeLibrary(m);
    return m != NULL;
}

} // namespace

// ---- bridge implementations (external linkage to match media.h) ---------

HWND MediaHwnd() { return (g_m && g_m->hwnd) ? g_m->hwnd : NULL; }

int MediaHardwareDecode() {
    // Answers "can THIS GPU decode video" via D3D11 decoder profiles,
    // adapter by adapter. Runs only on first Settings open (cached); never
    // at boot or idle. Fully dynamic (GetProcAddress): no D3D import library
    // is linked, so nothing maps until this runs.
    if (g_hwProbe >= 0) return g_hwProbe;
    g_hwProbe = 0; // guilty until proven innocent
    if (!MediaDllPresent(L"d3d11.dll") || !MediaDllPresent(L"dxgi.dll"))
        return g_hwProbe;
    bool uninit = false;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
                                      COINIT_DISABLE_OLE1DDE);
    if (hr == S_OK) uninit = true;
    else if (hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return g_hwProbe;
    HMODULE dxgiLib = LoadLibraryW(L"dxgi.dll");
    HMODULE d3dLib = LoadLibraryW(L"d3d11.dll");
    typedef HRESULT (WINAPI *CreateDxgiFn)(REFIID, void**);
    typedef HRESULT (WINAPI *CreateDevFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE,
        UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
        D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    CreateDxgiFn pCreateDxgi = dxgiLib
        ? (CreateDxgiFn)GetProcAddress(dxgiLib, "CreateDXGIFactory1") : nullptr;
    CreateDevFn pCreateDev = d3dLib
        ? (CreateDevFn)GetProcAddress(d3dLib, "D3D11CreateDevice") : nullptr;
    if (pCreateDxgi && pCreateDev) {
        IDXGIFactory1* dxgi = nullptr;
        if (SUCCEEDED(pCreateDxgi(IID_IDXGIFactory1, (void**)&dxgi)) && dxgi) {
            static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                                       D3D_FEATURE_LEVEL_11_0};
            for (UINT i = 0;; i++) {
                IDXGIAdapter1* ad = nullptr;
                if (FAILED(dxgi->EnumAdapters1(i, &ad))) break;
                ID3D11Device* dev = nullptr;
                HRESULT hr2 = pCreateDev(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL,
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
    }
    if (dxgiLib) FreeLibrary(dxgiLib);
    if (d3dLib) FreeLibrary(d3dLib);
    if (uninit) CoUninitialize();
    return g_hwProbe;
}

std::wstring MediaDecodeLabel() {
    int hw = MediaHardwareDecode();
    if (hw > 0) return L"Video decode: Hardware (GPU)";
    if (hw == 0) return L"Video decode: Software (CPU)";
    return L"Video decode: Unknown";
}

namespace {

// ---- shared helpers -----------------------------------------------------

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

// ---- images (stb_image, CPU/GDI only) -----------------------------------
// File bytes are read with CreateFileW (Unicode-safe; stb has no W variant)
// and decoded from memory. stb has no scaler-at-decode: the full frame is
// materialised transiently (~72 MB for 24 MP, released on close/navigate),
// which the old WIC path avoided — documented tradeoff for zero new DLLs.

bool ReadFileBytes(const std::wstring& path, std::vector<BYTE>& out) {
    out.clear();
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = false;
    if (GetFileSizeEx(hf, &sz) && sz.QuadPart > 0 &&
        sz.QuadPart <= (LONGLONG)256 * 1024 * 1024) {
        out.resize((size_t)sz.QuadPart);
        DWORD done = 0, total = 0;
        ok = true;
        while (total < out.size()) {
            DWORD chunk = (DWORD)((out.size() - total) > 32 * 1024 * 1024
                                      ? 32 * 1024 * 1024
                                      : out.size() - total);
            if (!ReadFile(hf, out.data() + total, chunk, &done, NULL) || done == 0) {
                ok = false;
                break;
            }
            total += done;
        }
        if (total != out.size()) ok = false;
    }
    CloseHandle(hf);
    if (!ok) out.clear();
    return ok;
}

// In-place RGBA -> BGRA swizzle (GDI DIBs are BGRA; alpha byte ignored).
void SwizzleRB(unsigned char* px, int w, int h) {
    for (int i = 0, n = w * h; i < n; i++, px += 4) {
        unsigned char t = px[0];
        px[0] = px[2];
        px[2] = t;
    }
}

// Decode + swizzle. Returns the buffer (caller frees with stbi_image_free).
unsigned char* DecodeImage(const std::wstring& path, int& w, int& h) {
    w = h = 0;
    std::vector<BYTE> file;
    if (!ReadFileBytes(path, file)) return nullptr;
    int comp = 0;
    unsigned char* px = stbi_load_from_memory(file.data(), (int)file.size(),
                                              &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        if (px) stbi_image_free(px);
        return nullptr;
    }
    SwizzleRB(px, w, h);
    return px;
}

// 90-degree clockwise transpose of an RGBA/BGRA buffer (rotation uses it).
unsigned char* Rotate90CW(const unsigned char* src, int w, int h) {
    if (!src || w <= 0 || h <= 0) return nullptr;
    unsigned char* dst = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
    if (!dst) return nullptr;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            memcpy(dst + ((size_t)x * (size_t)h + (size_t)(h - 1 - y)) * 4,
                   src + ((size_t)y * (size_t)w + (size_t)x) * 4, 4);
    return dst;
}

void ImageCenter(MediaCtx* m) {
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    int vw = cl.right - cl.left, vh = cl.bottom - cl.top;
    int cw = (int)(m->pxW * m->zoom + 0.5);
    int ch = (int)(m->pxH * m->zoom + 0.5);
    m->offX = (vw - cw) / 2.0;
    m->offY = (vh - ch) / 2.0;
    MediaClampPan(m->offX, m->offY, cw, ch, vw, vh);
}

void ImageZoomAt(MediaCtx* m, int cx, int cy, double factor) {
    double nz = MediaClampZoom(m->zoom * factor);
    if (nz == m->zoom) return;
    // Anchor the content point under the cursor.
    double wx = (cx - m->offX) / m->zoom;
    double wy = (cy - m->offY) / m->zoom;
    m->zoom = nz;
    m->offX = cx - wx * nz;
    m->offY = cy - wy * nz;
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    MediaClampPan(m->offX, m->offY, (int)(m->pxW * nz + 0.5),
                  (int)(m->pxH * nz + 0.5), cl.right - cl.left, cl.bottom - cl.top);
    InvalidateRect(m->hwnd, NULL, FALSE);
}

void ImagePaint(MediaCtx* m, HDC dc) {
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(dc, &cl, bg);
    if (!m->px || m->pxW <= 0 || m->pxH <= 0 || m->zoom <= 0.0) return;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m->pxW;
    bmi.bmiHeader.biHeight = -m->pxH; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    int dw = (int)(m->pxW * m->zoom + 0.5);
    int dh = (int)(m->pxH * m->zoom + 0.5);
    int dx = (int)(m->offX + 0.5), dy = (int)(m->offY + 0.5);
    SetStretchBltMode(dc, HALFTONE);
    if (StretchDIBits(dc, dx, dy, dw, dh, 0, 0, m->pxW, m->pxH,
                      m->px, &bmi, DIB_RGB_COLORS, SRCCOPY) == 0) {
        // GDI refused the blit: an honest error beats a mystery blank window.
        // WM_DESTROY reports these via the identical restore path.
        m->failed = true;
        m->err = L"This image couldn't be drawn on this PC.";
        PostMessageW(m->hwnd, WM_CLOSE, 0, 0);
    }
}

// (Re)load path into the live window. Resets the view; keeps the window.
bool LoadImagePath(MediaCtx* m, const std::wstring& path) {
    int w = 0, h = 0;
    unsigned char* px = DecodeImage(path, w, h);
    if (!px) return false;
    if (m->px) stbi_image_free(m->px);
    m->px = px;
    m->pxW = m->imgW = w;
    m->pxH = m->imgH = h;
    m->path = path;
    size_t sep = path.find_last_of(L"\\/");
    m->folder = (sep == std::wstring::npos) ? L"" : path.substr(0, sep + 1);
    m->turns = 0;
    RECT cl{};
    GetClientRect(m->hwnd, &cl);
    m->fitZoom = MediaFitScale(w, h, cl.right - cl.left, cl.bottom - cl.top);
    m->zoom = m->fitZoom;
    ImageCenter(m);
    return true;
}

} // namespace

// ---- open / close lifecycle (bridge implementation) -------------------

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
        if (g_m->comInit) CoUninitialize();
        delete g_m;
        g_m = nullptr;
    }
    MediaOnClosed(true, e);
}

static bool OpenImage(const std::wstring& path);
static bool OpenVideo(const std::wstring& path);

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

namespace {

// All-or-nothing gate: a missing media DLL must degrade to an honest
// message, never a delay-load fault (which would terminate the process).
// stb_image needs no DLL at all; video needs the MF + EVR stack.
bool MediaStackPresent(MediaKind mk, std::wstring& err) {
    auto need = [&](const wchar_t* dll, const wchar_t* what) {
        if (MediaDllPresent(dll)) return true;
        err = std::wstring(L"Couldn't open this file (") + what +
              L" missing on this Windows build).";
        return false;
    };
    if (mk == MediaKind::Video) {
        if (!need(L"mfplat.dll", L"Media Feature Pack")) return false;
        if (!need(L"evr.dll", L"video renderer")) return false;
    }
    return true;
}

static bool OpenImage(const std::wstring& path) {
    MediaCtx* m = g_m;
    RECT area{0, 0, 1024, 768};
    WorkAreaFor(m->hwnd, area); // hwnd NULL: primary monitor work area
    int aw = area.right - area.left, ah = area.bottom - area.top;
    // Initial window covers ~70% of the work area, centered (never
    // edge-to-edge); small images still open at native 1:1.
    int ww = 0, wh = 0;
    int w = 0, h = 0;
    unsigned char* px = DecodeImage(path, w, h);
    if (!px) {
        m->err = L"This image couldn't be decoded on this PC.";
        return false;
    }
    MediaFitWindow(w, h, (aw * 7) / 10, (ah * 7) / 10, ww, wh);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMediaClass, L"SneekPeek",
                                WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
                                (area.left + area.right - ww) / 2,
                                (area.top + area.bottom - wh) / 2, ww, wh,
                                NULL, NULL, m->hInst, NULL);
    if (!hwnd) {
        stbi_image_free(px);
        m->err = L"Couldn't open this file";
        return false;
    }
    m->hwnd = hwnd;
    m->px = px;
    m->pxW = m->imgW = w;
    m->pxH = m->imgH = h;
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

// ---- video path (Media Foundation session + EVR) -----------------------
// Source Resolver -> partial topology (decoder auto-inserted, hardware when
// offered) -> EVR video + audio renderer, driven by session events. No
// swapchain, no D3D device management, no overlay surfaces: EVR presents
// itself and letterboxes internally; controls are a GDI child window.

class SessionEvents : public IMFAsyncCallback {
    LONG m_ref = 1;
    IMFMediaSession* m_ses = nullptr;
    HWND m_hwnd = NULL;
    bool m_dead = false;
public:
    SessionEvents(IMFMediaSession* ses, HWND hwnd) : m_hwnd(hwnd) {
        m_ses = ses;
        if (m_ses) m_ses->AddRef();
    }
    void Detach() {
        m_dead = true;
        m_hwnd = NULL;
        SafeRelease(m_ses);
    }
    void SetHwnd(HWND h) { m_hwnd = h; }
    STDMETHODIMP QueryInterface(REFIID riid, void** pp) {
        if (!pp) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IMFAsyncCallback) {
            *pp = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *pp = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* r) {
        MediaEventType t = MEUnknown;
        HRESULT edata = S_OK;
        if (r && !m_dead && m_ses) {
            IMFMediaEvent* e = nullptr;
            if (SUCCEEDED(m_ses->EndGetEvent(r, &e)) && e) {
                e->GetType(&t);
                if (t == MESessionTopologyStatus) {
                    UINT32 st = 0;
                    if (SUCCEEDED(e->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &st)))
                        edata = (HRESULT)st;
                } else if (t == MEError) {
                    e->GetStatus(&edata);
                }
                SafeRelease(e);
            }
        }
        if (!m_dead && m_hwnd) PostMessageW(m_hwnd, WM_APP_SES, (WPARAM)t, (LPARAM)edata);
        if (!m_dead && m_ses) m_ses->BeginGetEvent(this, NULL);
        return S_OK;
    }
};

bool VideoBuildTopology(MediaCtx* m, IMFMediaSource* src) {
    IMFPresentationDescriptor* pd = nullptr;
    if (FAILED(src->CreatePresentationDescriptor(&pd)) || !pd) return false;
    m->pd = pd; // owned from here (released in teardown)
    DWORD streams = 0;
    if (FAILED(pd->GetStreamDescriptorCount(&streams))) return false;
    IMFTopology* topo = nullptr;
    if (FAILED(MFCreateTopology(&topo)) || !topo) return false;
    m->topo = topo;
    for (DWORD i = 0; i < streams; i++) {
        IMFStreamDescriptor* sd = nullptr;
        BOOL selected = FALSE;
        if (FAILED(pd->GetStreamDescriptorByIndex(i, &selected, &sd)) || !sd) continue;
        IMFMediaTypeHandler* handler = nullptr;
        GUID major = GUID_NULL;
        if (SUCCEEDED(sd->GetMediaTypeHandler(&handler)) && handler)
            handler->GetMajorType(&major);
        SafeRelease(handler);
        DWORD sid = 0;
        sd->GetStreamIdentifier(&sid);
        IMFActivate* act = nullptr;
        bool want = false;
        if (major == MFMediaType_Video) {
            want = SUCCEEDED(MFCreateVideoRendererActivate(m->hwnd, &act)) && act;
            pd->SelectStream(sid);
        } else if (major == MFMediaType_Audio) {
            want = SUCCEEDED(MFCreateAudioRendererActivate(&act)) && act;
            pd->SelectStream(sid);
        } else {
            pd->DeselectStream(sid); // subtitles, scripts: not rendered
        }
        if (!want) {
            SafeRelease(sd);
            continue;
        }
        IMFTopologyNode* srcNode = nullptr;
        IMFTopologyNode* outNode = nullptr;
        bool linked = false;
        if (SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &srcNode)) &&
            srcNode &&
            SUCCEEDED(MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &outNode)) &&
            outNode) {
            srcNode->SetUnknown(MF_TOPONODE_SOURCE, (IUnknown*)m->source);
            srcNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, (IUnknown*)pd);
            srcNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, (IUnknown*)sd);
            outNode->SetObject(act);
            if (SUCCEEDED(topo->AddNode(srcNode)) && SUCCEEDED(topo->AddNode(outNode)) &&
                SUCCEEDED(srcNode->ConnectOutput(0, outNode, 0)))
                linked = true;
        }
        SafeRelease(srcNode);
        SafeRelease(outNode);
        SafeRelease(act);
        SafeRelease(sd);
        if (!linked) return false;
    }
    // Duration for the scrub bar (absent on some files: UI degrades to 0:00).
    UINT64 dur100ns = 0;
    if (SUCCEEDED(pd->GetUINT64(MF_PD_DURATION, &dur100ns)) && dur100ns > 0)
        m->duration = (double)dur100ns / 10000000.0;
    return true;
}

void VideoTrackClock(MediaCtx* m) {
    // Snapshot the clock base for position(). Called after every Start.
    SafeRelease(m->clock);
    if (m->session) m->session->GetClock(&m->clock);
    if (m->clock) {
        LONGLONG t = 0;
        if (SUCCEEDED(m->clock->GetTime(&t))) m->segClock = t;
    }
}

double VideoPosition(MediaCtx* m) {
    if (!m->clock || !m->playing) return m->segPos; // frozen while paused
    LONGLONG t = 0;
    if (FAILED(m->clock->GetTime(&t))) return m->segPos;
    double p = m->segPos + (double)(t - m->segClock) / 10000000.0;
    if (p < 0.0) p = 0.0;
    if (m->duration > 0.0 && p > m->duration) p = m->duration;
    return p;
}

bool VideoEnsureRate(MediaCtx* m) {
    if (m->rate || !m->session) return m->rate != nullptr;
    IMFRateControl* rc = nullptr;
    if (SUCCEEDED(MFGetService(m->session, MF_RATE_CONTROL_SERVICE,
                               IID_PPV_ARGS(&rc))) &&
        rc) {
        m->rate = rc;
        return true;
    }
    SafeRelease(rc);
    return false;
}

bool VideoEnsureAudio(MediaCtx* m) {
    if (m->audio || !m->session) return m->audio != nullptr;
    IMFSimpleAudioVolume* av = nullptr;
    if (SUCCEEDED(MFGetService(m->session, MR_POLICY_VOLUME_SERVICE,
                               IID_PPV_ARGS(&av))) &&
        av) {
        m->audio = av;
        return true;
    }
    SafeRelease(av);
    return false;
}

void VideoUpdateBar(MediaCtx* m) {
    if (m->hBar) InvalidateRect(m->hBar, NULL, FALSE);
}

void VideoSetPlaying(MediaCtx* m, bool play) {
    if (!m->session || m->playing == play) return;
    if (play) {
        if (FAILED(m->session->Start(&GUID_NULL, NULL))) return;
        m->playing = true;
        VideoTrackClock(m);
        m->lastTime = -1.0;
        m->lastAdvanceTick = GetTickCount();
        SetTimer(m->hwnd, IDT_UI, 100, NULL);
        VideoShowBar(m, true);
    } else {
        m->segPos = VideoPosition(m); // freeze where we are
        m->session->Pause();
        m->playing = false;
        KillTimer(m->hwnd, IDT_UI);
        VideoShowBar(m, true); // forced visible while paused
    }
}

void VideoSeekTo(MediaCtx* m, double t, bool resume) {
    if (!m->session) return;
    if (t < 0.0) t = 0.0;
    if (m->duration > 0.0 && t > m->duration) t = m->duration;
    PROPVARIANT var{};
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(t * 10000000.0);
    HRESULT hr = m->session->Start(&GUID_NULL, &var);
    PropVariantClear(&var);
    if (FAILED(hr)) return;
    m->segPos = t;
    VideoTrackClock(m);
    m->lastTime = -1.0;
    m->lastAdvanceTick = GetTickCount();
    if (resume && !m->playing) {
        m->playing = true; // Start() is already rolling: mirror the state
        SetTimer(m->hwnd, IDT_UI, 100, NULL);
    } else if (!resume && m->playing) {
        // Paused seek: start-then-pause lands exactly on the frame.
        m->session->Pause();
        m->playing = false;
        KillTimer(m->hwnd, IDT_UI);
        m->segPos = t;
        VideoTrackClock(m);
    }
    m->lastShownSec = -1;
    VideoUpdateBar(m);
}

void VideoFrameStep(MediaCtx* m, bool fwd) {
    if (!m->session) return;
    if (m->playing) VideoSetPlaying(m, false);
    VideoSeekTo(m, VideoPosition(m) + (fwd ? 1.0 / 30.0 : -1.0 / 30.0), false);
}

void VideoSetRate(MediaCtx* m, int idx) {
    idx = MediaSpeedClamp(idx);
    if (idx == m->rateIdx) return;
    if (VideoEnsureRate(m) && SUCCEEDED(m->rate->SetRate(FALSE, (float)kMediaSpeeds[idx]))) {
        m->rateIdx = idx;
        VideoTrackClock(m); // the clock now advances at the new rate
        m->lastShownSec = -1;
        VideoUpdateBar(m);
    }
}

void VideoSetVolume(MediaCtx* m, double v, bool mute) {
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    if (VideoEnsureAudio(m)) {
        m->audio->SetMasterVolume((float)v);
        m->audio->SetMute(mute ? TRUE : FALSE);
    }
    m->volume = v;
    m->muted = mute;
    m->lastShownSec = -1;
    VideoUpdateBar(m);
    if (!m->controls) VideoShowBar(m, true);
}

void VideoShowBar(MediaCtx* m, bool show) {
    if (!m->hBar) return;
    if (m->controls == show) {
        if (show) {
            KillTimer(m->hwnd, IDT_HIDE);
            if (m->playing && !m->scrubbing)
                SetTimer(m->hwnd, IDT_HIDE, 2500, NULL);
        }
        return;
    }
    m->controls = show;
    ShowWindow(m->hBar, show ? SW_SHOW : SW_HIDE);
    if (show) {
        KillTimer(m->hwnd, IDT_HIDE);
        if (m->playing && !m->scrubbing) SetTimer(m->hwnd, IDT_HIDE, 2500, NULL);
        m->lastShownSec = -1;
        VideoUpdateBar(m);
    } else {
        KillTimer(m->hwnd, IDT_HIDE);
    }
}

// 100 ms UI tick while playing: clock labels, end-of-stream, stalls.
// Nothing runs while paused: the frame simply holds.
void VideoTick(MediaCtx* m) {
    if (!m->session || m->closing || !m->playing) return;
    double cur = VideoPosition(m);
    if (m->duration > 0.0 && cur >= m->duration - 0.05) {
        // End of stream: hold the last frame, show the bar.
        m->session->Pause();
        m->playing = false;
        KillTimer(m->hwnd, IDT_UI);
        m->segPos = m->duration;
        VideoShowBar(m, true);
        VideoUpdateBar(m);
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
    if (sec != m->lastShownSec) {
        m->lastShownSec = sec;
        VideoUpdateBar(m);
    }
}

void VideoTeardown(MediaCtx* m) {
    // Stop events first so no callback can fire mid-teardown.
    if (m->events) {
        m->events->Detach();
        m->events->Release();
        m->events = nullptr;
    }
    if (m->session) {
        m->session->CloseSession();
        m->session->Shutdown();
    }
    SafeRelease(m->rate);
    SafeRelease(m->audio);
    SafeRelease(m->clock);
    SafeRelease(m->topo);
    SafeRelease(m->pd);
    SafeRelease(m->source);
    SafeRelease(m->session);
    m->playing = false;
    m->duration = 0.0;
    m->segPos = 0.0;
    KillTimer(m->hwnd, IDT_UI);
    KillTimer(m->hwnd, IDT_HIDE);
    KillTimer(m->hwnd, IDT_WATCHDOG);
}

bool VideoLoad(MediaCtx* m, const std::wstring& path) {
    VideoTeardown(m);
    m->path = path;
    size_t sep = path.find_last_of(L"\\/");
    m->folder = (sep == std::wstring::npos) ? L"" : path.substr(0, sep + 1);
    m->rateIdx = MediaSpeedDefaultIndex();
    m->muted = false;
    m->volume = 1.0;
    m->controls = true;
    m->scrubbing = false;
    m->lastShownSec = -1;
    m->lastTime = -1.0;
    IMFSourceResolver* resolver = nullptr;
    if (FAILED(MFCreateSourceResolver(&resolver)) || !resolver) {
        m->err = L"Couldn't start video playback on this PC.";
        return false;
    }
    MF_OBJECT_TYPE objType = MF_OBJECT_INVALID;
    IUnknown* unk = nullptr;
    HRESULT hr = resolver->CreateObjectFromURL(path.c_str(), MF_RESOLUTION_MEDIASOURCE,
                                               NULL, &objType, &unk);
    SafeRelease(resolver);
    if (FAILED(hr) || !unk || objType != MF_OBJECT_MEDIASOURCE) {
        SafeRelease(unk);
        m->err = L"This video couldn't be opened on this Windows build "
                 L"(missing codec — try Ctrl+Enter).";
        return false;
    }
    IMFMediaSource* src = nullptr;
    hr = unk->QueryInterface(IID_PPV_ARGS(&src));
    SafeRelease(unk);
    if (FAILED(hr) || !src) {
        m->err = L"This video couldn't be opened on this Windows build "
                 L"(missing codec — try Ctrl+Enter).";
        return false;
    }
    m->source = src; // owned from here
    if (!VideoBuildTopology(m, src)) {
        m->err = L"This video has no playable streams on this Windows build.";
        return false;
    }
    // Fresh session + events per source: the teardown above shut the old
    // session down, and a shut session never plays again.
    if (FAILED(MFCreateMediaSession(NULL, &m->session)) || !m->session) {
        m->err = L"Couldn't start video playback on this PC.";
        return false;
    }
    m->events = new (std::nothrow) SessionEvents(m->session, m->hwnd);
    if (!m->events) {
        m->err = L"Out of memory";
        return false;
    }
    if (FAILED(m->session->SetTopology(0, m->topo))) {
        m->err = L"This video couldn't be played on this Windows build.";
        return false;
    }
    // Events flow from here; READY starts playback (watchdog bounds it).
    m->events->BeginGetEvent(m->events, NULL);
    SetTimer(m->hwnd, IDT_WATCHDOG, 10000, NULL);
    return true;
}

static bool OpenVideo(const std::wstring& path);
static LRESULT CALLBACK MediaBarProc(HWND hwnd, UINT m, WPARAM w, LPARAM l);
static void EnsureMediaBarClass(HINSTANCE hInst);

} // namespace

static bool OpenVideo(const std::wstring& path) {
    MediaCtx* m = g_m;
    if (!g_mfStarted) {
        // Process-lifetime MF: started on first video open (never at boot),
        // never shut down (reopen cost would return for zero benefit).
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
            m->err = L"Couldn't start video playback on this Windows build.";
            return false;
        }
        g_mfStarted = true;
    }
    // Hidden window first (session + events are created per source inside
    // VideoLoad, after the window exists to post to): a failure can never
    // leave a black window behind.
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
    EnsureMediaBarClass(m->hInst);
    // Control bar: GDI child window docked at the bottom (own proc below).
    m->hBar = CreateWindowExW(0, kMediaBarClass, L"",
                              WS_CHILD | WS_CLIPSIBLINGS,
                              0, wh - kBarH, ww, kBarH,
                              hwnd, (HMENU)7, m->hInst, NULL);
    if (m->hBar) {
        HFONT f = CreateFontW(-DpxM(13, m->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                              FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (f) {
            if (m->barFont) DeleteObject(m->barFont);
            m->barFont = f;
        }
        ShowWindow(m->hBar, SW_SHOW);
    }
    if (!VideoLoad(m, path)) {
        // Window exists but nothing was shown: destroy it — WM_DESTROY runs
        // the identical restore path, so MediaOpen just reports failure.
        DestroyWindow(hwnd);
        return false;
    }
    return true;
}

// ---- control bar (GDI child window, video only) -------------------------

namespace {

void BarLayout(MediaCtx* m) {
    if (m->hBar && IsWindow(m->hBar)) {
        RECT cl{};
        GetClientRect(m->hwnd, &cl);
        SetWindowPos(m->hBar, NULL, 0, cl.bottom - cl.top - kBarH,
                     cl.right - cl.left, kBarH, SWP_NOZORDER);
    }
}

void BarScrubTo(MediaCtx* m, int x) {
    RECT cl{};
    GetClientRect(m->hBar, &cl);
    int l = 16, r = cl.right - cl.left - 16;
    if (r <= l || m->duration <= 0.0) return;
    double frac = (double)(x - l) / (double)(r - l);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    VideoSeekTo(m, frac * m->duration, m->wasPlaying);
}

bool BarHitTest(MediaCtx* m, int y) {
    RECT cl{};
    GetClientRect(m->hBar, &cl);
    return y >= 8 && y <= 32; // progress strip at the top of the bar
}

} // namespace

static LRESULT CALLBACK MediaBarProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    MediaCtx* mc = g_m;
    if (!mc || mc->hBar != hwnd) return DefWindowProcW(hwnd, m, w, l);
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cl{};
        GetClientRect(hwnd, &cl);
        HBRUSH bg = (HBRUSH)GetStockObject(BLACK_BRUSH);
        FillRect(dc, &cl, bg);
        SetBkMode(dc, TRANSPARENT);
        if (mc->barFont) SelectObject(dc, mc->barFont);
        double frac = 0.0, cur = VideoPosition(mc);
        if (mc->duration > 0.0 && cur >= 0.0) {
            frac = cur / mc->duration;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
        }
        int x0 = 16, x1 = cl.right - cl.left - 16;
        int by = 20;
        HPEN track = CreatePen(PS_SOLID, 6, RGB(255, 255, 255));
        HGDIOBJ old = SelectObject(dc, track);
        // Track (dim) + fill (accent) as thick lines with round ends.
        MoveToEx(dc, x0, by, NULL);
        SelectObject(dc, GetStockObject(DC_PEN));
        SetDCPenColor(dc, RGB(70, 70, 74));
        LineTo(dc, x1, by);
        MoveToEx(dc, x0, by, NULL);
        SetDCPenColor(dc, RGB(96, 165, 250));
        LineTo(dc, x0 + (int)(frac * (x1 - x0)), by);
        int kx = x0 + (int)(frac * (x1 - x0));
        HBRUSH knob = CreateSolidBrush(RGB(255, 255, 255));
        HGDIOBJ ob = SelectObject(dc, knob);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, kx - 5, by - 5, kx + 5, by + 5);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(knob);
        SelectObject(dc, old);
        DeleteObject(track);
        wchar_t left[64], right[64];
        swprintf_s(left, 64, L"%s  %s", MediaFormatSpeed(kMediaSpeeds[mc->rateIdx]).c_str(),
                   MediaVolumeLabel(mc->muted, mc->volume).c_str());
        std::wstring rt = MediaFormatTimePair(cur < 0.0 ? 0.0 : cur,
                                              mc->duration < 0.0 ? 0.0 : mc->duration);
        SetTextColor(dc, RGB(150, 150, 155));
        RECT lr{x0, 34, x0 + 260, cl.bottom - 4};
        DrawTextW(dc, left, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT rr{x1 - 260, 34, x1, cl.bottom - 4};
        DrawTextW(dc, rt.c_str(), -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (mc->duration > 0.0 && BarHitTest(mc, GET_Y_LPARAM(l))) {
            mc->scrubbing = true;
            mc->wasPlaying = mc->playing;
            if (mc->playing) VideoSetPlaying(mc, false);
            SetCapture(hwnd);
            BarScrubTo(mc, GET_X_LPARAM(l));
            VideoShowBar(mc, true);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (mc->scrubbing) BarScrubTo(mc, GET_X_LPARAM(l));
        return 0;
    case WM_LBUTTONUP:
        if (mc->scrubbing) {
            mc->scrubbing = false;
            ReleaseCapture();
            if (mc->wasPlaying) VideoSetPlaying(mc, true);
            VideoShowBar(mc, true);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, m, w, l);
}

// ---- window procedure -----------------------------------------------------

namespace {

void MediaRequestClose() {
    if (!g_m || g_m->closing) return;
    g_m->closing = true;
    if (g_m->hwnd && IsWindow(g_m->hwnd)) {
        KillTimer(g_m->hwnd, IDT_UI);
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

int MediaHop(MediaCtx* m, int delta) {
    auto sibs = MediaSiblings(m->folder, m->kind);
    int to = SiblingStep(sibs, m->path, delta);
    if (to < 0) return -1;
    if (m->kind == MediaKind::Video) {
        if (!VideoLoad(m, sibs[(size_t)to])) {
            // Windowed failure already restored via WM_DESTROY; the caller
            // must not touch m afterwards (MediaRequestClose guards this).
            MediaFail(L"This video couldn't be played on this Windows build.");
        }
        return to;
    }
    if (!MediaLoadPath(m, sibs[(size_t)to])) {
        MediaFail(L"This image couldn't be decoded on this PC.");
        return to; // window gone; caller guards via MediaRequestClose
    }
    InvalidateRect(m->hwnd, NULL, FALSE);
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

void EnsureMediaBarClass(HINSTANCE hInst) {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.lpszClassName = kMediaBarClass;
    wc.lpfnWndProc = MediaBarProc;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc); // ALREADY_EXISTS is fine (single registration)
}

// Native video size for the initial window. Falls back to a default box:
// EVR letterboxes internally, so playback never depends on knowing it.
bool VideoNativeSize(MediaCtx* m, int& w, int& h) {
    w = h = 0;
    if (!m->session) return false;
    IMFVideoDisplayControl* vdc = nullptr;
    if (FAILED(MFGetService(m->session, MR_VIDEO_DISPLAY_SERVICE,
                            IID_PPV_ARGS(&vdc))) ||
        !vdc)
        return false;
    SIZE video{0, 0}, ar{0, 0};
    bool ok = SUCCEEDED(vdc->GetNativeVideoSize(&video, &ar)) &&
              video.cx > 0 && video.cy > 0;
    if (ok) {
        w = video.cx;
        h = video.cy;
    }
    SafeRelease(vdc);
    return ok;
}

// Session event dispatch (posted by SessionEvents::Invoke; all decisions
// funnel through the same graceful-failure + restore contract).
void OnSessionEvent(MediaCtx* m, MediaEventType type, LONG_PTR data) {
    if (!m || m->closing) return;
    switch (type) {
    case MESessionTopologyStatus:
        // Only READY matters; other topology statuses are normal progress,
        // never failures.
        if ((UINT32)data == MF_TOPOSTATUS_READY && !m->shown) {
            int nw = 0, nh = 0;
            RECT area{0, 0, 960, 540};
            WorkAreaFor(m->hwnd, area);
            int aw = area.right - area.left, ah = area.bottom - area.top;
            int ww = 960, wh = 540;
            if (VideoNativeSize(m, nw, nh)) {
                int bw = (aw * 7) / 10, bh = (ah * 7) / 10;
                MediaFitWindow(nw, nh, bw, bh, ww, wh);
            } else {
                if (ww > aw) ww = aw;
                if (wh > ah) wh = ah;
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
        return;
    case MESessionStarted:
        m->playing = true;
        VideoTrackClock(m);
        m->lastTime = -1.0;
        m->lastAdvanceTick = GetTickCount();
        SetTimer(m->hwnd, IDT_UI, 100, NULL);
        VideoShowBar(m, true);
        return;
    case MESessionPaused:
        m->segPos = VideoPosition(m);
        m->playing = false;
        KillTimer(m->hwnd, IDT_UI);
        VideoShowBar(m, true);
        return;
    case MESessionEnded:
        m->playing = false;
        KillTimer(m->hwnd, IDT_UI);
        m->segPos = m->duration;
        VideoShowBar(m, true);
        VideoUpdateBar(m);
        return;
    case MEError:
        MediaFail(L"This video couldn't be played on this Windows build.");
        return;
    default:
        return;
    }
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
        return 1; // every pixel painted in WM_PAINT (images) or by EVR
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (mc->kind == MediaKind::Image) {
            ImagePaint(mc, dc);
        } else {
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
            MediaClampPan(mc->offX, mc->offY, (int)(mc->pxW * mc->zoom + 0.5),
                          (int)(mc->pxH * mc->zoom + 0.5),
                          LOWORD(l), HIWORD(l));
            InvalidateRect(hwnd, NULL, FALSE);
        } else {
            BarLayout(mc); // EVR tracks the window itself; re-dock the bar
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_DPICHANGED:
        mc->dpi = HIWORD(w);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_APP_SES:
        OnSessionEvent(mc, (MediaEventType)w, (LONG_PTR)l);
        return 0;
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
                    ImageCenter(mc);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
                break;
            case 'R': {
                unsigned char* rot = Rotate90CW(mc->px, mc->pxW, mc->pxH);
                if (rot) {
                    if (mc->px) stbi_image_free(mc->px);
                    mc->px = rot;
                    int t = mc->pxW;
                    mc->pxW = mc->pxH;
                    mc->pxH = t;
                    mc->turns = (mc->turns + 1) % 4;
                    RECT cl{};
                    GetClientRect(hwnd, &cl);
                    mc->fitZoom = MediaFitScale(mc->pxW, mc->pxH,
                                               cl.right - cl.left, cl.bottom - cl.top);
                    mc->zoom = mc->fitZoom;
                    ImageCenter(mc);
                    InvalidateRect(hwnd, NULL, FALSE);
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
                    VideoSeekTo(mc, VideoPosition(mc) + dir * 5.0, true);
                }
                VideoShowBar(mc, true);
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
                VideoShowBar(mc, true);
                return 0;
            case VK_UP:
                VideoSetVolume(mc, mc->volume + 0.1, false);
                VideoShowBar(mc, true);
                return 0;
            case VK_DOWN:
                VideoSetVolume(mc, mc->volume - 0.1, false);
                VideoShowBar(mc, true);
                return 0;
            case 'F': MediaToggleFullscreen(mc); return 0;
            case VK_RETURN: {
                std::wstring p = mc->path;
                if ((UINT_PTR)ShellExecuteW(NULL, L"open", p.c_str(), NULL, NULL,
                                            SW_SHOWNORMAL) > 32)
                    MediaRequestClose();
                return 0;
            }
            }
            VideoShowBar(mc, true); // any other key wakes the controls
        }
        break;
    case WM_LBUTTONDOWN: {
        if (mc->kind == MediaKind::Image) {
            RECT cl{};
            GetClientRect(hwnd, &cl);
            int cw = (int)(mc->pxW * mc->zoom + 0.5);
            int ch = (int)(mc->pxH * mc->zoom + 0.5);
            if (cw > cl.right - cl.left || ch > cl.bottom - cl.top) {
                mc->panning = true;
                mc->panAt.x = GET_X_LPARAM(l);
                mc->panAt.y = GET_Y_LPARAM(l);
                mc->panOX = mc->offX;
                mc->panOY = mc->offY;
                SetCapture(hwnd);
            }
        }
        // Video: clicks do nothing (no accidental toggles); the scrub bar
        // handles its own hits and double-click fullscreens.
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
            VideoShowBar(mc, true);
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
                          (int)(mc->pxW * mc->zoom + 0.5),
                          (int)(mc->pxH * mc->zoom + 0.5),
                          cl.right - cl.left, cl.bottom - cl.top);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (mc->scrubbing) {
            RECT cl{};
            GetClientRect(mc->hBar, &cl);
            int l16 = 16, r16 = cl.right - cl.left - 16;
            if (r16 > l16 && mc->duration > 0.0) {
                double frac = (double)(GET_X_LPARAM(l) - l16) / (double)(r16 - l16);
                if (frac < 0.0) frac = 0.0;
                if (frac > 1.0) frac = 1.0;
                VideoSeekTo(mc, frac * mc->duration, mc->wasPlaying);
            }
        } else if (mc->kind == MediaKind::Video) {
            VideoShowBar(mc, true); // wakes + restarts the hide timer
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
            VideoShowBar(mc, true);
        }
        return 0;
    case WM_TIMER:
        if (w == IDT_UI) {
            VideoTick(mc); // clock labels, end-of-stream, stalls
            return 0;
        }
        if (w == IDT_HIDE) {
            KillTimer(hwnd, IDT_HIDE);
            if (mc->playing && !mc->scrubbing) VideoShowBar(mc, false);
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
        KillTimer(hwnd, IDT_UI);
        KillTimer(hwnd, IDT_HIDE);
        KillTimer(hwnd, IDT_WATCHDOG);
        VideoTeardown(mc);
        if (mc->barFont) {
            DeleteObject(mc->barFont);
            mc->barFont = NULL;
        }
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
