#include <imgui.h>
#include "thprac_feature_services.h"
#include "thprac_hook.h"
#include "feature_runtime.h"
#include "quick_practice_adapter.h"
#include <cstring>

namespace THPrac { void TH15Init(); }
namespace THPrac::TH15 { int EditQuickPractice(QuickPractice::Edit, QuickPractice::Request&); }

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
    THPrac::QuickPractice::Claim(*host, game);
    THPrac::QuickPractice::Install();
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

extern "C" __declspec(dllexport) int __cdecl THPracPracticeEditor(const char* game,
    const THPrac::QuickPractice::EditorContext* context, THPrac::QuickPractice::Edit action,
    THPrac::QuickPractice::Request* request)
{
    using namespace THPrac::QuickPractice;
    if (!SupportsGame(game)) return 0;
    if (action == Edit::Probe) return 1;
    if (!context || !request) return 0;
    PreviewScope preview(*context, *request);
    return THPrac::TH15::EditQuickPractice(action, *request);
}
