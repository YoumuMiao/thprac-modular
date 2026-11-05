#include "route_service.h"
#include "thprac_games.h"
#include "thprac_route_trace.h"
#include "thprac_utils.h"
#include "thprac_gui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <sstream>
#include <string>
#include <vector>
#include <shlobj.h>

namespace THPrac {
namespace {

RouteTraceProfile g_routeProfiles[32];

std::wstring WideFromUtf8(const std::string& text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(size > 0 ? size : 0, L'\0');
    if (size > 1)
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    if (!result.empty()) result.pop_back();
    return result;
}

std::string Utf8FromWide(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(size > 0 ? size : 0, '\0');
    if (size > 1)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    if (!result.empty()) result.pop_back();
    return result;
}

std::wstring ExpandPath(const std::wstring& path)
{
    wchar_t buffer[MAX_PATH * 2] = {};
    if (!ExpandEnvironmentStringsW(path.c_str(), buffer, MAX_PATH * 2))
        return path;
    return buffer;
}

bool ReadTextFile(const std::wstring& path, std::string& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > (64ll << 20)) {
        CloseHandle(file);
        return false;
    }
    text.assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < text.size()) {
        DWORD read = 0;
        if (!ReadFile(file, text.data() + offset, static_cast<DWORD>(text.size() - offset), &read, nullptr)
            || read == 0)
            break;
        offset += read;
    }
    CloseHandle(file);
    text.resize(offset);
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::string& text, bool createNew = false)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        createNew ? CREATE_NEW : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    size_t offset = 0;
    bool ok = true;
    while (offset < text.size()) {
        DWORD written = 0;
        if (!WriteFile(file, text.data() + offset, static_cast<DWORD>(text.size() - offset), &written, nullptr)
            || written == 0) {
            ok = false;
            break;
        }
        offset += written;
    }
    CloseHandle(file);
    return ok;
}

// The calibration scans a few thousand offsets inside the player object. The
// region is validated once per attempt so the scan itself can use plain reads.
bool MemoryReadable(uintptr_t address, size_t size)
{
    if (!address)
        return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)))
        return false;
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const auto base = reinterpret_cast<uintptr_t>(region.BaseAddress);
    return address + size > address && address + size <= base + region.RegionSize;
}

inline float Distance(const ImVec2& a, const ImVec2& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

RouteTraceProfile& RouteProfileFor(int game)
{
    static RouteTraceProfile fallback;
    if (game < 0 || game >= static_cast<int>(std::size(g_routeProfiles)))
        return fallback;
    return g_routeProfiles[game];
}

RouteTrace& RouteTrace::Instance()
{
    static RouteTrace trace;
    return trace;
}

bool ReadPlayerPair(uintptr_t pointerGlobal, int offset, float xOffset, float& x, float& y)
{
    if (!pointerGlobal || offset <= 0)
        return false;
    const uintptr_t player = *reinterpret_cast<const uintptr_t*>(pointerGlobal);
    if (!player || !MemoryReadable(player + offset, sizeof(float) * 2))
        return false;
    const float rawX = *reinterpret_cast<const float*>(player + offset);
    const float rawY = *reinterpret_cast<const float*>(player + offset + 4);
    if (!std::isfinite(rawX) || !std::isfinite(rawY))
        return false;
    // Reject transitions and menu values; the playfield is 384x448.
    if (rawX < -300.0f || rawX > 500.0f || rawY < -100.0f || rawY > 560.0f)
        return false;
    x = rawX - xOffset;
    y = rawY;
    return x >= -260.0f && x <= 260.0f && y >= -80.0f && y <= 640.0f;
}

bool WritePlayerPair(uintptr_t pointerGlobal, int offset, float xOffset, float x, float y, float fixedScale)
{
    if (!std::isfinite(x) || !std::isfinite(y) || x < -192 || x > 192 || y < 0 || y > 448)
        return false;
    const uintptr_t player = BossCaptureReadPointer(pointerGlobal);
    if (!player || offset <= 0 || !MemoryReadable(player + offset, 0x14)) return false;
    const int fixedX = static_cast<int>((x + xOffset) * fixedScale);
    const int fixedY = static_cast<int>(y * fixedScale);
    *reinterpret_cast<int*>(player + offset + 0xc) = fixedX;
    *reinterpret_cast<int*>(player + offset + 0x10) = fixedY;
    *reinterpret_cast<float*>(player + offset) = fixedX / fixedScale;
    *reinterpret_cast<float*>(player + offset + 4) = fixedY / fixedScale;
    return true;
}

bool WriteStaticPlayer(uintptr_t player, unsigned position, std::initializer_list<unsigned> mirrors,
    float xOffset, float x, float y)
{
    if (!std::isfinite(x) || !std::isfinite(y) || x < -192 || x > 192 || y < 0 || y > 448
        || !MemoryReadable(player + position, 8)) return false;
    for (auto offset : mirrors) if (!MemoryReadable(player + offset, 8)) return false;
    const float dx = x + xOffset - *reinterpret_cast<float*>(player + position);
    const float dy = y - *reinterpret_cast<float*>(player + position + 4);
    for (auto offset : mirrors) {
        *reinterpret_cast<float*>(player + offset) += dx;
        *reinterpret_cast<float*>(player + offset + 4) += dy;
    }
    *reinterpret_cast<float*>(player + position) = x + xOffset;
    *reinterpret_cast<float*>(player + position + 4) = y;
    return true;
}

std::wstring RouteTrace::RouteDirectory(const RouteTraceProfile& profile)
{
    wchar_t appData[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) || !profile.appdata)
        return {};
    std::wstring path(appData);
    path += L"\\";
    path += profile.appdata;
    path += L"\\";
    path += L"thprac_route";
    return path;
}

// Directory the running executable lives in. The pre-TH10 titles write their
// replays next to the game instead of under %APPDATA%.
std::wstring GameDirectory()
{
    static std::wstring cached;
    static bool resolved = false;
    if (resolved)
        return cached;
    resolved = true;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
        return cached;
    std::wstring text(path);
    const size_t slash = text.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        text.resize(slash + 1);
    cached = text;
    return cached;
}

