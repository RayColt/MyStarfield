#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>

// --- Linker Directives (for Visual Studio) ---
#pragma comment(lib, "d2d1.lib") 
#pragma comment(lib, "d3d11.lib") // Often needed for Direct2D context setup
#pragma comment(lib, "windowscodecs.lib") // For image loading, good practice

using namespace std;

// Constants
const float g_StarSize = 0.5f;
// Defaults
static int g_StarCount = 200;
static int g_StarSpeed = 10;
static int g_MaxStars = 1000;
static int g_MaxSpeed = 250;
static COLORREF g_CurrentStarColor = RGB(255, 255, 255); // Default Star Color!
HBRUSH g_hBrushColor = NULL;

// COM Helper (for releasing Direct2D objects) ---
template <class T> void SafeRelease(T** ppT) 
{
    if (*ppT) 
    {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

// Handle Helper (for closing Windows handles)
static void SafeCloseHandle(HANDLE& h)
{
    if (h)
    {
        CloseHandle(h);
        h = NULL;
    }
}

// --- Star Structure ---
struct Star 
{
    float x; // x-coordinate in 3D space (relative to center)
    float y; // y-coordinate in 3D space (relative to center)
    float z; // depth/distance
	float speed; // speed of the star
};

// RenderWindow
struct RenderWindow
{
    HWND hwnd = NULL;
    HDC backHdc = NULL;
    HBITMAP backBmp = NULL;
    HBITMAP oldBackBmp = NULL;
    RECT rc = {};
    vector<Star> g_stars;
    mt19937 rng;
    bool isPreview = false;

    // Direct2D per-window resources (owned by the thread that created the window)
    ID2D1HwndRenderTarget* pRT = nullptr;
    ID2D1SolidColorBrush* pBrush = nullptr;

    HANDLE hThread = NULL;        // worker thread handle
    DWORD  threadId = 0;          // thread id
    HANDLE hStopEvent = NULL;     // signal thread to stop (optional)
};

// ThreadParam
struct ThreadParam
{
    RECT rc;
    // optional: pass as owner so 
    // overlay/preview behaviors remain
    HWND ownerForParent;
    RenderWindow* rw;
    const wchar_t* className;
};

// global collection of windows/threads
static vector<RenderWindow*> g_Windows;
static HINSTANCE g_hInst = NULL;
static atomic_bool g_Running{ false };

// Input filtering
static LARGE_INTEGER g_PerfFreq;
static LARGE_INTEGER g_StartCounter;
static double g_InputDebounceSeconds = 0.66; // time to stop screensaver after mouse move
static POINT g_StartMouse = { 0,0 };
static bool g_StartMouseInit = false;
static const int g_MouseMoveThreshold = 12; // pixels

// Config / registry keys
static LPCWSTR REG_KEY = L"Software\\MyStarfieldD2d1";
static LPCWSTR REG_STARS = L"StarCount";
static LPCWSTR REG_SPEED = L"Speed";
static LPCWSTR REG_COLOR = L"Color";
static LPCWSTR REG_CCOLORS = L"Custom Colors";
static COLORREF g_CustomColors[16];

// --- Direct2D Globals and State ---
ID2D1Factory* pD2DFactory = nullptr;
ID2D1HwndRenderTarget* pRT = nullptr;
ID2D1SolidColorBrush* pWhiteBrush = nullptr;

// Registry helpers
static int GetRegDWORD(LPCWSTR name, int def)
{
    HKEY hKey;
    DWORD val = def;
    DWORD size = sizeof(val);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        RegQueryValueExW(hKey, name, NULL, NULL, (LPBYTE)&val, &size);
        RegCloseKey(hKey);
    }
    return (int)val;
}

static void SetRegDWORD(LPCWSTR name, DWORD v)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(DWORD));
        RegCloseKey(hKey);
    }
}

static void LoadSettings()
{
    g_StarCount = GetRegDWORD(REG_STARS, g_StarCount);
    g_StarSpeed = GetRegDWORD(REG_SPEED, g_StarSpeed);
    g_CurrentStarColor = GetRegDWORD(REG_COLOR, g_CurrentStarColor);
}

