#pragma once

// Cross-title player-route reference.
//
// A route is recorded while a replay plays: the player position is sampled once
// per game frame and stored per stage. During normal play the saved path is
// drawn as an overlay so a practice attempt can follow the recorded movement.
//
// Like TH15, recording preserves each logic frame and playback follows native
// game clocks. Native ECL locations saved with the replay establish the start
// of a practice jump without depending on player movement.
//
// TH15 keeps its own implementation; this engine covers the other integer
// titles. It is driven from GameUpdateInner/GameUpdateOuter so every title uses
// the same F10 window.

#include "thprac_gui_components.h"
#include "thprac_player_offsets.h"
#include "thprac_route_sync.h"

#include <string>
#include <vector>
#include <initializer_list>

namespace THPrac {

struct RouteNativeState {
    int frame = -1;
    int segment = 0;
    uintptr_t generation = 0;
    std::vector<RouteSync::ScriptStamp> scripts;
};
bool RouteReadNativeState(int game, RouteNativeState& state);
void RouteNativeInvalidate();

// Everything a title has to provide. Addresses and pointer chains live in the
// per-title translation unit; the engine only sees these callbacks.
struct RouteTraceProfile {
    bool configured = false;
    int game = 0;
    // %APPDATA% subdirectory that holds the game's data (L"ShanghaiAlice\\th16").
    const wchar_t* appdata = nullptr;
    int stageCount = 7;
    // Player position in the unified stage space: x is centred on 0 (the
    // playfield spans -192..192), y starts at 0 at the top and ends at 448.
    // Returns false whenever the value cannot be read safely.
    bool (*readPlayer)(float& x, float& y) = nullptr;
    // 0-based stage index, negative when no stage is running.
    int (*readStage)() = nullptr;
    int (*readDifficulty)() = nullptr;
    int (*readShot)() = nullptr;
    bool (*replayActive)() = nullptr;
    // Optional: name of the replay being played (no path).
    const wchar_t* (*replayName)() = nullptr;
    // Selected practice target, 0 when playing normally.
    int (*practiceSection)() = nullptr;
    int (*practicePhase)() = nullptr;
    // Optional native writer, including the collision position and mirrors.
    bool (*writePlayer)(float x, float y) = nullptr;
    // Player object: global that holds the pointer, and the float pair offset
    // inside it. Static legacy players leave this pair unset.
    uintptr_t playerPointer = 0;
    int playerOffset = 0;
    // Subtracted from the raw x to obtain the centred coordinate (192 for the
    // 0..384 titles, 0 for the modern centred ones).
    float playerXOffset = 0.0f;
};

// Per-title slot, filled once from the title's own THGuiCreate.
RouteTraceProfile& RouteProfileFor(int game);

// Reads the player position pair through a global pointer plus a struct offset.
// The position field is fixed by the title's verified profile.
bool ReadPlayerPair(uintptr_t pointerGlobal, int offset, float xOffset, float& x, float& y);
bool WritePlayerPair(uintptr_t pointerGlobal, int offset, float xOffset, float x, float y,
    float fixedScale = 128.0f);
bool WriteStaticPlayer(uintptr_t player, unsigned position, std::initializer_list<unsigned> mirrors,
    float xOffset, float x, float y);

class RouteTrace {
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

    static RouteTrace& Instance();

    // Per game frame (GameUpdateInner); optional follow writes are session-only.
    void OnFrame(int game);
    // Inside the ImGui frame (GameUpdateOuter).
    void Draw(ImDrawList* drawList, int game);
    void Refresh(int game);
    void NewRun(int game);

    bool HasProfile() const { return mProfile && mProfile->configured; }
    int Game() const { return mGame; }
    bool CanFollow() const { return mProfile && mProfile->writePlayer; }
    size_t LoadedPointCount() const { return loadedPoints.size(); }

    // Display settings, saved to thprac_route/settings.tsv.
    bool displayEnabled = false;
    float displayAlpha = 0.55f;
    float displayThickness = 2.5f;
    ImVec4 displayColor = ImVec4(0.11f, 0.78f, 0.42f, 1.00f);
    bool displayMarkers = true;
    bool displayLabel = true;
    bool autoMatch = true;
    bool windowEnabled = true;
    float windowBackSeconds = 2.0f;
    float windowAheadSeconds = 1.0f;
    // Manual trim applied after resolving the native script anchor.
    int practiceSkipAdjust = 0;
    bool followPlayerToRoute = false; // Session only; never persisted.
    bool timedRoute = false;

    // Playback state, mirrored for the F10 panel. The reference advances with
    // the engine clock from an anchor sample, like TH15's chapter clock.
    RouteSync::Anchor anchor;
    bool anchorFromMark = false;
    std::string anchorNote;
    int playbackSample = -1;
    int playbackClock = 0;
    // Native script anchor sample, before the manual trim. The trim is
    // applied live so the slider does not need a re-alignment.
    int anchorBase = -1;

    // Extraction.
    bool autoExtractReplay = true;
    bool extractArmed = false;
    std::wstring armedReplay;

    std::vector<RouteInfo> routes;
    int selectedRoute = -1;
    // Full paths, not bare names: the pre-TH10 titles keep their replays next
    // to the game while the later ones use %APPDATA%.
    std::vector<std::wstring> replayFiles;
    int replaySelection = -1;
    std::string pendingDelete;

