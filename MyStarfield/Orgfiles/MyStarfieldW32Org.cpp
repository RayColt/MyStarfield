/*
* MyStarfield.cpp
* Single Threaded starfield:)
* Build as Windows GUI with Console Entry point with:
* - Linker/SubSystem/Windows/(/SUBSYSTEM:WINDOWS)
* - C/C++/Preprocessor/Preprocessor Definitions/(WIN32;DEBUG;_CONSOLE;)
* 
* Copy generated MyStarfield.scr in Debug directory to C:\Windows\System32
* 
* If you experience some latencies lower down the amount of stars and speed,
* use settings like g_MaxStars=200 and g_MaxSpeed=20, only the Win32 API is used.
*/
#include <windows.h>
#include <random>
#include <string>

using namespace std;

// ---- Config / registry keys
static LPCWSTR REG_KEY = L"Software\\MyStarfieldW32";
static LPCWSTR REG_STARS = L"StarCount";
static LPCWSTR REG_SPEED = L"Speed";
static LPCWSTR REG_COLOR = L"Color";
static LPCWSTR REG_CCOLORS = L"Custom Colors";
static COLORREF g_CustomColors[16];

// Defaults
static int g_StarCount = 200;
static int g_Speed = 10;
static int g_MaxStars = 1000;
static int g_MaxSpeed = 250;
static COLORREF g_CurrentStarColor = RGB(255, 255, 255);// Default Star Color!
HBRUSH g_hBrushColor = NULL;

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
    g_Speed = GetRegDWORD(REG_SPEED, g_Speed);
	g_CurrentStarColor = GetRegDWORD(REG_COLOR, g_CurrentStarColor);
}
static void SaveSettings()
{
    SetRegDWORD(REG_STARS, (DWORD)g_StarCount);
    SetRegDWORD(REG_SPEED, (DWORD)g_Speed);
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
        static_cast<DWORD>(sizeof(COLORREF) * colors.size())) == ERROR_SUCCESS;
        RegCloseKey(hKey);
    }
    RegCloseKey(hKey);
}

// Star model
struct Star
{
    float x;     // world X (centered)
    float y;     // world Y (centered)
    float z;     // depth
    float speed; // per-star speed (depth units / second or arbitrary units)
};

// RenderWindow
struct RenderWindow
{
    HWND hwnd = NULL;
    HDC backHdc = NULL;
    HBITMAP backBmp = NULL;
    HBITMAP oldBackBmp = NULL;
    RECT rc = {};
    vector<Star> stars;
    mt19937 rng;
    bool isPreview = false;
};

// Globals
static HINSTANCE g_hInst = NULL;
static vector<RenderWindow*> g_Windows;
static bool g_Running = true;

// Input filtering
static LARGE_INTEGER g_PerfFreq;
static LARGE_INTEGER g_StartCounter;
static double g_InputDebounceSeconds = 0.66; // time to stop screensaver after mouse move
static POINT g_StartMouse = { 0,0 };
static bool g_StartMouseInit = false;
static const int g_MouseMoveThreshold = 12; // pixels

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

// ---- backbuffer helpers
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

// InitStars: centered world coords (so projection works predictably)
// Smaller Z_MIN(closer to 0) makes stars appear larger and move faster
// as they approach because projection uses 1 / z.
static const float Z_MIN = 1.0f;
// decrease Z_MAX (e.g., 800) to bring more stars visually forward
static const float Z_MAX = 33.0f;
// FOCAL ~ 1.0 is appropriate for the sample values (x ~ [-1600..1600], z ~ [10..100])
static const float FOCAL = 9.0f;
// multiplier that controls drawn core size; increase for larger stars
static const float SIZE_SCALE = 1.0f;

static void InitStars(RenderWindow* rw)
{
    if (!rw) return;
    RECT r = rw->rc;
    int width = max(1, r.right - r.left);
    int height = max(1, r.bottom - r.top);
    rw->stars.clear();
    rw->stars.resize(g_StarCount);
    int jitterMax = max(1, g_Speed / 2 + 1);
    uniform_real_distribution<float> ud01(0.0f, 1.0f);

    for (int i = 0; i < g_StarCount; ++i)
    {
        float fx = ud01(rw->rng);
        float fy = ud01(rw->rng);
        float fz = ud01(rw->rng);

        // centered world coords
        rw->stars[i].x = (fx - 0.5f) * (float)width * 2.0f;
        rw->stars[i].y = (fy - 0.5f) * (float)height * 2.0f;

        // deep range (classic)
        rw->stars[i].z = fz * (Z_MAX - Z_MIN) + Z_MIN;
        rw->stars[i].speed = (float)g_Speed + float(rw->rng() % jitterMax);
    }
}