static void SaveSettings()
{
    SetRegDWORD(REG_STARS, (DWORD)g_StarCount);
    SetRegDWORD(REG_SPEED, (DWORD)g_StarSpeed);
    SetRegDWORD(REG_COLOR, (DWORD)g_CurrentStarColor);
}

// Pick Color Dialog custom colors load/save
static void LoadCustomColors(vector<COLORREF>& colors)
{
    colors.assign(16, RGB(255, 255, 255));
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(hKey, REG_CCOLORS, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
            type == REG_BINARY && size == sizeof(COLORREF) * 16)
        {
            RegQueryValueExW(hKey, REG_CCOLORS, nullptr, &type, reinterpret_cast<LPBYTE>(colors.data()), &size);
            RegCloseKey(hKey);
        }
    }
    RegCloseKey(hKey);
}

static void SaveCustomColors(const vector<COLORREF>& colors)
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, REG_CCOLORS, 0, REG_BINARY, reinterpret_cast<const BYTE*>(colors.data()),
            static_cast<DWORD>(sizeof(COLORREF) * colors.size()));
        RegCloseKey(hKey);
    }
    RegCloseKey(hKey);
}

// Utilities
// Simple arg parsing
static void ParseArgs(int argc, wchar_t** argv, wchar_t& modeOut, HWND& hwndOut)
{
    modeOut = 0;
    hwndOut = NULL;
    if (argc <= 1) return;
    wstring a1 = argv[1];
    if (a1.size() >= 2 && (a1[0] == L'/' || a1[0] == L'-'))
    {
        wchar_t c = towlower(a1[1]);
        modeOut = c;
        size_t colon = a1.find(L':');
        if (colon != wstring::npos)
        {
            wstring num = a1.substr(colon + 1);
            if (!num.empty()) hwndOut = (HWND)_wcstoui64(num.c_str(), nullptr, 0);
        }
        else if (argc >= 3)
        {
            wstring a2 = argv[2];
            bool numeric = !a2.empty();
            for (wchar_t ch : a2) if (!iswdigit(ch)) { numeric = false; break; }
            if (numeric) hwndOut = (HWND)_wcstoui64(a2.c_str(), nullptr, 0);
        }
    }
}

// backbuffer helpers
static void DestroyBackbuffer(RenderWindow* rw)
{
    if (!rw) return;
    if (rw->backHdc)
    {
        SelectObject(rw->backHdc, rw->oldBackBmp);
        DeleteObject(rw->backBmp);
        DeleteDC(rw->backHdc);
        rw->backHdc = NULL;
        rw->backBmp = NULL;
        rw->oldBackBmp = NULL;
    }
}

static bool CreateBackbuffer(RenderWindow* rw)
{
    if (!rw || !rw->hwnd) return false;
    HDC wnd = GetDC(rw->hwnd);
    if (!wnd) return false;
    // release existing
    DestroyBackbuffer(rw);
    int w = max(1, rw->rc.right - rw->rc.left);
    int h = max(1, rw->rc.bottom - rw->rc.top);
    HDC mem = CreateCompatibleDC(wnd);
    HBITMAP bmp = CreateCompatibleBitmap(wnd, w, h);
    if (!mem || !bmp)
    {
        if (mem) DeleteDC(mem);
        ReleaseDC(rw->hwnd, wnd);
        return false;
    }
    rw->oldBackBmp = (HBITMAP)SelectObject(mem, bmp);
    rw->backHdc = mem;
    rw->backBmp = bmp;
    // init black background
    HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT rc = { 0,0,w,h };
    FillRect(rw->backHdc, &rc, b);
    ReleaseDC(rw->hwnd, wnd);
    return true;
}

// Get top-level owner to disable instead of a child preview window
static HWND GetModalDisableTarget(HWND possibleParent)
{
    if (!possibleParent || !IsWindow(possibleParent)) return NULL;
    LONG_PTR style = GetWindowLongPtr(possibleParent, GWL_STYLE);
    if (style & WS_CHILD)
    {
        HWND top = GetAncestor(possibleParent, GA_ROOTOWNER);
        return top ? top : possibleParent;
    }
    return possibleParent;
}

