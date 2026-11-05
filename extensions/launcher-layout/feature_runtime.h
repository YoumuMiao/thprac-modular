#pragma once
#include "thprac_feature_services.h"
#include "thprac_plugin_sdk.h"

struct ImDrawList;
struct ThpracRepairService;

namespace THPrac {
struct RouteService;
const RouteService* RouteAddon();
const ThpracRepairService* RepairAddon();

enum class Addon { Capture, Route, Volume, Settings, Repairs, Th15, Hud };
const wchar_t* AdapterInstallDirectory();
void ConfigureAddons(const ThpracGameHost& host, const char* game);
bool AddonEnabled(Addon feature);
const ThpracHudService* HudAddon();
const ThpracVolumeService* VolumeAddon();
const ThpracSettingsService* SettingsAddon();
void DrawAdapterPluginFrame(ImDrawList* draw, void* window);
void NotifyAdapterPluginWindowClosed(void* window);
void ReportPluginStage(const ThpracPluginStage* stage);
inline void ReportPluginStage(const ThpracPluginStage& stage) { ReportPluginStage(&stage); }
void DispatchAddonFrame(ImDrawList* draw, int game, bool visible);
}
