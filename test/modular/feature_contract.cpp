#include "capture_service.h"
#include "route_service.h"
#include "repair_service.h"
#include "thprac_feature_services.h"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>

namespace {
const ThpracExtension& Load(const wchar_t* path, const char* id)
{
    const auto module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto query = module ? reinterpret_cast<ThpracExtensionQueryFn>(GetProcAddress(module, THPRAC_EXTENSION_QUERY)) : nullptr;
    const auto* extension = query ? query(THPRAC_EXTENSION_ABI) : nullptr;
    if (!extension || std::strcmp(extension->id, id) || query(THPRAC_EXTENSION_ABI - 1u)) {
        std::fprintf(stderr, "Invalid extension %s\n", id);
        std::exit(2);
    }
    return *extension;
}

bool Capture(const ThpracExtension& extension)
{
    using namespace THPrac::BossCapture;
    const auto& service = *static_cast<const CaptureService*>(extension.service);
    if (!service.feature.supports_game("th08") || service.feature.supports_game("th15")) return false;
    auto& runtime = service.instance();
    auto& store = runtime.Storage();
    service.load(runtime);
    service.clearAll(store);
    service.enable(store, true);
    service.setContext(runtime, {8, 1, 0, false, false});
    const Key key {8, 1, 1, kSpell, 1, 0};
    const std::string name = "regression";
    if (!service.begin(runtime, key.stage, key.attack, key.kind, name)
        || !service.settle(runtime, kCaptured)
        || service.settle(runtime, kCaptured)) return false;
    const auto found = store.Total().find(key);
    return found != store.Total().end() && found->second.attempts == 1 && found->second.captures == 1
        && service.save(store) && std::filesystem::exists(store.Path());
}

bool Route(const ThpracExtension& extension)
{
    using namespace THPrac;
    const auto& service = *static_cast<const RouteService*>(extension.service);
    struct Data { int script = -1; } data;
    RoutePlaybackRequest request {5, 1, 101, 0, {}, 0, 0, &data,
        [](const void* raw) { return static_cast<const Data*>(raw)->script; },
        [](const void*, int clock, bool exact) { return exact ? -1 : (clock - 10) / 2; },
        [](const void*, int sample) { return 10 + sample * 2; },
        [](const void*, int sample) { return sample < 3 ? 1 : 2; }};
    if (service.align(request).sample != -1) return false;
    data.script = 1;
    const auto first = service.align(request);
    if (first.sample != 1 || first.segmentEnd != 3) return false;
    data.script = -1;
    request.anchor = first.anchor;
    request.segmentBegin = first.segmentBegin;
    request.segmentEnd = first.segmentEnd;
    request.currentFrame = 110;
    return service.align(request).sample == 2;
}

bool Repairs(const ThpracExtension& extension)
{
    const auto& service = *static_cast<const ThpracRepairService*>(extension.service);
    std::array<uint32_t, 4> viewport {};
    return service.feature.supports_game("th08") && !service.feature.supports_game("th15")
        && !service.clip_dx8(-3, -1, -1, 7, 64, 48, viewport.data())
        && service.clip_dx8(-4, 2.5f, 66, 10.2f, 64, 48, viewport.data())
        && viewport == std::array<uint32_t, 4>{0, 2, 64, 9};
}

bool Settings(const ThpracExtension& extension)
{
    static unsigned page = 0;
    static unsigned buttons = 0;
    const auto& service = *static_cast<const ThpracSettingsService*>(extension.service);
    const ThpracSettingsUi ui { sizeof(ui), 1, SettingsVolume,
        [](const char*) {},
        [](const char* title, int) -> int { ++buttons; return std::strcmp(title, "Volume") == 0; },
        [] {},
        [](unsigned section) { page = section; }};
    service.draw(&ui);
    return buttons == 1 && page == SettingsVolume;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) return 1;
    wchar_t ownPath[32768];
    const auto length = GetModuleFileNameW(nullptr, ownPath, static_cast<DWORD>(std::size(ownPath)));
    if (!length || length >= std::size(ownPath)) return 3;
    std::filesystem::create_directory(std::filesystem::path(ownPath).parent_path() / L".thprac_data");
    if (!Capture(Load(argv[1], "rueee-c7.capture"))) return 4;
    if (!Route(Load(argv[2], "rueee-c7.route"))) return 5;
    if (!Settings(Load(argv[3], "rueee-c7.settings"))) return 6;
    if (!Repairs(Load(argv[4], "rueee-c7.repairs"))) return 7;
    std::puts("Capture persistence, route alignment, settings tabs and DX8 clipping passed");
    return 0;
}
