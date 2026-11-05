#define NOMINMAX
#include "thprac_feature_services.h"
#include <Windows.h>
#include <commctrl.h>
#include <ShlObj.h>
#include <filesystem>
#include <string>
#include <algorithm>
#include <array>
#include "resource.h"

namespace {
constexpr wchar_t channelName[] = L"Local\\THPracC7VolumeV1";
constexpr wchar_t lockName[] = L"Local\\THPracC7VolumeV1-lock";
struct VolumePacket { LONG serial; LONG bgm; LONG effects; };

struct Channel {
    HANDLE lock = CreateMutexW(nullptr, FALSE, lockName);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(VolumePacket), channelName);
    VolumePacket* packet = mapping ? static_cast<VolumePacket*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VolumePacket))) : nullptr;
    ~Channel()
    {
        if (packet) UnmapViewOfFile(packet);
        if (mapping) CloseHandle(mapping);
        if (lock) CloseHandle(lock);
    }
};
Channel& GetChannel() { static Channel channel; return channel; }

struct LockedChannel {
    Channel& channel = GetChannel();
    bool acquired = false;
    explicit LockedChannel(DWORD timeout = 0)
    {
        if (channel.packet && channel.lock) {
            const auto result = WaitForSingleObject(channel.lock, timeout);
            acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }
    ~LockedChannel() { if (acquired) ReleaseMutex(channel.lock); }
};

LONG lastSerial;
bool hasSerial;
void __cdecl Begin()
{
    LockedChannel lock;
    hasSerial = lock.acquired;
    if (hasSerial) lastSerial = lock.channel.packet->serial;
}
int __cdecl Poll(int* bgm, int* effects)
{
    LockedChannel lock;
    if (!lock.acquired) return 0;
    if (!hasSerial) {
        lastSerial = lock.channel.packet->serial;
        hasSerial = true;
        return 0;
    }
    if (lock.channel.packet->serial == lastSerial) return 0;
    lastSerial = lock.channel.packet->serial;
    *bgm = lock.channel.packet->bgm;
    *effects = lock.channel.packet->effects;
    return 1;
}
bool Send(int bgm, int effects)
{
    LockedChannel lock(20);
    if (!lock.acquired) return false;
    auto& packet = *lock.channel.packet;
    packet.bgm = std::clamp(bgm, 0, 100);
    packet.effects = std::clamp(effects, 0, 100);
    InterlockedIncrement(&packet.serial);
    return true;
}

std::wstring VolumeConfigPath()
{
    wchar_t path[32768];
    const auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (length && length < std::size(path)) {
        const auto portable = std::filesystem::path(path).parent_path() / L".thprac_data";
        const auto attrs = GetFileAttributesW(portable.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
            return (portable / L"thprac_volume.ini").wstring();
    }
    wchar_t appData[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData) != S_OK) return {};
    const auto directory = std::filesystem::path(appData) / L"thprac";
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    return (directory / L"thprac_volume.ini").wstring();
}

void UpdateLabels(HWND dialog)
{
    SetDlgItemInt(dialog, IDC_BGM_VALUE, static_cast<UINT>(SendDlgItemMessageW(dialog, IDC_BGM, TBM_GETPOS, 0, 0)), FALSE);
    SetDlgItemInt(dialog, IDC_SE_VALUE, static_cast<UINT>(SendDlgItemMessageW(dialog, IDC_SE, TBM_GETPOS, 0, 0)), FALSE);
}
INT_PTR CALLBACK Dialog(HWND dialog, UINT message, WPARAM command, LPARAM data)
{
    if (message == WM_INITDIALOG) SetWindowLongPtrW(dialog, DWLP_USER, data);
    const auto* configPath = reinterpret_cast<const std::wstring*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (!configPath) return FALSE;
    const auto& config = *configPath;
    switch (message) {
    case WM_INITDIALOG: {
        const auto get = [&](const wchar_t* name) {
            return config.empty() ? 100u : std::min(100u, GetPrivateProfileIntW(L"global-volume", name, 100, config.c_str()));
        };
        const auto bgm = get(L"bgm");
        const auto effects = get(L"effects");
        for (int id : {IDC_BGM, IDC_SE}) SendDlgItemMessageW(dialog, id, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendDlgItemMessageW(dialog, IDC_BGM, TBM_SETPOS, TRUE, bgm);
        SendDlgItemMessageW(dialog, IDC_SE, TBM_SETPOS, TRUE, effects);
        UpdateLabels(dialog);
        return TRUE;
    }
    case WM_HSCROLL:
        UpdateLabels(dialog);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(command) == IDOK) {
            const auto nextBgm = static_cast<int>(SendDlgItemMessageW(dialog, IDC_BGM, TBM_GETPOS, 0, 0));
            const auto nextEffects = static_cast<int>(SendDlgItemMessageW(dialog, IDC_SE, TBM_GETPOS, 0, 0));
            if (!Send(nextBgm, nextEffects)) {
                MessageBoxW(dialog, L"Cannot send volume settings. Try again.", L"thprac", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            if (!config.empty()) {
                WritePrivateProfileStringW(L"global-volume", L"bgm", std::to_wstring(nextBgm).c_str(), config.c_str());
                WritePrivateProfileStringW(L"global-volume", L"effects", std::to_wstring(nextEffects).c_str(), config.c_str());
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(command) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}
void __cdecl LauncherAction(void* window)
{
    INITCOMMONCONTROLSEX controls {sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    HMODULE module;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&LauncherAction), &module)) {
        const auto config = VolumeConfigPath();
        DialogBoxParamW(module, MAKEINTRESOURCEW(IDD_VOLUME), static_cast<HWND>(window), Dialog,
            reinterpret_cast<LPARAM>(&config));
    }
}

const ThpracVolumeService volume {sizeof(volume), Begin, Poll};
const ThpracExtension extension {sizeof(extension), "rueee-c7.volume", nullptr, nullptr, nullptr,
    "全局游戏音量 / Game volume", LauncherAction, nullptr, &volume};
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
