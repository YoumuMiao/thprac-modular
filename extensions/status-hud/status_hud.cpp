#define NOMINMAX
#include "thprac_feature_services.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdio>

namespace {
struct Layout {
    int game;
    uintptr_t player;
    uintptr_t manager;
    unsigned playerPosition, bossSlots, listHead, bossPosition, health, flags;
    uint32_t bossFlag;
    uintptr_t gameMode;
};

constexpr Layout layouts[] {
    // TH06–TH08 use static player positions, game state and boss pointer arrays.
    {6, 0x6caa68, 0x4b79c8, 0, 0xee598, 0, 0xc6c, 0xce4, 0, 0, 0x6c6ea4},
    {7, 0x4be408, 0x12fe098, 0, 0, 0, 0x2b0c, 0x2bb8, 0, 0, 0x575aa4},
    {8, 0x17d61ac, 0xf54cc0, 0, 0, 0, 0x2d88, 0x2dfc, 0, 0, 0x17ce8b0},
    {10, 0x477834, 0x477704, 0x3c0, 0x10, 0, 0x1070, 0x23fc},
    {11, 0x4a8eb4, 0x4a8d7c, 0x87c, 0x1c, 0, 0x1070, 0x252c},
    {12, 0x4b4514, 0x4b43dc, 0x97c, 0x1c, 0, 0x1074, 0x2648},
    {13, 0x4c22c4, 0x4c2188, 0x5b8, 0x5c, 0, 0x1230, 0x5138},
    {14, 0x4db67c, 0x4db544, 0x5e0, 0x5c, 0, 0x1234, 0x5164},
    {16, 0x4a6ef8, 0x4a6dc0, 0x610, 0, 0x1250, 0x5180, 0x526c, 0x800000},
    {17, 0x4b77d0, 0x4b76a0, 0x610, 0, 0x1250, 0x5180, 0x528c, 0x800000},
    {18, 0x4cf410, 0x4cf2d0, 0x620, 0, 0x1270, 0x6220, 0x635c, 0x800000},
    {20, 0x1ba56c, 0x1ba570, 0x614, 0, 0x10c, 0x198, 0x214, 0x354, 0x80},
};

struct GameMemory {
    const ThpracGameFrame& frame;

    template<class T>
    bool Read(uintptr_t address, T& value) const
    {
        return frame.read_game_memory(address, &value, sizeof(value)) != 0;
    }

    template<class T>
    T Read(uintptr_t address) const
    {
        T value {};
        return Read(address, value) ? value : T {};
    }
};

struct Position {
    float x, y;
};

bool ReadPosition(const GameMemory& memory, uintptr_t address, Position& position)
{
    return memory.Read(address, position) && std::isfinite(position.x) && std::isfinite(position.y);
}

uintptr_t FindBoss(const GameMemory& memory, const Layout& layout, uintptr_t base)
{
    const bool early = layout.game < 10;
    const auto manager = early ? layout.manager : memory.Read<uintptr_t>(base + layout.manager);
    if (!manager) return 0;
    if (early || !layout.listHead) {
        const unsigned slots = early ? 8 : 4;
        for (unsigned i = 0; i < slots; ++i) {
            if (const auto boss = memory.Read<uintptr_t>(manager + layout.bossSlots + i * sizeof(uintptr_t))) {
                return boss;
            }
        }
        return 0;
    }
    auto node = memory.Read<uintptr_t>(manager + layout.listHead);
    for (unsigned i = 0; node && i < 4096; ++i) {
        const auto enemy = memory.Read<uintptr_t>(node);
        if (enemy && (memory.Read<uint32_t>(enemy + layout.flags) & layout.bossFlag)) {
            return enemy;
        }
        const auto next = memory.Read<uintptr_t>(node + sizeof(uintptr_t));
        if (next == node) break;
        node = next;
    }
    return 0;
}

int PhaseHp(const GameMemory& memory, const Layout& layout, uintptr_t enemy)
{
    const int hp = memory.Read<int>(enemy + layout.health);
    int boundary = 0;
    if (layout.game == 6) {
        boundary = memory.Read<int>(enemy + 0xea8);
    } else if (layout.game == 7 || layout.game == 8) {
        const unsigned offset = layout.game == 7 ? 0x2ebc : 0x3358;
        for (unsigned i = 0; i < 4; ++i) {
            const int candidate = memory.Read<int>(enemy + offset + i * 4);
            if (candidate > boundary && candidate < hp) boundary = candidate;
        }
    } else {
        boundary = memory.Read<int>(enemy + layout.health + 0x10);
    }
    return std::max(0, hp - (boundary > 0 && boundary < hp ? boundary : 0));
}

unsigned visibleFields = 7;
unsigned __cdecl Fields() { return visibleFields; }
void __cdecl SetFields(unsigned fields) { visibleFields = fields & 7u; }

void __cdecl Draw(const ThpracGameFrame* frame)
{
    if (!visibleFields || !frame->game_info_visible || !frame->read_game_memory || !frame->draw_text_box) return;
    const GameMemory memory { *frame };
    const auto layout = std::find_if(std::begin(layouts), std::end(layouts),
        [frame](const Layout& item) { return item.game == frame->game; });
    if (layout == std::end(layouts)) return;
    const bool early = layout->game < 10;
    const uintptr_t base = layout->game == 20 ? frame->image_base : 0;
    if (early && memory.Read<int>(layout->gameMode) != 2) return;
    const auto player = early ? layout->player : memory.Read<uintptr_t>(base + layout->player);
    Position playerPosition {};
    if (!player || !ReadPosition(memory, player + layout->playerPosition, playerPosition)) return;
    if (early && (playerPosition.x < -68.0f || playerPosition.x > 452.0f
            || playerPosition.y < -80.0f || playerPosition.y > 640.0f)) {
        return;
    }

    char hp[40] = "HP --";
    char boss[80] = "B (--,--)";
    char position[80];
    const auto enemy = FindBoss(memory, *layout, base);
    Position bossPosition {};
    if (enemy && ReadPosition(memory, enemy + layout->bossPosition, bossPosition)) {
        std::snprintf(hp, sizeof(hp), "HP %d", PhaseHp(memory, *layout, enemy));
        std::snprintf(boss, sizeof(boss), "B(%.1f,%.1f)", bossPosition.x, bossPosition.y);
    }
    std::snprintf(position, sizeof(position), "P(%.1f,%.1f)", playerPosition.x, playerPosition.y);
    const float scale = layout->game <= 13 ? 0.5f : 1.0f;
    if (visibleFields & 1u) frame->draw_text_box(frame->drawing, 224.0f * scale, 0.0f, 132.0f * scale, hp);
    if (visibleFields & 2u) frame->draw_text_box(frame->drawing, 360.0f * scale, 0.0f, 150.0f * scale, boss);
    if (visibleFields & 4u) frame->draw_text_box(frame->drawing, 514.0f * scale, 0.0f, 176.0f * scale, position);
}

const ThpracHudService hud {sizeof(hud), Fields, SetFields};
const ThpracExtension extension { sizeof(ThpracExtension), "rueee-c7.status-hud", nullptr, Draw, nullptr, nullptr, nullptr, nullptr, &hud };
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
