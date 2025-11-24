#include <windows.h>
#include <d2d1.h>
#include <wincodec.h>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <ctime>

// --- Linker Directives (for Visual Studio) ---
#pragma comment(lib, "d2d1.lib") 
#pragma comment(lib, "d3d11.lib") // Often needed for Direct2D context setup
#pragma comment(lib, "windowscodecs.lib") // For image loading, good practice

// --- Constants ---
const int NUM_STARS = 1024;
const float STAR_SPEED = 12.0f;

// --- COM Helper (for releasing Direct2D objects) ---
template <class T> void SafeRelease(T** ppT) 
{
    if (*ppT) 
    {
        (*ppT)->Release();
        *ppT = NULL;
    }
}

// --- Star Structure ---
struct Star 
{
    float x; // x-coordinate in 3D space (relative to center)
    float y; // y-coordinate in 3D space (relative to center)
    float z; // depth/distance
};

// --- Direct2D Globals and State ---
ID2D1Factory* pD2DFactory = nullptr;
ID2D1HwndRenderTarget* pRT = nullptr;
ID2D1SolidColorBrush* pWhiteBrush = nullptr;
std::vector<Star> g_stars;

// Window dimensions
int g_screenWidth = 800;
int g_screenHeight = 600;

// --- Forward Declarations ---
HRESULT CreateD2DResources(HWND hWnd);
void DiscardD2DResources();
void InitializeStars();
void AnimateAndRender(HWND hWnd);

/**
 * @brief Initializes stars with random positions based on current window size.
 */
void InitializeStars() {
    g_stars.clear();
    // Use screenWidth for the initial z-range, similar to JS 'canvas.width'
    float zRange = (float)g_screenWidth;

    for (int i = 0; i < NUM_STARS; ++i) {
        g_stars.push_back({
            // x: Math.random() * canvas.width - canvas.width/2
            (float)std::rand() / RAND_MAX * g_screenWidth - g_screenWidth / 2.0f,
            // y: Math.random() * canvas.height - canvas.height/2
            (float)std::rand() / RAND_MAX * g_screenHeight - g_screenHeight / 2.0f,
            // z: Math.random() * canvas.width
            (float)std::rand() / RAND_MAX * zRange
            });
    }
}

/**
 * @brief Creates Direct2D factory, render target, and brushes.
 */
HRESULT CreateD2DResources(HWND hWnd) 
{
    HRESULT hr = S_OK;
    if (!pRT) 
    {
        // Get window dimensions
        RECT rc;
        GetClientRect(hWnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right, rc.bottom);
        // Create a Direct2D render target
        hr = pD2DFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hWnd, size),
            &pRT
        );
        if (SUCCEEDED(hr)) 
        {
            // Create a white brush for the stars
            hr = pRT->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &pWhiteBrush);
        }

        if (SUCCEEDED(hr)) {
            // Update global size and re-initialize stars
            g_screenWidth = rc.right;
            g_screenHeight = rc.bottom;
            InitializeStars();
        }
    }
    return hr;
}

/**
 * @brief Releases Direct2D resources.
 */
void DiscardD2DResources() 
{
    // Release COM objects
    // Note: pRT needs to be released first, as it holds a reference to pWhiteBrush
    SafeRelease(&pRT);
    SafeRelease(&pWhiteBrush);
}

/**
 * @brief Updates star positions and renders the starfield.
 * * @param hWnd The handle to the window (used for InvalidateRect).
 */
void AnimateAndRender(HWND hWnd) 
{
    HRESULT hr = CreateD2DResources(hWnd);

    if (SUCCEEDED(hr)) 
    {
        pRT->BeginDraw();

        // --- Clear the screen (ctx.fillStyle = "black"; ctx.fillRect) ---
        // Direct2D clears to the specified color.
        pRT->Clear(D2D1::ColorF(D2D1::ColorF::Black));
        for (auto& star : g_stars) 
        {
            star.z -= STAR_SPEED;

            if (star.z <= 0) 
            {
                star.z = (float)g_screenWidth;
                // Reset x/y when wrapping z
                star.x = (float)std::rand() / RAND_MAX * g_screenWidth - g_screenWidth / 2.0f;
                star.y = (float)std::rand() / RAND_MAX * g_screenHeight - g_screenHeight / 2.0f;
            }

            float k = 1024.0f / star.z;

            // Perspective Projection:
            float px = star.x * k + g_screenWidth / 2.0f;
            float py = star.y * k + g_screenHeight / 2.0f;

            // Check if star is on screen (clipping)
            if (px >= 0 && px < g_screenWidth && py >= 0 && py < g_screenHeight) 
            {
                float size = (1.0f - star.z / g_screenWidth) * 1.0f;
                if (size < 1.0f) size = 1.0f; // Minimum size of 1 pixel
                size = 0.5;
                // Draw the star (ctx.fillRect(px, py, size, size))
                D2D1_RECT_F starRect = D2D1::RectF(px, py, px + size, py + size);
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
    SetTimer(hWnd, 1, 16, NULL); // Aim for ~60 FPS (16ms)
}

// --- Standard Win32 Window Procedure ---
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) 
{
    switch (message) 
    {
    case WM_CREATE:
        // Initialize Direct2D factory
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pD2DFactory))) 
        {
            return -1; // Fail window creation
        }
        // Seed random number generator
        std::srand(static_cast<unsigned int>(time(nullptr)));
        // Start the timer for the animation loop
        SetTimer(hWnd, 1, 16, NULL); // Timer ID 1, 16ms delay (~60 FPS)
        return 0;

    case WM_SIZE: 
    {
        // Handle resizing (similar to window.addEventListener("resize", resize))
        UINT width = LOWORD(lParam);
        UINT height = HIWORD(lParam);
        if (pRT) 
        {
            HRESULT hr = pRT->Resize(D2D1::SizeU(width, height));
            if (SUCCEEDED(hr)) 
            {
                g_screenWidth = width;
                g_screenHeight = height;
                InitializeStars(); // Re-initialize stars on resize
            }
            else {
                DiscardD2DResources();
            }
        }
        return 0;
    }

    case WM_PAINT:
        // Validate rect before drawing
        AnimateAndRender(hWnd);
        ValidateRect(hWnd, NULL);
        return 0;

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
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

// --- WinMain Entry Point ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) 
{
    // 1. Register Window Class
    const wchar_t CLASS_NAME[] = L"StarfieldAppClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&wc);

    // 2. Create Window
    HWND hWnd = CreateWindowEx(
        0, CLASS_NAME, L"Direct2D Starfield",
        WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, g_screenWidth, g_screenHeight,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // 3. Message Loop (The main program loop)
    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) 
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}