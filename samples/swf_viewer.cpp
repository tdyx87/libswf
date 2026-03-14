// Windows SWF Viewer
// Simple viewer using GDI/GDI+

#ifdef _WIN32

// Prevent min/max macro conflicts
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#ifdef RENDERER_GDIPLUS
    #include "common/gdiplus_fix.h"
    #ifdef _MSC_VER
        #pragma comment(lib, "gdiplus.lib")
    #endif
#endif

#include <string>
#include <iostream>
#include "renderer/renderer.h"

using namespace libswf;

const wchar_t* g_windowClass = L"libswf_viewer";
const wchar_t* g_windowTitle = L"libswf Viewer";
HWND g_hwnd = nullptr;
SWFRenderer* g_renderer = nullptr;
std::string g_filename;

// Forward declarations
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void OpenFile();
void RenderSWF(HDC hdc);

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                      LPWSTR lpCmdLine, int nCmdShow) {
#ifdef RENDERER_GDIPLUS
    // Initialize GDI+
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    gdiplusStartupInput.GdiplusVersion = 1;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
#endif
    
    // Register window class
    WNDCLASSEXW wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, IDI_APPLICATION);
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = NULL;
    wcex.lpszClassName = g_windowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, IDI_APPLICATION);
    
    RegisterClassExW(&wcex);
    
    // Create window
    g_hwnd = CreateWindowW(g_windowClass, g_windowTitle, WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL, NULL, hInstance, NULL);
    
    if (!g_hwnd) {
        return FALSE;
    }
    
    // Create renderer
    g_renderer = new SWFRenderer();
    
    // Get filename from command line
    if (__argc > 1) {
        g_filename = __argv[1];
        g_renderer->loadSWF(g_filename);
    }
    
    // Show window
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);
    
    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Cleanup
    delete g_renderer;
#ifdef RENDERER_GDIPLUS
    Gdiplus::GdiplusShutdown(gdiplusToken);
#endif
    
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            
            if (g_renderer) {
                RECT rect;
                GetClientRect(hWnd, &rect);
                int width = rect.right - rect.left;
                int height = rect.bottom - rect.top;
                
                g_renderer->render(hdc, width, height);
            }
            
            EndPaint(hWnd, &ps);
            break;
        }
        
        case WM_SIZE: {
            if (g_renderer) {
                InvalidateRect(hWnd, NULL, TRUE);
            }
            break;
        }
        
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            char filename[MAX_PATH];
            DragQueryFileA(hDrop, 0, filename, MAX_PATH);
            DragFinish(hDrop);
            
            if (g_renderer->loadSWF(filename)) {
                InvalidateRect(hWnd, NULL, TRUE);
            }
            break;
        }
        
        case WM_KEYDOWN: {
            if (wParam == VK_SPACE) {
                if (g_renderer->isPlaying()) {
                    g_renderer->stop();
                } else {
                    g_renderer->play();
                }
                InvalidateRect(hWnd, NULL, TRUE);
            } else if (wParam == VK_LEFT) {
                uint16 frame = g_renderer->getCurrentFrame();
                if (frame > 1) {
                    g_renderer->gotoFrame(frame - 1);
                    InvalidateRect(hWnd, NULL, TRUE);
                }
            } else if (wParam == VK_RIGHT) {
                g_renderer->advanceFrame();
                InvalidateRect(hWnd, NULL, TRUE);
            }
            break;
        }
        
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
            
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

#else

// Non-Windows stub
#include <iostream>

int main() {
    std::cerr << "This sample requires Windows to run." << std::endl;
    return 1;
}

#endif
