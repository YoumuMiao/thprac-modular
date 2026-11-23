#include "service.h"

#include <Windows.h>
#include <cstring>

namespace THPrac::QuickPractice {
namespace {
// Native title-menu dispatch sites and their stage-practice destinations.
// For TH18/TH20 the two handlers have the same thiscall signature, so the
// game's original return address remains on the stack.
enum class MenuRegister { Ebx, Ebp, Ecx, Edi, Th06, Th07Or08 };

struct Menu {
    const char* game;
    EntryPoint entry;
    uint32_t destination;
    MenuRegister object;
    uint32_t modeOffset, phaseOffset, timerOffset;
    uint8_t practiceMode;
    uint8_t shots, subshots;
    uint32_t difficulty, gameDifficulty, character, subshot, flags;
    EntryPoint confirm {};
    uint32_t confirmDestination = 0;
    EntryPoint display {};
    uint32_t displayFlags = 0;
    uint32_t displayCancelMask = 0;
    uint32_t displayDestination = 0;
    uint32_t displayProcedure = 0;
};

constexpr uint32_t Rva(uint32_t address) { return address - 0x400000; }
constexpr EntryPoint DisplayArgs(uint32_t address) { return { Rva(address), 2, { 0x6a, 0x00 } }; }

constexpr Menu menus[] = {
    { "th06", { Rva(0x435916), 3, { 0x8b, 0x45, 0x08 } }, Rva(0x43738c), MenuRegister::Th06,
        0, 0, 0, 0, 4, 2, 0x69bcb0, 0x6c6e49, 0x69d4bd, 0x69d4be, 0x69d4c3 },
    { "th07", { Rva(0x455502), 3, { 0x8b, 0x4d, 0xf8 } }, Rva(0x45555d), MenuRegister::Th07Or08,
        0xd0f8, 0x08, 0, 11, 6, 2, 0x575a89, 0x626280, 0x62f645, 0x62f646, 0x62f648,
        { Rva(0x45a5ab), 7, { 0x0f, 0xb6, 0x05, 0x45, 0xf6, 0x62, 0 } }, Rva(0x45a639) },
    { "th08", { Rva(0x4673f1), 3, { 0x8b, 0x4d, 0xf8 } }, Rva(0x46743f), MenuRegister::Th07Or08,
        0x14428, 0x0c, 0, 11, 12, 1, 0x17ce891, 0x160f538, 0x164d0b1, 0, 0x164d0b4,
        { Rva(0x46afb1), 7, { 0x0f, 0xb6, 0x05, 0xb1, 0xd0, 0x64, 0x01 } }, Rva(0x46b090) },
    { "th10", { Rva(0x42d0b6), 3, { 0x8b, 0x43, 0x10 } }, Rva(0x42d181), MenuRegister::Ebx,
        0x1c, 0x20, 0x2b4, 9, 6, 3, 0x474c74, 0, 0x474c68, 0x474c6c, 0x474ca0,
        {}, 0, DisplayArgs(0x438b7d), 0, 0, Rva(0x438b9e), Rva(0x43a3a0) },
    { "th11", { Rva(0x439c5f), 3, { 0x8b, 0x45, 0x10 } }, Rva(0x439d95), MenuRegister::Ebp,
        0x1c, 0x20, 0x2b4, 8, 6, 3, 0x4a5720, 0, 0x4a5710, 0x4a5714, 0x4a5758,
        {}, 0, { Rva(0x445619), 1, { 0x53 } }, 0x4c3dc0, 0x60, Rva(0x44562c), Rva(0x447910) },
    { "th12", { Rva(0x43f9ef), 3, { 0x8b, 0x45, 0x10 } }, Rva(0x43fb20), MenuRegister::Ebp,
        0x1c, 0x24, 0x2b8, 8, 6, 2, 0x4aebd0, 0x4b0ca8, 0x4b0c90, 0x4b0c94, 0x4b0ce0,
        {}, 0, { Rva(0x44f669), 1, { 0x53 } }, 0x4cf428, 0x60, Rva(0x44f67c), Rva(0x4518c0) },
    { "th13", { Rva(0x44c821), 3, { 0x8b, 0x43, 0x10 } }, Rva(0x44c9a5), MenuRegister::Ebx,
        0x1c, 0x24, 0x2b8, 8, 4, 1, 0x4be7c4, 0x4bb4d0, 0x4be7b8, 0, 0x4be830,
        {}, 0, DisplayArgs(0x45c392), 0x4df0e0, 0x60, Rva(0x45c3a7), Rva(0x45e910) },
    { "th14", { Rva(0x459bf4), 3, { 0x8b, 0x47, 0x10 } }, Rva(0x459d85), MenuRegister::Edi,
        0x1c, 0x24, 0x2b8, 8, 6, 2, 0x4f5834, 0x4d5984, 0x4f5828, 0x4f582c, 0x4f58b8,
        {}, 0, DisplayArgs(0x4695b7), 0x4f7a54, 0x180, Rva(0x469837), Rva(0x46bd30) },
    { "th15", { Rva(0x460bb1), 3, { 0x8b, 0x47, 0x0c } }, Rva(0x460cf7), MenuRegister::Edi,
        0x18, 0x20, 0x2b4, 9, 4, 1, 0x4e7410, 0x4e0ef4, 0x4e7404, 0, 0x4e7794,
        {}, 0, DisplayArgs(0x47160a), 0x51bbec, 0x180, Rva(0x4718aa), Rva(0x473de0) },
    { "th16", { Rva(0x44b24f), 3, { 0x8b, 0x47, 0x0c } }, Rva(0x44b3f9), MenuRegister::Edi,
        0x18, 0x20, 0x2b0, 8, 16, 4, 0x49f274, 0x4a57b4, 0x4a57a4, 0x4a57ac, 0x4a5bec,
        {}, 0, DisplayArgs(0x459b01), 0x4d9d1c, 0x180, Rva(0x459da7), Rva(0x45c110) },
    { "th17", { Rva(0x451921), 3, { 0x8b, 0x47, 0x0c } }, Rva(0x451bd6), MenuRegister::Edi,
        0x18, 0x20, 0x2b0, 8, 9, 3, 0x4b2b28, 0x4b5a00, 0x4b59f4, 0x4b59f8, 0x4b59c8,
        {}, 0, DisplayArgs(0x460b9f), 0x524700, 0x180, Rva(0x460e30), Rva(0x463400) },
    { "th18", { Rva(0x465aa0), 3, { 0x55, 0x8b, 0xec } }, Rva(0x467320), MenuRegister::Ecx,
        0x18, 0x20, 0x388, 8, 4, 1, 0x4c9ab0, 0x4ccd00, 0x4cccf4, 0, 0x4cccc8,
        {}, 0, { Rva(0x47174d), 5, { 0xe8, 0xfe, 0x30, 0, 0 } }, 0x56ac70, 0x300, 0, Rva(0x4746c0) },
    { "th20", { Rva(0x529e80), 3, { 0x55, 0x8b, 0xec } }, Rva(0x529430), MenuRegister::Ecx,
        0x18, 0x20, 0x154, 8, 16, 8, 0x5b0a60, 0x5ba7d0, 0x5ba5f8, 0x5ba60c, 0x5ba5d4,
        { Rva(0x529618), 2, { 0x33, 0xc0 } }, Rva(0x529661),
        { Rva(0x41e90b), 5, { 0xe8, 0x60, 0xc5, 0xff, 0xff } }, 0x5b87e8, 0x18, 0, Rva(0x41abe0) }
};

const Menu* FindMenu(const char* game)
{
    for (const auto& menu : menus)
        if (std::strcmp(game, menu.game) == 0) return &menu;
    return nullptr;
}

uintptr_t MenuObject(const CONTEXT& context, MenuRegister reg)
{
    switch (reg) {
    case MenuRegister::Ebx: return context.Ebx;
    case MenuRegister::Ebp: return context.Ebp;
    case MenuRegister::Ecx: return context.Ecx;
    case MenuRegister::Edi: return context.Edi;
    case MenuRegister::Th06: return *reinterpret_cast<const DWORD*>(context.Ebp + 8);
    case MenuRegister::Th07Or08: return *reinterpret_cast<const DWORD*>(context.Ebp - 8);
    }
    return 0;
}

struct DisplaySelection {
    uintptr_t procedure;
    int command;
};

BOOL CALLBACK AcceptDisplay(HWND window, LPARAM data)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != GetCurrentProcessId()) return TRUE;
    wchar_t name[8];
    if (!GetClassNameW(window, name, _countof(name)) || wcscmp(name, L"#32770") != 0) return TRUE;
    const auto& selection = *reinterpret_cast<const DisplaySelection*>(data);
    if (static_cast<uintptr_t>(GetWindowLongPtrA(window, DWLP_DLGPROC)) != selection.procedure
        && static_cast<uintptr_t>(GetWindowLongPtrW(window, DWLP_DLGPROC)) != selection.procedure) return TRUE;
    const auto button = GetDlgItem(window, selection.command);
    if (!button) return TRUE;
    return !PostMessageW(window, WM_COMMAND, MAKEWPARAM(selection.command, BN_CLICKED), reinterpret_cast<LPARAM>(button));
}
}

