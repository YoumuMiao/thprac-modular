#pragma once
#include "thprac_boss_capture.h"
#include <cstddef>
#include <vector>
namespace THPrac {
struct BossCaptureContext {
    int version = 0, stage = -1, difficulty = -1, shot = -1;
    int practiceSection = 0, phaseVariant = 0;
    bool replay = false, gameplay = false, survival = false, spellCapturable = true;
    uintptr_t fileTable = 0;
    // Byte offset of the ECL instruction pointer inside the owner object. Older
    // engines keep it in a flat field (TH08: 0x7f8); the 0x0c-era layout used
    // for TH10+ is handled separately, so 0 means "use the version default".
    int instructionOffset = 0;
    uintptr_t bosses[8] {};
};
using BossCaptureContextProvider = BossCaptureContext(*)();
using BossCaptureTransitionObserver = void(*)(bool, bool);
using BossCaptureEligibilityOverride = bool(*)();
void BossCaptureSetTransitionObserver(BossCaptureTransitionObserver, BossCaptureEligibilityOverride);
using BossCaptureLabelProvider = const char*(*)(int section, int difficulty);
void BossCaptureAttach(BossCaptureContextProvider context, BossCaptureLabelProvider label);
void BossCaptureNewRun();
void BossCaptureCancelTracking();
void BossCaptureFailed();
// TH08 reports the collision before its delayed life-loss/deathbomb callbacks.
void BossCapturePlayerHit();
// Authoritative native success, independent of HP/timeout script transitions.
void BossCaptureSpellCaptured(uintptr_t owner);
void BossCaptureFrame();
// TH07 uses separate stage scripts for Extra (7) and Phantasm (8).
inline bool BossCaptureStageMatchesDifficulty(int game, int stage, int difficulty)
{
    if (game != 7 || difficulty < 0)
        return true;
    if (stage == 7)
        return difficulty == 4;
    if (stage == 8)
        return difficulty == 5;
    return difficulty < 4;
}
struct BossCaptureCatalogRow { int stage, attack, kind; };
// A synthetic chapter row in the existing stage/attack/nonspell save format.
inline constexpr int kTh12LatterChapterAttack = 900;
enum class BossCaptureTh12RoadEvent { Start, Checkpoint, AutoClear, PracticeExit, Hit };
void BossCaptureTh12Road(BossCaptureTh12RoadEvent event);
void BossCaptureTh07Stage6Opening(uintptr_t instruction);
enum class BossCaptureMomijiState { Inactive, Fighting, Reinforcements };
BossCaptureMomijiState BossCaptureTh10MomijiStatus();
void BossCaptureTh10MomijiFinish(bool practiceExit);
std::vector<BossCaptureCatalogRow> BossCaptureCatalogRows(int game, int difficulty, int shot);
void BossCaptureOptions();
void BossCaptureConfigured(uintptr_t owner, uintptr_t instruction);
void BossCaptureConfiguredCurrent(uintptr_t owner);
void BossCaptureObservedCurrent(uintptr_t owner);
void BossCaptureEclArguments20(uintptr_t owner);
void BossCaptureCleared(uintptr_t owner, bool timeout = false);
void BossCaptureScriptExit(uintptr_t owner);
uintptr_t BossCaptureCurrentInstruction(uintptr_t owner);
uintptr_t BossCaptureReadPointer(uintptr_t address);
}
