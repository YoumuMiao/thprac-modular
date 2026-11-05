#include "thprac_extension_inventory.h"
#include <Windows.h>
#include <filesystem>
#include <fstream>

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 1;
    namespace fs = std::filesystem;
    using namespace THPrac;
    const auto directory = fs::absolute(argv[1]) / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(directory / L"game");
    fs::create_directories(directory / L"shared");
    std::ofstream(directory / L"game" / L"CAPTURE.thp.DLL").put('a');
    std::ofstream(directory / L"game" / L"route.thp.dll.disabled").put('b');
    std::ofstream(directory / L"shared" / L"音量%3D=1.thp.dll").put('c');
    std::ofstream(directory / L"game" / L"readme.txt").put('d');
    std::ofstream(directory / L"game" / L"CAPTURE.thp.DLL.json", std::ios::binary)
        << R"({"zh":"收率说明","en":"Capture help","ja":"取得率の説明","settings":true})";
    std::ofstream(directory / L"game" / L"route.thp.dll.json", std::ios::binary)
        << R"({"en":"Route help"})";
    std::ofstream(directory / L"shared" / L"音量%3D=1.thp.dll.json", std::ios::binary) << "{invalid";

    fs::create_directory(directory / L"game" / L"directory.thp.dll");

    const auto initial = InstalledExtensions(directory);
    if (initial.size() != 3 || !initial[0].enabled || initial[0].scope != ExtensionScope::Game
        || initial[1].enabled || !initial[1].renamed || initial[2].scope != ExtensionScope::Shared) return 2;
    if (!ReadExtensionMetadata(initial[0], 0).hasSettings
        || ReadExtensionMetadata(initial[1], 1).hasSettings
        || ReadExtensionMetadata(initial[0], 0).description != "收率说明"
        || ReadExtensionMetadata(initial[0], 2).description != "取得率の説明"
        || ReadExtensionMetadata(initial[1], 0).description != "Route help"
        || !ReadExtensionMetadata(initial[2], 1).description.empty()) return 9;
    if (!SetExtensionEnabled(initial[0], false) || !SetExtensionEnabled(initial[2], false)) return 3;
    const auto disabled = InstalledExtensions(directory);
    if (disabled.size() != 3 || disabled[0].enabled || disabled[2].enabled
        || !fs::is_regular_file(initial[0].path)) return 4;
    if (!SetExtensionEnabled(disabled[0], true) || !SetExtensionEnabled(disabled[1], true)) return 5;
    const auto enabled = InstalledExtensions(directory);
    if (enabled.size() != 3 || !enabled[0].enabled || !enabled[1].enabled || enabled[1].renamed || enabled[2].enabled
        || !fs::is_regular_file(directory / L"game" / L"route.thp.dll")) return 6;

    if (ReadExtensionMetadata(disabled[0], 1).description != "Capture help"
        || ReadExtensionMetadata(enabled[1], 2).description != "Route help") return 10;

    // A stale legacy row must not overwrite an installed DLL.
    std::ofstream(initial[1].path).put('e');
    if (SetExtensionEnabled(initial[1], true)) return 7;
    std::ifstream active(directory / L"game" / L"route.thp.dll");
    if (active.get() != 'b') return 8;

    const auto global = directory / L"global";
    fs::create_directories(global / L"game");
    std::ofstream(global / L"game" / L"CAPTURE.thp.DLL").put('g');
    const auto combined = InstalledExtensions(directory, global);
    if (combined.size() != 5 || combined.front().global || !combined.back().global
        || !combined.back().enabled) return 11;
    if (!SetExtensionEnabled(combined.back(), false)) return 12;
    const auto rescanned = InstalledExtensions(directory, global);
    if (rescanned.back().enabled || !rescanned.front().enabled) return 13;
    if (InstalledExtensions(directory, directory).size() != 4) return 14;
    return 0;
}
