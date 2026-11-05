#include <imgui.h>
#include "thprac_feature_services.h"
#include "thprac_hook.h"
#include "feature_runtime.h"
#include <cstring>

namespace THPrac { void TH15Init(); }

namespace {
int __cdecl SupportsGame(const char* game)
{
    return game && std::strcmp(game, "th15") == 0;
}
int __cdecl Start(const ThpracGameHost* host, const char* game)
{
    if (!host || host->size < offsetof(ThpracGameHost, dispatch_plugin_event) || !SupportsGame(game)) return 0;
    THPrac::ConfigureAddons(*host, game);
    THPrac::VEHHookInit();
    THPrac::TH15Init();
    return 1;
}
const ThpracFeatureService th15 {sizeof(th15), SupportsGame};
const ThpracExtension extension {sizeof(extension), "rueee-c7.th15-practice", nullptr, nullptr, nullptr,
    nullptr, nullptr, Start, &th15};
}
extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