/**
 * @brief Initializes stars with random positions based on current window size.
 */
void InitializeStars(RenderWindow* rw) 
{
    if (!rw) return;
    RECT r = rw->rc;
    int width = max(1, r.right - r.left);
    int height = max(1, r.bottom - r.top);
    rw->g_stars.clear();
    rw->g_stars.resize(g_StarCount);
    uniform_real_distribution<float> ud01(0.0f, 1.0f);
    for (int i = 0; i < g_StarCount; ++i)
    {
        float fx = ud01(rw->rng);
        float fy = ud01(rw->rng);
        float fz = ud01(rw->rng);
       // rw->g_stars[i].x = fx / RAND_MAX * width - width / 2.0f;
		//rw->g_stars[i].y = fy / RAND_MAX * height - height / 2.0f;
		//rw->g_stars[i].z = fz / RAND_MAX * width;
		//rw->g_stars[i].speed = g_StarSpeed + ud01(rw->rng) * g_StarSpeed; // Vary speed slightly

        rw->g_stars[i].x = (fx - 0.5f) * (float)width * 2.0f;
        rw->g_stars[i].y = (fy - 0.5f) * (float)height * 2.0f;
        rw->g_stars[i].z = fz * (float)width; // depth in screen space
        rw->g_stars[i].speed = (float)g_StarSpeed + ud01(rw->rng) * (float)g_StarSpeed; // Vary speed slightly
    
    }
}

/**
 * @brief Creates Direct2D factory, render target, and brushes.
 */
HRESULT CreateD2DResources(RenderWindow* rw)
{
    if (!rw) return E_POINTER;
    if (rw->pRT) return S_OK; // already created for this window
    if (!pD2DFactory) return E_FAIL; // factory must exist

    D2D1_SIZE_U size = D2D1::SizeU(max(1, rw->rc.right - rw->rc.left), max(1, rw->rc.bottom - rw->rc.top));
    HRESULT hr = pD2DFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(rw->hwnd, size),
        &rw->pRT
    );
    if (SUCCEEDED(hr))
    {
        hr = rw->pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &rw->pBrush);
    }
    if (SUCCEEDED(hr))
    {
        // Initialize stars once the render-target exists
        InitializeStars(rw);
    }
    return hr;
}

// ----- Per-window discard D2D resources -----
void DiscardD2DResources(RenderWindow* rw)
{
    if (!rw) return;
    SafeRelease(&rw->pBrush);
    SafeRelease(&rw->pRT);
}

// TODO: howto add backbuffer implementation with D2D1?
/**
 * @brief Updates star positions and renders the starfield.
 * * @param hWnd The handle to the window (used for InvalidateRect).
 */
void RenderFrame(RenderWindow* rw)
{
    if (!rw || !rw->backHdc) return;
    HRESULT hr = CreateD2DResources(rw);

    if (SUCCEEDED(hr)) 
    {
        pRT->BeginDraw();

        // Clear the screen (ctx.fillStyle = "black"; ctx.fillRect)
        // Direct2D clears to the specified color.
        pRT->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        RECT r = rw->rc;
        int width = max(1, r.right - r.left);
        int height = max(1, r.bottom - r.top);
        uniform_real_distribution<float> ud01(0.0f, 1.0f);
        for (auto& star : rw->g_stars) 
        {
            star.z -= g_StarSpeed * 0.05f; // was star.z -= g_StarSpeed scale down for smoother speeds
            float fx = ud01(rw->rng);
            float fy = ud01(rw->rng);
            float fz = ud01(rw->rng);

            if (star.z <= 0.1f) // was 0
            {
                star.x = (fx - 0.5f) * (float)width * 2.0f;
                star.y = (fy - 0.5f) * (float)height * 2.0f;
                //star.z = fz * (float)width; // depth in screen space
                star.z = fz * (float)width + 1.0f;
                star.speed = (float)g_StarSpeed + ud01(rw->rng) * (float)g_StarSpeed; // Vary speed slightly

            }

            float k = 1024.0f / star.z;

            // Perspective Projection:
            float px = star.x * k + width / 2.0f;
            float py = star.y * k + height / 2.0f;

            // Check if star is on screen (clipping)
            if (px >= 0 && px < width && py >= 0 && py < height)
            {
                // Draw the star (ctx.fillRect(px, py, size, size))
                D2D1_RECT_F starRect = D2D1::RectF(px, py, px + g_StarSize, py + g_StarSize);
                pRT->FillRectangle(&starRect, pWhiteBrush);
            }
        }
        hr = pRT->EndDraw();

        // Handle device loss (occurs if the window is moved to another monitor with a different driver)
        if (hr == D2DERR_RECREATE_TARGET)
        {
            hr = S_OK;
            DiscardD2DResources();
        }
    }

    // Trigger next redraw (replaces requestAnimationFrame)
    // The WM_TIMER approach is a common, simple substitute for rAF in Win32
    SetTimer(rw->hwnd, 1, 16, NULL); // Aim for ~60 FPS (16ms)
}