std::wstring WorkingDirectory()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetCurrentDirectoryW(MAX_PATH, path);
    if (!length || length >= MAX_PATH)
        return {};
    std::wstring text(path);
    if (!text.empty() && text.back() != L'\\')
        text += L'\\';
    return text;
}

std::wstring FileNameOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::vector<std::wstring> RouteTrace::ReplayDirectories(const RouteTraceProfile& profile)
{
    std::vector<std::wstring> directories;
    const auto add = [&directories](std::wstring path) {
        if (path.empty())
            return;
        if (path.back() != L'\\')
            path += L'\\';
        for (const auto& existing : directories) {
            if (_wcsicmp(existing.c_str(), path.c_str()) == 0)
                return;
        }
        directories.push_back(std::move(path));
    };
    wchar_t appData[MAX_PATH] = {};
    if (profile.appdata && GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) {
        std::wstring base(appData);
        base += L"\\";
        base += profile.appdata;
        add(base + L"\\replay");
    }
    // EoSD, PCB, IN, MoF, SA and UFO keep replays in the game folder; some
    // copies use a replay subfolder, others drop the .rpy straight into the
    // root. The working directory is checked as well because the old titles
    // open the files through a relative path.
    const std::wstring game = GameDirectory();
    add(game);
    add(game + L"replay");
    const std::wstring working = WorkingDirectory();
    if (!working.empty() && _wcsicmp(working.c_str(), game.c_str()) != 0) {
        add(working);
        add(working + L"replay");
    }
    return directories;
}

void RouteTrace::EnsureProfile(int game)
{
    if (mProfile && mGame == game)
        return;
    mGame = game;
    mProfile = &RouteProfileFor(game);
    nativeState = {};
    frameGate.Reset();
    extractSession = false;
    captureLimitReached = false;
    initialized = false;
    lastReplayActive = false;
    lastStage = -1;
    lastPracticeSection = 0;
    capturing = false;
    loadedFile.clear();
    loadedPoints.clear();
    loadedBreaks.clear();
    loadedSections.clear();
    loadedFrames.clear();
    scriptIndex.Clear();
    clockSamples.clear();
    timedRoute = false;
    routes.clear();
    selectedRoute = -1;
    replayFiles.clear();
    replaySelection = -1;
    capturePoints.clear();
    captureBreaks.clear();
    captureFrames.clear();
    captureLastFrame = -1;
    playerOffset = mProfile->playerOffset;
    playerXOffset = mProfile->playerXOffset;
    offsetKnown = mProfile->playerOffset != 0 || !mProfile->playerPointer;
    ResetPlayback();
    if (mProfile->configured) {
        LoadSettings();
        ScanRoutes();
        ScanReplays();
        initialized = true;
        logPath = Utf8FromWide(RouteDirectory(*mProfile) + L"\\route.log");
        if (!profileLogged) {
            profileLogged = true;
            SYSTEMTIME now{};
            GetLocalTime(&now);
            Log(std::format("=== route trace ({} {}) game={} {:04}-{:02}-{:02} ===",
                __DATE__, __TIME__, mProfile->game, now.wYear, now.wMonth, now.wDay));
            Log(std::format("offset=0x{:X} xbase={:.0f} known={} display={}",
                playerOffset, playerXOffset, offsetKnown ? 1 : 0, displayEnabled ? 1 : 0));
            std::string dirs;
            for (const auto& dir : ReplayDirectories(*mProfile)) {
                if (!dirs.empty())
                    dirs += " | ";
                dirs += Utf8FromWide(dir);
            }
            Log("replay dirs: " + dirs);
            Log(std::format("replays={} routes={}", replayFiles.size(), routes.size()));
        }
    }
}

void RouteTrace::Refresh(int game)
{
    EnsureProfile(game);
    if (!mProfile || !mProfile->configured)
        return;
    ScanRoutes();
    ScanReplays();
}

void RouteTrace::ResetPlayback()
{
    anchor = RouteSync::Anchor{};
    anchorFromMark = false;
    anchorNote.clear();
    anchorBase = -1;
    playbackSample = -1;
    segmentBegin = segmentEnd = 0;
}

// Append one line to thprac_route\route.log. State changes and a summary every
// few seconds only, so a player can reproduce a problem in game and hand the
// file over instead of describing it. Rotated at 256 KiB.
void RouteTrace::Log(const std::string& text)
{
    if (!logEnabled || !mProfile)
        return;
    const std::wstring directory = RouteDirectory(*mProfile);
    if (directory.empty())
        return;
    SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    const std::wstring path = directory + L"\\route.log";
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)
        && attributes.nFileSizeLow > 256 * 1024) {
        MoveFileExW(path.c_str(), (path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::string line = std::format("[{:02}:{:02}:{:02}.{:03}] {}\n",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text);
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
    ++logLines;
}

void RouteTrace::ClearLog()
{
    if (!mProfile)
        return;
    const std::wstring directory = RouteDirectory(*mProfile);
    if (directory.empty())
        return;
    DeleteFileW((directory + L"\\route.log").c_str());
    DeleteFileW((directory + L"\\route.log.old").c_str());
    logLines = 0;
}

// Player position as stored in the game, before the x origin is removed.
static std::vector<std::string> SplitTabsLocal(const std::string& line)
{
    std::vector<std::string> cells;
    std::stringstream stream(line);
    std::string cell;
    while (std::getline(stream, cell, '\t')) cells.push_back(cell);
    return cells;
}

static std::string SanitizeLabelLocal(std::string text)
{
    for (char& c : text) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return text;
}

void RouteTrace::LoadSettings()
{
    displayEnabled = false;
    displayAlpha = 0.55f;
    displayThickness = 2.5f;
    displayColor = ImVec4(0.11f, 0.78f, 0.42f, 1.00f);
    displayMarkers = true;
    displayLabel = true;
    autoMatch = true;
    windowEnabled = true;
    windowBackSeconds = 2.0f;
    windowAheadSeconds = 1.0f;
    autoExtractReplay = true;
    practiceSkipAdjust = 0;
    followPlayerToRoute = false;
    logEnabled = true;
    settingsSelection.clear();
    if (!mProfile)
        return;
    std::string text;
    if (!ReadTextFile(RouteDirectory(*mProfile) + L"\\settings.tsv", text))
        return;
    std::stringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        const auto cells = SplitTabsLocal(line);
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
            else if (cells[0] == "practice_skip") practiceSkipAdjust = std::clamp(std::stoi(cells[1]), -120, 240);
            else if (cells[0] == "autoextract") autoExtractReplay = std::stoi(cells[1]) != 0;
            else if (cells[0] == "log") logEnabled = std::stoi(cells[1]) != 0;
            else if (cells[0] == "selected") settingsSelection = cells[1];
            // Learned while a replay played; a wrong value would make every
            // sample land outside the playfield, so it is validated on load.
            // A title with a verified field (thprac_player_offsets.h) ignores
            // whatever an older session learned: those values were guesses and
            // would silently keep reading the wrong bytes.
            else if (cells[0] == "player_offset" && cells.size() >= 2 && mProfile->playerOffset == 0) {
                const int offset = std::stoi(cells[1]);
                if (offset >= 0 && offset <= 0x4000) {
                    playerOffset = offset;
                    offsetKnown = offset != 0;
                }
            }
        } catch (...) {
            continue;
        }
    }
    settingsSelectionPending = !settingsSelection.empty();
}

