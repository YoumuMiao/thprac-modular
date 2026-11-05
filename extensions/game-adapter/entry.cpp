#define THPRAC_GAME_RUNTIME
#define TH15Init TH15StockInit
#include "thprac_launcher_games_def.h"
#undef TH15Init
#include "thprac_hook.h"
#include "feature_runtime.h"
#include <cstring>

namespace {
int __cdecl Start(const ThpracGameHost* host, const char* gameId)
{
    using namespace THPrac;
    if (!host || host->size < offsetof(ThpracGameHost, dispatch_plugin_event)) return 0;
    ConfigureAddons(*host, gameId);
    if (!AddonEnabled(Addon::Capture) && !AddonEnabled(Addon::Route) && !AddonEnabled(Addon::Volume)
        && !AddonEnabled(Addon::Settings) && !AddonEnabled(Addon::Repairs) && !AddonEnabled(Addon::Th15))
        return 0;
    // Keep the exact executable match, including both TH19 builds.
    for (const auto& game : gGameDefs) {
        if (game.initFunc && std::strcmp(game.idStr, gameId) == 0
            && game.exeSig.timeStamp == host->exe_timestamp && game.exeSig.textSize == host->exe_text_size) {
            if (std::strcmp(gameId, "th15") == 0 && AddonEnabled(Addon::Th15)) return 0;
            VEHHookInit();
            game.initFunc();
            return 1;
        }
    }
    return 0;
}
const ThpracExtension extension { sizeof(extension), "rueee-c7.game-adapter", nullptr, nullptr, nullptr,
    nullptr, nullptr, Start, nullptr };
}
extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}

// Internal status entry used by the isolated x86 attachment regression.
extern "C" DWORD WINAPI THPracAdapterFeatureMask(void*)
{
    unsigned mask = 0;
    for (unsigned i = 0; i <= static_cast<unsigned>(THPrac::Addon::Hud); ++i)
        if (THPrac::AddonEnabled(static_cast<THPrac::Addon>(i))) mask |= 1u << i;
    return mask;
}
