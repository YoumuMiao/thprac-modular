#pragma once
#include "thprac_plugin_sdk.h"
#include <vector>

struct ImDrawList;

namespace THPrac {
const ThpracPluginCallbacks* PluginCallbacks(const ThpracExtension& extension);
ThpracPluginConfig MakePluginConfig(const char* id);
void SetPluginGame(const char* gameId, unsigned timestamp, unsigned textSize,
    const std::vector<const ThpracExtension*>& extensions);
void NotifyPluginGameReady();
void __cdecl DispatchPluginEvent(const ThpracPluginGameEvent* event);
void DrawPluginFrame(ImDrawList* draw, void* window);
void NotifyPluginWindowClosed(void* window);
void ReportPluginStage(const ThpracPluginStage* stage);
inline void ReportPluginStage(const ThpracPluginStage& stage) { ReportPluginStage(&stage); }
}
