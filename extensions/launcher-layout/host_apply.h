#pragma once
#include "thprac_extension.h"

namespace THPrac {
void DrawExtensionTools();
bool HasPluginManager();
void __cdecl RefreshPluginTools();
int __cdecl DrawPluginSettings(const wchar_t* path, const ThpracPluginUi* ui);
const wchar_t* LauncherInstallDirectory();
void SetLauncherExtensionHost(const ThpracLauncherHost* host);
}
