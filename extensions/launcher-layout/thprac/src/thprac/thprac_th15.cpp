#include "feature_runtime.h"
#include "thprac_games.h"
#include "thprac_utils.h"
#include "thprac_launcher_cfg.h"
#include "thprac_th15_abtest.h"
#include "thprac_th15_route_clock.h"
#include "thprac_igi_key_render.h"
#include "thprac_capture_table.h"
#include <format>
#include <numbers>
#include <cmath>
#include <fstream>
#include <limits>
#include <algorithm>
#include <array>
#include <vector>
#include <cstring>
#include <sstream>
#include <map>
#include <tuple>
#include <d3d9.h>

#ifndef THPRAC_TH15_HITBOX_FEATURE
#define THPRAC_TH15_HITBOX_FEATURE 1
#endif

#if THPRAC_TH15_HITBOX_FEATURE
namespace THPrac { extern DWORD* g_gameGuiDevice; }
#endif

namespace THPrac {
namespace TH15 {
    enum addrs {
        CHARACTER = 0x4e7404,
        BOMB_PTR = 0x4e9a68,
        PLAYER_PTR = 0x4e9bb8,
    };

      enum funcs {
        COPY_ANM_VM_FROM_LOADED = 0x40b880,
        ANM_VM_RUN = 0x477e10,
        APPEND_ANM_VM_TO_WORLD_LIST = 0x487bd0,
        ANM_VM_INTERRUPT_TREE = 0x488620,
        ALLOCATE_ANM_VM = 0x489460,
    };


    int g_lock_timer = 0;
    bool g_lock_timer_flag = false;
    static bool g_th15OnlyCurrentPracticeChapter = false;
    static bool g_th15OnlyCurrentPracticeChapterThisRun = false;
    static bool g_th15CaptureRateEnabled = true;
    static bool g_th15NativeCaptureRateEnabled = true;
    static bool g_th15ScoreRecordingEnabled = true;
    static bool g_th15ChapterHudEnabled = true;
    static bool g_th15BossHpHudEnabled = true;
    static bool g_th15BossPositionHudEnabled = true;
    static bool g_th15PlayerPositionHudEnabled = true;
    static bool g_th15CustomBossHpEnabled = true;
    static unsigned g_th15BossHpRuntimeLogCount = 0;
    static constexpr int kTh15St2Boss3HpSwitchThreshold = 2400;
    static constexpr int kTh15St2Boss3OriginalLife = 13400;
    static constexpr int kTh15St2Boss3EffectiveHpMax =
        kTh15St2Boss3OriginalLife - kTh15St2Boss3HpSwitchThreshold;

    static void TH15BossHpLog(const char* event, int hp,
        uint32_t value1 = 0, uint32_t value2 = 0,
        uint32_t value3 = 0, uint32_t value4 = 0, uint32_t context = 0)
    {
        char appData[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("APPDATA", appData, MAX_PATH))
            return;

        char aliceDirectory[MAX_PATH] = {};
        char directory[MAX_PATH] = {};
        char path[MAX_PATH] = {};
        if (sprintf_s(aliceDirectory, "%s\\ShanghaiAlice", appData) < 0
            || sprintf_s(directory, "%s\\ShanghaiAlice\\th15", appData) < 0
            || sprintf_s(path, "%s\\ShanghaiAlice\\th15\\thprac_bosshp.log", appData) < 0)
            return;
        CreateDirectoryA(aliceDirectory, nullptr);
        CreateDirectoryA(directory, nullptr);

        SYSTEMTIME now{};
        GetLocalTime(&now);
        std::ofstream log(path, std::ios::out | std::ios::app);
        if (log.is_open()) {
            log << std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} event={} hp={} value1={:#010x} value2={:#010x} value3={:#010x} value4={:#010x} context={:#010x}\n",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                now.wMilliseconds, event, hp, value1, value2, value3, value4, context);
        }
    }

    static uint32_t TH15ECLReadU32(ECLHelper& ecl, size_t offset)
    {
        uint32_t value = 0;
        ecl.SetPos(offset);
        ecl >> value;
        return value;
    }
    // scoreth15.dat layout follows TH15's CR records. Each character has two
    // game-mode blocks containing the same 119 spell-card records.
    class TH15NativeSpellSave {
        struct Count { int attempts = 0; int captures = 0; };
        std::array<std::array<Count, 119>, 4> cards{};
        ULONGLONG lastRead = 0;
        bool available = false;

        static uint32_t U32(const std::vector<uint8_t>& data, size_t offset)
        {
            return uint32_t(data[offset]) | (uint32_t(data[offset + 1]) << 8)
                | (uint32_t(data[offset + 2]) << 16) | (uint32_t(data[offset + 3]) << 24);
        }

        bool Read()
        {
            char appData[MAX_PATH] = {};
            if (!GetEnvironmentVariableA("APPDATA", appData, MAX_PATH)) return false;
            char path[MAX_PATH] = {};
            if (sprintf_s(path, "%s\\ShanghaiAlice\\th15\\scoreth15.dat", appData) < 0) return false;
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) return false;
            const auto length = file.tellg();
            if (length < 24 || length > 4 * 1024 * 1024) return false;
            std::vector<uint8_t> source(static_cast<size_t>(length));
            file.seekg(0);
            if (!file.read(reinterpret_cast<char*>(source.data()), source.size())) return false;
            if (std::memcmp(source.data(), "TH51", 4) != 0
                || U32(source, 4) != source.size()
                || U32(source, 16) != source.size() - 24) return false;
            const size_t decodedSize = U32(source, 20);
            if (decodedSize == 0 || decodedSize > 4 * 1024 * 1024) return false;

            std::vector<uint8_t> decrypted(source.begin() + 24, source.end());
            size_t decryptSize = decrypted.size();
            size_t tail = decryptSize % 16;
            if (tail >= 4) tail = 0;
            tail += decryptSize % 2;
            decryptSize -= tail;
            uint8_t key = 0xAC;
            for (size_t pos = 0; pos < decryptSize; pos += 16) {
                const size_t block = std::min<size_t>(16, decryptSize - pos);
                uint8_t output[16] = {};
                size_t input = 0;
                for (size_t parity = 0; parity < 2; ++parity) {
                    for (size_t i = 0; i < (block - parity + 1) / 2; ++i) {
                        output[block - parity - 1 - i * 2] = decrypted[pos + input++] ^ key;
                        key = uint8_t(key + 0x35);
                    }
                }
                std::memcpy(decrypted.data() + pos, output, block);
            }

            std::vector<uint8_t> decoded;
            decoded.reserve(decodedSize);
            std::array<uint8_t, 0x2000> dictionary{};
            size_t dictionaryPos = 1, bitPos = 0;
            auto bits = [&](int count, uint32_t& value) {
                if (bitPos + count > decrypted.size() * 8) return false;
                value = 0;
                for (int i = 0; i < count; ++i, ++bitPos)
                    value = (value << 1) | ((decrypted[bitPos / 8] >> (7 - bitPos % 8)) & 1);
                return true;
            };
            while (decoded.size() < decodedSize) {
                uint32_t flag = 0, value = 0;
                if (!bits(1, flag)) return false;
                if (flag) {
                    if (!bits(8, value)) return false;
                    decoded.push_back(uint8_t(value));
                    dictionary[dictionaryPos] = uint8_t(value);
                    dictionaryPos = (dictionaryPos + 1) & 0x1FFF;
                } else {
                    if (!bits(13, value) || value == 0) return false;
                    const size_t offset = value;
                    if (!bits(4, value)) return false;
                    for (uint32_t i = 0; i < value + 3; ++i) {
                        if (decoded.size() >= decodedSize) return false;
                        const uint8_t byte = dictionary[(offset + i) & 0x1FFF];
                        decoded.push_back(byte);
                        dictionary[dictionaryPos] = byte;
                        dictionaryPos = (dictionaryPos + 1) & 0x1FFF;
                    }
                }
            }

            std::array<std::array<Count, 119>, 4> next{};
            std::array<bool, 4> found{};
            for (size_t pos = 0; pos + 12 <= decoded.size();) {
                const size_t chunkSize = U32(decoded, pos + 8);
                if (chunkSize < 12 || chunkSize > decoded.size() - pos) return false;
                if (decoded[pos] == 'C' && decoded[pos + 1] == 'R' && chunkSize == 0xA4A0) {
                    const uint32_t character = U32(decoded, pos + 12);
                    if (character < 4) {
                        found[character] = true;
                        constexpr size_t modeSize = 20872;
                        constexpr size_t cardOffset = 2240;
                        constexpr size_t cardSize = 156;
                        for (size_t mode = 0; mode < 2; ++mode) {
                            const size_t first = pos + 16 + mode * modeSize + cardOffset;
                            if (first + 119 * cardSize > pos + chunkSize) return false;
                            for (size_t index = 0; index < 119; ++index) {
                                const size_t card = first + index * cardSize;
                                const uint32_t id = U32(decoded, card + 144);
                                if (id >= 119) return false;
                                auto& count = next[character][id];
                                count.captures += U32(decoded, card + 128) + U32(decoded, card + 132);
                                count.attempts += U32(decoded, card + 136) + U32(decoded, card + 140);
                            }
                        }
                    }
                }
                pos += chunkSize;
            }
            if (!std::all_of(found.begin(), found.end(), [](bool value) { return value; })) return false;
            cards = next;
            return true;
        }

    public:
        bool Get(int player, int cardId, int& captures, int& attempts)
        {
            const ULONGLONG now = GetTickCount64();
            if (!lastRead || now - lastRead >= 2000) {
                lastRead = now;
                available = Read();
            }
            if (!available || player < 0 || player >= 4 || cardId < 1 || cardId > 119)
                return false;
            const auto& count = cards[player][cardId - 1];
            captures = count.captures;
            attempts = count.attempts;
            return true;
        }
    };

    static int TH15SpellCardId(int section, int diff)
    {
        if (section >= TH15_ST7_MID1 && section <= TH15_ST7_MID3)
            return 107 + section - TH15_ST7_MID1;
        if (section == TH15_ST1_MID3 && diff >= 2) return diff - 1;
        if (section == TH15_ST3_MID2) return 23 + diff;
        if (section == TH15_ST1_BOSS2) return 3 + diff;
        if (section == TH15_ST1_BOSS4) return 7 + diff;
        if (section >= TH15_ST2_BOSS2 && section <= TH15_ST2_BOSS6 && (section - TH15_ST2_BOSS2) % 2 == 0)
            return 11 + (section - TH15_ST2_BOSS2) / 2 * 4 + diff;
        if (section == TH15_ST3_BOSS2) return 27 + diff;
        if (section == TH15_ST3_BOSS4) return 31 + diff;
        if (section == TH15_ST3_BOSS6) return 35 + diff;
        if (section == TH15_ST3_BOSS7) return 39 + diff;
        if (section >= TH15_ST4_BOSS2 && section <= TH15_ST4_BOSS6 && (section - TH15_ST4_BOSS2) % 2 == 0)
            return 43 + (section - TH15_ST4_BOSS2) / 2 * 4 + diff;
        if (section == TH15_ST4_BOSS7) return 55 + diff;
        if (section >= TH15_ST5_BOSS2 && section <= TH15_ST5_BOSS6 && (section - TH15_ST5_BOSS2) % 2 == 0)
            return 59 + (section - TH15_ST5_BOSS2) / 2 * 4 + diff;
        if (section == TH15_ST5_BOSS7) return 71 + diff;
        if (section == TH15_ST5_BOSS8) return 75 + diff;
        if (section >= TH15_ST6_BOSS2 && section <= TH15_ST6_BOSS8 && (section - TH15_ST6_BOSS2) % 2 == 0)
            return 79 + (section - TH15_ST6_BOSS2) / 2 * 4 + diff;
        if (section == TH15_ST6_BOSS9) return 95 + diff;
        if (section == TH15_ST6_BOSS10) return 99 + diff;
        if (section == TH15_ST6_BOSS11) return 103 + diff;
        if (section >= TH15_ST7_END_S1 && section <= TH15_ST7_END_S8)
            return 110 + (section - TH15_ST7_END_S1) / 2;
        if (section == TH15_ST7_END_S9) return 118;
        if (section == TH15_ST7_END_S10) return 119;
        return 0;
    }

    // Stage is zero-based; chapter is the one-based number in the practice menu.
    static const char* TH15ChapterNickname(int stage, int chapter)
    {
        if (stage == 4) {
            switch (chapter) {
            case 6: return "白激光";
            case 7: return "三吴克";
            case 8: return "黄绿激光";
            case 9: return "太田飞行阵";
            case 10: return "黄绿蓝";
            }
        }
        if (stage == 5 && chapter == 3) return "流星雨";
        return nullptr;
    }

    // Keep combat/practice counters for save compatibility; both views sum them.
    // [combat/practice][stage][chapter][difficulty][shot type][attempt/capture].
    class TH15ChapterSave {
        SINGLETON(TH15ChapterSave)
        static constexpr int kChapterCounts[7] = { 6, 6, 6, 6, 10, 4, 9 };
        static constexpr int kFirstEndBossSections[7] = {
            TH15_ST1_BOSS1, TH15_ST2_BOSS1, TH15_ST3_BOSS1, TH15_ST4_BOSS1,
            TH15_ST5_BOSS1, TH15_ST6_BOSS1, TH15_ST7_END_NS1
        };
        int32_t total[2][7][10][5][4][2] = {};
        int32_t current[2][7][10][5][4][2] = {};
        // Replay attempts are visible only during playback and never enter the
        // saved totals or the live game's current-session counters.
        int32_t replayCurrent[2][7][10][5][4][2] = {};
        // Keep boss non-spells and spell cards separate from the built-in
        // chapter selector records. The enum value is a stable TH15 section id.
        int32_t sectionTotal[2][7][75][5][4][2] = {};
        int32_t sectionCurrent[2][7][75][5][4][2] = {};
        int32_t replaySectionCurrent[2][7][75][5][4][2] = {};
        struct ChapterStatBest {
            int32_t bluePoints = 0;
            int32_t graze = 0;
            int32_t maxPoint = 0;
            int32_t maxPointDelta = 0;
            int64_t chapterScore = 0;
            int64_t scoreEnd = 0;
            int64_t postSettlementScore = 0;
            bool hasBluePoints = false;
            bool hasGraze = false;
            bool hasMaxPoint = false;
            bool hasMaxPointDelta = false;
            bool hasChapterScore = false;
            bool hasScoreEnd = false;
            bool hasPostSettlementScore = false;
        };
        struct LiveChapterSnapshot {
            bool valid = false;
            int source = 0, mode = 0, stage = 0, chapter = 0, section = 0, diff = 0, player = 0;
            bool bossSection = false;
            int32_t bluePoints = 0, graze = 0, maxPointDelta = 0;
            int64_t chapterScore = 0, currentScore = 0;
            double resource = 0.0, killRatePercent = 0.0;
        };
        struct ManualChapterGoal {
            int32_t bluePoints = -1;
            int32_t graze = -1;
            int32_t maxPointDelta = -1;
            int64_t chapterScore = -1;
            bool HasAny() const
            {
                return bluePoints >= 0 || graze >= 0 || maxPointDelta >= 0 || chapterScore >= 0;
            }
        };
        ChapterStatBest chapterStatBest[3][7][10][5][4] = {};
        ChapterStatBest sectionStatBest[3][7][75][5][4] = {};
        LiveChapterSnapshot lastLiveSnapshot{};
        LiveChapterSnapshot transitionSnapshot{};
        ULONGLONG transitionSnapshotUntil = 0;
        static constexpr ULONGLONG kChapterDisplayHoldMs = 2000;
        ManualChapterGoal manualGoals[7][10][5][4] = {};
        ManualChapterGoal sectionManualGoals[7][75][5][4] = {};
        bool chapterStatsLoaded = false;
        bool chapterGoalsLoaded = false;
        int chapterGoalMode = 0; // Auto from all saved records, or manually entered.
        struct PendingCapture {
            int mode, stage, chapter, section, diff, player;
            int32_t missesAtStart, bombsAtStart;
            int32_t grazeAtStart, killsAtStart, enemiesAtStart;
            int32_t scoreAtStart;
            int32_t grazeAtSettlement, killsAtSettlement, enemiesAtSettlement, bluePoints;
            int32_t maxPointAtStart, maxPointAtSettlement, scoreAtSettlement;
            int32_t missesAtSettlement, bombsAtSettlement;
            bool chapterCleared;
            bool bossSection, replay;
        };
        std::vector<PendingCapture> pendingCaptures;
        bool loaded = false;
        bool active = false;
        bool failed = false;
        bool settlementPending = false;
        bool settlementCaptured = false;
        bool bossIntroPending = false;
        bool bossGapPending = false;
        bool practiceFinished = false;
        bool completedAwaitingNext = false;
        int finishedPracticeStage = -1, finishedPracticeChapter = -1;
        int activeMode = 0, activeStage = 0, activeChapter = 0, activeDiff = 0, activePlayer = 0;
        int activeRuntimeStage = 0, activeRuntimeChapter = 0;
        bool activeRuntimeValid = false;
        bool activeIsBossSection = false;
        int activeSection = 0;
        int settledRuntimeChapter = -1;
        int activePracticeSection = 0, activePracticeMenuStage = 0;
        bool activeObservedGameplay = false;
        bool displayValid = false;
        bool replayDisplayOnly = false;
        bool captureRateEnabled = true;
        bool captureRateResumePending = false;
        bool onlyCurrentPracticeChapter = false;
        bool hudChapterViewLoaded = false;
        int displayMode = 0, displayStage = 0, displayChapter = 0, displayDiff = 0, displayPlayer = 0;
        bool displayIsBossSection = false;
        int displaySection = 0;
        int32_t missesAtStart = 0, bombsAtStart = 0;
        int32_t grazeAtStart = 0, killsAtStart = 0, enemiesAtStart = 0;
        int32_t scoreAtStart = 0, maxPointAtStart = 0, activeBluePoints = 0;
        int32_t lastMisses = 0, lastBombs = 0;
        TH15ChapterSave() = default;

        static int BossSectionStage(int section)
        {
            if (section >= TH15_ST1_MID1 && section <= TH15_ST1_MID3) return 0;
            if (section == TH15_ST2_MID1) return 1;
            if (section >= TH15_ST3_MID1 && section <= TH15_ST3_MID2) return 2;
            if (section == TH15_ST4_MID1) return 3;
            if (section >= TH15_ST1_BOSS1 && section <= TH15_ST1_BOSS4) return 0;
            if (section >= TH15_ST2_BOSS1 && section <= TH15_ST2_BOSS6) return 1;
            if (section >= TH15_ST3_BOSS1 && section <= TH15_ST3_BOSS7) return 2;
            if (section >= TH15_ST4_BOSS1 && section <= TH15_ST4_BOSS7) return 3;
            if (section >= TH15_ST5_BOSS1 && section <= TH15_ST5_BOSS8) return 4;
            if (section >= TH15_ST6_BOSS1 && section <= TH15_ST6_BOSS11) return 5;
            if (section >= TH15_ST7_MID1 && section <= TH15_ST7_END_S10) return 6;
            return -1;
        }

        static bool MidSectionRuntimeMatches(int section, int chapter)
        {
            switch (section) {
            case TH15_ST1_MID1: return chapter == 2;
            case TH15_ST1_MID2: return chapter == 4;
            case TH15_ST1_MID3: return chapter == 31;
            case TH15_ST2_MID1: return chapter == 3;
            case TH15_ST3_MID1: return chapter == 3;
            case TH15_ST3_MID2: return chapter == 31;
            // The practice jump suppresses its chapter write and leaves the
            // runtime index at zero until the midboss ends.
            case TH15_ST4_MID1: return chapter == 0 || chapter == 3;
            default: return false;
            }
        }

        static int ScoreSourceIndex(bool replay, int mode)
        {
            if (replay) return 0; // Replay
            return mode == 0 ? 1 : 2; // Combat / Practice
        }

        void MergeChapterBest(int source, int stage, int chapter, int diff, int player,
            bool hasBlue, int32_t blue, bool hasGraze, int32_t graze,
            bool hasMaxPoint, int32_t maxPoint, bool hasChapterScore, int64_t chapterScore,
            bool hasPostSettlementScore, int64_t postSettlementScore,
            int section = 0, int64_t scoreEnd = -1)
        {
            if (source < 0 || source >= 3 || stage < 0 || stage >= 7
                || (section ? (section < 1 || section >= 75 || BossSectionStage(section) != stage)
                            : (chapter < 0 || chapter >= 10)) || diff < 0 || diff >= 5
                || player < 0 || player >= 4)
                return;
            auto& best = section ? sectionStatBest[source][stage][section][diff][player]
                                 : chapterStatBest[source][stage][chapter][diff][player];
            if (hasBlue) {
                best.bluePoints = best.hasBluePoints ? std::max(best.bluePoints, blue) : blue;
                best.hasBluePoints = true;
            }
            if (hasGraze) {
                best.graze = best.hasGraze ? std::max(best.graze, graze) : graze;
                best.hasGraze = true;
            }
            if (hasMaxPoint) {
                best.maxPoint = best.hasMaxPoint ? std::max(best.maxPoint, maxPoint) : maxPoint;
                best.hasMaxPoint = true;
            }
            if (hasChapterScore) {
                best.chapterScore = best.hasChapterScore
                    ? std::max(best.chapterScore, chapterScore) : chapterScore;
                best.hasChapterScore = true;
            }
            if (scoreEnd >= 0) {
                best.scoreEnd = best.hasScoreEnd ? std::max(best.scoreEnd, scoreEnd) : scoreEnd;
                best.hasScoreEnd = true;
            }
            if (hasPostSettlementScore) {
                best.postSettlementScore = best.hasPostSettlementScore
                    ? std::max(best.postSettlementScore, postSettlementScore) : postSettlementScore;
                best.hasPostSettlementScore = true;
            }
        }

        void MergeChapterMaxPointDelta(int source, int stage, int chapter, int diff,
            int player, int32_t maxPointDelta, int section = 0)
        {
            if (source < 0 || source >= 3 || stage < 0 || stage >= 7
                || (section ? (section < 1 || section >= 75 || BossSectionStage(section) != stage)
                            : (chapter < 0 || chapter >= 10)) || diff < 0 || diff >= 5
                || player < 0 || player >= 4)
                return;
            auto& best = section ? sectionStatBest[source][stage][section][diff][player]
                                 : chapterStatBest[source][stage][chapter][diff][player];
            best.maxPointDelta = best.hasMaxPointDelta
                ? std::max(best.maxPointDelta, maxPointDelta) : maxPointDelta;
            best.hasMaxPointDelta = true;
        }

        void LoadChapterStatFile(const char* path, bool legacy)
        {
            std::ifstream file(path);
            std::string line;
            std::getline(file, line);
            while (std::getline(file, line)) {
                if (line.empty()) continue;
                std::array<std::string, 13> cells{};
                std::stringstream row(line);
                for (auto& cell : cells) std::getline(row, cell, '\t');
                try {
                    int source = -1;
                    int stage = 0, chapter = 0, section = 0, diff = 0, player = 0;
                    int32_t graze = 0, blue = 0, maxPoint = 0;
                    int64_t chapterScore = 0, postSettlementScore = 0;
                    bool hasBlue = false, hasPostScore = false;
                    if (legacy) {
                        source = cells[1] == "combat" ? 1 : cells[1] == "practice" ? 2 : -1;
                        stage = std::stoi(cells[2]) - 1;
                        chapter = std::stoi(cells[3]) - 1;
                        section = std::stoi(cells[4]);
                        diff = std::clamp(std::stoi(cells[5]), 0, 3);
                        player = std::clamp(std::stoi(cells[6]), 0, 3);
                        graze = std::stoi(cells[7]);
                        chapterScore = std::stoll(cells[9]);
                        maxPoint = std::stoi(cells[11]);
                    } else {
                        source = cells[1] == "R" ? 0 : cells[1] == "S" ? 1 : cells[1] == "P" ? 2 : -1;
                        stage = std::stoi(cells[2]) - 1;
                        chapter = std::stoi(cells[3]) - 1;
                        section = std::stoi(cells[4]);
                        diff = std::clamp(std::stoi(cells[5]), 0, 3);
                        player = std::clamp(std::stoi(cells[6]), 0, 3);
                        graze = std::stoi(cells[7]);
                        chapterScore = std::stoll(cells[9]);
                        if (cells[10] != "-") {
                            postSettlementScore = std::stoll(cells[10]);
                            hasPostScore = true;
                        }
                        blue = std::stoi(cells[11]);
                        maxPoint = std::stoi(cells[12]);
                        hasBlue = true;
                    }
                    // Old v2 blue-point values came from the wrong pickup path;
                    // keep its other usable records but never use those counts.
                    MergeChapterBest(source, stage, chapter, diff, player,
                        hasBlue, blue, true, graze, true, maxPoint, true, chapterScore,
                        hasPostScore, postSettlementScore, section);
                } catch (...) {
                    continue;
                }
            }
        }

        using ChapterStatKey = std::tuple<int, int, int, int, int>;
        using ChapterStatRows = std::map<ChapterStatKey, std::string>;
        using ChapterStatCells = std::array<std::string, 24>;

        static constexpr const char* kChapterStatHeader =
            "timestamp\tsource\tstage\tchapter\tsection\tdifficulty\tshot\tmisses\tbombs\tgraze\tkill_rate\tresource\tfragment\tchapter_bonus\tblue_points\tblue_score\tmax_point_start\tmax_point_end\tmax_point_delta\tscore_start\tscore_end\tscore_delta\tpost_settlement_score\tchapter_cleared";

        static bool ParseChapterStatCells(const std::string& line, ChapterStatCells& cells)
        {
            std::stringstream row(line);
            for (auto& cell : cells) std::getline(row, cell, '\t');
            return !cells[0].empty();
        }

        static ChapterStatKey ChapterStatRowKey(const ChapterStatCells& cells)
        {
            return { std::stoi(cells[2]), std::stoi(cells[3]), std::stoi(cells[4]),
                std::stoi(cells[5]), std::stoi(cells[6]) };
        }

        static int64_t ChapterStatCellInt(const ChapterStatCells& cells, size_t index)
        {
            if (cells[index].empty() || cells[index] == "-") return -1;
            return std::stoll(cells[index]);
        }

        static auto ChapterStatRowRank(const ChapterStatCells& cells)
        {
            // Prefer a completed chapter with the highest settlement Bonus.
            // Ties use chapter score, max-point growth, then blue-point count.
            return std::tuple<int, int64_t, int64_t, int64_t, int64_t>(
                cells[23] == "Yes" ? 1 : 0,
                ChapterStatCellInt(cells, 13),
                ChapterStatCellInt(cells, 21),
                ChapterStatCellInt(cells, 18),
                ChapterStatCellInt(cells, 14));
        }

        static ChapterStatRows ReadBestChapterStatRows(bool* fileExists = nullptr)
        {
            ChapterStatRows records;
            std::ifstream file("score15chapter_stats_v4.tsv");
            if (fileExists) *fileExists = file.is_open();
            if (!file.is_open()) return records;
            std::string line;
            std::getline(file, line); // header
            while (std::getline(file, line)) {
                ChapterStatCells cells{};
                try {
                    if (!ParseChapterStatCells(line, cells)) continue;
                    const auto key = ChapterStatRowKey(cells);
                    auto found = records.find(key);
                    if (found == records.end()) {
                        records.emplace(key, line);
                    } else {
                        ChapterStatCells oldCells{};
                        if (ParseChapterStatCells(found->second, oldCells)
                            && ChapterStatRowRank(cells) > ChapterStatRowRank(oldCells))
                            found->second = line;
                    }
                } catch (...) {
                    continue;
                }
            }
            return records;
        }

        static bool WriteBestChapterStatRows(const ChapterStatRows& records)
        {
            const char* temporaryPath = "score15chapter_stats_v4.tsv.tmp";
            std::ofstream file(temporaryPath, std::ios::out | std::ios::trunc);
            if (!file.is_open()) return false;
            file << kChapterStatHeader << '\n';
            for (const auto& entry : records)
                file << entry.second << '\n';
            file.flush();
            if (!file.good()) return false;
            file.close();
            return MoveFileExA(temporaryPath, "score15chapter_stats_v4.tsv",
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        }

        void LoadChapterStats()
        {
            if (chapterStatsLoaded) return;
            chapterStatsLoaded = true;
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            bool hasV4File = false;
            const auto records = ReadBestChapterStatRows(&hasV4File);
            if (hasV4File) {
                // Migrate older per-attempt files in place to one best snapshot
                // per stage/chapter-or-section/difficulty/shot combination.
                WriteBestChapterStatRows(records);
                for (const auto& entry : records) {
                    ChapterStatCells cells{};
                    try {
                        if (!ParseChapterStatCells(entry.second, cells)) continue;
                        const int source = cells[1] == "R" ? 0 : cells[1] == "S" ? 1
                            : cells[1] == "P" ? 2 : -1;
                        const int stage = std::stoi(cells[2]) - 1;
                        const int chapter = std::stoi(cells[3]) - 1;
                        const int section = std::stoi(cells[4]);
                        const int diff = std::clamp(std::stoi(cells[5]), 0, 4);
                        const int player = std::clamp(std::stoi(cells[6]), 0, 3);
                        if (source < 0 || cells[23] != "Yes") continue;
                        const int32_t graze = std::stoi(cells[9]);
                        const int32_t blue = std::stoi(cells[14]);
                        const int32_t maxPointEnd = std::stoi(cells[17]);
                        const int32_t maxPointDelta = std::stoi(cells[18]);
                        const int64_t scoreEnd = std::stoll(cells[20]);
                        const int64_t chapterScore = std::stoll(cells[21]);
                        const int64_t postSettlementScore = cells[22].empty() || cells[22] == "-"
                            ? -1 : std::stoll(cells[22]);
                        MergeChapterBest(source, stage, chapter, diff, player,
                            true, blue, true, graze, true, maxPointEnd, true, chapterScore,
                            postSettlementScore >= 0, postSettlementScore, section, scoreEnd);
                        MergeChapterMaxPointDelta(source, stage, chapter, diff, player,
                            maxPointDelta, section);
                    } catch (...) {
                        continue;
                    }
                }
            } else {
                LoadChapterStatFile("score15chapter_stats_v2.tsv", true);
                LoadChapterStatFile("score15chapter_stats_v3.tsv", false);
            }
            PopCurrentDirectory();
        }

        void LoadChapterGoals()
        {
            if (chapterGoalsLoaded) return;
            chapterGoalsLoaded = true;
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            std::ifstream file("score15chapter_goals_v1.tsv");
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty()) continue;
                std::array<std::string, 9> cells{};
                std::stringstream row(line);
                for (auto& cell : cells) std::getline(row, cell, '\t');
                try {
                    if (cells[0] == "mode") {
                        chapterGoalMode = std::clamp(std::stoi(cells[1]), 0, 1);
                    } else if (cells[0] == "goal" || cells[0] == "section_goal") {
                        const int stage = std::stoi(cells[1]);
                        const int chapter = std::stoi(cells[2]);
                        const int diff = std::stoi(cells[3]);
                        const int player = std::stoi(cells[4]);
                        const bool isSection = cells[0] == "section_goal";
                        if (stage < 0 || stage >= 7
                            || (isSection ? (chapter < 1 || chapter >= 75 || BossSectionStage(chapter) != stage)
                                          : (chapter < 0 || chapter >= 10))
                            || diff < 0 || diff >= 5 || player < 0 || player >= 4)
                            continue;
                        auto& goal = isSection ? sectionManualGoals[stage][chapter][diff][player]
                                               : manualGoals[stage][chapter][diff][player];
                        goal.bluePoints = std::stoi(cells[5]);
                        goal.graze = std::stoi(cells[6]);
                        goal.maxPointDelta = std::stoi(cells[7]);
                        goal.chapterScore = std::stoll(cells[8]);
                    }
                } catch (...) {
                    continue;
                }
            }
            PopCurrentDirectory();
        }

        void SaveChapterGoals()
        {
            chapterGoalsLoaded = true;
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            std::ofstream file("score15chapter_goals_v1.tsv", std::ios::out | std::ios::trunc);
            if (file.is_open()) {
                file << "mode\t" << chapterGoalMode << "\n";
                for (int stage = 0; stage < 7; ++stage) {
                    for (int chapter = 0; chapter < 10; ++chapter) {
                        for (int diff = 0; diff < 5; ++diff) {
                            for (int player = 0; player < 4; ++player) {
                                const auto& goal = manualGoals[stage][chapter][diff][player];
                                if (!goal.HasAny()) continue;
                                file << "goal\t" << stage << '\t' << chapter << '\t' << diff << '\t'
                                    << player << '\t' << goal.bluePoints << '\t' << goal.graze << '\t'
                                    << goal.maxPointDelta << '\t' << goal.chapterScore << "\n";
                            }
                        }
                    }
                }
                for (int stage = 0; stage < 7; ++stage) {
                    for (int section = 1; section < 75; ++section) {
                        if (BossSectionStage(section) != stage) continue;
                        for (int diff = 0; diff < 5; ++diff) {
                            for (int player = 0; player < 4; ++player) {
                                const auto& goal = sectionManualGoals[stage][section][diff][player];
                                if (!goal.HasAny()) continue;
                                file << "section_goal\t" << stage << '\t' << section << '\t' << diff << '\t'
                                    << player << '\t' << goal.bluePoints << '\t' << goal.graze << '\t'
                                    << goal.maxPointDelta << '\t' << goal.chapterScore << "\n";
                            }
                        }
                    }
                }
            }
            PopCurrentDirectory();
        }

        void RecordChapterStats(const PendingCapture& capture)
        {
            if (!g_th15ScoreRecordingEnabled)
                return;
            LoadChapterStats();
            const int source = ScoreSourceIndex(capture.replay, capture.mode);
            const char sourceTag = source == 0 ? 'R' : source == 1 ? 'S' : 'P';
            const int graze = std::max(0, capture.grazeAtSettlement - capture.grazeAtStart);
            const int64_t scoreStart = int64_t(capture.scoreAtStart) * 10;
            const int64_t scoreEnd = int64_t(capture.scoreAtSettlement) * 10;
            const int64_t chapterScore = std::max<int64_t>(0, scoreEnd - scoreStart);
            const int64_t postSettlementScore = source == 1
                ? std::max<int64_t>(0, int64_t(*(int32_t*)0x4E740C) * 10) : -1;
            const int enemiesAtSettlement = std::max(0, capture.enemiesAtSettlement - capture.enemiesAtStart);
            const int kills = std::clamp(capture.killsAtSettlement - capture.killsAtStart, 0, enemiesAtSettlement);
            const double killRate = enemiesAtSettlement
                ? static_cast<double>(kills) / enemiesAtSettlement : 0.0;
            const double resource = graze * killRate;
            const int64_t chapterBonus = capture.chapterCleared
                ? static_cast<int64_t>(std::llround(resource * 5000.0)) : -1;
            const int maxPointStart = capture.maxPointAtStart;
            const int maxPoint = capture.maxPointAtSettlement;
            const int maxPointDelta = std::max(0, maxPoint - maxPointStart);

            // Keep exactly one complete attempt for each shot, chapter/section,
            // and difficulty. A clear outranks an interrupted attempt; clears
            // rank by chapter Bonus, with score and growth as tie breakers.
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            auto records = ReadBestChapterStatRows();
            SYSTEMTIME now{};
            GetLocalTime(&now);
            const std::string settledScoreText = source == 1 && capture.chapterCleared
                ? std::to_string(postSettlementScore) : "-";
            const std::string record = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.2f}%\t{:.2f}\t{}\t{}\t{}\t-\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                sourceTag, capture.stage + 1,
                capture.bossSection ? 0 : capture.chapter + 1, capture.bossSection ? capture.section : 0,
                capture.diff, capture.player,
                std::max(0, capture.missesAtSettlement - capture.missesAtStart),
                std::max(0, capture.bombsAtSettlement - capture.bombsAtStart),
                graze, 100.0 * killRate, resource,
                capture.chapterCleared ? (resource >= 200.0 ? "Yes" : "No") : "-",
                chapterBonus < 0 ? "-" : std::to_string(chapterBonus),
                capture.bluePoints, maxPointStart, maxPoint, maxPointDelta,
                scoreStart, scoreEnd, chapterScore, settledScoreText,
                capture.chapterCleared ? "Yes" : "No");
            ChapterStatCells candidateCells{};
            bool candidateWins = false;
            try {
                ParseChapterStatCells(record, candidateCells);
                const auto key = ChapterStatRowKey(candidateCells);
                const auto found = records.find(key);
                ChapterStatCells previousCells{};
                candidateWins = found == records.end()
                    || (ParseChapterStatCells(found->second, previousCells)
                        && ChapterStatRowRank(candidateCells) > ChapterStatRowRank(previousCells));
                if (candidateWins)
                    records[key] = record;
            } catch (...) {
                candidateWins = false;
            }
            const bool saved = candidateWins && WriteBestChapterStatRows(records);
            PopCurrentDirectory();

            if (saved) {
                for (int oldSource = 0; oldSource < 3; ++oldSource) {
                    auto& oldBest = capture.bossSection
                        ? sectionStatBest[oldSource][capture.stage][capture.section][capture.diff][capture.player]
                        : chapterStatBest[oldSource][capture.stage][capture.chapter][capture.diff][capture.player];
                    oldBest = ChapterStatBest{};
                }
                if (capture.chapterCleared) {
                    MergeChapterBest(source, capture.stage, capture.chapter, capture.diff, capture.player,
                        true, capture.bluePoints, true, graze, true, maxPoint, true, chapterScore,
                        source == 1, postSettlementScore,
                        capture.bossSection ? capture.section : 0, scoreEnd);
                    MergeChapterMaxPointDelta(source, capture.stage, capture.chapter,
                        capture.diff, capture.player, maxPointDelta,
                        capture.bossSection ? capture.section : 0);
                }
            }
        }

        void RecordCapture(const PendingCapture& capture)
        {
            if (!g_th15CaptureRateEnabled)
                return;
            if (capture.replay) {
                if (capture.bossSection)
                    { if (g_th15CaptureRateEnabled) ++replaySectionCurrent[capture.mode][capture.stage][capture.section][capture.diff][capture.player][1]; }
                else
                    { if (g_th15CaptureRateEnabled) ++replayCurrent[capture.mode][capture.stage][capture.chapter][capture.diff][capture.player][1]; }
            } else {
                if (capture.bossSection) {
                    { if (g_th15CaptureRateEnabled) ++sectionTotal[capture.mode][capture.stage][capture.section][capture.diff][capture.player][1]; }
                    { if (g_th15CaptureRateEnabled) ++sectionCurrent[capture.mode][capture.stage][capture.section][capture.diff][capture.player][1]; }
                } else {
                    { if (g_th15CaptureRateEnabled) ++total[capture.mode][capture.stage][capture.chapter][capture.diff][capture.player][1]; }
                    { if (g_th15CaptureRateEnabled) ++current[capture.mode][capture.stage][capture.chapter][capture.diff][capture.player][1]; }
                }
                Save();
            }
        }

        static int32_t ReadMaxPoint()
        {
            // The game stores the maximum point value with 100x precision.
            return (*(int32_t*)0x4E7434 / 1000) * 10;
        }

        void SetDisplayFromSnapshot(const LiveChapterSnapshot& snapshot)
        {
            displayValid = snapshot.valid;
            displayMode = snapshot.mode;
            displayStage = snapshot.stage;
            displayChapter = snapshot.chapter;
            displayDiff = snapshot.diff;
            displayPlayer = snapshot.player;
            displayIsBossSection = snapshot.bossSection;
            displaySection = snapshot.section;
        }

        void HoldPreviousChapterDisplay(int nextMode, int nextStage, int nextChapter,
            bool nextIsBossSection, int nextSection, int nextDiff, int nextPlayer, int nextSource)
        {
            const auto& snapshot = lastLiveSnapshot;
            if (!snapshot.valid)
                return;
            const bool sameChapter = snapshot.source == nextSource && snapshot.mode == nextMode
                && snapshot.stage == nextStage && snapshot.diff == nextDiff
                && snapshot.player == nextPlayer && snapshot.bossSection == nextIsBossSection
                && (nextIsBossSection ? snapshot.section == nextSection
                    : snapshot.chapter == nextChapter);
            if (sameChapter)
                return;

            transitionSnapshot = snapshot;
            transitionSnapshotUntil = GetTickCount64() + kChapterDisplayHoldMs;
            SetDisplayFromSnapshot(transitionSnapshot);
        }

        bool IsHoldingPreviousChapterDisplay() const
        {
            return transitionSnapshot.valid && GetTickCount64() < transitionSnapshotUntil;
        }

        void RefreshLiveChapterSnapshot()
        {
            if (!active || !activeObservedGameplay || settlementPending
                || bossIntroPending || bossGapPending)
                return;
            auto& snapshot = lastLiveSnapshot;
            snapshot.valid = true;
            snapshot.source = ScoreSourceIndex(replayDisplayOnly, activeMode);
            snapshot.mode = activeMode;
            snapshot.stage = activeStage;
            snapshot.chapter = activeChapter;
            snapshot.section = activeSection;
            snapshot.diff = activeDiff;
            snapshot.player = activePlayer;
            snapshot.bossSection = activeIsBossSection;
            snapshot.bluePoints = activeBluePoints;
            snapshot.graze = std::max(0, *(int32_t*)0x4E7420 - grazeAtStart);
            snapshot.maxPointDelta = std::max(0, ReadMaxPoint() - maxPointAtStart);
            snapshot.chapterScore = std::max<int64_t>(0,
                (int64_t(*(int32_t*)0x4E740C) - scoreAtStart) * 10);
            snapshot.currentScore = std::max<int64_t>(0,
                int64_t(*(int32_t*)0x4E740C) * 10);
            const int enemies = std::max(0, *(int32_t*)0x4E7484 - enemiesAtStart);
            const int kills = std::clamp(*(int32_t*)0x4E7488 - killsAtStart, 0, enemies);
            snapshot.killRatePercent = enemies ? 100.0 * kills / enemies : 0.0;
            snapshot.resource = snapshot.graze * (snapshot.killRatePercent / 100.0);
        }

        void QueueCapture(int32_t misses, int32_t bombs)
        {
            pendingCaptures.push_back({ activeMode, activeStage, activeChapter, activeSection,
                activeDiff, activePlayer, missesAtStart, bombsAtStart,
                grazeAtStart, killsAtStart, enemiesAtStart, scoreAtStart,
                *(int32_t*)0x4E7420, *(int32_t*)0x4E7488, *(int32_t*)0x4E7484,
                activeBluePoints, maxPointAtStart, ReadMaxPoint(), *(int32_t*)0x4E740C,
                misses, bombs, true,
                activeIsBossSection, replayDisplayOnly });
        }

        void RecordInterruptedAttempt()
        {
            if (!active || !activeObservedGameplay || settlementPending || !captureRateEnabled)
                return;
            const int32_t graze = *(int32_t*)0x4E7420;
            const int32_t kills = *(int32_t*)0x4E7488;
            const int32_t enemies = *(int32_t*)0x4E7484;
            PendingCapture capture{ activeMode, activeStage, activeChapter, activeSection,
                activeDiff, activePlayer, missesAtStart, bombsAtStart,
                grazeAtStart, killsAtStart, enemiesAtStart, scoreAtStart,
                graze, kills, enemies, activeBluePoints, maxPointAtStart, ReadMaxPoint(),
                *(int32_t*)0x4E740C, lastMisses, lastBombs, false,
                activeIsBossSection, replayDisplayOnly };
            RecordChapterStats(capture);
        }

        void ResolvePendingCaptures(int32_t misses, int32_t bombs)
        {
            for (auto capture : pendingCaptures) {
                capture.scoreAtSettlement = *(int32_t*)0x4E740C;
                capture.maxPointAtSettlement = ReadMaxPoint();
                capture.missesAtSettlement = misses;
                capture.bombsAtSettlement = bombs;
                capture.chapterCleared = true;
                const bool success = misses == capture.missesAtStart && bombs == capture.bombsAtStart;
                if (success)
                    RecordCapture(capture);
                RecordChapterStats(capture);
            }
            pendingCaptures.clear();
        }

        void StartBossPhaseAfterEmpty(int32_t stage, int32_t chapter, int32_t misses,
            int32_t bombs)
        {
            HoldPreviousChapterDisplay(activeMode, activeStage, activeChapter,
                activeIsBossSection, activeSection, activeDiff, activePlayer,
                ScoreSourceIndex(replayDisplayOnly, activeMode));
            bossIntroPending = false;
            bossGapPending = false;
            settlementPending = false;
            settlementCaptured = false;
            settledRuntimeChapter = -1;
            activeRuntimeStage = stage;
            activeRuntimeChapter = chapter;
            activeObservedGameplay = GetMemContent(PLAYER_PTR) != 0;
            missesAtStart = lastMisses = misses;
            bombsAtStart = lastBombs = bombs;
            // Chapter counters are cleared by the game after its chapter-index
            // hook. Sampling them here can capture the previous chapter's
            // values and clamp the next chapter's deltas to zero.
            grazeAtStart = 0;
            killsAtStart = 0;
            enemiesAtStart = 0;
            scoreAtStart = *(int32_t*)0x4E740C;
            maxPointAtStart = ReadMaxPoint();
            activeBluePoints = 0;
            failed = false;
            if (!replayDisplayOnly) {
                { if (g_th15CaptureRateEnabled) ++sectionTotal[activeMode][activeStage][activeSection][activeDiff][activePlayer][0]; }
                { if (g_th15CaptureRateEnabled) ++sectionCurrent[activeMode][activeStage][activeSection][activeDiff][activePlayer][0]; }
                Save();
            } else {
                { if (g_th15CaptureRateEnabled) ++replaySectionCurrent[activeMode][activeStage][activeSection][activeDiff][activePlayer][0]; }
            }
        }

        void Load()
        {
            if (loaded) return;
            loaded = true;
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            std::fstream fs("score15chapter.dat", std::ios::in | std::ios::binary);
            if (fs.is_open()) {
                int32_t version = 1;
                fs.read((char*)&version, sizeof(version));
                if (version == 5) {
                    fs.read((char*)total, sizeof(total));
                    fs.read((char*)sectionTotal, sizeof(sectionTotal));
                } else if (version == 3 || version == 4) {
                    int32_t legacyTotal[2][7][10][4][4][2] = {};
                    int32_t legacySectionTotal[2][7][75][4][4][2] = {};
                    fs.read((char*)legacyTotal, sizeof(legacyTotal));
                    if (version == 4)
                        fs.read((char*)legacySectionTotal, sizeof(legacySectionTotal));
                    for (int mode = 0; mode < 2; ++mode) {
                        for (int stage = 0; stage < 7; ++stage) {
                            for (int chapter = 0; chapter < 10; ++chapter) {
                                for (int diff = 0; diff < 4; ++diff) {
                                    for (int player = 0; player < 4; ++player) {
                                        for (int result = 0; result < 2; ++result)
                                            total[mode][stage][chapter][diff][player][result]
                                                = legacyTotal[mode][stage][chapter][diff][player][result];
                                    }
                                }
                            }
                            if (version == 4) {
                                for (int section = 0; section < 75; ++section) {
                                    for (int diff = 0; diff < 4; ++diff) {
                                        for (int player = 0; player < 4; ++player) {
                                            for (int result = 0; result < 2; ++result)
                                                sectionTotal[mode][stage][section][diff][player][result]
                                                    = legacySectionTotal[mode][stage][section][diff][player][result];
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            PopCurrentDirectory();
            LoadChapterStats();
        }
        void Save()
        {
            if (replayDisplayOnly || !g_th15CaptureRateEnabled) return;
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            std::fstream fs("score15chapter.dat", std::ios::out | std::ios::binary);
            if (fs.is_open()) {
                int32_t version = 5;
                fs.write((char*)&version, sizeof(version));
                fs.write((char*)total, sizeof(total));
                fs.write((char*)sectionTotal, sizeof(sectionTotal));
            }
            PopCurrentDirectory();
        }
    public:
        void LoadHudChapterView()
        {
            if (hudChapterViewLoaded) return;
            hudChapterViewLoaded = true;

            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            std::ifstream file("score15chapter_view.txt");
            int view = -1;
            if (file >> view && (view == 0 || view == 1)) {
                g_adv_igi_options.th15_hudChapterView = view;
            } else {
                view = std::clamp(g_adv_igi_options.th15_hudChapterView, 0, 1);
                std::ofstream save("score15chapter_view.txt", std::ios::out | std::ios::trunc);
                if (save.is_open()) save << view << '\n';
            }
            PopCurrentDirectory();
        }

        void SaveHudChapterView(int view)
        {
            LoadHudChapterView();
            g_adv_igi_options.th15_hudChapterView = std::clamp(view, 0, 1);
            LauncherSettingSet("th15_hud_chapter_view", g_adv_igi_options.th15_hudChapterView);

            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            std::ofstream file("score15chapter_view.txt", std::ios::out | std::ios::trunc);
            if (file.is_open()) file << g_adv_igi_options.th15_hudChapterView << '\n';
            PopCurrentDirectory();
        }

        struct LiveChapterStats {
            int32_t bluePoints = 0, bestBluePoints = 0;
            int32_t graze = 0, bestGraze = 0;
            int32_t maxPointDelta = 0, bestMaxPointDelta = 0;
            int64_t chapterScore = 0, bestChapterScore = 0;
            int64_t currentScore = 0, bestCurrentScore = 0;
            bool hasBestCurrentScore = false;
            double resource = 0.0;
            int32_t targetBluePoints = -1, targetGraze = -1, targetMaxPointDelta = -1;
            int64_t targetChapterScore = -1;
            double killRatePercent = 0.0;
            bool hasBestBluePoints = false, hasBestGraze = false;
            bool hasBestMaxPointDelta = false, hasBestChapterScore = false;
            bool manualGoal = false;
            bool hasLiveAttempt = false;
        };
        void OnBluePointCollected()
        {
            if (active && !settlementPending)
                ++activeBluePoints;
        }

        bool GetLiveChapterStats(LiveChapterStats& out)
        {
            LoadChapterStats();
            LoadChapterGoals();
            if (!displayValid || displayStage < 0 || displayStage >= 7
                || (displayIsBossSection
                    ? (displaySection < 1 || displaySection >= 75
                        || BossSectionStage(displaySection) != displayStage)
                    : (displayChapter < 0 || displayChapter >= 10)))
                return false;

            const bool holdingPreviousChapter = IsHoldingPreviousChapterDisplay();
            const int source = holdingPreviousChapter ? transitionSnapshot.source
                : ScoreSourceIndex(replayDisplayOnly, displayMode);
            const bool activeStats = !holdingPreviousChapter && active && activeObservedGameplay && !settlementPending
                && !bossIntroPending && !bossGapPending && activeStage == displayStage
                && activeMode == displayMode && activeDiff == displayDiff
                && activePlayer == displayPlayer
                && ScoreSourceIndex(replayDisplayOnly, activeMode) == source
                && activeIsBossSection == displayIsBossSection
                && (displayIsBossSection ? activeSection == displaySection
                    : activeChapter == displayChapter);
            const auto& snapshot = holdingPreviousChapter ? transitionSnapshot : lastLiveSnapshot;
            const bool snapshotStats = snapshot.valid && snapshot.source == source
                && snapshot.mode == displayMode && snapshot.stage == displayStage
                && snapshot.diff == displayDiff && snapshot.player == displayPlayer
                && snapshot.bossSection == displayIsBossSection
                && (displayIsBossSection ? snapshot.section == displaySection
                    : snapshot.chapter == displayChapter);
            out.hasLiveAttempt = activeStats || snapshotStats;

            const int diff = std::clamp(displayDiff, 0, 4);
            const int player = std::clamp(displayPlayer, 0, 3);
            const auto& best = displayIsBossSection
                ? sectionStatBest[source][displayStage][displaySection][diff][player]
                : chapterStatBest[source][displayStage][displayChapter][diff][player];
            const auto& manual = displayIsBossSection
                ? sectionManualGoals[displayStage][displaySection][diff][player]
                : manualGoals[displayStage][displayChapter][diff][player];
            out.manualGoal = chapterGoalMode == 1;
            out.bluePoints = activeStats ? activeBluePoints : snapshotStats ? snapshot.bluePoints : 0;
            out.graze = activeStats ? std::max(0, *(int32_t*)0x4E7420 - grazeAtStart)
                : snapshotStats ? snapshot.graze : 0;
            out.maxPointDelta = activeStats ? std::max(0, ReadMaxPoint() - maxPointAtStart)
                : snapshotStats ? snapshot.maxPointDelta : 0;
            out.chapterScore = activeStats ? std::max<int64_t>(0,
                (int64_t(*(int32_t*)0x4E740C) - scoreAtStart) * 10)
                : snapshotStats ? snapshot.chapterScore : 0;
            out.currentScore = activeStats
                ? std::max<int64_t>(0, int64_t(*(int32_t*)0x4E740C) * 10)
                : snapshotStats ? snapshot.currentScore : 0;
            ChapterStatBest allRecordsBest{};
            for (int recordSource = 0; recordSource < 3; ++recordSource) {
                const auto& scoreBest = displayIsBossSection
                    ? sectionStatBest[recordSource][displayStage][displaySection][diff][player]
                    : chapterStatBest[recordSource][displayStage][displayChapter][diff][player];
                if (scoreBest.hasBluePoints && (!allRecordsBest.hasBluePoints
                    || scoreBest.bluePoints > allRecordsBest.bluePoints)) {
                    allRecordsBest.bluePoints = scoreBest.bluePoints;
                    allRecordsBest.hasBluePoints = true;
                }
                if (scoreBest.hasGraze && (!allRecordsBest.hasGraze
                    || scoreBest.graze > allRecordsBest.graze)) {
                    allRecordsBest.graze = scoreBest.graze;
                    allRecordsBest.hasGraze = true;
                }
                if (scoreBest.hasMaxPointDelta && (!allRecordsBest.hasMaxPointDelta
                    || scoreBest.maxPointDelta > allRecordsBest.maxPointDelta)) {
                    allRecordsBest.maxPointDelta = scoreBest.maxPointDelta;
                    allRecordsBest.hasMaxPointDelta = true;
                }
                if (scoreBest.hasChapterScore && (!allRecordsBest.hasChapterScore
                    || scoreBest.chapterScore > allRecordsBest.chapterScore)) {
                    allRecordsBest.chapterScore = scoreBest.chapterScore;
                    allRecordsBest.hasChapterScore = true;
                }
                if (scoreBest.hasScoreEnd && (!allRecordsBest.hasScoreEnd
                    || scoreBest.scoreEnd > allRecordsBest.scoreEnd)) {
                    allRecordsBest.scoreEnd = scoreBest.scoreEnd;
                    allRecordsBest.hasScoreEnd = true;
                }
            }
            out.hasBestCurrentScore = allRecordsBest.hasScoreEnd;
            out.bestCurrentScore = allRecordsBest.scoreEnd;
            const int enemies = activeStats
                ? std::max(0, *(int32_t*)0x4E7484 - enemiesAtStart) : 0;
            const int kills = activeStats
                ? std::clamp(*(int32_t*)0x4E7488 - killsAtStart, 0, enemies) : 0;
            out.killRatePercent = activeStats ? (enemies ? 100.0 * kills / enemies : 0.0)
                : snapshotStats ? snapshot.killRatePercent : 0.0;
            out.resource = activeStats ? (enemies ? out.graze * static_cast<double>(kills) / enemies : 0.0)
                : snapshotStats ? snapshot.resource : 0.0;
            out.bestBluePoints = best.bluePoints;
            out.bestGraze = best.graze;
            out.bestMaxPointDelta = best.maxPointDelta;
            out.bestChapterScore = best.chapterScore;
            out.hasBestBluePoints = best.hasBluePoints;
            out.hasBestGraze = best.hasGraze;
            out.hasBestMaxPointDelta = best.hasMaxPointDelta;
            out.hasBestChapterScore = best.hasChapterScore;
            if (out.manualGoal) {
                out.targetBluePoints = manual.bluePoints;
                out.targetGraze = manual.graze;
                out.targetMaxPointDelta = manual.maxPointDelta;
                out.targetChapterScore = manual.chapterScore;
            } else {
                out.targetBluePoints = allRecordsBest.hasBluePoints ? allRecordsBest.bluePoints : -1;
                out.targetGraze = allRecordsBest.hasGraze ? allRecordsBest.graze : -1;
                out.targetMaxPointDelta = allRecordsBest.hasMaxPointDelta ? allRecordsBest.maxPointDelta : -1;
                out.targetChapterScore = allRecordsBest.hasChapterScore ? allRecordsBest.chapterScore : -1;
            }
            return true;
        }

        void BeginRun(int32_t stage, int32_t misses, int32_t bombs,
            int32_t isPractice, int32_t practiceSection, int32_t practiceMenuStage,
            int32_t difficulty, int32_t shotType, bool replayPlayback = false)
        {
            ResetRun();
            // game_start precedes the new stage's chapter-index write. The
            // runtime index can still name a later chapter from the abandoned
            // run. Story and whole-stage playback start at chapter zero;
            // BeginAttempt resolves a practice jump from its selected target.
            BeginAttempt(stage, 0, misses, bombs, isPractice, practiceSection,
                practiceMenuStage, difficulty, shotType, replayPlayback);
        }

        void BeginAttempt(int32_t stage, int32_t chapter, int32_t misses, int32_t bombs,
            int32_t isPractice, int32_t practiceSection, int32_t practiceMenuStage,
            int32_t difficulty, int32_t shotType, bool replayPlayback = false)
        {
            Load();
            replayDisplayOnly = replayPlayback;
            const int mode = isPractice ? 1 : 0;
            const int diff = std::clamp(difficulty, 0, 4);
            const int player = std::clamp(shotType, 0, 3);
            const int sectionStage = isPractice ? BossSectionStage(practiceSection) : -1;
            const bool isBossSection = sectionStage >= 0;
            int targetStage = stage - 1;
            int targetChapter = chapter;
            // At game_start the runtime chapter can still be left over from the
            // previous run. A whole-stage practice always starts at chapter 1.
            if (isPractice && practiceSection == 0
                && practiceMenuStage >= 0 && practiceMenuStage < 7) {
                targetStage = practiceMenuStage;
                targetChapter = 0;
            }
            if (isPractice && practiceSection >= 10000) {
                const int chapterId = practiceSection - 10000;
                const int selectedStage = practiceMenuStage;
                const int selectedChapter = chapterId % 100;
                if (selectedStage >= 0 && selectedStage < 7
                    && chapterId / 100 == selectedStage + 1
                    && selectedChapter >= 1 && selectedChapter <= kChapterCounts[selectedStage]) {
                    targetStage = selectedStage;
                    targetChapter = selectedChapter - 1;
                }
            }
            if (isBossSection) {
                targetStage = sectionStage;
                targetChapter = 0; // The section-id table does not use a chapter index.
            }
            if (targetStage < 0 || targetStage >= 7 || targetChapter < 0
                || (!isBossSection && targetChapter >= kChapterCounts[targetStage]))
                return;

            if (!captureRateEnabled)
                return;

            HoldPreviousChapterDisplay(mode, targetStage, targetChapter, isBossSection,
                isBossSection ? practiceSection : 0, diff, player,
                ScoreSourceIndex(replayPlayback, mode));

            active = true;
            failed = false;
            settlementPending = false;
            settlementCaptured = false;
            bossIntroPending = false;
            bossGapPending = false;
            settledRuntimeChapter = -1;
            practiceFinished = false;
            completedAwaitingNext = false;
            finishedPracticeStage = finishedPracticeChapter = -1;
            activeMode = mode;
            activeStage = targetStage;
            activeChapter = targetChapter;
            activeDiff = diff;
            activePlayer = player;
            activeIsBossSection = isBossSection;
            activeSection = isBossSection ? practiceSection : 0;
            activePracticeSection = practiceSection;
            activePracticeMenuStage = practiceMenuStage;
            activeRuntimeStage = stage;
            activeRuntimeChapter = chapter;
            activeRuntimeValid = stage >= 1 && stage <= 7 && chapter >= 0
                && chapter < kChapterCounts[stage - 1];
            activeObservedGameplay = false;
            missesAtStart = misses;
            bombsAtStart = bombs;
            // In a combat run these are chapter-local counters and the game
            // may still expose the previous chapter's values at game_start.
            // Practice can seed them through the practice parameters, so keep
            // its configured baseline.
            grazeAtStart = mode == 0 ? 0 : *(int32_t*)0x4E7420;
            killsAtStart = mode == 0 ? 0 : *(int32_t*)0x4E7488;
            enemiesAtStart = mode == 0 ? 0 : *(int32_t*)0x4E7484;
            scoreAtStart = *(int32_t*)0x4E740C;
            maxPointAtStart = ReadMaxPoint();
            activeBluePoints = 0;
            // game_start resets the game's live counters before beginning a new
            // attempt. Keep the retry detector's baseline in sync with that reset;
            // otherwise Update sees (for example) 1 miss -> 0 misses and cancels
            // the freshly started attempt on its first frame.
            lastMisses = misses;
            lastBombs = bombs;
            if (!replayDisplayOnly) {
                if (activeIsBossSection) {
                    { if (g_th15CaptureRateEnabled) ++sectionTotal[activeMode][activeStage][activeSection][activeDiff][activePlayer][0]; }
                    { if (g_th15CaptureRateEnabled) ++sectionCurrent[activeMode][activeStage][activeSection][activeDiff][activePlayer][0]; }
                } else {
                    { if (g_th15CaptureRateEnabled) ++total[activeMode][activeStage][activeChapter][activeDiff][activePlayer][0]; }
                    { if (g_th15CaptureRateEnabled) ++current[activeMode][activeStage][activeChapter][activeDiff][activePlayer][0]; }
                }
                Save();
            } else if (activeIsBossSection) {
                { if (g_th15CaptureRateEnabled) ++replaySectionCurrent[activeMode][activeStage][activeSection][activeDiff][activePlayer][0]; }
            } else {
                { if (g_th15CaptureRateEnabled) ++replayCurrent[activeMode][activeStage][activeChapter][activeDiff][activePlayer][0]; }
            }
            if (!IsHoldingPreviousChapterDisplay()) {
                displayValid = true;
                displayMode = activeMode;
                displayStage = activeStage;
                displayChapter = activeChapter;
                displayDiff = activeDiff;
                displayPlayer = activePlayer;
                displayIsBossSection = activeIsBossSection;
                displaySection = activeSection;
            }
        }

        void OnChapterClear(int32_t misses, int32_t bombs)
        {
            if (!active || !activeObservedGameplay) {
                return;
            }
            if (settlementPending) {
                return;
            }
            RefreshLiveChapterSnapshot();
            // Some boss transitions contain a chapter with no attack. Its
            // settlement starts the next phase; it does not capture that phase.
            if (bossIntroPending || bossGapPending) {
                settlementPending = true;
                settledRuntimeChapter = *(int32_t*)0x4E73F8;
                return;
            }
            if (misses != missesAtStart || bombs != bombsAtStart)
                failed = true;
            settlementPending = true;
            settlementCaptured = false;
            settledRuntimeChapter = *(int32_t*)0x4E73F8;
            QueueCapture(misses, bombs);
            settlementCaptured = true;
        }

        void OnChapterIndexChanged(int32_t stage, int32_t chapter, int32_t misses, int32_t bombs,
            int32_t isPractice, int32_t difficulty, int32_t shotType)
        {
            // A game chapter can begin without a preceding clear callback.
            // Once an empty transition has been entered, a new runtime index
            // is enough to confirm that the next real boss phase has started.
            if (active && (bossIntroPending || bossGapPending) && !settlementPending
                && stage - 1 == activeStage && chapter >= 40
                && chapter != activeRuntimeChapter) {
                StartBossPhaseAfterEmpty(stage, chapter, misses, bombs);
                return;
            }
            if (!settlementPending || !active)
                return;

            if (bossIntroPending || bossGapPending) {
                if (stage - 1 != activeStage || (bossIntroPending && chapter == settledRuntimeChapter))
                    return;
                StartBossPhaseAfterEmpty(stage, chapter, misses, bombs);
                return;
            }

            // Several chapters share a runtime index. A confirmed settlement
            // advances the logical chapter even when that index stays behind.
            const bool laggingChapterTransition = !activeIsBossSection
                && stage - 1 == activeStage && chapter >= 0 && chapter <= activeChapter
                && activeChapter < kChapterCounts[activeStage];
            if (stage - 1 == activeStage && chapter == activeChapter
                && !laggingChapterTransition)
                return;
            const int nextChapter = laggingChapterTransition ? activeChapter + 1 : chapter;

            if (misses != missesAtStart || bombs != bombsAtStart)
                failed = true;
            if (!settlementCaptured) {
                QueueCapture(misses, bombs);
            }

            // Midboss nonspells share the ordinary chapter counter. Their
            // spell may be skipped after a timeout, so use the next runtime
            // chapter to choose the actual section instead of incrementing a
            // section id unconditionally.
            int nextMidSection = 0;
            int nextNormalChapter = -1;
            if (stage - 1 == activeStage) {
                if (!activeIsBossSection) {
                    if (activeStage == 0 && activeChapter == 1 && chapter == 2)
                        nextMidSection = TH15_ST1_MID1;
                    else if (activeStage == 0 && activeChapter == 3 && chapter == 4)
                        nextMidSection = TH15_ST1_MID2;
                    else if (activeStage == 1 && activeChapter == 2 && chapter == 3)
                        nextMidSection = TH15_ST2_MID1;
                    else if (activeStage == 2 && activeChapter == 2 && chapter == 3)
                        nextMidSection = TH15_ST3_MID1;
                    else if (activeStage == 3 && activeChapter == 2 && chapter == 3)
                        nextMidSection = TH15_ST4_MID1;
                } else {
                    switch (activeSection) {
                    case TH15_ST1_MID1:
                        if (chapter == 2) nextNormalChapter = 3;
                        break;
                    case TH15_ST1_MID2:
                        if (chapter == 31 && activeDiff >= 2) nextMidSection = TH15_ST1_MID3;
                        else if (chapter == 5) nextNormalChapter = 5;
                        break;
                    case TH15_ST1_MID3:
                        if (chapter == 5) nextNormalChapter = 5;
                        break;
                    case TH15_ST2_MID1:
                        if (chapter == 3 || chapter == 4) nextNormalChapter = 3;
                        break;
                    case TH15_ST3_MID1:
                        if (chapter == 31) nextMidSection = TH15_ST3_MID2;
                        else if (chapter == 4) nextNormalChapter = 4;
                        break;
                    case TH15_ST3_MID2:
                        if (chapter == 4) nextNormalChapter = 4;
                        break;
                    case TH15_ST4_MID1:
                        if (chapter == 4) nextNormalChapter = 3;
                        break;
                    }
                }
            }
            const bool continueChapterRun = !(activeMode == 1 && onlyCurrentPracticeChapter)
                && (nextNormalChapter >= 0
                || (!activeIsBossSection && !nextMidSection && stage >= 1 && stage <= 7
                    && nextChapter >= 0 && nextChapter < kChapterCounts[stage - 1]));
            const bool enterEndBossRun = !activeIsBossSection && stage - 1 == activeStage
                && activeChapter == kChapterCounts[activeStage] - 1 && chapter >= 40;
            // These spells are followed by an empty game chapter before the
            // next spell starts. Some transitions retain the same runtime index.
            const bool bossGapBoundary = activeIsBossSection
                && ((activeStage == 4 && (activeSection == TH15_ST5_BOSS6
                    || activeSection == TH15_ST5_BOSS7))
                    || (activeStage == 5 && (activeSection == TH15_ST6_BOSS8
                        || activeSection == TH15_ST6_BOSS9
                        || activeSection == TH15_ST6_BOSS10)));
            const bool continueBossRun = activeIsBossSection && stage - 1 == activeStage
                && (activeStage == 6 || activeSection >= kFirstEndBossSections[activeStage])
                && chapter >= 40 && (chapter != settledRuntimeChapter || bossGapBoundary)
                && BossSectionStage(activeSection + 1) == activeStage;
            const bool enterBossGap = continueBossRun && bossGapBoundary;
            active = false;
            settlementPending = false;
            settlementCaptured = false;
            settledRuntimeChapter = -1;
            bossIntroPending = false;
            bossGapPending = false;
            failed = false;
            completedAwaitingNext = true;
            if (activeMode == 1) {
                practiceFinished = true;
                finishedPracticeStage = activeStage;
                finishedPracticeChapter = activeChapter;
            }

            // Start the next attempt at the game’s chapter-index write. Polling the
            // index once per frame can miss short sections between settlement events.
            if (continueChapterRun || (!(activeMode == 1 && onlyCurrentPracticeChapter)
                && (nextMidSection || enterEndBossRun || continueBossRun))) {
                const int nextMode = isPractice ? 1 : 0;
                const int nextDiff = std::clamp(difficulty, 0, 4);
                const int nextPlayer = std::clamp(shotType, 0, 3);
                const int nextActiveChapter = enterEndBossRun || continueBossRun || nextMidSection ? 0
                    : nextNormalChapter >= 0 ? nextNormalChapter : nextChapter;
                const bool nextIsBossSection = enterEndBossRun || continueBossRun || nextMidSection;
                const int nextActiveSection = enterEndBossRun ? kFirstEndBossSections[stage - 1]
                    : continueBossRun ? activeSection + 1 : nextMidSection;
                HoldPreviousChapterDisplay(nextMode, stage - 1, nextActiveChapter,
                    nextIsBossSection, nextActiveSection, nextDiff, nextPlayer,
                    ScoreSourceIndex(replayDisplayOnly, nextMode));
                active = true;
                activeMode = nextMode;
                activeStage = stage - 1;
                activeChapter = nextActiveChapter;
                activeDiff = nextDiff;
                activePlayer = nextPlayer;
                activeIsBossSection = nextIsBossSection;
                activeSection = nextActiveSection;
                bossIntroPending = enterEndBossRun && chapter == 41;
                bossGapPending = enterBossGap;
                activeRuntimeStage = stage;
                activeRuntimeChapter = chapter;
                activeRuntimeValid = true;
                activeObservedGameplay = GetMemContent(PLAYER_PTR) != 0;
                missesAtStart = misses;
                bombsAtStart = bombs;
                // The chapter index changes before these chapter-local values
                // are reset, so the following chapter starts from zero.
                grazeAtStart = 0;
                killsAtStart = 0;
                enemiesAtStart = 0;
                scoreAtStart = *(int32_t*)0x4E740C;
                maxPointAtStart = ReadMaxPoint();
                activeBluePoints = 0;
                lastMisses = misses;
                lastBombs = bombs;
                failed = false;
                practiceFinished = false;
                completedAwaitingNext = false;
                // During the boss intro or an empty inter-phase chapter, retain
                // the completed section's display until the next phase begins.
                if (!IsHoldingPreviousChapterDisplay() && !bossIntroPending
                    && !bossGapPending && activeObservedGameplay) {
                    displayValid = true;
                    displayMode = nextMode;
                    displayStage = stage - 1;
                    displayChapter = activeChapter;
                    displayDiff = nextDiff;
                    displayPlayer = nextPlayer;
                    displayIsBossSection = activeIsBossSection;
                    displaySection = activeSection;
                }
                if (!replayDisplayOnly) {
                    if (activeIsBossSection) {
                        if (!bossIntroPending && !bossGapPending) {
                            { if (g_th15CaptureRateEnabled) ++sectionTotal[nextMode][stage - 1][activeSection][nextDiff][nextPlayer][0]; }
                            { if (g_th15CaptureRateEnabled) ++sectionCurrent[nextMode][stage - 1][activeSection][nextDiff][nextPlayer][0]; }
                        }
                    } else {
                        { if (g_th15CaptureRateEnabled) ++total[nextMode][stage - 1][activeChapter][nextDiff][nextPlayer][0]; }
                        { if (g_th15CaptureRateEnabled) ++current[nextMode][stage - 1][activeChapter][nextDiff][nextPlayer][0]; }
                    }
                    Save();
                } else if (activeIsBossSection) {
                    if (!bossIntroPending && !bossGapPending)
                        { if (g_th15CaptureRateEnabled) ++replaySectionCurrent[nextMode][stage - 1][activeSection][nextDiff][nextPlayer][0]; }
                } else {
                    { if (g_th15CaptureRateEnabled) ++replayCurrent[nextMode][stage - 1][activeChapter][nextDiff][nextPlayer][0]; }
                }
            }
        }

        void Update(int32_t stage, int32_t chapter, int32_t misses, int32_t bombs,
            int32_t isPractice, int32_t practiceSection, int32_t practiceMenuStage,
            int32_t difficulty, int32_t shotType, bool replayPlayback = false)
        {
            Load();
            if (!captureRateEnabled) {
                active = false;
                displayValid = false;
                pendingCaptures.clear();
                return;
            }
            // If this transition did not clear bullets, settle at frame end.
            // A clear uses OnBulletClearFinished before this update returns.
            ResolvePendingCaptures(misses, bombs);
            const bool playerActive = GetMemContent(PLAYER_PTR) != 0;
            const int mode = isPractice ? 1 : 0;
            const int diff = std::clamp(difficulty, 0, 4);
            const int player = std::clamp(shotType, 0, 3);

            // The practice selector stores a 1-based chapter in section while the
            // game's runtime chapter counter is 0-based.
            int practiceStage = -1, practiceChapter = -1;
            if (isPractice && practiceSection >= 10000) {
                const int chapterId = practiceSection - 10000;
                practiceStage = practiceMenuStage;
                const int selectedChapter = chapterId % 100;
                practiceChapter = selectedChapter - 1;
                if (chapterId / 100 != practiceStage + 1 || practiceStage < 0 || practiceStage >= 7 || selectedChapter < 1
                    || selectedChapter > kChapterCounts[practiceStage])
                    practiceStage = practiceChapter = -1;
            }
            const bool isChapterPractice = practiceStage >= 0;
            const int bossSectionStage = isPractice ? BossSectionStage(practiceSection) : -1;
            const bool isBossPractice = bossSectionStage >= 0;
            const bool runtimeValid = stage >= 1 && stage <= 7 && chapter >= 0
                && chapter < kChapterCounts[stage - 1];
            replayDisplayOnly = replayPlayback;
            if (captureRateResumePending && !replayPlayback && playerActive
                && (isChapterPractice || isBossPractice)) {
                BeginAttempt(stage, chapter, misses, bombs, isPractice, practiceSection,
                    practiceMenuStage, difficulty, shotType, false);
                captureRateResumePending = !active;
            }
            if (replayPlayback) {
                // Follow the same chapter events as a live run, but leave both
                // persisted and session counters untouched during playback.
                if (active && playerActive)
                    activeObservedGameplay = true;
                if (!active && !isChapterPractice && !isBossPractice && playerActive
                    && runtimeValid && (!completedAwaitingNext || stage - 1 != activeStage
                        || chapter != activeChapter)) {
                    BeginAttempt(stage, chapter, misses, bombs, 0, 0, practiceMenuStage,
                        difficulty, shotType, true);
                    activeObservedGameplay = true;
                }
                if (IsHoldingPreviousChapterDisplay()) {
                    SetDisplayFromSnapshot(transitionSnapshot);
                } else {
                    transitionSnapshot.valid = false;
                    displayValid = active;
                }
                if (active && !IsHoldingPreviousChapterDisplay()) {
                    displayMode = activeMode;
                    displayStage = activeStage;
                    displayChapter = activeChapter;
                    displayDiff = activeDiff;
                    displayPlayer = activePlayer;
                    displayIsBossSection = activeIsBossSection;
                    displaySection = activeSection;
                    if (misses != missesAtStart || bombs != bombsAtStart)
                        failed = true;
                }
                return;
            }
            if (active && playerActive)
                activeObservedGameplay = true;
            if (!isChapterPractice || (practiceStage != finishedPracticeStage
                || practiceChapter != finishedPracticeChapter)) {
                practiceFinished = false;
                finishedPracticeStage = finishedPracticeChapter = -1;
            }

            // ESC+R can reset the live counters without passing through the regular
            // game-start hook. Use the counter reset as the retry signal. The runtime
            // chapter can briefly move backward during scene transitions, so it is
            // not reliable enough to cancel an attempt on its own.
            // The practice selector stays on the chapter used to enter the game.
            // Compare the selection with the one that started this run, not with
            // the active chapter, which can advance after a clear.
            const bool targetChanged = active && (isChapterPractice || isBossPractice)
                && (activeMode != 1 || activePracticeSection != practiceSection
                    || activePracticeMenuStage != practiceMenuStage);
            const bool counterReset = misses < lastMisses || bombs < lastBombs;
            const bool retryReset = (active || completedAwaitingNext) && counterReset;
            if (retryReset || targetChanged) {
                // This path bypassed game_start. Reset the complete tracker,
                // including held HUD snapshots and completion guards, then
                // start the new attempt even for a practice selector jump.
                ResetRun();
                BeginAttempt(stage, chapter, misses, bombs, isPractice, practiceSection,
                    practiceMenuStage, difficulty, shotType, replayPlayback);
                if (active && playerActive)
                    activeObservedGameplay = true;
            }

            const bool holdingPreviousChapter = IsHoldingPreviousChapterDisplay();
            if (holdingPreviousChapter) {
                SetDisplayFromSnapshot(transitionSnapshot);
            } else if (transitionSnapshot.valid) {
                transitionSnapshot.valid = false;
            }
            if (holdingPreviousChapter) {
                // Keep the settled chapter and its values readable briefly after
                // the game has already started the next chapter.
            } else if (settlementPending || bossIntroPending || bossGapPending
                || (active && !activeObservedGameplay)
                || (!playerActive && lastLiveSnapshot.valid)) {
                // Preserve the last meaningful chapter and its live values
                // while the game is in a blank or transition phase.
                if (lastLiveSnapshot.valid) {
                    displayValid = true;
                    displayMode = lastLiveSnapshot.mode;
                    displayStage = lastLiveSnapshot.stage;
                    displayChapter = lastLiveSnapshot.chapter;
                    displayDiff = lastLiveSnapshot.diff;
                    displayPlayer = lastLiveSnapshot.player;
                    displayIsBossSection = lastLiveSnapshot.bossSection;
                    displaySection = lastLiveSnapshot.section;
                }
            } else if (isChapterPractice && active && activeMode == 1
                && (activeIsBossSection || activeStage != practiceStage
                    || activeChapter != practiceChapter)) {
                displayValid = true;
                displayMode = activeMode;
                displayStage = activeStage;
                displayChapter = activeChapter;
                displayDiff = activeDiff;
                displayPlayer = activePlayer;
                displayIsBossSection = activeIsBossSection;
                displaySection = activeSection;
            } else if (isChapterPractice) {
                displayValid = true;
                displayMode = 1;
                displayStage = practiceStage;
                displayChapter = practiceChapter;
                displayDiff = diff;
                displayPlayer = player;
                if (!IsHoldingPreviousChapterDisplay()) {
                    displayIsBossSection = false;
                    displaySection = 0;
                }
            } else if (isBossPractice) {
                displayValid = true;
                displayMode = 1;
                displayStage = bossSectionStage;
                displayDiff = diff;
                displayPlayer = player;
                if (active && activeMode == 1 && activeStage == bossSectionStage) {
                    displayChapter = activeChapter;
                    displayIsBossSection = activeIsBossSection;
                    displaySection = activeIsBossSection ? activeSection : 0;
                } else {
                    displayIsBossSection = true;
                    displaySection = practiceSection;
                }
            } else if (runtimeValid) {
                displayValid = true;
                // The logical attempt tracker distinguishes chapters and boss
                // sections that share a runtime chapter index. Use its key while
                // gameplay is active so the HUD title, live counters, and records
                // all refer to the same attempt.
                const bool matchingActiveAttempt = active && activeObservedGameplay
                    && activeMode == mode && activeStage == stage - 1
                    && activeDiff == diff && activePlayer == player;
                displayMode = matchingActiveAttempt ? activeMode : mode;
                displayStage = matchingActiveAttempt ? activeStage : stage - 1;
                displayChapter = matchingActiveAttempt ? activeChapter : chapter;
                displayDiff = matchingActiveAttempt ? activeDiff : diff;
                displayPlayer = matchingActiveAttempt ? activePlayer : player;
                displayIsBossSection = matchingActiveAttempt && activeIsBossSection;
                displaySection = displayIsBossSection ? activeSection : 0;
            } else if (active) {
                displayValid = true;
                displayMode = activeMode;
                displayStage = activeStage;
                displayChapter = activeChapter;
                displayDiff = activeDiff;
                displayPlayer = activePlayer;
                displayIsBossSection = activeIsBossSection;
                displaySection = activeSection;
            }

            if (active && (misses != missesAtStart || bombs != bombsAtStart))
                failed = true;

            RefreshLiveChapterSnapshot();

            // A restart can briefly expose the previous chapter value. Move the
            // attempt's runtime origin backward with it so that stale forward state
            // cannot be mistaken for a clear immediately after retry.
            if (active && runtimeValid) {
                if (!activeRuntimeValid || stage < activeRuntimeStage
                    || (stage == activeRuntimeStage && chapter < activeRuntimeChapter)) {
                    activeRuntimeStage = stage;
                    activeRuntimeChapter = chapter;
                    activeRuntimeValid = true;
                }
            }

            const int targetStage = isChapterPractice ? practiceStage : (runtimeValid ? stage - 1 : -1);
            const int targetChapter = isChapterPractice ? practiceChapter : (runtimeValid ? chapter : -1);
            if (!isChapterPractice && !isBossPractice && playerActive && !active && targetStage >= 0
                && (!completedAwaitingNext || targetStage != activeStage || targetChapter != activeChapter)) {
                HoldPreviousChapterDisplay(mode, targetStage, targetChapter, false, 0,
                    diff, player, ScoreSourceIndex(replayDisplayOnly, mode));
                active = true;
                failed = false;
                completedAwaitingNext = false;
                missesAtStart = misses;
                bombsAtStart = bombs;
                activeMode = mode;
                activeStage = targetStage;
                activeChapter = targetChapter;
                activeDiff = diff;
                activePlayer = player;
                activeIsBossSection = false;
                activeSection = 0;
                activeRuntimeStage = stage;
                activeRuntimeChapter = chapter;
                activeRuntimeValid = runtimeValid;
                activeObservedGameplay = true;
                // The ordinary-run fallback may create the next attempt after
                // the chapter hook has already passed; seed chapter-local
                // counters from zero instead of reusing the previous values.
                grazeAtStart = mode == 0 ? 0 : *(int32_t*)0x4E7420;
                killsAtStart = mode == 0 ? 0 : *(int32_t*)0x4E7488;
                enemiesAtStart = mode == 0 ? 0 : *(int32_t*)0x4E7484;
                scoreAtStart = *(int32_t*)0x4E740C;
                maxPointAtStart = ReadMaxPoint();
                activeBluePoints = 0;
                { if (g_th15CaptureRateEnabled) ++total[activeMode][activeStage][activeChapter][activeDiff][activePlayer][0]; }
                { if (g_th15CaptureRateEnabled) ++current[activeMode][activeStage][activeChapter][activeDiff][activePlayer][0]; }
                displayIsBossSection = false;
                displaySection = 0;
                Save();
            }
            lastMisses = misses;
            lastBombs = bombs;
            static ULONGLONG lastLogTick = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - lastLogTick >= 1000) {
                lastLogTick = now;
            }
        }
        int32_t Total(int mode, int stage, int chapter, int diff, int player, int result) { Load(); return total[mode][stage][chapter][diff][player][result]; }
        int32_t Current(int mode, int stage, int chapter, int diff, int player, int result) const { return current[mode][stage][chapter][diff][player][result]; }
        int32_t SectionTotal(int mode, int stage, int section, int diff, int player, int result) { Load(); return sectionTotal[mode][stage][section][diff][player][result]; }
        int32_t SectionCurrent(int mode, int stage, int section, int diff, int player, int result) const { return sectionCurrent[mode][stage][section][diff][player][result]; }
        int32_t ReplayCurrent(int mode, int stage, int chapter, int diff, int player, int result) const { return replayCurrent[mode][stage][chapter][diff][player][result]; }
        int32_t ReplaySectionCurrent(int mode, int stage, int section, int diff, int player, int result) const { return replaySectionCurrent[mode][stage][section][diff][player][result]; }
        struct CaptureRateCounts {
            int totalAttempts = 0, totalPasses = 0;
            int sessionAttempts = 0, sessionPasses = 0;
        };
        CaptureRateCounts GetCaptureRateCounts(int stage, int chapter, int section,
            int diff, int player, bool isBossSection)
        {
            Load();
            CaptureRateCounts counts{};
            for (int mode = 0; mode < 2; ++mode) {
                counts.totalAttempts += isBossSection ? SectionTotal(mode, stage, section, diff, player, 0)
                                                      : Total(mode, stage, chapter, diff, player, 0);
                counts.totalPasses += isBossSection ? SectionTotal(mode, stage, section, diff, player, 1)
                                                    : Total(mode, stage, chapter, diff, player, 1);
                counts.sessionAttempts += replayDisplayOnly
                    ? (isBossSection ? ReplaySectionCurrent(mode, stage, section, diff, player, 0)
                                     : ReplayCurrent(mode, stage, chapter, diff, player, 0))
                    : (isBossSection ? SectionCurrent(mode, stage, section, diff, player, 0)
                                     : Current(mode, stage, chapter, diff, player, 0));
                counts.sessionPasses += replayDisplayOnly
                    ? (isBossSection ? ReplaySectionCurrent(mode, stage, section, diff, player, 1)
                                     : ReplayCurrent(mode, stage, chapter, diff, player, 1))
                    : (isBossSection ? SectionCurrent(mode, stage, section, diff, player, 1)
                                     : Current(mode, stage, chapter, diff, player, 1));
            }
            return counts;
        }
        bool IsActive() const { return active; }
        bool IsFailed() const { return failed; }
        bool IsReplayDisplayOnly() const { return replayDisplayOnly; }
        void OnBulletClearFinished(int32_t misses, int32_t bombs)
        {
            ResolvePendingCaptures(misses, bombs);
        }
        int ActiveMode() const { return activeMode; }
        int ActiveStage() const { return activeStage; }
        int ActiveChapter() const { return activeChapter; }
        int ActiveDiff() const { return activeDiff; }
        int ActivePlayer() const { return activePlayer; }
        bool HasDisplay() const
        {
            if (!displayValid || displayStage < 0 || displayStage >= 7)
                return false;
            if (displayIsBossSection
                ? (displaySection < 1 || displaySection >= 75
                    || BossSectionStage(displaySection) != displayStage)
                : (displayChapter < 0 || displayChapter >= kChapterCounts[displayStage]))
                return false;

            // Keep the selected chapter's saved targets visible in menus and
            // during scene transitions, even when the player object is absent.
            if (!active || !activeObservedGameplay || !GetMemContent(PLAYER_PTR))
                return true;
            if (displayIsBossSection && displayStage < 6) {
                if (*(int32_t*)0x4E73F0 != displayStage + 1)
                    return false;
                const int runtimeChapter = *(int32_t*)0x4E73F8;
                if (displaySection < kFirstEndBossSections[displayStage]) {
                    if (!MidSectionRuntimeMatches(displaySection, runtimeChapter))
                        return false;
                } else if (runtimeChapter < 40) {
                    return false;
                }
            }
            return true;
        }
        int DisplayMode() const { return displayMode; }
        int DisplayStage() const { return displayStage; }
        int DisplayChapter() const { return displayChapter; }
        int DisplayDiff() const { return displayDiff; }
        int DisplayPlayer() const { return displayPlayer; }
        bool DisplayIsBossSection() const { return displayIsBossSection; }
        int DisplaySection() const { return displaySection; }
        bool ActiveIsBossSection() const { return active && activeIsBossSection; }
        int ActiveSection() const { return activeSection; }
        void ShowChapterRates()
        {
            static int diff = -1;
            static int player = -1;
            static TH15NativeSpellSave nativeSpells;
            static const int bossRanges[7][2] = {
                { TH15_ST1_BOSS1, TH15_ST1_BOSS4 }, { TH15_ST2_BOSS1, TH15_ST2_BOSS6 },
                { TH15_ST3_BOSS1, TH15_ST3_BOSS7 }, { TH15_ST4_BOSS1, TH15_ST4_BOSS7 },
                { TH15_ST5_BOSS1, TH15_ST5_BOSS8 }, { TH15_ST6_BOSS1, TH15_ST6_BOSS11 },
                { TH15_ST7_MID1, TH15_ST7_END_S10 }
            };
            static const int midRanges[4][2] = {
                { TH15_ST1_MID1, TH15_ST1_MID3 },
                { TH15_ST2_MID1, TH15_ST2_MID1 },
                { TH15_ST3_MID1, TH15_ST3_MID2 },
                { TH15_ST4_MID1, TH15_ST4_MID1 }
            };
            std::vector<std::pair<int, const char*>> difficulties, shots;
            for (int i = 0; i < 5; ++i)
                difficulties.emplace_back(i, S(IGI_DIFF[i]));
            for (int i = 0; i < 4; ++i)
                shots.emplace_back(i, S(IGI_PL_15[i]));
            CaptureTable::DrawTabs("##th15_rate_difficulty", difficulties, diff, 3);
            CaptureTable::DrawTabs("##th15_rate_shot", shots, player, 0);

            const int locale = Gui::LocaleGet();
            const char* nameHeader = locale == Gui::LOCALE_ZH_CN ? "攻击 / 章节"
                : locale == Gui::LOCALE_JA_JP ? "攻撃 / チャプター" : "Attack / Chapter";
            if (!CaptureTable::Begin("th15_chapter_rates", nameHeader))
                return;
            auto addRow = [&](const char* name, int stage, int chapter, int section, bool isBoss) {
                const int recordDiff = std::min(diff, 4);
                const auto counts = GetCaptureRateCounts(stage, chapter, section, recordDiff, player, isBoss);
                int attempts = counts.totalAttempts, passes = counts.totalPasses;
                const int sessionAttempts = counts.sessionAttempts, sessionPasses = counts.sessionPasses;
                const int cardId = isBoss ? TH15SpellCardId(section, diff) : 0;
                const bool nativeUnavailable = cardId && !nativeSpells.Get(player, cardId, passes, attempts);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (stage == 6)
                    ImGui::TextUnformatted(S(TH_STAGE_EXTRA));
                else
                    ImGui::Text("%d", stage + 1);
                ImGui::TableNextColumn();
                CaptureTable::DrawName(name);
                ImGui::TableNextColumn();
                if (nativeUnavailable) ImGui::TextUnformatted("--");
                else CaptureTable::DrawRate(passes, attempts);
                ImGui::TableNextColumn();
                CaptureTable::DrawRate(sessionPasses, sessionAttempts);
            };
            for (int stage = 0; stage < 7; ++stage) {
                if ((stage == 6) != (diff == 4)) continue;
                for (int chapter = 0; chapter < kChapterCounts[stage]; ++chapter) {
                    auto label = std::format("{} {}", S(TH_CHAPTER), chapter + 1);
                    if (locale == Gui::LOCALE_ZH_CN) {
                        if (const char* nickname = TH15ChapterNickname(stage, chapter + 1))
                            label += std::format("（{}）", nickname);
                    }
                    addRow(label.c_str(), stage, chapter, 0, false);
                }
                if (stage < 4) {
                    for (int section = midRanges[stage][0]; section <= midRanges[stage][1]; ++section) {
                        if (section == TH15_ST1_MID3 && diff < 2)
                            continue;
                        const char* sectionName = th_sections_str[locale][std::min(diff, 4)][section];
                        addRow(sectionName ? sectionName : "Midboss section", stage, 0, section, true);
                    }
                }
                for (int section = bossRanges[stage][0]; section <= bossRanges[stage][1]; ++section) {
                    const char* sectionName = th_sections_str[locale][std::min(diff, 4)][section];
                    addRow(sectionName ? sectionName : "Boss section", stage, 0, section, true);
                }
            }
            ImGui::EndTable();
        }
        void ShowChapterGoals()
        {
            LoadChapterStats();
            LoadChapterGoals();
            static int stage = 1, chapter = 1, diff = 3, player = 0;
            static int selectedSection = TH15_ST1_BOSS1;
            const int locale = Gui::LocaleGet();
            const char* modeItems = locale == Gui::LOCALE_ZH_CN
                ? "自动：所有记录最高值\0手动\0"
                : locale == Gui::LOCALE_JA_JP
                    ? "自動：全記録の最高値\0手動\0"
                    : "Auto: best across records\0Manual\0";
            if (ImGui::Combo(locale == Gui::LOCALE_ZH_CN ? "HUD目标方式"
                    : locale == Gui::LOCALE_JA_JP ? "HUD目標方式" : "HUD goal mode",
                    &chapterGoalMode, modeItems))
                SaveChapterGoals();

            const char* stageLabel = locale == Gui::LOCALE_ZH_CN ? "面数"
                : locale == Gui::LOCALE_JA_JP ? "ステージ" : "Stage";
            const char* chapterLabel = locale == Gui::LOCALE_ZH_CN ? "章节"
                : locale == Gui::LOCALE_JA_JP ? "チャプター" : "Chapter";
            ImGui::Columns(2, "th15_chapter_goal_filters");
            ImGui::SetNextItemWidth(130.0f);
            ImGui::InputInt(stageLabel, &stage, 1, 1);
            stage = std::clamp(stage, 1, 7);
            ImGui::NextColumn();
            ImGui::SetNextItemWidth(130.0f);
            ImGui::InputInt(chapterLabel, &chapter, 1, 1);
            chapter = std::clamp(chapter, 1, kChapterCounts[stage - 1]);
            ImGui::Columns(1);

            std::string diffItems, playerItems;
            for (int i = 0; i < 5; ++i) {
                diffItems += S(IGI_DIFF[i]);
                diffItems.push_back('\0');
            }
            for (int i = 0; i < 4; ++i) {
                playerItems += S(IGI_PL_15[i]);
                playerItems.push_back('\0');
            }
            diffItems.push_back('\0');
            playerItems.push_back('\0');
            diff = std::clamp(diff, 0, 4);
            player = std::clamp(player, 0, 3);
            std::vector<int> sectionIds;
            std::string sectionItems;
            for (int section = 1; section < 75; ++section) {
                if (BossSectionStage(section) != stage - 1) continue;
                sectionIds.push_back(section);
                const char* name = th_sections_str[locale][diff][section];
                sectionItems += std::format("{}: {}", section, name ? name : "Section");
                sectionItems.push_back('\0');
            }
            sectionItems.push_back('\0');
            if (!sectionIds.empty()) {
                auto it = std::find(sectionIds.begin(), sectionIds.end(), selectedSection);
                int selection = it == sectionIds.end() ? 0 : int(it - sectionIds.begin());
                if (ImGui::Combo(locale == Gui::LOCALE_ZH_CN ? "非符/符卡章节"
                        : locale == Gui::LOCALE_JA_JP ? "通常/スペル章" : "Nonspell / spell section",
                        &selection, sectionItems.c_str()))
                    selectedSection = sectionIds[selection];
                else if (it == sectionIds.end())
                    selectedSection = sectionIds[0];
            }
            ImGui::Columns(2, "th15_chapter_goal_character");
            ImGui::Combo(S(TH_DIFFICULTY), &diff, diffItems.c_str());
            ImGui::NextColumn();
            ImGui::Combo("Player", &player, playerItems.c_str());
            ImGui::Columns(1);

            const int stageIndex = stage - 1;
            const int chapterIndex = chapter - 1;
            auto valueOrDash = [](bool available, auto value) {
                return available ? std::to_string(value) : std::string("--");
            };
            auto bestAcrossSources = [&](bool isSection, int chapterOrSection) {
                ChapterStatBest best{};
                for (int source = 0; source < 3; ++source) {
                    const auto& record = isSection
                        ? sectionStatBest[source][stageIndex][chapterOrSection][diff][player]
                        : chapterStatBest[source][stageIndex][chapterOrSection][diff][player];
                    if (record.hasBluePoints && (!best.hasBluePoints || record.bluePoints > best.bluePoints)) {
                        best.bluePoints = record.bluePoints;
                        best.hasBluePoints = true;
                    }
                    if (record.hasGraze && (!best.hasGraze || record.graze > best.graze)) {
                        best.graze = record.graze;
                        best.hasGraze = true;
                    }
                    if (record.hasMaxPointDelta && (!best.hasMaxPointDelta || record.maxPointDelta > best.maxPointDelta)) {
                        best.maxPointDelta = record.maxPointDelta;
                        best.hasMaxPointDelta = true;
                    }
                    if (record.hasChapterScore && (!best.hasChapterScore || record.chapterScore > best.chapterScore)) {
                        best.chapterScore = record.chapterScore;
                        best.hasChapterScore = true;
                    }
                }
                return best;
            };
            auto showGoalGroup = [&](const char* id, const char* title,
                const ChapterStatBest& bestRecord, ManualChapterGoal& manual) {
                ImGui::Separator();
                ImGui::TextUnformatted(title);
                ImGui::Text(locale == Gui::LOCALE_ZH_CN
                    ? "最高记录：蓝点 %s  擦弹 %s  最大得点增量 +%s  得分增量 +%s"
                    : locale == Gui::LOCALE_JA_JP
                        ? "最高記録：青点 %s  グレイズ %s  最大得点増分 +%s  スコア増分 +%s"
                        : "Best record: Blue %s  Graze %s  Max point delta +%s  Score delta +%s",
                    valueOrDash(bestRecord.hasBluePoints, bestRecord.bluePoints).c_str(),
                    valueOrDash(bestRecord.hasGraze, bestRecord.graze).c_str(),
                    valueOrDash(bestRecord.hasMaxPointDelta, bestRecord.maxPointDelta).c_str(),
                    valueOrDash(bestRecord.hasChapterScore, bestRecord.chapterScore).c_str());
                if (chapterGoalMode != 1) return;

                ImGui::PushID(id);
                bool changed = false;
                auto inputGoal = [&](const char* label, int32_t& value) {
                    if (ImGui::InputInt(label, &value, 1, 10)) {
                        value = std::max(-1, value);
                        changed = true;
                    }
                };
                inputGoal(locale == Gui::LOCALE_ZH_CN ? "蓝点目标（-1关闭）" : "Blue point goal (-1 off)", manual.bluePoints);
                inputGoal(locale == Gui::LOCALE_ZH_CN ? "擦弹目标（-1关闭）" : "Graze goal (-1 off)", manual.graze);
                inputGoal(locale == Gui::LOCALE_ZH_CN ? "最大得点增量目标（-1关闭）" : "Max point delta goal (-1 off)", manual.maxPointDelta);
                const int64_t scoreStep = 10000;
                const int64_t scoreFastStep = 100000;
                if (ImGui::InputScalar(locale == Gui::LOCALE_ZH_CN ? "章节得分增量目标（-1关闭）" : "Score delta goal (-1 off)",
                        ImGuiDataType_S64, &manual.chapterScore, &scoreStep, &scoreFastStep)) {
                    manual.chapterScore = std::max<int64_t>(-1, manual.chapterScore);
                    changed = true;
                }
                if (changed)
                    SaveChapterGoals();

                if (ImGui::Button(locale == Gui::LOCALE_ZH_CN ? "用最高记录填入手动目标"
                        : locale == Gui::LOCALE_JA_JP ? "最高記録を手動目標にコピー"
                        : "Copy best record to manual goals")) {
                    manual.bluePoints = bestRecord.hasBluePoints ? bestRecord.bluePoints : -1;
                    manual.graze = bestRecord.hasGraze ? bestRecord.graze : -1;
                    manual.maxPointDelta = bestRecord.hasMaxPointDelta ? bestRecord.maxPointDelta : -1;
                    manual.chapterScore = bestRecord.hasChapterScore ? bestRecord.chapterScore : -1;
                    SaveChapterGoals();
                }
                ImGui::PopID();
            };
            if (locale == Gui::LOCALE_ZH_CN)
                ImGui::TextDisabled("自动目标取 R/S/P 所有已保存记录中各项最高值；手动目标按道中和符卡分别保存。");
            else if (locale == Gui::LOCALE_JA_JP)
                ImGui::TextDisabled("自動目標は各項目の最高記録を使い、手動目標は種類ごとに保存します。");
            else
                ImGui::TextDisabled("Auto goals use each metric's best across R/S/P. Manual goals are saved separately by type.");

            const auto chapterBest = bestAcrossSources(false, chapterIndex);
            auto& chapterManual = manualGoals[stageIndex][chapterIndex][diff][player];
            showGoalGroup("stage_chapter", locale == Gui::LOCALE_ZH_CN ? "道中章节"
                : locale == Gui::LOCALE_JA_JP ? "道中章" : "Stage chapter",
                chapterBest, chapterManual);

            if (!sectionIds.empty()) {
                const auto sectionBest = bestAcrossSources(true, selectedSection);
                auto& sectionManual = sectionManualGoals[stageIndex][selectedSection][diff][player];
                const char* sectionTitle = th_sections_str[locale][diff][selectedSection];
                const std::string title = std::format("{}{}", locale == Gui::LOCALE_ZH_CN
                    ? "非符/符卡：" : locale == Gui::LOCALE_JA_JP ? "通常/スペル：" : "Nonspell / spell: ",
                    sectionTitle ? sectionTitle : "Section");
                showGoalGroup("boss_section", title.c_str(), sectionBest, sectionManual);
            }
        }
        void ShowChapterRecords()
        {
            ShowChapterGoals();
            ImGui::Separator();
            const int locale = Gui::LocaleGet();
            const char* helpLabel = locale == Gui::LOCALE_ZH_CN ? "记录存储说明"
                : locale == Gui::LOCALE_JA_JP ? "記録保存の説明" : "Record storage guide";
            if (ImGui::CollapsingHeader(helpLabel)) {
                if (locale == Gui::LOCALE_ZH_CN) {
                    ImGui::TextWrapped("每个面数、道中章节或非符/符卡、难度和机体组合只保存一条最佳记录；R（Replay）、S（实战）、P（练习）共用这一条，来源栏标出胜出的记录来源。");
                    ImGui::BulletText("比较顺序：章节通过优先；通过记录按章节 Bonus、章节得分增量、最大得点增量、蓝点数依次比较。相同成绩保留先记录的一条。");
                    ImGui::BulletText("自动目标按每项指标汇总所有来源的最高值；切换到手动后，可分别编辑道中和非符/符卡目标，也可把最高记录复制为目标。-1 表示不设该项目标。");
                    ImGui::BulletText("实时 HUD 的当前值来自正在进行的章节；切换章节后会保留上一章节标题和结算值至少 2 秒，空场景过场仍保留上一章。击破率和 RES 也会显示在记录行下方。");
                    ImGui::BulletText("章节成绩文件位于 %APPDATA%\\ShanghaiAlice\\th15\\score15chapter_stats_v4.tsv。旧版 v4 多次记录会在读取时自动压缩为每组一条最佳记录。");
                } else if (locale == Gui::LOCALE_JA_JP) {
                    ImGui::TextWrapped("ステージ、道中/通常弾幕・スペル、難易度、自機の組み合わせにつき、最高記録を1件保存します。R（Replay）、S（実戦）、P（練習）は同じ枠を共有し、勝った記録の種別を表示します。");
                    ImGui::BulletText("優先順：クリア、章Bonus、章スコア増分、最大得点増分、青点数。同点なら先の記録を維持します。");
                    ImGui::BulletText("自動目標は全種別の各項目最高値です。手動では道中とボス区間を別々に編集でき、-1で項目を無効にします。");
                    ImGui::BulletText("記録ファイル：%APPDATA%\\ShanghaiAlice\\th15\\score15chapter_stats_v4.tsv。旧形式の複数記録は読み込み時に各組の最高1件へ整理します。");
                } else {
                    ImGui::TextWrapped("One best row is kept per stage, chapter or boss section, difficulty, and shot. Replay (R), combat (S), and practice (P) share that row; its source column shows which attempt won.");
                    ImGui::BulletText("Ranking: clear first, then Chapter Bonus, score delta, max-point delta, and blue points. Ties keep the earlier row.");
                    ImGui::BulletText("Auto goals use each metric's maximum across all sources. Manual goals are separate for stage chapters and boss sections; use -1 to disable a metric.");
                    ImGui::BulletText("File: %APPDATA%\\ShanghaiAlice\\th15\\score15chapter_stats_v4.tsv. Older multi-attempt v4 files are reduced to one best row per key when loaded.");
                }
            }
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            std::ifstream file("score15chapter_stats_v4.tsv");
            std::vector<std::string> rows;
            std::string line;
            if (file.is_open()) {
                std::getline(file, line); // header
                while (std::getline(file, line)) {
                    if (!line.empty()) rows.push_back(line);
                }
            }
            bool currentRecordFormat = !rows.empty();
            if (rows.empty()) {
                file.close();
                file.open("score15chapter_stats_v3.tsv");
                if (file.is_open()) {
                    std::getline(file, line); // legacy header
                    while (std::getline(file, line)) {
                        if (!line.empty()) rows.push_back(line);
                    }
                }
            }
            PopCurrentDirectory();

            std::sort(rows.begin(), rows.end(), [](const std::string& left, const std::string& right) {
                return left.substr(0, 23) < right.substr(0, 23);
            });
            ImGui::TextUnformatted(locale == Gui::LOCALE_ZH_CN ? "各机体/章节/难度最佳记录（最近 100 条）"
                : locale == Gui::LOCALE_JA_JP ? "自機/章/難易度ごとのベスト記録（最新100件）"
                : "Best records per shot/chapter/difficulty (latest 100)");
            if (ImGui::BeginTable("th15_chapter_records", 15,
                ImGuiTableFlags_::ImGuiTableFlags_Resizable | ImGuiTableFlags_::ImGuiTableFlags_Borders
                | ImGuiTableFlags_::ImGuiTableFlags_RowBg | ImGuiTableFlags_::ImGuiTableFlags_ScrollY
                | ImGuiTableFlags_::ImGuiTableFlags_ScrollX,
                ImVec2(0.0f, 260.0f))) {
                const char* headers[] = {
                    locale == Gui::LOCALE_ZH_CN ? "时间" : locale == Gui::LOCALE_JA_JP ? "日時" : "Time",
                    locale == Gui::LOCALE_ZH_CN ? "来源" : locale == Gui::LOCALE_JA_JP ? "種別" : "Source",
                    locale == Gui::LOCALE_ZH_CN ? "章节" : locale == Gui::LOCALE_JA_JP ? "チャプター" : "Chapter",
                    locale == Gui::LOCALE_ZH_CN ? "难度/机体" : locale == Gui::LOCALE_JA_JP ? "難易度/自機" : "Difficulty/shot",
                    locale == Gui::LOCALE_ZH_CN ? "Miss" : "Miss",
                    locale == Gui::LOCALE_ZH_CN ? "Bomb" : "Bomb",
                    locale == Gui::LOCALE_ZH_CN ? "擦弹" : locale == Gui::LOCALE_JA_JP ? "グレイズ" : "Graze",
                    locale == Gui::LOCALE_ZH_CN ? "击破率" : locale == Gui::LOCALE_JA_JP ? "撃破率" : "Kill rate",
                    locale == Gui::LOCALE_ZH_CN ? "资源进度" : locale == Gui::LOCALE_JA_JP ? "リソース" : "Resource",
                    locale == Gui::LOCALE_ZH_CN ? "章节Bonus" : locale == Gui::LOCALE_JA_JP ? "章ボーナス" : "Chapter Bonus",
                    locale == Gui::LOCALE_ZH_CN ? "蓝点" : locale == Gui::LOCALE_JA_JP ? "青点" : "Blue points",
                    locale == Gui::LOCALE_ZH_CN ? "最大得点 起→终（增量）" : locale == Gui::LOCALE_JA_JP ? "最大得点 開始→終了（増分）" : "Max point start→end (delta)",
                    locale == Gui::LOCALE_ZH_CN ? "得分 起→终（增量）" : locale == Gui::LOCALE_JA_JP ? "スコア 開始→終了（増分）" : "Score start→end (delta)",
                    locale == Gui::LOCALE_ZH_CN ? "碎片" : locale == Gui::LOCALE_JA_JP ? "欠片" : "Fragment",
                    locale == Gui::LOCALE_ZH_CN ? "章节通过" : locale == Gui::LOCALE_JA_JP ? "章クリア" : "Chapter cleared"
                };
                for (const auto* header : headers) ImGui::TableSetupColumn(header);
                ImGui::TableHeadersRow();
                const size_t first = rows.size() > 100 ? rows.size() - 100 : 0;
                for (size_t i = rows.size(); i > first; --i) {
                    std::array<std::string, 24> cells{};
                    std::stringstream row(rows[i - 1]);
                    for (auto& cell : cells) std::getline(row, cell, '\t');
                    if (cells[0].empty()) continue;
                    if (!currentRecordFormat) {
                        const auto legacy = cells;
                        cells = {};
                        cells[0] = legacy[0]; cells[1] = legacy[1];
                        cells[2] = legacy[2]; cells[3] = legacy[3];
                        cells[4] = legacy[4]; cells[5] = legacy[5]; cells[6] = legacy[6];
                        cells[7] = cells[8] = "--";
                        cells[9] = legacy[7]; cells[10] = legacy[8];
                        cells[11] = cells[12] = cells[13] = "--";
                        cells[14] = legacy[11];
                        cells[16] = "--"; cells[17] = legacy[12]; cells[18] = "--";
                        cells[19] = cells[20] = "--"; cells[21] = legacy[9];
                        cells[22] = legacy[10];
                        cells[23] = "--";
                    }
                    ImGui::TableNextRow();
                    const int recordStage = std::stoi(cells[2]);
                    const int recordChapter = std::stoi(cells[3]);
                    const int recordSection = std::stoi(cells[4]);
                    const int recordDiff = std::clamp(std::stoi(cells[5]), 0, 4);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[0].c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[1].c_str());
                    ImGui::TableNextColumn();
                    if (recordSection > 0 && recordSection < 75) {
                        const char* name = th_sections_str[locale][recordDiff][recordSection];
                        if (name) ImGui::Text("%s %d - %s", S(TH_STAGE), recordStage, name);
                        else ImGui::Text("%s %d - %s %d", S(TH_STAGE), recordStage,
                            locale == Gui::LOCALE_ZH_CN ? "章节" : "Section", recordSection);
                    } else {
                        ImGui::Text("%s %d - %s %d", S(TH_STAGE), recordStage,
                            S(TH_CHAPTER), recordChapter);
                    }
                    ImGui::TableNextColumn(); ImGui::Text("%s / %s",
                        S(IGI_DIFF[recordDiff]), S(IGI_PL_15[std::clamp(std::stoi(cells[6]), 0, 3)]));
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[7].c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[8].c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[9].c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[10].c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s / 200", cells[11].c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[13].c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[14].c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s → %s (+%s)",
                        cells[16].c_str(), cells[17].c_str(), cells[18].c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s → %s (+%s)",
                        cells[19].c_str(), cells[20].c_str(), cells[21].c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[12].c_str());
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(cells[23].c_str());
                }
                ImGui::EndTable();
            }
        }
        void ResetRun() {
            ResolvePendingCaptures(lastMisses, lastBombs);
            RecordInterruptedAttempt();
            lastLiveSnapshot.valid = false;
            transitionSnapshot.valid = false;
            transitionSnapshotUntil = 0;
            active = false;
            failed = false;
            settlementPending = false;
            settlementCaptured = false;
            bossIntroPending = false;
            bossGapPending = false;
            settledRuntimeChapter = -1;
            practiceFinished = false;
            completedAwaitingNext = false;
            finishedPracticeStage = finishedPracticeChapter = -1;
            displayValid = false;
            replayDisplayOnly = false;
            pendingCaptures.clear();
            captureRateResumePending = false;
            memset(replayCurrent, 0, sizeof(replayCurrent));
            memset(replaySectionCurrent, 0, sizeof(replaySectionCurrent));
        }
        void SetCaptureRateEnabled(bool enabled)
        {
            if (captureRateEnabled == enabled)
                return;
            captureRateEnabled = enabled;
            if (!enabled) {
                active = false;
                displayValid = false;
                transitionSnapshot.valid = false;
                transitionSnapshotUntil = 0;
                pendingCaptures.clear();
                captureRateResumePending = false;
            } else {
                captureRateResumePending = true;
            }
        }
        void SetOnlyCurrentPracticeChapter(bool enabled) { onlyCurrentPracticeChapter = enabled; }
        // Route trace: the chapter and boss section this attempt is filed under.
        // A practice jump uses the same keys (chapter - 1 for a chapter jump, the
        // section id for a midboss/boss section), so a route mark labelled here
        // can be matched against the practice menu without re-deriving the map.
        bool RouteAnchor(int& logicalChapter, int& section) const
        {
            if (!active)
                return false;
            logicalChapter = activeChapter;
            section = activeIsBossSection ? activeSection : 0;
            return true;
        }
    };

    // Bridge for TH15RouteTrace: -1 when no attempt is being tracked.
    static void TH15RouteAnchor(int& logicalChapter, int& section)
    {
        logicalChapter = -1;
        section = 0;
        TH15ChapterSave::singleton().RouteAnchor(logicalChapter, section);
    }

    using std::pair;
    bool g_blind_view = false;
    float g_blind_size = 150.0f;
    // Time spent waiting in the patched ECL. This is NOT the amount of original
    // content skipped by the warp; those clocks have different origins.
    static int g_th15WarpWaitFrames = 0;
    // Every installed practice warp starts a new route clock, including retries
    // made before either game frame counter has advanced.
    static unsigned g_th15RouteWarpGeneration = 0;

#if THPRAC_TH15_HITBOX_FEATURE
    bool g_show_bullet_hitbox = false;
    static bool g_th15CircleHitboxEnabled = false;
    static bool g_th15RectHitboxEnabled = false;
    static bool g_th15LaserHitboxEnabled = false;
    static void TH15SetHitboxCaptureHooks(bool enabled);
    static LPDIRECT3DTEXTURE9 g_th15_hitbox_texture = nullptr;
    struct th15_bullet_hitbox_draw {
        ImVec2 pos;
        float radius;
    };
    struct th15_rect_hitbox_draw {
        ImVec2 pos1;
        ImVec2 pos2;
    };
    struct th15_laser_hitbox_draw {
        ImVec2 origin;
        ImVec2 playerHitbox;
        float width;
        float height;
        float angle;
    };
    std::vector<th15_bullet_hitbox_draw> g_th15_bullet_hit_draw_vec;
    std::vector<th15_rect_hitbox_draw> g_th15_rect_hit_draw_vec;
    std::vector<th15_laser_hitbox_draw> g_th15_laser_hit_draw_vec;
#endif

    struct THPracParam {
        int32_t mode;
        int32_t stage;
        int32_t section;
        int32_t phase;
        int32_t st2_boss2_hp = kTh15St2Boss3EffectiveHpMax;
        bool pure_sign_fixed_seed = false;
        int32_t pure_sign_seed = 0;

        int64_t score;
        int32_t life;
        int32_t life_fragment;
        int32_t bomb;
        int32_t bomb_fragment;
        int32_t power;
        int32_t value;
        int32_t graze;
        int32_t reisen_shield;

        float doremy_normal_1_phase; // used for 123 Normal 1
        float enhanced_para; // used for st6boss6

        bool dlg;

        bool _playLock = false;
        void Reset()
        {
            memset(this, 0, sizeof(THPracParam));
            st2_boss2_hp = kTh15St2Boss3EffectiveHpMax;
        }
        bool ReadJson(std::string& json)
        {
            ParseJson();

            ForceJsonValue(game, "th15");
            GetJsonValue(mode);
            GetJsonValue(stage);
            GetJsonValue(section);
            GetJsonValue(phase);
            GetJsonValue(st2_boss2_hp);
            st2_boss2_hp = std::clamp(st2_boss2_hp,
                0, kTh15St2Boss3EffectiveHpMax);
            GetJsonValueEx(pure_sign_fixed_seed, Bool);
            // Reject out-of-range numeric types before conversion to an int.
            GetJsonValueEx(pure_sign_seed, Int);
            pure_sign_seed = std::clamp(pure_sign_seed, 0, 65535);
            GetJsonValueEx(dlg, Bool);

            GetJsonValue(score);
            GetJsonValue(life);
            GetJsonValue(life_fragment);
            GetJsonValue(bomb);
            GetJsonValue(bomb_fragment);
            GetJsonValue(power);
            GetJsonValue(value);
            GetJsonValue(graze);
            GetJsonValue(doremy_normal_1_phase);
            GetJsonValue(enhanced_para);
            GetJsonValue(reisen_shield);

            return true;
        }
        std::string GetJson()
        {
            CreateJson();

            AddJsonValueEx(version, GetVersionStr(), jalloc);
            AddJsonValueEx(game, "th15", jalloc);
            AddJsonValue(mode);
            AddJsonValue(stage);
            if (section)
                AddJsonValue(section);
            if (phase)
                AddJsonValue(phase);
            if (section == TH15_ST2_BOSS3)
                AddJsonValue(st2_boss2_hp);
            if (dlg)
                AddJsonValue(dlg);
            if (section == TH15_ST3_BOSS1)
                AddJsonValue(doremy_normal_1_phase);
            if (section == TH15_ST6_BOSS6)
                AddJsonValue(enhanced_para);
            if (section == TH15_ST6_BOSS11 && pure_sign_fixed_seed) {
                AddJsonValue(pure_sign_fixed_seed);
                AddJsonValue(pure_sign_seed);
            }

            AddJsonValue(score);
            AddJsonValue(life);
            AddJsonValue(life_fragment);
            AddJsonValue(bomb);
            AddJsonValue(bomb_fragment);
            AddJsonValue(power);
            AddJsonValue(value);
            AddJsonValue(graze);
            if (reisen_shield)
                AddJsonValue(reisen_shield);

            ReturnJson();
        }
    };
    THPracParam thPracParam {};

    // Each emitter owns a native TH15 RNG state. Its init instruction resets
    // that state even on a checkpoint retry that reuses the patched ECL.
    // Separating emitters also keeps their nth circle reproducible when other
    // emitters or unrelated game systems consume random values between shots.
    struct TH15PureSignRngState {
        uint16_t seed = 0;
        uint16_t padding = 0;
        uint32_t calls = 0;
    };
    static_assert(sizeof(TH15PureSignRngState) == 8);
    static std::array<TH15PureSignRngState, 5> g_th15PureSignRng;
    static constexpr int32_t kTh15PureSignAngleVar = -19998;
    static constexpr int32_t kTh15PureSignResetVar = -20008;
    static std::array<float, 5> g_th15PureSignInitialAngles;

    struct TH15PureSignRngSite {
        size_t init;
        size_t rotation; // zero for the center emitter, which never rotates
        size_t offset;
    };
    static constexpr TH15PureSignRngSite kTh15PureSignRngSites[] = {
        { 0xc2c8, 0, 0xc384 },
        { 0xc554, 0xc67c, 0xc698 },
        { 0xc868, 0xc990, 0xc9ac },
        { 0xcb7c, 0xcca4, 0xccc0 },
        { 0xce90, 0xcfb8, 0xcfd4 },
    };

    static bool TH15PureSignSeedEnabled()
    {
        return thPracParam.mode == 1 && thPracParam.section == TH15_ST6_BOSS11
            && thPracParam.pure_sign_fixed_seed;
    }

    static int TH15PureSignVariableIndex(int32_t variable, int32_t base)
    {
        return variable <= base && variable > base - 5 ? base - variable : -1;
    }

    using TH15GlobalFloatGetter = float(__thiscall*)(void*, int32_t);
    static TH15GlobalFloatGetter g_th15OriginalGlobalFloatGetter = nullptr;

    static float __fastcall TH15PureSignGlobalFloat(void* enemy,
        [[maybe_unused]] void* edx, int32_t variable)
    {
        const int reset = TH15PureSignVariableIndex(variable, kTh15PureSignResetVar);
        if (reset >= 0) {
            if (TH15PureSignSeedEnabled()) {
                // An odd stride gives each emitter a distinct, reproducible
                // 16-bit seed without advancing the game's shared RNG.
                g_th15PureSignRng[reset] = {
                    static_cast<uint16_t>(thPracParam.pure_sign_seed + reset * 0x9e37u), 0, 0 };
            }
            return g_th15PureSignInitialAngles[reset];
        }
        const int source = TH15PureSignVariableIndex(variable, kTh15PureSignAngleVar);
        if (source >= 0) {
            // Preserve the original shared RNG consumption for player shots,
            // other game systems and replay checks, even when substituting the
            // angle with the emitter's private sequence.
            const float original = g_th15OriginalGlobalFloatGetter(enemy, -9998);
            if (TH15PureSignSeedEnabled()) {
                const float unit = asm_call<0x403840, Thiscall, float>(
                    &g_th15PureSignRng[source]);
                return unit * *reinterpret_cast<const float*>(0x4cff3c);
            }
            return original;
        }
        return g_th15OriginalGlobalFloatGetter(enemy, variable);
    }

    static void TH15InstallPureSignRngGetter()
    {
        if (g_th15OriginalGlobalFloatGetter)
            return;
        // Enemy's float-global virtual method, used by ECL's float operand
        // reader (0x48e8db). Keep the previous method for all ordinary IDs.
        void* vtable = reinterpret_cast<void*>(0x4cc2e8);
        HookVTable(&vtable, 3, reinterpret_cast<void*>(TH15PureSignGlobalFloat),
            reinterpret_cast<void**>(&g_th15OriginalGlobalFloatGetter));
    }

    static bool TH15PatchPureSignRng(ECLHelper& ecl, bool enabled)
    {
        // Check the whole patch set before writing anything. Accept both the
        // original operands and ours so a reused ECL can be restored on disable.
        for (size_t i = 0; i < elementsof(kTh15PureSignRngSites); ++i) {
            const auto& site = kTh15PureSignRngSites[i];
            const auto mask = TH15ECLReadU32(ecl, site.init + 8) & 0xffff;
            if (TH15ECLReadU32(ecl, site.init + 4) != 0x001c025c
                || (mask != 1 && mask != 3))
                return false;
            const uint32_t initBits = TH15ECLReadU32(ecl, site.init + 20);
            float initOperand;
            std::memcpy(&initOperand, &initBits, sizeof(initOperand));
            if (!(std::abs(initOperand - 1.57079637f) <= 0.00001f)
                && initOperand != float(kTh15PureSignResetVar - int(i)))
                return false;
            for (const size_t offset : { site.rotation, site.offset }) {
                if (!offset) continue;
                const auto expected = offset == site.rotation ? 0x001c025cu : 0x001c0272u;
                if (TH15ECLReadU32(ecl, offset + 4) != expected)
                    return false;
                const uint32_t bits = TH15ECLReadU32(ecl, offset + 20);
                float operand;
                std::memcpy(&operand, &bits, sizeof(operand));
                if (operand != -9998.0f && operand != float(kTh15PureSignAngleVar - int(i)))
                    return false;
            }
        }
        if (enabled)
            TH15InstallPureSignRngGetter();
        for (size_t i = 0; i < elementsof(kTh15PureSignRngSites); ++i) {
            const auto& site = kTh15PureSignRngSites[i];
            const uint32_t bits = TH15ECLReadU32(ecl, site.init + 20);
            float current;
            std::memcpy(&current, &bits, sizeof(current));
            if (current != float(kTh15PureSignResetVar - int(i)))
                g_th15PureSignInitialAngles[i] = current;
            ecl << pair{ site.init + 8, uint16_t(enabled ? 3 : 1) }
                << pair{ site.init + 20, enabled ? float(kTh15PureSignResetVar - int(i))
                    : g_th15PureSignInitialAngles[i] };
            for (const size_t offset : { site.rotation, site.offset }) {
                if (offset) ecl << pair{ offset + 20,
                    enabled ? float(kTh15PureSignAngleVar - int(i)) : -9998.0f };
            }
        }
        return true;
    }

    class THGuiPrac : public Gui::GameGuiWnd {
        THGuiPrac() noexcept
        {
            SetPracticePanel();
            *mMode = 1;
            *mLife = 8;
            *mBomb = 8;
            *mPower = 400;
            *mValue = 10000;
            *mBossHp = kTh15St2Boss3EffectiveHpMax;

            SetFade(0.8f, 0.1f);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
            OnLocaleChange();
        }
        SINGLETON(THGuiPrac)
    public:

        __declspec(noinline) void State(int state)
        {
            switch (state) {
            case 0:
                break;
            case 1:
                mDiffculty = *((int32_t*)0x4e7410);
                SetFade(0.8f, 0.1f);
                Open();
                thPracParam.Reset();
            case 2:
                break;
            case 3:
                SetFade(0.8f, 0.1f);
                Close();

                // Fill Param
                thPracParam.mode = *mMode;
                thPracParam.stage = *mStage;
                thPracParam.section = CalcSection();
                thPracParam.phase = SpellPhase() ? *mPhase : 0;
                if (thPracParam.section == TH15_ST2_BOSS3) {
                    thPracParam.st2_boss2_hp = std::clamp(*mBossHp,
                        0, kTh15St2Boss3EffectiveHpMax);
                }
                if (thPracParam.mode == 1 && thPracParam.section == TH15_ST6_BOSS11) {
                    thPracParam.pure_sign_fixed_seed = *mPureSignFixedSeed;
                    thPracParam.pure_sign_seed = std::clamp(*mPureSignSeed, 0, 65535);
                }
                if (SectionHasDlg(thPracParam.section))
                    thPracParam.dlg = *mDlg;

                thPracParam.score = *mScore;
                thPracParam.doremy_normal_1_phase = *mDoremyNormal1Phase;
                thPracParam.enhanced_para = *mEnhanced_para;
                thPracParam.life = *mLife;
                thPracParam.life_fragment = *mLifeFragment;
                thPracParam.bomb = *mBomb;
                thPracParam.bomb_fragment = *mBombFragment;
                thPracParam.power = *mPower;
                thPracParam.value = *mValue;
                thPracParam.graze = *mGraze;
                if (GetMemContent(CHARACTER) == 3)
                    thPracParam.reisen_shield = *mReisenShield;
                break;
            case 4:
                Close();
                *mNavFocus = 0;
                break;
            default:
                break;
            }
        }

    protected:
        virtual void OnLocaleChange() override
        {
            SetTitle(S(TH_MENU));
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                SetSizeRel(0.5f, 0.81f);
                SetPosRel(0.4f, 0.14f);
                SetItemWidthRel(-0.100f);
                SetAutoSpacing(true);
                break;
            case Gui::LOCALE_EN_US:
                SetSizeRel(0.6f, 0.75f);
                SetPosRel(0.35f, 0.165f);
                SetItemWidthRel(-0.100f);
                SetAutoSpacing(true);
                break;
            case Gui::LOCALE_JA_JP:
                SetSizeRel(0.56f, 0.81f);
                SetPosRel(0.37f, 0.14f);
                SetItemWidthRel(-0.105f);
                SetAutoSpacing(true);
                break;
            default:
                break;
            }
        }
        virtual void OnContentUpdate() override
        {
            Gui::PracticeHeader();
            if (*mMode == 1) {
                const char* currentChapterLabel = Gui::LocaleGet() == Gui::LOCALE_ZH_CN
                    ? "仅记录当前章节收率"
                    : Gui::LocaleGet() == Gui::LOCALE_JA_JP
                        ? "現在のチャプターのみ取得率を記録"
                        : "Record only this chapter's capture rate";
                ImGui::Checkbox(currentChapterLabel, &g_th15OnlyCurrentPracticeChapter);
            }
            ImGui::Separator();

            PracticeMenu();
        }
        const th_glossary_t* SpellPhase()
        {
            auto section = CalcSection();
            switch (section)
            {
            default:
                break;
            case TH15_ST6_BOSS11:
                return TH_SPELL_PHASE2;
            case TH15_ST7_END_S9:
                return TH15_SPELL_PHASE_EXTRA_TIMEOUT;
            case TH15_ST7_END_S10:
                return TH15_SPELL_PHASE_EXTRA_LAST;
            case TH15_ST5_MID1:
                return TH15_ITS_LUNATIC_TIME;
            case TH15_ST3_BOSS1:
                return TH15_ST3_NORMAL1_TYPE;
            case TH15_ST8_AB_TEST:
                return TH15_AB_5PHASE;
            case 10000 + 5 * 100 + 8:
                return TH_PHASE_INF_MODE_TH15_LASERS;
            case 10000 + 5 * 100 + 10:
                return TH_PHASE_INF_MODE;
            case TH15_ST6_STARS:
                return TH_PHASE_INF_MODE;
            case TH15_ST6_BOSS4:
                return TH_SPELL_PHASE1;
            case TH15_ST6_BOSS6:
                if (mDiffculty>=2)
                    return TH15_ST6_BOSS6_PHASE;
                return nullptr;
            case TH15_ST5_BOSS4:
                return TH_TIMEOUT_SETTING;
            case 10000 + 3 * 100 + 3:
                return TH_STAGE_SKIP_HALF;
            case 10000 + 4 * 100 + 2:
                return TH_STAGE_SKIP_HALF;
            }
            return nullptr;
        }
        void PracticeMenu()
        {
            Gui::PracticeSection(false);
            mMode();
            if (mStage())
                *mSection = *mChapter = *mPhase = 0;
            if (*mMode == 1) {
                int mbs = -1;
                if (*mStage == 5) { // Counting from 0
                    mbs = 2;
                    if (*mWarp == 2)
                        *mWarp = 0;
                }
                if (mWarp(mbs))
                    *mSection = *mChapter = *mPhase = 0;
                if (*mWarp) {
                    SectionWidget();
                    mPhase(TH_PHASE, SpellPhase());
                    auto section = CalcSection();
                    if (section == TH15_ST3_BOSS1 && *mPhase>=2){
                        mDoremyNormal1Phase();
                    } else if (section == TH15_ST6_BOSS6 && *mPhase==1 && mDiffculty >= 2){
                        mEnhanced_para();
                    }
                    if (section == TH15_ST2_BOSS3) {
                        mBossHp.SetBound(0, kTh15St2Boss3EffectiveHpMax);
                        mBossHp();
                    }
                    if (section == TH15_ST6_BOSS11) {
                        mPureSignFixedSeed();
                        if (*mPureSignFixedSeed) {
                            mPureSignSeed();
                            *mPureSignSeed = std::clamp(*mPureSignSeed, 0, 65535);
                        }
                    }
                }

                Gui::PracticeSection(true);
                mLife();

                if (*mStage == 6) {
                    mLifeFragment.SetBound(0, 5);
                } else {
                    mLifeFragment.SetBound(0, 3);
                }
                mLifeFragment();
                mBomb();
                mBombFragment();
                auto power_str = std::to_string((float)(*mPower) / 100.0f).substr(0, 4);
                mPower(power_str.c_str());
                mValue();
                mValue.RoundDown(10);
                mGraze();
                mScore();
                mScore.RoundDown(10);

                if (GetMemContent(CHARACTER) == 3)
                    mReisenShield();
            }

            mNavFocus();
        }
        int CalcSection()
        {
            int chapterId = 0;
            switch (*mWarp) {
            case 1: // Chapter
                // Chapter Id = 10000 + Stage * 100 + Section
                chapterId += (*mStage + 1) * 100;
                chapterId += *mChapter;
                chapterId += 10000; // Base of chapter ID is 1000.
                return chapterId;
                break;
            case 2:
            case 3: // Mid boss & End boss
                return th_sections_cba[*mStage][*mWarp - 2][*mSection];
                break;
            case 4:
            case 5: // Non-spell & Spellcard
                return th_sections_cbt[*mStage][*mWarp - 4][*mSection];
                break;
            default:
                return 0;
                break;
            }
        }
        bool SectionHasDlg(int32_t section)
        {
            switch (section) {
            case TH15_ST1_BOSS1:
            case TH15_ST2_BOSS1:
            case TH15_ST3_BOSS1:
            case TH15_ST4_BOSS1:
            case TH15_ST5_BOSS1:
            case TH15_ST6_BOSS1:
            case TH15_ST7_END_NS1:
            case TH15_ST7_MID1:
                return true;
            default:
                return false;
            }
        }
        void SectionWidget()
        {
            static int Morbius = 0;
            const char _MorbStr[] = "MORBIUS";
            if (Morbius < 7) {
                if (Gui::KeyboardInputUpdate(_MorbStr[Morbius]))
                    Morbius++;
            } else {
                th_sections_str[::THPrac::Gui::LocaleGet()][mDiffculty][TH15_ST5_MID1] = "it's morbin time";
            }

            static char chapterStr[256] {};
            auto& chapterCounts = mChapterSetup[*mStage];

            switch (*mWarp) {
            case 1: // Chapter
                mChapter.SetBound(1, chapterCounts[0] + chapterCounts[1]);

                if (chapterCounts[1] == 0 && chapterCounts[0] != 0) {
                    sprintf_s(chapterStr, S(TH_STAGE_PORTION_N), *mChapter);
                } else if (*mChapter <= chapterCounts[0]) {
                    sprintf_s(chapterStr, S(TH_STAGE_PORTION_1), *mChapter);
                } else {
                    sprintf_s(chapterStr, S(TH_STAGE_PORTION_2), *mChapter - chapterCounts[0]);
                };

                if (Gui::LocaleGet() == Gui::LOCALE_ZH_CN) {
                    if (const char* nickname = TH15ChapterNickname(*mStage, *mChapter)) {
                        strcat_s(chapterStr, "（");
                        strcat_s(chapterStr, nickname);
                        strcat_s(chapterStr, "）");
                    }
                }

                mChapter(chapterStr);
                break;
            case 2:
            case 3: // Mid boss & End boss
                if (mSection(TH_WARP_SELECT[*mWarp],
                        th_sections_cba[*mStage][*mWarp - 2],
                        th_sections_str[::THPrac::Gui::LocaleGet()][mDiffculty]))
                    *mPhase = 0;
                if (SectionHasDlg(th_sections_cba[*mStage][*mWarp - 2][*mSection]))
                    mDlg();
                break;
            case 4:
            case 5: // Non-spell & Spellcard
                if (mSection(TH_WARP_SELECT[*mWarp],
                        th_sections_cbt[*mStage][*mWarp - 4],
                        th_sections_str[::THPrac::Gui::LocaleGet()][mDiffculty]))
                    *mPhase = 0;
                if (SectionHasDlg(th_sections_cbt[*mStage][*mWarp - 4][*mSection]))
                    mDlg();
                break;
            default:
                break;
            }
        }

        Gui::GuiCombo mMode { TH_MODE, TH_MODE_SELECT };
        Gui::GuiCombo mStage { TH_STAGE, TH_STAGE_SELECT };
        Gui::GuiCombo mWarp { TH_WARP, TH_WARP_SELECT };
        Gui::GuiCombo mSection { TH_MODE };
        Gui::GuiCombo mPhase { TH_PHASE };
        Gui::GuiCheckBox mDlg { TH_DLG };
        Gui::GuiCheckBox mPureSignFixedSeed { TH15_PURE_SIGN_FIXED_SEED };
        Gui::GuiDrag<int, ImGuiDataType_S32> mPureSignSeed {
            TH15_PURE_SIGN_SEED, 0, 65535, 1, 1000 };

        Gui::GuiSlider<float, ImGuiDataType_Float> mDoremyNormal1Phase{ TH_BT_PHASE,-3.14159265f,3.14159265f,0.01f,1.0f };
        Gui::GuiSlider<float, ImGuiDataType_Float> mEnhanced_para { TH_ENHANCEMENT_VALUE, 0.0f, 1.0f, 0.01f, 1.0f };

        Gui::GuiSlider<int, ImGuiDataType_S32> mChapter { TH_CHAPTER, 0, 0 };
        Gui::GuiDrag<int64_t, ImGuiDataType_S64> mScore { TH_SCORE, 0, 9999999990, 10, 100000000 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mLife { TH_LIFE, 0, 8 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mLifeFragment { TH_LIFE_FRAGMENT, 0, 2 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mBomb { TH_BOMB, 0, 8 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mBombFragment { TH_BOMB_FRAGMENT, 0, 4 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mPower { TH_POWER, 0, 400 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mValue { TH_VALUE, 0, 999990, 10, 100000 };
        Gui::GuiDrag<int, ImGuiDataType_S32> mGraze { TH_GRAZE, 0, 999999, 1, 100000 };

        Gui::GuiSlider<int, ImGuiDataType_S32> mReisenShield { TH15_REISEN_SHIELD, 0, 3 };
        Gui::GuiSlider<int, ImGuiDataType_S32> mBossHp {
            TH15_BOSS_HP, 0, static_cast<int>(kTh15St2Boss3EffectiveHpMax) };

        Gui::GuiNavFocus mNavFocus { TH_STAGE, TH_MODE, TH_WARP, TH_DLG,
            TH_MID_STAGE, TH_END_STAGE, TH_NONSPELL, TH_SPELL, TH_PHASE, TH_CHAPTER,
            TH_SCORE, TH_LIFE, TH_LIFE_FRAGMENT, TH_BOMB, TH_BOMB_FRAGMENT,
            TH_POWER, TH_VALUE, TH_GRAZE, TH15_REISEN_SHIELD, TH15_BOSS_HP,
            TH15_PURE_SIGN_FIXED_SEED, TH15_PURE_SIGN_SEED };

        int mChapterSetup[7][2] {
            { 2, 4 },
            { 3, 3 },
            { 3, 3 },
            { 3, 3 },
            { 4, 6 },
            { 4, 0 },
            { 5, 4 },
        };

        int mDiffculty = 0;
    };
    class THGuiRep : public Gui::GameGuiWnd {
        THGuiRep() noexcept
        {
            wchar_t appdata[MAX_PATH];
            GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
            mAppdataPath = appdata;
        }
        SINGLETON(THGuiRep)
    public:

        void CheckReplay()
        {
            mParamStatus = false;
            mRepParam.Reset();
            mReplayName.clear();
            uint32_t index = GetMemContent(0x4e9be0, 0x5a00);
            char* repName = (char*)GetMemAddr(0x4e9be0, index * 4 + 0x5a08, 0x21c);
            // The replay name field is Shift-JIS and may fill its whole buffer.
            const std::string replayNameText(repName, strnlen(repName, 0x21c));
            mReplayName = mb_to_utf16(replayNameText.c_str(), 932);
            std::wstring repDir(mAppdataPath);
            repDir.append(L"\\ShanghaiAlice\\th15\\replay\\");
            repDir.append(mReplayName);

            std::string param;
            if (ReplayLoadParam(repDir.c_str(), param) && mRepParam.ReadJson(param))
                mParamStatus = true;
        }

        bool mRepStatus = false;
        const std::wstring& ReplayName() const { return mReplayName; }
        void State(int state)
        {
            switch (state) {
            case 1:
                mRepStatus = false;
                mParamStatus = false;
                mReplayName.clear();
                thPracParam.Reset();
                TH15ChapterSave::singleton().ResetRun();
                break;
            case 2:
                CheckReplay();
                break;
            case 3:
                mRepStatus = true;
                if (mParamStatus)
                    memcpy(&thPracParam, &mRepParam, sizeof(THPracParam));
                break;
            default:
                break;
            }
        }

    protected:
        std::wstring mAppdataPath;
        std::wstring mReplayName;
        bool mParamStatus = false;
        THPracParam mRepParam;
    };
    // Player-route reference. While a replay plays, the player position is
    // sampled once per rendered frame and stored per stage; the saved path is
    // drawn as an overlay during normal play. The F10 window drives everything
    // here.
    class TH15RouteTrace {
        TH15RouteTrace() = default;
        SINGLETON(TH15RouteTrace)
    public:
        struct RouteInfo {
            std::string file;
            std::string label;
            std::string stamp;
            int stage = 0;
            int diff = 0;
            int shot = 0;
            int source = 0;
            int points = 0;
        };

        static constexpr const char* kRouteDir = "thprac_route";
        static constexpr int kMaxPoints = 150000;
        // A short initial mark can carry the previous stage's chapter index.
        // Later short marks are real chapters and must still be followed.
        static constexpr int kMinFollowSamples = 60;

        // Display settings (stored in thprac_route/settings.tsv).
        bool displayEnabled = false;
        float displayAlpha = 0.55f;
        float displayThickness = 2.5f;
        // Default track color follows the icon accent (blue-cyan replaced by green).
        ImVec4 displayColor = ImVec4(0.11f, 0.78f, 0.42f, 1.00f);
        bool displayMarkers = true;
        bool displayLabel = true;
        bool autoMatch = true;
        // Show only the stretch of route around the current stage progress
        // instead of the whole stage path. One sample is stored per rendered
        // frame, so progress and sample index share the same time base.
        bool windowEnabled = true;
        float windowBackSeconds = 2.0f;
        float windowAheadSeconds = 1.0f;
        static constexpr float kSamplesPerSecond = 60.0f;
        // Manual trim after selecting the script-derived playback origin. The
        // warp's wait is a separate clock and is not a skipped-content offset.
        int practiceSkipAdjust = 0;

        // A route stores the chapter index at the sample where each chapter
        // started; playback re-syncs on every chapter so a pause or a frame
        // rate difference cannot drift the window away from the run.
        // The game keeps its own counters at 0x4E73FC (frames in stage) and
        // 0x4E7400 (frames in chapter, cleared by the chapter manager). Using
        // them means the overlay stops exactly when the game stops, unlike a
        // counter of our own frames.
        struct ChapterMark {
            int chapter = 0;
            int frame = 0;
            int chapterFrames = 0;
            // Midboss/boss section id this mark starts (0 = ordinary chapter).
            // Section practice selects a section, so keeping it with the mark lets
            // the overlay start at that exact spell instead of the whole chapter.
            int section = 0;
        };

        // Extraction settings.
        bool autoExtractReplay = true;
        bool extractArmed = false;
        std::wstring armedReplay;

        // Saved routes and replay files.
        std::vector<RouteInfo> routes;
        int selectedRoute = -1;
        std::vector<std::wstring> replayFiles;
        int replaySelection = -1;
        std::string pendingDelete;

        // Test aid: pin the player to the reference route so a fixed pattern can
        // be checked against a fixed dodge. Session only, never saved.
        bool followPlayerToRoute = false;

        // Live capture state.
        bool capturing = false;
        std::string captureLabel;
        int captureStage = 0;
        int captureDiff = 0;
        int captureShot = 0;
        int captureSource = 0;
        size_t capturePointCount = 0;
        int captureFrame = 0;
        int captureChapter = -1;
        // Last value of the game's chapter frame counter seen while sampling.
        int captureLastCounter = -1;

        // Practice jump anchor. thprac jumps straight into the selected chapter
        // or boss section, and that jump does not always run the game's chapter
        // routine, so both the runtime chapter index and the chapter frame counter
        // can keep a stale value from before the jump. The anchor pins playback to
        // the selected mark and re-baselines the counter at that moment, so the
        // route starts at the beginning of the chapter that was jumped to.
        bool practiceAnchorValid = false;
        int practiceAnchorSample = -1;
        int practiceAnchorEnd = -1;
        int practiceBaselineCounter = 0;
        // Chapter the anchor is pinned to, and the runtime index at the moment
        // the anchor was taken. A later change of the runtime index means the
        // game's own chapter routine has started working (the jump skipped it),
        // and playback follows it from then on.
        int practiceAnchorChapter = -1;
        int practiceAnchorRuntimeChapter = -1;
        // True once playback moved to a chapter the game entered by itself; the
        // warp intro compensation no longer applies from that point on.
        bool practiceAnchorFollowed = false;
        bool practiceAwaitRuntimeChapter = false;
        int practiceAnchorRecoverWait = 0;
        // Playback start actually used last frame, and the last chapter counter
        // seen while anchored. A chapter switch compares the two to measure how
        // far the anchor still is from the game's own timing.
        int practicePlaybackStart = -1;
        int practiceLastCounter = 0;
        int practiceMeasuredResidual = 0;
        bool practiceMeasureValid = false;
        // Frames of the current anchor that actually ran with the route-following
        // test on. The chapter-switch measurement is only meaningful when the run
        // followed the reference path the whole way.
        int practiceAutopilotFrames = 0;

    private:
        bool initialized = false;
        bool lastReplayActive = false;
        int lastPracticeSection = 0;
        int lastStageFrames = 0;
        unsigned lastWarpGeneration = 0;
        bool practiceAnchorPending = false;
        int practiceAnchorWait = 0;
        // Diagnostic throttles (see TraceLog).
        int debugTick = 0;
        int debugLastChapter = -1;
        std::vector<ImVec2> capturePoints;
        std::vector<uint8_t> captureBreaks;
        std::vector<ChapterMark> captureMarks;
        std::vector<ImVec2> loadedPoints;
        std::vector<uint8_t> loadedBreaks;
        std::vector<ChapterMark> loadedMarks;
        std::vector<ImVec2> segmentBuffer;
        std::string loadedFile;
        std::string settingsSelection;
        bool settingsSelectionPending = false;
        static bool ValidStage(int stage) { return stage >= 0 && stage < 7; }

        // Append one line to thprac_route\practice.log. The practice anchor
        // depends on live game state, so the decision has to be observable
        // without a debugger. Capped so it can never grow without bound.
        static void TraceLog(const std::string& text)
        {
            wchar_t appData[MAX_PATH] = {};
            if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH))
                return;
            const std::wstring path = std::wstring(appData)
                + L"\\ShanghaiAlice\\th15\\thprac_route\\practice.log";
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)
                && attributes.nFileSizeLow > 128 * 1024)
                DeleteFileW(path.c_str());
            std::ofstream file(path.c_str(), std::ios::out | std::ios::app);
            if (!file.is_open())
                return;
            SYSTEMTIME now{};
            GetLocalTime(&now);
            file << std::format("{:02}:{:02}:{:02}.{:03} {}\n", now.wHour, now.wMinute,
                now.wSecond, now.wMilliseconds, text);
        }

        static int ReadStageFrames() { return GetMemContent<int32_t>(0x4E73FC); }
        static int ReadChapterFrames() { return GetMemContent<int32_t>(0x4E7400); }

        // Section id of the attempt being tracked (0 for an ordinary chapter).
        static int CurrentRouteSection()
        {
            int logical = -1;
            int section = 0;
            TH15RouteAnchor(logical, section);
            return section;
        }

        static bool WindowAround(int progress, float backSeconds, float aheadSeconds,
            size_t pointCount, size_t& begin, size_t& end)
        {
            begin = 0;
            end = pointCount;
            if (pointCount < 2)
                return false;
            const auto back = static_cast<int64_t>(
                std::lround(std::max(0.0f, backSeconds) * kSamplesPerSecond));
            const auto ahead = static_cast<int64_t>(
                std::lround(std::max(0.0f, aheadSeconds) * kSamplesPerSecond));
            const int64_t first = std::max<int64_t>(0, static_cast<int64_t>(progress) - back);
            const int64_t last = std::min<int64_t>(static_cast<int64_t>(pointCount) - 1,
                static_cast<int64_t>(progress) + ahead);
            if (last < first)
                return false;
            begin = static_cast<size_t>(first);
            end = static_cast<size_t>(last + 1);
            return end > begin;
        }

        // Preserve the exact playback sample separately from the clipped trail.
        // Adding the trail length back to a clipped begin advances the marker
        // (and the route-following test) at the start of every chapter.
        static bool PlaybackWindow(int progress, int chapterStart, int chapterEnd,
            float backSeconds, float aheadSeconds, size_t pointCount,
            size_t& begin, size_t& end, size_t& current)
        {
            if (chapterStart < 0 || chapterEnd <= chapterStart
                || static_cast<size_t>(chapterEnd) > pointCount)
                return false;
            current = static_cast<size_t>(std::clamp(progress, chapterStart, chapterEnd - 1));
            if (!WindowAround(static_cast<int>(current), backSeconds, aheadSeconds,
                    pointCount, begin, end))
                return false;
            begin = std::max(begin, static_cast<size_t>(chapterStart));
            end = std::min(end, static_cast<size_t>(chapterEnd));
            return end > begin;
        }

        // Playback position inside the anchored stretch. The counter is
        // re-baselined when the anchor is taken, so a value left over from before
        // the practice jump cannot push the window to the end of the chapter.
        // Only some practice targets run ECLSetChapter; the rest keep the counter
        // they had, and a target that does run it clears the counter right after
        // the jump. Following any drop covers both cases: the game's counter is
        // the only clock that matches the route once the chapter starts.
        static int ProgressOf(int anchorSample, int anchorEnd, int baseline, int counter)
        {
            const int length = std::max(0, anchorEnd - anchorSample);
            const int elapsed = std::clamp(counter - baseline, 0, std::max(0, length - 1));
            return anchorSample + elapsed;
        }

        static int PracticeProgress(int anchorSample, int anchorEnd, int& baseline, int counter)
        {
            if (counter < baseline)
                baseline = counter;
            return ProgressOf(anchorSample, anchorEnd, baseline, counter);
        }

    public:
        // Generated clocks cover all stage-practice entries and their phases.
        // Mode 0: original chapter start; 1: fixed tail to a later chapter;
        // 2: fixed prefix before an external event; 3: a repeating script block.
        static int PracticeTailFrames(int section, int phase, int warpWait)
        {
            const auto* clock = FindTH15RouteClock(section, phase, GetMemContent<int32_t>(0x4E7410));
            return clock && clock->mode == 1 ? clock->frames + warpWait - clock->warpWait : -1;
        }

        bool PracticeScriptRange(int section, size_t& begin, size_t& end) const
        {
            const auto* clock = FindTH15RouteClock(section, thPracParam.phase, GetMemContent<int32_t>(0x4E7410));
            if (!clock || loadedPoints.size() < 2)
                return false;
            for (size_t i = 0; i < loadedMarks.size(); ++i) {
                const auto& mark = loadedMarks[i];
                if (mark.chapter != clock->entryChapter || mark.frame < 0
                    || static_cast<size_t>(mark.frame) >= loadedPoints.size())
                    continue;
                size_t next = i + 1;
                if (clock->mode == 1) {
                    while (next < loadedMarks.size() && loadedMarks[next].chapter != clock->syncChapter)
                        ++next;
                    if (next == loadedMarks.size())
                        return false;
                }
                begin = static_cast<size_t>(mark.frame);
                end = next < loadedMarks.size() ? static_cast<size_t>(loadedMarks[next].frame) : loadedPoints.size();
                if (end > loadedPoints.size() || end <= begin + 1)
                    return false;
                if (clock->mode == 3 && (clock->loopBegin < 0 || clock->loopEnd <= clock->loopBegin
                    || begin + static_cast<size_t>(clock->loopEnd) > end))
                    return false;
                return true;
            }
            return false;
        }

        bool PracticeScriptStart(int section, int& start) const
        {
            if (!practiceAnchorValid || practiceAnchorFollowed)
                return false;
            size_t begin = 0, end = 0;
            if (!PracticeScriptRange(section, begin, end)
                || begin != static_cast<size_t>(practiceAnchorSample)
                || end != static_cast<size_t>(practiceAnchorEnd))
                return false;
            const auto& clock = *FindTH15RouteClock(section, thPracParam.phase, GetMemContent<int32_t>(0x4E7410));
            if (clock.mode == 1)
                start = static_cast<int>(end) - (clock.frames + g_th15WarpWaitFrames - clock.warpWait);
            else if (clock.mode == 0)
                start = static_cast<int>(begin);
            else
                start = static_cast<int>(begin) + clock.frames - (g_th15WarpWaitFrames - clock.warpWait);
            return true;
        }

        int PracticeStartSample(int section) const
        {
            if (!practiceAnchorValid)
                return -1;
            if (practiceAnchorFollowed)
                return practiceAnchorSample;
            int start = practiceAnchorSample;
            PracticeScriptStart(section, start);
            // Unknown targets use their chapter mark as an approximation. The
            // ECL wait alone cannot supply a content offset for them.
            return std::min(start + practiceSkipAdjust, practiceAnchorEnd - 1);
        }

        int PracticeSample(int section, int counter, int& lower, int& upper)
        {
            const int start = PracticeStartSample(section);
            if (counter < practiceBaselineCounter)
                practiceBaselineCounter = counter;
            int64_t progress = static_cast<int64_t>(start)
                + std::max<int64_t>(0, static_cast<int64_t>(counter) - practiceBaselineCounter);
            lower = std::max(start, practiceAnchorSample);
            upper = practiceAnchorEnd;
            const auto* clock = FindTH15RouteClock(section, thPracParam.phase, GetMemContent<int32_t>(0x4E7410));
            int scriptStart = 0;
            if (clock && clock->mode == 3 && PracticeScriptStart(section, scriptStart)) {
                const int loopBegin = practiceAnchorSample + clock->loopBegin;
                const int loopEnd = practiceAnchorSample + clock->loopEnd;
                if (progress >= loopBegin) {
                    progress = loopBegin + (progress - loopBegin) % (loopEnd - loopBegin);
                    lower = loopBegin;
                    upper = loopEnd;
                }
            }
            return static_cast<int>(std::clamp<int64_t>(progress, lower, upper - 1));
        }

    private:
        int CurrentChapter() const { return GetMemContent<int32_t>(0x4E73F8); }

        // Map the current chapter + frames elapsed inside it to a sample index.
        // Returns false when the route has no mark for this chapter, in which
        // case the caller falls back to drawing the whole route.
        // Each chapter is its own playback unit: the window is anchored at the
        // sample where that chapter started and never crosses into the next one.
        // Chapters can be cleared at very different speeds, so a stage-wide
        // timeline would put the reference in the wrong chapter entirely.
        static bool ComputeWindow(const std::vector<ChapterMark>& marks, int chapter,
            int chapterFrames, float backSeconds, float aheadSeconds, size_t pointCount,
            size_t& begin, size_t& end, size_t* currentSample = nullptr)
        {
            begin = 0;
            end = pointCount;
            if (marks.empty() || pointCount == 0)
                return false;
            const auto mark = std::find_if(marks.begin(), marks.end(),
                [chapter](const ChapterMark& entry) { return entry.chapter == chapter; });
            if (mark == marks.end())
                return false;
            const auto position = static_cast<size_t>(mark - marks.begin());
            const int total = static_cast<int>(pointCount);
            const int chapterStart = std::clamp(mark->frame, 0, total - 1);
            const int chapterEnd = position + 1 < marks.size()
                ? std::clamp(marks[position + 1].frame, chapterStart + 1, total)
                : total;
            // Slower than the reference: hold at the end of this chapter instead
            // of sliding into the next chapter's samples.
            const int elapsed = std::clamp(chapterFrames - mark->chapterFrames,
                0, std::max(0, chapterEnd - chapterStart - 1));
            const int progress = chapterStart + elapsed;
            size_t current = 0;
            const bool valid = PlaybackWindow(progress, chapterStart, chapterEnd,
                backSeconds, aheadSeconds, pointCount, begin, end, current);
            if (valid && currentSample)
                *currentSample = current;
            return valid;
        }

        static std::vector<std::string> SplitTabs(const std::string& line)
        {
            std::vector<std::string> cells;
            std::stringstream row(line);
            std::string cell;
            while (std::getline(row, cell, '\t'))
                cells.push_back(cell);
            return cells;
        }

        static std::string SanitizeLabel(const std::string& label)
        {
            std::string text = label;
            for (auto& ch : text) {
                if (ch == '\t' || ch == '\n' || ch == '\r')
                    ch = ' ';
            }
            return text;
        }

        static bool ParseRouteHeader(const std::string& path, RouteInfo& info)
        {
            std::ifstream file(path);
            if (!file.is_open())
                return false;
            std::string line;
            if (!std::getline(file, line) || line.rfind("#thprac-route", 0) != 0)
                return false;
            while (std::getline(file, line)) {
                if (line == "data")
                    return ValidStage(info.stage);
                const auto cells = SplitTabs(line);
                if (cells.size() < 2)
                    continue;
                try {
                    if (cells[0] == "label") info.label = cells[1];
                    else if (cells[0] == "stage") info.stage = std::stoi(cells[1]);
                    else if (cells[0] == "diff") info.diff = std::stoi(cells[1]);
                    else if (cells[0] == "shot") info.shot = std::stoi(cells[1]);
                    else if (cells[0] == "source") info.source = std::stoi(cells[1]);
                    else if (cells[0] == "points") info.points = std::stoi(cells[1]);
                    else if (cells[0] == "stamp") info.stamp = cells[1];
                } catch (...) {
                    return false;
                }
            }
            return false;
        }

        // pV2 stores one sample per rendered frame plus the chapter index that
        // each chapter starts at, so the display window can follow the stage
        // timeline. V1 files (two columns, "break" lines) still load, they just
        // have no chapter marks and therefore always show the whole route.
        static bool LoadRoute(const std::string& path, std::vector<ImVec2>& points,
            std::vector<uint8_t>& breaks, std::vector<ChapterMark>& marks)
        {
            std::ifstream file(path);
            if (!file.is_open())
                return false;
            points.clear();
            breaks.clear();
            marks.clear();
            points.reserve(4096);
            breaks.reserve(4096);
            std::string line;
            bool inData = false;
            bool pendingBreak = false;
            while (std::getline(file, line)) {
                if (!inData) {
                    if (line == "data") {
                        inData = true;
                        continue;
                    }
                    const auto cells = SplitTabs(line);
                    if (cells.size() >= 3 && cells[0] == "chapter") {
                        try {
                            // Files written before the game counter was stored
                            // only carry (chapter, sample); treat the mark as
                            // starting at counter 0, which is close enough.
                            const int counter = cells.size() >= 4 ? std::stoi(cells[3]) : 0;
                            // Section ids were added later still; routes saved
                            // before that only match by chapter.
                            const int section = cells.size() >= 5 ? std::stoi(cells[4]) : 0;
                            marks.push_back({ std::stoi(cells[1]), std::stoi(cells[2]), counter, section });
                        } catch (...) {
                        }
                    }
                    continue;
                }
                if (line.empty())
                    continue;
                if (line[0] == 'b') {
                    // V1 break marker: the next point starts a new polyline.
                    pendingBreak = true;
                    continue;
                }
                const auto cells = SplitTabs(line);
                if (cells.size() < 2)
                    continue;
                try {
                    const float x = std::stof(cells[0]);
                    const float y = std::stof(cells[1]);
                    if (!std::isfinite(x) || !std::isfinite(y))
                        continue;
                    const bool breakBefore = cells.size() >= 3 && std::stoi(cells[2]) != 0;
                    points.emplace_back(x, y);
                    breaks.push_back((breakBefore || pendingBreak) ? 1 : 0);
                    pendingBreak = false;
                } catch (...) {
                    continue;
                }
            }
            return !points.empty();
        }

        void BeginCapture(int stage, int diff, int shot, int source)
        {
            if (!ValidStage(stage))
                return;
            captureStage = stage;
            captureDiff = std::clamp(diff, 0, 4);
            captureShot = std::clamp(shot, 0, 3);
            captureSource = source;
            capturePoints.clear();
            captureBreaks.clear();
            captureMarks.clear();
            capturePoints.reserve(4096);
            captureBreaks.reserve(4096);
            capturePointCount = 0;
            captureFrame = 0;
            captureLastCounter = -1;
            captureChapter = CurrentChapter();
            captureMarks.push_back({ captureChapter, 0, ReadChapterFrames(), CurrentRouteSection() });
            capturing = true;
        }

        bool SaveCapture()
        {
            if (capturePoints.size() < 20)
                return false;
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            CreateDirectoryA(kRouteDir, nullptr);

            SYSTEMTIME now{};
            GetLocalTime(&now);
            const std::string stamp = std::format("{:04}{:02}{:02}-{:02}{:02}{:02}",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
            const std::string directory = std::string(kRouteDir) + "\\";
            const std::string stem = std::format("route_s{}_d{}_p{}_{}",
                captureStage + 1, captureDiff, captureShot, stamp);
            std::string path = directory + stem + ".tsv";
            for (int attempt = 2; attempt < 100
                && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++attempt) {
                path = std::format("{}{}_{}.tsv", directory, stem, attempt);
            }
            const std::string temporary = path + ".tmp";
            bool written = false;
            {
                std::ofstream file(temporary, std::ios::out | std::ios::trunc);
                if (!file.is_open()) {
                    PopCurrentDirectory();
                    return false;
                }
                file << "#thprac-route-v3\n";
                file << "label\t" << SanitizeLabel(captureLabel) << '\n';
                file << "stage\t" << captureStage << '\n';
                file << "diff\t" << captureDiff << '\n';
                file << "shot\t" << captureShot << '\n';
                file << "source\t" << captureSource << '\n';
                file << "points\t" << capturePoints.size() << '\n';
                file << "frames\t" << captureFrame << '\n';
                file << "stamp\t" << stamp << '\n';
                for (const auto& mark : captureMarks)
                    file << "chapter\t" << mark.chapter << '\t' << mark.frame
                         << '\t' << mark.chapterFrames << '\t' << mark.section << '\n';
                file << "data\n";
                for (size_t i = 0; i < capturePoints.size(); ++i) {
                    const int breakBefore = i < captureBreaks.size() ? captureBreaks[i] : 0;
                    file << std::format("{:.2f}\t{:.2f}\t{}\n",
                        capturePoints[i].x, capturePoints[i].y, breakBefore);
                }
                file.flush();
                written = file.good();
            }
            if (!written) {
                DeleteFileA(temporary.c_str());
                PopCurrentDirectory();
                return false;
            }
            const bool moved = MoveFileExA(temporary.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            if (!moved)
                DeleteFileA(temporary.c_str());
            PopCurrentDirectory();
            return moved;
        }

        bool ReplayNameMatches(const std::wstring& name) const
        {
            const auto& playing = THGuiRep::singleton().ReplayName();
            if (name.empty() || playing.empty())
                return false;
            return _wcsicmp(name.c_str(), playing.c_str()) == 0;
        }

        void EnsureLoaded(const std::string& file)
        {
            if (loadedFile == file)
                return;
            loadedFile = file;
            // Sample indices belong to this file. A newly selected route must
            // never inherit an anchor resolved against another recording.
            practiceAnchorValid = false;
            practiceAnchorPending = thPracParam.mode == 1 && thPracParam.section != 0;
            practiceAnchorWait = 0;
            practiceAwaitRuntimeChapter = false;
            loadedPoints.clear();
            loadedBreaks.clear();
            loadedMarks.clear();
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            LoadRoute(std::string(kRouteDir) + "\\" + file, loadedPoints, loadedBreaks, loadedMarks);
            PopCurrentDirectory();
            TraceLog(std::format("route file={} points={} marks={}",
                loadedFile, loadedPoints.size(), loadedMarks.size()));
        }

        const RouteInfo* ResolveTarget(int stage, int diff, int shot)
        {
            const RouteInfo* selected = nullptr;
            if (selectedRoute >= 0 && selectedRoute < (int)routes.size())
                selected = &routes[selectedRoute];
            if (!autoMatch)
                return selected;
            if (selected && selected->stage == stage && selected->diff == diff
                && selected->shot == shot)
                return selected;
            // routes are sorted newest first, so the first hit is the latest run.
            for (const auto& route : routes) {
                if (route.stage == stage && route.diff == diff && route.shot == shot)
                    return &route;
            }
            return nullptr;
        }

    public:
        void Init()
        {
            if (initialized)
                return;
            initialized = true;
            // One diagnostic log per launch: the practice anchor depends on game
            // state that a static check cannot reproduce, so record the decision.
            wchar_t appData[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) {
                DeleteFileW((std::wstring(appData)
                    + L"\\ShanghaiAlice\\th15\\thprac_route\\practice.log").c_str());
            }
            LoadSettings();
            ScanRoutes();
            ScanReplays();
        }

        void Refresh()
        {
            Init();
            ScanRoutes();
            ScanReplays();
        }

        void ScanRoutes()
        {
            const std::string keep = (selectedRoute >= 0 && selectedRoute < (int)routes.size())
                ? routes[selectedRoute].file
                : std::string();
            routes.clear();
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            CreateDirectoryA(kRouteDir, nullptr);
            const std::string pattern = std::string(kRouteDir) + "\\route_*.tsv";
            WIN32_FIND_DATAA data{};
            HANDLE find = FindFirstFileA(pattern.c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;
                    RouteInfo info;
                    if (!ParseRouteHeader(std::string(kRouteDir) + "\\" + data.cFileName, info))
                        continue;
                    info.file = data.cFileName;
                    routes.push_back(std::move(info));
                } while (FindNextFileA(find, &data));
                FindClose(find);
            }
            PopCurrentDirectory();
            std::sort(routes.begin(), routes.end(), [](const RouteInfo& left, const RouteInfo& right) {
                return left.stamp > right.stamp;
            });
            selectedRoute = -1;
            std::string wanted;
            if (!keep.empty())
                wanted = keep;
            else if (settingsSelectionPending)
                wanted = settingsSelection;
            settingsSelectionPending = false;
            if (!wanted.empty()) {
                for (size_t i = 0; i < routes.size(); ++i) {
                    if (routes[i].file == wanted) {
                        selectedRoute = (int)i;
                        break;
                    }
                }
            }
            if (selectedRoute < 0 && !routes.empty() && !autoMatch)
                selectedRoute = 0;
        }

        void ScanReplays()
        {
            replayFiles.clear();
            wchar_t appData[MAX_PATH] = {};
            if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH))
                return;
            std::wstring pattern(appData);
            pattern += L"\\ShanghaiAlice\\th15\\replay\\*.rpy";
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW(pattern.c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        continue;
                    replayFiles.emplace_back(data.cFileName);
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
            std::sort(replayFiles.begin(), replayFiles.end());
            if (replaySelection >= (int)replayFiles.size())
                replaySelection = (int)replayFiles.size() - 1;
        }

        void LoadSettings()
        {
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            std::ifstream file(std::string(kRouteDir) + "\\settings.tsv");
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#')
                    continue;
                const auto cells = SplitTabs(line);
                if (cells.size() < 2)
                    continue;
                try {
                    if (cells[0] == "enabled") displayEnabled = std::stoi(cells[1]) != 0;
                    else if (cells[0] == "alpha") displayAlpha = std::clamp(std::stof(cells[1]), 0.05f, 1.0f);
                    else if (cells[0] == "thickness") displayThickness = std::clamp(std::stof(cells[1]), 0.5f, 6.0f);
                    else if (cells[0] == "color" && cells.size() >= 5)
                        displayColor = ImVec4(std::stof(cells[1]), std::stof(cells[2]),
                            std::stof(cells[3]), std::stof(cells[4]));
                    else if (cells[0] == "markers") displayMarkers = std::stoi(cells[1]) != 0;
                    else if (cells[0] == "label") displayLabel = std::stoi(cells[1]) != 0;
                    else if (cells[0] == "automatch") autoMatch = std::stoi(cells[1]) != 0;
                    else if (cells[0] == "window") windowEnabled = std::stoi(cells[1]) != 0;
                    else if (cells[0] == "window_back") windowBackSeconds = std::clamp(std::stof(cells[1]), 0.0f, 15.0f);
                    else if (cells[0] == "window_ahead") windowAheadSeconds = std::clamp(std::stof(cells[1]), 0.0f, 15.0f);
                    else if (cells[0] == "practice_skip") practiceSkipAdjust = std::clamp(std::stoi(cells[1]), -300, 600);
                    // Old automatically learned offsets used chapter clear time,
                    // which also changes with kills and player actions. Ignore
                    // those values; retain the user's explicit manual adjustment.
                    else if (cells[0] == "autoextract") autoExtractReplay = std::stoi(cells[1]) != 0;
                    else if (cells[0] == "selected") settingsSelection = cells[1];
                } catch (...) {
                    continue;
                }
            }
            PopCurrentDirectory();
            settingsSelectionPending = !settingsSelection.empty();
        }

        void SaveSettings()
        {
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            CreateDirectoryA(kRouteDir, nullptr);
            std::ofstream file(std::string(kRouteDir) + "\\settings.tsv", std::ios::out | std::ios::trunc);
            if (file.is_open()) {
                file << "#thprac-route-settings-v1\n";
                file << "enabled\t" << (displayEnabled ? 1 : 0) << '\n';
                file << std::format("alpha\t{:.3f}\n", displayAlpha);
                file << std::format("thickness\t{:.3f}\n", displayThickness);
                file << std::format("color\t{:.4f}\t{:.4f}\t{:.4f}\t{:.4f}\n",
                    displayColor.x, displayColor.y, displayColor.z, displayColor.w);
                file << "markers\t" << (displayMarkers ? 1 : 0) << '\n';
                file << "label\t" << (displayLabel ? 1 : 0) << '\n';
                file << "automatch\t" << (autoMatch ? 1 : 0) << '\n';
                file << "window\t" << (windowEnabled ? 1 : 0) << '\n';
                file << std::format("window_back\t{:.2f}\n", windowBackSeconds);
                file << std::format("window_ahead\t{:.2f}\n", windowAheadSeconds);
                file << "practice_skip\t" << practiceSkipAdjust << '\n';
                file << "autoextract\t" << (autoExtractReplay ? 1 : 0) << '\n';
                const std::string selected = (selectedRoute >= 0 && selectedRoute < (int)routes.size())
                    ? routes[selectedRoute].file
                    : std::string();
                file << "selected\t" << selected << '\n';
            }
            PopCurrentDirectory();
        }

        void ArmReplay(const std::wstring& name)
        {
            armedReplay = name;
            extractArmed = !name.empty();
        }

        void DeleteRoute(const std::string& file)
        {
            PushCurrentDirectory(L"%appdata%\\ShanghaiAlice\\th15");
            DeleteFileA((std::string(kRouteDir) + "\\" + file).c_str());
            PopCurrentDirectory();
            if (loadedFile == file) {
                loadedFile.clear();
                loadedPoints.clear();
                loadedBreaks.clear();
                loadedMarks.clear();
            }
            if (selectedRoute >= 0 && selectedRoute < (int)routes.size()
                && routes[selectedRoute].file == file) {
                selectedRoute = -1;
                settingsSelection.clear();
            }
            ScanRoutes();
        }

        void FinishCapture(bool save)
        {
            if (!capturing)
                return;
            capturing = false;
            capturePointCount = 0;
            const bool saved = save && SaveCapture();
            capturePoints.clear();
            captureBreaks.clear();
            captureMarks.clear();
            if (saved)
                ScanRoutes();
        }

        void OnFrame()
        {
            Init();
            UpdatePracticeAnchor();
            const bool replayActive = THGuiRep::singleton().mRepStatus;
            if (replayActive) {
                if (!lastReplayActive)
                    TryStartCapture();
                if (capturing)
                    SamplePlayer();
            } else if (lastReplayActive) {
                FinishCapture(true);
                extractArmed = false;
            }
            lastReplayActive = replayActive;
        }

        void Draw(ImDrawList* drawList)
        {
            if (!drawList)
                return;
            Init();
            if (!displayEnabled)
                return;
            const uintptr_t player = GetMemContent(PLAYER_PTR);
            if (!player)
                return;
            const int stage = GetMemContent<int32_t>(0x4E73F0) - 1;
            if (!ValidStage(stage))
                return;
            const int diff = std::clamp(GetMemContent<int32_t>(0x4E7410), 0, 4);
            const int shot = std::clamp(GetMemContent<int32_t>(0x4E7404), 0, 3);
            const RouteInfo* target = ResolveTarget(stage, diff, shot);
            if (!target)
                return;
            EnsureLoaded(target->file);
            if (loadedPoints.size() < 2)
                return;

            const int chapterNow = CurrentChapter();
            const int chapterFramesNow = ReadChapterFrames();
            const int stageFramesNow = ReadStageFrames();

            const int practiceSection = thPracParam.mode == 1 ? thPracParam.section : 0;
            const int anchorStart = PracticeStartSample(practiceSection);
            if (practiceAnchorValid)
                practicePlaybackStart = anchorStart;
            // Diagnostic: what the overlay is being asked to draw, throttled to
            // roughly two lines per second (and on every chapter change). The
            // The player starts at the practice spawn, so coordinate differences
            // cannot establish the route's timing. Log the clock origin instead.
            if (practiceSection != 0
                && (++debugTick % 30 == 0 || chapterNow != debugLastChapter)) {
                debugLastChapter = chapterNow;
                int logLower = 0, logUpper = 0;
                const int logProgress = practiceAnchorValid
                    ? PracticeSample(practiceSection, chapterFramesNow, logLower, logUpper)
                    : -1;
                std::string routePoint = "-";
                if (logProgress >= 0 && static_cast<size_t>(logProgress) < loadedPoints.size()) {
                    const ImVec2& point = loadedPoints[logProgress];
                    routePoint = std::format("{:.1f},{:.1f}", point.x, point.y);
                }
                TraceLog(std::format(
                    "tick section={} phase={} stage={} chapter={} counter={} stageFrames={} anchor={} "
                    "sample={} end={} baseline={} start={} tail={} progress={} player={:.1f},{:.1f} route={}",
                    practiceSection, thPracParam.phase, stage, chapterNow, chapterFramesNow, stageFramesNow,
                    practiceAnchorValid ? 1 : 0, practiceAnchorSample, practiceAnchorEnd,
                    practiceBaselineCounter, anchorStart,
                    PracticeTailFrames(practiceSection, thPracParam.phase, g_th15WarpWaitFrames), logProgress,
                    GetMemContent<float>(player + 0x618), GetMemContent<float>(player + 0x61C),
                    routePoint));
            }

            // The window follows stage progress (chapter start + frames inside
            // that chapter), not the player position.
            size_t rangeBegin = 0;
            size_t rangeEnd = loadedPoints.size();
            size_t currentSample = 0;
            bool windowed = false;
            if (windowEnabled && practiceSection != 0 && !practiceAnchorValid) {
                // After a practice jump the runtime chapter index is not the one
                // that was jumped to - that is exactly why the anchor exists - so
                // chapter tracking would draw an unrelated stretch, in practice
                // the first chapter of the stage. Draw nothing until the anchor
                // resolves, which for a chapter jump needs no game state at all.
                return;
            }
            if (windowEnabled && practiceAnchorValid) {
                // Practice jump: playback is pinned to the mark that was jumped
                // to and runs from the moment the anchor was taken, so the route
                // always starts at the beginning of the selected chapter or
                // section instead of wherever the game's counters happen to be.
                int lower = 0, upper = 0;
                const int progress = PracticeSample(practiceSection, chapterFramesNow, lower, upper);
                windowed = PlaybackWindow(progress, lower, upper,
                    windowBackSeconds, windowAheadSeconds, loadedPoints.size(),
                    rangeBegin, rangeEnd, currentSample);
                if (!windowed)
                    return;
            } else if (windowEnabled) {
                if (!loadedMarks.empty()) {
                    // The route knows this chapter only if it was recorded there.
                    // Showing another chapter's stretch would be misleading, so
                    // nothing is drawn instead.
                    if (!ComputeWindow(loadedMarks, chapterNow, chapterFramesNow,
                            windowBackSeconds, windowAheadSeconds, loadedPoints.size(),
                            rangeBegin, rangeEnd, &currentSample)) {
                        return;
                    }
                    windowed = true;
                } else if (stageFramesNow > 0) {
                    // Routes captured before chapter marks existed: samples were
                    // recorded once per frame, so the stage frame counter is a
                    // close approximation of the position in those files.
                    windowed = PlaybackWindow(stageFramesNow, 0, static_cast<int>(loadedPoints.size()),
                        windowBackSeconds, windowAheadSeconds, loadedPoints.size(),
                        rangeBegin, rangeEnd, currentSample);
                }
                if (!windowed) {
                    rangeBegin = 0;
                    rangeEnd = loadedPoints.size();
                }
            }

            // Sample the overlay is currently at: the marker is drawn there, and
            // the test switch below moves the player onto it.
            const bool hasCurrent = windowed && rangeEnd > rangeBegin;
            // Test aid: pin the player onto the reference route. The position the
            // game actually moves and collides with is the 1/128-pixel pair at
            // +0x624/+0x628; +0x618/+0x61c is only its float mirror, which the
            // game recomputes every frame (writing just that moved the sprite
            // while the hitbox and options stayed behind). Mirror the game's own
            // update: fixed point, float copy, then flag all eight parts (player
            // plus seven options) dirty so they re-sync to the new position.
            if (followPlayerToRoute && hasCurrent) {
                const ImVec2& point = loadedPoints[currentSample];
                const int fixedX = static_cast<int>(point.x * 128.0f);
                const int fixedY = static_cast<int>(point.y * 128.0f);
                *(int*)(player + 0x624) = fixedX;
                *(int*)(player + 0x628) = fixedY;
                *(float*)(player + 0x618) = static_cast<float>(fixedX) / 128.0f;
                *(float*)(player + 0x61C) = static_cast<float>(fixedY) / 128.0f;
                for (int part = 0; part < 8; ++part)
                    *(int*)(player + 0x6c8 + part * 0xE4 + 0x74) = 1;
            }

            ImGuiIO& io = ImGui::GetIO();
            const float xRatio = io.DisplaySize.x / 1280.0f;
            const float yRatio = io.DisplaySize.y / 960.0f;
            // Same stage-space mapping as the bullet hitbox overlay.
            auto toClient = [&](const ImVec2& point) {
                return ImVec2{ (point.x * 2.0f + 448.0f) * xRatio, (point.y * 2.0f + 32.0f) * yRatio };
            };

            drawList->PushClipRect(
                { 64.0f * xRatio, 32.0f * yRatio },
                { 832.0f * xRatio, 928.0f * yRatio });
            const ImU32 color = ImGui::ColorConvertFloat4ToU32(
                ImVec4(displayColor.x, displayColor.y, displayColor.z, displayAlpha));
            const float thickness = std::max(0.5f, displayThickness * xRatio);

            auto isBreakBefore = [&](size_t index) {
                return index < loadedBreaks.size() && loadedBreaks[index] != 0;
            };

            ImVec2 startPoint{}, endPoint{};
            bool hasStart = false;
            // A stage route can hold tens of thousands of samples. Feeding all
            // of them to AddPolyline every frame would rebuild the vertices with
            // antialiasing per segment, so thin the outline for display only.
            const size_t visibleCount = rangeEnd - rangeBegin;
            const size_t drawStride = std::max<size_t>(1, visibleCount / 4000);
            size_t index = rangeBegin;
            while (index < rangeEnd) {
                const size_t begin = index;
                size_t end = index + 1;
                while (end < rangeEnd && !isBreakBefore(end))
                    ++end;
                const size_t count = end - begin;
                if (count >= 2) {
                    segmentBuffer.clear();
                    segmentBuffer.reserve(count / drawStride + 2);
                    for (size_t i = begin; i < end; i += drawStride)
                        segmentBuffer.push_back(toClient(loadedPoints[i]));
                    const ImVec2 lastPoint = toClient(loadedPoints[end - 1]);
                    if (segmentBuffer.back().x != lastPoint.x || segmentBuffer.back().y != lastPoint.y)
                        segmentBuffer.push_back(lastPoint);
                    drawList->AddPolyline(segmentBuffer.data(), (int)segmentBuffer.size(),
                        color, 0, thickness);
                    if (!hasStart) {
                        startPoint = segmentBuffer.front();
                        hasStart = true;
                    }
                    endPoint = segmentBuffer.back();
                }
                index = end;
            }

            if (displayMarkers && (hasCurrent || hasStart)) {
                const float radius = std::max(3.0f, 3.0f * xRatio);
                if (windowed) {
                    // The window slides with progress; mark the sample matching
                    // the current frame instead of misleading start/end dots.
                    drawList->AddCircle(toClient(loadedPoints[currentSample]),
                        radius + 1.0f, IM_COL32(255, 255, 255, 200), 12, 1.5f);
                } else {
                    drawList->AddCircleFilled(startPoint, radius, IM_COL32(90, 255, 130, 235), 12);
                    drawList->AddCircleFilled(endPoint, radius, IM_COL32(255, 90, 90, 235), 12);
                }
            }
            drawList->PopClipRect();

            if (displayLabel) {
                std::string text = std::format("S{} {}", stage + 1, target->label);
                if (text.size() > 64)
                    text.resize(64);
                const ImVec2 size = ImGui::CalcTextSize(text.c_str());
                // Keep the label in the border under the playfield so it never
                // covers the player or the bullets.
                const ImVec2 position{ 68.0f * xRatio, (932.0f - size.y * 0.5f) * yRatio };
                drawList->AddRectFilled({ position.x - 2.0f, position.y - 1.0f },
                    { position.x + size.x + 2.0f, position.y + size.y + 1.0f },
                    IM_COL32(0, 0, 0, 130));
                drawList->AddText(position, IM_COL32(230, 255, 255, 230), text.c_str());
            }
        }

    private:
        void SamplePlayer();
        void TryStartCapture();
        void UpdatePracticeAnchor();
        void FollowRuntimeChapter();
        bool ResolvePracticeAnchor(int section, size_t& sample, size_t& end);
    };

    // Locate the mark the practice jump landed on. A chapter jump resolves by the
    // runtime chapter index thprac writes (chapter - 1, the key the chapter
    // records use), a section jump by the section id stored with the mark. When
    // neither matches, the route cannot identify this target. Practice always
    // uses the initial player spawn; that position cannot identify a replay mark.
    inline bool TH15RouteTrace::ResolvePracticeAnchor(int section, size_t& sample, size_t& end)
    {
        if (loadedMarks.empty() || loadedPoints.size() < 2)
            return false;
        if (PracticeScriptRange(section, sample, end)) {
            const auto* clock = FindTH15RouteClock(section, thPracParam.phase, GetMemContent<int32_t>(0x4E7410));
            practiceAnchorChapter = clock->entryChapter;
            TraceLog(std::format("script-match section={} mode={} chapter={} sync={} sample={} end={}",
                section, clock->mode, clock->entryChapter, clock->syncChapter, sample, end));
            return true;
        }
        int targetChapter = -1;
        int targetSection = 0;
        if (section >= 10000 && section < 20000) {
            const auto* clock = FindTH15RouteClock(section, thPracParam.phase, GetMemContent<int32_t>(0x4E7410));
            // Some menus share a runtime chapter or skip indices entirely.
            targetChapter = clock ? clock->entryChapter : (section - 10000) % 100 - 1;
        } else if (section > 0) {
            targetSection = section;
        }
        int best = -1;
        for (size_t i = 0; i < loadedMarks.size() && best < 0; ++i) {
            const auto& mark = loadedMarks[i];
            if (mark.frame < 0 || static_cast<size_t>(mark.frame) >= loadedPoints.size())
                continue;
            if (targetChapter >= 0 ? mark.chapter == targetChapter
                                   : mark.section == targetSection)
                best = static_cast<int>(i);
        }
        if (best < 0)
            return false;
        sample = static_cast<size_t>(loadedMarks[best].frame);
        // Record which chapter the anchor landed on so a later chapter change can
        // be told apart from the stale index the jump left behind.
        practiceAnchorChapter = loadedMarks[best].chapter;
        end = static_cast<size_t>(best + 1) < loadedMarks.size()
            ? static_cast<size_t>(loadedMarks[best + 1].frame)
            : loadedPoints.size();
        if (end > loadedPoints.size() || end <= sample)
            end = loadedPoints.size();
        TraceLog(std::format(
            "match section={} targetChapter={} targetSection={} mark={} markChapter={} sample={} end={}",
            section, targetChapter, targetSection, best, loadedMarks[best].chapter, sample, end));
        return end > sample + 1;
    }

    inline void TH15RouteTrace::UpdatePracticeAnchor()
    {
        const int section = thPracParam.mode == 1 ? thPracParam.section : 0;
        const int stageFrames = ReadStageFrames();
        // A retry reuses the same practice selection, so re-arm when the stage
        // frame counter restarts as well.
        const bool restarted = stageFrames < lastStageFrames
            || lastWarpGeneration != g_th15RouteWarpGeneration;
        lastStageFrames = stageFrames;
        lastWarpGeneration = g_th15RouteWarpGeneration;
        if (section != lastPracticeSection || restarted) {
            lastPracticeSection = section;
            practiceAnchorValid = false;
            practiceAnchorPending = section != 0;
            practiceAnchorWait = 0;
            practiceAutopilotFrames = 0;
            practiceMeasureValid = false;
            practicePlaybackStart = -1;
            practiceAwaitRuntimeChapter = false;
            TraceLog(std::format("arm section={} stageFrames={} restarted={}",
                section, stageFrames, restarted ? 1 : 0));
        }
        if (practiceAnchorValid || practiceAwaitRuntimeChapter)
            FollowRuntimeChapter();
        if (practiceAnchorValid) {
            if (!followPlayerToRoute)
                practiceAutopilotFrames = 0;
            else if (ReadChapterFrames() != practiceLastCounter)
                ++practiceAutopilotFrames;
            practiceLastCounter = ReadChapterFrames();
        }
        // Recover from a dropped anchor or a route reloaded. Resolve
        // again from the practice selection, but not on every single frame.
        if (section != 0 && !practiceAnchorValid && !practiceAnchorPending
            && !practiceAwaitRuntimeChapter) {
            if (++practiceAnchorRecoverWait > 30) {
                practiceAnchorRecoverWait = 0;
                practiceAnchorPending = true;
                practiceAnchorWait = 0;
                TraceLog(std::format("recover section={}", section));
            }
        } else {
            practiceAnchorRecoverWait = 0;
        }
        if (!practiceAnchorPending)
            return;
        const int stage = GetMemContent<int32_t>(0x4E73F0) - 1;
        if (!ValidStage(stage))
            return;
        const bool chapterJump = section >= 10000 && section < 20000;
        // A chapter id names its own stage. Resolving while the previous stage is
        // still loaded would pin playback to a sample index taken from another
        // file, which then lands on an unrelated stretch of the new stage.
        if (chapterJump && (section - 10000) / 100 - 1 != stage)
            return;
        // Wait for the jump to land; the first frames of a stage load can still
        // hold the previous run's position.
        if (++practiceAnchorWait > 600) {
            practiceAnchorPending = false;
            return;
        }
        const int diff = std::clamp(GetMemContent<int32_t>(0x4E7410), 0, 4);
        const int shot = std::clamp(GetMemContent<int32_t>(0x4E7404), 0, 3);
        const RouteInfo* target = ResolveTarget(stage, diff, shot);
        if (!target) {
            practiceAnchorPending = false;
            return;
        }
        EnsureLoaded(target->file);
        size_t sample = 0;
        size_t end = 0;
        const bool resolved = ResolvePracticeAnchor(section, sample, end);
        if (resolved) {
            // This range identifies the recorded chapter, not the warp's clock
            // origin. Draw resolves that origin live from the selected script
            // path and the next recorded chapter, after the warp is installed.
            practiceAnchorSample = static_cast<int>(sample);
            practiceAnchorEnd = static_cast<int>(end);
            practiceBaselineCounter = ReadChapterFrames();
            practiceAnchorRuntimeChapter = CurrentChapter();
            practiceAnchorFollowed = false;
            practiceAutopilotFrames = 0;
            practiceAnchorValid = true;
            TraceLog(std::format(
                "anchor section={} stage={} sample={} end={} baseline={} runtimeChapter={}",
                section, stage, practiceAnchorSample, practiceAnchorEnd,
                practiceBaselineCounter, CurrentChapter()));
        } else {
            TraceLog(std::format("anchor-fail section={} stage={} chapterJump={} marks={} points={}",
                section, stage, chapterJump ? 1 : 0, loadedMarks.size(), loadedPoints.size()));
        }
        practiceAnchorPending = false;
    }

    // A practice jump skips the chapter routine of the chapter it lands in, but
    // the chapters that follow run normally. Once the runtime index moves away
    // from the value it had when the anchor was taken, the game's own index is
    // authoritative again: re-seat playback on the chapter the game entered
    // instead of replaying the one that was jumped to.
    inline void TH15RouteTrace::FollowRuntimeChapter()
    {
        const int chapter = CurrentChapter();
        if (chapter == practiceAnchorRuntimeChapter)
            return;
        practiceAnchorRuntimeChapter = chapter;
        // Zero is the stage's initial index. The stage-4 practice paths skip the
        // selected chapter routine, so the game keeps this value until
        // the next chapter starts; following it dragged playback back to the
        // first chapter of the stage after a retry.
        if (chapter <= 0)
            return;
        const auto* clock = FindTH15RouteClock(lastPracticeSection, thPracParam.phase,
            GetMemContent<int32_t>(0x4E7410));
        int scriptStart = 0;
        if (clock && clock->mode == 1 && PracticeScriptStart(lastPracticeSection, scriptStart)
            && chapter != clock->syncChapter) {
            // ECLSetChapter can write a synthetic entry index during setup.
            // It is not the later script boundary this clock is anchored to.
            return;
        }
        const int counter = ReadChapterFrames();
        int best = -1;
        for (size_t i = 0; i < loadedMarks.size(); ++i) {
            if (loadedMarks[i].chapter == chapter) {
                // Skip only the transient stage-entry mark, so a duplicate id
                // later in the route resolves to the chapter actually entered.
                if (i == 0 && loadedMarks.size() > 1
                    && loadedMarks[1].frame - loadedMarks[0].frame < kMinFollowSamples)
                    continue;
                best = static_cast<int>(i);
                break;
            }
        }
        if (best < 0) {
            // The run has moved past the selected practice target. Replaying
            // that target here would show a different chapter; wait for the next
            // recorded runtime chapter instead.
            practiceAnchorValid = false;
            practiceAwaitRuntimeChapter = true;
            TraceLog(std::format("follow-none chapter={} counter={}", chapter, counter));
            return;
        }
        const size_t sample = static_cast<size_t>(loadedMarks[best].frame);
        size_t end = static_cast<size_t>(best + 1) < loadedMarks.size()
            ? static_cast<size_t>(loadedMarks[best + 1].frame)
            : loadedPoints.size();
        if (end > loadedPoints.size() || end <= sample + 1)
            end = loadedPoints.size();
        // The game just left the chapter the anchor was pinned to. Its elapsed
        // counter says how long that chapter took from the playback start; the
        // route's own chapter length is known, so the difference is the offset
        // that is still missing. Only meaningful while the test "move the player
        // along the route" switch is on, because then the run really does follow
        // the reference path instead of the player's own pace.
        if (!practiceAnchorFollowed && followPlayerToRoute && practicePlaybackStart >= 0
            && practiceAutopilotFrames > 300) {
            const int length = practiceAnchorEnd - practicePlaybackStart;
            const int elapsed = practiceLastCounter - practiceBaselineCounter;
            // This only reports a duration difference; a chapter's clear time
            // cannot establish the timing of the practice warp.
            if (length > 300 && elapsed > 300) {
                practiceMeasuredResidual = length - elapsed;
                practiceMeasureValid = true;
                // Clear time depends on kills, shooting and skipped script paths.
                // It is diagnostic only, never a reusable practice offset.
                TraceLog(std::format("measure residual={} length={} elapsed={} autopilotFrames={}",
                    practiceMeasuredResidual, length, elapsed, practiceAutopilotFrames));
            }
        }
        practiceAutopilotFrames = 0;
        practiceAnchorSample = static_cast<int>(sample);
        practiceAnchorEnd = static_cast<int>(end);
        practiceAnchorChapter = loadedMarks[best].chapter;
        practiceAnchorValid = true;
        practiceAwaitRuntimeChapter = false;
        // Both counters refer to the same chapter origin. Preserve any elapsed
        // frames when the first draw/update arrives after the chapter switch.
        practiceBaselineCounter = loadedMarks[best].chapterFrames;
        practiceAnchorFollowed = true;
        TraceLog(std::format("follow chapter={} sample={} end={} baseline={}",
            chapter, practiceAnchorSample, practiceAnchorEnd, practiceBaselineCounter));
    }

    inline void TH15RouteTrace::TryStartCapture()
    {
        if (capturing)
            return;
        const bool armedMatch = extractArmed && ReplayNameMatches(armedReplay);
        if (extractArmed && !armedMatch)
            return;
        if (!autoExtractReplay && !armedMatch)
            return;
        const int stage = GetMemContent<int32_t>(0x4E73F0) - 1;
        if (!ValidStage(stage))
            return;
        const std::wstring& name = THGuiRep::singleton().ReplayName();
        captureLabel = name.empty() ? std::string("replay") : utf16_to_utf8(name.c_str());
        BeginCapture(stage, GetMemContent<int32_t>(0x4E7410), GetMemContent<int32_t>(0x4E7404), 0);
        if (armedMatch)
            extractArmed = false;
    }

    inline void TH15RouteTrace::SamplePlayer()
    {
        const uintptr_t player = GetMemContent(PLAYER_PTR);
        if (!player)
            return;
        const int stage = GetMemContent<int32_t>(0x4E73F0) - 1;
        if (ValidStage(stage) && stage != captureStage) {
            // Keep the finished stage and continue with the new one.
            FinishCapture(true);
            BeginCapture(stage, GetMemContent<int32_t>(0x4E7410),
                GetMemContent<int32_t>(0x4E7404), captureSource);
            return;
        }
        const float x = GetMemContent<float>(player + 0x618);
        const float y = GetMemContent<float>(player + 0x61C);
        if (!std::isfinite(x) || !std::isfinite(y))
            return;
        // Reject values from transitions and menus; the stage is 448x448 wide.
        if (x < -260.0f || x > 260.0f || y < -80.0f || y > 640.0f)
            return;
        if (capturePoints.size() >= (size_t)kMaxPoints) {
            FinishCapture(true);
            return;
        }
        // One sample per rendered frame keeps sample index == stage frame, so
        // the display window can be positioned from progress alone.
        const int chapter = CurrentChapter();
        const int chapterFrames = ReadChapterFrames();
        // The game counter advances once per gameplay frame. Sampling only when
        // it moved keeps one sample per frame even if the game is paused or the
        // render loop runs ahead of the logic.
        if (!capturePoints.empty() && chapterFrames == captureLastCounter
            && chapter == captureChapter) {
            return;
        }
        captureLastCounter = chapterFrames;
        if (chapter != captureChapter) {
            captureChapter = chapter;
            captureMarks.push_back({ chapter, captureFrame, chapterFrames, CurrentRouteSection() });
        }
        bool breakBefore = false;
        if (!capturePoints.empty()) {
            const ImVec2& last = capturePoints.back();
            const float dx = x - last.x;
            const float dy = y - last.y;
            // Respawn or teleport: split the polyline instead of drawing a
            // straight line across the playfield.
            breakBefore = dx * dx + dy * dy > 3600.0f;
        }
        capturePoints.emplace_back(x, y);
        captureBreaks.push_back(breakBefore ? 1 : 0);
        ++captureFrame;
        capturePointCount = capturePoints.size();
    }

    class THOverlay : public Gui::GameGuiWnd {
        THOverlay() noexcept
        {
            SetTitle("Mod Menu");
            SetFade(0.5f, 0.5f);
            SetPos(10.0f, 10.0f);
            SetSize(0.0f, 0.0f);
            SetWndFlag(
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | 0);
            OnLocaleChange();
        }
        SINGLETON(THOverlay)
    public:

    protected:
        virtual void OnLocaleChange() override
        {
            float x_offset_1 = 0.0f;
            float x_offset_2 = 0.0f;
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                x_offset_1 = 0.1f;
                x_offset_2 = 0.14f;
                break;
            case Gui::LOCALE_EN_US:
                x_offset_1 = 0.1f;
                x_offset_2 = 0.14f;
                break;
            case Gui::LOCALE_JA_JP:
                x_offset_1 = 0.1f;
                x_offset_2 = 0.14f;
                break;
            default:
                break;
            }

            mMenu.SetTextOffsetRel(x_offset_1, x_offset_2);
            mMuteki.SetTextOffsetRel(x_offset_1, x_offset_2);
            mInfLives.SetTextOffsetRel(x_offset_1, x_offset_2);
            mInfBombs.SetTextOffsetRel(x_offset_1, x_offset_2);
            mInfPower.SetTextOffsetRel(x_offset_1, x_offset_2);
            mTimeLock.SetTextOffsetRel(x_offset_1, x_offset_2);
            mAutoBomb.SetTextOffsetRel(x_offset_1, x_offset_2);
            mElBgm.SetTextOffsetRel(x_offset_1, x_offset_2);
            mInGameInfo.SetTextOffsetRel(x_offset_1, x_offset_2);
            mEnemyMuteki.SetTextOffsetRel(x_offset_1, x_offset_2);
        }
        virtual void OnContentUpdate() override
        {
            mMuteki();
            mInfLives();
            mInfBombs();
            mInfPower();
            mTimeLock();
            mAutoBomb();
            mElBgm();
            mInGameInfo();
            mEnemyMuteki();
        }
        virtual void OnPreUpdate() override
        {
            if (mMenu(false) && !ImGui::IsAnyItemActive()) {
                if (*mMenu) {
                    Open();
                } else {
                    Close();
                }
            }
        }

        Gui::GuiHotKeyChord mMenu { "ModMenuToggle", "BACKSPACE", Gui::GetBackspaceMenuChord() };

        HOTKEY_DEFINE(mMuteki, TH_MUTEKI, "F1", VK_F1)
        PATCH_HK(0x4566a5, "01")
        HOTKEY_ENDDEF();


        HOTKEY_DEFINE(mInfBombs, TH_INFBOMBS, "F3", VK_F3)
        PATCH_HK(0x414963, "90")
        HOTKEY_ENDDEF();

        HOTKEY_DEFINE(mInfPower, TH_INFPOWER, "F4", VK_F4)
        PATCH_HK(0x4582fa, "45")
        HOTKEY_ENDDEF();



        HOTKEY_DEFINE(mAutoBomb, TH_AUTOBOMB, "F6", VK_F6)
        PATCH_HK(0x454cc9, "c6")
        HOTKEY_ENDDEF();


    public:
        Gui::GuiHotKey mInfLives { TH_INFLIVES2, "F2", VK_F2,};

        HOTKEY_DEFINE(mTimeLock, TH_TIMELOCK, "F5", VK_F5)
        PATCH_HK(0x41fdf5, "eb"),
        PATCH_HK(0x428b5d, "a7")
        HOTKEY_ENDDEF();

        Gui::GuiHotKey mElBgm { TH_EL_BGM, "F7", VK_F7 };
        Gui::GuiHotKey mInGameInfo { THPRAC_INGAMEINFO, "F8", VK_F8 };


        HOTKEY_DEFINE(mEnemyMuteki, TH_ENEMY_MUTEKI, "U", 'U')
        PATCH_HK(0x424E06, "9090"),
        PATCH_HK(0x424DFD, "9090")
        HOTKEY_ENDDEF();
    };

    class TH15InGameInfo : public Gui::GameGuiWnd {

        TH15InGameInfo() noexcept
        {
            SetInformationPanel(false);
            SetTitle("igi");
            SetFade(0.9f, 0.9f);
            SetSizeRel(360.0f / 1280.0f, 0.0f);
            SetPosRel(890.0f / 1280.0f, 510.0f / 960.0f);
            SetWndFlag(ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | 0);
            OnLocaleChange();
        }
        SINGLETON(TH15InGameInfo)

    public:
        int32_t mMissCount;
        int32_t mBombCount;
    protected:
        virtual void OnLocaleChange() override
        {
            float x_offset_1 = 0.0f;
            float x_offset_2 = 0.0f;
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                x_offset_1 = 0.12f;
                x_offset_2 = 0.172f;
                break;
            case Gui::LOCALE_EN_US:
                x_offset_1 = 0.12f;
                x_offset_2 = 0.16f;
                break;
            case Gui::LOCALE_JA_JP:
                x_offset_1 = 0.18f;
                x_offset_2 = 0.235f;
                break;
            default:
                break;
            }
        }

        virtual void OnContentUpdate() override
        {
            TH15ChapterSave::singleton().LoadHudChapterView();
            if (thPracParam.mode && thPracParam.section == TH15_ST8_AB_TEST) {
                DWORD ecl_glob = *(DWORD*)(0x4E9A80);
                float scores[5] = { 0.0f };
                if (ecl_glob){
                    for (int i=0;i<5;i++)
                        scores[i] = *(float*)(ecl_glob + 0x38 - i * 4);
                }
                ImGui::Columns(2);
                for (int i = 0; i < 5; i++)
                {
                    ImGui::Text("p%d",i+1);
                    ImGui::NextColumn();
                    ImGui::Text("%.2f", scores[i]);
                    ImGui::NextColumn();
                }
                ImGui::Columns(1);
                return;
            }
            bool is_in_P_mode = (*(byte*)(0x004E7795)) & 0x1;
            if (is_in_P_mode) // pplayer
            {
                byte cur_player_type = static_cast<byte>((*(int32_t*)(0x4E7404)));
                int32_t diff = *((int32_t*)0x4e7410);
                auto diff_pl = std::format("{} ({})", S(IGI_DIFF[diff]), S(IGI_PL_15[cur_player_type]));
                auto diff_pl_sz = ImGui::CalcTextSize(diff_pl.c_str());

                ImGui::SetCursorPosX(ImGui::GetWindowSize().x * 0.5f - diff_pl_sz.x * 0.5f);
                ImGui::Text(diff_pl.c_str());

                int cur_stage = *(int*)(0x004E73F0);
                int tot_re = *(int*)(0x004E7594);
                int cur_re = *(int*)(0x004E75B8);
                int* stage_re = (int*)(0x004E759C);
                ImGui::Columns(2);
                if (g_adv_igi_options.th15_showShootingDownRate) {
                    int32_t cur_killed = *(int32_t*)(0x004E7488);
                    int32_t tot_enm = *(int32_t*)(0x004E7484);
                    int32_t graze = *(int32_t*)(0x004E7420);
                    ImGui::Text(S(THPRAC_INGAMEINFO_TH15_SHOOTING_DOWN_RATE));
                    ImGui::NextColumn();
                    ImGui::Text("%2.1f%%(%d/%d)", tot_enm == 0 ? 0.0f : (float)cur_killed / (float)tot_enm * 100.0f, cur_killed, tot_enm);
                    ImGui::NextColumn();
                    ImGui::Text(S(THPRAC_INGAMEINFO_TH15_PRODUCT));
                    ImGui::NextColumn();
                    ImGui::Text("%2.1f", tot_enm == 0 ? 0.0f : (float)cur_killed / (float)tot_enm * (float)graze);
                    ImGui::NextColumn();
                }
                ImGui::Text(S(THPRAC_INGAMEINFO_15_RE_TIMES));
                ImGui::NextColumn();
                ImGui::Text("%d / %d", cur_re, tot_re);
                ImGui::NextColumn();
                if (cur_stage == 7) {
                    ImGui::Text(S(THPRAC_INGAMEINFO_15_RE_TIMES_STAGE), 7);
                    ImGui::NextColumn();
                    ImGui::Text("%8d", stage_re[6]);
                    ImGui::NextColumn();
                } else {
                    for (int i = 0; i < std::min(6,cur_stage); i++){
                        ImGui::Text(S(THPRAC_INGAMEINFO_15_RE_TIMES_STAGE), i + 1);
                        ImGui::NextColumn();
                        ImGui::Text("%8d", stage_re[i]);
                        ImGui::NextColumn();
                    }
                }
            } else {
                byte cur_player_type = static_cast<byte>((*(int32_t*)(0x4E7404)));
                int32_t diff = *((int32_t*)0x4e7410);
                auto diff_pl = std::format("{} ({})", S(IGI_DIFF[diff]), S(IGI_PL_15[cur_player_type]));
                auto diff_pl_sz = ImGui::CalcTextSize(diff_pl.c_str());

                ImGui::SetCursorPosX(ImGui::GetWindowSize().x * 0.5f - diff_pl_sz.x * 0.5f);
                ImGui::Text(diff_pl.c_str());

                ImGui::Columns(2);
                ImGui::Text(S(THPRAC_INGAMEINFO_MISS_COUNT));ImGui::NextColumn();ImGui::Text("%8d",mMissCount);
                ImGui::NextColumn();
                ImGui::Text(S(THPRAC_INGAMEINFO_BOMB_COUNT));ImGui::NextColumn();ImGui::Text("%8d",mBombCount);
                ImGui::NextColumn();
                if (g_adv_igi_options.th15_showShootingDownRate) {
                    int32_t cur_killed = *(int32_t*)(0x004E7488);
                    int32_t tot_enm = *(int32_t*)(0x004E7484);
                    int32_t graze = *(int32_t*)(0x004E7420);
                    ImGui::Text(S(THPRAC_INGAMEINFO_TH15_SHOOTING_DOWN_RATE));
                    ImGui::NextColumn();
                    ImGui::Text("%2.1f%%(%d/%d)", tot_enm == 0 ? 0.0f : (float)cur_killed / (float)tot_enm * 100.0f, cur_killed, tot_enm);
                    ImGui::NextColumn();
                    ImGui::Text(S(THPRAC_INGAMEINFO_TH15_PRODUCT));
                    ImGui::NextColumn();
                    ImGui::Text("%2.1f", tot_enm == 0 ? 0.0f : (float)cur_killed / (float)tot_enm * (float)graze);
                    ImGui::NextColumn();
                }
            }
            ImGui::Columns(1);
            auto& chapterRate = TH15ChapterSave::singleton();
            if (g_th15ChapterHudEnabled && chapterRate.HasDisplay()) {
                chapterRate.LoadHudChapterView();
                const int stage = chapterRate.DisplayStage();
                const int chapter = chapterRate.DisplayChapter();
                const int diff = chapterRate.DisplayDiff();
                const int player = chapterRate.DisplayPlayer();
                const bool isFailed = chapterRate.IsActive() && chapterRate.IsFailed();
                const bool isBossSection = chapterRate.DisplayIsBossSection();
                const int section = chapterRate.DisplaySection();
                const char* sectionName = isBossSection && section >= 0 && section < 75
                    ? th_sections_str[Gui::LocaleGet()][diff][section] : nullptr;
                auto title = isBossSection && sectionName
                    ? std::format("Stage {}  {}", stage + 1, sectionName)
                    : std::format("Stage {}  Chapter {}", stage + 1, chapter + 1);
                if (!isBossSection && Gui::LocaleGet() == Gui::LOCALE_ZH_CN) {
                    if (const char* nickname = TH15ChapterNickname(stage, chapter + 1))
                        title += std::format("（{}）", nickname);
                }
                const ImVec4 titleColor = isFailed
                    ? ImVec4(0.55f, 0.55f, 0.55f, 1.0f)
                    : ImGui::GetStyleColorVec4(ImGuiCol_::ImGuiCol_Text);
                ImGui::SetCursorPosX(ImGui::GetWindowSize().x * 0.5f - ImGui::CalcTextSize(title.c_str()).x * 0.5f);
                auto titleStart = ImGui::GetCursorScreenPos();
                ImGui::TextColored(titleColor, "%s", title.c_str());
                auto titleEnd = ImGui::GetCursorScreenPos();
                if (isFailed && g_adv_igi_options.th15_hudChapterView == 0) {
                    const float titleWidth = ImGui::CalcTextSize(title.c_str()).x;
                    BossCapture::DrawFailureSlash(titleStart, {titleStart.x + titleWidth, titleEnd.y});
                }
                TH15ChapterSave::LiveChapterStats liveStats{};
                const bool hasLiveStats = chapterRate.GetLiveChapterStats(liveStats);
                if (g_adv_igi_options.th15_hudChapterView == 0) {
                const auto counts = chapterRate.GetCaptureRateCounts(stage, chapter, section, diff, player, isBossSection);
                const int currentAttempts = counts.sessionAttempts, currentPasses = counts.sessionPasses;
                const int totalAttempts = counts.totalAttempts, totalPasses = counts.totalPasses;
                ImGui::Columns(2);
                ImGui::Text(S(THPRAC_INGAMEINFO_20_PASS_CUR)); ImGui::NextColumn();
                ImGui::Text("%d/%d (%.1f%%)", currentPasses, currentAttempts,
                    currentAttempts > 0 ? 100.0f * currentPasses / currentAttempts : 0.0f); ImGui::NextColumn();
                ImGui::Text(S(THPRAC_INGAMEINFO_20_PASS_TOT)); ImGui::NextColumn();
                ImGui::Text("%d/%d (%.1f%%)", totalPasses, totalAttempts,
                    totalAttempts > 0 ? 100.0f * totalPasses / totalAttempts : 0.0f);
                ImGui::Columns(1);
                } else {
                if (hasLiveStats) {
                    const int locale = Gui::LocaleGet();
                    ImGui::Separator();
                    ImGui::Columns(2);
                    auto compactScore = [](int64_t value) {
                        if (value >= 1000000)
                            return std::format("+{:.2f}M", value / 1000000.0);
                        return std::format("+{}", value);
                    };
                    auto showTarget = [&](const std::string& current, const std::string& target) {
                        const std::string shownCurrent = liveStats.hasLiveAttempt ? current : "--";
                        const auto value = target != "--"
                            ? std::format("{} / {}", shownCurrent, target)
                            : std::format("{} / --", shownCurrent);
                        ImGui::TextUnformatted(value.c_str());
                    };
                    ImGui::TextUnformatted(locale == Gui::LOCALE_ZH_CN ? "本章擦弹"
                        : locale == Gui::LOCALE_JA_JP ? "章グレイズ" : "Chapter grazes");
                    ImGui::NextColumn();
                    showTarget(std::to_string(liveStats.graze), liveStats.targetGraze >= 0
                        ? std::to_string(liveStats.targetGraze) : "--");
                    ImGui::NextColumn();
                    ImGui::TextUnformatted(locale == Gui::LOCALE_ZH_CN ? "本章蓝点"
                        : locale == Gui::LOCALE_JA_JP ? "章青点" : "Chapter point items");
                    ImGui::NextColumn();
                    showTarget(std::to_string(liveStats.bluePoints), liveStats.targetBluePoints >= 0
                        ? std::to_string(liveStats.targetBluePoints) : "--");
                    ImGui::NextColumn();
                    ImGui::TextUnformatted(locale == Gui::LOCALE_ZH_CN ? "最大得点增量"
                        : locale == Gui::LOCALE_JA_JP ? "最大得点増分" : "Max point delta");
                    ImGui::NextColumn();
                    showTarget(compactScore(liveStats.maxPointDelta), liveStats.targetMaxPointDelta >= 0
                        ? compactScore(liveStats.targetMaxPointDelta) : "--");
                    ImGui::NextColumn();
                    ImGui::TextUnformatted(locale == Gui::LOCALE_ZH_CN ? "章节得分增量"
                        : locale == Gui::LOCALE_JA_JP ? "章スコア増分" : "Chapter score delta");
                    ImGui::NextColumn();
                    showTarget(compactScore(liveStats.chapterScore), liveStats.targetChapterScore >= 0
                        ? compactScore(liveStats.targetChapterScore) : "--");
                    ImGui::Columns(1);
                    const int resourceRounded = static_cast<int>(std::llround(liveStats.resource));
                    if (liveStats.hasLiveAttempt)
                        ImGui::Text("RES %d    Kill %.1f%%", resourceRounded,
                            liveStats.killRatePercent);
                    else
                        ImGui::TextUnformatted("RES --    Kill --");
                }
                }
                if (hasLiveStats) {
                    const int locale = Gui::LocaleGet();
                    const auto scoreInBillions = [](int64_t score) {
                        return std::format("{:.2f}B", static_cast<double>(score) / 1000000000.0);
                    };
                    const std::string record = liveStats.hasBestCurrentScore
                        ? scoreInBillions(liveStats.bestCurrentScore)
                        : "--";
                    const std::string currentScore = liveStats.hasLiveAttempt
                        ? scoreInBillions(liveStats.currentScore) : "--";
                    ImGui::Text("%s %s / %s",
                        locale == Gui::LOCALE_ZH_CN ? "当前得分"
                            : locale == Gui::LOCALE_JA_JP ? "現在スコア" : "Current score",
                        currentScore.c_str(), record.c_str());
                }
            }
        }

        virtual void OnPreUpdate() override
        {
            if (GetMemContent(PLAYER_PTR)) {
                GameUpdateInner(15);
            } else {
            }
            if (*(THOverlay::singleton().mInGameInfo) && GetMemContent(PLAYER_PTR)) {
                SetSizeRel(360.0f / 1280.0f, 0.0f);
                SetPosRel(890.0f / 1280.0f, 510.0f / 960.0f);
                Open();
            } else {
                Close();
            }
        }

    public:
    };

    PATCH_ST(th15_all_clear_bonus_1, 0x43d99d, "eb0b909090");
    EHOOK_ST(th15_all_clear_bonus_2, 0x43daac, 7, {
        *(int32_t*)(GetMemAddr(0x4e9a8c, 0x160)) = *(int32_t*)(0x4e740c);
        if (GetMemContent(0x4e7794) & 0x10) {
            typedef void (*PScoreFunc)();
            PScoreFunc a = (PScoreFunc)0x4512a0;
            a();
            pCtx->Eip = 0x43d9a2;
        }
    });
    EHOOK_ST(th15_all_clear_bonus_3, 0x43dcb5, 7, {
        *(int32_t*)(GetMemAddr(0x4e9a8c, 0x160)) = *(int32_t*)(0x4e740c);
        if (GetMemContent(0x4e7794) & 0x10) {
            typedef void (*PScoreFunc)();
            PScoreFunc a = (PScoreFunc)0x4512a0;
            a();
            pCtx->Eip = 0x43d9a2;
        }
    });
    HOOKSET_DEFINE(th15_master_disable)
    PATCH_DY(th15_master_disable1a, 0x420346, "eb")
    PATCH_DY(th15_master_disable1b, 0x42036B, "eb")
    PATCH_DY(th15_master_disable1c, 0x420296, "00")
    HOOKSET_ENDDEF()

    float g_bossMoveDownRange = BOSS_MOVE_DOWN_RANGE_INIT;
    EHOOK_ST(th15_bossmovedown, 0x0042B261,5,{
        float* y_pos = (float*)(pCtx->Edi + 0x3F64);
        float* y_range = (float*)(pCtx->Edi + 0x3F6C);
        float y_max = (*y_pos) + (*y_range) * 0.5f;
        float y_min2 = y_max - (*y_range) * (1.0f - g_bossMoveDownRange);
        *y_pos = (y_max + y_min2) * 0.5f;
        *y_range = (y_max - y_min2);
    });

    // In TH15's bounded boss movement selector, these branches decide whether
    // the selected horizontal angle is reversed by PI after the random roll.
    // Keep the range correction branches intact so forced movement stays inside
    // the boss's ECL movement area.
    HOOKSET_DEFINE(th15_bossmovement_follow)
    PATCH_DY(th15_bossmovement_follow_right, 0x42B3B6, "eb54")
    PATCH_DY(th15_bossmovement_follow_left, 0x42B3FC, "9090")
    HOOKSET_ENDDEF()

    HOOKSET_DEFINE(th15_bossmovement_away)
    PATCH_DY(th15_bossmovement_away_right, 0x42B3B6, "9090")
    PATCH_DY(th15_bossmovement_away_left, 0x42B3FC, "eb0e")
    HOOKSET_ENDDEF()

    // Skip each clamp of the live maximum point value while uncapping is enabled.
    HOOKSET_DEFINE(th15_max_point_uncap)
    PATCH_DY(th15_max_point_uncap_item, 0x43AD51, "90909090909090")
    PATCH_DY(th15_max_point_uncap_shared, 0x43B21A, "90909090909090")
    PATCH_DY(th15_max_point_uncap_bonus, 0x440368, "90909090909090")
    PATCH_DY(th15_max_point_uncap_graze, 0x4573EE, "90909090909090")
    HOOKSET_ENDDEF()

    static void TH15MaxPointUncapInit()
    {
        for (auto& hook : th15_max_point_uncap)
            hook.Setup();
    }

    static void TH15MaxPointUncapSet(bool enabled)
    {
        for (auto& hook : th15_max_point_uncap)
            hook.Toggle(enabled);

        if (!enabled && GetMemContent(PLAYER_PTR)
            && *(int32_t*)0x4E7434 > *(int32_t*)0x4E743C)
            *(int32_t*)0x4E7434 = *(int32_t*)0x4E743C;
    }

    static void TH15BossMovementInit()
    {
        for (auto& hook : th15_bossmovement_follow)
            hook.Setup();
        for (auto& hook : th15_bossmovement_away)
            hook.Setup();
    }

    static void TH15BossMovementSet(int mode)
    {
        for (auto& hook : th15_bossmovement_follow)
            hook.Toggle(false);
        for (auto& hook : th15_bossmovement_away)
            hook.Toggle(false);

        if (mode == 1) {
            for (auto& hook : th15_bossmovement_away)
                hook.Toggle(true);
        } else if (mode == 2) {
            for (auto& hook : th15_bossmovement_follow)
                hook.Toggle(true);
        }
    }

    // Record the values actually written by TH15's ECL lifeSet handler.
    EHOOK_ST(th15_boss_hp_runtime, 0x0042B907, 5, {
        if (thPracParam.mode == 1 && thPracParam.section == TH15_ST2_BOSS3
            && g_th15BossHpRuntimeLogCount < 4) {
            const auto* script = reinterpret_cast<const uint8_t*>(pCtx->Edi);
            const uint32_t current = *reinterpret_cast<const uint32_t*>(script + 0x3f74);
            const uint32_t maxLife = *reinterpret_cast<const uint32_t*>(script + 0x3f78);
            const uint32_t marker = *reinterpret_cast<const uint32_t*>(script + 0x3f7c);
            const uint32_t derived = *reinterpret_cast<const uint32_t*>(script + 0x3f80);
            TH15BossHpLog("runtime-lifeSet", thPracParam.st2_boss2_hp,
                current, maxLife, marker, derived, pCtx->Edi);
            ++g_th15BossHpRuntimeLogCount;
        }
    });
    class THAdvOptWnd : public Gui::PPGuiWnd {
        SINGLETON(THAdvOptWnd)
    public:
        bool forceBossMoveDown = false;
    private:
        void MasterDisableInit()
        {
            for (int i = 0; i < 3; i++)
                th15_master_disable[i].Setup();
            for (int i = 0; i < 3; i++)
                th15_master_disable[i].Toggle(g_adv_igi_options.disable_master_autoly);
        }
        void FpsInit()
        {
            if ((mOptCtx.vpatch_base = (uintptr_t)GetModuleHandleW(L"openinputlagpatch.dll")) != NULL) {
                OILPInit(mOptCtx);
            } else if ((mOptCtx.vpatch_base = (uintptr_t)GetModuleHandleW(L"vpatch_th15.dll")) != NULL) {
                uint64_t hash[2];
                CalcFileHash(L"vpatch_th15.dll", hash);

                bool vp_valid = hash[0] == 7265142250215198902ll && hash[1] == 13547095955570115225ll;
                if (hash[0] == 16371671977271057239ll && hash[1] == 17823539316282081507ll) {
                    vp_valid = true;
                    if (MessageBoxW(
                        *(HWND*)0x519bb0,
                        L"Old version of vpatch detected. Do you want to download the newest version?",
                        L"thprac: warning",
                        MB_ICONWARNING | MB_YESNO
                    ) == IDYES) {
                        ShellExecuteW(NULL, NULL, L"https://maribelhearn.com/mirror/VsyncPatch.zip", NULL, NULL, SW_SHOW);
                    }
                }
                if (vp_valid && *(int32_t*)(mOptCtx.vpatch_base + 0x42fbc) == 0) {
                    mOptCtx.fps_status = 2;
                    mOptCtx.fps = *(int32_t*)(mOptCtx.vpatch_base + 0x42fcc);
                } else if (!vp_valid) {
                    mOptCtx.fps_status = -1;
                }
            } else if (*(uint8_t*)0x4e79c9 == 3) {
                mOptCtx.fps_status = 1;

                DWORD oldProtect;
                VirtualProtect((void*)0x472802, 4, PAGE_EXECUTE_READWRITE, &oldProtect);
                *(double**)0x472802 = &mOptCtx.fps_dbl;
                VirtualProtect((void*)0x472802, 4, oldProtect, &oldProtect);
            } else
                mOptCtx.fps_status = 0;
        }
        void FpsSet()
        {
            if (mOptCtx.fps_status == 3) {
                mOptCtx.oilp_set_game_fps(mOptCtx.fps);
                mOptCtx.oilp_set_replay_skip_fps(mOptCtx.fps_replay_fast);
                mOptCtx.oilp_set_replay_slow_fps(mOptCtx.fps_replay_slow);
            } else if (mOptCtx.fps_status == 1) {
                mOptCtx.fps_dbl = 1.0 / (double)mOptCtx.fps;
            } else if (mOptCtx.fps_status == 2) {
                *(int32_t*)(mOptCtx.vpatch_base + 0x40a34) = mOptCtx.fps;
                *(int32_t*)(mOptCtx.vpatch_base + 0x42fcc) = mOptCtx.fps;
            }
        }
        void GameplayInit()
        {
            th15_all_clear_bonus_1.Setup();
            th15_all_clear_bonus_2.Setup();
            th15_all_clear_bonus_3.Setup();
        }
        void GameplaySet()
        {
            th15_all_clear_bonus_1.Toggle(mOptCtx.all_clear_bonus);
            th15_all_clear_bonus_2.Toggle(mOptCtx.all_clear_bonus);
            th15_all_clear_bonus_3.Toggle(mOptCtx.all_clear_bonus);
        }

        THAdvOptWnd() noexcept
        {
            SetMouseOnly();
            SetWndFlag(ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove);
            SetFade(0.8f, 0.8f);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);

            InitUpdFunc([&]() { ContentUpdate(); },
                [&]() { LocaleUpdate(); },
                [&]() {},
                []() {});

            OnLocaleChange();
            FpsInit();
            GameplayInit();
            MasterDisableInit();
            th15_bossmovedown.Setup();
        }


    public:
        __declspec(noinline) static bool StaticUpdate(bool togglePressed)
        {
            auto& advOptWnd = THAdvOptWnd::singleton();

            if (togglePressed) {
                if (advOptWnd.IsOpen())
                    advOptWnd.Close();
                else
                    advOptWnd.Open();
            }
            advOptWnd.Update();

            return advOptWnd.IsOpen();
        }


    protected:
        void LocaleUpdate()
        {
            SetTitle(S(TH_SPELL_PRAC));
            switch (Gui::LocaleGet()) {
            case Gui::LOCALE_ZH_CN:
                SetSizeRel(1.0f, 1.0f);
                SetPosRel(0.0f, 0.0f);
                SetItemWidthRel(-0.0f);
                SetAutoSpacing(true);
                break;
            case Gui::LOCALE_EN_US:
                SetSizeRel(1.0f, 1.0f);
                SetPosRel(0.0f, 0.0f);
                SetItemWidthRel(-0.0f);
                SetAutoSpacing(true);
                break;
            case Gui::LOCALE_JA_JP:
                SetSizeRel(1.0f, 1.0f);
                SetPosRel(0.0f, 0.0f);
                SetItemWidthRel(-0.0f);
                SetAutoSpacing(true);
                break;
            default:
                break;
            }
        }
        void ContentUpdate()
        {
            ImGui::TextUnformatted(S(TH_ADV_OPT));
            ImGui::Separator();
            ImGui::BeginChild("Adv. Options", ImVec2(0.0f, 0.0f));

            if (BeginOptGroup<TH_GAME_SPEED>()) {
                if (GameFPSOpt(mOptCtx))
                    FpsSet();
                EndOptGroup();
            }
            if (BeginOptGroup<TH_GAMEPLAY>()) {
                DisableKeyOpt(nullptr, nullptr, false);
                ImGui::Checkbox(S(THPRAC_INGAMEINFO_TH15_SHOW_SHOOTING_DOWN_RATE), &g_adv_igi_options.th15_showShootingDownRate);
                if (ImGui::Checkbox(S(TH_DISABLE_MASTER), &g_adv_igi_options.disable_master_autoly)) {
                    for (int i = 0; i < 3; i++)
                        th15_master_disable[i].Toggle(g_adv_igi_options.disable_master_autoly);
                }
                ImGui::SameLine();
                HelpMarker(S(TH_DISABLE_MASTER_DESC));
                ImGui::Checkbox(S(TH_ENABLE_LOCK_TIMER), &g_adv_igi_options.enable_lock_timer_autoly);

                KeyHUDOpt();

                InfLifeOpt();
                if (GameplayOpt(mOptCtx))
                    GameplaySet();
                EndOptGroup();
            }
            SSS::SSS_UI(15);
            InGameReactionTestOpt();
            AboutOpt();
            if (ImGui::CollapsingHeader(S(THPRAC_INGAMEINFO_06_SHOWDETAIL_COLLAPSE))) {
                TH15ChapterSave::singleton().ShowChapterRates();
                ImGui::Spacing();
            }
            const char* chapterRecordsLabel = Gui::LocaleGet() == Gui::LOCALE_ZH_CN
                ? "章节目标与记录"
                : Gui::LocaleGet() == Gui::LOCALE_JA_JP
                    ? "チャプター目標と記録"
                    : "Chapter goals and records";
            if (ImGui::CollapsingHeader(chapterRecordsLabel))
                TH15ChapterSave::singleton().ShowChapterRecords();
            ImGui::EndChild();
        }

        adv_opt_ctx mOptCtx;
    };
    class TH15HitboxWnd : public Gui::GameGuiWnd {
        int bossMoveMode = 1;
        bool bossMoveEnabled = false;
        bool showChapterRates = false;
        bool showChapterRecords = false;

        static void RefreshRecording(bool restart = false)
        {
            auto& save = TH15ChapterSave::singleton();
            // Restart sampling at the current chapter when recording options change.
            if (restart) save.SetCaptureRateEnabled(false);
            save.SetCaptureRateEnabled(g_th15CaptureRateEnabled || g_th15ScoreRecordingEnabled || g_th15ChapterHudEnabled);
            g_th15NativeCaptureRateEnabled = g_th15CaptureRateEnabled
                && !g_th15OnlyCurrentPracticeChapterThisRun;
        }

        TH15HitboxWnd() noexcept
        {
            SetMouseOnly();
            SetWndFlag(ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoSavedSettings);
            SetFade(0.8f, 0.8f);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
            TH15MaxPointUncapInit();
            TH15BossMovementInit();
            OnLocaleChange();
        }
        SINGLETON(TH15HitboxWnd)

    public:
        static bool StaticUpdate(bool togglePressed)
        {
            auto& wnd = TH15HitboxWnd::singleton();
            if (togglePressed) {
                if (wnd.IsClosed()) {
                    THAdvOptWnd::singleton().Close();
                    wnd.Open();
                } else {
                    wnd.Close();
                }
            }
            wnd.Update();
            return !wnd.IsClosed();
        }

    protected:
        bool mRemoveMaxPointCap = false;

        void OnLocaleChange() override
        {
            const int locale = Gui::LocaleGet();
            SetTitle(locale == Gui::LOCALE_ZH_CN ? "新增功能设置（F11）"
                : locale == Gui::LOCALE_JA_JP ? "追加機能設定（F11）" : "Added features (F11)");
            SetSizeRel(1.0f, 1.0f);
            SetPosRel(0.0f, 0.0f);
            SetItemWidthRel(-0.0f);
            SetAutoSpacing(true);
        }

        void OnContentUpdate() override
        {
            const int locale = Gui::LocaleGet();
            ImGui::TextUnformatted(locale == Gui::LOCALE_ZH_CN ? "新增功能设置（F11）"
                : locale == Gui::LOCALE_JA_JP ? "追加機能設定（F11）" : "Added features (F11)");
            ImGui::Separator();
            auto label = [locale](const char* zh, const char* ja, const char* en) {
                return locale == Gui::LOCALE_ZH_CN ? zh : locale == Gui::LOCALE_JA_JP ? ja : en;
            };
            static int page = 0;
            page = Gui::FeaturePageSelector(page, true);
            ImGui::PushID(page);
            ImGui::BeginChild("Added features", ImVec2(0.0f, 0.0f), true);
            if (page == 0) {
#if THPRAC_TH15_HITBOX_FEATURE
                const auto hitboxLabel = [locale](const char* zh, const char* ja, const char* en) {
                    return locale == Gui::LOCALE_ZH_CN ? zh : locale == Gui::LOCALE_JA_JP ? ja : en;
                };
                bool hitboxChanged = ImGui::Checkbox(hitboxLabel("显示圆形弹幕判定", "円形弾の判定を表示", "Show circular bullet hitboxes"), &g_th15CircleHitboxEnabled);
                hitboxChanged |= ImGui::Checkbox(hitboxLabel("显示矩形弹幕判定", "矩形弾の判定を表示", "Show rectangular bullet hitboxes"), &g_th15RectHitboxEnabled);
                hitboxChanged |= ImGui::Checkbox(hitboxLabel("显示激光判定", "レーザー判定を表示", "Show laser hitboxes"), &g_th15LaserHitboxEnabled);
                if (hitboxChanged) {
                    g_show_bullet_hitbox = g_th15CircleHitboxEnabled || g_th15RectHitboxEnabled || g_th15LaserHitboxEnabled;
                    TH15SetHitboxCaptureHooks(g_show_bullet_hitbox);
            }
            ImGui::TextDisabled("%s", hitboxLabel("仅在练习模式和 Replay 中显示", "プラクティスとリプレイでのみ表示", "Practice and replay only"));
#endif
            const char* maxPointCapLabel = locale == Gui::LOCALE_ZH_CN
                ? "移除最大得点上限"
                : locale == Gui::LOCALE_JA_JP
                    ? "最大得点の上限を解除"
                    : "Remove max point cap";
            if (ImGui::Checkbox(maxPointCapLabel, &mRemoveMaxPointCap))
                TH15MaxPointUncapSet(mRemoveMaxPointCap);
            ImGui::Separator();


            if (ImGui::Checkbox(label("强制 Boss 横向移动", "ボスの横移動を強制", "Force horizontal boss movement"), &bossMoveEnabled))
                TH15BossMovementSet(bossMoveEnabled ? bossMoveMode : 0);
            if (bossMoveEnabled) {
                const char* modes[] = { S(TH_BOSS_FORCE_MOVE_OPPOSITE_DIR), S(TH_BOSS_FORCE_MOVE_SAME_DIR) };
                int direction = bossMoveMode - 1;
                if (ImGui::Combo(S(TH_BOSS_HORIZONTAL), &direction, modes, 2)) {
                    bossMoveMode = direction + 1;
                    TH15BossMovementSet(bossMoveMode);
                }
            }
            auto& advanced = THAdvOptWnd::singleton();
            if (ImGui::Checkbox(S(TH_BOSS_FORCE_MOVE_DOWN), &advanced.forceBossMoveDown))
                th15_bossmovedown.Toggle(advanced.forceBossMoveDown);
            if (advanced.forceBossMoveDown) {
                ImGui::DragFloat(S(TH_BOSS_FORCE_MOVE_DOWN_RANGE), &g_bossMoveDownRange, 0.002f, 0.0f, 1.0f);
                g_bossMoveDownRange = std::clamp(g_bossMoveDownRange, 0.0f, 1.0f);
            }
            ImGui::Checkbox(label("启用二面第二非符自定义血量（下次练习生效）", "2面通常2のHP変更（次の練習から）", "Custom stage 2 nonspell 2 HP (next practice)"), &g_th15CustomBossHpEnabled);
            ImGui::Separator();
            }
            if (page == 3) {
                if (ImGui::Checkbox(label("记录章节收率", "章の取得率を記録", "Record chapter capture rates"), &g_th15CaptureRateEnabled))
                    RefreshRecording(true);
                if (ImGui::Checkbox(label("记录章节成绩（含 Replay）", "章スコアを記録（リプレイ含む）", "Record chapter scores (including replay)"), &g_th15ScoreRecordingEnabled))
                    RefreshRecording();
                if (ImGui::Checkbox(label("练习仅记录选定章节", "選択した練習章のみ記録", "Record only selected practice chapter"), &g_th15OnlyCurrentPracticeChapter)) {
                    g_th15OnlyCurrentPracticeChapterThisRun = thPracParam.mode == 1 && g_th15OnlyCurrentPracticeChapter;
                    TH15ChapterSave::singleton().SetOnlyCurrentPracticeChapter(g_th15OnlyCurrentPracticeChapterThisRun);
                    RefreshRecording(true);
            }
            }
            if (page == 1) {
                if (ImGui::Checkbox(label("显示章节统计 HUD", "章統計HUDを表示", "Show chapter statistics HUD"), &g_th15ChapterHudEnabled))
                    RefreshRecording();
                TH15ChapterSave::singleton().LoadHudChapterView();
                if (g_th15ChapterHudEnabled) {
                    const char* views[] = { label("通过率", "取得率", "Capture rate"), label("打分与目标", "スコアと目標", "Score and goals") };
                    int view = g_adv_igi_options.th15_hudChapterView;
                    if (ImGui::Combo(label("章节 HUD 模式", "章HUDモード", "Chapter HUD mode"), &view, views, 2))
                        TH15ChapterSave::singleton().SaveHudChapterView(view);
            }
            ImGui::Checkbox(label("显示 Boss 血量", "ボスHPを表示", "Show boss HP"), &g_th15BossHpHudEnabled);
            ImGui::Checkbox(label("显示 Boss 坐标", "ボス座標を表示", "Show boss position"), &g_th15BossPositionHudEnabled);
            ImGui::Checkbox(label("显示自机坐标", "自機座標を表示", "Show player position"), &g_th15PlayerPositionHudEnabled);
            ImGui::Separator();
            }
            if (page == 2) GameVolumeOpt();
            if (page == 3) {
                ImGui::Separator();
                ImGui::Checkbox(label("显示章节收率明细", "章取得率詳細を表示", "Show chapter capture rate details"), &showChapterRates);
                if (showChapterRates) TH15ChapterSave::singleton().ShowChapterRates();
                ImGui::Checkbox(label("显示章节目标与记录", "章目標と記録を表示", "Show chapter goals and records"), &showChapterRecords);
                if (showChapterRecords) TH15ChapterSave::singleton().ShowChapterRecords();
            }
            ImGui::EndChild();
            ImGui::PopID();
        }
    };

    class TH15RouteWnd : public Gui::GameGuiWnd {
        TH15RouteWnd() noexcept
        {
            SetMouseOnly();
            SetWndFlag(ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse
                | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoSavedSettings);
            SetFade(0.85f, 0.85f);
            SetStyle(ImGuiStyleVar_WindowRounding, 0.0f);
            SetStyle(ImGuiStyleVar_WindowBorderSize, 0.0f);
            OnLocaleChange();
        }
        SINGLETON(TH15RouteWnd)

    public:
        static bool StaticUpdate(bool togglePressed)
        {
            auto& wnd = TH15RouteWnd::singleton();
            if (togglePressed) {
                if (wnd.IsClosed()) {
                    THAdvOptWnd::singleton().CloseImmediately();
                    TH15HitboxWnd::singleton().CloseImmediately();
                    TH15RouteTrace::singleton().Refresh();
                    wnd.Open();
                } else {
                    wnd.Close();
                    TH15RouteTrace::singleton().SaveSettings();
                }
            }
            wnd.Update();
            return !wnd.IsClosed();
        }

    protected:
        static void WideToUtf8(const std::wstring& src, char* out, int outSize)
        {
            WideCharToMultiByte(CP_UTF8, 0, src.c_str(), -1, out, outSize, nullptr, nullptr);
        }

        void OnLocaleChange() override
        {
            const int locale = Gui::LocaleGet();
            SetTitle(locale == Gui::LOCALE_ZH_CN ? "Route reference (F10)"
                : locale == Gui::LOCALE_JA_JP ? "ルート参考（F10）" : "Route reference (F10)");
            SetSizeRel(1.0f, 1.0f);
            SetPosRel(0.0f, 0.0f);
            SetItemWidthRel(-0.0f);
            SetAutoSpacing(true);
        }

        void OnContentUpdate() override
        {
            auto& trace = TH15RouteTrace::singleton();
            trace.Init();
            const int locale = Gui::LocaleGet();
            const auto label = [locale](const char* zh, const char* ja, const char* en) {
                return locale == Gui::LOCALE_ZH_CN ? zh : locale == Gui::LOCALE_JA_JP ? ja : en;
            };

            ImGui::TextUnformatted(label("路线参考（F10）", "ルート参考（F10）", "Route reference (F10)"));
            ImGui::Separator();
            ImGui::BeginChild("##th15_route_body", ImVec2(0.0f, 0.0f));

            if (ImGui::CollapsingHeader(label("显示设置", "表示設定", "Display"), ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox(label("显示参考路线", "参考ルートを表示", "Show reference route"), &trace.displayEnabled);
                ImGui::SliderFloat(label("透明度", "不透明度", "Opacity"), &trace.displayAlpha, 0.05f, 1.0f, "%.2f");
                ImGui::SliderFloat(label("线宽", "線幅", "Line width"), &trace.displayThickness, 0.5f, 6.0f, "%.2f");
                ImGui::ColorEdit4(label("颜色", "色", "Color"), &trace.displayColor.x,
                    ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
                ImGui::Checkbox(label("标记起点与终点", "始点と終点を表示", "Mark start and end"), &trace.displayMarkers);
                ImGui::Checkbox(label("显示路线名称", "ルート名を表示", "Show route name"), &trace.displayLabel);
                ImGui::Checkbox(label("自动匹配当前面数/难度/机体", "ステージ/難易度/自機で自動選択",
                    "Match current stage/difficulty/shot"), &trace.autoMatch);
                ImGui::Checkbox(label("只显示当前进度附近的一段", "現在の進行度付近のみ表示",
                    "Show only the stretch near current progress"), &trace.windowEnabled);
                if (trace.windowEnabled) {
                    ImGui::SliderFloat(label("回看（秒）", "巻き戻し（秒）", "Trail (s)"),
                        &trace.windowBackSeconds, 0.0f, 10.0f, "%.1f");
                    ImGui::SliderFloat(label("预览（秒）", "先読み（秒）", "Look ahead (s)"),
                        &trace.windowAheadSeconds, 0.0f, 10.0f, "%.1f");
                    ImGui::TextDisabled("%s", label(
                        "以章节为单位播放：进入新章节即从该章起点重新对齐，不会跨到下一章；暂停时随游戏停住。旧格式轨迹按关卡帧数近似。",
                        "チャプター単位で再生します。章が変わるとその章の先頭に合わせ、次の章へはみ出しません。ポーズ中は停止します。",
                        "Plays per chapter: entering a chapter re-anchors to its start and never spills into the next one. Stops while paused. Older routes are approximated by stage frames."));
                }
                ImGui::TextDisabled("%s", label(
                    "练习跳转：已核对的段落按脚本时序对齐；其余段落暂按章节标记近似。练习自机初始位置不用于定位录像进度。",
                    "検証済み区間はスクリプト時刻、他は章マークで近似します。自機の初期位置は同期に使いません。",
                    "Verified targets use script timing; others approximate from chapter marks. Practice spawn position does not set replay progress."));
                ImGui::SliderInt(label("练习跳转补偿（帧）", "プラクティス移行の補正",
                    "Practice jump offset (frames)"), &trace.practiceSkipAdjust, -120, 240);
                int scriptStart = 0;
                if (trace.PracticeScriptStart(thPracParam.section, scriptStart)) {
                    const int offset = trace.PracticeStartSample(thPracParam.section) - trace.practiceAnchorSample;
                    ImGui::TextDisabled("%s %+d (%.2f s)", label("脚本起播偏移（含手动，帧）",
                        "スクリプト開始補正（手動込み、フレーム）", "Script playback offset (including manual, frames)"),
                        offset, offset / 60.0f);
                    const auto* clock = FindTH15RouteClock(thPracParam.section, thPracParam.phase,
                        GetMemContent<int32_t>(0x4E7410));
                    if (clock && clock->mode == 3)
                        ImGui::TextDisabled("%s", label("无限练习时，参考路线随对应脚本片段循环。",
                            "無限練習では該当区間のルートを繰り返します。",
                            "Infinite practice repeats the matching route segment."));
                } else if (trace.practiceAnchorValid && !trace.practiceAnchorFollowed) {
                    ImGui::TextDisabled("%s", label("本段暂按章节标记近似，尚未校准脚本时序。",
                        "この区間は章マークによる近似です。", "This target uses approximate chapter timing."));
                }
                if (ImGui::Button(label("重置手动补偿", "手動補正をリセット", "Reset manual offset")))
                    trace.practiceSkipAdjust = 0;
                if (trace.practiceMeasureValid) {
                    ImGui::Text("%s %+d", label("章节时长差（仅供参考，帧）", "章の所要時間差（参考、フレーム）",
                        "Chapter duration difference (diagnostic frames)"), trace.practiceMeasuredResidual);
                }
                ImGui::Checkbox(label("测试：自机沿参考路线移动（覆盖操作）",
                    "テスト：自機を参考ルートに沿って移動", "Test: move the player along the route"),
                    &trace.followPlayerToRoute);
                if (trace.followPlayerToRoute) {
                    ImGui::TextDisabled("%s", label(
                        "每帧用路线采样坐标覆盖自机位置，用于验证固定路线能否躲过固定弹幕；关闭后立即恢复操作。",
                        "毎フレーム参考ルートの座標で自機位置を上書きします。固定パターンの検証用です。",
                        "Overwrites the player position with the route samples every frame to check a fixed dodge; turn it off to take control back."));
                }
                if (trace.practiceAnchorValid) {
                    ImGui::Text("%s", label("已按练习跳转对齐", "プラクティスに合わせました",
                        "Aligned to the practice jump"));
                }
            }

            if (ImGui::CollapsingHeader(label("从录像提取轨迹", "リプレイから抽出", "Extract from replay"),
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox(label("播放录像时自动提取", "リプレイ再生時に自動抽出",
                    "Extract automatically while a replay plays"), &trace.autoExtractReplay);
                ImGui::TextWrapped("%s", label(
                    "在游戏中播放选定的录像，工具会在播放期间逐帧记录自机位置，切换面数时自动分段保存。",
                    "ゲーム内で選択したリプレイを再生すると、再生中の自機位置を毎フレーム記録し、ステージごとに保存します。",
                    "Play the selected replay in game: the player position is sampled every frame and saved per stage."));

                if (trace.replayFiles.empty()) {
                    ImGui::TextDisabled("%s", label("未找到录像文件（%APPDATA%\\ShanghaiAlice\\th15\\replay）",
                        "リプレイが見つかりません", "No replay files found"));
                } else {
                    ImGui::BeginChild("##th15_route_replays", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f));
                    for (int i = 0; i < (int)trace.replayFiles.size(); ++i) {
                        char name[512];
                        WideToUtf8(trace.replayFiles[i], name, sizeof(name));
                        if (ImGui::Selectable(name, trace.replaySelection == i))
                            trace.replaySelection = i;
                    }
                    ImGui::EndChild();
                }

                if (ImGui::Button(label("刷新录像列表", "リプレイ一覧を更新", "Refresh list")))
                    trace.ScanReplays();
                ImGui::SameLine();
                if (ImGui::Button(label("提取所选录像", "選択したリプレイを抽出", "Extract selected replay"))) {
                    if (trace.replaySelection >= 0 && trace.replaySelection < (int)trace.replayFiles.size())
                        trace.ArmReplay(trace.replayFiles[trace.replaySelection]);
                }
                if (trace.replaySelection < 0 && !trace.replayFiles.empty())
                    ImGui::TextDisabled("%s", label("请先在上方选择一个录像文件", "上でリプレイを選択してください", "Select a replay above first"));
                if (trace.extractArmed) {
                    ImGui::SameLine();
                    if (ImGui::Button(label("取消等待", "待機を解除", "Cancel waiting")))
                        trace.extractArmed = false;
                }
                if (trace.capturing) {
                    ImGui::Text("%s", label("正在提取：%s  面数 %d  点数 %d", "抽出中：%s  ステージ %d  点数 %d",
                        "Extracting: %s  stage %d  points %d"),
                        trace.captureLabel.c_str(), trace.captureStage, (int)trace.capturePointCount);
                } else if (trace.extractArmed) {
                    char name[512];
                    WideToUtf8(trace.armedReplay, name, sizeof(name));
                    ImGui::Text("%s", label("等待播放：%s", "再生待ち：%s", "Waiting for: %s"), name);
                } else {
                    ImGui::TextDisabled("%s", label("未在提取", "抽出していません", "Not extracting"));
                }
            }

            if (ImGui::CollapsingHeader(label("已保存路线", "保存済みルート", "Saved routes"),
                    ImGuiTreeNodeFlags_DefaultOpen)) {
                if (trace.routes.empty()) {
                    ImGui::TextDisabled("%s", label("暂无已保存路线", "保存済みルートはありません", "No saved routes"));
                } else {
                    ImGui::BeginChild("##th15_route_table", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 10.0f));
                    for (int i = 0; i < (int)trace.routes.size(); ++i) {
                        auto& route = trace.routes[i];
                        char text[512];
                        snprintf(text, sizeof(text), "%s: S%d  %s / %s    %s %d",
                            route.label.c_str(), route.stage + 1, route.stamp.c_str(), route.file.c_str(),
                            label("点数", "点数", "Points"), route.points);
                        if (ImGui::Selectable(text, trace.selectedRoute == i))
                            trace.selectedRoute = i;
                    }
                    ImGui::EndChild();
                }

                if (ImGui::Button(label("刷新列表", "一覧を更新", "Refresh")))
                    trace.ScanRoutes();
                ImGui::SameLine();
                if (ImGui::Button(label("取消参考", "参考を解除", "Clear reference")))
                    trace.selectedRoute = -1;
                ImGui::SameLine();
                if (trace.selectedRoute >= 0 && trace.selectedRoute < (int)trace.routes.size()) {
                    if (ImGui::Button(label("删除选中路线", "選択ルートを削除", "Delete selected route")))
                        trace.pendingDelete = trace.routes[trace.selectedRoute].file;
                }
                if (!trace.pendingDelete.empty()) {
                    ImGui::Text("%s", label("删除这条路线记录？", "このルートを削除しますか？", "Delete this route?"));
                    ImGui::SameLine();
                    if (ImGui::Button(label("删除", "削除", "Delete"))) {
                        trace.DeleteRoute(trace.pendingDelete);
                        trace.pendingDelete.clear();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(label("取消", "キャンセル", "Cancel")))
                        trace.pendingDelete.clear();
                }
                ImGui::TextDisabled("%s", label(
                    "轨迹按面数、难度和机体保存到 %APPDATA%\\ShanghaiAlice\\th15\\thprac_route。游玩时若存在匹配的路线，会以折线叠加在游戏画面上；录像中自机复活或瞬移会断开折线。",
                    "ルートはステージ・難易度・自機ごとに %APPDATA%\\ShanghaiAlice\\th15\\thprac_route に保存されます。プレイ中に対応するルートがある場合、折れ線として重ねて表示します。",
                    "Routes are stored per stage, difficulty, and shot under %APPDATA%\\ShanghaiAlice\\th15\\thprac_route and drawn as a polyline over the game."));
            }

            ImGui::EndChild();
        }
    };


    static int32_t chapter_set = -1;
    // TH15's own spell history has separate per-card and aggregate counters.
    // Skip only the increment/store pairs when capture-rate recording is off.
    EHOOK_ST(th15_native_spell_attempt, 0x42053b, 1, {
        if (!g_th15NativeCaptureRateEnabled) {
            ++pCtx->Eax;
            pCtx->Eip = 0x420543;
        }
    });
    EHOOK_ST(th15_native_spell_attempt_total, 0x420585, 1, {
        if (!g_th15NativeCaptureRateEnabled) {
            ++pCtx->Eax;
            pCtx->Eip = 0x42058d;
        }
    });
    EHOOK_ST(th15_native_spell_capture, 0x420e3a, 1, {
        if (!g_th15NativeCaptureRateEnabled) {
            ++pCtx->Eax;
            pCtx->Eip = 0x420e3d;
        }
    });
    EHOOK_ST(th15_native_spell_capture_total, 0x420e58, 1, {
        if (!g_th15NativeCaptureRateEnabled) {
            ++pCtx->Eax;
            pCtx->Eip = 0x420e5b;
        }
    });

    EHOOK_ST(th15_chapter_set, 0x43dd58, 6, {
        if (chapter_set != -1) {
            *((int32_t*)0x4e73f8) = chapter_set;
            chapter_set = -1;
        }
        TH15ChapterSave::singleton().OnChapterIndexChanged(
            *(int32_t*)0x4E73F0, *(int32_t*)0x4E73F8,
            TH15InGameInfo::singleton().mMissCount,
            TH15InGameInfo::singleton().mBombCount,
            thPracParam.mode,
            *(int32_t*)0x4E7410, *(int32_t*)0x4E7404);
    });
    // The chapter-change routine clears the bullet manager at 0x43de11.
    // Resolve a queued capture after that clear and before the next chapter runs.
    EHOOK_ST(th15_chapter_bullets_cleared, 0x43de19, 5, {
        TH15ChapterSave::singleton().OnBulletClearFinished(
            TH15InGameInfo::singleton().mMissCount,
            TH15InGameInfo::singleton().mBombCount);
    });

    static int32_t chapter_disable = 0;
    EHOOK_ST(th15_chapter_disable, 0x43d0b5, 2, {
        if (chapter_disable > 0) {
            --chapter_disable;
            pCtx->Eip = 0x43d0d5;
        }
    });
    // This call creates the chapter-settlement event. The preceding test at
    // 0x43d0b5 only decides whether the event should be skipped; it also has a
    // zero-value path that is not a clear. Record success at the actual call.
    EHOOK_ST(th15_chapter_settlement, 0x43d0d0, 5, {
        TH15ChapterSave::singleton().OnChapterClear(
            TH15InGameInfo::singleton().mMissCount,
            TH15InGameInfo::singleton().mBombCount);
    });
    EHOOK_ST(th15_st7boss1_chapter_bonus, 0x43dece, 3, {
        self->Disable();
        *(uint32_t*)0x4e7484 = 1;
    });
    EHOOK_ST(th15_stars_bgm_sync, 0x48c294, 2, {
        int32_t call_addr = *(int32_t*)(pCtx->Esp + 0x8);

        if (thPracParam.mode == 1 && thPracParam.section == TH15_ST6_STARS) {
            if (call_addr == 0x48B4EB) {
                *(uint32_t*)(pCtx->Esp + 0x10) = 0x8fc768;
                self->Disable();
            }
        }
    });
    void ECLSetChapter(size_t chapter)
    {
        chapter_set = chapter;
        th15_chapter_set.Enable();
    }
    void ECLSkipChapter(size_t times)
    {
        chapter_disable = (int32_t)times;
        th15_chapter_disable.Enable();
    }
    void ECLStarsBGMSync()
    {
        th15_stars_bgm_sync.Enable();
    }
    void ECLJump(ECLHelper& ecl, unsigned int start, unsigned int dest, int at_frame, int ecl_time = 0)
    {
        ecl.SetPos(start);
        g_th15WarpWaitFrames += ecl_time;
        ecl << ecl_time << 0x0018000C << 0x02ff0000 << 0x00000000 << dest - start << at_frame;
    }

    constexpr unsigned int st7Start = 0x8f6c;
    __declspec(noinline) void THStageWarp(ECLHelper& ecl, int stage, int portion)
    {
        if (stage == 1) {
            switch (portion) {
            case 1:
                break;
            case 2:
                ECLJump(ecl, 0x7248, 0x7494, 60, 90); // 0x7544
                ECLJump(ecl, 0x3f4c, 0x3fa8, 0, 0);
                break;
            case 3:
                ECLJump(ecl, 0x7248, 0x7544, 60, 90);
                ECLSetChapter(2);
                break;
            case 4:
                ECLJump(ecl, 0x7248, 0x7544, 60, 90);
                ECLJump(ecl, 0x4098, 0x40fc, 0, 0);
                break;
            case 5:
                ECLJump(ecl, 0x7248, 0x75f8, 60, 90);
                ECLSetChapter(5);
                break;
            case 6:
                ECLJump(ecl, 0x7248, 0x75f8, 60, 90);
                ECLJump(ecl, 0x4150, 0x41b8, 0, 0);
                break;
            default:
                break;
            }
        } else if (stage == 2) {
            switch (portion) {
            case 1:
                break;
            case 2:
                ECLJump(ecl, 0x74a0, 0x76ec, 60, 90); // 0x778c
                ECLJump(ecl, 0x42b4, 0x4354, 0, 0);
                break;
            case 3:
                ECLJump(ecl, 0x74a0, 0x76ec, 60, 90); // 0x778c
                ECLJump(ecl, 0x42b4, 0x43d4, 0, 0);
                break;
            case 4:
                ECLJump(ecl, 0x74a0, 0x778c, 60, 29);
                ECLSetChapter(4);
                break;
            case 5:
                ECLJump(ecl, 0x74a0, 0x778c, 60, 0);
                ECLJump(ecl, 0x443c, 0x448c, 0, 0);
                break;
            case 6:
                ECLJump(ecl, 0x74a0, 0x778c, 60, 90);
                ECLJump(ecl, 0x443c, 0x450c, 0, 0);
                break;
            default:
                break;
            }
        } else if (stage == 3) {
            switch (portion) {
            case 1:
                break;
            case 2:
                ECLJump(ecl, 0xa280, 0xa4cc, 60, 90); // 0xa56c
                ECLJump(ecl, 0x5b6c, 0x5c00, 0, 0);
                break;
            case 3:
                ECLJump(ecl, 0xa280, 0xa4cc, 60, 30); // 0xa56c
                ECLJump(ecl, 0x5b6c, 0x5c6c, 0, 0);
                if (thPracParam.phase == 1) {
                    ecl << pair { 0x71D8, (int32_t)30 };
                    ECLJump(ecl, 0x71DC, 0x7468, 0, 0);
                }
                break;
            case 4:
                ECLJump(ecl, 0xa280, 0xa56c, 60, 50);
                ECLSetChapter(4);
                break;
            case 5:
                ECLJump(ecl, 0xa280, 0xa56c, 60, 20);
                ECLJump(ecl, 0x5d0c, 0x5d70, 0, 0);
                break;
            case 6:
                ECLJump(ecl, 0xa280, 0xa56c, 60, 90);
                ECLJump(ecl, 0x5d0c, 0x5df0, 0, 0);
                break;
            default:
                break;
            }
        } else if (stage == 4) {
            switch (portion) {
            case 1:
                break;
            case 2:
                ECLJump(ecl, 0x8634, 0x8880, 60, 90); // 0x8930
                ECLJump(ecl, 0x406c, 0x40ec, 0, 0);
                if (thPracParam.phase == 1) {
                    ecl << pair { 0x4DD0, (int32_t)0 };
                    ECLJump(ecl, 0x485C, 0x4DC0, 0, 0);
                }
                break;
            case 3:
                ECLJump(ecl, 0x8634, 0x8880, 60, 90); // 0x8930
                ECLJump(ecl, 0x406c, 0x4158, 0, 0);
                ecl << pair{0x5cc4, 0};
                break;
            case 4:
                ECLJump(ecl, 0x8634, 0x8930, 60, 90);
                ecl << pair{0x6bc0, 0};
                break;
            case 5:
                ECLJump(ecl, 0x8634, 0x8930, 60, 90);
                ECLJump(ecl, 0x41e0, 0x4244, 0, 0);
                ecl << pair{0x6e74, 0};
                break;
            case 6:
                ECLJump(ecl, 0x8634, 0x8930, 60, 90);
                ECLJump(ecl, 0x41e0, 0x42c4, 0, 0);
                break;
            default:
                break;
            }
        } else if (stage == 5) {
            switch (portion) {
            case 1:
                break;
            case 2:
                ECLJump(ecl, 0x9f34, 0xa180, 60, 90); // 0xa1a0
                ECLJump(ecl, 0x6ed8, 0x6f58, 0, 0);
                break;
            case 3:
                ECLJump(ecl, 0x9f34, 0xa180, 60, 90); // 0xa1a0
                ECLJump(ecl, 0x6ed8, 0x7040, 0, 0);
                ecl << pair{0x7ba0, 0};
                break;
            case 4:
                ECLJump(ecl, 0x9f34, 0xa180, 60, 90); // 0xa1a0
                ECLJump(ecl, 0x6ed8, 0x7098, 0, 0);
                break;
            case 5:
                ECLJump(ecl, 0x9f34, 0xa1a0, 60, 90);
                ecl << pair{0x70f0, (int16_t)0};
                break;
            case 6:
                ECLJump(ecl, 0x9f34, 0xa1a0, 60, 90);
                ECLJump(ecl, 0x70ec, 0x716c, 0, 0);
                ecl << pair{0x81c0, 0};
                break;
            case 7:
                ECLJump(ecl, 0x9f34, 0xa1a0, 60, 90);
                ECLJump(ecl, 0x70ec, 0x71ec, 0, 0);
                ecl << pair{0x845c, 0};
                break;
            case 8:
                switch (thPracParam.phase)
                {
                default:
                case 0:
                    break;
                case 1:
                    ECLJump(ecl, 0x89C0, 0x8824, 0, 0);
                    break;
                case 2:
                    ECLJump(ecl, 0x880C, 0x8A00, 0, 0);
                    ECLJump(ecl, 0x8C78, 0x8A40, 0, 0);
                    break;
                case 3:
                    ECLJump(ecl, 0x880C, 0x8A00, 0, 0);
                    break;
                }
                ECLJump(ecl, 0x9f34, 0xa1a0, 60, 90);
                ECLJump(ecl, 0x70ec, 0x726c, 0, 0);
                ecl << pair{0x87e0, 0};
                break;
            case 9:
                ECLJump(ecl, 0x9f34, 0xa1a0, 60, 90);
                ECLJump(ecl, 0x70ec, 0x72ec, 0, 0);
                ecl << pair{0x8ce8, 0};
                break;
            case 10:
                if (thPracParam.phase == 1) {
                    ECLJump(ecl, 0x9CE0, 0x9ABC, 0, 0);
                }
                ECLJump(ecl, 0x9f34, 0xa1a0, 60, 90);
                ECLJump(ecl, 0x70ec, 0x736c, 0, 0);
                ecl << pair{0x9a50, 0};
                break;
            default:
                break;
            }
        } else if (stage == 6) {
            switch (portion) {
            case 1:
                break;
            case 2:
                ECLJump(ecl, 0x91f8, 0x93e0, 60, 90);
                ECLJump(ecl, 0x2b34, 0x2be8, 0, 0);
                ecl << pair{0x38d0, 0};
                break;
            case 3:
                ECLJump(ecl, 0x91f8, 0x93e0, 60, 90);
                ECLJump(ecl, 0x2b34, 0x2c68, 0, 0);
                ecl << pair{0x40ac, 0};
                break;
            case 4:
                ECLJump(ecl, 0x91f8, 0x93e0, 60, 90);
                ECLJump(ecl, 0x2b34, 0x2ce8, 0, 0);
                ecl << pair{0x43b0, 0};
                break;
            default:
                break;
            }
        } else if (stage == 7) {
            switch (portion) {
            case 1:
                break;
            case 2:
                ECLJump(ecl, 0x8f94, 0x9208, 60, 90); // 0x9338
                ECLJump(ecl, 0x5d08, 0x5d88, 0, 0);
                break;
            case 3:
                ECLJump(ecl, 0x8f94, 0x9208, 60, 90); // 0x9338
                ECLJump(ecl, 0x5d08, 0x5e08, 0, 0);
                break;
            case 4:
                ECLJump(ecl, 0x8f94, 0x9208, 60, 90); // 0x9338
                ECLJump(ecl, 0x5d08, 0x5e9c, 0, 0);
                break;
            case 5:
                ECLJump(ecl, 0x8f94, 0x9208, 60, 90); // 0x9338
                ECLJump(ecl, 0x5d08, 0x5f30, 0, 0);
                break;
            case 6:
                ECLJump(ecl, 0x8f94, 0x9338, 60, 90);
                break;
            case 7:
                ECLJump(ecl, 0x8f94, 0x9338, 60, 10);
                ECLJump(ecl, 0x5f84, 0x6018, 0, 0);
                break;
            case 8:
                ECLJump(ecl, 0x8f94, 0x9338, 60, 90);
                ECLJump(ecl, 0x5f84, 0x60c0, 0, 0);
                break;
            case 9:
                ECLJump(ecl, 0x8f94, 0x9338, 60, 90);
                ECLJump(ecl, 0x5f84, 0x6154, 0, 0);
                break;
            default:
                break;
            }
        }
    }
    __declspec(noinline) void THPatch(ECLHelper& ecl, th_sections_t section)
    {
        auto st7_hide_subboss = [&]() {
            ecl.SetFile(4);
            // Void Particle Effect & Wait
            ecl << pair{0x190, (int16_t)0} << pair{0x1dc, (int16_t)0} << pair{0x228, (int16_t)0};
            // Jump Through Dialogue
            ECLJump(ecl, 0x2a0, 0x2f4, 5);
            // Remove Movement Restriction & Move Directly
            ecl.SetPos(0x480);
            ecl << 0 << 0x001001f9 << 0x00ff0000 << 0
                << 0 << 0x00340190 << 0x02ff0000 << 0 << 0x43400000 << 0xc2800000;
        };
        auto st7_enter_spell = [&](int ordinal, int health, bool is_junko = false) {
            ecl.SetFile(3);
            ecl.SetPos(0x6d0);
            ecl << 0 << 0x001401ff << 0x01ff0000 << 0 << health;
            if (is_junko) {
                ecl << 0 << 0x0014012e << 0x01ff0000 << 0 << 5
                    << 0 << 0x0018012f << 0x02ff0000 << 0 << 3 << 6;
                ecl << 0 << 0x0020000f << 0x01ff0000 << 0 << 0xc
                    << 0x73736f42 << 0x6f705f34 << 0x00000073; // "Boss", "4_po", "s"
            }
            ecl << 0 << 0x0020000b << 0x01ff0000 << 0 << 0xc
                << 0x73736f42 << 0x64726143 << ordinal;
        };
        constexpr unsigned int st7BossCreateCall = 0x93cc;
        constexpr unsigned int st7bsMovePosX = 0x498 + 0x10; // 0x4a8
        constexpr unsigned int st7bsMovePosY = 0x498 + 0x14; // 0x4ac
        constexpr unsigned int st7bsANMSelect = 0x56c + 0x10; // 0x57c
        constexpr unsigned int st7bsANMSetMainArg2 = 0x580 + 0x14; // 0x594
        constexpr unsigned int st7bsSetSpriteIns = 0x5d4 + 0x4; // 0x5d8
        constexpr unsigned int st7bsANMSetSprite1Arg2 = 0x674 + 0x14; // 0x688
        constexpr unsigned int st7bsANMSetSprite2Arg2 = 0x68c + 0x14; // 0x6a0
        constexpr unsigned int st7bsMoveLimitIns = 0x640 + 0x4; // 0x644

        switch (section) {
        case THPrac::TH15::TH15_ST1_MID1:
            ECLJump(ecl, 0x7220, 0x74d8, 60);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST1_MID2:
            ECLJump(ecl, 0x7220, 0x758c, 60);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST1_MID3:
            ECLJump(ecl, 0x7220, 0x75b4, 60);
            ecl.SetFile(4);
            ECLSkipChapter(1);
            ECLJump(ecl, 0x444, 0x560, 0); // Utilize Spell Practice Jump
            ecl << pair{0x434, 60} << pair{0x570, 60}; // Move Speed
            ecl << pair{0x588, 9999}; // Do not use spellcard
            ecl.SetPos(0x384);
            ecl << 0 << 0x001c0203 << 0x01ff0000 << 0 << 60; // Muteki
            break;
        case THPrac::TH15::TH15_ST1_BOSS1:
            if (thPracParam.dlg)
                ECLJump(ecl, 0x7220, 0x7678, 60);
            else
                ECLJump(ecl, 0x7220, 0x768c, 60);
            ecl.SetFile(2);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST1_BOSS2:
            ECLJump(ecl, 0x7220, 0x768c, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x3b8, 0x4a0, 1); // Utilize Spell Practice Jump
            ecl << pair{0x4b0, 1700}; // Set Health
            ecl << pair{0x4d0, (int8_t)0x31}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST1_BOSS3:
            ECLJump(ecl, 0x7220, 0x768c, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x5d0, (int8_t)0x32}; // Change Nonspell
            ecl << pair{0x13ec, (int16_t)0} << pair{0x151c, (int16_t)0}; // Disable Item Drops & SE
            break;
        case THPrac::TH15::TH15_ST1_BOSS4:
            ECLJump(ecl, 0x7220, 0x768c, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x3b8, 0x4a0, 1); // Utilize Spell Practice Jump
            ecl << pair{0x4b0, 1900}; // Set Health
            ecl << pair{0x4d0, (int8_t)0x32}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST2_MID1:
            ECLJump(ecl, 0x7444, 0x7720, 60);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST2_BOSS1:
            if (thPracParam.dlg)
                ECLJump(ecl, 0x7444, 0x7820, 60);
            else
                ECLJump(ecl, 0x7444, 0x7834, 60);
            ecl.SetFile(2);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST2_BOSS2:
            ECLJump(ecl, 0x7444, 0x7834, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x40c, 0x4f4, 1); // Utilize Spell Practice Jump
            ecl << pair{0x504, 2200}; // Set Health
            ecl << pair{0x524, (int8_t)0x31}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST2_BOSS3:
            ECLJump(ecl, 0x7444, 0x7834, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            {
                // Keep the initial boss lifeSet at its original 14200 so the
                // preceding section can finish. The next lifeSet (originally
                // 13400) initializes the second nonspell's remaining HP.
                const int hp = g_th15CustomBossHpEnabled
                    ? std::clamp(thPracParam.st2_boss2_hp, 0, kTh15St2Boss3EffectiveHpMax)
                    : kTh15St2Boss3EffectiveHpMax;
                const int gameHp = hp + kTh15St2Boss3HpSwitchThreshold;
                const uint32_t bossLifeBefore = TH15ECLReadU32(ecl, 0x30c);
                const uint32_t boss2LifeBefore = TH15ECLReadU32(ecl, 0x1134);
                ecl << pair{0x1134, gameHp};
                TH15BossHpLog("ecl-bosshp", hp,
                    bossLifeBefore, TH15ECLReadU32(ecl, 0x30c),
                    boss2LifeBefore, TH15ECLReadU32(ecl, 0x1134));
            }
            ecl << pair{0x644, (int8_t)0x32}; // Change Nonspell
            ecl << pair{0x1214, (int16_t)0} << pair{0x1344, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x14bc, 59} << pair{0x1508, 0} << pair{0x1120, 60}; // Change Move Time, Wait Time & Inv. Time
            break;
        case THPrac::TH15::TH15_ST2_BOSS4:
            ECLJump(ecl, 0x7444, 0x7834, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x40c, 0x4f4, 1); // Utilize Spell Practice Jump
            ecl << pair{0x504, kTh15St2Boss3HpSwitchThreshold}; // Set Health
            ecl << pair{0x524, (int8_t)0x32}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST2_BOSS5:
            ECLJump(ecl, 0x7444, 0x7834, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x644, (int8_t)0x33}; // Change Nonspell
            ecl << pair{0x1ea8, (int16_t)0} << pair{0x1fd8, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x2150, 59} << pair{0x219c, 0} << pair{0x1db4, 60}; // Change Move Time, Wait Time & Inv. Time
            break;
        case THPrac::TH15::TH15_ST2_BOSS6:
            ECLJump(ecl, 0x7444, 0x7834, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x40c, 0x4f4, 1); // Utilize Spell Practice Jump
            ecl << pair{0x504, 2500}; // Set Health
            ecl << pair{0x524, (int8_t)0x33}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST3_MID1:
            ECLJump(ecl, 0xa258, 0xa500, 60);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST3_MID2:
            ECLJump(ecl, 0xa258, 0xa528, 60);
            ecl.SetFile(3);
            ECLSkipChapter(1);
            ECLJump(ecl, 0x360, 0x47c, 0); // Utilize Spell Practice Jump
            ecl << pair{0x350, 60} << pair{0x48c, 60}; // Move Speed
            ecl << pair{0x4a4, 9999}; // Do not use spellcard
            ecl.SetPos(0x2a4);
            ecl << 0 << 0x001c0203 << 0x01ff0000 << 0 << 60; // Muteki
            break;
        case THPrac::TH15::TH15_ST3_BOSS1:
            if (thPracParam.dlg)
                ECLJump(ecl, 0xa258, 0xa600, 60);
            else
                ECLJump(ecl, 0xa258, 0xa614, 60);
            ecl.SetFile(2);
            ECLSkipChapter(1);
            // wave2
            switch (thPracParam.phase) {
            case 0:
                break;
            case 1:
                ECLJump(ecl, 0x7D4, 0x898, 0);
                break;
            case 2:
                ecl << pair { 0xD3C, thPracParam.doremy_normal_1_phase };
                ecl << pair { 0xD34, (BYTE)0x00 };
                break;
            case 3:
                ecl << pair { 0xD3C, thPracParam.doremy_normal_1_phase };
                ecl << pair { 0xD34, (BYTE)0x00 };
                ECLJump(ecl, 0x7D4, 0x898, 0);
                break;
            default:
                break;
            }
            //---
            break;
        case THPrac::TH15::TH15_ST8_AB_TEST:
            ECLJump(ecl, 0xa258, 0xa614, 60);
            ecl.SetFile(2);
            ECLSkipChapter(1);
            ecl << pair { 0x23C, (int8_t)0 };
            ecl << pair { 0x288, (int8_t)0 };
            ecl << pair { 0x2D4, (int8_t)0 };
            for (int i = 0; i < sizeof(th15_abtest_ecl_file);i++)
                ecl << pair { 0x694 + i, th15_abtest_ecl_file[i] };
            ecl << pair { 0x694 + (0x66C - 0x5E8), (int32_t)thPracParam.phase + 1 };
            //---
            break;
        case THPrac::TH15::TH15_ST3_BOSS2:
            ECLJump(ecl, 0xa258, 0xa614, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x448, 0x530, 1); // Utilize Spell Practice Jump
            ecl << pair{0x540, 2000}; // Set Health
            ecl << pair{0x560, (int8_t)0x31}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST3_BOSS3:
            ECLJump(ecl, 0xa258, 0xa614, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x680, (int8_t)0x32}; // Change Nonspell
            ecl << pair{0x15d8, (int16_t)0} << pair{0x1708, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x18ac, 0} << pair{0x14e4, 60}; // Change Wait Time & Inv. Time
            break;
        case THPrac::TH15::TH15_ST3_BOSS4:
            ECLJump(ecl, 0xa258, 0xa614, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x448, 0x530, 1); // Utilize Spell Practice Jump
            ecl << pair{0x540, 2100}; // Set Health
            ecl << pair{0x560, (int8_t)0x32}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST3_BOSS5:
            ECLJump(ecl, 0xa258, 0xa614, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x680, (int8_t)0x33}; // Change Nonspell
            ecl << pair{0x27b0, (int16_t)0} << pair{0x28e0, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x2a98, 0} << pair{0x2a58, 60}; // Change Wait Time & Inv. Time
            break;
        case THPrac::TH15::TH15_ST3_BOSS6:
            ECLJump(ecl, 0xa258, 0xa614, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x448, 0x530, 1); // Utilize Spell Practice Jump
            ecl << pair{0x540, 2500}; // Set Health
            ecl << pair{0x560, (int8_t)0x33}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST3_BOSS7:
            ECLJump(ecl, 0xa258, 0xa614, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x448, 0x530, 1); // Utilize Spell Practice Jump
            ecl << pair{0x540, 3000}; // Set Health
            ecl << pair{0x560, (int8_t)0x34}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST4_MID1:
            ECLJump(ecl, 0x860c, 0x88ec, 60);
            // Let the midboss clear reach the game's chapter settlement.
            break;
        case THPrac::TH15::TH15_ST4_BOSS1:
            if (thPracParam.dlg)
                ECLJump(ecl, 0x860c, 0x899c, 60);
            else
                ECLJump(ecl, 0x860c, 0x89b0, 60);
            ecl.SetFile(2);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST4_BOSS2:
            ECLJump(ecl, 0x860c, 0x89b0, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x41e0, 0x42c8, 1); // Utilize Spell Practice Jump
            ecl << pair{0x42d8, 2300}; // Set Health
            ecl << pair{0x42f8, (int8_t)0x31}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST4_BOSS3:
            ECLJump(ecl, 0x860c, 0x89b0, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x4418, (int8_t)0x32}; // Change Nonspell
            ecl << pair{0x4f24, (int16_t)0} << pair{0x5054, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x51cc, 20} << pair{0x4e30, 0}; // Change Wait Time & Inv. Time
            break;
        case THPrac::TH15::TH15_ST4_BOSS4:
            ECLJump(ecl, 0x860c, 0x89b0, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x41e0, 0x42c8, 1); // Utilize Spell Practice Jump
            ecl << pair{0x42d8, 3100}; // Set Health
            ecl << pair{0x42f8, (int8_t)0x32}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST4_BOSS5:
            ECLJump(ecl, 0x860c, 0x89b0, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x4418, (int8_t)0x33}; // Change Nonspell
            ecl << pair{0x5e70, (int16_t)0} << pair{0x5d40, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x5ffc, 20} << pair{0x5fe8, 0}; // Change Wait Time & Inv. Time
            break;
        case THPrac::TH15::TH15_ST4_BOSS6:
            ECLJump(ecl, 0x860c, 0x89b0, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x41e0, 0x42c8, 1); // Utilize Spell Practice Jump
            ecl << pair{0x42d8, 2500}; // Set Health
            ecl << pair{0x42f8, (int8_t)0x33}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST4_BOSS7:
            ECLJump(ecl, 0x860c, 0x89b0, 60);
            ecl.SetFile(2);
            // Skip the entrance transition, but settle the final spell natively.
            ECLSkipChapter(1);
            ECLJump(ecl, 0x41e0, 0x42b4, 1); // Utilize Spell Practice Jump
            ecl.SetPos(0x42b4);
            ecl << 1 << 0x00140279 << 0x01ff0000 << 0 << 1;
            ecl << pair{0x42d8, 3400}; // Set Health
            ecl << pair{0x42f8, (int8_t)0x34}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST5_MID1:
            switch (thPracParam.phase) {
            case 1:
                ECLJump(ecl, 0x9f0c, 0xa1a0, 60);
                ECLJump(ecl, 0x70ec, 0x7100, 0);
                break;
            case 2:
                ECLJump(ecl, 0x9f0c, 0xa1a0, 60);
                ECLJump(ecl, 0x70ec, 0x716c, 0);
                break;
            case 3:
                ECLJump(ecl, 0x9f0c, 0xa1a0, 60);
                ECLJump(ecl, 0x70ec, 0x71ec, 0);
                break;
            case 4:
                ECLJump(ecl, 0x9f0c, 0xa1a0, 60);
                ECLJump(ecl, 0x70ec, 0x726c, 0);
                break;
            case 5:
                ECLJump(ecl, 0x9f0c, 0xa1a0, 60);
                ECLJump(ecl, 0x70ec, 0x72ec, 0);
                break;
            case 6:
                ECLJump(ecl, 0x9f0c, 0xa1a0, 60);
                ECLJump(ecl, 0x70ec, 0x736c, 0);
                break;

            default:
                ECLJump(ecl, 0x9f0c, 0xa180, 60);
                ECLJump(ecl, 0x6ed8, 0x7074, 0);
                break;
            }
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST5_BOSS1:
            if (thPracParam.dlg)
                ECLJump(ecl, 0x9f0c, 0xa20c, 60);
            else
                ECLJump(ecl, 0x9f0c, 0xa220, 60);
            ecl.SetFile(3);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST5_BOSS2:
            ECLJump(ecl, 0x9f0c, 0xa220, 60);
            ecl.SetFile(3);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x528, 0x610, 0); // Utilize Spell Practice Jump
            ecl << pair{0x620, 1}; // Set Health
            ecl << pair{0x640, (int8_t)0x31}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST5_BOSS3:
            ECLJump(ecl, 0x9f0c, 0xa220, 60);
            ecl.SetFile(3);
            ECLSkipChapter(2);
            ecl << pair{0x760, (int8_t)0x32}; // Change Nonspell
            ecl << pair{0x1394, (int16_t)0} << pair{0x14c4, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x163c, 59} << pair{0x165c, 0} << pair{0x12ac, 0}; // Change Move Time, Wait Time & Inv. Time
            break;
        case THPrac::TH15::TH15_ST5_BOSS4:
            ECLJump(ecl, 0x9f0c, 0xa220, 60);
            ecl.SetFile(3);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x528, 0x610, 0); // Utilize Spell Practice Jump
            ecl << pair{0x620, 3400}; // Set Health
            ecl << pair{0x640, (int8_t)0x32}; // Set Spell Ordinal
            switch (thPracParam.phase) {
            default:
            case 0:
                break;
            case 1:
                ecl << pair { 0x5B20, (int16_t)542 };
                break;
            }
            break;
        case THPrac::TH15::TH15_ST5_BOSS5:
            ECLJump(ecl, 0x9f0c, 0xa220, 60);
            ecl.SetFile(3);
            ECLSkipChapter(2);
            ecl << pair{0x760, (int8_t)0x33}; // Change Nonspell
            ecl << pair{0x2744, (int16_t)0} << pair{0x2874, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x29ec, 59} << pair{0x2a0c, 0}; // Change Move Time & Wait Time
            break;
        case THPrac::TH15::TH15_ST5_BOSS6:
            ECLJump(ecl, 0x9f0c, 0xa220, 60);
            ecl.SetFile(3);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x528, 0x610, 0); // Utilize Spell Practice Jump
            ecl << pair{0x620, 2320}; // Set Health
            ecl << pair{0x640, (int8_t)0x33}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST5_BOSS7:
            ECLJump(ecl, 0x9f0c, 0xa220, 60);
            ecl.SetFile(3);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x528, 0x610, 0); // Utilize Spell Practice Jump
            ecl << pair{0x620, 3001}; // Set Health
            ecl << pair{0x640, (int8_t)0x34}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST5_BOSS8:
            ECLJump(ecl, 0x9f0c, 0xa220, 60);
            ecl.SetFile(3);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x528, 0x610, 0); // Utilize Spell Practice Jump
            ecl << pair{0x620, 1}; // Set Health
            ecl << pair{0x640, (int8_t)0x35}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST6_STARS:
            if (thPracParam.phase == 1) {
                ECLJump(ecl, 0x91d0, 0x93e0, 0);
                ECLJump(ecl, 0x2b34, 0x2c68, 0);
                ecl << pair(0x436C, (int32_t)99999999) // endless
                    << pair(0x40AC, (int32_t)0)
                    // << pair(0x40F4, (int32_t)0)
                    // << pair(0x413C, (int32_t)0)
                    // << pair(0x4184, (int32_t)0)
                    // << pair(0x41CC, (int32_t)0)
                    << pair(0x4214, (int32_t)0)
                    //<< pair(0x425C, (int32_t)0)
                    //<< pair(0x42A4, (int32_t)0)
                    //<< pair(0x42EC, (int32_t)0)
                    //<< pair(0x4334, (int32_t)0)
                    << pair(0x79CC, (int32_t)99999999)
                    << pair(0x8FC4, (int32_t)99999999)
                    ;
            }else{
                ECLJump(ecl, 0x91d0, 0x93e0, 0);
                ECLJump(ecl, 0x2b34, 0x2c40, 60);
                ECLSkipChapter(1);
                ECLStarsBGMSync();
            }
            break;
        case THPrac::TH15::TH15_ST6_BOSS1:
            if (thPracParam.dlg)
                ECLJump(ecl, 0x91d0, 0x94ec, 60);
            else
                ECLJump(ecl, 0x91d0, 0x9500, 60);
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST6_BOSS2:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x5a0, 0x688, 0); // Utilize Spell Practice Jump
            ecl << pair{0x698, 4000}; // Set Health
            ecl << pair{0x6b8, (int8_t)0x31}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST6_BOSS3:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x7d8, (int8_t)0x32}; // Change Nonspell
            ecl << pair{0x1274, (int16_t)0} << pair{0x13a4, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x1528, 0} << pair{0x1178, 0}; // Change Wait Time & Inv. Time
            break;
        case THPrac::TH15::TH15_ST6_BOSS4:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            if (thPracParam.phase == 1) {
                ECLJump(ecl, 0x5A94, 0x5BB8, 94);
                ECLJump(ecl, 0x390, 0x43C, 0);//disable waiting time
            }
            ECLSkipChapter(2);
            ECLJump(ecl, 0x5a0, 0x688, 0); // Utilize Spell Practice Jump
            ecl << pair{0x698, 2600}; // Set Health
            ecl << pair{0x6b8, (int8_t)0x32}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST6_BOSS5:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x7d8, (int8_t)0x33}; // Change Nonspell
            ecl << pair{0x1f84, (int16_t)0} << pair{0x20b4, (int16_t)0}; // Disable Item Drops & SE
            break;
        case THPrac::TH15::TH15_ST6_BOSS6:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            if (thPracParam.phase == 1) {
                ECLJump(ecl, 0x390, 0x43C, 0);//disable waiting time
                ecl << pair { 0x94BC, -9999.0f};
                ecl << pair { 0x94D0, (2.0f - thPracParam.enhanced_para) * 1.570796f };

            }
            ECLSkipChapter(2);
            ECLJump(ecl, 0x5a0, 0x63c, 0); // Utilize Spell Practice Jump
            ecl << pair{0x698, 4000}; // Set Health
            ecl << pair{0x6b8, (int8_t)0x33}; // Set Spell Ordinal
            ecl.SetPos(0x63c); // Boss Effect
            ecl << 0 << 0x0014012e << 0x01ff0000 << 0 << 3
                << 0 << 0x0018012f << 0x02ff0000 << 0 << 3 << 6;
            ecl << 0 << 0x0020000f << 0x01ff0000 << 0 << 0xc
                << 0x73736f42 << 0x6f705f33 << 0x00000073; // "Boss", "3_po", "s"
            break;
        case THPrac::TH15::TH15_ST6_BOSS7:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ecl << pair{0x7d8, (int8_t)0x34}; // Change Nonspell
            ecl << pair{0x302c, (int16_t)0} << pair{0x315c, (int16_t)0}; // Disable Item Drops & SE
            break;
        case THPrac::TH15::TH15_ST6_BOSS8:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x5a0, 0x63c, 0); // Utilize Spell Practice Jump
            ecl << pair{0x698, 3700}; // Set Health
            ecl << pair{0x6b8, (int8_t)0x34}; // Set Spell Ordinal
            ecl.SetPos(0x63c); // Boss Effect
            ecl << 0 << 0x0014012e << 0x01ff0000 << 0 << 3
                << 0 << 0x0018012f << 0x02ff0000 << 0 << 3 << 6;
            ecl << 0 << 0x0020000f << 0x01ff0000 << 0 << 0xc
                << 0x73736f42 << 0x6f705f34 << 0x00000073; // "Boss", "4_po", "s"
            break;
        case THPrac::TH15::TH15_ST6_BOSS9:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x5a0, 0x688, 0); // Utilize Spell Practice Jump
            ecl << pair{0x698, 5000}; // Set Health
            ecl << pair{0x6b8, (int8_t)0x35}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST6_BOSS10:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x5a0, 0x688, 0); // Utilize Spell Practice Jump
            ecl << pair{0x698, 6000}; // Set Health
            ecl << pair{0x6b8, (int8_t)0x36}; // Set Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST6_BOSS11:
            ECLJump(ecl, 0x91d0, 0x9500, 60);
            ecl.SetFile(2);
            ECLSkipChapter(2);
            ECLJump(ecl, 0x5a0, 0x63c, 0); // Utilize Spell Practice Jump
            ecl << pair{0x698, 8000}; // Set Health
            ecl << pair{0x6b8, (int8_t)0x37}; // Set Spell Ordinal
            ecl.SetPos(0x63c); // Boss Effect
            ecl << 0 << 0x0014012e << 0x01ff0000 << 0 << 3
                << 0 << 0x0018012f << 0x02ff0000 << 0 << 3 << 6;
            ecl << 0 << 0x0020000f << 0x01ff0000 << 0 << 0xc
                << 0x73736f42 << 0x6f705f34 << 0x00000073; // "Boss", "4_po", "s"

            if (thPracParam.phase) {
                if (thPracParam.phase == 1)
                    ecl << pair{0x698, 5500};
                else if (thPracParam.phase == 2)
                    ecl << pair{0x698, 3500};
                else if (thPracParam.phase == 3)
                    ecl << pair{0x698, 1200};
            }
            if (!TH15PatchPureSignRng(ecl, thPracParam.pure_sign_fixed_seed)) {
                thPracParam.pure_sign_fixed_seed = false;
                TH15BossHpLog("pure-sign-rng-layout-mismatch", 0);
            }
            break;
        case THPrac::TH15::TH15_ST7_MID1:
            if (thPracParam.dlg)
                ECLJump(ecl, st7Start, 0x9270, 60);
            else {
                ECLJump(ecl, st7Start, 0x92a8, 60);
                ECLSkipChapter(1);
                ecl.SetFile(2);

                ecl.SetPos(0x34c); // Inv. Time
                ecl << 0 << 0x00200203 << 0x01ff0000 << 0 << 60;
                ecl.SetPos(0x37c); // Boss Movement Restriction
                ecl << 0 << 0x002c01f8 << 0x04ff0000 << 0
                    << 0x0 << 0x43000000 << 0x438c0000 << 0x43800000;
                ecl << pair{0x33c, (int16_t)0}; // Void Wait
                ecl << pair{0x36c, 60} << pair{0x328, 60}; // Wait Time & Move Time
                ECLJump(ecl, 0x48c, 0x508, 0); // Spell Jump
                ecl << pair{0x2fc, 2200} << pair{0x525, (int8_t)0x31}; // Spell Health & Spell Ordinal
            }
            break;
        case THPrac::TH15::TH15_ST7_MID2:
            ECLJump(ecl, st7Start, 0x92a8, 60);
            ECLSkipChapter(1);
            ecl.SetFile(2);

            ecl.SetPos(0x34c); // Inv. Time
            ecl << 0 << 0x00200203 << 0x01ff0000 << 0 << 60;
            ecl.SetPos(0x37c); // Boss Movement Restriction
            ecl << 0 << 0x002c01f8 << 0x04ff0000 << 0
                << 0x0 << 0x43000000 << 0x438c0000 << 0x43800000;
            ecl << pair{0x33c, (int16_t)0}; // Void Wait
            ecl << pair{0x36c, 60} << pair{0x328, 60}; // Wait Time & Move Time
            ECLJump(ecl, 0x48c, 0x508, 0); // Spell Jump
            ecl << pair{0x2fc, 2200} << pair{0x525, (int8_t)0x32}; // Spell Health & Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST7_MID3:
            ECLJump(ecl, st7Start, 0x92a8, 60);
            ECLSkipChapter(1);
            ecl.SetFile(2);

            ecl.SetPos(0x34c); // Inv. Time
            ecl << 0 << 0x00200203 << 0x01ff0000 << 0 << 60;
            ecl.SetPos(0x37c); // Boss Movement Restriction
            ecl << 0 << 0x002c01f8 << 0x04ff0000 << 0
                << 0x0 << 0x43000000 << 0x438c0000 << 0x43800000;
            ecl << pair{0x33c, (int16_t)0}; // Void Wait
            ecl << pair{0x36c, 60} << pair{0x328, 60}; // Wait Time & Move Time
            ECLJump(ecl, 0x48c, 0x508, 0); // Spell Jump
            ecl << pair{0x2fc, 3000} << pair{0x525, (int8_t)0x33}; // Spell Health & Spell Ordinal
            break;
        case THPrac::TH15::TH15_ST7_END_NS1:
            if (thPracParam.dlg)
                ECLJump(ecl, st7Start, 0x93b8, 60);
            else {
                th15_st7boss1_chapter_bonus.Enable();
                ECLJump(ecl, st7Start, st7BossCreateCall, 60);
                st7_hide_subboss();
            }
            ECLSkipChapter(1);
            break;
        case THPrac::TH15::TH15_ST7_END_S1:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            st7_enter_spell(0x31, 3000);
            break;
        case THPrac::TH15::TH15_ST7_END_NS2:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();

            ecl.SetFile(3);
            ecl << pair{0x708, 0x32}; // Change Nonspell
            ecl << pair{0x16f4, (int16_t)0} << pair{0x1824, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x199c, 59} << pair{0x19bc, 0} << pair{0x15dc, 0}; // Change Move Time, Wait Time & Inv. Time

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0} << pair{0x191c, (int16_t)0}; // Void 303-0 & 306-0
            ecl << pair{st7bsANMSetMainArg2, 14} << pair{st7bsANMSetSprite2Arg2, 120}; // Change 306-0 & 303-2
            ECLJump(ecl, 0x1a90, 0x1ae8, 24); // Jump Over Sprite Change
            break;
        case THPrac::TH15::TH15_ST7_END_S2:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            st7_enter_spell(0x32, 3400);

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0}; // Void 303-0
            ecl << pair{st7bsANMSetMainArg2, 14} << pair{st7bsANMSetSprite2Arg2, 120}; // Change 306-0 & 303-2
            break;
        case THPrac::TH15::TH15_ST7_END_NS3:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();

            ecl.SetFile(3);
            ecl << pair{0x708, 0x33}; // Change Nonspell
            ecl << pair{0x2430, (int16_t)0} << pair{0x2560, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x26d8, 59} << pair{0x26f8, 0} << pair{0x2318, 0}; // Change Move Time, Wait Time & Inv. Time

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0} << pair{0x2658, (int16_t)0}; // Void 303-0 & 306-0
            ecl << pair{st7bsANMSetMainArg2, 7} << pair{st7bsANMSetSprite2Arg2, 132}; // Change 306-0 & 303-2
            ECLJump(ecl, 0x276c, 0x2798, 24); // Jump Over Sprite Change
            break;
        case THPrac::TH15::TH15_ST7_END_S3:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            st7_enter_spell(0x33, 3000);

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0}; // Void 303-0
            ecl << pair{st7bsANMSetMainArg2, 7} << pair{st7bsANMSetSprite2Arg2, 132}; // Change 306-0 & 303-2
            break;
        case THPrac::TH15::TH15_ST7_END_NS4:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();

            ecl.SetFile(3);
            ecl << pair{0x708, 0x34}; // Change Nonspell
            ecl << pair{0x32a0, (int16_t)0}; // Don't Activate Sub Boss
            ecl << pair{0x32e8, (int16_t)0} << pair{0x3418, (int16_t)0}; // Disable Item Drops & SE
            ECLJump(ecl, 0x3548, 0x35b0, 0); // Skip Move
            ecl << pair{0x31c4, 10}; // Inv. Time

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0} << pair{0x3510, (int16_t)0}; // Void 303-0 & 306-0
            ecl << pair{st7bsANMSetMainArg2, 0} << pair{st7bsANMSetSprite2Arg2, 116}; // Change 306-0 & 303-2
            ecl << pair{st7bsANMSelect, 5}; // Change 302
            break;
        case THPrac::TH15::TH15_ST7_END_S4:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            st7_enter_spell(0x34, 2000, true);

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0}; // Void 303-0
            ecl << pair{st7bsANMSetMainArg2, 0} << pair{st7bsANMSetSprite2Arg2, 116}; // Change 306-0 & 303-2
            ecl << pair{st7bsANMSelect, 5}; // Change 302
            break;
        case THPrac::TH15::TH15_ST7_END_NS5:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();

            ecl.SetFile(3);
            ecl << pair{0x708, 0x35}; // Change Nonspell
            ecl << pair{0x4464, (int16_t)0}; // Don't Activate Sub Boss
            ecl << pair{0x44c0, (int16_t)0} << pair{0x45f0, (int16_t)0}; // Disable Item Drops & SE
            ECLJump(ecl, 0x4720, 0x4788, 0); // Skip Move
            ecl << pair{0x4388, 0}; // Inv. Time

            ecl.SetFile(3);
            ecl << pair{0x46e8, (int16_t)0}; // Void 306-0
            break;
        case THPrac::TH15::TH15_ST7_END_S5:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            st7_enter_spell(0x35, 3500);
            break;
        case THPrac::TH15::TH15_ST7_END_NS6:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();

            ecl.SetFile(3);
            ecl << pair{0x708, 0x36}; // Change Nonspell
            ecl << pair{0x5368, (int16_t)0} << pair{0x5498, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x5610, 59} << pair{0x5630, 0} << pair{0x5250, 0}; // Change Move Time, Wait Time & Inv. Time

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0} << pair{0x5590, (int16_t)0}; // Void 303-0 & 306-0
            ecl << pair{st7bsANMSetMainArg2, 14} << pair{st7bsANMSetSprite2Arg2, 120}; // Change 306-0 & 303-2
            ECLJump(ecl, 0x5694, 0x56ec, 24); // Jump Over Sprite Change
            break;
        case THPrac::TH15::TH15_ST7_END_S6:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            st7_enter_spell(0x36, 3500);
            ecl << pair{0xd898, (int16_t)0}; // Void 316

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0}; // Void 303-0
            ecl << pair{st7bsANMSetMainArg2, 14} << pair{st7bsANMSetSprite2Arg2, 120}; // Change 306-0 & 303-2
            break;
        case THPrac::TH15::TH15_ST7_END_NS7:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();

            ecl.SetFile(3);
            ecl << pair{0x708, 0x37}; // Change Nonspell
            ecl << pair{0x6198, (int16_t)0} << pair{0x62c8, (int16_t)0}; // Disable Item Drops & SE
            ecl << pair{0x6440, 59} << pair{0x6460, 0} << pair{0x6080, 0}; // Change Move Time, Wait Time & Inv. Time

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0} << pair{0x63c0, (int16_t)0}; // Void 303-0 & 306-0
            ecl << pair{st7bsANMSetMainArg2, 7} << pair{st7bsANMSetSprite2Arg2, 132}; // Change 306-0 & 303-2
            ECLJump(ecl, 0x64d4, 0x652c, 24); // Jump Over Sprite Change
            break;
        case THPrac::TH15::TH15_ST7_END_S7:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            st7_enter_spell(0x37, 3000);

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0}; // Void 303-0
            ecl << pair{st7bsANMSetMainArg2, 7} << pair{st7bsANMSetSprite2Arg2, 132}; // Change 306-0 & 303-2
            break;
        case THPrac::TH15::TH15_ST7_END_NS8:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();

            ecl.SetFile(3);
            ecl << pair{0x708, 0x38}; // Change Nonspell
            ecl << pair{0x7004, (int16_t)0}; // Don't Activate Sub Boss
            ecl << pair{0x708c, (int16_t)0} << pair{0x71bc, (int16_t)0}; // Disable Item Drops & SE
            ECLJump(ecl, 0x72ec, 0x7354, 0); // Skip Move
            ecl << pair{0x6f54, 0}; // Inv. Time

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0} << pair{0x72b4, (int16_t)0}; // Void 303-0 & 306-0
            ecl << pair{st7bsANMSetMainArg2, 0} << pair{st7bsANMSetSprite2Arg2, 116}; // Change 306-0 & 303-2
            ecl << pair{st7bsANMSelect, 5}; // Change 302
            break;
        case THPrac::TH15::TH15_ST7_END_S8:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            st7_enter_spell(0x38, 3000, true);

            ecl.SetFile(3);
            ecl << pair{st7bsSetSpriteIns, (int16_t)0}; // Void 303-0
            ecl << pair{st7bsANMSetMainArg2, 0} << pair{st7bsANMSetSprite2Arg2, 116}; // Change 306-0 & 303-2
            ecl << pair{st7bsANMSelect, 5}; // Change 302
            break;
        case THPrac::TH15::TH15_ST7_END_S9:
        {
            constexpr unsigned int st7bsANMSelect2 = 0x6a4 + 0x10; // 0x6b4
            constexpr unsigned int st7bsANMSetMainIns = 0x580 + 0x4; // 0x584

            constexpr unsigned int st7BossSpell9Duration = 0x10408 + 0x18;
            constexpr unsigned int st7BossSpell9TRStart = 0x104b8;
            constexpr unsigned int st7BossSpell9TRBluePhaseTime = 120 * 3 + 180 * 5 + 280;

            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);
            st7_hide_subboss();
            ecl.SetFile(3);
            ecl << pair { st7bsMovePosX, 0 } << pair { st7bsMovePosY, 0x43600000 }; // Center pos
            ecl << pair { st7bsSetSpriteIns, (int16_t)0 }; // Cancel changing boss sprite
            ecl << pair { st7bsANMSelect, 5 } << pair { st7bsANMSelect2, 5 }; // Change selected anim
            ecl << pair { st7bsANMSetMainIns, (int16_t)0 }
                << pair(st7bsANMSetSprite1Arg2, -1)
                << pair(st7bsANMSetSprite2Arg2, -1); // More ANM fixing
            ecl << pair { st7bsMoveLimitIns, (int16_t)0 }; // Remove move limits
            ecl.SetPos(0x6d0);
            // 306
            ecl << 0 << 0x00180132 << 0x02ff0000 << 0 << 0 << 0;
            // Flag
            ecl << 0 << 0x001401f6 << 0x01ff0000 << 0 << 0x20;
            // Health
            ecl << 0 << 0x001401ff << 0x01ff0000 << 0 << 3000;
            // Card
            ecl << 0 << 0x0020000b << 0x01ff0000 << 0 << 0xc
                << 0x73736f42 << 0x64726143 << 0x39;

            switch (thPracParam.phase) {
            case 1: {
                constexpr unsigned int st7BossSpell9TRGreenPhase = 0x10a50;

                ecl << pair { st7BossSpell9Duration, 5400 - st7BossSpell9TRBluePhaseTime + 90 }; // Adjust duration (removing waits)
                ecl << pair { 0x10bb0 + 0x10, 55 - (st7BossSpell9TRBluePhaseTime - 90) / 60 }; // Also adjust p2->p3 transition condition
                ECLJump(ecl, st7BossSpell9TRStart, st7BossSpell9TRGreenPhase, 0);
                break;
            }
            case 2: {
                constexpr unsigned int st7BossSpell9TRRedPhase = 0x10c7c;

                ecl << pair { st7BossSpell9Duration, 5400 - (55 * 60) - (90 + 18) }; // Adjust duration (hardcoded time threshold for p3 start, + wait)
                ECLJump(ecl, st7BossSpell9TRStart, st7BossSpell9TRRedPhase, 0);
                break;
            }
            default:
                break;
            }

        }
            break;
        case THPrac::TH15::TH15_ST7_END_S10:
            ECLJump(ecl, st7Start, st7BossCreateCall, 60);
            ECLSkipChapter(2);

            // Hecatia
            ecl.SetFile(3);
            ecl << pair{st7bsMovePosX, 0xc2800000} << pair{st7bsMovePosY, 0x43000000}; // Pos
            ecl << pair{st7bsMoveLimitIns, (int16_t)0}; // Void 504
            ecl << pair(st7bsANMSetSprite1Arg2, -1) << pair(st7bsANMSetSprite2Arg2, -1); // Change 303-1 & 303-2
            ecl.SetPos(0x6d0);
            // Effect
            ecl << 5 << 0x0014012e << 0x01ff0000 << 0 << 5
                << 5 << 0x0018012f << 0x02ff0000 << 0 << 3 << 6;
            ecl << 5 << 0x0020000f << 0x01ff0000 << 0 << 0xc
                << 0x73736f42 << 0x6f705f34 << 0x00000073; // "Boss", "4_po", "s"
            ecl << 5 << 0x0014012e << 0x01ff0000 << 0 << 3;
            // Health
            ecl << 5 << 0x001401ff << 0x01ff0000 << 0 << 7000;
            // Card
            ecl << 6 << 0x0020000b << 0x01ff0000 << 0 << 0xc
                << 0x73736f42 << 0x64726143 << 0x3031;

            // Junko
            ecl.SetFile(4);
            ecl << pair{0x184, 0x42800000} << pair{0x188, 0x43000000}; // Pos
            ecl << pair{0x328, (int16_t)0}; // Void 504
            ecl << pair(0x36c, -1) << pair(0x384, -1); // Change 303-1 & 303-2
            ecl << pair{0x2b4, (int16_t)0}; // Void 401
            ecl.SetPos(0x3b4);
            // Health
            ecl << 5 << 0x001401ff << 0x01ff0000 << 0 << 7000;
            // Inv.
            ecl << 5 << 0x00140203 << 0x01ff0000 << 0 << 14;
            // Stall
            ecl << 9999 << 0x00100000 << 0x00ff0000 << 0;

            switch (thPracParam.phase) {
            case 1:
                ecl.SetFile(3);
                ECLJump(ecl, 0x8f30, 0x8fc8, 94);
                ecl.SetFile(4);
                ECLJump(ecl, 0xb08, 0xba0, 0);
                break;
            case 2:
                ecl.SetFile(3);
                ECLJump(ecl, 0x8f30, 0x8fc8, 94);
                break;
            case 3:
                ecl.SetFile(4);
                ECLJump(ecl, 0xb08, 0xba0, 0);
                break;
            }
            break;
        default:
            break;
        }
    }
    __declspec(noinline) void THSectionPatch()
    {
        if (thPracParam.mode == 1 && thPracParam.section == TH15_ST2_BOSS3)
            g_th15BossHpRuntimeLogCount = 0;

        ECLHelper ecl;
        ecl.SetBaseAddr((void*)GetMemAddr(0x4e9a80, 0x17c, 0xC));

        auto section = thPracParam.section;
        ++g_th15RouteWarpGeneration;
        g_th15WarpWaitFrames = 0;
        if (section >= 10000 && section < 20000) {
            int stage = (section - 10000) / 100;
            int portionId = (section - 10000) % 100;
            THStageWarp(ecl, stage, portionId);
        } else {
            THPatch(ecl, (th_sections_t)section);
        }
    }

    int THBGMTest()
    {
        if (!thPracParam.mode)
            return 0;
        else if (thPracParam.section >= 10000)
            return 0;
        else
            return th_sections_bgm[thPracParam.section];
    }
    void THSaveReplay(char* repName)
    {
        ReplaySaveParam(mb_to_utf16(repName, 932).c_str(), thPracParam.GetJson());
    }

    static bool frameStarted = false;

    struct BossHudStatus {
        int32_t hp = 0;
        int32_t maxHp = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct BossPhaseHealthRef {
        int file = 0;
        size_t offset = 0;
        int32_t fallback = 0;
    };

    // Locate the next spell's health argument in TH15's loaded ECL. The
    // fallback values mirror the practice spell health configured in THPatch.
    static bool NextBossPhaseHealthRef(int section, BossPhaseHealthRef& ref)
    {
        switch (section) {
        case TH15_ST1_BOSS1: ref = { 2, 0x4b0, 1700 }; return true;
        case TH15_ST1_BOSS3: ref = { 2, 0x4b0, 1900 }; return true;
        case TH15_ST2_BOSS1: ref = { 2, 0x504, 2200 }; return true;
        case TH15_ST2_BOSS3: ref = { 2, 0x504, kTh15St2Boss3HpSwitchThreshold }; return true;
        case TH15_ST2_BOSS5: ref = { 2, 0x504, 2500 }; return true;
        case TH15_ST3_BOSS1: ref = { 2, 0x540, 2000 }; return true;
        case TH15_ST3_BOSS3: ref = { 2, 0x540, 2100 }; return true;
        case TH15_ST3_BOSS5: ref = { 2, 0x540, 2500 }; return true;
        case TH15_ST4_BOSS1: ref = { 2, 0x42d8, 2300 }; return true;
        case TH15_ST4_BOSS3: ref = { 2, 0x42d8, 3100 }; return true;
        case TH15_ST4_BOSS5: ref = { 2, 0x42d8, 2500 }; return true;
        case TH15_ST5_BOSS1: ref = { 3, 0x620, 1 }; return true;
        case TH15_ST5_BOSS3: ref = { 3, 0x620, 3400 }; return true;
        case TH15_ST5_BOSS5: ref = { 3, 0x620, 2320 }; return true;
        case TH15_ST5_BOSS7: ref = { 3, 0x620, 1 }; return true;
        case TH15_ST6_BOSS1: ref = { 2, 0x698, 4000 }; return true;
        case TH15_ST6_BOSS3: ref = { 2, 0x698, 2600 }; return true;
        case TH15_ST6_BOSS5: ref = { 2, 0x698, 4000 }; return true;
        case TH15_ST6_BOSS7: ref = { 2, 0x698, 3700 }; return true;
        case TH15_ST7_END_NS1: ref = { 3, 0x6e0, 3000 }; return true;
        case TH15_ST7_END_NS2: ref = { 3, 0x6e0, 3400 }; return true;
        case TH15_ST7_END_NS3: ref = { 3, 0x6e0, 3000 }; return true;
        case TH15_ST7_END_NS4: ref = { 3, 0x6e0, 2000 }; return true;
        case TH15_ST7_END_NS5: ref = { 3, 0x6e0, 3500 }; return true;
        case TH15_ST7_END_NS6: ref = { 3, 0x6e0, 3500 }; return true;
        case TH15_ST7_END_NS7: ref = { 3, 0x6e0, 3000 }; return true;
        case TH15_ST7_END_NS8: ref = { 3, 0x6e0, 3000 }; return true;
        default: return false;
        }
    }

    static int32_t NextBossPhaseHealth(int section)
    {
        BossPhaseHealthRef ref{};
        if (!NextBossPhaseHealthRef(section, ref))
            return 0;

        const uintptr_t manager = GetMemContent(0x4E9A80);
        const uintptr_t eclContainer = manager ? GetMemContent(manager + 0x17c) : 0;
        if (eclContainer) {
            const uintptr_t eclTable = eclContainer + 0xC;
            const uintptr_t nextPhaseScript = GetMemContent<uint32_t>(eclTable + ref.file * 4);
            if (!nextPhaseScript)
                return ref.fallback;
            ECLHelper ecl;
            ecl.SetBaseAddr(reinterpret_cast<void*>(eclTable));
            ecl.SetFile(ref.file);
            ecl.SetPos(ref.offset);
            int32_t health = 0;
            ecl >> health;
            if (health > 0 && health < 100000)
                return health;
        }
        return ref.fallback;
    }

    static bool ReadBossHudStatus(BossHudStatus& status)
    {
        const uintptr_t manager = GetMemContent(0x4E9A80);
        if (!manager)
            return false;

        // TH15 enemy manager: list head at +0x180; each node holds the
        // enemy pointer at +0 and the next node at +4.
        uintptr_t node = GetMemContent(manager + 0x180);
        for (int i = 0; node && i < 512; ++i) {
            const uintptr_t enemy = GetMemContent(node);
            const uintptr_t next = GetMemContent(node + 4);
            if (enemy) {
                const uintptr_t data = enemy + 0x120C;
                const uint32_t flags = GetMemContent<uint32_t>(data + 0x4060);
                if (flags & (1u << 23)) {
                    const int32_t maxHp = GetMemContent<int32_t>(data + 0x3F78);
                    if (maxHp > 0) {
                        status.hp = GetMemContent<int32_t>(data + 0x3F74);
                        status.maxHp = maxHp;
                        status.x = GetMemContent<float>(data + 0x44);
                        status.y = GetMemContent<float>(data + 0x48);
                        return true;
                    }
                }
            }
            node = next;
        }
        return false;
    }

    static void RenderBossAndPlayerStatus(ImDrawList* p)
    {
        if (!*THOverlay::singleton().mInGameInfo
            || (!g_th15BossHpHudEnabled && !g_th15BossPositionHudEnabled && !g_th15PlayerPositionHudEnabled))
            return;
        const uintptr_t player = GetMemContent(PLAYER_PTR);
        if (!player)
            return;

        const float playerX = GetMemContent<float>(player + 0x618);
        const float playerY = GetMemContent<float>(player + 0x61C);
        BossHudStatus boss{};
        std::string hpText = "HP --";
        std::string bossPosText = "B (--,--)";
        if (ReadBossHudStatus(boss)) {
            int32_t displayedHp = std::max(0, boss.hp);
            const auto& chapterSave = TH15ChapterSave::singleton();
            int section = 0;
            BossPhaseHealthRef practiceHealthRef{};
            const bool practiceBossSection = thPracParam.mode == 1
                && NextBossPhaseHealthRef(thPracParam.section, practiceHealthRef);
            if (chapterSave.ActiveIsBossSection()) {
                section = chapterSave.ActiveSection();
            } else if (chapterSave.DisplayIsBossSection()) {
                section = chapterSave.DisplaySection();
            } else if (practiceBossSection) {
                // The practice parameter names the section used to enter the
                // fight. Use it only until the live section tracker is ready.
                section = thPracParam.section;
            }
            if (section) {
                const int32_t phaseBoundaryHp = NextBossPhaseHealth(section);
                // A selected practice section can remain in thPracParam after
                // gameplay has entered its next spell. Once the live HP reaches
                // that boundary, show the new phase's own HP instead of
                // subtracting the boundary a second time (which would show 0).
                if (phaseBoundaryHp > 0 && boss.hp > phaseBoundaryHp)
                    displayedHp = boss.hp - phaseBoundaryHp;
            }
            hpText = std::format("HP {}", displayedHp);
            bossPosText = std::format("B({:.1f},{:.1f})", boss.x, boss.y);
        }
        const std::string playerPosText = std::format("P({:.1f},{:.1f})",
            playerX, playerY);
        const float rowHeight = ImGui::GetTextLineHeight();
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize();
        auto drawField = [&](float x, float width, const std::string& value) {
            p->AddRectFilled({ x, 0.0f }, { x + width, rowHeight }, 0xFFFFFFFF);
            p->PushClipRect({ x, 0.0f }, { x + width, rowHeight }, true);
            const ImVec2 textSize = ImGui::CalcTextSize(value.c_str());
            const float scale = textSize.x > width - 8.0f
                ? (width - 8.0f) / textSize.x : 1.0f;
            const float scaledFontSize = fontSize * scale;
            const float y = (rowHeight - textSize.y * scale) * 0.5f;
            p->AddText(font, scaledFontSize, { x + 4.0f, y }, 0xFF000000, value.c_str());
            p->PopClipRect();
        };
        // Keep the three status fields clear of the right-side keyboard HUD.
        if (g_th15BossHpHudEnabled) drawField(224.0f, 132.0f, hpText);
        if (g_th15BossPositionHudEnabled) drawField(360.0f, 150.0f, bossPosText);
        if (g_th15PlayerPositionHudEnabled) drawField(514.0f, 176.0f, playerPosText);
    }

    static void RenderLockTimer(ImDrawList* p)
    {
        if (g_lock_timer_flag) {
            g_lock_timer++;
            g_lock_timer_flag = false;
        }

        if (g_adv_igi_options.enable_lock_timer_autoly && *THOverlay::singleton().mTimeLock) {
            std::string time_text = std::format("{:.2f}", (float)g_lock_timer / 60.0f);
            auto sz = ImGui::CalcTextSize(time_text.c_str());
            p->AddRectFilled({ 64.0f, 0.0f }, { 220.0f, sz.y }, 0xFFFFFFFF);
            p->AddText({ 220.0f - sz.x, 0.0f }, 0xFF000000, time_text.c_str());
        }
    }

    void ABTestRender()
    {
        static int t = 0;
        DWORD ecl_glob = *(DWORD*)(0x4E9A80);
        DWORD m9923 = ecl_glob ? *(DWORD*)(ecl_glob + 0x18) : 0;
        if (m9923 == 6) {
            t++;
            float intp = (t >= 90) ? 1.0f : t / 90.0f;
            auto& io = ImGui::GetIO();
            auto szx = io.DisplaySize.x;
            auto szy = io.DisplaySize.y;
            ImVec2 mid_st = { 64.0f + 384.0f, 32.0f + 448.0f };
            mid_st.x *= szx / 1280.0f;
            mid_st.y *= szy / 960.0f;
            ImVec2 sz = { 380.0f * 2.0f, 600.0f * intp };
            sz.x *= szx / 1280.0f;
            sz.y *= szy / 960.0f;

            ImGui::SetNextWindowPos({ mid_st.x - sz.x * 0.5f, mid_st.y - sz.y * 0.5f });
            ImGui::SetNextWindowSize(sz);
            ImGui::SetNextWindowBgAlpha(0.8f);
            ImGui::Begin("##res", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
            if (t >= 90) {
                float result_origs[5] = {
                    *(float*)(ecl_glob + 0x38),
                    *(float*)(ecl_glob + 0x34),
                    *(float*)(ecl_glob + 0x30),
                    *(float*)(ecl_glob + 0x2C),
                    *(float*)(ecl_glob + 0x28)
                };
                float scores[6] = { 0, 0.1, 0.5, 0.6, 0.9, 1.0 };
                // calculates
                {
                    float div[5] = { 3.5f, 4.8f, 4.0f, 3.1f, 3.0f };
                    float pows[5] = { 1.2, 1.1, 1.1, 1.2, 2.05 };
                    for (int i = 0; i < 5; i++) {
                        result_origs[i] = (result_origs[i] - 3.0f) / div[i];
                        result_origs[i] = powf(result_origs[i], pows[i]);
                    }
                    scores[0] = 0.6f * result_origs[0] + 0.15f * result_origs[1] + 0.05f * result_origs[2] + 0.15f * result_origs[3] + 0.05f * result_origs[4];
                    scores[1] = 0.0f * result_origs[0] + 0.5f * result_origs[1] + 0.2f * result_origs[2] + 0.15f * result_origs[3] + 0.15f * result_origs[4];
                    scores[2] = 0.0f * result_origs[0] + 0.1f * result_origs[1] + 0.75f * result_origs[2] + 0.05f * result_origs[3] + 0.1f * result_origs[4];
                    scores[3] = 0.1f * result_origs[0] + 0.4f * result_origs[1] + 0.0f * result_origs[2] + 0.5f * result_origs[3] + 0.0f * result_origs[4];
                    scores[4] = 0.05f * result_origs[0] + 0.05f * result_origs[1] + 0.0f * result_origs[2] + 0.1f * result_origs[3] + 0.8f * result_origs[4];
                    scores[5] = 0.1f * result_origs[0] + 0.3f * result_origs[1] + 0.3f * result_origs[2] + 0.2f * result_origs[3] + 0.1f * result_origs[4];
                    for (int i = 0; i < 6; i++) {
                        scores[i] = 1.0f / (1.0f + expf(1.8f - 4.0f * scores[i]));
                    }
                }
                float avg_score = 0.0f;
                for (int i = 0; i < 6; i++)
                    avg_score += scores[i];
                avg_score /= 6.0f;

                const char* abtest_rank;
                if (avg_score < 0.2f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_10);
                else if (avg_score < 0.36f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_9);
                else if (avg_score < 0.54f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_8);
                else if (avg_score < 0.72f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_7);
                else if (avg_score < 0.9f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_6);
                else if (avg_score < 0.95f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_5);
                else if (avg_score < 0.98f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_4);
                else if (avg_score < 0.985f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_3);
                else if (avg_score < 0.99f)
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_2);
                else
                    abtest_rank = S(TH15_AB_TEST_RES_RANK_1);

                ImGui::Text("%s %s", S(TH15_AB_TEST_RES), abtest_rank);
                ImGui::Text(S(TH15_AB_TEST_RES_DESC));

                ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImVec2 csz = ImGui::GetContentRegionAvail();
                ImVec2 cmid = { p0.x + csz.x * 0.5f, p0.y + csz.y * 0.5f };
                ImVec2 p1 = { cmid.x + csz.x * 0.5f, cmid.y + csz.y * 0.5f };
                float hheight = csz.y * 0.75f * 0.5f;

                ImDrawList* pw = ImGui::GetWindowDrawList();
                ImVec4 color1 = ImGui::GetStyleColorVec4(ImGuiCol_::ImGuiCol_ChildBg);
                color1.w = 0.1f;
                pw->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(color1));
                pw->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_::ImGuiCol_Text)));
                pw->PushClipRect(p0, p1);
                {
                    ImVec2 points[6];
                    // radar
                    for (int i = 0; i < 6; i++) {
                        pw->AddLine(cmid, { cmid.x + hheight * cosf(i * std::numbers::pi_v<float> * 2.0f / 6.0f - 1.57079f), cmid.y + hheight * sinf(i * std::numbers::pi_v<float> * 2.0f / 6.0f - 1.57079f) }, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_::ImGuiCol_Text)), 2.0f);
                    }
                    // lines
                    constexpr float radiuses_ranks[] = {
                        0.18f / 0.9f * 0.5f,
                        0.36f / 0.9f * 0.5f,
                        0.54f / 0.9f * 0.5f,
                        0.72f / 0.9f * 0.5f,
                        0.5f, 0.65f, 0.85f, 1.0f
                    };
                    for (int i = 0; i < 8; i++) {
                        float ra = radiuses_ranks[i] * hheight;
                        for (int j = 0; j < 6; j++) {
                            points[j] = { cmid.x + ra * cosf(j * std::numbers::pi_v<float> * 2.0f / 6.0f - 1.57079f), cmid.y + ra * sinf(j * std::numbers::pi_v<float> * 2.0f / 6.0f - 1.57079f) };
                        }
                        pw->AddPolyline(points, 6, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_::ImGuiCol_Text)), ImDrawFlags_::ImDrawFlags_Closed, i == 7 ? 2.0f : 1.0f);
                    }
                    // scores
                    auto MInterpolation = [](float t, float a, float b) {
                        if (t < 0.0f) {
                            return a;
                        } else if (t < 0.5) {
                            float k = (b - a) * 2.0f;
                            return k * t * t + a;
                        } else if (t < 1.0f) {
                            float k = (b - a) * 2.0f;
                            t = t - 1.0f;
                            return -k * t * t + b;
                        }
                        return b;
                    };
                    float t2 = MInterpolation((t - 90.0f) / 90.0f, 0.0f, 1.0f);
                    for (int i = 0; i < 6; i++)
                        scores[i] *= t2;
                    // animation
                    for (int i = 0; i < 6; i++) {
                        float radius = scores[i];
                        if (scores[i] < 0.9f) {
                            radius = scores[i] / 0.9f * 0.5f;
                        } else if (scores[i] < 0.95f) {
                            radius = scores[i] * 3.0f - 2.2f;
                        } else if (scores[i] < 0.98f) {
                            radius = scores[i] * 6.66667f - 5.683333f;
                        } else {
                            radius = scores[i] * 7.5f - 6.5f;
                        }
                        points[i] = { cmid.x + hheight * radius * cosf(i * std::numbers::pi_v<float> * 2.0f / 6.0f - 1.57079f), cmid.y + hheight * radius * sinf(i * std::numbers::pi_v<float> * 2.0f / 6.0f - 1.57079f) };
                    }
                    for (int i = 0; i < 6; i++) {
                        auto col = ImGui::GetStyleColorVec4(ImGuiCol_PlotHistogram);
                        col.w = 0.75f;
                        pw->AddTriangleFilled(points[i], points[(i + 1) % 6], cmid, ImGui::ColorConvertFloat4ToU32(col));
                    }
                    pw->AddPolyline(points, 6, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_::ImGuiCol_PlotLines)), ImDrawFlags_::ImDrawFlags_Closed, 1.0f);

                    // text
                    const char* chars[] = {
                        S(TH15_AB_TEST_REACTION),
                        S(TH15_AB_TEST_MOV),
                        S(TH15_AB_TEST_RET),
                        S(TH15_AB_TEST_HORIZON),
                        S(TH15_AB_TEST_PREC),
                        S(TH15_AB_TEST_LUCK)
                    };
                    for (int i = 0; i < 6; i++) {
                        const char* rank = "E";
                        if (scores[i] < 0.18f)
                            rank = "F";
                        else if (scores[i] < 0.36f)
                            rank = "E";
                        else if (scores[i] < 0.54f)
                            rank = "D";
                        else if (scores[i] < 0.72f)
                            rank = "C";
                        else if (scores[i] < 0.9f)
                            rank = "B";
                        else if (scores[i] < 0.95f)
                            rank = "A"; // 0.9-0.95
                        else if (scores[i] < 0.98f)
                            rank = "S"; // 0.95-0.98
                        else if (scores[i] < 0.99f)
                            rank = "SS"; // 0.98-0.99
                        else
                            rank = "???"; // 0.99+
                        std::string text = std::format("{}:{}({:>3.1f})", chars[i], rank, scores[i] * 100.0f).c_str();
                        auto sz_text = ImGui::CalcTextSize(text.c_str());
                        float h2 = hheight;
                        if (i == 0 || i == 3)
                            h2 = hheight + sz_text.y;
                        else
                            h2 = hheight + sz_text.x * 0.85f;
                        ImVec2 pos = { cmid.x + h2 * cosf(i * std::numbers::pi_v<float> * 2.0f / 6.0f - 1.57079f), cmid.y + h2 * sinf(i * std::numbers::pi_v<float> * 2.0f / 6.0f - 1.57079f) };
                        pos.x -= sz_text.x * 0.5f;
                        pos.y -= sz_text.y * 0.5f;
                        pw->AddText(pos, ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_::ImGuiCol_Text)), text.c_str());
                    }
                }
                pw->PopClipRect();
            }
            ImGui::End();
        } else {
            t = 0;
        }
    }

    void MakeReisenShieldANM(uintptr_t bombPtr, int32_t shieldCount)
    {
        if (!bombPtr || !shieldCount)
            return;

        // create shield anmVM
        uintptr_t newAnmVm = asm_call<ALLOCATE_ANM_VM, Cdecl, uintptr_t>();
        asm_call<COPY_ANM_VM_FROM_LOADED, Thiscall>(GetMemContent(PLAYER_PTR, 0xc), newAnmVm, 0xa);
        *(int32_t*)(newAnmVm + 0x1c) |= 0x400;
        asm_call<ANM_VM_RUN, Thiscall>(newAnmVm);

        // add it to the world & to the bomb
        uint32_t outID = 0;
        asm_call<APPEND_ANM_VM_TO_WORLD_LIST, Stdcall>(&outID, newAnmVm);
        *(int32_t*)(bombPtr + 0x50) = outID;

        // remove shield rings based on shield count
        // can't trigger 2 interrupts in the same frame to remove 2 rings, so we do the work ourselves
        if (shieldCount <= 2) {
            ThList<struct AnmVM>* childListHead = (ThList<struct AnmVM>*)(newAnmVm + 0x544 + 0x50);

            for (ThList<struct AnmVM>* vmList = childListHead; vmList; vmList = vmList->next) {
                uintptr_t childVm = GetMemContent((uintptr_t)vmList);
                uint32_t childId = GetMemContent(childVm + 0x30);

                if (childId == 11 || (childId == 12 && shieldCount == 1)) {
                    uint8_t& flags = *GetMemAddr<uint8_t*>(childVm + 0x1c);
                    flags = (flags & 0x9F) | 0x20; // mark for deletion
                }
            }
        }
    }

#if THPRAC_TH15_HITBOX_FEATURE
    static ImTextureID TH15EnsureHitboxTexture()
    {
        if (g_th15_hitbox_texture)
            return (ImTextureID)g_th15_hitbox_texture;

        if (!THPrac::g_gameGuiDevice || !*THPrac::g_gameGuiDevice)
            return nullptr;

        auto* device = reinterpret_cast<IDirect3DDevice9*>(*THPrac::g_gameGuiDevice);
        if (FAILED(device->CreateTexture(128, 128, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &g_th15_hitbox_texture, nullptr)))
            return nullptr;

        D3DLOCKED_RECT locked{};
        if (FAILED(g_th15_hitbox_texture->LockRect(0, &locked, nullptr, 0))) {
            g_th15_hitbox_texture->Release();
            g_th15_hitbox_texture = nullptr;
            return nullptr;
        }

        constexpr int textureSize = 128;
        constexpr int circleSize = 64;
        constexpr float circleCenter = circleSize * 0.5f;
        auto* pixels = static_cast<uint8_t*>(locked.pBits);
        for (int y = 0; y < textureSize; ++y)
            std::memset(pixels + y * locked.Pitch, 0, textureSize * sizeof(uint32_t));

        for (int y = 0; y < circleSize; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(pixels + y * locked.Pitch);
            for (int x = 0; x < circleSize; ++x) {
                const float dx = (x + 0.5f - circleCenter) / (circleCenter - 1.0f);
                const float dy = (y + 0.5f - circleCenter) / (circleCenter - 1.0f);
                const float radius = std::sqrt(dx * dx + dy * dy);
                if (radius > 1.0f)
                    continue;

                const float edge = std::clamp((radius - 0.86f) / 0.14f, 0.0f, 1.0f);
                const uint32_t alpha = static_cast<uint32_t>(92.0f + edge * 163.0f);
                row[x] = (alpha << 24) | 0x00FFFFFF;
            }
        }

        // The lower-right texel is a solid white pixel used to tint rectangle
        // and laser quads without creating additional geometry.
        auto* whitePixelRow = reinterpret_cast<uint32_t*>(pixels + 96 * locked.Pitch);
        whitePixelRow[96] = 0xFFFFFFFF;
        g_th15_hitbox_texture->UnlockRect(0);
        return (ImTextureID)g_th15_hitbox_texture;
    }

    static bool TH15HitboxCaptureEnabled()
    {
        if (!g_show_bullet_hitbox)
            return false;

        const bool replay = THGuiRep::singleton().mRepStatus;
        return replay || (GetMemContent<int32_t>(0x4e7794) & 0x10) != 0;
    }

    template <typename T>
    static void TH15StoreHitbox(std::vector<T>& hitboxes, const T& hitbox)
    {
        // Keep every hitbox observed during fast replay; the draw path uses
        // one textured quad per hitbox instead of polygon tessellation.
        hitboxes.push_back(hitbox);
    }

    static bool TH15HitboxIsFinite(ImVec2 pos)
    {
        return std::isfinite(pos.x) && std::isfinite(pos.y);
    }

    static void TH15CaptureCircleHitbox(uintptr_t esp, float radius)
    {
        if (!TH15HitboxCaptureEnabled() || !std::isfinite(radius) || radius < 0.0f)
            return;

        const uintptr_t posPtr = GetMemContent<uintptr_t>(esp + 4);
        if (!posPtr)
            return;

        const ImVec2 pos = GetMemContent<ImVec2>(posPtr);
        if (!TH15HitboxIsFinite(pos))
            return;

        float playerRadius = 0.0f;
        const uintptr_t player = GetMemContent(PLAYER_PTR);
        if (player) {
            const uintptr_t playerHitbox = GetMemContent<uintptr_t>(player + 0x2c008);
            if (playerHitbox)
                playerRadius = GetMemContent<float>(playerHitbox + 4);

            if ((GetMemContent<uint32_t>(player + 0x16294) & 0x10) != 0) {
                const float scale = GetMemContent<float>(player + 0x2c044)
                    * GetMemContent<float>(0x4cff50);
                playerRadius *= scale;
            }
        }

        if (!std::isfinite(playerRadius) || playerRadius < 0.0f)
            playerRadius = 0.0f;
        const float dangerRadius = std::sqrt(radius * radius + playerRadius * playerRadius);
        if (std::isfinite(dangerRadius))
            TH15StoreHitbox(g_th15_bullet_hit_draw_vec, { pos, dangerRadius });
    }

    __declspec(noinline) static void TH15CaptureCircleHitboxFromXmm2(uintptr_t esp)
    {
        float radius = 0.0f;
        __asm { movss radius, xmm2 }
        TH15CaptureCircleHitbox(esp, radius);
    }

    static void TH15CaptureRectHitbox(uintptr_t esp)
    {
        if (!TH15HitboxCaptureEnabled())
            return;

        const uintptr_t posPtr = GetMemContent<uintptr_t>(esp + 4);
        const uintptr_t sizePtr = GetMemContent<uintptr_t>(esp + 8);
        const uintptr_t player = GetMemContent(PLAYER_PTR);
        if (!posPtr || !sizePtr || !player)
            return;

        const ImVec2 pos = GetMemContent<ImVec2>(posPtr);
        const ImVec2 size = GetMemContent<ImVec2>(sizePtr);
        const ImVec2 playerHitbox = GetMemContent<ImVec2>(player + 0x2bfc8);
        if (!TH15HitboxIsFinite(pos) || !TH15HitboxIsFinite(size)
            || !TH15HitboxIsFinite(playerHitbox) || size.x < 0.0f || size.y < 0.0f)
            return;

        const ImVec2 halfSize = {
            size.x * 0.5f + std::abs(playerHitbox.x),
            size.y * 0.5f + std::abs(playerHitbox.y),
        };
        TH15StoreHitbox(g_th15_rect_hit_draw_vec, {
            { pos.x - halfSize.x, pos.y - halfSize.y },
            { pos.x + halfSize.x, pos.y + halfSize.y },
        });
    }

    static void TH15CaptureLaserHitbox(uintptr_t esp, float angle, float height)
    {
        if (!TH15HitboxCaptureEnabled() || !std::isfinite(angle) || !std::isfinite(height))
            return;

        const uintptr_t posPtr = GetMemContent<uintptr_t>(esp + 4);
        const uintptr_t player = GetMemContent(PLAYER_PTR);
        if (!posPtr || !player)
            return;

        const ImVec2 pos = GetMemContent<ImVec2>(posPtr);
        const float width = GetMemContent<float>(esp + 8);
        const ImVec2 playerHitbox = GetMemContent<ImVec2>(player + 0x2bfc8);
        if (!TH15HitboxIsFinite(pos) || !TH15HitboxIsFinite(playerHitbox)
            || !std::isfinite(width) || width < 0.0f || height < 0.0f)
            return;

        TH15StoreHitbox(g_th15_laser_hit_draw_vec,
            { pos, playerHitbox, width, height, angle });
    }

    __declspec(noinline) static void TH15CaptureLaserHitboxFromXmm(uintptr_t esp)
    {
        float angle = 0.0f;
        float height = 0.0f;
        __asm {
            movss angle, xmm2
            movss height, xmm3
        }
        TH15CaptureLaserHitbox(esp, angle, height);
    }

    static void TH15DrawHitboxes(ImDrawList* drawList)
    {
        if (!drawList)
            return;

        if (TH15HitboxCaptureEnabled()) {
            ImGuiIO& io = ImGui::GetIO();
            const float xRatio = io.DisplaySize.x / 1280.0f;
            const float yRatio = io.DisplaySize.y / 960.0f;
            const auto toClient = [&](ImVec2 stagePos) {
                return ImVec2{
                    (stagePos.x * 2.0f + 448.0f) * xRatio,
                    (stagePos.y * 2.0f + 32.0f) * yRatio,
                };
            };

            drawList->PushClipRect(
                { 64.0f * xRatio, 32.0f * yRatio },
                { 832.0f * xRatio, 928.0f * yRatio });

            const ImTextureID hitboxTexture = TH15EnsureHitboxTexture();
            constexpr float whiteTexelUv = 96.5f / 128.0f;
            const ImVec2 whiteUv = { whiteTexelUv, whiteTexelUv };
            for (const auto& hitbox : g_th15_bullet_hit_draw_vec) {
                const ImVec2 center = toClient(hitbox.pos);
                const float radius = hitbox.radius * xRatio * 2.0f;
                if (hitboxTexture) {
                    drawList->AddImage(hitboxTexture,
                        { center.x - radius, center.y - radius },
                        { center.x + radius, center.y + radius },
                        { 0.0f, 0.0f }, { 0.5f, 0.5f }, IM_COL32(80, 255, 220, 220));
                } else {
                    drawList->AddCircleFilled(center, radius, IM_COL32(40, 220, 180, 75), 16);
                    drawList->AddCircle(center, radius, IM_COL32(80, 255, 220, 220), 16);
                }
            }
            for (const auto& hitbox : g_th15_rect_hit_draw_vec) {
                const ImVec2 p1 = toClient(hitbox.pos1);
                const ImVec2 p2 = toClient(hitbox.pos2);
                if (hitboxTexture) {
                    drawList->AddImage(hitboxTexture, p1, p2, whiteUv, whiteUv,
                        IM_COL32(40, 220, 180, 75));
                } else {
                    drawList->AddRectFilled(p1, p2, IM_COL32(40, 220, 180, 75));
                    drawList->AddRect(p1, p2, IM_COL32(80, 255, 220, 220));
                }
            }
            for (const auto& hitbox : g_th15_laser_hit_draw_vec) {
                const float halfHeight = hitbox.height * 0.5f + std::abs(hitbox.playerHitbox.y);
                const float left = std::abs(hitbox.playerHitbox.x);
                const float right = hitbox.width + left;
                const float cosAngle = std::cos(hitbox.angle);
                const float sinAngle = std::sin(hitbox.angle);
                const auto rotateAndTranslate = [&](float x, float y) {
                    return ImVec2{
                        hitbox.origin.x + x * cosAngle - y * sinAngle,
                        hitbox.origin.y + x * sinAngle + y * cosAngle,
                    };
                };
                const ImVec2 a = toClient(rotateAndTranslate(right, -halfHeight));
                const ImVec2 b = toClient(rotateAndTranslate(right, halfHeight));
                const ImVec2 c = toClient(rotateAndTranslate(-left, halfHeight));
                const ImVec2 d = toClient(rotateAndTranslate(-left, -halfHeight));
                if (!TH15HitboxIsFinite(a) || !TH15HitboxIsFinite(b)
                    || !TH15HitboxIsFinite(c) || !TH15HitboxIsFinite(d))
                    continue;
                if (hitboxTexture) {
                    drawList->AddImageQuad(hitboxTexture, a, b, c, d,
                        whiteUv, whiteUv, whiteUv, whiteUv, IM_COL32(220, 55, 55, 95));
                } else {
                    drawList->AddQuadFilled(a, b, c, d, IM_COL32(220, 55, 55, 95));
                    drawList->AddQuad(a, b, c, d, IM_COL32(255, 110, 110, 230));
                }
            }
            drawList->PopClipRect();
        }

        g_th15_bullet_hit_draw_vec.clear();
        g_th15_rect_hit_draw_vec.clear();
        g_th15_laser_hit_draw_vec.clear();
    }
#else
    static void TH15DrawHitboxes([[maybe_unused]] ImDrawList* drawList) {}
#endif

#if THPRAC_TH15_HITBOX_FEATURE
    HOOKSET_DEFINE(th15_hitbox_hooks)
    EHOOK_DY(th15_circle_hit_test, 0x455D00, 1, {
        TH15CaptureCircleHitboxFromXmm2(pCtx->Esp);
    })
    EHOOK_DY(th15_rect_hit_test, 0x455BE0, 1, {
        TH15CaptureRectHitbox(pCtx->Esp);
    })
    EHOOK_DY(th15_laser_hit_test, 0x455E10, 1, {
        TH15CaptureLaserHitboxFromXmm(pCtx->Esp);
    })
    HOOKSET_ENDDEF()

    static void TH15SetHitboxCaptureHooks(bool enabled)
    {
        const bool types[] = { g_th15CircleHitboxEnabled, g_th15RectHitboxEnabled, g_th15LaserHitboxEnabled };
        size_t index = 0;
        for (auto& hook : th15_hitbox_hooks) {
            hook.Setup();
            hook.Toggle(enabled && types[index++]);
        }
        if (!g_th15CircleHitboxEnabled) g_th15_bullet_hit_draw_vec.clear();
        if (!g_th15RectHitboxEnabled) g_th15_rect_hit_draw_vec.clear();
        if (!g_th15LaserHitboxEnabled) g_th15_laser_hit_draw_vec.clear();
        if (!enabled) {
            g_th15_bullet_hit_draw_vec.clear();
            g_th15_rect_hit_draw_vec.clear();
            g_th15_laser_hit_draw_vec.clear();
        }
    }
#endif


    HOOKSET_DEFINE(THMainHook)
    EHOOK_DY(th15_inf_lives, 0x456397,1,
    {
        if ((*(THOverlay::singleton().mInfLives))) {
            if (!g_adv_igi_options.map_inf_life_to_no_continue) {
                pCtx->Eax++;
            } else {
                if (*(DWORD*)(0x4E7450) == 0)
                    pCtx->Eax++;
            }
        }
    })
    EHOOK_DY(th15_everlasting_bgm, 0x476f10, 1, {
        int32_t retn_addr = ((int32_t*)pCtx->Esp)[0];
        int32_t bgm_cmd = ((int32_t*)pCtx->Esp)[1];
        int32_t bgm_id = ((int32_t*)pCtx->Esp)[2];
        // 4th stack item = i32 call_addr

        bool el_switch;
        bool is_practice;
        bool result;

        el_switch = *(THOverlay::singleton().mElBgm) && !THGuiRep::singleton().mRepStatus && thPracParam.mode && thPracParam.section;
        is_practice = (*((int32_t*)0x4e7794) & 0x1);
        if (thPracParam.mode && thPracParam.section == TH15_ST6_STARS)
            el_switch = false;
        result = ElBgmTest<0x44d413, 0x43cb40, 0x450efd, 0x452a63, 0xffffffff>(
            el_switch, is_practice, retn_addr, bgm_cmd, bgm_id, 0xffffffff);

        if (result) {
            pCtx->Eip = 0x476fa3;
        }
    })
    EHOOK_DY(th15_param_reset, 0x461070, 7, {
        thPracParam.Reset();
        THGuiRep::singleton().mRepStatus = false;
        TH15ChapterSave::singleton().ResetRun();
    })
    EHOOK_DY(th15_prac_menu_1, 0x4677be, 5, {
        THGuiPrac::singleton().State(1);
    })
    EHOOK_DY(th15_prac_menu_2, 0x4677e1, 3, {
        THGuiPrac::singleton().State(2);
    })
    EHOOK_DY(th15_prac_menu_3, 0x467b2a, 7, {
        THGuiPrac::singleton().State(3);
    })
    EHOOK_DY(th15_prac_menu_4, 0x467bcb, 7, {
        THGuiPrac::singleton().State(4);
    })
    PATCH_DY(th15_prac_menu_enter_1, 0x4678a2, "eb")
    EHOOK_DY(th15_prac_menu_enter_2, 0x467b8a, 1, {
        pCtx->Ecx = thPracParam.stage;
    })
    PATCH_DY(th15_disable_prac_menu_1, 0x467d31, "eb3b")
    EHOOK_DY(th15_menu_rank_fix, 0x4527be, 5, {
        *((int32_t*)0x4e7424) = -1;
        *((int32_t*)0x4e7410) = *((int32_t*)0x4e0ef4);
    })
    EHOOK_DY(th15_patch_main, 0x43c68c, 1, {
        // Keep both chapter hooks active for ordinary chapter transitions.
        // Clear requests left over from the previous run instead.
        chapter_set = -1;
        chapter_disable = 0;
        th15_stars_bgm_sync.Disable();
        if (thPracParam.mode == 1) {
            *(int32_t*)(0x4E740C) = (int32_t)(thPracParam.score / 10);
            *(int32_t*)(0x4E7450) = thPracParam.life;
            *(int32_t*)(0x4E7454) = thPracParam.life_fragment;
            *(int32_t*)(0x4E745C) = thPracParam.bomb;
            *(int32_t*)(0x4E7460) = thPracParam.bomb_fragment;
            *(int32_t*)(0x4E7440) = thPracParam.power;
            *(int32_t*)(0x4E7434) = thPracParam.value * 100;
            *(int32_t*)(0x4E741C) = thPracParam.graze; // 0x4E7420: Chapter Graze

            THSectionPatch();
            if (thPracParam.reisen_shield) {
                uintptr_t bombPtr = GetMemContent(BOMB_PTR);
                *(int32_t*)(bombPtr + 0x24) = 1; // active bomb
                *(int32_t*)(bombPtr + 0x2c) = 31; // skip frame 30 of active state (plays shield SFX)
                *(int32_t*)(bombPtr + 0x60) = thPracParam.reisen_shield; // shield count
                MakeReisenShieldANM(bombPtr, thPracParam.reisen_shield);
            }
        }
        thPracParam._playLock = true;
    })
    EHOOK_DY(th15_bgm, 0x43d5c1, 2, {
        if (THBGMTest()) {
            PushHelper32(pCtx, 1);
            pCtx->Eip = 0x43d5c3;
        }
    })
    EHOOK_DY(th15_rep_save, 0x45cc49, 5, {
        char* repName = (char*)(pCtx->Esp + 0x38);
        if (thPracParam.mode)
            THSaveReplay(repName);
    })
    EHOOK_DY(th15_rep_menu_1, 0x468197, 3, {
        THGuiRep::singleton().State(1);
    })
    EHOOK_DY(th15_rep_menu_2, 0x4682b0, 5, {
        THGuiRep::singleton().State(2);
    })
    EHOOK_DY(th15_rep_menu_3, 0x468474, 2, {
        THGuiRep::singleton().State(3);
    })

     EHOOK_DY(th15_update, 0x4015fa, 1, {
        GameGuiBegin(IMPL_WIN32_DX9,
            !THAdvOptWnd::singleton().IsOpen() && !TH15HitboxWnd::singleton().IsOpen()
                && !TH15RouteWnd::singleton().IsOpen());

        // Gui components update
        THGuiPrac::singleton().Update();
        THGuiRep::singleton().Update();
        THOverlay::singleton().Update();
        TH15InGameInfo::singleton().Update();
        TH15ChapterSave::singleton().Update(
            *(int32_t*)0x4E73F0, *(int32_t*)0x4E73F8,
            TH15InGameInfo::singleton().mMissCount,
            TH15InGameInfo::singleton().mBombCount, thPracParam.mode,
            thPracParam.section, thPracParam.stage,
            *(int32_t*)0x4E7410, *(int32_t*)0x4E7404,
            THGuiRep::singleton().mRepStatus);

        TH15RouteTrace::singleton().OnFrame();
        auto p = ImGui::GetOverlayDrawList();
        TH15DrawHitboxes(p);
        TH15RouteTrace::singleton().Draw(p);

        // in case boss movedown do not disabled when playing normal games
        {
            if (THAdvOptWnd::singleton().forceBossMoveDown) {
                auto sz = ImGui::CalcTextSize(S(TH_BOSS_FORCE_MOVE_DOWN));
                const float y = ImGui::GetTextLineHeightWithSpacing();
                p->AddRectFilled({ 64.0f, y }, { sz.x + 64.0f, y + sz.y }, 0xFFCCCCCC);
                p->AddText({ 64.0f, y }, 0xFFFF0000, S(TH_BOSS_FORCE_MOVE_DOWN));
            }
        }

        // ab test
        {
            if (thPracParam.mode && thPracParam.section == TH15_ST8_AB_TEST) {
                ABTestRender();
            }
        }
        SSS::SSS_Update(15);
        if (g_adv_igi_options.show_keyboard_monitor && GetMemContent(PLAYER_PTR))
            KeysHUD(15, { 1280.0f, 0.0f }, { 840.0f, 0.0f }, g_adv_igi_options.keyboard_style);

        RenderLockTimer(p);
        RenderBossAndPlayerStatus(p);
        GameUpdateOuter(p, 15);

        // Read each key once and resolve exclusivity before drawing any page.
        const bool advancedChordPressed = Gui::GetChordPressed(Gui::GetAdvancedMenuChord());
        const bool featuresChordPressed = AddonEnabled(Addon::Settings) && Gui::GetChordPressed(1 << Gui::ChordKey_F11);
        const bool routePressed = Gui::KeyboardInputUpdate(VK_F10) == 1;
        const bool featuresPressed = featuresChordPressed && !routePressed;
        const bool advancedPressed = advancedChordPressed && !featuresChordPressed && !routePressed;
        const auto closeRouteForSwitch = [] {
            auto& route = TH15RouteWnd::singleton();
            if (!route.IsClosed()) {
                TH15RouteTrace::singleton().SaveSettings();
                route.CloseImmediately();
            }
        };
        if (featuresPressed && TH15HitboxWnd::singleton().IsClosed()) {
            THAdvOptWnd::singleton().CloseImmediately();
            closeRouteForSwitch();
        }
        if (advancedPressed && THAdvOptWnd::singleton().IsClosed()) {
            TH15HitboxWnd::singleton().CloseImmediately();
            closeRouteForSwitch();
        }
        if (routePressed && TH15RouteWnd::singleton().IsClosed()) {
            THAdvOptWnd::singleton().CloseImmediately();
            TH15HitboxWnd::singleton().CloseImmediately();
        }
        const bool advOptWndOpen = THAdvOptWnd::StaticUpdate(advancedPressed);
        const bool hitboxWndOpen = AddonEnabled(Addon::Settings) && TH15HitboxWnd::StaticUpdate(featuresPressed);
        const bool routeWndOpen = TH15RouteWnd::StaticUpdate(routePressed);
        bool drawCursor = advOptWndOpen || hitboxWndOpen || routeWndOpen
            || THGuiPrac::singleton().IsOpen();
        if (GetMemContent(PLAYER_PTR)) {
            ReportPluginStage({
                GetMemContent<int>(0x4e73f0) - 1, GetMemContent<int>(CHARACTER),
                thPracParam.mode != 0, THGuiRep::singleton().mRepStatus
            });
        } else {
            ReportPluginStage(nullptr);
        }
        GameGuiEnd(drawCursor);
    })
    EHOOK_DY(th15_player_state, 0x454a70,1,{
        if (g_adv_igi_options.show_keyboard_monitor)
            RecordKey(15, *(DWORD*)(0x004E6F28));
    })
     EHOOK_DY(th15_render, 0x40170a, 1, {
         if (!GameGuiUsesDx9OverlayScene()) GameGuiRender(IMPL_WIN32_DX9);
     })
    HOOKSET_ENDDEF()

    HOOKSET_DEFINE(THInGameInfo)
    // Item type 1 is the ordinary point item. Count it at the point-item
    // handler entry, where the original instruction is a single-byte prologue.
    EHOOK_DY(th15_blue_point, 0x4405c0, 1, {
        TH15ChapterSave::singleton().OnBluePointCollected();
    })
    EHOOK_DY(th15_game_start, 0x43E6EE,7, // gamestart-bomb set
    {
        ReportPluginStage(nullptr);
        auto& chapterSave = TH15ChapterSave::singleton();
        chapterSave.SetCaptureRateEnabled(g_th15CaptureRateEnabled || g_th15ScoreRecordingEnabled || g_th15ChapterHudEnabled);
        g_th15OnlyCurrentPracticeChapterThisRun = thPracParam.mode == 1
            && g_th15OnlyCurrentPracticeChapter;
        g_th15NativeCaptureRateEnabled = g_th15CaptureRateEnabled
            && !g_th15OnlyCurrentPracticeChapterThisRun;
        chapterSave.SetOnlyCurrentPracticeChapter(g_th15OnlyCurrentPracticeChapterThisRun);
        TH15InGameInfo::singleton().mBombCount = 0;
        TH15InGameInfo::singleton().mMissCount = 0;
        chapterSave.BeginRun(
            *(int32_t*)0x4E73F0, 0, 0,
            thPracParam.mode, thPracParam.section, thPracParam.stage,
            *(int32_t*)0x4E7410, *(int32_t*)0x4E7404,
            THGuiRep::singleton().mRepStatus);
    })
    EHOOK_DY(th15_bomb_dec, 0x41497A,5, // bomb dec
    {
        TH15InGameInfo::singleton().mBombCount++;
    })
    EHOOK_DY(th15_life_dec, 0x456398,5, // life dec
    {
       TH15InGameInfo::singleton().mMissCount++;
       FastRetry(thPracParam.mode);
    })
    EHOOK_DY(th15_lock_timer1, 0x43404A,10, // initialize
    {
        g_lock_timer = 0;
    })
    EHOOK_DY(th15_lock_timer2, 0x42C738,4, // SetNextPattern case 514
    {
        g_lock_timer = 0;
    })
    EHOOK_DY(th15_lock_timer3, 0x42B938,2,// set boss mode case 512
    {
        g_lock_timer = 0;
    })
    EHOOK_DY(th15_lock_timer4, 0x4301E8,6, // decrease time (update)
    {
        g_lock_timer_flag = true;
    })
    HOOKSET_ENDDEF()
    static __declspec(noinline) void THGuiCreate()
    {
        if (ImGui::GetCurrentContext()) {
            return;
        }
#if THPRAC_TH15_HITBOX_FEATURE
        // Reserve a typical frame's workload up front. These vectors grow as
        // needed so fast replay never drops older hitboxes from the frame.
        g_th15_bullet_hit_draw_vec.reserve(1024);
        g_th15_rect_hit_draw_vec.reserve(512);
        g_th15_laser_hit_draw_vec.reserve(256);
#endif
        // Init
        GameGuiInit(IMPL_WIN32_DX9, 0x4e77d8, 0x519bb0,
            Gui::INGAGME_INPUT_GEN2, 0x4e6d1c, 0x4e6d18, 0,
            (*((int32_t*)0x51bbec) >> 2) & 0xf);
        GameGuiEnableDx9OverlayScene();
#if THPRAC_TH15_HITBOX_FEATURE
        TH15SetHitboxCaptureHooks(false);
#endif
        SetDpadHook(0x401D22, 3);
        // Gui components creation
        THGuiPrac::singleton();
        THGuiRep::singleton();
        THOverlay::singleton();
        TH15InGameInfo::singleton();
        TH15HitboxWnd::singleton();
        TH15RouteWnd::singleton();

        // Hooks
        EnableAllHooks(THMainHook);
        EnableAllHooks(THInGameInfo);
        th15_native_spell_attempt.Setup();
        th15_native_spell_attempt.Enable();
        th15_native_spell_attempt_total.Setup();
        th15_native_spell_attempt_total.Enable();
        th15_native_spell_capture.Setup();
        th15_native_spell_capture.Enable();
        th15_native_spell_capture_total.Setup();
        th15_native_spell_capture_total.Enable();
        th15_chapter_set.Setup();
        th15_chapter_set.Enable();
        th15_chapter_bullets_cleared.Setup();
        th15_chapter_bullets_cleared.Enable();
        th15_chapter_disable.Setup();
        th15_chapter_disable.Enable();
        th15_chapter_settlement.Setup();
        th15_chapter_settlement.Enable();
        th15_boss_hp_runtime.Setup();
        th15_boss_hp_runtime.Enable();
        th15_st7boss1_chapter_bonus.Setup();
        th15_stars_bgm_sync.Setup();

        // Reset thPracParam
        thPracParam.Reset();
    }
    HOOKSET_DEFINE(THInitHook)
    PATCH_DY(th15_disable_demo, 0x460900, "ffffff7f")
    PATCH_DY(th15_disable_mutex, 0x4713ec, "90e9")
    PATCH_DY(th15_startup_1, 0x46055f, "9090")
    PATCH_DY(th15_startup_2, 0x4610c3, "eb")
    EHOOK_DY(th15_gui_init_1, 0x4617ff, 3, {
        self->Disable();
        THGuiCreate();
    })
    EHOOK_DY(th15_gui_init_2, 0x47341b, 1, {
        self->Disable();
        THGuiCreate();
    })
    HOOKSET_ENDDEF()
}

void TH15Init()
{
    EnableAllHooks(TH15::THInitHook);
    InitHook(15,(void*)0x47356c, (void*)0x47333d);
    if (GetModuleHandleA("vpatch_th15.dll")) {
        InitHook(15,(void*)((DWORD)GetModuleHandleA("vpatch_th15.dll") + 0x6bd9));
    }
}
}