void RenderFrame(RenderWindow* rw)
{
    if (!rw) return;

    // Create D2D resources for this window (owned by this thread)
    HRESULT hr = CreateD2DResources(rw);
    if (FAILED(hr)) return;

    ID2D1HwndRenderTarget* rt = rw->pRT;
    ID2D1SolidColorBrush* brush = rw->pBrush;
    if (!rt || !brush) return;

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(D2D1::ColorF::Black));

    RECT r = rw->rc;
    int width = max(1, r.right - r.left);
    int height = max(1, r.bottom - r.top);

    // Update and draw stars (simple fixed-step movement; you can add dt later)
    for (auto& star : rw->g_stars)
    {
        star.z -= star.speed * 0.05f; // scale down for smoother speeds

        if (star.z <= 0.1f)
        {
            // respawn
            uniform_real_distribution<float> ud01(0.0f, 1.0f);
            star.x = (ud01(rw->rng) - 0.5f) * (float)width * 2.0f;
            star.y = (ud01(rw->rng) - 0.5f) * (float)height * 2.0f;
            star.z = ud01(rw->rng) * (float)width + 1.0f;
            star.speed = (float)g_StarSpeed + ud01(rw->rng) * (float)g_StarSpeed;
        }

        // Perspective projection
        float k = 1024.0f / max(0.0001f, star.z);
        float px = star.x * k + width * 0.5f;
        float py = star.y * k + height * 0.5f;

        if (px >= 0 && px < width && py >= 0 && py < height)
        {
            D2D1_RECT_F starRect = D2D1::RectF(px, py, px + g_StarSize, py + g_StarSize);
            rt->FillRectangle(&starRect, brush);
        }
    }

    hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        // GPU/device loss or driver change; drop per-window resources so they will be recreated
        DiscardD2DResources(rw);
    }

    // trigger next frame (timer already used elsewhere)
}

// Foreground check and window procs
static bool ForegroundIsOurWindow()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return (fgPid == GetCurrentProcessId());
}

