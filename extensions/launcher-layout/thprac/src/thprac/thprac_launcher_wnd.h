#pragma once
#include <imgui.h>
#include <string>

namespace THPrac {
int LauncherWndInit(unsigned int width, unsigned int height, unsigned int maxWidth, unsigned int maxHeight, unsigned int widthCurrent, unsigned int heightCurrent);
bool LauncherWndNewFrame();
bool LauncherWndEndFrame(ImVec2& wndPos, ImVec2& wndSize, bool canMove = false);
bool LauncherWndMinimize();
bool LauncherWndShutdown();
ImVec2 LauncherWndGetSize();
// Largest size the window can be dragged to, in physical pixels. This is the
// cap ImGui's resize grip is clamped to.
ImVec2 LauncherWndGetMaxSize();
// Size the window was created at, in physical pixels (the startup-resolution
// setting scaled by the monitor DPI).
ImVec2 LauncherWndGetStartupSize();
float LauncherWndGetScale();
// Monitor DPI factor alone; LauncherWndGetScale additionally carries the UI
// scale setting.
float LauncherWndGetDpiScale();
// Number of startup-size presets. The preset index equal to this count means
// "custom", i.e. launcher_window_size is used instead of a preset.
int LauncherWindowSizePresetCount();
// True when the window size or UI scale in the config no longer matches the
// values the window was created with, i.e. a restart is needed to apply them.
bool LauncherWndDisplaySettingsPending();
// Application icon (IDI_ICON1) uploaded as a D3D9 texture for the title bar.
// Returns an ImTextureID-compatible handle, or nullptr if it could not be made.
void* LauncherGetIconTexture();
// Uploads a top-down 32bpp BGRA buffer (stride = width * 4 * n) as a managed
// D3D9 texture. Returns an ImTextureID-compatible handle or nullptr.
void* LauncherCreateTextureBGRA(const void* pixels, int width, int height, int pitch);
// Releases a texture made by LauncherCreateTextureBGRA / LauncherGetIconTexture.
void LauncherReleaseTexture(void* texture);
std::wstring LauncherWndFolderSelect(const wchar_t* title = L"Browse for folder...");
std::wstring LauncherWndFileSelect(const wchar_t* title = L"Browse for file...", const wchar_t* filter = nullptr);
int LauncherWndMsgBox(const wchar_t* title, const wchar_t* text, int flag);
}
