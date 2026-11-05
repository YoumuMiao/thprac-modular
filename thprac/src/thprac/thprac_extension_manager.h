#pragma once

struct ThpracPluginUi;

namespace THPrac {
void DrawExtensionManager(int locale, bool entering, void (__cdecl *refreshLauncherPlugins)(),
    int (__cdecl *drawSettings)(const wchar_t*, const ThpracPluginUi*));
}
