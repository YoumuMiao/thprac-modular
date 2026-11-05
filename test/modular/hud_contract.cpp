#include "thprac_extension.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

namespace {
std::vector<std::string> fields;
std::map<uintptr_t, unsigned char> gameMemory;

template<class T>
void Set(uintptr_t address, T value)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) gameMemory[address + i] = bytes[i];
}

int __cdecl Read(uintptr_t address, void* buffer, uint32_t size)
{
    auto* bytes = static_cast<unsigned char*>(buffer);
    for (uint32_t i = 0; i < size; ++i) {
        const auto entry = gameMemory.find(address + i);
        if (entry == gameMemory.end()) return 0;
        bytes[i] = entry->second;
    }
    return 1;
}
void __cdecl Record(void*, float, float, float, const char* text)
{
    fields.emplace_back(text);
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) return 2;
    const auto module = LoadLibraryW(argv[1]);
    if (!module) return 3;
    const auto query = reinterpret_cast<ThpracExtensionQueryFn>(GetProcAddress(module, THPRAC_EXTENSION_QUERY));
    if (!query || query(0) || query(THPRAC_EXTENSION_ABI + 1)) return 4;
    const auto* extension = query(THPRAC_EXTENSION_ABI);
    if (!extension || !extension->on_game_frame || extension->run_launcher || extension->replace_game_init) return 5;

    // Match TH08's checked frame addresses in isolated simulated memory.
    const uintptr_t boss = 0x2000000;
    Set(0x17ce8b0, 2);
    Set(0x17d61ac, 128.5f);
    Set(0x17d61b0, 250.0f);
    Set(0xf54cc0, boss);
    Set(boss + 0x2d88, 64.0f);
    Set(boss + 0x2d8c, 100.0f);
    Set(boss + 0x2dfc, 3000);
    Set(boss + 0x3358, 1000);

    ThpracGameFrame frame { sizeof(ThpracGameFrame), 8, 0, 0, nullptr, nullptr, Read, Record };
    extension->on_game_frame(&frame);
    if (!fields.empty()) return 7;
    frame.game_info_visible = 1;
    extension->on_game_frame(&frame);
    const std::vector<std::string> expected { "HP 2000", "B(64.0,100.0)", "P(128.5,250.0)" };
    if (fields != expected) return 8;
    fields.clear();
    Set(0x17ce8b0, 1);
    extension->on_game_frame(&frame);
    if (!fields.empty()) return 9;
    std::puts("TH08 status HUD respects visibility and gameplay and draws all three fields");
    return 0;
}
