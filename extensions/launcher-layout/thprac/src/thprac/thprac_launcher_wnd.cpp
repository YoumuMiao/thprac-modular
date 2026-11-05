#include "thprac_launcher_wnd.h"
#include "imgui.h"
#include "thprac_gui_components.h"
#include "thprac_launcher_cfg.h"
#include "thprac_launcher_cover.h"
#include "thprac_gui_impl_dx9.h"
#include "thprac_gui_impl_win32.h"
#include "thprac_gui_locale.h"
#include "thprac_launcher_utils.h"
#include "thprac_utils.h"
#include "..\..\resource.h"
#include <algorithm>
#include <array>
#include <d3d9.h>
#include <tchar.h>
#include <cstring>
#include <Windows.h>
#pragma warning(disable : 4091)
#include <Shlobj.h>
#pragma warning(default : 4091)

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

typedef HRESULT(WINAPI* PSetProcessDpiAwareness)(DWORD value);
typedef HRESULT(WINAPI* PGetDpiForMonitor)(HMONITOR hmonitor, DWORD dpiType, UINT* dpiX, UINT* dpiY);

namespace THPrac {
extern bool g_isLauncher;

namespace Gui {
    extern LRESULT ImplWin32WndProcHandlerW(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
}

// Layout metrics use __thprac_lc_scale, which carries both the monitor DPI and
// the user's UI scale. The window itself is sized from the DPI factor only, so
// that the chosen startup resolution keeps meaning "pixels on screen".
static float __thprac_lc_scale = 1.0f;
static float __thprac_lc_dpiScale = 1.0f;
static float __thprac_lc_uiScale = 1.0f;
static bool __thprac_lc_hasInited = false;
static HWND __thprac_lc_hwnd;
static bool __thprac_lc_canMove = false;
// Resize cap, already scaled for the monitor DPI and raised with the UI scale.
// The window opens at the startup size and can be dragged up to this cap.
static unsigned int __thprac_lc_maxWidth = 0;
static unsigned int __thprac_lc_maxHeight = 0;
// Size the window was created at, in physical pixels.
static unsigned int __thprac_lc_startupWidth = 0;
static unsigned int __thprac_lc_startupHeight = 0;
// UI scale the window was created with, in percent (for the "restart to apply"
// hint in the settings page).
static int __thprac_lc_usedUiScale = 100;

// Startup window size presets, in 96-DPI logical pixels. The settings combo
// lists them in this order; the index equal to the count means "custom".
struct LauncherSizePreset {
    int width, height;
};
static constexpr LauncherSizePreset __thprac_lc_sizePresets[] = {
    { 640, 480 }, { 800, 600 }, { 960, 720 }, { 1024, 768 }, { 1152, 864 }, { 1280, 960 }
};
static constexpr int __thprac_lc_sizePresetCount = (int)(sizeof(__thprac_lc_sizePresets) / sizeof(__thprac_lc_sizePresets[0]));
// 1280x960 is the design canvas, i.e. the behaviour before this setting existed.
static constexpr int __thprac_lc_sizePresetDefault = 5;

// Resolves a preset index to 96-DPI logical pixels. For any index outside the
// table ("custom") the incoming width/height are kept and clamped to the range
// the settings page offers, so a hand-edited config cannot produce a nonsense
// window.
static void LauncherWindowSizePreset(int index, int& width, int& height)
{
    if (index >= 0 && index < __thprac_lc_sizePresetCount) {
        width = __thprac_lc_sizePresets[index].width;
        height = __thprac_lc_sizePresets[index].height;
    } else {
        width = std::clamp(width, 640, 1280);
        height = std::clamp(height, 480, 960);
    }
}

static HANDLE __thprac_lc_mutex;
static MSG __thprac_lc_msg;
static WNDCLASSEX __thprac_lc_wc;
static LPDIRECT3D9 g_pD3D = nullptr;
LPDIRECT3DDEVICE9 g_pd3dDevice = nullptr;
static D3DPRESENT_PARAMETERS g_d3dpp = {};

// D3D Functions

bool D3DCreateDevice(HWND hWnd, unsigned int width, unsigned int height){
    // Create D3D object using LoadLibrary
    auto d3d9Lib = LoadLibraryW(L"d3d9.dll");
    if (!d3d9Lib)
        return false;
    decltype(Direct3DCreate9)* d3dCreate9 = (decltype(Direct3DCreate9)*)GetProcAddress(d3d9Lib, "Direct3DCreate9");
    if (!d3dCreate9 || (g_pD3D = d3dCreate9(D3D_SDK_VERSION)) == nullptr)
        return false;

    // Create the D3DDevice
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE; // Present with vsync
    g_d3dpp.BackBufferWidth = width;
    g_d3dpp.BackBufferHeight = height;
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) < 0)
        return false;

