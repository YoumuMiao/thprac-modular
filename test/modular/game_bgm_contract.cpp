#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include "thprac_plugin_sdk.h"

namespace {
decltype(&mmioOpenA) waveHook;
decltype(&CreateFileA) archiveHookA;
decltype(&CreateFileW) archiveHookW;
decltype(&ReadFile) archiveRead;
decltype(&CloseHandle) archiveClose;
int enabled = 1;

void Check(bool passed, const char* description)
{
    if (passed) return;
    std::fprintf(stderr, "%s\n", description);
    std::exit(1);
}

int __cdecl Hook(const char* library, const char* symbol, void* replacement, void** original)
{
    *original = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(library), symbol));
    if (std::strcmp(symbol, "mmioOpenA") == 0)
        waveHook = reinterpret_cast<decltype(waveHook)>(replacement);
    else if (std::strcmp(symbol, "CreateFileA") == 0)
        archiveHookA = reinterpret_cast<decltype(archiveHookA)>(replacement);
    else if (std::strcmp(symbol, "CreateFileW") == 0)
        archiveHookW = reinterpret_cast<decltype(archiveHookW)>(replacement);
    else if (std::strcmp(symbol, "ReadFile") == 0)
        archiveRead = reinterpret_cast<decltype(archiveRead)>(replacement);
    else if (std::strcmp(symbol, "CloseHandle") == 0)
        archiveClose = reinterpret_cast<decltype(archiveClose)>(replacement);
    return *original != nullptr;
}

int __cdecl ReadSetting(const char*, const char*, int)
{
    return enabled;
}

void Write(const char* path, std::string_view contents)
{
    std::ofstream output(path, std::ios::binary);
    output.write(contents.data(), contents.size());
    Check(static_cast<bool>(output), path);
}

void WriteWave(const char* path, char sample, bool complete = true)
{
    constexpr char header[] = "RIFF\x2c\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0"
                              "\x22\x56\0\0\x44\xac\0\0\x02\0\x10\0data\x08\0\0\0";
    std::string bytes(header, sizeof(header) - 1);
    bytes.append(complete ? 8 : 4, sample);
    Write(path, bytes);
}

char FirstByte(HANDLE file)
{
    char data = 0;
    DWORD received = 0;
    Check(file != INVALID_HANDLE_VALUE && ReadFile(file, &data, 1, &received, nullptr)
            && received == 1,
        "ReadFile failed");
    if (archiveClose) archiveClose(file);
    else CloseHandle(file);
    return data;
}

char FirstSample(decltype(waveHook) open)
{
    char path[] = "bgm\\th06_01.wav";
    const auto file = open(path, nullptr, MMIO_READ | MMIO_ALLOCBUF);
    char data = 0;
    Check(file && mmioSeek(file, 44, SEEK_SET) == 44 && mmioRead(file, &data, 1) == 1,
        "WAV read failed");
    mmioClose(file, 0);
    return data;
}
}

