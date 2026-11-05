#include <imgui.h>
#include "feature_runtime.h"
#include "capture_service.h"
#include "route_service.h"
#include "repair_service.h"
#include "thprac_games.h"
#include <Windows.h>
#include <array>

namespace THPrac {
namespace BossCapture { void SetCaptureService(const void* raw); }
extern DWORD* g_gameGuiHwnd;
namespace {
const ThpracGameHost* gameHost;
std::array<bool, 7> addons {};
const ThpracVolumeService* volume;
const ThpracSettingsService* settings;
const RouteService* route;
const ThpracRepairService* repairs;
const ThpracHudService* hud;

int __cdecl ReadMemory(uintptr_t address, void* output, uint32_t bytes)
{
    SIZE_T read = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), output, bytes, &read)
        && read == bytes;
}

void __cdecl DrawText(void* drawing, float x, float y, float width, const char* text)
{
    auto* draw = static_cast<ImDrawList*>(drawing);
    const auto size = ImGui::CalcTextSize(text);
    const auto height = ImGui::GetTextLineHeight();
    const float scale = size.x > width - 4.0f ? (width - 4.0f) / size.x : 1.0f;
    draw->AddRectFilled({x, y}, {x + width, y + height}, 0xFFFFFFFF);
    draw->PushClipRect({x, y}, {x + width, y + height}, true);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale,
        {x + 2.0f, y + (height - size.y * scale) * 0.5f}, 0xFF000000, text);
    draw->PopClipRect();
}
}

void ConfigureAddons(const ThpracGameHost& host, const char* game)
{
    gameHost = &host;
    const auto find = [&](const char* id) { return host.find_extension(id); };
    const auto supports = [&](const char* id) {
        const auto* plugin = find(id);
        const auto* service = plugin ? static_cast<const ThpracFeatureService*>(plugin->service) : nullptr;
        return service && service->size >= sizeof(*service) && service->supports_game && service->supports_game(game);
    };
    if (const auto* plugin = find("rueee-c7.volume")) {
        const auto* service = static_cast<const ThpracVolumeService*>(plugin->service);
        if (service && service->size >= sizeof(*service) && service->begin && service->poll)
            volume = service;
    }
    if (const auto* plugin = find("rueee-c7.settings")) {
        const auto* service = static_cast<const ThpracSettingsService*>(plugin->service);
        if (service && service->size >= sizeof(*service) && service->draw)
            settings = service;
    }
    if (const auto* plugin = find("rueee-c7.capture"))
        BossCapture::SetCaptureService(plugin->service);
    if (const auto* plugin = find("rueee-c7.route")) {
        const auto* service = static_cast<const RouteService*>(plugin->service);
        if (service && service->feature.size >= sizeof(*service) && service->align) route = service;
    }
    if (const auto* plugin = find("rueee-c7.repairs")) {
        const auto* service = static_cast<const ThpracRepairService*>(plugin->service);
        if (service && service->feature.size >= sizeof(*service) && service->clip_dx8) repairs = service;
    }
    if (const auto* plugin = find("rueee-c7.status-hud")) {
        const auto* service = static_cast<const ThpracHudService*>(plugin->service);
        if (service && service->size >= sizeof(*service) && service->fields && service->set_fields) hud = service;
    }
    addons = { supports("rueee-c7.capture"), route && supports("rueee-c7.route"), volume != nullptr, settings != nullptr,
        repairs && supports("rueee-c7.repairs"), supports("rueee-c7.th15-practice"), hud != nullptr };
}

const wchar_t* AdapterInstallDirectory()
{
    return gameHost && gameHost->size >= sizeof(ThpracGameHost)
        ? gameHost->install_directory : nullptr;
}

bool AddonEnabled(Addon feature)
{
    return addons[static_cast<size_t>(feature)];
}

const ThpracRepairService* RepairAddon() { return AddonEnabled(Addon::Repairs) ? repairs : nullptr; }
const RouteService* RouteAddon() { return route; }
const ThpracHudService* HudAddon() { return hud; }
const ThpracVolumeService* VolumeAddon() { return volume; }
const ThpracSettingsService* SettingsAddon() { return settings; }

namespace {
void EmitPluginEvent(const ThpracPluginGameEvent& event)
{
    if (gameHost && gameHost->size >= offsetof(ThpracGameHost, install_directory) && gameHost->dispatch_plugin_event)
        gameHost->dispatch_plugin_event(&event);
}
}

void DrawAdapterPluginFrame(ImDrawList* draw, void* window)
{
    const ThpracPluginOverlay overlay {draw, DrawText};
    ThpracPluginGameEvent event {};
    event.kind = THPRAC_EVENT_FRAME;
    event.window = window;
    event.overlay = &overlay;
    EmitPluginEvent(event);
}

void NotifyAdapterPluginWindowClosed(void* window)
{
    ThpracPluginGameEvent event {};
    event.kind = THPRAC_EVENT_WINDOW_CLOSED;
    event.window = window;
    EmitPluginEvent(event);
}

void ReportPluginStage(const ThpracPluginStage* stage)
{
    ThpracPluginGameEvent event {};
    event.kind = THPRAC_EVENT_STAGE_STARTED;
    event.stage = stage;
    EmitPluginEvent(event);
}

void DispatchAddonFrame(ImDrawList* draw, int game, bool visible)
{
    if (!gameHost) return;
    const ThpracGameFrame frame { sizeof(frame), game, visible ? 1u : 0u,
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),
        g_gameGuiHwnd ? reinterpret_cast<void*>(*g_gameGuiHwnd) : nullptr, draw, ReadMemory, DrawText };
    gameHost->dispatch_frame(&frame);
}
}