    return true;
}

// Title-bar brand mark: the real application icon, rendered into a 32bpp DIB
// and uploaded once as a D3D9 texture, so the launcher shows the same yin-yang
// as the exe instead of a hand-drawn stand-in.
static LPDIRECT3DTEXTURE9 g_launcherIconTex = nullptr;

void LauncherReleaseIconTexture()
{
    if (g_launcherIconTex) {
        g_launcherIconTex->Release();
        g_launcherIconTex = nullptr;
    }
}

void* LauncherGetIconTexture()
{
    if (g_launcherIconTex)
        return g_launcherIconTex;
    if (!g_pd3dDevice)
        return nullptr;

    constexpr int iconSize = 64;
    HICON icon = (HICON)LoadImageW(reinterpret_cast<HMODULE>(&__ImageBase), MAKEINTRESOURCEW(IDI_ICON1),
        IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR);
    if (!icon)
        return nullptr;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = iconSize;
    bmi.bmiHeader.biHeight = -iconSize;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dib) {
        DestroyIcon(icon);
        return nullptr;
    }
    std::memset(pixels, 0, static_cast<size_t>(iconSize) * iconSize * 4);

    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldBitmap = SelectObject(dc, dib);
    ::DrawIconEx(dc, 0, 0, icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    DestroyIcon(icon);

    LPDIRECT3DTEXTURE9 texture = nullptr;
    if (SUCCEEDED(g_pd3dDevice->CreateTexture(iconSize, iconSize, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))
        && texture) {
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(texture->LockRect(0, &locked, nullptr, 0))) {
            for (int y = 0; y < iconSize; ++y)
                std::memcpy(static_cast<char*>(locked.pBits) + y * locked.Pitch,
                    static_cast<char*>(pixels) + static_cast<size_t>(y) * iconSize * 4,
                    static_cast<size_t>(iconSize) * 4);
            texture->UnlockRect(0);
        }
        g_launcherIconTex = texture;
    }
    DeleteObject(dib);
    return g_launcherIconTex;
}

void* LauncherCreateTextureBGRA(const void* pixels, int width, int height, int pitch)
{
    if (!g_pd3dDevice || !pixels || width <= 0 || height <= 0 || pitch < width * 4)
        return nullptr;

    LPDIRECT3DTEXTURE9 texture = nullptr;
    if (FAILED(g_pd3dDevice->CreateTexture(width, height, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))
        || !texture)
        return nullptr;

    D3DLOCKED_RECT locked = {};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0))) {
        texture->Release();
        return nullptr;
    }
    for (int y = 0; y < height; ++y)
        std::memcpy(static_cast<char*>(locked.pBits) + y * locked.Pitch,
            static_cast<const char*>(pixels) + static_cast<size_t>(y) * pitch,
            static_cast<size_t>(width) * 4);
    texture->UnlockRect(0);
    return texture;
}

void LauncherReleaseTexture(void* texture)
{
    if (texture)
        static_cast<LPDIRECT3DTEXTURE9>(texture)->Release();
}

void D3DCleanupDevice()
{
    Gui::LauncherCoverShutdown();
    LauncherReleaseIconTexture();
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
    if (g_pD3D) {
        g_pD3D->Release();
        g_pD3D = nullptr;
    }
}

void D3DResetDevice()
{
    Gui::ImplDX9InvalidateDeviceObjects();
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    if (hr == D3DERR_INVALIDCALL)
        IM_ASSERT(0);
    Gui::ImplDX9CreateDeviceObjects();
}

// Win32 Functions

