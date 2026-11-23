// Offline quick-practice packet exporter. No window, game or injection is used.
//
// Prefixes checked against the production structs in thprac_th*.cpp:
// TH06: mode, stage, section, phase, frame. The current editor wraps these
// parameters in TH06QuickPracticeState with six trailing int32 UI fields. We
// export its supported legacy parameter portion so RestoreLegacyWarp rebuilds
// UI selection from the patched section instead of retaining default UI fields.
// TH07/TH08: mode, stage, warp, section, phase, frame (warp is left untouched).
// TH10--TH18: mode, stage, section, phase. TH12 exports PracticeFields, and the
// TH15 editor is taken from its dedicated plugin rather than the stock adapter.
// TH20: PracticeFields also starts with mode, stage, section, phase.

#define NOMINMAX
#include <Windows.h>
#include <imgui.h>
#include "../../extensions/quick-practice/service.h"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <type_traits>

using namespace THPrac::QuickPractice;

namespace {
struct Prefix {
    const char* game;
    int shots;
    int lastStage;
    size_t sectionOffset;
    size_t phaseOffset;
};
constexpr Prefix prefixes[] = {
    {"th06", 4, 6, 8, 12}, {"th07", 6, 7, 12, 16}, {"th08", 12, 9, 12, 16},
    {"th10", 6, 6, 8, 12}, {"th11", 6, 6, 8, 12}, {"th12", 6, 6, 8, 12},
    {"th13", 4, 6, 8, 12}, {"th14", 6, 6, 8, 12}, {"th15", 4, 6, 8, 12},
    {"th16", 16, 6, 8, 12}, {"th17", 9, 6, 8, 12}, {"th18", 4, 6, 8, 12},
    {"th20", 16, 6, 8, 12},
};

bool ReadInt(const wchar_t* text, int32_t& value)
{
    errno = 0;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text, &end, 10);
    if (errno == ERANGE || !end || end == text || *end
        || parsed < std::numeric_limits<int32_t>::min()
        || parsed > std::numeric_limits<int32_t>::max()) return false;
    value = static_cast<int32_t>(parsed);
    return true;
}

int32_t Field(const Request& packet, size_t offset)
{
    int32_t value;
    std::memcpy(&value, packet.parameters + offset, sizeof(value));
    return value;
}

void Put(Request& packet, size_t offset, int32_t value)
{
    std::memcpy(packet.parameters + offset, &value, sizeof(value));
}