int __cdecl SupportsGame(const char* game)
{
    return game && FindMenu(game);
}

const EntryPoint* __cdecl NativeEntry(const Request* request, unsigned step)
{
    const auto* menu = FindMenu(request->game);
    if (!menu || request->difficulty < 0 || request->difficulty > 3
        || request->shot < 0 || request->shot >= menu->shots)
        return nullptr;
    const int finalStage = std::strcmp(menu->game, "th08") == 0 ? 9
        : std::strcmp(menu->game, "th07") == 0 ? 7 : 6;
    if (request->stage < 0 || request->stage > finalStage) return nullptr;
    if (step == Title) return &menu->entry;
    if (step == Confirm && menu->confirm.length) return &menu->confirm;
    if (step == Display && menu->display.length) return &menu->display;
    return nullptr;
}

int __cdecl NativeMenuReady(const Request* request, const void* registers)
{
    const auto& menu = *FindMenu(request->game);
    if (std::strcmp(menu.game, "th07") != 0) return 1;
    const auto object = MenuObject(*static_cast<const CONTEXT*>(registers), menu.object);
    // The original title initialization allocates the DX8 buffers used by
    // the next scene. Let it run through the first title animation.
    return *reinterpret_cast<const int*>(object + menu.phaseOffset) == 1;
}

