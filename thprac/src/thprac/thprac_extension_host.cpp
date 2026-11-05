#include "thprac_extension_host.h"
#include "thprac_plugin_runtime.h"
#include "thprac_extension_inventory.h"
#include "thprac_extension.h"
#include "thprac_launcher_main.h"
#include "thprac_load_exe.h"
#include <Windows.h>
#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace THPrac {
extern DWORD* g_gameGuiHwnd;

namespace {
namespace fs = std::filesystem;

const ThpracExtension* LoadExtension(const fs::path& path, bool game)
{
    // Successfully loaded DLLs stay resident for the process lifetime. A tool
    // can be disabled without invalidating its callbacks or any work it owns.
    struct Loaded { fs::path path; const ThpracExtension* descriptor; };
    static std::vector<Loaded> loaded;
    for (const auto& item : loaded) if (item.path == path) return item.descriptor;

    const auto module = LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        OutputDebugStringW((L"thprac: failed to load " + path.wstring() + L"\n").c_str());
        return nullptr;
    }
    const auto query = reinterpret_cast<ThpracExtensionQueryFn>(GetProcAddress(module, THPRAC_EXTENSION_QUERY));
    const ThpracExtension* extension = nullptr;
    try {
        if (query) extension = query(THPRAC_EXTENSION_ABI);
    } catch (...) {
        OutputDebugStringW(L"thprac: extension query failed\n");
    }
    if (!extension || extension->size < offsetof(ThpracExtension, plugin) || !extension->id || !*extension->id) {
        FreeLibrary(module);
        return nullptr;
    }
    const auto* plugin = PluginCallbacks(*extension);
    const bool gameEntry = extension->on_game_frame || extension->replace_game_init || extension->start_game_adapter
        || extension->service || (plugin && (plugin->game_event || plugin->draw_settings));
    const bool launcherEntry = extension->run_launcher || extension->launcher_action || (plugin && plugin->draw_settings);
    if (!(game ? gameEntry : launcherEntry)) {
        FreeLibrary(module);
        return nullptr;
    }
    loaded.push_back({path, extension});
    return extension;
}

std::vector<const ThpracExtension*> DiscoverExtensions(bool game)
{
    std::vector<const ThpracExtension*> extensions;
    for (const auto& file : InstalledExtensions(ExtensionDirectory(&__ImageBase), GlobalExtensionDirectory())) {
        if (!file.enabled || file.scope == (game ? ExtensionScope::Launcher : ExtensionScope::Game)) continue;
        const auto* extension = LoadExtension(file.path, game);
        if (!extension || std::any_of(extensions.begin(), extensions.end(), [extension](const auto* registered) {
                return strcmp(registered->id, extension->id) == 0;
            })) continue;
        extensions.push_back(extension);
    }
    return extensions;
}

std::vector<const ThpracExtension*>& LauncherExtensions()
{
    static auto extensions = DiscoverExtensions(false);
    return extensions;
}

int __cdecl ApplyFromExtension(void* process, uintptr_t base)
{
    return WriteTHPracSig(process, base) && LoadSelf(process);
}

int __cdecl ReadGameMemory(uintptr_t address, void* buffer, uint32_t bytes)
{
    SIZE_T read = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), buffer, bytes, &read)
        && read == bytes;
}

void __cdecl DrawTextBox(void* drawing, float x, float y, float width, const char* text)
{
    auto* draw = static_cast<ImDrawList*>(drawing);
    const auto size = ImGui::CalcTextSize(text);
    const float rowHeight = ImGui::GetTextLineHeight();
    const float scale = size.x > width - 4.0f ? (width - 4.0f) / size.x : 1.0f;
    draw->AddRectFilled({ x, y }, { x + width, y + rowHeight }, 0xFFFFFFFF);
    draw->PushClipRect({ x, y }, { x + width, y + rowHeight }, true);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale,
        { x + 2.0f, y + (rowHeight - size.y * scale) * 0.5f }, 0xFF000000, text);
    draw->PopClipRect();
}
}

