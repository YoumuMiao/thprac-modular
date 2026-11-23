#include <iostream>
#include "thprac_native_spell_profiles.h"
using namespace THPrac;
void site(const char* name, const NativeSpellSite& s) {
    std::cout << '"' << name << "\":{\"address\":" << s.address << ",\"length\":" << s.Length()
              << ",\"bytes\":\"" << s.bytes << "\"},";
}
int main() {
    std::cout << '[';
    bool comma = false;
    for (int game : {6,7,8,10,11,12,13,14,16,17,18,20}) {
        if (comma) std::cout << ','; comma = true;
        const auto p = NativeSpellProfileFor(game);
        std::cout << "{\"game\":" << p.game << ",\"rva\":" << p.rva << ',';
        site("before",p.before); site("success",p.success); site("after",p.after);
        std::cout << "\"owner_register\":" << static_cast<int>(p.ownerRegister) << ",\"owner_offset\":" << p.ownerOffset
                  << ",\"object\":" << p.object << ",\"object_indirect\":" << p.objectIndirect
                  << ",\"id_offset\":" << p.idOffset << ",\"flags_offset\":" << p.flagsOffset
                  << ",\"active_mask\":" << p.activeMask << ",\"active_value\":" << p.activeValue
                  << ",\"existing_success_hook\":" << p.existingSuccessHook
                  << ",\"counter\":{\"base\":" << p.counter.base << ",\"indirect\":" << p.counter.indirect
                  << ",\"offset\":" << p.counter.offset << ",\"id_stride\":" << p.counter.idStride
                  << ",\"shot_stride\":" << p.counter.shotStride << ",\"width\":" << p.counter.width
                  << ",\"shot_first\":" << p.counter.shotFirst << ",\"shot_second\":" << p.counter.shotSecond
                  << ",\"shot_scale\":" << p.counter.shotScale << ",\"shot_width\":" << p.counter.shotWidth
                  << ",\"mode_flags\":" << p.counter.modeFlags << ",\"mode_mask\":" << p.counter.modeMask
                  << ",\"mode_value\":" << p.counter.modeValue << ",\"mode_stride\":" << p.counter.modeStride
                  << ",\"saturation\":" << p.counter.saturation << "},\"replay\":{\"address\":" << p.replay.address
                  << ",\"indirect\":" << p.replay.indirect << ",\"offset\":" << p.replay.offset
                  << ",\"width\":" << p.replay.width << ",\"mask\":" << p.replay.mask
                  << ",\"value\":" << p.replay.value << ",\"nonzero\":" << p.replay.nonzero << "}}";
    }
    std::cout << "]\n";
}