// Window procedure used by render thread windows(uses input filtering)
LRESULT CALLBACK FullWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    RenderWindow* rw = (RenderWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (msg)
    {
        case WM_CREATE:
            g_StartMouseInit = false;
            // Create a Direct2D factory with multithread support
            D2D1_FACTORY_OPTIONS options = {};
            ID2D1Factory* pFactory = nullptr;

            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory),
                &options, reinterpret_cast<void**>(&pFactory))))
            {
                return -1; // Fail window creation
            }

            // Seed random number generator
            std::srand(static_cast<unsigned int>(time(nullptr)));
            // Start the timer for the animation loop
            SetTimer(hWnd, 1, 16, NULL); // Timer ID 1, 16ms delay (~60 FPS)
            return 0;
        case WM_PAINT:
            // Validate rect before drawing
            RenderFrame(rw);
            ValidateRect(hWnd, NULL);
            return 0;
        case WM_SIZE:
            if (rw)
            {
                GetClientRect(hWnd, &rw->rc);
                DestroyBackbuffer(rw);
                CreateBackbuffer(rw);
                g_StartMouseInit = false;
            }
            return 0;
        case WM_KEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_MOUSEMOVE:
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double seconds = double(now.QuadPart - g_StartCounter.QuadPart) / double(g_PerfFreq.QuadPart);
            if (seconds < g_InputDebounceSeconds) { return 0; }
            if (!ForegroundIsOurWindow()) { return 0; }
            if (msg == WM_MOUSEMOVE)
            {
                POINT cur; GetCursorPos(&cur);
                if (!g_StartMouseInit)
                {
                    g_StartMouse = cur;
                    g_StartMouseInit = true;
                    return 0;
                }
                int dx = abs(cur.x - g_StartMouse.x), dy = abs(cur.y - g_StartMouse.y);
                if (dx < g_MouseMoveThreshold && dy < g_MouseMoveThreshold) { return 0; }
            }
            g_Running = false;
            PostQuitMessage(0);
            return 0;
        }
        case WM_TIMER:
            // When the timer fires, redraw the window
            if (wParam == 1)
            { // Check for our timer ID
                InvalidateRect(hWnd, NULL, FALSE); // Forces WM_PAINT without erasing background
            }
            return 0;
        case WM_DESTROY:
            // Clean up resources
            KillTimer(hWnd, 1);
            DiscardD2DResources();
            SafeRelease(&pD2DFactory);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }
}

// Preview proc
LRESULT CALLBACK PreviewProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        FillRect(hdc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
        EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// Thread proc
static DWORD WINAPI RenderThreadProc(LPVOID lpv)
{
    ThreadParam* tp = (ThreadParam*)lpv;
    RenderWindow* rw = tp->rw;
    RECT r = tp->rc;
    HWND owner = tp->ownerForParent;
    wstring className = tp->className ? tp->className : L"MyStarfieldD2d1";
    delete tp;

    // Register a thread-local window class
    WNDCLASSW wc = {};
    wc.lpfnWndProc = FullWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = className.c_str();
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(4 + 1);
    RegisterClassW(&wc);

    // Create the window on this thread. Use owner=owner so if owner was a preview host
    // the dialog remains properly associated; the thread owns this window.
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        className.c_str(),
        L"MyStarfield",
        WS_POPUP | WS_VISIBLE,
        r.left, r.top, r.right - r.left, r.bottom - r.top,
        owner, // owner/parent for z-order/embedding
        NULL,
        g_hInst,
        NULL
    );

    if (!hwnd)
    {
        UnregisterClassW(className.c_str(), g_hInst);
        // signal failure
        return 1;
    }

    // attach RenderWindow to hwnd
    rw->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)rw);

    // initialize backbuffer and stars on this thread
    GetClientRect(hwnd, &rw->rc);
    CreateBackbuffer(rw);
    InitializeStars(rw);

    // optionally hide cursor for fullscreen effect
    // call ShowCursor(FALSE) until hidden; keep balanced on exit
    while (ShowCursor(FALSE) >= 0) { /* decrement to hidden */ }

    // high-resolution timing
    LARGE_INTEGER perfFreq = {};
    QueryPerformanceFrequency(&perfFreq);
    LARGE_INTEGER last;
    QueryPerformanceCounter(&last);
    double totalTime = 0.0;

    MSG msg;
    // message + render loop
    while (g_Running && IsWindow(hwnd))
    {
        // process all pending messages
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_Running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // render frame using your RenderFrame function; this draws into rw->backHdc
        RenderFrame(rw);

        // check stop event quickly
        if (rw->hStopEvent && WaitForSingleObject(rw->hStopEvent, 0) == WAIT_OBJECT_0) break;

        // frame pacing; yield a bit (adjust to taste)
        Sleep(8);
    }

    // restore cursor visibility
    while (ShowCursor(TRUE) < 0) { /* increment until visible again */ }

    // cleanup backbuffer and window (must run on this thread)
    DestroyBackbuffer(rw);
    if (IsWindow(hwnd))
    {
        DestroyWindow(hwnd); // this will send WM_DESTROY
    }
    UnregisterClassW(className.c_str(), g_hInst);
    return 0;
}

