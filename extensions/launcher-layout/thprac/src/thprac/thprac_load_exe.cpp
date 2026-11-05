#include <imgui.h>
#include "host_apply.h"
#include "thprac_load_exe.h"
#include "thprac_launcher_games.h"
#include "utils/utils.h"
#include "utils/wininternal.h"

namespace THPrac {
namespace {
void* GameSignatureAddress(HANDLE process, uintptr_t base)
{
    DWORD offset = 0;
    SIZE_T read = 0;
    if (!base || !ReadProcessMemory(process, reinterpret_cast<void*>(base + 0x3c), &offset, sizeof(offset), &read)
        || read != sizeof(offset) || offset < sizeof(IMAGE_DOS_HEADER) + sizeof(DWORD)) {
        return nullptr;
    }
    return reinterpret_cast<void*>(base + offset - sizeof(DWORD));
}

bool HasEmptyGameSignature(HANDLE process, void* address)
{
    DWORD signature = 0;
    SIZE_T read = 0;
    return address && ReadProcessMemory(process, address, &signature, sizeof(signature), &read)
        && read == sizeof(signature) && signature == 0;
}

}

uintptr_t GetGameModuleBase(HANDLE process)
{
    PROCESS_BASIC_INFORMATION info {};
    if (NtQueryInformationProcess(process, ProcessBasicInformation, &info, sizeof(info), nullptr) < 0) {
        return 0;
    }
    const auto address = reinterpret_cast<uintptr_t>(info.PebBaseAddress) + offsetof(PEB, ImageBaseAddress);
    uintptr_t base = 0;
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<void*>(address), &base, sizeof(base), &read)
        || read != sizeof(base)) {
        return 0;
    }
    return base;
}

THGameSig* CheckOngoingGameByPID(DWORD pid, uintptr_t* base, HANDLE* outHandle)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION
        | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process) {
        return nullptr;
    }
    defer(if (process) CloseHandle(process));

    *base = GetGameModuleBase(process);
    if (!HasEmptyGameSignature(process, GameSignatureAddress(process, *base))) {
        return nullptr;
    }

    ExeSig sig {};
    if (!GetExeInfoEx(reinterpret_cast<uintptr_t>(process), *base, sig)) {
        return nullptr;
    }
    for (auto& game : gGameDefs) {
        if ((game.catagory == CAT_MAIN || game.catagory == CAT_SPINOFF_STG)
            && game.exeSig.textSize == sig.textSize && game.exeSig.timeStamp == sig.timeStamp) {
            if (outHandle) {
                *outHandle = process;
                process = nullptr;
            }
            return &game;
        }
    }
    return nullptr;
}

namespace {
const ThpracLauncherHost* launcherHost = nullptr;
}

void SetLauncherExtensionHost(const ThpracLauncherHost* host)
{
    launcherHost = host;
}

const wchar_t* LauncherInstallDirectory()
{
    return launcherHost && launcherHost->size >= sizeof(ThpracLauncherHost)
        ? launcherHost->install_directory : nullptr;
}

void DrawExtensionTools()
{
    if (!launcherHost || !launcherHost->draw_tools) return;
    launcherHost->draw_tools([](const char* id, const char* label) -> int {
        ImGui::PushID(id);
        const auto clicked = ImGui::Button(label);
        ImGui::PopID();
        return clicked;
    }, GetActiveWindow());
}

bool HasPluginManager()
{
    return launcherHost && launcherHost->size >= offsetof(ThpracLauncherHost, plugin_settings) && launcherHost->refresh_plugins;
}

void __cdecl RefreshPluginTools()
{
    if (HasPluginManager()) launcherHost->refresh_plugins();
}

int __cdecl DrawPluginSettings(const wchar_t* path, const ThpracPluginUi* ui)
{
    return launcherHost && launcherHost->size >= offsetof(ThpracLauncherHost, install_directory) && launcherHost->plugin_settings
        && launcherHost->plugin_settings(path, ui);
}

bool ApplyGameRuntime(HANDLE process, uintptr_t base)
{
    return launcherHost && launcherHost->apply_to_game(process, base);
}
}
