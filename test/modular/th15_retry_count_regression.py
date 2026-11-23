"""Run TH15 tracker event regressions without launching/injecting the game.

Extracts the production tracker members and methods. Only persistence is replaced
with an in-memory test sink; Windows memory below is a synthetic game snapshot.
Requires Python 3 and a Windows g++ compiler (or pass --compiler).
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def block(text, start):
    """Return a C++ brace block, ignoring braces inside strings/comments."""
    opening = text.index("{", start)
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for token in tokens.finditer(text, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[start:token.end()]
    raise ValueError("Unclosed C++ block")


def harness():
    source = (ROOT / "extensions/launcher-layout/thprac/src/thprac/thprac_th15.cpp").read_text(encoding="utf-8-sig")
    tracker = block(source, source.index("    class TH15ChapterSave"))
    members = tracker[:tracker.index("        TH15ChapterSave() = default;")]
    methods = []
    for name in (
        "BeginNativeAttempt", "RememberNativeSpellAttempt", "NativeSpellBefore", "NativeSpellSucceeded", "NativeSpellAfter", "BossSectionStage", "MidSectionRuntimeMatches", "ScoreSourceIndex", "RecordCapture",
        "ReadMaxPoint", "SetDisplayFromSnapshot", "HoldPreviousChapterDisplay",
        "IsHoldingPreviousChapterDisplay", "RefreshLiveChapterSnapshot", "QueueCapture",
        "RecordInterruptedAttempt", "ResolvePendingCaptures", "StartBossPhaseAfterEmpty",
        "BeginRunCounterReset", "EndRunCounterReset", "BeginRun", "BeginAttempt", "OnChapterClear", "OnChapterIndexChanged", "Update",
        "Total", "Current", "SectionTotal", "SectionCurrent", "ReplayCurrent",
        "ReplaySectionCurrent", "GetCaptureRateCounts", "OnBluePointCollected",
        "OnBulletClearFinished", "ResetRun", "SetCaptureRateEnabled", "SetOnlyCurrentPracticeChapter",
    ):
        match = re.search(r"^        (?:static )?(?:void|bool|int|int32_t|CaptureRateCounts) "
                          + name + r"\(", tracker, re.MULTILINE)
        if not match:
            raise ValueError(f"Production method missing: {name}")
        methods.append(block(tracker, match.start()))
    counts = block(tracker, tracker.index("        struct CaptureRateCounts")) + ";"
    enums = (ROOT / "extensions/launcher-layout/thprac/src/thprac/thprac_locale_def.h").read_text(encoding="utf-8-sig")
    enum_start = enums.index("    enum th_sections_t", enums.index("namespace TH15 {"))
    enum = block(enums, enum_start) + ";"
    # Both production views use the combined reader. Exercise the real
    # game-start hook body with an update interleaved at its counter reset.
    assert source.count("const auto counts = GetCaptureRateCounts(") == 1
    assert source.count("const auto counts = chapterRate.GetCaptureRateCounts(") == 1
    game_start = source[source.index("EHOOK_DY(th15_game_start"):source.index("EHOOK_DY(th15_bomb_dec")]
    assert "0x4E73F8" not in game_start
    start_body = block(game_start, game_start.index("{"))[1:-1]
    start_body = start_body.replace("ReportPluginStage(nullptr);", "")
    start_body = start_body.replace("TH15ChapterSave::singleton()", "tracker")
    start_body = start_body.replace("TH15InGameInfo::singleton().mBombCount", "testBombs")
    start_body = start_body.replace("TH15InGameInfo::singleton().mMissCount", "testMisses")
    start_body = start_body.replace("THGuiRep::singleton().mRepStatus", "testReplay")
    start_body = start_body.replace("testBombs = 0;", "testBombs = 0; if (testAfterCounterReset) testAfterCounterReset();")
    start_body = start_body.replace("testMisses = 0;", "testMisses = 0; if (testAfterCounterReset) testAfterCounterReset();")
    return r'''
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <functional>
#include <stdexcept>
#include <vector>
#define SINGLETON(name) public:
constexpr uintptr_t PLAYER_PTR = 0x4e9bb8;
template<class T = uintptr_t> T GetMemContent(uintptr_t address) { return *reinterpret_cast<T*>(address); }
bool g_th15CaptureRateEnabled = true;
bool g_th15ScoreRecordingEnabled = true, g_th15ChapterHudEnabled = true;
bool g_th15OnlyCurrentPracticeChapter = false, g_th15OnlyCurrentPracticeChapterThisRun = false;
bool g_th15NativeCaptureRateEnabled = true;
''' + enum + "\n" + block(source, source.index("    static int TH15SpellCardId(")) + "\n" + members + "\n" + counts + r'''
        void TraceNativeSpell(const char*, const NativeSpellFinalize&, uint32_t = 0, bool = false) const {}
        int testSaves = 0, testStatRecords = 0;
        void Load() { loaded = true; }
        void Save() { if (!replayDisplayOnly && g_th15CaptureRateEnabled) ++testSaves; }
        void RecordChapterStats(const PendingCapture&) { ++testStatRecords; }
''' + "\n".join(methods) + "\n};\n" + r'''
int testMisses = 0, testBombs = 0;
bool testReplay = false;
struct { int mode = 1, section = TH15_ST6_BOSS11, stage = 5; } thPracParam;
std::function<void()> testAfterCounterReset;
void TestGameStart(TH15ChapterSave& tracker) {
''' + start_body + r'''
}
void CapturePureSign(TH15ChapterSave& tracker) {
    *reinterpret_cast<uintptr_t*>(0x4e9a70) = 0x600000;
    *reinterpret_cast<uint32_t*>(0x600078) = 3;
    *reinterpret_cast<uint32_t*>(0x600074) = TH15SpellCardId(TH15_ST6_BOSS11, 3) - 1;
    *reinterpret_cast<uintptr_t*>(0x4e9bc4) = 0x610000;
    *reinterpret_cast<int32_t*>(0x61000c) = testReplay;
    *reinterpret_cast<uintptr_t*>(0x4e9bc8) = 0;
    tracker.NativeSpellBefore();
    tracker.NativeSpellSucceeded();
    tracker.NativeSpellAfter();
}
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
void memory(int chapter = 0) {
    std::memset(reinterpret_cast<void*>(0x4e0000), 0, 0x10000);
    *reinterpret_cast<uintptr_t*>(PLAYER_PTR) = 1;
    *reinterpret_cast<int32_t*>(0x4e73f0) = 1;
    *reinterpret_cast<int32_t*>(0x4e73f8) = chapter;
}
int main() {
    if (!VirtualAlloc(reinterpret_cast<void*>(0x4e0000), 0x10000,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 2;
    if (!VirtualAlloc(reinterpret_cast<void*>(0x600000), 0x20000,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return 2;
    try {
        {
            // The game-start hook runs in TH15's stage loader. A GUI update
            // can observe its reset counters before the start callback ends.
            // Exercise the actual hook body, including the failure-only signal.
            for (int failure : {0, 1, 2}) {
                memory(68);
                *reinterpret_cast<int32_t*>(0x4e73f0) = 6;
                *reinterpret_cast<int32_t*>(0x4e7410) = 3;
                auto t = std::make_unique<TH15ChapterSave>();
                t->BeginRun(6, 0, 0, 1, TH15_ST6_BOSS11, 5, 3, 0);
                t->Update(6, 68, failure == 1, failure == 2, 1, TH15_ST6_BOSS11, 5, 3, 0);
                testMisses = failure == 1;
                testBombs = failure == 2;
                testAfterCounterReset = [&] {
                    t->Update(6, 68, testMisses, testBombs, 1, TH15_ST6_BOSS11, 5, 3, 0);
                };
                TestGameStart(*t);
                testAfterCounterReset = {};
                // Arguments can have been sampled before the loader reset.
                t->Update(6, 68, failure == 1, failure == 2, 1, TH15_ST6_BOSS11, 5, 3, 0);
                for (int frame = 0; frame < 10; ++frame)
                    t->Update(6, 68, 0, 0, 1, TH15_ST6_BOSS11, 5, 3, 0);
                auto counts = t->GetCaptureRateCounts(5, 0, TH15_ST6_BOSS11, 3, 0, true);
                check(counts.totalAttempts == 2 && counts.sessionAttempts == 2,
                    "Pure Sign retry adds exactly one attempt across loader/GUI interleaving");
                check(counts.totalPasses == 0 && counts.sessionPasses == 0,
                    "retry does not invent a capture");
                CapturePureSign(*t);
                t->OnChapterClear(0, 0);
                t->OnBulletClearFinished(0, 0);
                counts = t->GetCaptureRateCounts(5, 0, TH15_ST6_BOSS11, 3, 0, true);
                check(counts.totalAttempts == 2 && counts.totalPasses == 1,
                    "retried Pure Sign can settle once");
            }
        }
        {
            memory(68);
            *reinterpret_cast<int32_t*>(0x4e73f0) = 6;
            *reinterpret_cast<int32_t*>(0x4e7410) = 3;
            auto t = std::make_unique<TH15ChapterSave>();
            t->BeginRun(6, 0, 0, 1, TH15_ST6_BOSS11, 5, 3, 0);
            t->Update(6, 68, 0, 0, 1, TH15_ST6_BOSS11, 5, 3, 0);
            CapturePureSign(*t);
                t->OnChapterClear(0, 0);
            t->OnBulletClearFinished(0, 0);
            TestGameStart(*t);
            t->Update(6, 68, 0, 0, 1, TH15_ST6_BOSS11, 5, 3, 0);
            auto counts = t->GetCaptureRateCounts(5, 0, TH15_ST6_BOSS11, 3, 0, true);
            check(counts.totalAttempts == 2 && counts.totalPasses == 1,
                "retry after a capture preserves the previous success");
            // The same notification path must preserve replay isolation.
            testReplay = true;
            TestGameStart(*t);
            t->Update(6, 68, 0, 0, 1, TH15_ST6_BOSS11, 5, 3, 0, true);
            CapturePureSign(*t);
                t->OnChapterClear(0, 0);
            t->OnBulletClearFinished(0, 0);
            counts = t->GetCaptureRateCounts(5, 0, TH15_ST6_BOSS11, 3, 0, true);
            check(counts.totalAttempts == 2 && counts.totalPasses == 1,
                "queued replay start preserves live totals");
            check(counts.sessionAttempts == 1 && counts.sessionPasses == 1,
                "queued replay start records playback only");
            testReplay = false;
        }
        {
            memory(4);
            auto t = std::make_unique<TH15ChapterSave>();
            t->BeginAttempt(1, 4, 0, 0, 0, 0, 0, 3, 0);
            t->Update(1, 4, 0, 0, 0, 0, 0, 3, 0);
            t->OnBluePointCollected();
            t->RefreshLiveChapterSnapshot();
            check(t->lastLiveSnapshot.valid, "old live snapshot exists");
            // Retry without any miss/bomb, while the runtime index is stale.
            t->BeginRun(1, 0, 0, 0, 0, 0, 3, 0);
            check(t->active && t->activeChapter == 0 && t->displayChapter == 0,
                "story retry starts chapter one immediately");
            check(!t->lastLiveSnapshot.valid && !t->transitionSnapshot.valid,
                "retry clears live and held HUD snapshots");
            check(t->activeBluePoints == 0, "retry clears blue point progress");
            for (int i = 0; i < 10; ++i) t->Update(1, 0, 0, 0, 0, 0, 0, 3, 0);
            check(t->total[0][0][0][3][0][0] == 1, "one attempt across repeated frames");
            check(t->total[0][0][4][3][0][0] == 1, "old attempt preserved");
            t->BeginRun(1, 0, 0, 0, 0, 0, 3, 0);
            check(t->total[0][0][0][3][0][0] == 2, "consecutive zero-miss retry counted");
        }
        {
            memory(1);
            auto t = std::make_unique<TH15ChapterSave>();
            t->BeginRun(6, 0, 0, 1, 10602, 5, 3, 0);
            t->Update(6, 1, 0, 0, 1, 10602, 5, 3, 0);
            t->OnChapterClear(0, 0);
            t->OnChapterIndexChanged(6, 2, 0, 0, 1, 3, 0);
            t->OnBulletClearFinished(0, 0);
            check(t->activeChapter == 2 && t->IsHoldingPreviousChapterDisplay(),
                "practice progresses and holds previous chapter normally");
            t->Update(6, 2, 1, 0, 1, 10602, 5, 3, 0);
            t->Update(6, 1, 0, 0, 1, 10602, 5, 3, 0);
            check(t->active && t->activeChapter == 1 && t->displayChapter == 1,
                "counter-reset retry returns to selected practice chapter");
            check(!t->transitionSnapshot.valid && t->activeBluePoints == 0,
                "fallback retry drops held progress");
            check(t->total[1][5][1][3][0][0] == 2 && t->total[1][5][1][3][0][1] == 1,
                "retry adds one attempt and keeps completed capture");
            for (int i = 0; i < 10; ++i) t->Update(6, 1, 0, 0, 1, 10602, 5, 3, 0);
            check(t->total[1][5][1][3][0][0] == 2, "fallback retry is not counted twice");
            t->Update(6, 2, 0, 0, 1, 10603, 5, 3, 0);
            check(t->activeChapter == 2 && t->displayChapter == 2,
                "changed practice selection refreshes immediately");
        }
        {
            memory();
            auto t = std::make_unique<TH15ChapterSave>();
            t->BeginRun(2, 0, 0, 1, TH15_ST2_BOSS1, 1, 3, 0);
            t->Update(2, 40, 1, 0, 1, TH15_ST2_BOSS1, 1, 3, 0);
            t->Update(2, 0, 0, 0, 1, TH15_ST2_BOSS1, 1, 3, 0);
            check(t->active && t->activeIsBossSection && t->activeSection == TH15_ST2_BOSS1,
                "boss practice retry restarts its selected section");
            check(t->sectionTotal[1][1][TH15_ST2_BOSS1][3][0][0] == 2,
                "boss retry adds one attempt");
            t->OnChapterClear(0, 0);
            t->OnBulletClearFinished(0, 0);
            t->OnBulletClearFinished(0, 0);
            check(t->sectionTotal[1][1][TH15_ST2_BOSS1][3][0][1] == 1,
                "restarted boss capture settles exactly once");
        }
        {
            memory();
            auto t = std::make_unique<TH15ChapterSave>();
            t->onlyCurrentPracticeChapter = true;
            t->BeginRun(6, 0, 0, 1, 10602, 5, 3, 0);
            t->Update(6, 1, 1, 0, 1, 10602, 5, 3, 0);
            t->OnChapterClear(1, 0);
            t->OnChapterIndexChanged(6, 2, 1, 0, 1, 3, 0);
            t->Update(6, 2, 1, 0, 1, 10602, 5, 3, 0);
            check(!t->active && t->completedAwaitingNext, "selected-only attempt completed");
            t->Update(6, 1, 0, 0, 1, 10602, 5, 3, 0);
            check(t->active && !t->completedAwaitingNext && !t->practiceFinished,
                "retry after completion clears completion guards");
        }
        {
            for (int stage : {1, 7}) {
                memory(40);
                auto t = std::make_unique<TH15ChapterSave>();
                const int diff = stage == 7 ? 4 : 3;
                t->BeginRun(stage, 0, 0, 0, 0, 0, diff, 0);
                t->Update(stage, 40, 0, 0, 0, 0, 0, diff, 0);
                check(t->active && t->activeChapter == 0 && t->displayChapter == 0,
                    "story/Extra retry ignores stale boss runtime index");
                check(t->total[0][stage - 1][0][diff][0][0] == 1,
                    "story/Extra retry counts the first chapter");
            }
        }
        {
            memory();
            auto t = std::make_unique<TH15ChapterSave>();
            t->BeginRun(1, 0, 0, 1, 10103, 0, 3, 0);
            t->Update(1, 2, 0, 2, 1, 10103, 0, 3, 0);
            t->Update(1, 2, 0, 0, 1, 10103, 0, 3, 0);
            check(t->active && t->activeChapter == 2 && !t->failed,
                "bomb-counter reset restarts practice cleanly");
            check(t->total[1][0][2][3][0][0] == 2,
                "bomb-counter reset adds one attempt");
        }
        {
            memory();
            auto t = std::make_unique<TH15ChapterSave>();
            t->BeginRun(1, 0, 0, 0, 0, 0, 3, 0);
            t->Update(1, 0, 0, 0, 0, 0, 0, 3, 0);
            t->OnChapterClear(0, 0);
            t->OnBulletClearFinished(0, 0);
            t->BeginRun(1, 0, 0, 1, 10101, 0, 3, 0);
            t->Update(1, 0, 1, 0, 1, 10101, 0, 3, 0);
            t->OnChapterClear(1, 0);
            t->OnBulletClearFinished(1, 0);
            const auto c = t->GetCaptureRateCounts(0, 0, 0, 3, 0, false);
            check(c.totalAttempts == 2 && c.totalPasses == 1,
                "story capture and failed practice combine as 1/2");
            check(c.sessionAttempts == 2 && c.sessionPasses == 1,
                "switching modes retains combined session counts");
        }
        {
            memory();
            auto t = std::make_unique<TH15ChapterSave>();
            // Historical saves retain distinct mode buckets.
            for (int mode = 0; mode < 2; ++mode) {
                t->total[mode][0][0][3][0][0] = (mode + 1) * 10;
                t->total[mode][0][0][3][0][1] = mode * 10 + 5;
                t->current[mode][0][0][3][0][0] = mode + 3;
                t->current[mode][0][0][3][0][1] = mode + 1;
                t->sectionTotal[mode][0][TH15_ST1_BOSS1][3][0][0] = mode + 2;
                t->sectionTotal[mode][0][TH15_ST1_BOSS1][3][0][1] = mode + 1;
                t->sectionCurrent[mode][0][TH15_ST1_BOSS1][3][0][0] = mode + 1;
                t->sectionCurrent[mode][0][TH15_ST1_BOSS1][3][0][1] = mode;
            }
            auto c = t->GetCaptureRateCounts(0, 0, 0, 3, 0, false);
            check(c.totalAttempts == 30 && c.totalPasses == 20, "merge historical chapter totals");
            check(c.sessionAttempts == 7 && c.sessionPasses == 3, "merge session chapter counts");
            auto b = t->GetCaptureRateCounts(0, 0, TH15_ST1_BOSS1, 3, 0, true);
            check(b.totalAttempts == 5 && b.totalPasses == 3, "merge historical section totals");
            check(b.sessionAttempts == 3 && b.sessionPasses == 1, "merge session section counts");
            auto other = t->GetCaptureRateCounts(0, 0, 0, 2, 0, false);
            check(other.totalAttempts == 0, "difficulty stays separate");
            other = t->GetCaptureRateCounts(0, 0, 0, 3, 1, false);
            check(other.totalAttempts == 0, "shot stays separate");
            t->BeginRun(1, 0, 0, 0, 0, 0, 3, 0, true);
            t->Update(1, 0, 0, 0, 0, 0, 0, 3, 0, true);
            t->OnChapterClear(0, 0);
            t->OnBulletClearFinished(0, 0);
            c = t->GetCaptureRateCounts(0, 0, 0, 3, 0, false);
            check(c.totalAttempts == 30 && c.totalPasses == 20, "replay does not write live totals");
            check(c.sessionAttempts == 1 && c.sessionPasses == 1, "replay shows playback-only counters");
            check(t->current[0][0][0][3][0][0] == 3 && t->current[1][0][0][3][0][0] == 4,
                "replay preserves both live session buckets");
            check(t->testSaves == 0, "replay never saves capture rates");
        }
        std::cout << "PASS: " << checks << " TH15 tracker regression checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
'''


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=shutil.which("g++"))
    parser.add_argument("--output", type=Path, default=ROOT / "Release/th15-chapter-rate-regression")
    args = parser.parse_args()
    if not args.compiler:
        parser.error("g++ is required; pass --compiler with its path")
    args.output.mkdir(parents=True, exist_ok=True)
    cpp, exe = args.output / "tracker.cpp", args.output / "tracker.exe"
    cpp.write_text(harness(), encoding="utf-8")
    subprocess.run([args.compiler, "-std=c++20", "-O0", "-g", "-static", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