void RouteTrace::SaveSettings()
{
    if (!mProfile)
        return;
    const std::wstring directory = RouteDirectory(*mProfile);
    if (directory.empty())
        return;
    SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    std::string text;
    text += "#thprac-route-settings-v1\n";
    text += std::format("enabled\t{}\n", displayEnabled ? 1 : 0);
    text += std::format("alpha\t{:.3f}\n", displayAlpha);
    text += std::format("thickness\t{:.3f}\n", displayThickness);
    text += std::format("color\t{:.4f}\t{:.4f}\t{:.4f}\t{:.4f}\n",
        displayColor.x, displayColor.y, displayColor.z, displayColor.w);
    text += std::format("markers\t{}\n", displayMarkers ? 1 : 0);
    text += std::format("label\t{}\n", displayLabel ? 1 : 0);
    text += std::format("automatch\t{}\n", autoMatch ? 1 : 0);
    text += std::format("window\t{}\n", windowEnabled ? 1 : 0);
    text += std::format("window_back\t{:.2f}\n", windowBackSeconds);
    text += std::format("window_ahead\t{:.2f}\n", windowAheadSeconds);
    text += std::format("practice_skip\t{}\n", practiceSkipAdjust);
    text += std::format("autoextract\t{}\n", autoExtractReplay ? 1 : 0);
    text += std::format("log\t{}\n", logEnabled ? 1 : 0);
    text += std::format("player_offset\t{}\n", playerOffset);
    text += std::format("player_x_offset\t{:.1f}\n", playerXOffset);
    const std::string selected = (selectedRoute >= 0 && selectedRoute < static_cast<int>(routes.size()))
        ? routes[selectedRoute].file
        : std::string();
    text += "selected\t" + selected + "\n";
    WriteTextFile(directory + L"\\settings.tsv", text);
}