// RenderFrame tuned to the sample values (uses totalTime)
static void RenderFrame(RenderWindow* rw, float dt, float totalTime)
{
    if (!rw || !rw->backHdc) return;
    int w = max(1, rw->rc.right - rw->rc.left);
    int h = max(1, rw->rc.bottom - rw->rc.top);
    float cx = w * 0.5f, cy = h * 0.5f;

    // clear
    RECT fill = { 0,0,w,h };
    FillRect(rw->backHdc, &fill, (HBRUSH)GetStockObject(BLACK_BRUSH));

    HPEN oldPen = (HPEN)SelectObject(rw->backHdc, GetStockObject(NULL_PEN));
    int baseR = GetRValue(g_CurrentStarColor), baseG = GetGValue(g_CurrentStarColor), baseB = GetBValue(g_CurrentStarColor);

    // subtle pulse
    float pulse = 1.0f + 0.05f * sinf(totalTime * 1.5f);

    // small brush cache (few buckets)
    const int BUCKETS = 6;
    HBRUSH brushes[BUCKETS] = { 0 };

    for (auto& s : rw->stars)
    {
        // advance depth
        s.z -= s.speed * dt * 0.5f;
        if (s.z <= Z_MIN)
        {
            // respawn centered and far
            uniform_real_distribution<float> ud01(0.0f, 1.0f);
            float fx = ud01(rw->rng), fy = ud01(rw->rng), fz = ud01(rw->rng);
            s.x = (fx - 0.5f) * (float)w * 2.0f;
            s.y = (fy - 0.5f) * (float)h * 2.0f;
            s.z = fz * (Z_MAX - Z_MIN) + Z_MIN;
            int jitterMax = max(1, g_Speed / 2 + 1);
            s.speed = (float)g_Speed + float(rw->rng() % jitterMax);
        }

        // projection using small focal factor
        float px = cx + s.x * (FOCAL / s.z);
        float py = cy + s.y * (FOCAL / s.z);

        // star size scales with inverse depth; near -> larger
        float inv = (Z_MIN / s.z); // near => closer to 1
        int psz = (int)ceilf(max(1.0f, SIZE_SCALE * inv));
        if (psz > 128) psz = 128;

        // intensity from depth (near -> brighter) then pulsate
        float t = (s.z - Z_MIN) / (Z_MAX - Z_MIN); // 0..1
        float depthIntensity = 1.0f - t;
        int intensity = (int)lroundf(100.0f + depthIntensity * 155.0f * pulse);
        intensity = max(0, min(255, intensity));

        // map to bucket
        int bucket = (int)((intensity) / (256.0f / BUCKETS));
        bucket = max(0, min(BUCKETS - 1, bucket));
        int br = (baseR * intensity) / 255;
        int bg = (baseG * intensity) / 255;
        int bb = (baseB * intensity) / 255;
        if (!brushes[bucket])
        {
            // slightly move nearer buckets toward white for pop
            float whiten = 0.5f + 0.5f * (bucket / (float)(BUCKETS - 1));
            br = min(255, (int)lroundf(br * whiten + 255 * (1.0f - whiten)));
            bg = min(255, (int)lroundf(bg * whiten + 255 * (1.0f - whiten)));
            bb = min(255, (int)lroundf(bb * whiten + 255 * (1.0f - whiten)));
            brushes[bucket] = CreateSolidBrush(RGB(br, bg, bb));
        }
        // skip if offscreen
        if (px + psz < 0 || px - psz > w || py + psz < 0 || py - psz > h) continue;
        HBRUSH oldBrush = nullptr;
        if (brushes[bucket]) 
        {
            int ix = (int)lroundf(px);
            int iy = (int)lroundf(py);
            if (psz <= 1) 
            {
                // 1-pixel star: avoid creating a brush per star, used SetPixelV for speed
                SetPixelV(rw->backHdc, ix, iy, RGB(br, bg, bb));
            }
            else 
            {
                // existing bucket brush approach for larger stars
                HBRUSH oldBrush = (HBRUSH)SelectObject(rw->backHdc, brushes[bucket]);
                Ellipse(rw->backHdc, ix - psz, iy - psz, ix + psz + 1, iy + psz + 1);
                SelectObject(rw->backHdc, oldBrush);
            }
        }
        /* Old if (brushes[bucket]){}/Ellipse part, for >= 2px stars
        HBRUSH oldBrush = (HBRUSH)SelectObject(rw->backHdc, brushes[bucket]);
        Ellipse(rw->backHdc,
            (int)floorf(px - psz), (int)floorf(py - psz),
            (int)ceilf(px + psz + 1), (int)ceilf(py + psz + 1));
        SelectObject(rw->backHdc, oldBrush);
        */
    }

    // cleanup
    for (int i = 0; i < BUCKETS; ++i) if (brushes[i])
    {
        DeleteObject(brushes[i]); brushes[i] = NULL;
    }
    SelectObject(rw->backHdc, oldPen);
    // blit
    HDC wnd = GetDC(rw->hwnd);
    BitBlt(wnd, 0, 0, w, h, rw->backHdc, 0, 0, SRCCOPY);
    ReleaseDC(rw->hwnd, wnd);
}

