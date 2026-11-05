#include "capture_service.h"
#include <cstring>

namespace {
using namespace THPrac::BossCapture;

int __cdecl SupportsGame(const char* id)
{
    constexpr const char* games[] {
        "th06", "th07", "th08", "th10", "th11", "th12", "th13", "th14", "th16", "th17", "th18", "th20"
    };
    for (const char* game : games)
        if (std::strcmp(id, game) == 0) return 1;
    return 0;
}

const CaptureService capture {
    {sizeof(capture), SupportsGame}, Instance, SetNativeBridgeActive, NativeBridgeActive,
    [](Runtime& runtime, const Context& context) { runtime.SetContext(context); },
    [](Runtime& runtime) { runtime.EnsureLoaded(); },
    [](Runtime& runtime, int stage, int attack, int kind, const std::string& name) { return runtime.BeginAttack(stage, attack, kind, name); },
    [](Runtime& runtime) { runtime.MarkFailure(); },
    [](Runtime& runtime, Outcome outcome) { return runtime.Settle(outcome); },
    [](Runtime& runtime) { runtime.AbortAttack(); },
    [](Runtime& runtime, int stage, int attack, int kind, uint32_t captures, uint32_t failures, const std::string& name) {
        return runtime.RecordCompleted(stage, attack, kind, captures, failures, name);
    },
    [](Runtime& runtime) { runtime.Tick(); },
    [](Runtime& runtime) { runtime.Flush(); },
    [](Store& store, bool value) { store.SetEnabled(value); },
    [](Store& store, bool value) { store.SetShowHud(value); },
    [](Store& store) { store.ClearSession(); },
    [](Store& store) { store.ClearTotal(); },
    [](Store& store) { store.ClearAll(); },
    [](const Key& key) { return key.Valid(); },
    [](Store& store) { return store.Save(); },
    [](Store& store, const Key& key) { return store.RecordCapture(key); },
    [](Store& store, const Key& key) { return store.RecordTimeout(key); },
    AppendLog
};
const ThpracExtension extension {sizeof(extension), "rueee-c7.capture", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, &capture};
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
