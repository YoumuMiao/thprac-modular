#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include "thprac_locale_def.h"
#include "thprac_boss_capture_runtime.h"
namespace THPrac {
enum class Addon { Capture };
bool capturePluginEnabled = true;
bool AddonEnabled(Addon) { return capturePluginEnabled; }
#include "thprac_boss_capture_runtime.inl"
}
namespace THPrac::BossCapture { void DrawSettingsSection() {} }
using namespace THPrac;
namespace {
BossCaptureContext ctx;
constexpr uintptr_t owner = 0x12340000, other = 0x22340000;
int checks = 0;
void check(bool value) { ++checks; if (!value) throw checks; }
BossCaptureContext Context() { return ctx; }
const char* Label(int, int) { return "native event regression"; }
BossCapture::Key key(int attack = 20, int kind = BossCapture::kSpell) { return {ctx.version, ctx.stage + 1, attack, kind, ctx.difficulty, ctx.shot}; }
BossCapture::Counts counts(int attack = 20, int kind = BossCapture::kSpell) {
    const auto& all = BossCapture::Instance().Storage().Total();
    const auto it = all.find(key(attack, kind));
    return it == all.end() ? BossCapture::Counts{} : it->second;
}
void activate(int attack = 20, int kind = BossCapture::kSpell) {
    CaptureConfiguration configured {key(attack, kind), captureFailures, true};
    captureConfigured[owner] = configured;
    CaptureActivate(owner, configured);
}
void fresh(int kind = BossCapture::kSpell, int game = 14) {
    capturePluginEnabled = true;
    ctx = {}; ctx.version = game; ctx.stage = 2; ctx.difficulty = 3; ctx.shot = 5;
    ctx.gameplay = true; ctx.bosses[0] = owner;
    BossCaptureAttach(Context, Label);
    BossCapture::Instance().Storage().SetEnabled(true);
    BossCaptureNewRun();
    BossCapture::Instance().Storage().ClearAll();
    activate(20, kind);
}
void flush() { CaptureFlush(); }
}
int main() {
    try {
        fresh();
        BossCaptureCleared(owner); BossCaptureScriptExit(owner); flush();
        check(counts().attempts == 1 && counts().captures == 0); // HP is not native capture

        fresh(); BossCaptureFailed();
        auto event = BossCaptureNativeSpellBefore(owner);
        check(event.valid && event.key == key() && event.owner == owner);
        BossCaptureNativeSpellAfter(event, true); BossCaptureFailed(); flush();
        check(counts().attempts == 1 && counts().captures == 1); // native result beats late failure
        BossCaptureNativeSpellAfter(event, true); flush();
        check(counts().captures == 1); // duplicate callback

        fresh(); event = BossCaptureNativeSpellBefore(owner);
        BossCaptureNativeSpellAfter(event, false); flush();
        check(counts().attempts == 1 && counts().captures == 0);

        fresh(); event = BossCaptureNativeSpellBefore(owner);
        BossCaptureCleared(owner); activate(21, BossCapture::kNonSpell);
        BossCaptureNativeSpellAfter(event, true); flush();
        check(counts().captures == 1 && counts(21, BossCapture::kNonSpell).attempts == 1);
        check(BossCapture::Instance().ActiveKey() == key(21, BossCapture::kNonSpell));

        fresh(); BossCaptureCleared(owner); activate(21, BossCapture::kNonSpell);
        event = BossCaptureNativeSpellBefore(owner); // native end follows next-pattern opcode
        check(event.valid && event.key == key());
        BossCaptureNativeSpellAfter(event, true); BossCaptureNativeSpellAfter(event, false); flush();
        check(counts().captures == 1); // duplicate failure cannot downgrade native capture

        fresh(); event = BossCaptureNativeSpellBefore(owner);
        BossCaptureNativeSpellAfter(event, false);
        activate(22, BossCapture::kSpell);
        event = BossCaptureNativeSpellBefore(owner);
        check(event.valid && event.key == key(22)); // a failed native result is resolved too
        BossCaptureNativeSpellAfter(event, true); flush();
        check(counts().captures == 0 && counts(22).captures == 1);

        fresh(); check(!BossCaptureNativeSpellBefore(other).valid);
        ctx.replay = true; check(!BossCaptureNativeSpellBefore(owner).valid);
        ctx.replay = false;
        event = BossCaptureNativeSpellBefore(owner); ctx.replay = true;
        BossCaptureNativeSpellAfter(event, true); flush(); check(counts().captures == 0);

        fresh(); event = BossCaptureNativeSpellBefore(owner); ++ctx.shot;
        BossCaptureNativeSpellAfter(event, true); flush(); check(counts().captures == 0);

        fresh(); event = BossCaptureNativeSpellBefore(owner); ++captureFrameNumber;
        BossCaptureNativeSpellAfter(event, true); flush(); check(counts().captures == 0);

        fresh(); BossCapture::Instance().Storage().SetEnabled(false);
        check(!BossCaptureNativeSpellBefore(owner).valid);

        fresh(); event = BossCaptureNativeSpellBefore(owner); capturePluginEnabled = false;
        BossCaptureNativeSpellAfter(event, true); capturePluginEnabled = true;
        flush(); check(counts().captures == 0);

        fresh(BossCapture::kNonSpell);
        check(!BossCaptureNativeSpellBefore(owner).valid);
        BossCaptureCleared(owner); BossCaptureScriptExit(owner); flush();
        check(counts(20, BossCapture::kNonSpell).captures == 1);

        for (int game : {6,7,8,10,11,12,13,14,16,17,18,20}) {
            fresh(BossCapture::kSpell, game);
            event = BossCaptureNativeSpellBefore(0);
            check(event.valid && event.key.game == game);
            BossCaptureNativeSpellAfter(event, true); flush();
            check(counts().attempts == 1 && counts().captures == 1);
            fresh(BossCapture::kSpell, game);
            BossCaptureCleared(owner); BossCaptureScriptExit(owner); flush();
            check(counts().captures == 0); // no native success, no inferred capture
            fresh(BossCapture::kSpell, game);
            event = BossCaptureNativeSpellBefore(owner);
            BossCaptureNativeSpellAfter(event, false); flush();
            check(counts().attempts == 1 && counts().captures == 0);
        }
        fresh(); ctx.bosses[0] = 0;
        check(BossCaptureNativeSpellBefore(0).valid); // outgoing owner survives boss-slot teardown
        ctx.version = 9; check(!BossCaptureNativeSpellBefore(owner).valid);
        ctx.version = 19; check(!BossCaptureNativeSpellBefore(owner).valid);

        std::array<uint32_t,128> manager {};
        std::array<uint32_t,32> enemyA {}, enemyB {}, enemyC {};
        struct Node { uintptr_t owner, next; } a {}, b {}, cc {};
        const auto mp = reinterpret_cast<uintptr_t>(manager.data());
        const auto ap = reinterpret_cast<uintptr_t>(enemyA.data());
        const auto bp = reinterpret_cast<uintptr_t>(enemyB.data());
        const auto cp = reinterpret_cast<uintptr_t>(enemyC.data());
        enemyA[8]=42; enemyB[8]=43; enemyC[8]=42;
        a={ap,0}; b={bp,reinterpret_cast<uintptr_t>(&a)}; cc={cp,reinterpret_cast<uintptr_t>(&b)};
        manager[0x48/4]=42; manager[0x48/4+1]=43; manager[0x100/4]=reinterpret_cast<uintptr_t>(&b);
        uintptr_t resolved[8] {};
        BossCaptureReadBossHandles(16,mp,0x48,0x100,0x20,resolved);
        check(resolved[0]==ap && resolved[1]==bp); // slot order, not list order
        enemyA[8]=99; manager[0x100/4]=reinterpret_cast<uintptr_t>(&cc);
        BossCaptureReadBossHandles(16,mp,0x48,0x100,0x20,resolved);
        check(resolved[0]==cp && resolved[1]==bp); // reused pointer rejected by native ID
        manager[0x48/4]=77;
        BossCaptureReadBossHandles(16,mp,0x48,0x100,0x20,resolved);
        check(resolved[0]==0 && resolved[1]==bp); // no match must never return last entity
        manager[0x48/4]=42;
        ++captureFrameNumber; manager[0x100/4]=reinterpret_cast<uintptr_t>(&b);
        BossCaptureReadBossHandles(16,mp,0x48,0x100,0x20,resolved);
        check(resolved[0]==0); // removed entity cannot survive into next frame's cache
        BossCaptureReadBossHandles(16,0,0x48,0x100,0x20,resolved);
        check(resolved[0]==0 && resolved[1]==0);

        for (int game : {6,7,8}) {
            const unsigned first=game==8?8:4, subs=game==6?0x10:game==7?0x44:0x48;
            std::vector<uint8_t> original(subs+8,0);
            original[game==8?4:0]=2;
            uint32_t value=0x80; std::memcpy(original.data()+first,&value,4);
            value=0x20; std::memcpy(original.data()+subs,&value,4);
            value=0x40; std::memcpy(original.data()+subs+4,&value,4);
            CaptureFile file{};file.version=game;file.headerSize=original.size();
            std::memcpy(file.prefix,original.data(),16);
            auto relocated=original;
            const uintptr_t base=reinterpret_cast<uintptr_t>(relocated.data());
            for(unsigned at=first;at<relocated.size();at+=4){
                std::memcpy(&value,original.data()+at,4);
                if(value || game!=6)value+=static_cast<uint32_t>(base);
                std::memcpy(relocated.data()+at,&value,4);
            }
            check(CaptureHeaderRelocated(file,base));
            bool equal=true;
            for(size_t at=0;at<original.size();++at)
                equal &= CaptureHeaderByte(file,base,at,true)==original[at];
            check(equal && CaptureHeaderPrefixMatches(file,base,true));
        }

        std::cout << "PASS: " << checks << " native-event attribution checks\n";
    } catch (int n) { std::cerr << "FAIL check " << n << '\n'; return 1; }
}