// ---- Foreground check and window procs
static bool ForegroundIsOurWindow()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(fg, &fgPid);
    return (fgPid == GetCurrentProcessId());
}

// Fullscreen window proc (uses input filtering)
LRESULT CALLBACK FullWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    RenderWindow* rw = (RenderWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    switch (msg)
    {
        case WM_CREATE:
            g_StartMouseInit = false;
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
        case WM_DESTROY:
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

// Monitor enumeration -> create fullscreen windows
static BOOL CALLBACK MonEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
    RECT r = mi.rcMonitor;
    RenderWindow* rw = new RenderWindow();
    rw->rc = r;
    random_device rd;
    rw->rng.seed(rd());
    static bool reg = false;
    if (!reg)
    {
        WNDCLASSW wc = {}; 
        wc.lpfnWndProc = FullWndProc; 
        wc.hInstance = g_hInst;
        wc.lpszClassName = L"StarfieldFullClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&wc); 
        reg = true;
    }
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"StarfieldFullClass", L"Starfield", WS_POPUP | WS_VISIBLE,
        r.left, r.top, r.right - r.left, r.bottom - r.top, NULL, NULL, g_hInst, NULL);
    if (!hwnd)
    {
        delete rw;
        return TRUE;
    }
    rw->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)rw);
    ShowCursor(FALSE);
    ShowWindow(hwnd, SW_SHOW);
    GetClientRect(hwnd, &rw->rc);
    CreateBackbuffer(rw);
    InitStars(rw);
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
        for (auto rw : g_Windows) RenderFrame(rw, (float)dt, (float)total);
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

// Simple preview runner
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
    InitStars(rw);
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
        RenderFrame(rw, (float)dt, (float)total);
        Sleep(15);
    }
    DestroyBackbuffer(rw);
    ShowCursor(TRUE);
    DestroyWindow(child);
    UnregisterClassW(wc.lpszClassName, g_hInst);
    delete rw;
    return 0;
}

// ---------------- Settings dialog programmatic UI ----------------
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
    HWND hColorButton = CreateWindowExW(0, L"BUTTON", L"Pick color", WS_CHILD | WS_VISIBLE, 250, 8, 80, 24, dlg, (HMENU)CID_BUTTON_COLOR, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"     ",  WS_CHILD | WS_VISIBLE | SS_SIMPLE | SS_BLACKFRAME, 250, 38, 80, 26, dlg, (HMENU)CID_LABEL_COLOR, g_hInst, NULL);
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
        SetDlgItemInt(hWnd, CID_EDIT_SPEED, g_Speed, FALSE);
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
            stars = (int)fmax(1, fmin(g_MaxStars, stars));

            int speed = GetDlgItemInt(hWnd, CID_EDIT_SPEED, &ok, FALSE);
            if (!ok) speed = g_Speed;
            speed = (int)fmax(1, fmin(g_MaxSpeed, speed));

            g_StarCount = stars;
            g_Speed = speed;

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

// Entry point
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
    return 0;
}