void __cdecl CompleteDisplay(const Request* request)
{
    const auto* menu = FindMenu(request->game);
    if (!menu || !menu->displayProcedure) return;

    // thcrap can reach the display chooser before the adapter is loaded.
    // Accept the saved settings through that game's own dialog procedure.
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    int command = 0xd0;
    if (std::strcmp(menu->game, "th10") == 0) {
        command = *reinterpret_cast<const uint8_t*>(image + Rva(0x491d65)) ? 0xcb : 0xc9;
    } else if (std::strcmp(menu->game, "th18") == 0 || std::strcmp(menu->game, "th20") == 0) {
        command = 0xce;
    }
    const DisplaySelection selection { image + menu->displayProcedure, command };
    EnumWindows(AcceptDisplay, reinterpret_cast<LPARAM>(&selection));
}

void __cdecl EnterNativePractice(const Request* request, void* registers, unsigned step)
{
    const auto& menu = *FindMenu(request->game);
    auto& context = *static_cast<CONTEXT*>(registers);
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (step == Confirm) {
        context.Eip = image + menu.confirmDestination;
        return;
    }
    const auto field = [image](uint32_t address) -> uint32_t& {
        return *reinterpret_cast<uint32_t*>(image + Rva(address));
    };
    if (step == Display) {
        // Continue with the display settings already loaded by WinMain.
        if (menu.displayFlags) field(menu.displayFlags) &= ~menu.displayCancelMask;
        context.Eip = menu.displayDestination ? image + menu.displayDestination : context.Eip + menu.display.length;
        return;
    }
    auto* object = reinterpret_cast<unsigned char*>(MenuObject(context, menu.object));
    const bool imperishableNight = std::strcmp(menu.game, "th08") == 0;
    const int extra = imperishableNight ? 8 : 6;
    const int difficulty = request->stage == extra ? 4
        : std::strcmp(menu.game, "th07") == 0 && request->stage == 7 ? 5 : request->difficulty;
    const auto byte = [image](uint32_t address) -> uint8_t& {
        return *reinterpret_cast<uint8_t*>(image + Rva(address));
    };
    if (menu.object == MenuRegister::Th06 || menu.object == MenuRegister::Th07Or08) {
        byte(menu.difficulty) = static_cast<uint8_t>(difficulty);
        byte(menu.character) = static_cast<uint8_t>(request->shot / menu.subshots);
        if (menu.subshot) byte(menu.subshot) = static_cast<uint8_t>(request->shot % menu.subshots);
        if (menu.object == MenuRegister::Th06) {
            byte(menu.gameDifficulty) = static_cast<uint8_t>(difficulty);
            byte(menu.flags) = 1;
            *reinterpret_cast<int*>(object + 0x81a0) = request->stage;
        } else {
            field(menu.gameDifficulty) = difficulty;
            field(menu.flags) |= 1;
            if (!imperishableNight) {
                using SetMenu = void (__thiscall *)(void*, int);
                reinterpret_cast<SetMenu>(image + Rva(0x455435))(object, menu.practiceMode);
            } else {
                *reinterpret_cast<int*>(object + menu.modeOffset) = menu.practiceMode;
                *reinterpret_cast<int*>(object + menu.phaseOffset) = 1;
            }
            *reinterpret_cast<int*>(object) = 0;
        }
        context.Eip = image + menu.destination;
        return;
    }

    field(menu.difficulty) = difficulty;
    if (menu.gameDifficulty) field(menu.gameDifficulty) = difficulty;
    field(menu.character) = request->shot / menu.subshots;
    if (menu.subshot) field(menu.subshot) = request->shot % menu.subshots;
    field(menu.flags) |= 0x10;

    *reinterpret_cast<int*>(object + menu.modeOffset) = menu.practiceMode;
    if (std::strcmp(menu.game, "th20") == 0) {
        *reinterpret_cast<int*>(object + menu.phaseOffset) = 0;
        using SetTimer = void (__thiscall *)(void*, int);
        reinterpret_cast<SetTimer>(image + Rva(0x423520))(object + menu.timerOffset, 0);
    } else {
        *reinterpret_cast<int*>(object + menu.phaseOffset) = 3;
        *reinterpret_cast<int*>(object + menu.timerOffset) = 41;
        *reinterpret_cast<float*>(object + menu.timerOffset + 4) = 41.0f;
    }
    context.Eip = image + menu.destination;
}
}