int wmain(int argc, wchar_t** argv)
{
    Check(argc == 2, "Expected DLL path");
    const auto dll = LoadLibraryW(argv[1]);
    Check(dll != nullptr, "Cannot load BGM plugin");
    const auto query = reinterpret_cast<ThpracExtensionQueryFn>(GetProcAddress(dll, THPRAC_EXTENSION_QUERY));
    Check(query != nullptr, "Missing plugin export");
    const auto* extension = query(THPRAC_EXTENSION_ABI);
    Check(extension && extension->plugin, "Missing SDK descriptor");

    std::filesystem::current_path(std::filesystem::absolute(argv[0]).parent_path());
    std::filesystem::create_directories("thprac-bgm");
    std::filesystem::create_directories("bgm");
    constexpr char generation[] = "1234567890abcdef1234567890abcdef";
    const auto manager = std::filesystem::path("thprac-bgm/manager");
    const auto wavProfile = manager / "wav";
    const auto datProfile = manager / "thbgm";
    const auto trialProfile = manager / "thbgm_tr";
    std::filesystem::create_directories(wavProfile / generation);
    std::filesystem::create_directories(datProfile / generation);
    std::filesystem::create_directories(trialProfile / generation);
    std::filesystem::remove(wavProfile / "active.ini");
    std::filesystem::remove(datProfile / "active.ini");
    std::filesystem::remove(trialProfile / "active.ini");
    const std::string active = std::string("[BGM]\ngeneration=") + generation + "\n";

    ThpracPluginGameEvent event {};
    event.kind = THPRAC_EVENT_GAME_READY;
    event.game_id = "th06";
    event.config.get_int = ReadSetting;
    event.hook_import = Hook;
    enabled = 0;
    extension->plugin->game_event(&event);
    Check(waveHook == nullptr, "Disabled plugin installed a hook");
    enabled = 1;
    extension->plugin->game_event(&event);
    Check(waveHook != nullptr, "Missing WAV hook");
    WriteWave("bgm/th06_01.wav", 'a');
    WriteWave("thprac-bgm/th06_01.wav", 'b');
    Check(FirstSample(waveHook) == 'b', "TH06 replacement was not used");
    WriteWave((wavProfile / generation / "th06_01.wav").string().c_str(), 'c');
    Write((wavProfile / "active.ini").string().c_str(), active);
    Write((wavProfile / generation / "in-use").string().c_str(), "");
    Write((wavProfile / generation / "requires-metadata").string().c_str(), "");
    extension->plugin->game_event(&event);
    std::filesystem::remove(wavProfile / "active.ini");
    Check(FirstSample(waveHook) == 'b', "Single-play audio needs paired metadata");
    Write("testMD.dat", "original");
    Write((wavProfile / generation / "meta-testMD.dat").string().c_str(), "metadata");
    Check(FirstByte(archiveHookW(L"testMD.dat", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr))
            == 'm', "One-shot metadata should be redirected");
    Check(FirstSample(waveHook) == 'c', "Editor WAV should take precedence after metadata");
    extension->plugin->game_event(&event);
    Check(FirstSample(mmioOpenA) == 'a', "Original TH06 WAV changed");
    WriteWave("thprac-bgm/th06_01.wav", 'b', false);
    Check(FirstSample(waveHook) == 'a', "Truncated WAV should fall back to the original");
    std::filesystem::remove("thprac-bgm/th06_01.wav");
    Check(FirstSample(waveHook) == 'a', "Missing WAV should fall back to the original");

    event.game_id = "th08";
    extension->plugin->game_event(&event);
    Check(archiveHookA && archiveHookW, "Missing archive hooks");
    Write("thbgm.dat", "original");
    Write("thprac-bgm/thbgm.dat", "replaced");
    Check(FirstByte(archiveHookA("thbgm.dat", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr))
            == 'r',
        "ANSI archive replacement was not used");
    Check(FirstByte(archiveHookW(L".\\thbgm.dat", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr))
            == 'r',
        "Unicode archive replacement was not used");
    Write((datProfile / generation / "thbgm.dat").string().c_str(), "new-data");
    const auto alternative = datProfile / generation / "variants" / "th08_01";
    std::filesystem::create_directories(alternative);
    Write((alternative / "1.pcm").string().c_str(), "variant!");
    Write((datProfile / generation / "variants.txt").string().c_str(), "0 8 2 0 th08_01\n");
    Write((datProfile / "active.ini").string().c_str(), active);
    Write("thbgm_tr.dat", "original");
    Write((trialProfile / generation / "thbgm_tr.dat").string().c_str(), "new-data");
    const auto trialAlternative = trialProfile / generation / "variants" / "th08_01";
    std::filesystem::create_directories(trialAlternative);
    Write((trialAlternative / "0.pcm").string().c_str(), "variant!");
    Write((trialProfile / generation / "variants.txt").string().c_str(), "0 8 2 1 th08_01\n");
    Write((trialProfile / "active.ini").string().c_str(), active);
    extension->plugin->game_event(&event);
    Check(FirstByte(archiveHookW(L"thbgm.dat", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr))
            == 'n',
        "Editor archive should take precedence");
    const auto read = [&](const wchar_t* name) {
        auto file = archiveHookW(name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        char sample = 0;
        DWORD received = 0;
        Check(archiveRead(file, &sample, 1, &received, nullptr) && received == 1, "Random archive read failed");
        archiveClose(file);
        return sample;
    };
    const auto pick = read(L"thbgm.dat");
    const auto trialPick = read(L"thbgm_tr.dat");
    Check((pick == 'n' && trialPick == 'v') || (pick == 'v' && trialPick == 'n'),
        "Exactly one archive must read its alternate track");
    Check(read(L"thbgm.dat") == pick, "Random selection must remain stable during a session");
    std::filesystem::remove(datProfile / "active.ini");
    std::filesystem::remove(trialProfile / "active.ini");
    extension->plugin->game_event(&event);
    Check(FirstByte(CreateFileW(L"thbgm.dat", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr))
            == 'o',
        "Original archive changed");
    Write("thprac-bgm/thbgm.dat", "bad");
    Check(FirstByte(archiveHookA("thbgm.dat", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr))
            == 'o',
        "Wrong-sized archive should fall back to the original");
    std::filesystem::remove("thprac-bgm/thbgm.dat");
    Check(FirstByte(archiveHookW(L"thbgm.dat", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr))
            == 'o',
        "Missing archive should fall back to the original");
}