void ResizeWindow(HWND hwnd, ImVec2& wndPos, ImVec2& wndSize)
{
    RECT wndRect;
    ::GetWindowRect(hwnd, &wndRect);
    if ((LONG)wndSize.x != wndRect.right - wndRect.left || (LONG)wndSize.y != wndRect.bottom - wndRect.top) {
        RECT rect = { 0, 0, (LONG)wndSize.x, (LONG)wndSize.y };
        ::AdjustWindowRectEx(&rect, WS_POPUP, FALSE, WS_EX_APPWINDOW); // Client to Screen
        ::SetWindowPos(hwnd, nullptr,
            wndRect.left + (LONG)wndPos.x, wndRect.top + (LONG)wndPos.y,
            rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize.x = (float)wndSize.x;
        io.DisplaySize.y = (float)wndSize.y;

        g_d3dpp.BackBufferWidth = (LONG)wndSize.x;
        g_d3dpp.BackBufferHeight = (LONG)wndSize.y;
        D3DResetDevice();
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (Gui::ImplWin32WndProcHandlerW(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_GETMINMAXINFO: {
        LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
        lpMMI->ptMinTrackSize.x = 300;
        lpMMI->ptMinTrackSize.y = 300;
    }
        return 0;
    case WM_SIZE:
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_NCHITTEST:
        if (__thprac_lc_canMove)
            return HTCAPTION; // allows dragging of the window
        else
            break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProc(hWnd, msg, wParam, lParam);
}

bool WndMsgUpdate(HWND hWnd = nullptr)
{
    MSG msg;
    while (::PeekMessage(&msg, hWnd, 0U, 0U, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
        if (msg.message == WM_QUIT)
            return false;
    }
    return true;
}

// Launcher Functions
void GetDesktopResolution(int& horizontal, int& vertical)
{
    RECT desktop;
    // Get a handle to the desktop window
    const HWND hDesktop = GetDesktopWindow();
    // Get the size of screen to the variable desktop
    GetWindowRect(hDesktop, &desktop);
    // The top left corner will have coordinates (0,0)
    // and the bottom right corner will have coordinates
    // (horizontal, vertical)
    horizontal = desktop.right;
    vertical = desktop.bottom;
}

int LauncherWndInit(unsigned int width, unsigned int height, unsigned int maxWidth, unsigned int maxHeight, unsigned int widthCurrent, unsigned int heightCurrent)
{
    g_isLauncher = true;

    if (__thprac_lc_hasInited)
        return 0;

    __thprac_lc_mutex = CreateMutex(nullptr, TRUE, L"thprac launcher mutex");
    if (__thprac_lc_mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 3; ++i) {
            auto tmpTitle = utf8_to_utf16(th_glossary_str[i][THPRAC_LAUNCHER]);
            HWND existingApp = FindWindowW(0, tmpTitle.c_str());
            if (existingApp) {
                ShowWindow(existingApp, SW_RESTORE);
                SetForegroundWindow(existingApp);
            }
        }
        return 0;
    }

    // Create application window
    auto windowTitle = utf8_to_utf16(Gui::LocaleGetStr(THPRAC_LAUNCHER));
    auto icon = LoadIcon(reinterpret_cast<HMODULE>(&__ImageBase), MAKEINTRESOURCE(IDI_ICON1));
    __thprac_lc_wc = { sizeof(WNDCLASSEX), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), icon, nullptr, nullptr, nullptr, _T("thprac launcher window"), nullptr };
    ::RegisterClassEx(&__thprac_lc_wc);
    __thprac_lc_hwnd = ::CreateWindow(__thprac_lc_wc.lpszClassName, windowTitle.c_str(),
        WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, 0, 0, width, height,
        nullptr, nullptr, __thprac_lc_wc.hInstance, nullptr);

    // DPI handling
    DEVMODE devMod;
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &devMod);
    auto displayX = devMod.dmPelsWidth;
    auto displayY = devMod.dmPelsHeight;
    if (auto shcore = GetModuleHandleW(L"shcore.dll")) {
        auto setProcDpiAwareness = (PSetProcessDpiAwareness)GetProcAddress(shcore, "SetProcessDpiAwareness");
        auto getDpiForMonitor = (PGetDpiForMonitor)GetProcAddress(shcore, "GetDpiForMonitor");
        if (setProcDpiAwareness && getDpiForMonitor) {
            UINT dpiX;
            UINT dpiY;
            setProcDpiAwareness(1);
            getDpiForMonitor(MonitorFromWindow(__thprac_lc_hwnd, MONITOR_DEFAULTTONEAREST), 0, &dpiX, &dpiY);
            if (dpiX != 96) {
                __thprac_lc_dpiScale = (float)dpiX / 96.0f;
                width = (unsigned int)((float)width * __thprac_lc_dpiScale);
                height = (unsigned int)((float)height * __thprac_lc_dpiScale);
                maxWidth = (unsigned int)((float)maxWidth * __thprac_lc_dpiScale);
                maxHeight = (unsigned int)((float)maxHeight * __thprac_lc_dpiScale);
                widthCurrent = (unsigned int)((float)widthCurrent * __thprac_lc_dpiScale);
                heightCurrent = (unsigned int)((float)heightCurrent * __thprac_lc_dpiScale);
            }
        }
    }

    // Display settings (Launcher tab). They have to be known before the font
    // atlas is built and before the window is placed, which is why they are
    // read straight from the config instead of through the settings page.
    int sizePreset = __thprac_lc_sizePresetDefault;
    std::array<int, 2> customSize { 1280, 960 };
    int uiScalePercent = 100;
    LauncherSettingGet("launcher_window_size_type", sizePreset);
    LauncherSettingGet("launcher_window_size", customSize);
    LauncherSettingGet("launcher_ui_scale", uiScalePercent);
    uiScalePercent = std::clamp(uiScalePercent, 75, 150);
    __thprac_lc_uiScale = (float)uiScalePercent / 100.0f;
    __thprac_lc_usedUiScale = uiScalePercent;
    __thprac_lc_scale = __thprac_lc_dpiScale * __thprac_lc_uiScale;

    // A scaled-up UI needs more room to breathe, so the cap follows it; the
    // startup size itself stays in physical pixels.
    __thprac_lc_maxWidth = (unsigned int)((float)maxWidth * std::max(1.0f, __thprac_lc_uiScale));
    __thprac_lc_maxHeight = (unsigned int)((float)maxHeight * std::max(1.0f, __thprac_lc_uiScale));

    int startupWidth = customSize[0];
    int startupHeight = customSize[1];
    LauncherWindowSizePreset(sizePreset, startupWidth, startupHeight);
    __thprac_lc_startupWidth = std::clamp((unsigned int)((float)startupWidth * __thprac_lc_dpiScale),
        (unsigned int)(640.0f * __thprac_lc_dpiScale), __thprac_lc_maxWidth);
    __thprac_lc_startupHeight = std::clamp((unsigned int)((float)startupHeight * __thprac_lc_dpiScale),
        (unsigned int)(480.0f * __thprac_lc_dpiScale), __thprac_lc_maxHeight);
    // Open at the size picked in the settings, centred inside the work area.
    // Only the position is fitted to the work area, so the title bar and the
    // bottom-right grip stay reachable even if the window is bigger than the
    // screen.
    widthCurrent = __thprac_lc_startupWidth;
    heightCurrent = __thprac_lc_startupHeight;
    auto wndPosX = (LONG)(((LONG)displayX - (LONG)widthCurrent) / 2);
    auto wndPosY = (LONG)(((LONG)displayY - (LONG)heightCurrent) / 2);
    RECT workArea = {};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        const auto workWidth = workArea.right - workArea.left;
        const auto workHeight = workArea.bottom - workArea.top;
        wndPosX = workArea.left + (workWidth - (LONG)widthCurrent) / 2;
        wndPosY = workArea.top + (workHeight - (LONG)heightCurrent) / 2;
        if (wndPosX < workArea.left)
            wndPosX = workArea.left;
        if (wndPosY < workArea.top)
            wndPosY = workArea.top;
    }
    SetWindowPos(__thprac_lc_hwnd, 0, wndPosX, wndPosY, widthCurrent, heightCurrent, 0);

    // Initialize Direct3D
    if (!D3DCreateDevice(__thprac_lc_hwnd, __thprac_lc_maxWidth, __thprac_lc_maxHeight)) {
        D3DCleanupDevice();
        ::UnregisterClass(__thprac_lc_wc.lpszClassName, __thprac_lc_wc.hInstance);
        return -1;
    }

    // Show the window
    ::ShowWindow(__thprac_lc_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(__thprac_lc_hwnd);
    MovWndToTop(__thprac_lc_hwnd);

    ImGui::CreateContext();
    Gui::ImplWin32Init(__thprac_lc_hwnd);
    Gui::ImplDX9Init(g_pd3dDevice);
    Gui::LocaleCreateMergeFont(Gui::LocaleGet(), 20.0f * __thprac_lc_scale); //30.0f LOCALE_ZH_CN LOCALE_EN_US LOCALE_JA_JP
    if (__thprac_lc_scale != 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(__thprac_lc_scale);
    }

    ImGuiIO& io = ImGui::GetIO();
    // The jade style derives every metric from the font size, so it already
    // carries the DPI and UI scale; nothing is overridden afterwards.
    Gui::StyleColorsLightJade();
    io.IniFilename = nullptr;
    io.ImeWindowHandle = __thprac_lc_hwnd;
    io.DisplaySize = ImVec2((float)__thprac_lc_startupWidth, (float)__thprac_lc_startupHeight);
    io.KeyMap[ImGuiKey_Tab] = VK_TAB;
    io.KeyMap[ImGuiKey_LeftArrow] = VK_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = VK_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = VK_UP;
    io.KeyMap[ImGuiKey_DownArrow] = VK_DOWN;
    io.KeyMap[ImGuiKey_PageUp] = VK_PRIOR;
    io.KeyMap[ImGuiKey_PageDown] = VK_NEXT;
    io.KeyMap[ImGuiKey_Home] = VK_HOME;
    io.KeyMap[ImGuiKey_End] = VK_END;
    io.KeyMap[ImGuiKey_Insert] = VK_INSERT;
    io.KeyMap[ImGuiKey_Delete] = VK_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = VK_BACK;
    io.KeyMap[ImGuiKey_Space] = VK_SPACE;
    io.KeyMap[ImGuiKey_Enter] = VK_RETURN;
    io.KeyMap[ImGuiKey_Escape] = VK_ESCAPE;
    io.KeyMap[ImGuiKey_KeyPadEnter] = VK_RETURN;
    io.KeyMap[ImGuiKey_A] = 'A';
    io.KeyMap[ImGuiKey_C] = 'C';
    io.KeyMap[ImGuiKey_V] = 'V';
    io.KeyMap[ImGuiKey_X] = 'X';
    io.KeyMap[ImGuiKey_Y] = 'Y';
    io.KeyMap[ImGuiKey_Z] = 'Z';

    return 1;
}

bool LauncherWndNewFrame()
{
    if (!WndMsgUpdate())
        return false;

    while (IsIconic(__thprac_lc_hwnd)) {
        HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
        // Handle loss of D3D9 device
        if (result == D3DERR_DEVICELOST && g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
            D3DResetDevice();
        if (!WndMsgUpdate())
            return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.KeyCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    io.KeyShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    io.KeyAlt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    io.KeySuper = false;

    //ResizeWindow(__thprac_lc_hwnd, wndPos, wndSize);
    Gui::ImplDX9NewFrame();
    Gui::ImplWin32NewFrame(false);
    ImGui::NewFrame();

    return true;
}

bool LauncherWndEndFrame(ImVec2& wndPos, ImVec2& wndSize, bool canMove)
{
    __thprac_lc_canMove = canMove;
    static bool moved = false;

    RECT wndRect;
    ::GetWindowRect(__thprac_lc_hwnd, &wndRect);
    if ((LONG)wndSize.x != wndRect.right - wndRect.left || (LONG)wndSize.y != wndRect.bottom - wndRect.top) {
        moved = true;
        RECT rect = { 0, 0, (LONG)wndSize.x, (LONG)wndSize.y };
        ::AdjustWindowRectEx(&rect, WS_POPUP, FALSE, WS_EX_APPWINDOW); // Client to Screen
        ::SetWindowPos(__thprac_lc_hwnd, nullptr,
            wndRect.left + (LONG)wndPos.x, wndRect.top + (LONG)wndPos.y,
            rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ::GetClientRect(__thprac_lc_hwnd, &wndRect);
    RECT renderRect = { (LONG)wndPos.x, (LONG)wndPos.y,
        (LONG)(wndSize.x) + (LONG)(wndPos.x), (LONG)(wndSize.y) + (LONG)(wndPos.y) };

    ImGui::EndFrame();

    // Rendering
    g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, false);
    g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, false);
    g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, false);

    // Icon-green tinted backdrop; the stock value was a blue-grey (0.45, 0.55, 0.60).
    ImVec4 clear_color = ImVec4(0.42f, 0.55f, 0.47f, 1.00f);
    D3DCOLOR clear_col_dx = D3DCOLOR_RGBA(
        (int)(clear_color.x * 255.0f), (int)(clear_color.y * 255.0f),
        (int)(clear_color.z * 255.0f), (int)(clear_color.w * 255.0f));
    g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear_col_dx, 1.0f, 0);

    if (g_pd3dDevice->BeginScene() >= 0) {
        ImGui::Render();
        Gui::ImplDX9RenderDrawData(ImGui::GetDrawData());
        g_pd3dDevice->EndScene();
    }

    HRESULT result = g_pd3dDevice->Present(&renderRect, nullptr, nullptr, nullptr);
    // Handle loss of D3D9 device
    if (result == D3DERR_DEVICELOST && g_pd3dDevice->TestCooperativeLevel() == D3DERR_DEVICENOTRESET)
        D3DResetDevice();

    if (moved) {
        if (!(GetKeyState(VK_LBUTTON) < 0)) {
            moved = false;
        }
    }
    ImGui::GetIO().DisplaySize = moved ? ImVec2(5000.0f, 5000.0f) : ImVec2(wndSize.x, wndSize.y);

    return true;
}

bool LauncherWndMinimize()
{
    return ShowWindow(__thprac_lc_hwnd, SW_MINIMIZE);
}

bool LauncherWndShutdown()
{
    Gui::ImplDX9Shutdown();
    Gui::ImplWin32Shutdown();
    ImGui::DestroyContext();

    D3DCleanupDevice();
    ::DestroyWindow(__thprac_lc_hwnd);
    ::UnregisterClass(__thprac_lc_wc.lpszClassName, __thprac_lc_wc.hInstance);
    ::CloseHandle(__thprac_lc_mutex);

    return true;
}

ImVec2 LauncherWndGetSize()
{
    RECT wndRect {};
    ::GetWindowRect(__thprac_lc_hwnd, &wndRect);
    return ImVec2((float)(wndRect.right - wndRect.left), (float)(wndRect.bottom - wndRect.top));
}

ImVec2 LauncherWndGetMaxSize()
{
    return ImVec2((float)__thprac_lc_maxWidth, (float)__thprac_lc_maxHeight);
}

ImVec2 LauncherWndGetStartupSize()
{
    return ImVec2((float)__thprac_lc_startupWidth, (float)__thprac_lc_startupHeight);
}

float LauncherWndGetDpiScale()
{
    return __thprac_lc_dpiScale;
}

int LauncherWindowSizePresetCount()
{
    return __thprac_lc_sizePresetCount;
}

bool LauncherWndDisplaySettingsPending()
{
    int sizePreset = __thprac_lc_sizePresetDefault;
    std::array<int, 2> customSize { 1280, 960 };
    int uiScalePercent = 100;
    LauncherSettingGet("launcher_window_size_type", sizePreset);
    LauncherSettingGet("launcher_window_size", customSize);
    LauncherSettingGet("launcher_ui_scale", uiScalePercent);
    if (uiScalePercent != __thprac_lc_usedUiScale)
        return true;

    int width = customSize[0];
    int height = customSize[1];
    LauncherWindowSizePreset(sizePreset, width, height);
    auto wantWidth = std::clamp((unsigned int)((float)width * __thprac_lc_dpiScale),
        (unsigned int)(640.0f * __thprac_lc_dpiScale), __thprac_lc_maxWidth);
    auto wantHeight = std::clamp((unsigned int)((float)height * __thprac_lc_dpiScale),
        (unsigned int)(480.0f * __thprac_lc_dpiScale), __thprac_lc_maxHeight);
    return wantWidth != __thprac_lc_startupWidth || wantHeight != __thprac_lc_startupHeight;
}

float LauncherWndGetScale()
{
    return __thprac_lc_scale;
}

// From thcrap
// https://github.com/thpatch/thcrap/blob/33894bbac0def84d4f6d89aee7735716d1543b52/thcrap_configure/src/configure_search.cpp#L56

// Work around a bug in Windows 7 and later by sending
// BFFM_SETSELECTION a second time.
// https://connect.microsoft.com/VisualStudio/feedback/details/518103/bffm-setselection-does-not-work-with-shbrowseforfolder-on-windows-7
typedef struct {
    ITEMIDLIST* path;
    int attempts;
} initial_path_t;

int CALLBACK SetInitialBrowsePathProc(HWND hWnd, UINT uMsg, [[maybe_unused]] LPARAM lp, LPARAM pData)
{
    initial_path_t* ip = (initial_path_t*)pData;
    if (ip) {
        switch (uMsg) {
        case BFFM_INITIALIZED:
            ip->attempts = 0;
            [[fallthrough]];
        case BFFM_SELCHANGED:
            if (ip->attempts < 2) {
                SendMessageW(hWnd, BFFM_SETSELECTION, FALSE, (LPARAM)ip->path);
                ip->attempts++;
            }
        }
    }
    return 0;
}

template <typename T>
struct ComRAII {
    T* p;

    operator bool() const { return !!p; }
    operator T*() const { return p; }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    T& operator*() const { return *p; }

    ComRAII()
        : p(nullptr)
    {
    }
    explicit ComRAII(T* p)
        : p(p)
    {
    }
    ComRAII(const ComRAII<T>& other) = delete;
    ComRAII<T>& operator=(const ComRAII<T>& other) = delete;
    ~ComRAII()
    {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }
};

static int SelectFolderVista(HWND owner, PIDLIST_ABSOLUTE initial_path, PIDLIST_ABSOLUTE& pidl, const wchar_t* window_title)
{
    // Those two functions are absent in XP, so we have to load them dynamically
    HMODULE shell32 = LoadLibraryW(L"Shell32.dll");
    if (!shell32)
        return -1;
    auto pSHCreateItemFromIDList = (HRESULT(WINAPI*)(PCIDLIST_ABSOLUTE, REFIID, void**))GetProcAddress(shell32, "SHCreateItemFromIDList");
    auto pSHGetIDListFromObject = (HRESULT(WINAPI*)(IUnknown*, PIDLIST_ABSOLUTE*))GetProcAddress(shell32, "SHGetIDListFromObject");
    if (!pSHCreateItemFromIDList || !pSHGetIDListFromObject)
        return -1;

    ComRAII<IFileDialog> pfd;
    if(FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
        return -1;
    if (!pfd)
        return -1;

    {
        ComRAII<IShellItem> psi;
        pSHCreateItemFromIDList(initial_path, IID_PPV_ARGS(&psi));
        if (!psi)
            return -1;
        pfd->SetDefaultFolder(psi);
    }

    pfd->SetOptions(
        FOS_NOCHANGEDIR | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM
        | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT);
    pfd->SetTitle(window_title);
    HRESULT hr = pfd->Show(owner);
    ComRAII<IShellItem> psi;
    if (SUCCEEDED(hr) && SUCCEEDED(pfd->GetResult(&psi))) {
        pSHGetIDListFromObject(psi, &pidl);
        return 0;
    }

    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return 0;
    return -1;
}

static int SelectFolderXP(HWND owner, PIDLIST_ABSOLUTE initial_path, PIDLIST_ABSOLUTE& pidl, const wchar_t* window_title)
{
    BROWSEINFOW bi = { 0 };
    initial_path_t ip = { 0 };
    ip.path = initial_path;

    bi.lpszTitle = window_title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NONEWFOLDERBUTTON | BIF_USENEWUI;
    bi.hwndOwner = owner;
    bi.lpfn = SetInitialBrowsePathProc;
    bi.lParam = (LPARAM)&ip;
    pidl = SHBrowseForFolderW(&bi);
    return 0;
}

std::wstring LauncherWndFolderSelect(const wchar_t* title) {
    if (FAILED(CoInitialize(nullptr)))
        return L"";
    defer(CoUninitialize());

    PIDLIST_ABSOLUTE initial_path = nullptr;
    wchar_t path[MAX_PATH] = {};

    GetCurrentDirectoryW(MAX_PATH, path);
    SHParseDisplayName(path, nullptr, &initial_path, 0, nullptr);

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (-1 == SelectFolderVista(__thprac_lc_hwnd, initial_path, pidl, title)) {
        SelectFolderXP(__thprac_lc_hwnd, initial_path, pidl, title);
    }

    if (pidl) {
        defer(CoTaskMemFree(pidl));
        if (SHGetPathFromIDListW(pidl, path)) {
            return path;
        }
    }
    return L"";
}

std::wstring LauncherWndFileSelect([[maybe_unused]] const wchar_t* title, const wchar_t* filter) {
    OPENFILENAME ofn = {};
    wchar_t szFile[MAX_PATH] = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = __thprac_lc_hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NODEREFERENCELINKS;
    GetOpenFileName(&ofn);
    return std::wstring(szFile);
}

int LauncherWndMsgBox(const wchar_t* title, const wchar_t* text, int flag)
{
    return MessageBoxW(__thprac_lc_hwnd, text, title, flag);
}
}