int Fail(int code, const char* message)
{
    std::fprintf(stderr, "%s (Win32=%lu)\n", message, GetLastError());
    return code;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    static_assert(sizeof(int) == 4 && sizeof(void*) == 4, "Build this helper for Win32/x86");
    static_assert(sizeof(Request) == 540 && offsetof(Request, parameters) == 28);
    static_assert(std::is_trivially_copyable_v<Request>);
    if (argc != 10) {
        std::fwprintf(stderr,
            L"Usage: native_capture_packet_export.exe <game_adapter.dll> <th15_practice.dll> "
            L"<thXX> <difficulty0-3> <shot> <stage0-based> <section> <phase> <new-output.bin>\n");
        return 1;
    }
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    char game[8] {};
    const size_t gameLength = std::wcslen(argv[3]);
    if (!gameLength || gameLength >= sizeof(game)) return Fail(1, "Invalid game ID");
    for (size_t i = 0; i < gameLength; ++i) {
        if (argv[3][i] > 0x7f) return Fail(1, "Game ID must be ASCII");
        game[i] = static_cast<char>(argv[3][i]);
    }
    const Prefix* prefix = nullptr;
    for (const auto& item : prefixes)
        if (std::strcmp(item.game, game) == 0) prefix = &item;
    if (!prefix) return Fail(1, "Unsupported game ID");
    int32_t difficulty, shot, stage, section, phase;
    if (!ReadInt(argv[4], difficulty) || !ReadInt(argv[5], shot) || !ReadInt(argv[6], stage)
        || !ReadInt(argv[7], section) || !ReadInt(argv[8], phase)
        || difficulty < 0 || difficulty > 3 || shot < 0 || shot >= prefix->shots
        || stage < 0 || stage > prefix->lastStage || section < 0 || phase < 0)
        return Fail(1, "Invalid practice selection");

    const HMODULE adapter = LoadLibraryW(argv[1]);
    const HMODULE th15 = LoadLibraryW(argv[2]);
    if (!adapter || !th15) return Fail(2, "Unable to load supplied plugins");
    const auto editor = reinterpret_cast<Editor>(GetProcAddress(
        std::strcmp(game, "th15") == 0 ? th15 : adapter, editorExport));
    if (!editor || !editor(game, nullptr, Edit::Probe, nullptr))
        return Fail(3, "Plugin has no suitable practice editor");

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = {850, 700};
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    if (!io.Fonts->Build()) {
        ImGui::DestroyContext();
        return Fail(3, "Unable to prepare the in-memory ImGui font atlas");
    }
    EditorContext context {ImGui::GetCurrentContext(), 0};
    ImGui::GetAllocatorFunctions(&context.alloc, &context.free, &context.allocContext);
    Request packet {};
    std::strcpy(packet.game, game);
    packet.version = protocol;
    packet.difficulty = difficulty;
    packet.shot = shot;
    packet.stage = stage;
    ImGui::NewFrame();
    ImGui::Begin("Offline native capture packet");
    const bool edited = editor(game, &context, Edit::Reset, &packet)
        && editor(game, &context, Edit::Draw, &packet)
        && editor(game, &context, Edit::Save, &packet);
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext();
    if (!edited || packet.version != protocol || std::strcmp(packet.game, game) != 0
        || packet.difficulty != difficulty || packet.shot != shot || !packet.bytes
        || packet.bytes > sizeof(packet.parameters) || prefix->phaseOffset + 4 > packet.bytes)
        return Fail(4, "Editor returned an invalid packet");

    const uint32_t editorBytes = packet.bytes;
    const bool th06Legacy = std::strcmp(game, "th06") == 0;
    if (th06Legacy) {
        // Validate the current production TH06QuickPracticeState footer, then
        // use the existing legacy-compatibility path in ApplyQuickPractice.
        constexpr uint32_t footerBytes = 6 * sizeof(int32_t);
        if (packet.bytes <= footerBytes + 20) return Fail(4, "TH06 state is too small");
        const uint32_t legacyBytes = packet.bytes - footerBytes;
        if (Field(packet, legacyBytes) != difficulty || Field(packet, legacyBytes + 4) != shot)
            return Fail(4, "TH06 editor state footer does not match the production layout");
        packet.bytes = legacyBytes;
    }
    packet.stage = stage;
    Put(packet, 0, 1); // practice mode
    Put(packet, 4, stage);
    Put(packet, prefix->sectionOffset, section);
    Put(packet, prefix->phaseOffset, phase);
    // Unused payload bytes are ignored by the protocol; normalize them so a
    // TH06 legacy packet does not carry the unused UI-state footer.
    std::memset(packet.parameters + packet.bytes, 0, sizeof(packet.parameters) - packet.bytes);

    const HANDLE output = CreateFileW(argv[9], GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return Fail(5, "Output file already exists or cannot be created");
    DWORD written = 0;
    const bool saved = WriteFile(output, &packet, sizeof(packet), &written, nullptr)
        && written == sizeof(packet) && FlushFileBuffers(output);
    const DWORD savedError = GetLastError();
    CloseHandle(output);
    if (!saved) {
        SetLastError(savedError);
        return Fail(5, "Unable to write the complete request packet");
    }
    std::printf("{\"game\":\"%s\",\"difficulty\":%d,\"shot\":%d,\"stage\":%d,"
                "\"section\":%d,\"phase\":%d,\"editor_bytes\":%u,\"parameter_bytes\":%u,"
                "\"request_bytes\":%zu,\"section_offset\":%zu,\"phase_offset\":%zu,"
                "\"th06_legacy_payload\":%s}\n",
        game, difficulty, shot, stage, section, phase, editorBytes, packet.bytes, sizeof(packet),
        prefix->sectionOffset, prefix->phaseOffset, th06Legacy ? "true" : "false");
    return 0;
}