// Monitor enumeration + thread spawn -> create fullscreen windows
static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    RECT r = mi.rcMonitor;

    // create RenderWindow and seed RNG
    RenderWindow* rw = new RenderWindow();
    random_device rd;
    rw->rng.seed((unsigned int)(rd() ^ GetTickCount()));

    // create a stop event used to tell the worker to exit quickly
    rw->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    // prepare thread parameters
    ThreadParam* tp = new ThreadParam();
    tp->rc = r;
    tp->ownerForParent = NULL; // no owner for top-level fullscreen windows
    tp->rw = rw;
    tp->className = L"MyStarfieldCls";

    // spawn worker thread (thread creates window and does rendering)
    rw->hThread = CreateThread(NULL, 0, RenderThreadProc, tp, 0, &rw->threadId);
    if (!rw->hThread)
    {
        SafeCloseHandle(rw->hStopEvent);
        delete rw;
        delete tp;
        return TRUE;
    }

    g_Windows.push_back(rw);
    return TRUE;
}

// Run fullscreen
static void RunFull()
{
    EnumDisplayMonitors(NULL, NULL, MonEnumProc, 0);
    QueryPerformanceFrequency(&g_PerfFreq);
    QueryPerformanceCounter(&g_StartCounter);
    POINT p;
    GetCursorPos(&p);
    g_StartMouse = p;
    g_StartMouseInit = true;
    LARGE_INTEGER last;
    QueryPerformanceCounter(&last);
    double total = 0.0;
    MSG msg;
    while (g_Running)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                g_Running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(g_PerfFreq.QuadPart);
        last = now;
        total += dt;
        for (auto rw : g_Windows) RenderFrame(rw);
        Sleep(8);
    }
    for (auto rw : g_Windows)
    {
        DestroyBackbuffer(rw);
        if (rw->hwnd) DestroyWindow(rw->hwnd);
        delete rw;
    }
    g_Windows.clear();
}

// Preview runner
static int RunPreview(HWND parent)
{
    if (!IsWindow(parent)) return 0;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PreviewProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"MyStarPre";
    RegisterClassW(&wc);

    RECT pr;
    GetClientRect(parent, &pr);
    HWND child = CreateWindowExW(0, wc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, pr.right - pr.left, pr.bottom - pr.top, parent, NULL, g_hInst, NULL);
    if (!child)
    {
        UnregisterClassW(wc.lpszClassName, g_hInst);
        return 0;
    }

    RenderWindow* rw = new RenderWindow();
    rw->hwnd = child;
    rw->isPreview = true;
    rw->rc = pr;
    random_device rd;
    rw->rng.seed(rd());
    CreateBackbuffer(rw);
    InitializeStars(rw);
    QueryPerformanceFrequency(&g_PerfFreq);
    LARGE_INTEGER last;
    QueryPerformanceCounter(&last);
    double total = 0.0;
    MSG msg;
    while (IsWindow(child))
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { DestroyWindow(child); break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double dt = double(now.QuadPart - last.QuadPart) / double(g_PerfFreq.QuadPart);
        last = now; total += dt;
        RenderFrame(rw);
        Sleep(15);
    }
    DestroyBackbuffer(rw);
    ShowCursor(TRUE);
    DestroyWindow(child);
    UnregisterClassW(wc.lpszClassName, g_hInst);
    delete rw;
    return 0;
}

//SETTINGS DIALOG///////////////////////////////////////////////////////////////////////////////////////////
// IDs
enum { CID_OK = 100, CID_CANCEL = 101, CID_EDIT_STARS = 110, CID_EDIT_SPEED = 111, CID_PREVIEW = 112, CID_BUTTON_COLOR = 113, CID_LABEL_COLOR = 114 };

