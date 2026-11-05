#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <distorm.h>
#include <mnemonics.h>

namespace THPrac {
// Discover the native option-to-SoundPlayer copy, rather than relying on one
// executable's addresses. All three stores must agree before admitting a match.
struct NativeVolumeLayout {
    uintptr_t config = 0; // Adjacent signed-byte BGM / effects percentages.
    uintptr_t runtime = 0; // Adjacent int BGM / effects / attenuation cache.
    bool ambiguous = false;
    explicit operator bool() const { return config && runtime && !ambiguous; }
};

template <typename IsWritable>
NativeVolumeLayout FindNativeVolumeLayout(const uint8_t* code, size_t size,
    uintptr_t address, IsWritable isWritable)
{
    NativeVolumeLayout found;
    for (size_t at = 0; at + 7 < size; ++at) {
        // MOVSX r32, byte ptr [absolute]; only decode plausible entry points.
        if (code[at] != 0x0f || code[at + 1] != 0xbe || (code[at + 2] & 0xc7) != 5)
            continue;
        uint32_t config;
        std::memcpy(&config, code + at + 3, sizeof(config));
        if (!isWritable(config, 2))
            continue;
        _DInst instructions[100];
        unsigned count = 0;
        const size_t length = size - at < 256 ? size - at : 256;
        _CodeInfo info {};
        info.codeOffset = address + at;
        info.code = code + at;
        info.codeLen = static_cast<int>(length);
        info.dt = Decode32Bits;
        distorm_decompose(&info, instructions, 100, &count);
        uintptr_t origins[8] {};
        uintptr_t stores[2] {};
        bool minimumStore = false;
        for (unsigned i = 0; i < count; ++i) {
            const auto& ins = instructions[i];
            if (ins.opcode == I_MOVSX && ins.ops[0].type == O_REG
                && ins.ops[1].type == O_DISP && ins.ops[1].size == 8) {
                const unsigned reg = ins.ops[0].index - REGS32_BASE;
                if (reg < 8) origins[reg] = static_cast<uintptr_t>(ins.disp);
            } else if (ins.opcode == I_MOV && ins.ops[0].type == O_DISP) {
                if (ins.ops[1].type == O_REG) {
                    const unsigned reg = ins.ops[1].index - REGS32_BASE;
                    if (reg < 8 && origins[reg] >= config && origins[reg] <= config + 1)
                        stores[origins[reg] - config] = static_cast<uintptr_t>(ins.disp);
                } else if (ins.ops[1].type == O_IMM && stores[0]
                    && ins.disp == stores[0] + 8 && ins.imm.sdword == -10000) {
                    minimumStore = true;
                }
            } else if (ins.ops[0].type == O_REG && (ins.flags & FLAG_DST_WR)) {
                const unsigned reg = ins.ops[0].index - REGS32_BASE;
                if (reg < 8) origins[reg] = 0;
            }
            if (minimumStore && stores[1] == stores[0] + 4
                && isWritable(stores[0], 12)) {
                NativeVolumeLayout candidate { config, stores[0] };
                if (found && (found.config != candidate.config || found.runtime != candidate.runtime))
                    return { 0, 0, true }; // Never write a guessed location.
                found = candidate;
                break;
            }
        }
    }
    return found;
}
}