static bool ParseRouteHeader(const std::wstring& path, RouteTrace::RouteInfo& info)
{
    std::string text;
    if (!ReadTextFile(path, text))
        return false;
    std::stringstream stream(text);
    std::string line;
    if (!std::getline(stream, line) || line.rfind("#thprac-route", 0) != 0)
        return false;
    while (std::getline(stream, line)) {
        if (line == "data")
            return info.stage >= 0 && info.stage < 9;
        const auto cells = SplitTabsLocal(line);
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

bool RouteTrace::LoadRouteFile(const std::wstring& path, std::vector<ImVec2>& points,
    std::vector<uint8_t>& breaks, std::vector<RouteSync::SectionMark>& sections)
{
    std::string text;
    if (!ReadTextFile(path, text))
        return false;
    points.clear();
    breaks.clear();
    sections.clear();
    points.reserve(4096);
    breaks.reserve(4096);
    std::stringstream stream(text);
    std::string line;
    bool inData = false;
    bool v5 = text.rfind("#thprac-route-v5", 0) == 0;
    bool pendingBreak = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!inData) {
            if (v5 && (line.rfind("clock\t", 0) == 0 || line.rfind("script\t", 0) == 0)) {
                const auto cells = SplitTabsLocal(line);
                try {
                    if (cells.size() != 4) return false;
                    int sample = std::stoi(cells[1]);
                    if (sample < 0 || sample >= kMaxPoints) return false;
                    if (loadedFrames.size() <= static_cast<size_t>(sample)) loadedFrames.resize(sample + 1);
                    if (cells[0] == "clock") {
                        loadedFrames[sample].frame = std::stoi(cells[2]);
                        loadedFrames[sample].segment = std::stoi(cells[3]);
                    } else {
                        loadedFrames[sample].scripts.push_back({static_cast<uint32_t>(std::stoul(cells[2])), std::stoi(cells[3])});
                    }
                } catch (...) { return false; }
                continue;
            }
            if (line == "data")
                inData = true;
            else if (line.rfind("section\t", 0) == 0) {
                // Learned practice anchor: section id and the sample it starts
                // on. Written back the first time a target is played.
                const auto cells = SplitTabsLocal(line);
                if (cells.size() >= 3) {
                    try {
                        RouteSync::SectionMark mark;
                        mark.section = std::stoi(cells[1]);
                        mark.sample = std::stoi(cells[2]);
                        if (mark.section > 0 && mark.sample >= 0)
                            sections.push_back(mark);
                    } catch (...) {
                    }
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
        const auto cells = SplitTabsLocal(line);
        if (cells.size() < 2)
            continue;
        try {
            const float x = std::stof(cells[0]);
            const float y = std::stof(cells[1]);
            if (!std::isfinite(x) || !std::isfinite(y)) {
                if (v5) return false;
                continue;
            }
            // Routes recorded before the extraction filtered unspawned frames
            // carry (0, 0) samples; they are not a real position.
            if (!v5 && x == 0.0f && y == 0.0f)
                continue;
            const bool breakBefore = cells.size() >= 3 && std::stoi(cells[2]) != 0;
            points.emplace_back(x, y);
            breaks.push_back((breakBefore || pendingBreak) ? 1 : 0);
            pendingBreak = false;
        } catch (...) {
            continue;
        }
    }
    timedRoute = v5 && loadedFrames.size() == points.size();
    if (timedRoute) {
        for (size_t i = 0; i < loadedFrames.size(); ++i) {
            const auto& frame = loadedFrames[i];
            if (frame.frame < 0) { timedRoute = false; break; }
            clockSamples.emplace(frame.frame, static_cast<int>(i));
            for (const auto& stamp : frame.scripts) scriptIndex.Add(stamp, static_cast<int>(i));
        }
    }
    return !points.empty();
}

void RouteTrace::ScanRoutes()
{
    if (!mProfile)
        return;
    const std::string keep = (selectedRoute >= 0 && selectedRoute < static_cast<int>(routes.size()))
        ? routes[selectedRoute].file
        : std::string();
    routes.clear();
    const std::wstring directory = RouteDirectory(*mProfile);
    if (directory.empty())
        return;
    SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((directory + L"\\route_*.tsv").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            RouteInfo info;
            if (!ParseRouteHeader(directory + L"\\" + data.cFileName, info))
                continue;
            info.file = Utf8FromWide(data.cFileName);
            routes.push_back(std::move(info));
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
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
                selectedRoute = static_cast<int>(i);
                break;
            }
        }
    }
    if (selectedRoute < 0 && !routes.empty() && !autoMatch)
        selectedRoute = 0;
}

void RouteTrace::ScanReplays()
{
    replayFiles.clear();
    if (!mProfile)
        return;
    std::vector<std::pair<std::wstring, std::wstring>> found; // name, full path
    for (const auto& directory : ReplayDirectories(*mProfile)) {
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((directory + L"*.rpy").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            const std::wstring name = data.cFileName;
            bool seen = false;
            for (const auto& entry : found) {
                if (_wcsicmp(entry.first.c_str(), name.c_str()) == 0) {
                    seen = true;
                    break;
                }
            }
            if (seen)
                continue;
            found.emplace_back(name, directory + name);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    std::sort(found.begin(), found.end(),
        [](const auto& left, const auto& right) {
            return _wcsicmp(left.first.c_str(), right.first.c_str()) < 0;
        });
    for (auto& entry : found)
        replayFiles.push_back(std::move(entry.second));
    if (replaySelection >= static_cast<int>(replayFiles.size()))
        replaySelection = static_cast<int>(replayFiles.size()) - 1;
}

void RouteTrace::DeleteRoute(const std::string& file)
{
    if (!mProfile)
        return;
    DeleteFileW((RouteDirectory(*mProfile) + L"\\" + WideFromUtf8(file)).c_str());
    if (loadedFile == file) {
        loadedFile.clear();
        loadedPoints.clear();
        loadedBreaks.clear();
    }
    ScanRoutes();
}

void RouteTrace::ArmReplay(const std::wstring& name)
{
    // The list stores full paths; the armed name is compared against the name
    // the title reports for the replay it is playing.
    armedReplay = FileNameOf(name);
    extractArmed = !armedReplay.empty();
    Log("replay armed: " + Utf8FromWide(armedReplay));
}

void RouteTrace::EnsureLoaded(const std::string& file)
{
    if (loadedFile == file)
        return;
    loadedFile = file;
    loadedPoints.clear();
    loadedBreaks.clear();
    loadedSections.clear();
    loadedFrames.clear();
    scriptIndex.Clear();
    clockSamples.clear();
    timedRoute = false;
    ResetPlayback();
    if (!LoadRouteFile(RouteDirectory(*mProfile) + L"\\" + WideFromUtf8(file), loadedPoints,
            loadedBreaks, loadedSections)) {
        loadedPoints.clear();
        loadedBreaks.clear();
        timedRoute = false;
    }
}

const RouteTrace::RouteInfo* RouteTrace::ResolveTarget(int stage, int diff, int shot)
{
    const RouteInfo* selected = nullptr;
    if (selectedRoute >= 0 && selectedRoute < static_cast<int>(routes.size()))
        selected = &routes[selectedRoute];
    if (!autoMatch)
        return selected;
    if (selected && selected->stage == stage && selected->diff == diff && selected->shot == shot)
        return selected;
    for (const auto& route : routes) {
        if (route.stage == stage && route.diff == diff && route.shot == shot)
            return &route;
    }
    return nullptr;
}

void RouteTrace::BeginCapture(int stage, int diff, int shot, [[maybe_unused]] int source)
{
    captureStage = stage;
    // Titles differ: TH08 keeps a separate extra difficulty id, and the modern
    // games have far more than four shot types. Only reject negative values.
    captureDiff = std::clamp(diff, 0, 9);
    captureShot = std::clamp(shot, 0, 63);
    capturePoints.clear();
    captureBreaks.clear();
    captureFrames.clear();
    captureLastFrame = -1;
    capturePoints.reserve(8192);
    captureBreaks.reserve(8192);
    capturePointCount = 0;
    capturing = true;
    Log(std::format("capture begin stage={} diff={} shot={}", captureStage, captureDiff, captureShot));
}

void RouteTrace::FinishCapture(bool save)
{
    if (!capturing)
        return;
    capturing = false;
    capturePointCount = 0;
    const size_t points = capturePoints.size();
    const bool saved = save && SaveCapture();
    Log(std::format("capture end save={} points={}", saved ? 1 : 0, points));
    if (saved)
        ScanRoutes();
    capturePoints.clear();
    captureBreaks.clear();
    captureFrames.clear();
    captureLastFrame = -1;
}

bool RouteTrace::SaveCapture()
{
    if (capturePoints.size() < 20 || !mProfile)
        return false;
    const std::wstring directory = RouteDirectory(*mProfile);
    if (directory.empty())
        return false;
    SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::string stamp = std::format("{:04}{:02}{:02}-{:02}{:02}{:02}",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::string stem = std::format("route_s{}_d{}_p{}_{}",
        captureStage + 1, captureDiff, captureShot, stamp);
    std::wstring path = directory + L"\\" + WideFromUtf8(stem) + L".tsv";
    for (int attempt = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++attempt)
        path = directory + L"\\" + WideFromUtf8(std::format("{}_{}", stem, attempt)) + L".tsv";
    std::string text;
    text += "#thprac-route-v5\n";
    text += std::format("game\t{}\n", mProfile->game);
    text += "label\t" + SanitizeLabelLocal(captureLabel) + "\n";
    text += std::format("stage\t{}\n", captureStage);
    text += std::format("diff\t{}\n", captureDiff);
    text += std::format("shot\t{}\n", captureShot);
    text += std::format("source\t{}\n", mProfile->game);
    text += std::format("points\t{}\n", capturePoints.size());
    text += std::format("stamp\t{}\n", stamp);
    for (size_t i = 0; i < captureFrames.size(); ++i) {
        const auto& frame = captureFrames[i];
        text += std::format("clock\t{}\t{}\t{}\n", i, frame.frame, frame.segment);
        for (const auto& stamp : frame.scripts)
            text += std::format("script\t{}\t{}\t{}\n", i, stamp.script, stamp.time);
    }
    text += "data\n";
    for (size_t i = 0; i < capturePoints.size(); ++i) {
        text += std::format("{:.2f}\t{:.2f}\t{}\n", capturePoints[i].x, capturePoints[i].y,
            i < captureBreaks.size() ? captureBreaks[i] : 0);
    }
    return WriteTextFile(path, text, true);
}

void RouteTrace::TryStartCapture()
{
    const wchar_t* rawName = mProfile->replayName ? mProfile->replayName() : nullptr;
    const std::wstring playingFile = FileNameOf(rawName ? rawName : L"");
    const wchar_t* playing = playingFile.c_str();
    const bool named = playing && *playing;
    // Titles that cannot report the replay name still support "record the next
    // replay that starts".
    const bool armedMatch = extractArmed && !armedReplay.empty()
        && (!named || _wcsicmp(armedReplay.c_str(), playing) == 0);
    if (extractArmed && !armedMatch)
        return;
    if (captureLimitReached || (!autoExtractReplay && !armedMatch && !extractSession))
        return;
    const int stage = mProfile->readStage ? mProfile->readStage() : -1;
    if (stage < 0)
        return;
    if (armedMatch)
        extractArmed = false;
    extractSession = true;
    BeginCapture(stage, mProfile->readDifficulty ? mProfile->readDifficulty() : 0,
        mProfile->readShot ? mProfile->readShot() : 0, mProfile->game);
    captureLabel = named ? Utf8FromWide(playing) : std::string("replay");
}

void RouteTrace::SamplePlayer()
{
    if (nativeState.frame < 0 || nativeState.frame == captureLastFrame) return;
    if (capturePoints.size() >= static_cast<size_t>(kMaxPoints)) {
        FinishCapture(true);
        captureLimitReached = true;
        return;
    }
    float x = 0.0f, y = 0.0f;
    const bool valid = mProfile->readPlayer && mProfile->readPlayer(x, y);
    if (!valid && capturePoints.empty()) return;
    if (!valid) { x = capturePoints.back().x; y = capturePoints.back().y; }
    bool split = !valid || captureLastFrame < 0 || nativeState.frame != captureLastFrame + 1;
    if (!capturePoints.empty()) split |= Distance(capturePoints.back(), {x, y}) > 60.0f;
    capturePoints.emplace_back(x, y);
    captureBreaks.push_back(split ? 1 : 0);
    captureFrames.push_back({nativeState.frame, nativeState.segment, nativeState.scripts});
    captureLastFrame = nativeState.frame;
    capturePointCount = capturePoints.size();
}

void RouteTrace::NewRun(int game)
{
    if (!AddonEnabled(Addon::Route)) return;
    RouteNativeInvalidate();
    if (game != mGame) return;
    FinishCapture(true);
    ResetPlayback();
    frameGate.Reset();
    lastReplayActive = false;
    restartPending = true;
}

void RouteTrace::OnFrame(int game)
{
    if (!AddonEnabled(Addon::Route)) return;
    EnsureProfile(game);
    if (!mProfile || !mProfile->configured) return;
    RouteNativeState state;
    runtimeActive = RouteReadNativeState(game, state);
    if (!runtimeActive) {
        if (lastReplayActive && !(mProfile->replayActive && mProfile->replayActive())) {
            FinishCapture(true);
            lastReplayActive = extractSession = false;
        }
        return;
    }
    const bool replay = mProfile->replayActive && mProfile->replayActive();
    const int stage = mProfile->readStage ? mProfile->readStage() : -1;
    const int section = mProfile->practiceSection ? mProfile->practiceSection() : 0;
    const int phase = mProfile->practicePhase ? mProfile->practicePhase() : 0;
    if (stage < 0 || stage >= mProfile->stageCount) return;
    const bool reset = restartPending || stage != lastStage
        || section != lastPracticeSection || phase != lastPracticePhase
        || (nativeState.frame >= 0 && state.frame < nativeState.frame)
        || (nativeState.generation && state.generation != nativeState.generation);
    nativeState = std::move(state);
    if (reset) {
        FinishCapture(true);
        ResetPlayback();
        frameGate.Reset();
        lastReplayActive = false;
        restartPending = false;
        lastStage = stage;
        lastPracticeSection = section;
        lastPracticePhase = phase;
        captureLimitReached = false;
        Log(std::format("reset stage={} section={} phase={} clock={}", stage, section, phase, nativeState.frame));
    }
    const bool tick = frameGate.Advance(nativeState.frame);
    playClock = playbackClock = nativeState.frame;
    if (replay) {
        // Replay status can become true before player/stage creation. Retry
        // until the first valid logic frame instead of losing that transition.
        if (!capturing && tick) TryStartCapture();
        if (capturing && tick) SamplePlayer();
    } else if (lastReplayActive) {
        FinishCapture(true);
        extractArmed = false;
        extractSession = false;
    }
    lastReplayActive = replay;
    if (!replay && (displayEnabled || followPlayerToRoute)) {
        const auto* target = ResolveTarget(stage,
            mProfile->readDifficulty ? mProfile->readDifficulty() : 0,
            mProfile->readShot ? mProfile->readShot() : 0);
        if (!target) { ResetPlayback(); return; }
        EnsureLoaded(target->file);
        AcquireAnchor(section);
        if (tick && followPlayerToRoute && playbackSample >= 0 && mProfile->writePlayer)
            mProfile->writePlayer(loadedPoints[playbackSample].x, loadedPoints[playbackSample].y);
    }
}

bool RouteTrace::AcquireAnchor(int practiceSection)
{
    const int count = static_cast<int>(loadedPoints.size());
    if (!timedRoute || count < 1) {
        playbackSample = -1;
        anchorNote = "旧路线缺少原生时钟，请重新播放录像提取";
        return false;
    }
    const auto* service = RouteAddon();
    if (!service) return false;
    const RoutePlaybackRequest request {
        count, practiceSection, nativeState.frame, practiceSkipAdjust, anchor, segmentBegin, segmentEnd, this,
        [](const void* state) {
            const auto& trace = *static_cast<const RouteTrace*>(state);
            for (const auto& stamp : trace.nativeState.scripts) {
                const int index = trace.scriptIndex.Find(stamp);
                if (index >= 0) return index;
            }
            return -1;
        },
        [](const void* state, int clock, bool exact) {
            const auto& samples = static_cast<const RouteTrace*>(state)->clockSamples;
            if (exact) {
                const auto found = samples.find(clock);
                return found == samples.end() ? -1 : found->second;
            }
            const auto found = samples.upper_bound(clock);
            return found == samples.begin() ? 0 : std::prev(found)->second;
        },
        [](const void* state, int i) { return static_cast<const RouteTrace*>(state)->loadedFrames[i].frame; },
        [](const void* state, int i) { return static_cast<const RouteTrace*>(state)->loadedFrames[i].segment; }
    };
    const auto result = service->align(request);
    if (result.scriptSample >= 0) {
        if (!anchor.valid || result.scriptSample < anchor.sample)
            Log(std::format("script anchor sample={} clock={} section={}", result.scriptSample, nativeState.frame, practiceSection));
        anchorBase = result.scriptSample;
        anchorFromMark = true;
        anchorNote = "已按游戏脚本时钟对齐";
    }
    anchor = result.anchor;
    segmentBegin = result.segmentBegin;
    segmentEnd = result.segmentEnd;
    playbackSample = result.sample;
    if (playbackSample < 0) anchorNote = "等待录像中对应的脚本位置";
    return playbackSample >= 0;
}

void RouteTrace::Draw(ImDrawList* drawList, int game)
{
    if (!AddonEnabled(Addon::Route)) return;
    EnsureProfile(game);
    if (!mProfile || !mProfile->configured || !drawList)
        return;

    const int stage = mProfile->readStage ? mProfile->readStage() : -1;
    const int diff = mProfile->readDifficulty ? std::clamp(mProfile->readDifficulty(), 0, 9) : 0;
    const int shot = mProfile->readShot ? std::clamp(mProfile->readShot(), 0, 63) : 0;

    // Some titles only tick GameUpdateInner while a stage is running, so the
    // end of a replay would otherwise never flush the captured file.
    const bool replayActive = mProfile->replayActive && mProfile->replayActive();
    if (!replayActive) {
        if (capturing) {
            FinishCapture(true);
            extractArmed = false;
        }
        extractSession = false;
            lastReplayActive = false;
    }

    if (!displayEnabled || !runtimeActive || stage < 0 || stage >= mProfile->stageCount || replayActive)
        return;
    const RouteInfo* target = ResolveTarget(stage, diff, shot);
    if (!target)
        return;
    EnsureLoaded(target->file);
    if (loadedPoints.size() < 2)
        return;

    const int count = static_cast<int>(loadedPoints.size());
    const int current = playbackSample;
    ImGuiIO& io = ImGui::GetIO();
    const float baseWidth = game <= 13 ? 640.0f : 1280.0f;
    const float scale = game <= 13 ? 1.0f : 2.0f;
    const float xRatio = io.DisplaySize.x / baseWidth;
    const float yRatio = io.DisplaySize.y / (baseWidth * 0.75f);
    auto toClient = [&](const ImVec2& point) {
        return ImVec2{ (point.x + 224.0f) * scale * xRatio, (point.y + 16.0f) * scale * yRatio };
    };

    int rangeBegin = 0;
    int rangeEnd = count;
    if (windowEnabled && current >= 0) {
        const int back = static_cast<int>(std::lround(std::max(0.0f, windowBackSeconds) * kSamplesPerSecond));
        const int ahead = static_cast<int>(std::lround(std::max(0.0f, windowAheadSeconds) * kSamplesPerSecond));
        rangeBegin = std::max(segmentBegin, current - back);
        rangeEnd = std::min(segmentEnd, current + ahead + 1);
        if (rangeEnd - rangeBegin < 2) {
            rangeBegin = std::clamp(current, 0, count - 1);
            rangeEnd = rangeBegin + 1;
        }
    }

    drawList->PushClipRect(
        { 32.0f * scale * xRatio, 16.0f * scale * yRatio },
        { 416.0f * scale * xRatio, 464.0f * scale * yRatio });
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
        ImVec4(displayColor.x, displayColor.y, displayColor.z, displayAlpha));
    const float thickness = std::max(0.5f, displayThickness * xRatio);
    auto isBreakBefore = [&](int index) {
        return index >= 0 && index < static_cast<int>(loadedBreaks.size()) && loadedBreaks[index] != 0;
    };
    // Feeding tens of thousands of samples to AddPolyline every frame rebuilds
    // the whole vertex buffer with antialiasing per segment; thin the outline
    // for display only.
    const int visibleCount = rangeEnd - rangeBegin;
    const int drawStride = std::max(1, visibleCount / 4000);
    int index = rangeBegin;
    while (index < rangeEnd) {
        const int begin = index;
        int end = index + 1;
        while (end < rangeEnd && !isBreakBefore(end))
            ++end;
        if (end - begin >= 2) {
            segmentBuffer.clear();
            segmentBuffer.reserve(static_cast<size_t>(end - begin) / drawStride + 2);
            for (int i = begin; i < end; i += drawStride)
                segmentBuffer.push_back(toClient(loadedPoints[i]));
            const ImVec2 lastPoint = toClient(loadedPoints[end - 1]);
            if (segmentBuffer.back().x != lastPoint.x || segmentBuffer.back().y != lastPoint.y)
                segmentBuffer.push_back(lastPoint);
            drawList->AddPolyline(segmentBuffer.data(), static_cast<int>(segmentBuffer.size()),
                color, 0, thickness);
        }
        index = end;
    }

    if (displayMarkers) {
        const float radius = std::max(3.0f, 3.0f * xRatio);
        if (current >= 0)
            drawList->AddCircle(toClient(loadedPoints[current]), radius + 1.0f,
                IM_COL32(255, 255, 255, 200), 12, 1.5f);
    }
    drawList->PopClipRect();

    if (displayLabel) {
        std::string text = std::format("S{} {}", stage + 1, target->label);
        if (text.size() > 64)
            text.resize(64);
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const ImVec2 position{ 34.0f * scale * xRatio, 466.0f * scale * yRatio - size.y };
        drawList->AddRectFilled({ position.x - 2.0f, position.y - 1.0f },
            { position.x + size.x + 2.0f, position.y + size.y + 1.0f },
            IM_COL32(0, 0, 0, 130));
        drawList->AddText(position, IM_COL32(230, 255, 255, 230), text.c_str());
    }
}

bool RouteTraceWindowTogglePressed()
{
    if (!AddonEnabled(Addon::Route)) return false;
    return RouteTrace::Instance().HasProfile() && Gui::ImplWin32CheckForeground()
        && Gui::ImplWin32GetKeyFrame(VK_F10) == 1;
}

void RouteTraceWindowClose()
{
    if (!AddonEnabled(Addon::Route)) return;
    if (!RouteTrace::Instance().HasProfile()) return;
    auto& window = RouteTraceWnd::singleton();
    if (!window.IsClosed()) {
        RouteTrace::Instance().SetSettingsDirty();
        window.CloseImmediately();
    }
}

bool RouteTraceWindowUpdate(bool togglePressed)
{
    if (!AddonEnabled(Addon::Route)) return false;
    if (!RouteTrace::Instance().HasProfile()) return false;
    const bool open = RouteTraceWnd::StaticUpdate(togglePressed);
    if (open) ImGui::GetIO().MouseDrawCursor = true;
    return open;
}

void RouteTraceFrameDraw(ImDrawList* drawList, int game)
{
    RouteTrace::Instance().Draw(drawList, game);
}

void RouteTraceWnd::OnContentUpdate()
{
    auto& trace = RouteTrace::Instance();
    if (!trace.HasProfile()) {
        ImGui::TextUnformatted("Route reference is not available in this title.");
        return;
    }
    const int locale = Gui::LocaleGet();
    const auto label = [locale](const char* zh, const char* ja, const char* en) {
        return locale == Gui::LOCALE_ZH_CN ? zh : locale == Gui::LOCALE_JA_JP ? ja : en;
    };

    ImGui::TextUnformatted(label("路线参考（F10）", "ルート参考（F10）", "Route reference (F10)"));
    ImGui::SameLine();
    if (ImGui::Button(label("关闭", "閉じる", "Close"))) {
        trace.SetSettingsDirty();
        Close();
    }
    ImGui::Separator();
    ImGui::BeginChild("##thprac_route_body", ImVec2(0.0f, 0.0f));

    if (ImGui::CollapsingHeader(label("显示设置", "表示設定", "Display"), ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox(label("显示参考路线", "参考ルートを表示", "Show reference route"), &trace.displayEnabled);
        ImGui::SliderFloat(label("透明度", "不透明度", "Opacity"), &trace.displayAlpha, 0.05f, 1.0f, "%.2f");
        ImGui::SliderFloat(label("线宽", "線幅", "Line width"), &trace.displayThickness, 0.5f, 6.0f, "%.2f");
        ImGui::ColorEdit4(label("颜色", "色", "Color"), &trace.displayColor.x,
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::Checkbox(label("标记当前进度", "現在位置を表示", "Mark current progress"), &trace.displayMarkers);
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
                "路线按帧推进：练习起点对齐后逐帧前进，暂停时随游戏停住，不会自己漂走。",
                "ルートはフレーム単位で進みます。練習開始位置に合わせた後は毎フレーム進み、ポーズ中は停止します。",
                "The route advances frame by frame from the aligned practice start and holds while the game is paused."));
        }
        ImGui::TextDisabled("%s", label(
            "练习跳转按录像中的原生脚本位置对齐；重试自动复位，静止和偏离参考路线不影响时钟。",
            "練習移行はリプレイのスクリプト位置に合わせます。リトライ時は自動リセットします。",
            "Practice jumps align to the replay script clock and reset on retry, regardless of player movement."));
        ImGui::SliderInt(label("练习跳转补偿（帧）", "プラクティス移行の補正",
            "Practice jump offset (frames)"), &trace.practiceSkipAdjust, -120, 240);
        if (ImGui::Button(label("重置手动补偿", "手動補正をリセット", "Reset manual offset")))
            trace.practiceSkipAdjust = 0;
        if (trace.CanFollow()) {
            ImGui::Checkbox(label("测试：让自机沿参考路线移动", "テスト：自機を参考ルートに沿って移動",
                "Test: move the player along the route"), &trace.followPlayerToRoute);
            if (trace.followPlayerToRoute)
                ImGui::TextWrapped("%s", label("逐帧写入路线坐标；取消勾选即可恢复手动操作。",
                    "毎フレーム自機位置を書き換えます。解除すると手動操作に戻ります。",
                    "Writes the route position every game frame. Turn off to take control back."));
        }
        if (!trace.anchorNote.empty())
            ImGui::TextDisabled("%s", trace.anchorNote.c_str());
        if (trace.anchor.valid) {
            ImGui::TextDisabled("%s", trace.anchorFromMark
                ? label("对齐来源：练习段落 / 面数开头", "整列元：練習区間・ステージ先頭",
                    "Anchor: practice section or stage start")
                : label("对齐来源：游戏脚本时钟", "整列元：スクリプトクロック",
                    "Anchor: native script clock"));
        }
        if (trace.anchor.valid && trace.playbackSample >= 0)
            ImGui::Text(label("当前进度：第 %d 帧 / 共 %d 帧（时钟 %d）",
                "現在位置：%d フレーム目 / 全 %d フレーム（クロック %d）",
                "Progress: frame %d of %d (clock %d)"),
                trace.playbackSample, static_cast<int>(trace.LoadedPointCount()), trace.playbackClock);
    }

    if (ImGui::CollapsingHeader(label("从录像提取轨迹", "リプレイから抽出", "Extract from replay"),
            ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox(label("播放录像时自动提取", "リプレイ再生時に自動抽出",
            "Extract automatically while a replay plays"), &trace.autoExtractReplay);
        ImGui::TextWrapped("%s", label(
            "在游戏中播放录像，工具会在播放期间逐帧记录自机位置，切换面数时自动分段保存。",
            "ゲーム内でリプレイを再生すると、再生中の自機位置を毎フレーム記録し、ステージごとに保存します。",
            "Play a replay in game: the player position is sampled every frame and saved per stage."));

        if (trace.replayFiles.empty()) {
            ImGui::TextDisabled("%s", label("未找到录像文件（已查找 %APPDATA%、游戏目录和当前目录）",
                "リプレイが見つかりません（%APPDATA%・ゲームフォルダ・カレントを検索済み）",
                "No replay files found (searched %APPDATA%, the game folder and the working directory)"));
        } else {
            ImGui::BeginChild("##thprac_route_replays", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 6.0f));
            for (int i = 0; i < static_cast<int>(trace.replayFiles.size()); ++i) {
                const std::string name = Utf8FromWide(FileNameOf(trace.replayFiles[i]));
                if (ImGui::Selectable(name.c_str(), trace.replaySelection == i))
                    trace.replaySelection = i;
            }
            ImGui::EndChild();
        }
        if (ImGui::Button(label("刷新录像列表", "リプレイ一覧を更新", "Refresh list")))
            trace.ScanForUi();
        ImGui::SameLine();
        if (ImGui::Button(label("提取所选录像", "選択したリプレイを抽出", "Extract selected replay"))) {
            if (trace.replaySelection >= 0 && trace.replaySelection < static_cast<int>(trace.replayFiles.size()))
                trace.ArmForUi(trace.replayFiles[trace.replaySelection]);
        }
        if (trace.extractArmed) {
            ImGui::SameLine();
            if (ImGui::Button(label("取消等待", "待機を解除", "Cancel waiting")))
                trace.extractArmed = false;
        }
        if (trace.capturing) {
            ImGui::Text(label("正在提取：%s  面数 %d  点数 %d", "抽出中：%s  ステージ %d  点数 %d",
                "Extracting: %s  stage %d  points %d"),
                trace.captureLabel.c_str(), trace.captureStage, static_cast<int>(trace.capturePointCount));
        } else if (trace.extractArmed) {
            ImGui::Text(label("等待播放：%s", "再生待ち：%s", "Waiting for: %s"),
                Utf8FromWide(trace.armedReplay).c_str());
        } else {
            ImGui::TextDisabled("%s", label("未在提取", "抽出していません", "Not extracting"));
        }
    }

    if (ImGui::CollapsingHeader(label("已保存路线", "保存済みルート", "Saved routes"),
            ImGuiTreeNodeFlags_DefaultOpen)) {
        if (trace.routes.empty()) {
            ImGui::TextDisabled("%s", label("暂无已保存路线", "保存済みルートはありません", "No saved routes"));
        } else {
            ImGui::BeginChild("##thprac_route_table", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f));
            for (int i = 0; i < static_cast<int>(trace.routes.size()); ++i) {
                auto& route = trace.routes[i];
                const std::string text = std::format("{}: S{}  {} / {}    {} {}",
                    route.label, route.stage + 1, route.stamp, route.file,
                    label("点数", "点数", "Points"), route.points);
                if (ImGui::Selectable(text.c_str(), trace.selectedRoute == i))
                    trace.selectedRoute = i;
            }
            ImGui::EndChild();
        }
        if (ImGui::Button(label("刷新列表", "一覧を更新", "Refresh")))
            trace.ScanForUi();
        ImGui::SameLine();
        if (ImGui::Button(label("取消参考", "参考を解除", "Clear reference")))
            trace.selectedRoute = -1;
        ImGui::SameLine();
        if (trace.selectedRoute >= 0 && trace.selectedRoute < static_cast<int>(trace.routes.size())) {
            if (ImGui::Button(label("删除选中路线", "選択ルートを削除", "Delete selected route")))
                trace.pendingDelete = trace.routes[trace.selectedRoute].file;
        }
        if (!trace.pendingDelete.empty()) {
            ImGui::Text("%s", label("删除这条路线记录？", "このルートを削除しますか？", "Delete this route?"));
            ImGui::SameLine();
            if (ImGui::Button(label("删除", "削除", "Delete"))) {
                trace.DeleteForUi(trace.pendingDelete);
                trace.pendingDelete.clear();
            }
            ImGui::SameLine();
            if (ImGui::Button(label("取消", "キャンセル", "Cancel")))
                trace.pendingDelete.clear();
        }
        ImGui::TextDisabled("%s", label(
            "轨迹按面数、难度和机体保存在游戏自身的 %APPDATA% 目录下（thprac_route 子目录）。"
            "旧版缺少时钟的路线仍可显示完整折线；重新播放录像提取后即可按帧同步。",
            "ルートはステージ・難易度・自機ごとに各作品の %APPDATA% 配下（thprac_route）に保存されます。"
            "古いルートは全体表示のみです。再抽出するとフレーム単位で同期できます。",
            "Routes are stored per stage, difficulty and shot under the game's own %APPDATA% folder (thprac_route). "
            "Older routes remain visible as a full path. Re-extract the replay to enable native clock synchronization."));
    }

    ImGui::EndChild();
}

} // namespace THPrac