// Create child controls on given window
static void CreateSettingsControls(HWND dlg)
{
    wstring mst = L"Stars (max " + to_wstring(g_MaxStars) + L"):";
    wstring msp = L"Speed (max " + to_wstring(g_MaxSpeed) + L"):";
    HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HWND hColorLabel = CreateWindowExW(0, L"STATIC", mst.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 10, 120, 18, dlg, NULL, g_hInst, NULL);
    HWND hColorEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT, 150, 8, 80, 20, dlg, (HMENU)CID_EDIT_STARS, g_hInst, NULL);
    HWND hSpeedLabel = CreateWindowExW(0, L"STATIC", msp.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 40, 120, 18, dlg, NULL, g_hInst, NULL);
    HWND hSpeedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT, 150, 38, 80, 20, dlg, (HMENU)CID_EDIT_SPEED, g_hInst, NULL);
    HWND hColorButton = CreateWindowExW(0, L"BUTTON", L"Pick color", WS_CHILD | WS_VISIBLE, 250, 6, 80, 20, dlg, (HMENU)CID_BUTTON_COLOR, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"     ", WS_CHILD | WS_VISIBLE | SS_SIMPLE | SS_BLACKFRAME, 250, 38, 80, 20, dlg, (HMENU)CID_LABEL_COLOR, g_hInst, NULL);
    HWND hOkButton = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 80, 70, 80, 24, dlg, (HMENU)CID_OK, g_hInst, NULL);
    HWND hCancelButton = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 168, 70, 80, 24, dlg, (HMENU)CID_CANCEL, g_hInst, NULL);

    SendMessageW(hColorLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hColorEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hSpeedLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hSpeedEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hColorButton, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hOkButton, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCancelButton, WM_SETFONT, (WPARAM)hFont, TRUE);
}

