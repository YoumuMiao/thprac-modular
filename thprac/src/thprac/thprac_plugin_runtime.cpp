#include "thprac_plugin_runtime.h"
#include "thprac_plugin_stage_tracker.h"
#include "thprac_extension_inventory.h"
#include <Windows.h>
#include <imgui.h>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace THPrac {
void** FindImportAddress(HANDLE module, LPCSTR dll, LPCSTR symbol);

namespace {
struct Subscriber {
    const char* id;
    const ThpracPluginCallbacks* callbacks;
};
std::vector<Subscriber> subscribers;
ThpracPluginGameEvent game {};
PluginStageTracker stageTracker;
std::atomic<bool> ready {false};

bool IsConfigName(const char* name)
{
    if (!name || !*name) return false;
    for (; *name; ++name) {
        const char c = *name;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}
int __cdecl ReadConfig(const char* id, const char* key, int fallback)
{
    if (!IsConfigName(id) || !IsConfigName(key)) return fallback;
    const auto directory = ExtensionDirectory(&__ImageBase);
    if (directory.empty()) return fallback;
    const auto path = directory / L"settings.ini";
    return static_cast<int>(GetPrivateProfileIntW(std::wstring(id, id + strlen(id)).c_str(),
        std::wstring(key, key + strlen(key)).c_str(), fallback, path.c_str()));
}
int __cdecl WriteConfig(const char* id, const char* key, int value)
{
    if (!IsConfigName(id) || !IsConfigName(key)) return 0;
    const auto directory = ExtensionDirectory(&__ImageBase);
    if (directory.empty()) return 0;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return 0;
    const auto path = directory / L"settings.ini";
    return WritePrivateProfileStringW(std::wstring(id, id + strlen(id)).c_str(),
        std::wstring(key, key + strlen(key)).c_str(), std::to_wstring(value).c_str(), path.c_str()) != FALSE;
}

int __cdecl HookGameImport(const char* dll, const char* symbol, void* replacement, void** original)
{
    if (!dll || !symbol || !replacement || !original) return 0;
    const auto imported = GetModuleHandleA(dll);
    const auto expected = imported ? reinterpret_cast<void*>(GetProcAddress(imported, symbol)) : nullptr;
    if (!expected) return 0;
    const auto target = GetModuleHandleW(nullptr);
    const auto image = reinterpret_cast<uintptr_t>(target);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
    if (!pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) return 0;
    auto** slot = FindImportAddress(target, dll, symbol);
    if (!slot || *slot != expected) return 0;

    DWORD protection;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection)) return 0;
    // Publish the original before another thread can enter the replacement.
    *original = expected;
    const bool installed = InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
    DWORD previous;
    VirtualProtect(slot, sizeof(*slot), protection, &previous);
    if (!installed) *original = nullptr;
    return installed;
}

void __cdecl DrawSdkText(void* drawing, float x, float y, float width, const char* text)
{
    auto* draw = static_cast<ImDrawList*>(drawing);
    const auto height = ImGui::GetTextLineHeight();
    draw->AddRectFilled({x, y}, {x + width, y + height}, 0xE6222222);
    draw->PushClipRect({x, y}, {x + width, y + height}, true);
    draw->AddText({x + 3.0f, y}, 0xFFFFFFFF, text);
    draw->PopClipRect();
}

bool Supports(const ThpracPluginCallbacks& callbacks, const char* id)
{
    if (!callbacks.games) return true;
    for (auto game = callbacks.games; *game; ++game)
        if (strcmp(*game, id) == 0) return true;
    return false;
}

void Emit(const ThpracPluginGameEvent& event)
{
    for (const auto& subscriber : subscribers) {
        auto message = event;
        message.config = MakePluginConfig(subscriber.id);
        subscriber.callbacks->game_event(&message);
    }
}
}

const ThpracPluginCallbacks* PluginCallbacks(const ThpracExtension& extension)
{
    if (extension.size < sizeof(ThpracExtension) || !extension.plugin) return nullptr;
    const auto* plugin = extension.plugin;
    return plugin->size >= sizeof(*plugin) && plugin->version == THPRAC_PLUGIN_SDK_VERSION ? plugin : nullptr;
}

ThpracPluginConfig MakePluginConfig(const char* id)
{
    return {id, ReadConfig, WriteConfig};
}

void SetPluginGame(const char* gameId, unsigned timestamp, unsigned textSize,
    const std::vector<const ThpracExtension*>& extensions)
{
    game = {};
    game.size = sizeof(game);
    game.game_id = gameId;
    game.exe_timestamp = timestamp;
    game.exe_text_size = textSize;
    for (const auto* extension : extensions) {
        const auto* callbacks = PluginCallbacks(*extension);
        if (callbacks && callbacks->game_event && Supports(*callbacks, gameId))
            subscribers.push_back({extension->id, callbacks});
    }
}

void NotifyPluginGameReady()
{
    auto event = game;
    event.kind = THPRAC_EVENT_GAME_READY;
    event.hook_import = HookGameImport;
    Emit(event);
    ready.store(true, std::memory_order_release);
}

void __cdecl DispatchPluginEvent(const ThpracPluginGameEvent* event)
{
    if (!ready.load(std::memory_order_acquire) || subscribers.empty()) return;
    if (event->kind == THPRAC_EVENT_STAGE_STARTED && !stageTracker.Update(event->stage)) return;
    auto message = game;
    message.kind = event->kind;
    message.window = event->window;
    message.overlay = event->overlay;
    message.stage = event->stage;
    Emit(message);
}

void DrawPluginFrame(ImDrawList* draw, void* window)
{
    const ThpracPluginOverlay overlay {draw, DrawSdkText};
    ThpracPluginGameEvent event {};
    event.kind = THPRAC_EVENT_FRAME;
    event.window = window;
    event.overlay = &overlay;
    DispatchPluginEvent(&event);
}

void NotifyPluginWindowClosed(void* window)
{
    ThpracPluginGameEvent event {};
    event.kind = THPRAC_EVENT_WINDOW_CLOSED;
    event.window = window;
    DispatchPluginEvent(&event);
}

void ReportPluginStage(const ThpracPluginStage* stage)
{
    ThpracPluginGameEvent event {};
    event.kind = THPRAC_EVENT_STAGE_STARTED;
    event.stage = stage;
    DispatchPluginEvent(&event);
}
}
