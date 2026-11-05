#pragma once

// THP-owned Boss non-spell / spell capture statistics.
//
// The runtime deliberately separates three events that games report at
// different moments:
//   * an attack really starts (player has control, the attack exists);
//   * the game declares the attack finished (boss killed or timer expired);
//   * the player failed (miss / bomb) before that settlement.
// Leaving a stage, retrying, a replay, a boss disappearing or a timer reset are
// not settlements. Game adapters must call AbortAttack() for those.
//
// Native game saves (score06.dat, score20pyra.dat, ...) are never touched.

#include <cstdint>
#include <format>
#include <map>
#include <string>

struct ImDrawList;
struct ImVec2;

namespace THPrac {
namespace BossCapture {

enum AttackKind : int {
    kNonSpell = 0,
    kSpell = 1,
};

enum Outcome : int {
    kCaptured = 0,
    kTimedOut = 1,
};

// Stage 0 is reserved for attacks that do not belong to any single stage. TH08
// Last Words reuse the home stage of the character whose ECL file holds them,
// so they would otherwise be spread over nine stage rows; they share this group.
constexpr int kGroupedStage = 0;

struct Key {
    int game = 0; // integer titles only: 6..20, excluding 9/15/19
    int stage = 0;
    int attack = 0; // 1-based ordinal of the attack inside the stage
    int kind = kNonSpell;
    int difficulty = 0;
    int shot = 0;

    bool operator<(const Key& other) const
    {
        if (game != other.game) return game < other.game;
        if (stage != other.stage) return stage < other.stage;
        if (attack != other.attack) return attack < other.attack;
        if (kind != other.kind) return kind < other.kind;
        if (difficulty != other.difficulty) return difficulty < other.difficulty;
        return shot < other.shot;
    }
    bool operator==(const Key& other) const
    {
        return game == other.game && stage == other.stage && attack == other.attack
            && kind == other.kind && difficulty == other.difficulty && shot == other.shot;
    }
    bool Valid() const;
};

struct Counts {
    uint32_t attempts = 0;
    uint32_t captures = 0;
    uint32_t timeouts = 0;
};

// Game, difficulty, shot and replay state. Adapters update this whenever the
// game reports a change; the runtime never guesses these values.
struct Context {
    int game = 0;
    int difficulty = 0;
    int shot = 0;
    bool replay = false;
    bool practice = false;

    bool operator==(const Context& other) const
    {
        // practice only labels the source; story and practice share counters.
        return game == other.game && difficulty == other.difficulty
            && shot == other.shot && replay == other.replay;
    }
};

class Store {
public:
    using Records = std::map<Key, Counts>;

    explicit Store(std::wstring path = DefaultPath());

    // Returns false when the file is missing or rejected. A rejected file is
    // renamed with a .corrupt suffix so that no later save hides the evidence.
    bool Load();
    bool Save();
    bool Loaded() const { return loaded; }
    bool Corrupt() const { return corrupt; }

    const Records& Total() const { return total; }
    const Records& Session() const { return session; }
    void ClearSession();
    void ClearTotal();
    void ClearAll();

    bool Enabled() const { return enabled; }
    void SetEnabled(bool value);
    bool ShowHud() const { return showHud; }
    void SetShowHud(bool value);

    bool Dirty() const { return dirty; }
    void MarkDirty() { dirty = true; }
    const std::wstring& Path() const { return path; }
    static std::wstring DefaultPath();

    // Counting primitives. They return false when statistics are disabled.
    bool RecordAttempt(const Key& key);
    bool RecordCapture(const Key& key);
    bool RecordTimeout(const Key& key);
    // Native-counter adapters see a finished attack, not its start: one call
    // records every attempt of the batch and how many of them captured.
    bool RecordCompleted(const Key& key, uint32_t captures, uint32_t failures);

private:
    Records total;
    Records session;
    std::wstring path;
    bool enabled = true;
    bool showHud = true;
    bool dirty = false;
    bool corrupt = false;
    bool loaded = false;
};

class Runtime {
public:
    explicit Runtime(std::wstring storePath = std::wstring());

    Store& Storage() { return store; }
    const Store& Storage() const { return store; }

    void SetContext(const Context& value);
    const Context& GetContext() const { return context; }

    // A duplicate call for the same still-active attack is ignored, so polling
    // adapters cannot inflate the attempt counter. A different attack while one
    // is active implicitly aborts the old attempt (it stays in the denominator).
    bool BeginAttack(int stage, int attack, int kind, const std::string& name = {});
    void MarkFailure();
    bool Settle(Outcome outcome);
    void AbortAttack();

    // Adapters that only observe native end-of-attack counters use this instead
    // of Begin/Settle. Attempts = captures + failures for the batch.
    bool RecordCompleted(int stage, int attack, int kind, uint32_t captures,
        uint32_t failures, const std::string& name = {});

    bool Active() const { return active; }
    bool Failed() const { return active && failed; }
    const Key& ActiveKey() const { return activeKey; }
    const std::map<Key, std::string>& Names() const { return names; }

    // Saves at most every few seconds; call once per frame from the game loop.
    void Tick();
    void Flush();
    // Loads the database on first use, including from the settings page.
    void EnsureLoaded();

    static double Rate(const Counts& counts);

private:
    Store store;
    Context context;
    std::map<Key, std::string> names;
    Key activeKey;
    bool active = false;
    bool failed = false;
    bool settled = false;
    uint64_t lastSaveMs = 0;
    bool savePending = false;

    void CancelActive();
};

Runtime& Instance();

// Canonical ECL adapters supersede legacy native-counter mirrors when attached.
bool NativeBridgeActive();
void SetNativeBridgeActive(bool active);

// Adapter helpers: a game module only reports facts it can prove.
void SetContext(const Context& context);
bool BeginAttack(int stage, int attack, int kind, const std::string& name = {});
void MarkFailure();
bool Settle(Outcome outcome);
void AbortAttack();
bool RecordCompleted(int stage, int attack, int kind, uint32_t captures,
    uint32_t failures, const std::string& name = {});

// thprac_boss_capture_ui.cpp: F11 section and in-game HUD.
void DrawSettingsSection();
void DrawInfoPanelCapture();
void DrawFailureSlash(const ImVec2& minimum, const ImVec2& maximum);

// Diagnostics: one line per event next to the store file (boss_capture.log,
// rotated to .old at 256 KB). Adapters cannot be verified without the game, so
// a failed identification has to leave a trace.
void AppendLog(const std::string& line);
template <typename... Args>
inline void Log(std::format_string<Args...> format, Args&&... args)
{
    AppendLog(std::vformat(format.get(), std::make_format_args(args...)));
}

}
}