// Settings window proc handles control actions and closes window
LRESULT CALLBACK SettingsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // retrieve the disabled window when the dialog was created
    HWND owner = (HWND)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (!owner) owner = GetParent(hWnd); // fallback for safety

    HWND hColorLabel = GetDlgItem(hWnd, CID_LABEL_COLOR);
    g_hBrushColor = CreateSolidBrush(g_CurrentStarColor);
    switch (msg)
    {
    case WM_CREATE:
    {
        CreateSettingsControls(hWnd);
        SetDlgItemInt(hWnd, CID_EDIT_STARS, g_StarCount, FALSE);
        SetDlgItemInt(hWnd, CID_EDIT_SPEED, g_StarSpeed, FALSE);
        return 0;
    }
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        if (id == CID_BUTTON_COLOR)
        {
            vector<COLORREF> persisted;
            LoadCustomColors(persisted);
            for (int i = 0; i < 16; ++i) g_CustomColors[i] = persisted[i];

            CHOOSECOLOR cc = {};
            ZeroMemory(&cc, sizeof(cc));
            cc.lStructSize = sizeof(cc);
            cc.hwndOwner = hWnd; // parent window handle
            cc.lpCustColors = g_CustomColors;
            cc.rgbResult = g_CurrentStarColor; // initial color
            cc.Flags = CC_FULLOPEN | CC_RGBINIT;

            if (ChooseColor(&cc))
            {
                // user picked a color — cc.rgbResult now contains the selection
                g_CurrentStarColor = cc.rgbResult;
                // Macro's to update RGB components inside the Colorpicker
                BYTE r = GetRValue(g_CurrentStarColor), g = GetGValue(g_CurrentStarColor), b = GetBValue(g_CurrentStarColor);
                // save back the updated custom colors
                vector<COLORREF> toSave(16);
                for (int i = 0; i < 16; ++i)
                {
                    toSave[i] = g_CustomColors[i];
                }
                SaveCustomColors(toSave);
                if (g_hBrushColor) DeleteObject(g_hBrushColor);
                g_hBrushColor = CreateSolidBrush(g_CurrentStarColor);
                InvalidateRect(hColorLabel, NULL, TRUE);
            }
            return 0;
        }
        else if (id == CID_OK)
        {
            BOOL ok;
            int stars = GetDlgItemInt(hWnd, CID_EDIT_STARS, &ok, FALSE);
            if (!ok) stars = g_StarCount;
            else stars = (int)fmax(1, fmin(g_MaxStars, stars));

            int speed = GetDlgItemInt(hWnd, CID_EDIT_SPEED, &ok, FALSE);
            if (!ok) speed = g_StarSpeed;
            else speed = (int)fmax(1, fmin(g_MaxSpeed, speed));

            g_StarCount = stars;
            g_StarSpeed = speed;

            SaveSettings();
            DestroyWindow(hWnd);
            return 0;
        }
        else if (id == CID_CANCEL)
        {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC hdcStatic = (HDC)wParam;
        HWND hStatic = (HWND)lParam;

        if (GetDlgCtrlID(hStatic) == CID_LABEL_COLOR)
        {
            SetBkMode(hdcStatic, OPAQUE);
            SetBkColor(hdcStatic, g_CurrentStarColor);
            return (INT_PTR)g_hBrushColor;
        }
        break;
    }
    case WM_DESTROY:
    {
        if (owner && IsWindow(owner))
        {
            EnableWindow(owner, TRUE);
            BringWindowToTop(owner);
            SetActiveWindow(owner);
            SetForegroundWindow(owner);
            SetFocus(owner);

            // Ensure Z-order of Parent Settings Dialog
            SetWindowPos(owner, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

            if (g_hBrushColor)
            {
                DeleteObject(g_hBrushColor);
                g_hBrushColor = NULL;
            }
        }
        return 0;
    }
    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// Register settings class once
static void EnsureSettingsClassRegistered()
{
    static bool reg = false;
    if (reg) return;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"MyStarfieldSettingsClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    reg = true;
}

// Show modal popup (centered) and block until closed
static int ShowSettingsModalPopup(HWND parent)
{
    if (!IsWindow(parent)) return 0;
    EnsureSettingsClassRegistered();
    HWND wParent = GetParent(parent);
    if (wParent && IsWindow(wParent)) EnableWindow(wParent, FALSE);

    int w = 360, h = 144;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - w) / 2, y = (sh - h) / 2;

    // In ShowSettingsModalPopup, after disabling wParent store it on the dialog
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MyStarfieldSettingsClass", L"MyStarfield Settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, w, h, parent, NULL, g_hInst, NULL);
    if (!dlg)
    {
        if (wParent && IsWindow(wParent)) EnableWindow(wParent, TRUE);
        return -1;
    }

    // remember which window is disabled so SettingsWndProc can restore it later
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)wParent);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    // Modal message loop: run until dlg destroyed
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (wParent && IsWindow(wParent)) EnableWindow(wParent, TRUE);
    return 0;
}

// Shutdown helpers
static void StopAllThreadsAndCleanup()
{
    // signal stop to each worker
    for (auto rw : g_Windows)
    {
        if (rw->hStopEvent) SetEvent(rw->hStopEvent);
        // optionally PostThreadMessage to wake thread message loop
        if (rw->threadId) PostThreadMessage(rw->threadId, WM_USER + 1, 0, 0);
    }

    // wait for threads to exit
    for (auto rw : g_Windows)
    {
        if (rw->hThread)
        {
            WaitForSingleObject(rw->hThread, 2000);
            SafeCloseHandle(rw->hThread);
        }
        SafeCloseHandle(rw->hStopEvent);
        // RenderWindow memory belongs to us; windows are destroyed inside threads
        delete rw;
    }
    g_Windows.clear();
}

// --- WinMain Entry Point ---
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    g_hInst = hInstance;
    LoadSettings();
    wchar_t modPath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, modPath, MAX_PATH);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t mode = 0;
    HWND argH = NULL;
    ParseArgs(argc, argv, mode, argH);
    {
        char buf[256]{};
    }
    if (mode == 'c')
    {
        ShowSettingsModalPopup(argH);
        LocalFree(argv);
        return 0;
    }
    if (mode == 'p')
    {
        if (argH)
        {
            RunPreview(argH);
            LocalFree(argv);
            return 0;
        }
    }
    // Default: fullscreen
    g_Running = true;
    RunFull();
    LocalFree(argv);
    StopAllThreadsAndCleanup();
    return 0;
}