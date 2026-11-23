#pragma once
#include <cstdint>

namespace THPrac {
enum class NativeSpellRegister { Primary, Ebx, Ecx, Edi, Ebp };
struct NativeSpellSite {
    uintptr_t address = 0;
    const char* bytes = nullptr;
    unsigned Length() const { unsigned n = 0; if (bytes) while (bytes[n]) ++n; return n / 2; }
};
struct NativeSpellCounter {
    uintptr_t base = 0;
    bool indirect = true;
    unsigned offset = 0, idStride = 0, shotStride = 0, width = 4;
    uintptr_t shotFirst = 0, shotSecond = 0;
    unsigned shotScale = 1, shotWidth = 4;
    uintptr_t modeFlags = 0;
    uint32_t modeMask = 0x30, modeValue = 0x20;
    unsigned modeStride = 4;
    uint32_t saturation = 99999;
};
struct NativeSpellReplay {
    uintptr_t address = 0;
    bool indirect = true;
    unsigned offset = 0x10, width = 4;
    uint32_t mask = 0xffffffffu, value = 1;
    bool nonzero = false;
};
struct NativeSpellProfile {
    int game = 0;
    bool rva = false;
    NativeSpellSite before, success, after;
    NativeSpellRegister ownerRegister = NativeSpellRegister::Primary;
    intptr_t ownerOffset = 0;
    uintptr_t object = 0;
    bool objectIndirect = true;
    unsigned idOffset = 0, flagsOffset = 0;
    uint32_t activeMask = 1, activeValue = 1;
    NativeSpellRegister objectArgument = NativeSpellRegister::Primary;
    bool existingSuccessHook = false;
    NativeSpellCounter counter;
    NativeSpellReplay replay;
};

// Exact bytes are validated against the loaded image before enabling hooks.
// TH20 uses RVAs; other supported images retain their original fixed layout.
inline NativeSpellProfile NativeSpellProfileFor(int game)
{
    NativeSpellProfile p;
    p.game = game;
    switch (game) {
    case 6:
        p.before = {0x40983b,"833d905f5a0000"}; p.success = {0x409978,"890dc8bc6900"}; p.after = {0x409988,"c705907b480000000000"};
        p.ownerRegister = NativeSpellRegister::Ebp; p.ownerOffset = 8;
        p.object = 0x5a5f90; p.objectIndirect = false; p.idOffset = 8; p.activeMask = 0xffffffffu;
        p.existingSuccessHook = true;
        p.counter.base = 0x69bcd0; p.counter.indirect = false; p.counter.idStride = 0x40; p.counter.offset = 0x3e; p.counter.width = 2; p.counter.saturation = 0;
        p.replay = {0x69bcbc,false,0,1,0xffffffffu,0,true};
        break;
    case 7:
        p.before = {0x4101a0,"55"}; p.success = {0x410479,"a178626200"}; p.after = {0x41050a,"c7051480340100000000"};
        p.object = 0x12fe0c8; p.objectIndirect = false; p.idOffset = 0x10; p.activeMask = 0xffffffffu;
        p.counter.base = 0x626288; p.counter.indirect = false; p.counter.idStride = 0x78; p.counter.offset = 0x6a; p.counter.shotStride = 2; p.counter.width = 2;
        p.counter.shotFirst = 0x62f647; p.counter.shotWidth = 1; p.counter.saturation = 9999;
        p.replay = {0x62f648,false,0,4,8,8,false};
        break;
    case 8:
        p.before = {0x4161b0,"55"}; p.success = {0x416265,"8b4808"}; p.after = {0x416a86,"8b45e0"};
        p.ownerRegister = NativeSpellRegister::Ecx; p.ownerOffset = 4;
        p.object = 0x4ea670; p.objectIndirect = false; p.idOffset = 8; p.objectArgument = NativeSpellRegister::Ecx;
        p.existingSuccessHook = true;
        p.counter.base = 0x160f548; p.counter.indirect = false; p.counter.idStride = 0x22c; p.counter.offset = 0x158; p.counter.shotStride = 4;
        p.counter.modeFlags = 0x164d0b4; p.counter.modeMask = p.counter.modeValue = 0x4000; p.counter.modeStride = 0x9c; p.counter.saturation = 9999;
        p.replay = {0x164d0b4,false,0,4,8,8,false};
        break;
    case 10:
        p.before = {0x410f5b,"e8a08cffff"}; p.success = {0x409d3c,"6a00"}; p.after = {0x410f60,"33c0"};
        p.ownerRegister = NativeSpellRegister::Ebx; p.ownerOffset = 0x14d8;
        p.object = 0x4776f4; p.idOffset = 0x3788; p.flagsOffset = 0x378c;
        p.counter.base = 0x47783c; p.counter.shotStride = 0x437c; p.counter.idStride = 0x90; p.counter.offset = 0x624;
        p.replay.address = 0x477838;
        break;
    case 11:
        p.before = {0x415c24,"e83771ffff"}; p.success = {0x40ce90,"be2d000000"}; p.after = {0x415c29,"83a304150000fe"};
        p.ownerRegister = NativeSpellRegister::Ebx; p.ownerOffset = 0x1614;
        p.object = 0x4a8d6c; p.idOffset = 0x8dc; p.flagsOffset = 0x8e0;
        p.counter.base = 0x4a8ebc; p.counter.shotStride = 0x68d4; p.counter.idStride = 0x90; p.counter.offset = 0x6ec;
        p.replay.address = 0x4a8eb8;
        break;
    case 12:
        p.before = {0x418975,"e8665cffff"}; p.success = {0x40e702,"ba30000000"}; p.after = {0x41897a,"83a31c160000fe"};
        p.ownerRegister = NativeSpellRegister::Ebx; p.ownerOffset = 0x174c;
        p.object = 0x4b43cc; p.idOffset = 0x78; p.flagsOffset = 0x7c;
        p.counter.base = 0x4b451c; p.counter.shotStride = 0x45f4; p.counter.idStride = 0x90; p.counter.offset = 0x6ec;
        p.replay.address = 0x4b4518;
        break;
    case 13:
        p.before = {0x41e582,"e8d954ffff"}; p.success = {0x413b8e,"ba2e000000"}; p.after = {0x41e587,"83a7643f0000fe"};
        p.ownerRegister = NativeSpellRegister::Edi; p.ownerOffset = 0x40c8;
        p.object = 0x4c2178; p.idOffset = 0x78; p.flagsOffset = 0x7c;
        p.counter.base = 0x4c22cc; p.counter.shotStride = 0x56dc; p.counter.idStride = 0x9c; p.counter.offset = 0xa00;
        p.counter.shotFirst = 0x4be7b8; p.counter.shotSecond = 0x4be7bc; p.counter.modeFlags = 0x4be830;
        p.replay.address = 0x4c22c8;
        break;
    case 14:
        p.before = {0x4289ea,"e8614affff"}; p.success = {0x41d57a,"51"}; p.after = {0x4289ef,"83a78c3f0000fe"};
        p.ownerRegister = NativeSpellRegister::Edi; p.ownerOffset = 0x40ec;
        p.object = 0x4db534; p.idOffset = 0x78; p.flagsOffset = 0x7c;
        p.counter.base = 0x4db68c; p.counter.shotStride = 0x5298; p.counter.idStride = 0x9c; p.counter.offset = 0xa00; p.counter.modeFlags = 0x4f58b8;
        p.replay.address = 0x4db688;
        break;
    case 16:
        p.before = {0x421891,"e85a6affff"}; p.success = {0x4183f4,"51"}; p.after = {0x421896,"83a78c3f0000fe"};
        p.ownerRegister = NativeSpellRegister::Edi; p.ownerOffset = 0x44b8;
        p.object = 0x4a6db0; p.idOffset = 0x74; p.flagsOffset = 0x78;
        p.counter.base = 0x4a6f0c; p.counter.shotStride = 0x5318; p.counter.idStride = 0x9c; p.counter.offset = 0x958;
        p.counter.shotFirst = 0x4a57a4; p.counter.shotSecond = 0x4a57a8; p.counter.modeFlags = 0x4a5bec;
        p.replay.address = 0x4a6f08; p.replay.offset = 0xc;
        break;
    case 17:
        p.before = {0x42525c,"e8df5effff"}; p.success = {0x41b2c5,"51"}; p.after = {0x425261,"6a00"};
        p.ownerRegister = NativeSpellRegister::Edi; p.ownerOffset = 0x44d8;
        p.object = 0x4b7690; p.idOffset = 0x74; p.flagsOffset = 0x78;
        p.counter.base = 0x4b77dc; p.counter.shotStride = 0x4820; p.counter.idStride = 0x9c; p.counter.offset = 0x958; p.counter.modeFlags = 0x4b59c8;
        p.replay.address = 0x4b77d8; p.replay.offset = 0xc;
        break;
    case 18:
        p.before = {0x435050,"e82b57ffff"}; p.success = {0x42a8f3,"51"}; p.after = {0x435055,"6a00"};
        p.ownerRegister = NativeSpellRegister::Edi; p.ownerOffset = 0x5588;
        p.object = 0x4cf2c0; p.idOffset = 0x74; p.flagsOffset = 0x78;
        p.counter.base = 0x4cf41c; p.counter.shotStride = 0x130f0; p.counter.idStride = 0xdc; p.counter.offset = 0x998;
        p.counter.shotFirst = 0x4cccf4; p.counter.shotSecond = 0x4cccf8; p.counter.modeFlags = 0x4cccc8;
        p.replay.address = 0x4cf418; p.replay.offset = 0xc;
        break;
    case 20:
        p.rva = true; p.before = {0x878b0,"55"}; p.success = {0x87aca,"6a00"}; p.after = {0x87b06,"8be5"};
        p.object = 0x1ba578; p.idOffset = 0x74; p.flagsOffset = 0x78; p.objectArgument = NativeSpellRegister::Ecx;
        p.counter.base = 0x1c6108; p.counter.idStride = 0xe0; p.counter.offset = 0x8b028;
        p.counter.modeFlags = 0x1ba5d8; p.counter.modeMask = 0xffffffffu; p.counter.modeValue = 2;
        p.replay.address = 0x1c60fc;
        break;
    default: p.game = 0; break;
    }
    return p;
}
}
