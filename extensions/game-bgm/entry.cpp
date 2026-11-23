#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#include "thprac_plugin_sdk.h"

namespace {
void* originalMmioOpenA;
void* originalCreateFileA;
void* originalCreateFileW;
void* originalReadFile;
void* originalCloseHandle;
wchar_t gameDirectory[MAX_PATH];
unsigned randomSeed;

struct Generation {
    wchar_t directory[MAX_PATH] {};
    HANDLE lock = INVALID_HANDLE_VALUE;
    bool needsMetadata = false;
    std::atomic<bool> metadataOpened {false};

    bool CanPlay() const { return !needsMetadata || metadataOpened.load(std::memory_order_acquire); }
};
Generation waveGeneration, archiveGeneration, trialGeneration;

struct Variant {
    unsigned long long offset;
    unsigned long long length;
    HANDLE file;
};
std::mutex archiveMutex;
std::map<HANDLE, std::vector<Variant>> archiveVariants;

unsigned RandomIndex(unsigned key, unsigned count)
{
    unsigned number = randomSeed ^ key;
    number = (number ^ (number >> 16)) * 0x7feb352du;
    number = (number ^ (number >> 15)) * 0x846ca68bu;
    return (number ^ (number >> 16)) % count;
}

const wchar_t* FileName(const wchar_t* path)
{
    const auto* backslash = wcsrchr(path, L'\\');
    const auto* slash = wcsrchr(path, L'/');
    const auto* separator = slash && (!backslash || slash > backslash) ? slash : backslash;
    return separator ? separator + 1 : path;
}

bool IsGameFile(const wchar_t* requested, const wchar_t* subdirectory, const wchar_t* name)
{
    wchar_t resolved[MAX_PATH];
    const auto length = GetFullPathNameW(requested, MAX_PATH, resolved, nullptr);
    if (!length || length >= MAX_PATH) return false;

    wchar_t original[MAX_PATH];
    const auto originalLength = _snwprintf(original, MAX_PATH, L"%ls\\%ls%ls", gameDirectory, subdirectory, name);
    return originalLength >= 0 && originalLength < MAX_PATH
        && CompareStringOrdinal(resolved, -1, original, -1, TRUE) == CSTR_EQUAL;
}

void PinGeneration(Generation& target, const wchar_t* category)
{
    if (target.lock != INVALID_HANDLE_VALUE) CloseHandle(target.lock);
    target.lock = INVALID_HANDLE_VALUE;
    target.directory[0] = 0;
    target.needsMetadata = false;
    target.metadataOpened.store(false, std::memory_order_relaxed);
    wchar_t profile[MAX_PATH], generation[34], marker[MAX_PATH];
    const auto length = _snwprintf(profile, MAX_PATH,
        L"%ls\\thprac-bgm\\manager\\%ls\\active.ini", gameDirectory, category);
    if (length <= 0 || length >= MAX_PATH) return;
    if (GetPrivateProfileStringW(L"BGM", L"generation", L"", generation, 34, profile) != 32
        || wcsspn(generation, L"0123456789abcdef") != 32) return;
    const auto directoryLength = _snwprintf(target.directory, MAX_PATH,
        L"%ls\\thprac-bgm\\manager\\%ls\\%ls", gameDirectory, category, generation);
    if (directoryLength <= 0 || directoryLength >= MAX_PATH) {
        target.directory[0] = 0;
        return;
    }
    const auto markerLength = _snwprintf(marker, MAX_PATH, L"%ls\\in-use", target.directory);
    if (markerLength > 0 && markerLength < MAX_PATH)
        target.lock = CreateFileW(marker, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
    const auto requiredLength = _snwprintf(marker, MAX_PATH, L"%ls\\requires-metadata", target.directory);
    if (requiredLength > 0 && requiredLength < MAX_PATH)
        target.needsMetadata = GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
}

bool ManagedPath(const Generation& target, const wchar_t* name, wchar_t (&path)[MAX_PATH])
{
    if (!target.directory[0]) return false;
    const auto count = _snwprintf(path, MAX_PATH, L"%ls\\%ls", target.directory, name);
    return count > 0 && count < MAX_PATH && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

bool MusicPath(const Generation& target, const wchar_t* name, wchar_t (&replacement)[MAX_PATH], bool& managed)
{
    managed = target.CanPlay() && ManagedPath(target, name, replacement);
    if (managed) return true;
    const auto replacementLength = _snwprintf(replacement, MAX_PATH, L"%ls\\thprac-bgm\\%ls", gameDirectory, name);
    return replacementLength > 0 && replacementLength < MAX_PATH;
}

struct WaveInfo {
    PCMWAVEFORMAT format;
    DWORD dataBytes;
};

bool ReadWaveInfo(wchar_t* filename, WaveInfo& wave)
{
    const auto file = mmioOpenW(filename, nullptr, MMIO_READ | MMIO_ALLOCBUF);
    if (!file) return false;
    MMCKINFO riff {};
    MMCKINFO format {};
    MMCKINFO data {};
    riff.fccType = mmioFOURCC('W', 'A', 'V', 'E');
    format.ckid = mmioFOURCC('f', 'm', 't', ' ');
    data.ckid = mmioFOURCC('d', 'a', 't', 'a');
    const bool parsed = mmioDescend(file, &riff, nullptr, MMIO_FINDRIFF) == MMSYSERR_NOERROR
        && mmioDescend(file, &format, &riff, MMIO_FINDCHUNK) == MMSYSERR_NOERROR
        && format.cksize >= sizeof(wave.format)
        && mmioRead(file, reinterpret_cast<char*>(&wave.format), sizeof(wave.format)) == sizeof(wave.format)
        && wave.format.wf.wFormatTag == WAVE_FORMAT_PCM
        && mmioAscend(file, &format, 0) == MMSYSERR_NOERROR
        && mmioDescend(file, &data, &riff, MMIO_FINDCHUNK) == MMSYSERR_NOERROR
        && wave.format.wf.nBlockAlign != 0
        && data.cksize != 0 && data.cksize % wave.format.wf.nBlockAlign == 0;
    const auto fileSize = parsed ? mmioSeek(file, 0, SEEK_END) : -1;
    wave.dataBytes = data.cksize;
    mmioClose(file, 0);
    return fileSize >= 0 && static_cast<unsigned long long>(data.dwDataOffset) + data.cksize
        <= static_cast<unsigned long long>(fileSize);
}

HMMIO WINAPI OpenWave(LPSTR filename, LPMMIOINFO info, DWORD flags)
{
    const auto original = reinterpret_cast<decltype(&mmioOpenA)>(originalMmioOpenA);
    constexpr DWORD allowedFlags = MMIO_ALLOCBUF | MMIO_DENYWRITE | MMIO_DENYREAD | MMIO_EXCLUSIVE | MMIO_DENYNONE;
    if (!filename || (flags & ~allowedFlags) != 0 || (info && (info->pIOProc || info->fccIOProc)))
        return original(filename, info, flags);

    wchar_t path[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, filename, -1, path, MAX_PATH))
        return original(filename, info, flags);

    const auto* name = FileName(path);
    if (wcslen(name) != 11 || _wcsnicmp(name, L"th06_", 5) != 0 || _wcsicmp(name + 7, L".wav") != 0
        || name[5] < L'0' || name[5] > L'9' || name[6] < L'0' || name[6] > L'9')
        return original(filename, info, flags);
    const int track = (name[5] - L'0') * 10 + (name[6] - L'0');
    wchar_t replacement[MAX_PATH];
    WaveInfo source {};
    WaveInfo candidate {};
    bool managed = false;
    if (track >= 1 && track <= 17 && IsGameFile(path, L"bgm\\", name)
        && MusicPath(waveGeneration, name, replacement, managed)) {
        wchar_t variants[MAX_PATH], selected[MAX_PATH];
        if (managed && ManagedPath(waveGeneration, L"variants.ini", variants)) {
            const auto count = GetPrivateProfileIntW(L"WAV", name, 0, variants);
            const auto len = count > 1 && count <= 1000
                ? _snwprintf(selected, MAX_PATH, L"variants\\th06_%02d\\%u.wav", track, RandomIndex(track, count)) : -1;
            wchar_t alternate[MAX_PATH];
            if (len > 0 && len < MAX_PATH && ManagedPath(waveGeneration, selected, alternate))
                wcscpy_s(replacement, alternate);
        }
        if (ReadWaveInfo(replacement, candidate) && ReadWaveInfo(path, source)
            && candidate.dataBytes == source.dataBytes
            && std::memcmp(&candidate.format, &source.format, sizeof(source.format)) == 0) {
            if (const auto file = mmioOpenW(replacement, info, flags)) return file;
        }
    }
    return original(filename, info, flags);
}

void RegisterVariants(HANDLE handle, const Generation& generation, unsigned long long archiveSize)
{
    if (!originalReadFile || !originalCloseHandle) return;
    wchar_t path[MAX_PATH];
    FILE* index = nullptr;
    if (!ManagedPath(generation, L"variants.txt", path) || _wfopen_s(&index, path, L"r") != 0) return;
    std::vector<Variant> variants;
    unsigned long long offset, length;
    unsigned count, fallback;
    wchar_t name[20];
    while (fwscanf_s(index, L"%llu %llu %u %u %19ls", &offset, &length, &count, &fallback, name, 20u) == 5) {
        if (length == 0 || offset > archiveSize || length > archiveSize - offset || count <= 1 || count > 1000
            || fallback >= count || wcsspn(name, L"0123456789abcdefghijklmnopqrstuvwxyz-_") != wcslen(name)) continue;
        const auto choice = RandomIndex(static_cast<unsigned>(offset), count);
        if (choice == fallback) continue;
        wchar_t relative[MAX_PATH];
        const auto size = _snwprintf(relative, MAX_PATH, L"variants\\%ls\\%u.pcm", name, choice);
        if (size <= 0 || size >= MAX_PATH || !ManagedPath(generation, relative, path)) continue;
        const auto file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER actual;
        if (!GetFileSizeEx(file, &actual) || static_cast<unsigned long long>(actual.QuadPart) != length) {
            CloseHandle(file);
            continue;
        }
        variants.push_back({offset, length, file});
    }
    fclose(index);
    if (!variants.empty()) {
        std::lock_guard guard(archiveMutex);
        archiveVariants.emplace(handle, std::move(variants));
    }
}

BOOL WINAPI ReadArchive(HANDLE handle, LPVOID buffer, DWORD bytes, LPDWORD read, LPOVERLAPPED overlapped)
{
    const auto original = reinterpret_cast<decltype(&ReadFile)>(originalReadFile);
    if (overlapped || !read) return original(handle, buffer, bytes, read, overlapped);
    std::unique_lock guard(archiveMutex);
    const auto entry = archiveVariants.find(handle);
    if (entry == archiveVariants.end()) {
        guard.unlock();
        return original(handle, buffer, bytes, read, nullptr);
    }
    LARGE_INTEGER offset {};
    const auto tracked = SetFilePointerEx(handle, {}, &offset, FILE_CURRENT) != FALSE;
    const auto result = original(handle, buffer, bytes, read, nullptr);
    const auto error = GetLastError();
    if (result && tracked) {
        const auto from = static_cast<unsigned long long>(offset.QuadPart);
        const auto to = from + *read;
        for (const auto& variant : entry->second) {
            const auto start = from > variant.offset ? from : variant.offset;
            const auto variantEnd = variant.offset + variant.length;
            const auto end = to < variantEnd ? to : variantEnd;
            if (start >= end) continue;
            LARGE_INTEGER at;
            at.QuadPart = start - variant.offset;
            DWORD actual;
            if (SetFilePointerEx(variant.file, at, nullptr, FILE_BEGIN))
                ReadFile(variant.file, static_cast<char*>(buffer) + (start - from),
                    static_cast<DWORD>(end - start), &actual, nullptr);
        }
    }
    SetLastError(error);
    return result;
}

BOOL WINAPI CloseArchive(HANDLE handle)
{
    {
        std::lock_guard guard(archiveMutex);
        const auto entry = archiveVariants.find(handle);
        if (entry != archiveVariants.end()) {
            for (const auto& variant : entry->second) CloseHandle(variant.file);
            archiveVariants.erase(entry);
        }
    }
    return reinterpret_cast<decltype(&CloseHandle)>(originalCloseHandle)(handle);
}

HANDLE ReplaceArchive(HANDLE original, LPCWSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD flags, HANDLE fileTemplate)
{
    const auto* name = FileName(path);
    const auto nameLength = wcslen(name);
    if (nameLength <= 4 || _wcsicmp(name + nameLength - 4, L".dat") != 0) return original;
    if (!IsGameFile(path, L"", name)) return original;

    wchar_t replacement[MAX_PATH];
    const bool full = _wcsicmp(name, L"thbgm.dat") == 0;
    const bool trial = _wcsicmp(name, L"thbgm_tr.dat") == 0;
    const auto& generation = trial ? trialGeneration : archiveGeneration;
    Generation* metadataFor = nullptr;
    bool managed = false;
    if (full || trial) {
        if (!MusicPath(generation, name, replacement, managed)) return original;
    } else {
        wchar_t metadata[MAX_PATH];
        const auto count = _snwprintf(metadata, MAX_PATH, L"meta-%ls", name);
        if (count <= 0 || count >= MAX_PATH) return original;
        for (auto* candidate : {&waveGeneration, &archiveGeneration, &trialGeneration}) {
            if (ManagedPath(*candidate, metadata, replacement)) {
                metadataFor = candidate;
                break;
            }
        }
        if (!metadataFor) return original;
    }
    const auto candidate = CreateFileW(replacement, access, sharing, security, OPEN_EXISTING, flags, fileTemplate);
    if (candidate == INVALID_HANDLE_VALUE) return original;
    if (metadataFor) {
        metadataFor->metadataOpened.store(true, std::memory_order_release);
        CloseHandle(original);
        return candidate;
    }
    LARGE_INTEGER sourceSize, candidateSize;
    if (GetFileSizeEx(original, &sourceSize) && GetFileSizeEx(candidate, &candidateSize)
        && sourceSize.QuadPart > 0 && sourceSize.QuadPart == candidateSize.QuadPart) {
        CloseHandle(original);
        if (managed && !(flags & FILE_FLAG_OVERLAPPED)) RegisterVariants(candidate, generation, candidateSize.QuadPart);
        return candidate;
    }
    CloseHandle(candidate);
    return original;
}

bool MayReplaceArchive(HANDLE file, DWORD access, DWORD disposition, DWORD flags)
{
    return file != INVALID_HANDLE_VALUE && access == GENERIC_READ
        && disposition == OPEN_EXISTING && !(flags & FILE_FLAG_DELETE_ON_CLOSE);
}

HANDLE WINAPI OpenArchiveA(LPCSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD flags, HANDLE fileTemplate)
{
    const auto original = reinterpret_cast<decltype(&CreateFileA)>(originalCreateFileA);
    const auto file = original(path, access, sharing, security, disposition, flags, fileTemplate);
    const auto error = GetLastError();
    auto result = file;
    wchar_t widePath[MAX_PATH];
    if (path && MayReplaceArchive(file, access, disposition, flags)
        && MultiByteToWideChar(CP_ACP, 0, path, -1, widePath, MAX_PATH)) {
        result = ReplaceArchive(file, widePath, access, sharing, security, flags, fileTemplate);
    }
    SetLastError(error);
    return result;
}

HANDLE WINAPI OpenArchiveW(LPCWSTR path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD flags, HANDLE fileTemplate)
{
    const auto original = reinterpret_cast<decltype(&CreateFileW)>(originalCreateFileW);
    const auto file = original(path, access, sharing, security, disposition, flags, fileTemplate);
    const auto error = GetLastError();
    const auto result = path && MayReplaceArchive(file, access, disposition, flags)
        ? ReplaceArchive(file, path, access, sharing, security, flags, fileTemplate) : file;
    SetLastError(error);
    return result;
}

void __cdecl DrawSettings(const ThpracPluginUi* ui)
{
    const auto& config = ui->config;
    int enabled = config.get_int(config.plugin_id, "enabled", 1);
    const auto label = ui->locale == 0 ? "启用背景音乐替换"
        : ui->locale == 2 ? "BGMの差し替えを有効にする" : "Enable BGM replacement";
    if (ui->checkbox(label, &enabled) && !config.set_int(config.plugin_id, "enabled", enabled)) {
        ui->text(ui->locale == 0 ? "保存失败"
            : ui->locale == 2 ? "保存に失敗しました" : "Could not save the setting.");
    }
    ui->text(ui->locale == 0 ? "在工具页打开「背景音乐」。更改后重启游戏。"
        : ui->locale == 2 ? "ツールから「背景音乐」を開き、変更後にゲームを再起動してください。"
        : "Open Background Music on the Tools page and restart the game after applying changes.");
}

void __cdecl GameEvent(const ThpracPluginGameEvent* event)
{
    if (event->kind != THPRAC_EVENT_GAME_READY || !event->hook_import
        || !event->config.get_int(event->config.plugin_id, "enabled", 1)) return;

    const auto length = GetModuleFileNameW(nullptr, gameDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    auto* separator = wcsrchr(gameDirectory, L'\\');
    if (!separator) return;
    *separator = L'\0';
    LARGE_INTEGER performance {};
    QueryPerformanceCounter(&performance);
    randomSeed = performance.LowPart ^ GetCurrentProcessId();
    if (std::strcmp(event->game_id, "th06") == 0) {
        PinGeneration(waveGeneration, L"wav");
        event->hook_import("winmm.dll", "mmioOpenA", reinterpret_cast<void*>(OpenWave), &originalMmioOpenA);
    } else {
        PinGeneration(archiveGeneration, L"thbgm");
        PinGeneration(trialGeneration, L"thbgm_tr");
        event->hook_import("kernel32.dll", "CloseHandle", reinterpret_cast<void*>(CloseArchive), &originalCloseHandle);
        if (originalCloseHandle)
            event->hook_import("kernel32.dll", "ReadFile", reinterpret_cast<void*>(ReadArchive), &originalReadFile);
    }
    event->hook_import("kernel32.dll", "CreateFileA", reinterpret_cast<void*>(OpenArchiveA), &originalCreateFileA);
    event->hook_import("kernel32.dll", "CreateFileW", reinterpret_cast<void*>(OpenArchiveW), &originalCreateFileW);
}

const char* const games[] {
    "th06", "th07", "th08", "th09", "th10", "th11", "th12", "th13", "th14", "th15",
    "th16", "th17", "th18", "th19", "th20", "th095", "th125", "th128", "th143", "th165",
    "th185", nullptr
};
const ThpracPluginCallbacks callbacks {sizeof(callbacks), THPRAC_PLUGIN_SDK_VERSION, games, DrawSettings, GameEvent};
const ThpracExtension extension {sizeof(extension), "rueee-c7.game-bgm", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, &callbacks};
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
