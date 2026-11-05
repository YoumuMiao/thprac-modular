#include "host_apply.h"
#include "thprac_launcher_main.h"
#include <Windows.h>
#include <cstddef>

namespace {
int __cdecl RunLauncher(const ThpracLauncherHost* callbacks)
{
    if (!callbacks || callbacks->size < offsetof(ThpracLauncherHost, refresh_plugins) || !callbacks->apply_to_game) {
        return -1;
    }
    THPrac::SetLauncherExtensionHost(callbacks);
    return THPrac::GuiLauncherMain();
}

const ThpracExtension extension { sizeof(ThpracExtension), "rueee-c7.launcher-layout", RunLauncher, nullptr, nullptr };
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
