#pragma once

struct ImDrawList;
struct ThpracPluginUi;

namespace THPrac {
int RunLauncherWithExtensions();
int ListLauncherExtensions();
void __cdecl RefreshLauncherPlugins();
int __cdecl RenderLauncherPluginSettings(const wchar_t* path, const ThpracPluginUi* ui);
bool TryExtensionGameInit(const char* gameId, unsigned timestamp, unsigned textSize);
void DrawLauncherExtensionTools();
void DrawGameExtensions(ImDrawList* draw, int game, bool gameInfoVisible);
}