static void __cdecl RenderToolActions(int (__cdecl *button)(const char*, const char*), void* owner)
{
    for (const auto* extension : LauncherExtensions()) {
        if (!extension->launcher_action || !extension->launcher_action_title) continue;
        if (button(extension->id, extension->launcher_action_title)) extension->launcher_action(owner);
    }
}

void DrawLauncherExtensionTools()
{
    RenderToolActions([](const char* id, const char* label) -> int {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() * 0.91f);
        ImGui::PushID(id);
        const auto clicked = ImGui::Button(label);
        ImGui::PopID();
        return clicked;
    }, GetActiveWindow());
}

void __cdecl RefreshLauncherPlugins()
{
    LauncherExtensions() = DiscoverExtensions(false);
}

int __cdecl RenderLauncherPluginSettings(const wchar_t* path, const ThpracPluginUi* ui)
{
    const auto* extension = LoadExtension(fs::path(path), true);
    const auto* plugin = extension ? PluginCallbacks(*extension) : nullptr;
    if (!plugin || !plugin->draw_settings) return 0;
    auto panel = *ui;
    panel.config = MakePluginConfig(extension->id);
    plugin->draw_settings(&panel);
    return 1;
}

int RunLauncherWithExtensions()
{
    static const auto installDirectory = ExtensionDirectory(&__ImageBase).parent_path();
    static const ThpracLauncherHost host {sizeof(host), ApplyFromExtension, RenderToolActions,
        RefreshLauncherPlugins, RenderLauncherPluginSettings, installDirectory.c_str()};
    for (const auto* extension : LauncherExtensions()) {
        if (extension->run_launcher) return extension->run_launcher(&host);
    }
    return GuiLauncherMain();
}

int ListLauncherExtensions()
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        output = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!output || output == INVALID_HANDLE_VALUE) {
        return 1;
    }
    for (const auto* extension : LauncherExtensions()) {
        const std::string line = std::string(extension->id) + "\r\n";
        DWORD written = 0;
        if (!WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)
            || written != line.size()) {
            return 1;
        }
    }
    return 0;
}

static const std::vector<const ThpracExtension*>& GameExtensions()
{
    static const auto extensions = DiscoverExtensions(true);
    return extensions;
}

static const ThpracExtension* __cdecl FindGameExtension(const char* id)
{
    for (const auto* extension : GameExtensions()) {
        if (strcmp(extension->id, id) == 0) return extension;
    }
    return nullptr;
}

static void __cdecl DispatchGameFrame(const ThpracGameFrame* frame)
{
    for (const auto* extension : GameExtensions()) {
        if (!extension->on_game_frame) continue;
        try {
            extension->on_game_frame(frame);
        } catch (...) {
            OutputDebugStringW(L"thprac: game extension frame callback failed\n");
        }
    }
}

bool TryExtensionGameInit(const char* gameId, unsigned timestamp, unsigned textSize)
{
    static const auto installDirectory = ExtensionDirectory(&__ImageBase).parent_path();
    static const ThpracGameHost host {sizeof(host), timestamp, textSize, FindGameExtension, DispatchGameFrame,
        DispatchPluginEvent, installDirectory.c_str()};
    SetPluginGame(gameId, timestamp, textSize, GameExtensions());
    for (const auto* extension : GameExtensions()) {
        if (extension->start_game_adapter && extension->start_game_adapter(&host, gameId)) return true;
        if (extension->replace_game_init && extension->replace_game_init(gameId)) return true;
    }
    return false;
}

void DrawGameExtensions(ImDrawList* draw, int game, bool gameInfoVisible)
{
    const auto& extensions = GameExtensions();
    if (extensions.empty()) {
        return;
    }
    const ThpracGameFrame frame { sizeof(ThpracGameFrame), game, gameInfoVisible ? 1u : 0u,
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),
        g_gameGuiHwnd ? reinterpret_cast<void*>(*g_gameGuiHwnd) : nullptr, draw, ReadGameMemory, DrawTextBox };
    DispatchGameFrame(&frame);
}
}