    // Live capture state, shown in the window.
    bool capturing = false;
    std::string captureLabel;
    int captureStage = 0;
    int captureDiff = 0;
    int captureShot = 0;
    size_t capturePointCount = 0;
    int playerOffset = 0;
    float playerXOffset = 0.0f;
    bool offsetKnown = false;

    // Diagnostics. One file per title under thprac_route\route.log, written on
    // state changes plus a summary every few seconds, so a player can reproduce
    // a problem in game and hand the log over instead of describing it.
    bool logEnabled = true;
    std::string logPath;
    int logLines = 0;

    static constexpr const char* kRouteDir = "thprac_route";
    static constexpr int kMaxPoints = 150000;
    static constexpr float kSamplesPerSecond = 60.0f;

private:
    RouteTrace() = default;

    void EnsureProfile(int game);
    void LoadSettings();
    void SaveSettings();
    void ScanRoutes();
    void ScanReplays();
    void EnsureLoaded(const std::string& file);
    const RouteInfo* ResolveTarget(int stage, int diff, int shot);
    void DeleteRoute(const std::string& file);
    void ArmReplay(const std::wstring& name);

    void BeginCapture(int stage, int diff, int shot, int source);
    void FinishCapture(bool save);
    bool SaveCapture();
    void TryStartCapture();
    void SamplePlayer();
    void ResetPlayback();
    void Log(const std::string& text);
    void ClearLog();
    bool LoadRouteFile(const std::wstring& path, std::vector<ImVec2>& points,
        std::vector<uint8_t>& breaks, std::vector<RouteSync::SectionMark>& sections);
    bool AcquireAnchor(int practiceSection);

    static std::wstring RouteDirectory(const RouteTraceProfile& profile);
    // Every directory a title may keep replays in: %APPDATA%, the game folder
    // and the working directory, with and without a replay subfolder.
    static std::vector<std::wstring> ReplayDirectories(const RouteTraceProfile& profile);

    RouteTraceProfile* mProfile = nullptr;
    int mGame = 0;
    bool initialized = false;
    bool profileLogged = false;
    bool lastReplayActive = false;
    int lastStage = -1;
    int lastPracticeSection = 0;
    int lastPracticePhase = 0;
    RouteNativeState nativeState;
    RouteSync::FrameGate frameGate;
    bool restartPending = false;
    bool extractSession = false;
    bool captureLimitReached = false;
    bool runtimeActive = false;
    int captureLastFrame = -1;
    struct FrameRecord {
        int frame = -1;
        int segment = 0;
        std::vector<RouteSync::ScriptStamp> scripts;
    };
    std::vector<FrameRecord> captureFrames;
    std::vector<FrameRecord> loadedFrames;
    RouteSync::ScriptIndex scriptIndex;
    std::map<int, int> clockSamples;
    int segmentBegin = 0;
    int segmentEnd = 0;

    std::vector<ImVec2> capturePoints;
    std::vector<uint8_t> captureBreaks;
    std::vector<ImVec2> loadedPoints;
    std::vector<uint8_t> loadedBreaks;
    std::vector<RouteSync::SectionMark> loadedSections;
    std::vector<ImVec2> segmentBuffer;
    std::string loadedFile;
    std::string settingsSelection;
    bool settingsSelectionPending = false;

    int playClock = 0;

public:
    // Used by the F10 window.
    void SetSettingsDirty() { SaveSettings(); }
    void ScanForUi() { ScanRoutes(); ScanReplays(); }
    void DeleteForUi(const std::string& file) { DeleteRoute(file); }
    void ArmForUi(const std::wstring& name) { ArmReplay(name); }
    void ReloadForUi() { LoadSettings(); ScanRoutes(); ScanReplays(); }
    void ClearLogForUi() { ClearLog(); }

};

class RouteTraceWnd : public Gui::GameGuiWnd {
    RouteTraceWnd() noexcept
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
    SINGLETON(RouteTraceWnd)

public:
    // Returns true while the window is open (the caller must keep the cursor).
    static bool StaticUpdate(bool togglePressed)
    {
        auto& wnd = RouteTraceWnd::singleton();
        if (togglePressed) {
            if (wnd.IsClosed()) {
                RouteTrace::Instance().Refresh(RouteTrace::Instance().Game());
                wnd.Open();
            } else {
                wnd.Close();
                RouteTrace::Instance().SetSettingsDirty();
            }
        }
        wnd.Update();
        return !wnd.IsClosed();
    }

protected:
    void OnLocaleChange() override
    {
        const int locale = Gui::LocaleGet();
        SetTitle(locale == Gui::LOCALE_ZH_CN ? "路线参考（F10）"
            : locale == Gui::LOCALE_JA_JP ? "ルート参考（F10）" : "Route reference (F10)");
        SetSizeRel(1.0f, 1.0f);
        SetPosRel(0.0f, 0.0f);
        SetItemWidthRel(-0.0f);
        SetAutoSpacing(true);
    }

    void OnContentUpdate() override;
};

// F10 handling shared by every title: returns true while the window is open.
bool RouteTraceWindowTogglePressed();
bool RouteTraceWindowUpdate(bool togglePressed);
void RouteTraceWindowClose();
// Draws the overlay and updates the window inside the ImGui frame.
void RouteTraceFrameDraw(ImDrawList* drawList, int game);

} // namespace THPrac
