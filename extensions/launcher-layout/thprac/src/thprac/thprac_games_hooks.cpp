#include "feature_runtime.h"

#include "thprac_games_hooks.h"
#include <vector>
#include "../3rdParties/d3d8/include/dsound.h"
#include "thprac_hook.h"
#include <string>
#include <cstring>
#include <mmsystem.h>
#include "thprac_utils.h"
#include "thprac_native_volume.h"

namespace THPrac {
bool g_is_d3d_hooked = false;
extern DWORD* g_gameGuiDevice;
extern DWORD* g_gameGuiHwnd;
extern std::unordered_map<KeyDefine, KeyDefine, KeyDefineHashFunction> g_keybind;// launcher_cfg.cpp


#pragma region Sound
struct SoundOpt {
    HRESULT(STDMETHODCALLTYPE* g_realCreateSoundBuffer)
    (IDirectSound8* thiz, LPCDSBUFFERDESC pcDSBufferDesc, LPDIRECTSOUNDBUFFER* ppDSBuffer, LPUNKNOWN pUnkOuter);
    HRESULT(STDMETHODCALLTYPE* g_realDuplicateSoundBuffer)
    (IDirectSound8* thiz, LPDIRECTSOUNDBUFFER pDSBufferOriginal, LPDIRECTSOUNDBUFFER* ppDSBufferDuplicate);
    bool enable_fasterBGM;
    bool no_change_pitch;
    GameVolumeSettings volume;
    float pitch_ofs = 1.0f;
    struct SB_Struct {
        LPDIRECTSOUNDBUFFER p_sb;
        WAVEFORMATEX orig_format;
        DWORD* vtbl_orig;
        DWORD* vtbl_changed;
        LONG base_volume = 0;
        bool is_bgm = false;
    };
    float bgm_speed = 1.0f;
    std::vector<SB_Struct> soundbuffers;
} g_sound_opt;
static int g_gameVolumeVersion = 0;
static bool g_nativeVolumeReady = false;
static int g_th06BgmRestoreFrames = 0;
static NativeVolumeLayout g_nativeVolume;
static bool g_pendingNativeVolume = false;
static void AttachEarlyGameSound();


static bool ReadableMemory(uintptr_t address, size_t size)
{
    MEMORY_BASIC_INFORMATION region {};
    return address && VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))
        && region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        && address + size >= address
        && address + size <= reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
}

static bool WritableImageMemory(uintptr_t address, size_t size)
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const uintptr_t start = base + section->VirtualAddress;
        if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) && address >= start
            && address + size >= address && address + size <= start + section->Misc.VirtualSize)
            return true;
    }
    return false;
}

static NativeVolumeLayout DiscoverNativeVolume()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    // TH08's original implementation is also available as reconstructed source.
    if (g_gameVolumeVersion == 8)
        return { base + 0x13ce897, base + 0x14bdc80 };
    // TH06/07 have music mode and sound on/off settings, but no native percentages.
    if (g_gameVolumeVersion == 6 || g_gameVolumeVersion == 7 || !g_gameVolumeVersion)
        return {};
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    NativeVolumeLayout found;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        const uintptr_t start = base + section->VirtualAddress;
        const auto candidate = FindNativeVolumeLayout(reinterpret_cast<const uint8_t*>(start),
            section->Misc.VirtualSize, start, WritableImageMemory);
        if (candidate.ambiguous)
            return {};
        if (!candidate)
            continue;
        if (found && (found.config != candidate.config || found.runtime != candidate.runtime))
            return {};
        found = candidate;
    }
    return found;
}

void SetGameVolumeGameVersion(int version)
{
    g_gameVolumeVersion = version;
    g_nativeVolumeReady = false;
    g_pendingNativeVolume = false;
    g_nativeVolume = DiscoverNativeVolume();
    if (const auto* volume = VolumeAddon()) volume->begin();
}

HRESULT STDMETHODCALLTYPE CreateSoundBuffer_Changed(IDirectSound8*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer_Changed(IDirectSound8*, LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER*);
static HRESULT(WINAPI* g_realDirectSoundCreate8)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
static HRESULT(WINAPI* g_realDirectSoundCreate)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);

static void HookSoundDevice(void* device)
{
    if (!device)
        return;
    auto** vtable = *reinterpret_cast<void***>(device);
    if (vtable[3] != reinterpret_cast<void*>(CreateSoundBuffer_Changed))
        HookVTable(device, 3, CreateSoundBuffer_Changed, reinterpret_cast<void**>(&g_sound_opt.g_realCreateSoundBuffer));
    if (vtable[5] != reinterpret_cast<void*>(DuplicateSoundBuffer_Changed))
        HookVTable(device, 5, DuplicateSoundBuffer_Changed, reinterpret_cast<void**>(&g_sound_opt.g_realDuplicateSoundBuffer));
}

static HRESULT WINAPI DirectSoundCreate8_Changed(LPCGUID guid, LPDIRECTSOUND8* out, LPUNKNOWN outer)
{
    HRESULT hr = g_realDirectSoundCreate8(guid, out, outer);
    if (SUCCEEDED(hr) && out)
        HookSoundDevice(*out);
    return hr;
}

static HRESULT WINAPI DirectSoundCreate_Changed(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN outer)
{
    HRESULT hr = g_realDirectSoundCreate(guid, out, outer);
    if (SUCCEEDED(hr) && out)
        HookSoundDevice(*out);
    return hr;
}

static bool HookGameDirectSoundCreate()
{
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress)
        return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    bool hooked = false;
    for (; descriptor->Name; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char*>(base + descriptor->Name), "dsound.dll"))
            continue;
        if (!descriptor->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* entries = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++entries) {
            const bool ordinal = IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) != 0;
            const char* name = ordinal ? nullptr
                : reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
            const bool create8 = ordinal ? IMAGE_ORDINAL(names->u1.Ordinal) == 11
                : strcmp(name, "DirectSoundCreate8") == 0;
            const bool create = ordinal ? IMAGE_ORDINAL(names->u1.Ordinal) == 1
                : strcmp(name, "DirectSoundCreate") == 0;
            if (!create8 && !create)
                continue;
            if (create8)
                g_realDirectSoundCreate8 = reinterpret_cast<decltype(g_realDirectSoundCreate8)>(entries->u1.Function);
            else
                g_realDirectSoundCreate = reinterpret_cast<decltype(g_realDirectSoundCreate)>(entries->u1.Function);
            DWORD previous;
            if (!VirtualProtect(&entries->u1.Function, sizeof(entries->u1.Function), PAGE_READWRITE, &previous))
                continue;
            entries->u1.Function = reinterpret_cast<uintptr_t>(create8
                ? reinterpret_cast<void*>(DirectSoundCreate8_Changed)
                : reinterpret_cast<void*>(DirectSoundCreate_Changed));
            DWORD ignored;
            VirtualProtect(&entries->u1.Function, sizeof(entries->u1.Function), previous, &ignored);
            hooked = true;
        }
    }
    return hooked;
}

struct MidiVolumeDevice {
    HMIDIOUT handle;
    BYTE channels[16];
};
static SRWLOCK g_midiVolumeLock = SRWLOCK_INIT;
static std::vector<MidiVolumeDevice> g_midiVolumeDevices;
static MMRESULT(WINAPI* g_realMidiOutOpen)(LPHMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT(WINAPI* g_realMidiOutClose)(HMIDIOUT);
static MMRESULT(WINAPI* g_realMidiOutShortMsg)(HMIDIOUT, DWORD);
static MMRESULT(WINAPI* g_realMidiOutReset)(HMIDIOUT);

static BYTE ScaledMidiVolume(BYTE original)
{
    const int percent = g_sound_opt.volume.bgm;
    return static_cast<BYTE>(std::clamp((original * percent + 50) / 100, 0, 127));
}

static void ApplyMidiVolume()
{
    if (!g_realMidiOutShortMsg)
        return;
    AcquireSRWLockShared(&g_midiVolumeLock);
    for (const auto& device : g_midiVolumeDevices) {
        for (DWORD channel = 0; channel < 16; ++channel) {
            const DWORD message = (0xb0 | channel) | (7 << 8)
                | (static_cast<DWORD>(ScaledMidiVolume(device.channels[channel])) << 16);
            g_realMidiOutShortMsg(device.handle, message);
        }
    }
    ReleaseSRWLockShared(&g_midiVolumeLock);
}

static MMRESULT WINAPI MidiOutOpen_Changed(LPHMIDIOUT handle, UINT deviceId,
    DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
    const MMRESULT result = g_realMidiOutOpen(handle, deviceId, callback, instance, flags);
    if (result == MMSYSERR_NOERROR && handle) {
        MidiVolumeDevice device { *handle, {} };
        std::fill(std::begin(device.channels), std::end(device.channels), static_cast<BYTE>(100));
        AcquireSRWLockExclusive(&g_midiVolumeLock);
        g_midiVolumeDevices.push_back(device);
        ReleaseSRWLockExclusive(&g_midiVolumeLock);
        ApplyMidiVolume();
    }
    return result;
}

static MMRESULT WINAPI MidiOutClose_Changed(HMIDIOUT handle)
{
    const MMRESULT result = g_realMidiOutClose(handle);
    if (result == MMSYSERR_NOERROR) {
        AcquireSRWLockExclusive(&g_midiVolumeLock);
        std::erase_if(g_midiVolumeDevices, [handle](const auto& device) { return device.handle == handle; });
        ReleaseSRWLockExclusive(&g_midiVolumeLock);
    }
    return result;
}

static MMRESULT WINAPI MidiOutShortMsg_Changed(HMIDIOUT handle, DWORD message)
{
    if ((message & 0xf0) == 0x90 && g_sound_opt.volume.bgm == 0)
        message &= 0xff00ffff; // Velocity zero is a note-off, even after a MIDI device reset.
    if ((message & 0xf0) == 0xb0 && ((message >> 8) & 0x7f) == 7) {
        const auto channel = message & 0xf;
        const auto original = static_cast<BYTE>((message >> 16) & 0x7f);
        AcquireSRWLockExclusive(&g_midiVolumeLock);
        for (auto& device : g_midiVolumeDevices) {
            if (device.handle == handle) {
                device.channels[channel] = original;
                message = (message & 0xff00ffff)
                    | (static_cast<DWORD>(ScaledMidiVolume(original)) << 16);
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_midiVolumeLock);
    }
    return g_realMidiOutShortMsg(handle, message);
}

static MMRESULT WINAPI MidiOutReset_Changed(HMIDIOUT handle)
{
    const MMRESULT result = g_realMidiOutReset(handle);
    if (result == MMSYSERR_NOERROR)
        ApplyMidiVolume();
    return result;
}

static void HookGameMidiVolume()
{
    if (g_gameVolumeVersion != 6 && g_gameVolumeVersion != 7)
        return;
    const auto module = GetModuleHandleW(nullptr);
    HookIAT(module, "WINMM.dll", "midiOutShortMsg", MidiOutShortMsg_Changed,
        reinterpret_cast<void**>(&g_realMidiOutShortMsg));
    HookIAT(module, "WINMM.dll", "midiOutOpen", MidiOutOpen_Changed,
        reinterpret_cast<void**>(&g_realMidiOutOpen));
    HookIAT(module, "WINMM.dll", "midiOutClose", MidiOutClose_Changed,
        reinterpret_cast<void**>(&g_realMidiOutClose));
    HookIAT(module, "WINMM.dll", "midiOutReset", MidiOutReset_Changed,
        reinterpret_cast<void**>(&g_realMidiOutReset));
}

static LONG AdjustedVolume(LONG base, bool isBgm)
{
    if (g_nativeVolume)
        return base; // Native percentages already enter the game's volume curve.
    const int percent = isBgm
        ? g_sound_opt.volume.bgm
        : g_sound_opt.volume.effects;
    if (percent <= 0)
        return DSBVOLUME_MIN;
    const LONG adjustment = static_cast<LONG>(2000.0 * log10(percent / 100.0));
    return std::clamp(base + adjustment, static_cast<LONG>(DSBVOLUME_MIN), static_cast<LONG>(DSBVOLUME_MAX));
}

GameVolumeSettings GetGameVolume()
{
    return g_sound_opt.volume;
}

static GameVolumeSettings g_lastNativeVolume;

static void UpdateNativeBgmBuffer(LPDIRECTSOUNDBUFFER buffer, int previous, int current)
{
    LONG volume;
    if (!buffer || FAILED(buffer->GetVolume(&volume)))
        return;
    if (current == 0) {
        buffer->SetVolume(DSBVOLUME_MIN);
        return;
    }
    const float remaining = 1.0f - current / 100.0f;
    const float factor = 1.0f - remaining * remaining;
    if (previous == 0) {
        volume = static_cast<LONG>(5000.0f * factor) - 5000;
    } else if (volume > DSBVOLUME_MIN) {
        // Preserve the native fade offset when changing the master percentage.
        const float oldRemaining = 1.0f - previous / 100.0f;
        const float oldFactor = 1.0f - oldRemaining * oldRemaining;
        volume = static_cast<LONG>((volume + 5000) * factor / oldFactor) - 5000;
    }
    buffer->SetVolume(std::clamp(volume, static_cast<LONG>(DSBVOLUME_MIN), static_cast<LONG>(DSBVOLUME_MAX)));
}

static uintptr_t ReadPointer(uintptr_t address)
{
    return ReadableMemory(address, sizeof(uintptr_t)) ? *reinterpret_cast<uintptr_t*>(address) : 0;
}

static uintptr_t SoundBufferQueryInterface()
{
    static uintptr_t queryInterface = 0;
    if (queryInterface)
        return queryInterface;
    // A silent, never-played buffer supplies an exact COM implementation identity
    // for validating unfamiliar SoundPlayer layouts. Never call guessed vtables.
    LPDIRECTSOUND8 device = nullptr;
    if (FAILED(DirectSoundCreate8(nullptr, &device, nullptr)))
        return 0;
    device->SetCooperativeLevel(GetDesktopWindow(), DSSCL_NORMAL);
    WAVEFORMATEX format { WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0 };
    DSBUFFERDESC descriptor {};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPOSITIONNOTIFY;
    descriptor.dwBufferBytes = 512;
    descriptor.lpwfxFormat = &format;
    LPDIRECTSOUNDBUFFER buffer = nullptr;
    if (SUCCEEDED(device->CreateSoundBuffer(&descriptor, &buffer, nullptr))) {
        queryInterface = ReadPointer(ReadPointer(reinterpret_cast<uintptr_t>(buffer)));
        buffer->Release();
    }
    device->Release();
    return queryInterface;
}

static LPDIRECTSOUNDBUFFER FindNativeBgmBuffer(uintptr_t slot, unsigned buffersOffset, uintptr_t identity)
{
    if (!WritableImageMemory(slot, sizeof(uintptr_t)))
        return nullptr;
    const auto manager = ReadPointer(slot);
    const auto array = ReadPointer(manager ? manager + buffersOffset : 0);
    const auto buffer = ReadPointer(array);
    const auto vtable = ReadPointer(buffer);
    if (!identity || ReadPointer(vtable) != identity)
        return nullptr;
    LPDIRECTSOUNDBUFFER candidate = nullptr;
    auto* unknown = reinterpret_cast<IUnknown*>(buffer);
    if (FAILED(unknown->QueryInterface(IID_IDirectSoundBuffer, reinterpret_cast<void**>(&candidate))))
        return nullptr;
    DSBCAPS caps {};
    caps.dwSize = sizeof(caps);
    if (FAILED(candidate->GetCaps(&caps)) || !(caps.dwFlags & DSBCAPS_CTRLPOSITIONNOTIFY)) {
        candidate->Release();
        return nullptr;
    }
    return candidate;
}

static void ApplyNativeBgmVolume(int previous, int current)
{
    if (previous == current)
        return;
    std::vector<LPDIRECTSOUNDBUFFER> updated;
    const auto update = [&](LPDIRECTSOUNDBUFFER buffer) {
        if (std::find(updated.begin(), updated.end(), buffer) == updated.end()) {
            updated.push_back(buffer);
            UpdateNativeBgmBuffer(buffer, previous, current);
        }
    };
    // The older and newer SoundPlayer/CSound layouts differ. These offsets are
    // verified against the respective executables (TH08 also has source maps).
    int managerOffset = 0;
    int buffersOffset = g_gameVolumeVersion >= 17 && g_gameVolumeVersion < 95 ? 8 : 4;
    switch (g_gameVolumeVersion) {
    case 8: managerOffset = 0x10; break;
    case 10: managerOffset = 0xbc; break;
    case 11: managerOffset = 0xe0; break;
    case 12: managerOffset = 0x28; break;
    case 13: case 14: case 15: case 16: case 17: case 18: managerOffset = 0x2c; break;
    case 20: managerOffset = 0x18; break;
    }
    if (managerOffset) {
        const auto manager = ReadPointer(g_nativeVolume.runtime - managerOffset);
        const auto buffers = ReadPointer(manager ? manager + buffersOffset : 0);
        const auto buffer = ReadPointer(buffers);
        if (buffer && ReadPointer(buffer))
            update(reinterpret_cast<LPDIRECTSOUNDBUFFER>(buffer));
    } else {
        // Side games and TH19: locate the live stream near its native volume
        // fields, accepting only the actual DirectSound COM implementation and
        // a streaming buffer. This also works when attaching after BGM creation.
        const auto identity = SoundBufferQueryInterface();
        if (identity) {
            for (uintptr_t slot = g_nativeVolume.runtime - 0x200;
                 slot < g_nativeVolume.runtime; slot += sizeof(uintptr_t)) {
                for (const unsigned offset : { 4u, 8u }) {
                    if (auto* buffer = FindNativeBgmBuffer(slot, offset, identity)) {
                        update(buffer);
                        buffer->Release();
                    }
                }
            }
        }
    }
    // Also covers streams created through hooks, including the side games.
    for (const auto& buffer : g_sound_opt.soundbuffers) {
        if (buffer.is_bgm)
            update(buffer.p_sb);
    }
}

void SetGameVolume(const GameVolumeSettings& settings)
{
    const bool wasSilent = g_sound_opt.volume.bgm == 0;
    g_sound_opt.volume = { std::clamp(settings.bgm, 0, 100), std::clamp(settings.effects, 0, 100) };
    if (g_gameVolumeVersion == 6 && wasSilent && g_sound_opt.volume.bgm > 0)
        g_th06BgmRestoreFrames = 120;
    if (g_nativeVolume) {
        if (!g_nativeVolumeReady) {
            g_pendingNativeVolume = true;
            return;
        }
        const int previous = std::clamp(*reinterpret_cast<int*>(g_nativeVolume.runtime), 0, 100);
        const int bgm = g_sound_opt.volume.bgm;
        *reinterpret_cast<BYTE*>(g_nativeVolume.config) = static_cast<BYTE>(bgm);
        *reinterpret_cast<BYTE*>(g_nativeVolume.config + 1) = static_cast<BYTE>(g_sound_opt.volume.effects);
        *reinterpret_cast<int*>(g_nativeVolume.runtime) = bgm;
        *reinterpret_cast<int*>(g_nativeVolume.runtime + 4) = g_sound_opt.volume.effects;
        const float remaining = 1.0f - bgm / 100.0f;
        const float remainingFourth = remaining * remaining * remaining * remaining;
        // Match the game's option-copy cache, including its effects-zero branch.
        if (g_gameVolumeVersion != 8)
            *reinterpret_cast<int*>(g_nativeVolume.runtime + 8) = g_sound_opt.volume.effects == 0
                ? DSBVOLUME_MIN : -5000 - static_cast<int>(-5000.0f * (1.0f - remainingFourth));
        ApplyNativeBgmVolume(previous, bgm);
        g_lastNativeVolume = g_sound_opt.volume;
        g_pendingNativeVolume = false;
        return;
    }
    for (auto& buffer : g_sound_opt.soundbuffers) {
        auto realSetVolume = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(LPDIRECTSOUNDBUFFER, LONG)>(buffer.vtbl_orig[15]);
        realSetVolume(buffer.p_sb, AdjustedVolume(buffer.base_volume, buffer.is_bgm));
    }
    ApplyMidiVolume();
}

void PollGameVolumeBroadcast()
{
    if (g_gameVolumeVersion == 6 || g_gameVolumeVersion == 7)
        AttachEarlyGameSound();
    if (g_gameVolumeVersion == 6 && g_th06BgmRestoreFrames > 0) {
        for (auto& buffer : g_sound_opt.soundbuffers) {
            if (!buffer.is_bgm)
                continue;
            auto realSetVolume = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(LPDIRECTSOUNDBUFFER, LONG)>(buffer.vtbl_orig[15]);
            realSetVolume(buffer.p_sb, AdjustedVolume(buffer.base_volume, true));
        }
        --g_th06BgmRestoreFrames;
    }
    if (g_nativeVolume) {
        const GameVolumeSettings native {
            *reinterpret_cast<BYTE*>(g_nativeVolume.config),
            *reinterpret_cast<BYTE*>(g_nativeVolume.config + 1)
        };
        if (native.bgm <= 100 && native.effects <= 100) {
            if (!g_nativeVolumeReady) {
                g_nativeVolumeReady = true;
                if (g_pendingNativeVolume)
                    SetGameVolume(g_sound_opt.volume);
                else
                    g_lastNativeVolume = g_sound_opt.volume = native;
            } else if (native.bgm != g_lastNativeVolume.bgm || native.effects != g_lastNativeVolume.effects) {
                // Native menus already update the current sound; only adopt their
                // exact percentage here, so ongoing fades are not restarted.
                g_lastNativeVolume = g_sound_opt.volume = native;
            }
        }
    }
    if (const auto* volume = VolumeAddon()) {
        GameVolumeSettings settings;
        if (volume->poll(&settings.bgm, &settings.effects)) SetGameVolume(settings);
    }
}

HRESULT STDMETHODCALLTYPE GetVolume_Changed(LPDIRECTSOUNDBUFFER thiz, LPLONG volume)
{
    for (const auto& buffer : g_sound_opt.soundbuffers) {
        if (buffer.p_sb == thiz) {
            if (!volume)
                return DSERR_INVALIDPARAM;
            *volume = buffer.base_volume;
            return DS_OK;
        }
    }
    return DSERR_INVALIDPARAM;
}

HRESULT STDMETHODCALLTYPE SetVolume_Changed(LPDIRECTSOUNDBUFFER thiz, LONG volume)
{
    if (volume < DSBVOLUME_MIN || volume > DSBVOLUME_MAX)
        return DSERR_INVALIDPARAM;
    for (auto& buffer : g_sound_opt.soundbuffers) {
        if (buffer.p_sb == thiz) {
            auto realSetVolume = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(LPDIRECTSOUNDBUFFER, LONG)>(buffer.vtbl_orig[15]);
            const HRESULT result = realSetVolume(thiz, AdjustedVolume(volume, buffer.is_bgm));
            if (SUCCEEDED(result))
                buffer.base_volume = volume;
            return result;
        }
    }
    return DSERR_INVALIDPARAM;
}

HRESULT STDMETHODCALLTYPE SetFormat_Changed(LPDIRECTSOUNDBUFFER thiz, LPCWAVEFORMATEX pcfxFormat)
{
    for (auto i = g_sound_opt.soundbuffers.begin(); i != g_sound_opt.soundbuffers.end(); i++) {
        if (i->p_sb == thiz) {
            i->orig_format = *pcfxFormat;
            HRESULT(STDMETHODCALLTYPE * realSetFormat)
            (LPDIRECTSOUNDBUFFER thiz, LPCWAVEFORMATEX pcfxFormat);
            realSetFormat = (decltype(realSetFormat))(i->vtbl_orig[14]);
            HRESULT res = realSetFormat(thiz, pcfxFormat);
            return res;
        }
    }
    return -1;
}

HRESULT(STDMETHODCALLTYPE SoundBuffer_Release_Changed)
(LPDIRECTSOUNDBUFFER thiz)
{
    for (auto i = g_sound_opt.soundbuffers.begin(); i != g_sound_opt.soundbuffers.end();) {
        if (i->p_sb == thiz) {
            HRESULT(STDMETHODCALLTYPE * realSoundBuffer_Release)
            (LPDIRECTSOUNDBUFFER thiz);
            realSoundBuffer_Release = (decltype(realSoundBuffer_Release))(i->vtbl_orig[2]);
            HRESULT res = realSoundBuffer_Release(thiz);
            if (res == 0) {
                delete[] i->vtbl_changed;
                delete[] i->vtbl_orig;
                i = g_sound_opt.soundbuffers.erase(i);
            }
            return res;
        } else {
            i++;
        }
    }
    return -1;
}

HRESULT STDMETHODCALLTYPE SoundBuffer_Initialize_Changed(LPDIRECTSOUNDBUFFER thiz, LPDIRECTSOUND pDirectSound, LPCDSBUFFERDESC pcDSBufferDesc)
{
    for (auto i = g_sound_opt.soundbuffers.begin(); i != g_sound_opt.soundbuffers.end(); i++) {
        if (i->p_sb == thiz) {

            DSBUFFERDESC desc = *pcDSBufferDesc;
            desc.dwFlags |= DSBCAPS_CTRLVOLUME;
            if (i->is_bgm && g_sound_opt.enable_fasterBGM)
                desc.dwFlags |= DSBCAPS_CTRLFREQUENCY;

            HRESULT(STDMETHODCALLTYPE * realSoundBuffer_Initialize)
            (LPDIRECTSOUNDBUFFER thiz, LPDIRECTSOUND pDirectSound, LPCDSBUFFERDESC pcDSBufferDesc);
            realSoundBuffer_Initialize = (decltype(realSoundBuffer_Initialize))(i->vtbl_orig[10]);
            HRESULT res = realSoundBuffer_Initialize(thiz, pDirectSound, &desc);
            return res;
        }
    }
    return -1;
}

HRESULT STDMETHODCALLTYPE Play_Changed(LPDIRECTSOUNDBUFFER thiz, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags)
{
    for (auto i = g_sound_opt.soundbuffers.begin(); i != g_sound_opt.soundbuffers.end(); i++) {
        if (i->p_sb == thiz) {
            HRESULT(STDMETHODCALLTYPE * realPlay)
            (LPDIRECTSOUNDBUFFER thiz, DWORD dwReserved1, DWORD dwPriority, DWORD dwFlags);
            realPlay = (decltype(realPlay))(i->vtbl_orig[12]);


            if (i->orig_format.nChannels != 0) {
                WAVEFORMATEX fmt = i->orig_format;
                i->p_sb->SetFrequency((DWORD)floorf(fmt.nSamplesPerSec * g_sound_opt.bgm_speed));
            }

            HRESULT res = realPlay(thiz, dwReserved1, dwPriority, dwFlags);
            return res;
        }
    }
    return -1;
}

/****************************************************************************
 *
 * NAME: smbPitchShift.cpp
 * VERSION: 1.2
 * HOME URL: http://blogs.zynaptiq.com/bernsee
 * KNOWN BUGS: none
 *
 * SYNOPSIS: Routine for doing pitch shifting while maintaining
 * duration using the Short Time Fourier Transform.
 *
 * DESCRIPTION: The routine takes a pitchShift factor value which is between 0.5
 * (one octave down) and 2. (one octave up). A value of exactly 1 does not change
 * the pitch. numSampsToProcess tells the routine how many samples in indata[0...
 * numSampsToProcess-1] should be pitch shifted and moved to outdata[0 ...
 * numSampsToProcess-1]. The two buffers can be identical (ie. it can process the
 * data in-place). fftFrameSize defines the FFT frame size used for the
 * processing. Typical values are 1024, 2048 and 4096. It may be any value <=
 * MAX_FRAME_LENGTH but it MUST be a power of 2. osamp is the STFT
 * oversampling factor which also determines the overlap between adjacent STFT
 * frames. It should at least be 4 for moderate scaling ratios. A value of 32 is
 * recommended for best quality. sampleRate takes the sample rate for the signal
 * in unit Hz, ie. 44100 for 44.1 kHz audio. The data passed to the routine in
 * indata[] should be in the range [-1.0, 1.0), which is also the output range
 * for the data, make sure you scale the data accordingly (for 16bit signed integers
 * you would have to divide (and multiply) by 32768).
 *
 * COPYRIGHT 1999-2015 Stephan M. Bernsee <s.bernsee [AT] zynaptiq [DOT] com>
 *
 * 						The Wide Open License (WOL)
 *
 * Permission to use, copy, modify, distribute and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice and this license appear in all source copies.
 * THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY OF
 * ANY KIND. See http://www.dspguru.com/wol.htm for more information.
 *
 *****************************************************************************/


#define M_PI 3.14159265358979323846
#define MAX_FRAME_LENGTH 8192

void smbFft(float* fftBuffer, long fftFrameSize, long sign)
{
    float wr, wi, arg, *p1, *p2, temp;
    float tr, ti, ur, ui, *p1r, *p1i, *p2r, *p2i;
    long i, bitm, j, le, le2, k;
    for (i = 2; i < 2 * fftFrameSize - 2; i += 2) {
        for (bitm = 2, j = 0; bitm < 2 * fftFrameSize; bitm <<= 1) {
            if (i & bitm)
                j++;
            j <<= 1;
        }
        if (i < j) {
            p1 = fftBuffer + i;
            p2 = fftBuffer + j;
            temp = *p1;
            *(p1++) = *p2;
            *(p2++) = temp;
            temp = *p1;
            *p1 = *p2;
            *p2 = temp;
        }
    }
    for (k = 0, le = 2; k < (long)(log(fftFrameSize) / log(2.) + .5); k++) {
        le <<= 1;
        le2 = le >> 1;
        ur = 1.0;
        ui = 0.0;
        arg = static_cast<float>(M_PI / (le2 >> 1));
        wr = cos(arg);
        wi = sign * sin(arg);
        for (j = 0; j < le2; j += 2) {
            p1r = fftBuffer + j;
            p1i = p1r + 1;
            p2r = p1r + le2;
            p2i = p2r + 1;
            for (i = j; i < 2 * fftFrameSize; i += le) {
                tr = *p2r * ur - *p2i * ui;
                ti = *p2r * ui + *p2i * ur;
                *p2r = *p1r - tr;
                *p2i = *p1i - ti;
                *p1r += tr;
                *p1i += ti;
                p1r += le;
                p1i += le;
                p2r += le;
                p2i += le;
            }
            tr = ur * wr - ui * wi;
            ui = ur * wi + ui * wr;
            ur = tr;
        }
    }
}

void smbPitchShift(float pitchShift, long numSampsToProcess, long fftFrameSize, long osamp, float sampleRate, float* indata, float* outdata)
{
    static float gInFIFO[MAX_FRAME_LENGTH] = { 0 };
    static float gOutFIFO[MAX_FRAME_LENGTH] = { 0 };
    static float gFFTworksp[2 * MAX_FRAME_LENGTH] = { 0 };
    static float gLastPhase[MAX_FRAME_LENGTH / 2 + 1] = { 0 };
    static float gSumPhase[MAX_FRAME_LENGTH / 2 + 1] = { 0 };
    static float gOutputAccum[2 * MAX_FRAME_LENGTH] = { 0 };
    static float gAnaFreq[MAX_FRAME_LENGTH] = { 0 };
    static float gAnaMagn[MAX_FRAME_LENGTH] = { 0 };
    static float gSynFreq[MAX_FRAME_LENGTH] = { 0 };
    static float gSynMagn[MAX_FRAME_LENGTH] = { 0 };
    static long gRover = false, gInit = false;
    double magn, phase, tmp, window, real, imag;
    double freqPerBin, expct;
    long i, k, qpd, index, inFifoLatency, stepSize, fftFrameSize2;

    /* set up some handy variables */
    fftFrameSize2 = fftFrameSize / 2;
    stepSize = fftFrameSize / osamp;
    freqPerBin = sampleRate / (double)fftFrameSize;
    expct = 2. * M_PI * (double)stepSize / (double)fftFrameSize;
    inFifoLatency = fftFrameSize - stepSize;
    if (gRover == false)
        gRover = inFifoLatency;

    /* initialize our static arrays */
    if (gInit == false) {
        memset(gInFIFO, 0, MAX_FRAME_LENGTH * sizeof(float));
        memset(gOutFIFO, 0, MAX_FRAME_LENGTH * sizeof(float));
        memset(gFFTworksp, 0, 2 * MAX_FRAME_LENGTH * sizeof(float));
        memset(gLastPhase, 0, (MAX_FRAME_LENGTH / 2 + 1) * sizeof(float));
        memset(gSumPhase, 0, (MAX_FRAME_LENGTH / 2 + 1) * sizeof(float));
        memset(gOutputAccum, 0, 2 * MAX_FRAME_LENGTH * sizeof(float));
        memset(gAnaFreq, 0, MAX_FRAME_LENGTH * sizeof(float));
        memset(gAnaMagn, 0, MAX_FRAME_LENGTH * sizeof(float));
        gInit = true;
    }

    /* main processing loop */
    for (i = 0; i < numSampsToProcess; i++) {

        /* As long as we have not yet collected enough data just read in */
        gInFIFO[gRover] = indata[i];
        outdata[i] = gOutFIFO[gRover - inFifoLatency];
        gRover++;

        /* now we have enough data for processing */
        if (gRover >= fftFrameSize) {
            gRover = inFifoLatency;

            /* do windowing and re,im interleave */
            for (k = 0; k < fftFrameSize; k++) {
                window = -.5 * cos(2. * M_PI * (double)k / (double)fftFrameSize) + .5;
                gFFTworksp[2 * k] = static_cast<float>(gInFIFO[k] * window);
                gFFTworksp[2 * k + 1] = 0.;
            }

            /* ***************** ANALYSIS ******************* */
            /* do transform */
            smbFft(gFFTworksp, fftFrameSize, -1);

            /* this is the analysis step */
            for (k = 0; k <= fftFrameSize2; k++) {

                /* de-interlace FFT buffer */
                real = gFFTworksp[2 * k];
                imag = gFFTworksp[2 * k + 1];

                /* compute magnitude and phase */
                magn = 2. * sqrt(real * real + imag * imag);
                phase = atan2(imag, real);

                /* compute phase difference */
                tmp = phase - gLastPhase[k];
                gLastPhase[k] = static_cast<float>(phase);

                /* subtract expected phase difference */
                tmp -= (double)k * expct;

                /* map delta phase into +/- Pi interval */
                qpd = static_cast<long>(tmp / M_PI);
                if (qpd >= 0)
                    qpd += qpd & 1;
                else
                    qpd -= qpd & 1;
                tmp -= M_PI * (double)qpd;

                /* get deviation from bin frequency from the +/- Pi interval */
                tmp = osamp * tmp / (2. * M_PI);

                /* compute the k-th partials' true frequency */
                tmp = (double)k * freqPerBin + tmp * freqPerBin;

                /* store magnitude and true frequency in analysis arrays */
                gAnaMagn[k] = static_cast<float>(magn);
                gAnaFreq[k] = static_cast<float>(tmp);
            }

            /* ***************** PROCESSING ******************* */
            /* this does the actual pitch shifting */
            memset(gSynMagn, 0, fftFrameSize * sizeof(float));
            memset(gSynFreq, 0, fftFrameSize * sizeof(float));
            for (k = 0; k <= fftFrameSize2; k++) {
                index = static_cast<long>(k * pitchShift);
                if (index <= fftFrameSize2) {
                    gSynMagn[index] += gAnaMagn[k];
                    gSynFreq[index] = gAnaFreq[k] * pitchShift;
                }
            }

            /* ***************** SYNTHESIS ******************* */
            /* this is the synthesis step */
            for (k = 0; k <= fftFrameSize2; k++) {

                /* get magnitude and true frequency from synthesis arrays */
                magn = gSynMagn[k];
                tmp = gSynFreq[k];

                /* subtract bin mid frequency */
                tmp -= (double)k * freqPerBin;

                /* get bin deviation from freq deviation */
                tmp /= freqPerBin;

                /* take osamp into account */
                tmp = 2. * M_PI * tmp / osamp;

                /* add the overlap phase advance back in */
                tmp += (double)k * expct;

                /* accumulate delta phase to get bin phase */
                gSumPhase[k] += static_cast<float>(tmp);
                phase = gSumPhase[k];

                /* get real and imag part and re-interleave */
                gFFTworksp[2 * k] = static_cast<float>(magn) * cosf(static_cast<float>(phase));
                gFFTworksp[2 * k + 1] = static_cast<float>(magn) * sinf(static_cast<float>(phase));
            }

            /* zero negative frequencies */
            for (k = fftFrameSize + 2; k < 2 * fftFrameSize; k++)
                gFFTworksp[k] = 0.;

            /* do inverse transform */
            smbFft(gFFTworksp, fftFrameSize, 1);

            /* do windowing and add to output accumulator */
            for (k = 0; k < fftFrameSize; k++) {
                window = -.5 * cos(2. * M_PI * (double)k / (double)fftFrameSize) + .5;
                gOutputAccum[k] += static_cast<float>(2. * window * gFFTworksp[2 * k] / (fftFrameSize2 * osamp));
            }
            for (k = 0; k < stepSize; k++)
                gOutFIFO[k] = gOutputAccum[k];

            /* shift accumulator */
            memmove(gOutputAccum, gOutputAccum + stepSize, fftFrameSize * sizeof(float));

            /* move input FIFO */
            for (k = 0; k < inFifoLatency; k++)
                gInFIFO[k] = gInFIFO[k + stepSize];
        }
    }
}


void smbPitchShift2(float pitchShift, long numSampsToProcess, long fftFrameSize, long osamp, float sampleRate, float* indata, float* outdata)
{
    static float gInFIFO[MAX_FRAME_LENGTH] = { 0 };
    static float gOutFIFO[MAX_FRAME_LENGTH] = { 0 };
    static float gFFTworksp[2 * MAX_FRAME_LENGTH] = { 0 };
    static float gLastPhase[MAX_FRAME_LENGTH / 2 + 1] = { 0 };
    static float gSumPhase[MAX_FRAME_LENGTH / 2 + 1] = { 0 };
    static float gOutputAccum[2 * MAX_FRAME_LENGTH] = { 0 };
    static float gAnaFreq[MAX_FRAME_LENGTH]= { 0 };
    static float gAnaMagn[MAX_FRAME_LENGTH]= { 0 };
    static float gSynFreq[MAX_FRAME_LENGTH]= { 0 };
    static float gSynMagn[MAX_FRAME_LENGTH]= { 0 };
    static long gRover = false, gInit = false;
    double magn, phase, tmp, window, real, imag;
    double freqPerBin, expct;
    long i, k, qpd, index, inFifoLatency, stepSize, fftFrameSize2;

    /* set up some handy variables */
    fftFrameSize2 = fftFrameSize / 2;
    stepSize = fftFrameSize / osamp;
    freqPerBin = sampleRate / (double)fftFrameSize;
    expct = 2. * M_PI * (double)stepSize / (double)fftFrameSize;
    inFifoLatency = fftFrameSize - stepSize;
    if (gRover == false)
        gRover = inFifoLatency;

    /* initialize our static arrays */
    if (gInit == false) {
        memset(gInFIFO, 0, MAX_FRAME_LENGTH * sizeof(float));
        memset(gOutFIFO, 0, MAX_FRAME_LENGTH * sizeof(float));
        memset(gFFTworksp, 0, 2 * MAX_FRAME_LENGTH * sizeof(float));
        memset(gLastPhase, 0, (MAX_FRAME_LENGTH / 2 + 1) * sizeof(float));
        memset(gSumPhase, 0, (MAX_FRAME_LENGTH / 2 + 1) * sizeof(float));
        memset(gOutputAccum, 0, 2 * MAX_FRAME_LENGTH * sizeof(float));
        memset(gAnaFreq, 0, MAX_FRAME_LENGTH * sizeof(float));
        memset(gAnaMagn, 0, MAX_FRAME_LENGTH * sizeof(float));
        gInit = true;
    }

    /* main processing loop */
    for (i = 0; i < numSampsToProcess; i++) {

        /* As long as we have not yet collected enough data just read in */
        gInFIFO[gRover] = indata[i];
        outdata[i] = gOutFIFO[gRover - inFifoLatency];
        gRover++;

        /* now we have enough data for processing */
        if (gRover >= fftFrameSize) {
            gRover = inFifoLatency;

            /* do windowing and re,im interleave */
            for (k = 0; k < fftFrameSize; k++) {
                window = -.5 * cos(2. * M_PI * (double)k / (double)fftFrameSize) + .5;
                gFFTworksp[2 * k] = static_cast<float>(gInFIFO[k] * window);
                gFFTworksp[2 * k + 1] = 0.;
            }

            /* ***************** ANALYSIS ******************* */
            /* do transform */
            smbFft(gFFTworksp, fftFrameSize, -1);

            /* this is the analysis step */
            for (k = 0; k <= fftFrameSize2; k++) {

                /* de-interlace FFT buffer */
                real = gFFTworksp[2 * k];
                imag = gFFTworksp[2 * k + 1];

                /* compute magnitude and phase */
                magn = 2. * sqrt(real * real + imag * imag);
                phase = atan2(imag, real);

                /* compute phase difference */
                tmp = phase - gLastPhase[k];
                gLastPhase[k] = static_cast<float>(phase);

                /* subtract expected phase difference */
                tmp -= (double)k * expct;

                /* map delta phase into +/- Pi interval */
                qpd = static_cast<long>(tmp / M_PI);
                if (qpd >= 0)
                    qpd += qpd & 1;
                else
                    qpd -= qpd & 1;
                tmp -= M_PI * (double)qpd;

                /* get deviation from bin frequency from the +/- Pi interval */
                tmp = osamp * tmp / (2. * M_PI);

                /* compute the k-th partials' true frequency */
                tmp = (double)k * freqPerBin + tmp * freqPerBin;

                /* store magnitude and true frequency in analysis arrays */
                gAnaMagn[k] = static_cast<float> (magn);
                gAnaFreq[k] = static_cast<float> (tmp);
            }

            /* ***************** PROCESSING ******************* */
            /* this does the actual pitch shifting */
            memset(gSynMagn, 0, fftFrameSize * sizeof(float));
            memset(gSynFreq, 0, fftFrameSize * sizeof(float));
            for (k = 0; k <= fftFrameSize2; k++) {
                index = static_cast<long> (k * pitchShift);
                if (index <= fftFrameSize2) {
                    gSynMagn[index] += gAnaMagn[k];
                    gSynFreq[index] = gAnaFreq[k] * pitchShift;
                }
            }

            /* ***************** SYNTHESIS ******************* */
            /* this is the synthesis step */
            for (k = 0; k <= fftFrameSize2; k++) {

                /* get magnitude and true frequency from synthesis arrays */
                magn = gSynMagn[k];
                tmp = gSynFreq[k];

                /* subtract bin mid frequency */
                tmp -= (double)k * freqPerBin;

                /* get bin deviation from freq deviation */
                tmp /= freqPerBin;

                /* take osamp into account */
                tmp = 2. * M_PI * tmp / osamp;

                /* add the overlap phase advance back in */
                tmp += (double)k * expct;

                /* accumulate delta phase to get bin phase */
                gSumPhase[k] += static_cast<float>(tmp);
                phase = gSumPhase[k];

                /* get real and imag part and re-interleave */
                gFFTworksp[2 * k] = static_cast<float> (magn) * cosf(static_cast<float>(phase));
                gFFTworksp[2 * k + 1] = static_cast<float> (magn) * sinf(static_cast<float>(phase));
            }

            /* zero negative frequencies */
            for (k = fftFrameSize + 2; k < 2 * fftFrameSize; k++)
                gFFTworksp[k] = 0.;

            /* do inverse transform */
            smbFft(gFFTworksp, fftFrameSize, 1);

            /* do windowing and add to output accumulator */
            for (k = 0; k < fftFrameSize; k++) {
                window = -.5 * cos(2. * M_PI * (double)k / (double)fftFrameSize) + .5;
                gOutputAccum[k] +=  static_cast<float>(2. * window * gFFTworksp[2 * k] / (fftFrameSize2 * osamp));
            }
            for (k = 0; k < stepSize; k++)
                gOutFIFO[k] = gOutputAccum[k];

            /* shift accumulator */
            memmove(gOutputAccum, gOutputAccum + stepSize, fftFrameSize * sizeof(float));

            /* move input FIFO */
            for (k = 0; k < inFifoLatency; k++)
                gInFIFO[k] = gInFIFO[k + stepSize];
        }
    }
}

void ChangePitch(char* buffer_in, char* buffer_out, long data_len, float pitch_change, float sample_rate)
{
    // assume it's 16-bit
    long num_samples = data_len / 2 / 2;
    static std::vector<float> float_inL;
    static std::vector<float> float_outL;
    static std::vector<float> float_inR;
    static std::vector<float> float_outR;
    float_inL.resize(num_samples);
    float_outL.resize(num_samples);
    float_inR.resize(num_samples);
    float_outR.resize(num_samples);
    short* pcm_src = (short*)buffer_in;
    for (long i = 0; i < num_samples; i++) {
        float_inL[i] = (float)pcm_src[i*2] / 32768.0f;
        float_inR[i] = (float)pcm_src[i*2+1] / 32768.0f;
    }
    smbPitchShift(pitch_change, num_samples, 2048, 4, sample_rate, float_inL.data(), float_outL.data());
    smbPitchShift2(pitch_change, num_samples, 2048, 4, sample_rate, float_inR.data(), float_outR.data());
    short* pcm_dst = (short*)buffer_out;
    for (long i = 0; i < num_samples; i++) {
        float valL = float_outL[i] * 32768.0f;
        valL = std::clamp(valL, -32768.0f, 32767.0f);

        float valR = float_outR[i] * 32768.0f;
        valR = std::clamp(valR, -32768.0f, 32767.0f);
        pcm_dst[i*2] = (short)valL;
        pcm_dst[i*2+1] = (short)valR;
    }
}


HRESULT STDMETHODCALLTYPE Unlock_Changed(LPDIRECTSOUNDBUFFER thiz, LPVOID pvAudioPtr1, DWORD dwAudioBytes1, LPVOID pvAudioPtr2, DWORD dwAudioBytes2)
{
    WAVEFORMATEX wavf;
    thiz->GetFormat(&wavf, sizeof(wavf), nullptr);
    for (auto i = g_sound_opt.soundbuffers.begin(); i != g_sound_opt.soundbuffers.end(); i++) {
        if (i->p_sb == thiz) {
            if (g_sound_opt.no_change_pitch){
                if (g_sound_opt.pitch_ofs / g_sound_opt.bgm_speed != 1.0f) {
                    if (pvAudioPtr1)
                        ChangePitch((char*)pvAudioPtr1, (char*)pvAudioPtr1, dwAudioBytes1, g_sound_opt.pitch_ofs / g_sound_opt.bgm_speed, static_cast<float>(wavf.nSamplesPerSec));
                    if (pvAudioPtr2)
                        ChangePitch((char*)pvAudioPtr2, (char*)pvAudioPtr2, dwAudioBytes2, g_sound_opt.pitch_ofs / g_sound_opt.bgm_speed, static_cast<float>(wavf.nSamplesPerSec));
                }
            }
            HRESULT(STDMETHODCALLTYPE * realUnlock)
            (LPDIRECTSOUNDBUFFER thiz, LPVOID pvAudioPtr1, DWORD dwAudioBytes1, LPVOID pvAudioPtr2, DWORD dwAudioBytes2);
            realUnlock = (decltype(realUnlock))(i->vtbl_orig[19]);
            HRESULT res = realUnlock(thiz, pvAudioPtr1, dwAudioBytes1, pvAudioPtr2, dwAudioBytes2);
            return res;
        }
    }
    return -1;
}



HRESULT STDMETHODCALLTYPE DuplicateSoundBuffer_Changed(IDirectSound8* thiz, LPDIRECTSOUNDBUFFER pDSBufferOriginal, LPDIRECTSOUNDBUFFER* ppDSBufferDuplicate)
{
    auto res = g_sound_opt.g_realDuplicateSoundBuffer(thiz, pDSBufferOriginal, ppDSBufferDuplicate);
    if (SUCCEEDED(res)) {
        int idx = -1;
        for (int i = 0; i < std::ssize(g_sound_opt.soundbuffers); i++) {
            if (g_sound_opt.soundbuffers[i].p_sb == pDSBufferOriginal) {
                idx = i;
                break;
            }
        }
        if (idx != -1) {
            SoundOpt::SB_Struct sb;
            memset(&sb, 0, sizeof(sb));
            sb.p_sb = *ppDSBufferDuplicate;
            sb.vtbl_orig = new DWORD[21];
            sb.vtbl_changed = new DWORD[21];
            DWORD* address = (DWORD*)(*(DWORD*)*ppDSBufferDuplicate);
            for (int i = 0; i < 21; i++) {
                sb.vtbl_orig[i] = (DWORD)address[i];
                sb.vtbl_changed[i] = (DWORD)address[i];
            }
            sb.is_bgm = g_sound_opt.soundbuffers[idx].is_bgm;
            sb.base_volume = g_sound_opt.soundbuffers[idx].base_volume;
            sb.vtbl_changed[2] = (DWORD)SoundBuffer_Release_Changed;
            sb.vtbl_changed[6] = (DWORD)GetVolume_Changed;
            sb.vtbl_changed[15] = (DWORD)SetVolume_Changed;
            if (sb.is_bgm && g_sound_opt.enable_fasterBGM) {
                sb.vtbl_changed[14] = (DWORD)SetFormat_Changed;
                sb.vtbl_changed[10] = (DWORD)SoundBuffer_Initialize_Changed;
                sb.vtbl_changed[12] = (DWORD)Play_Changed;
                sb.vtbl_changed[19] = (DWORD)Unlock_Changed;
            }
            (*(DWORD*)*ppDSBufferDuplicate) = (DWORD)sb.vtbl_changed;
            g_sound_opt.soundbuffers.push_back(sb);
            auto realSetVolume = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(LPDIRECTSOUNDBUFFER, LONG)>(sb.vtbl_orig[15]);
            realSetVolume(sb.p_sb, AdjustedVolume(sb.base_volume, sb.is_bgm));
        }
    }
    return res;
}

HRESULT STDMETHODCALLTYPE CreateSoundBuffer_Changed(IDirectSound8* thiz, LPCDSBUFFERDESC pcDSBufferDesc, LPDIRECTSOUNDBUFFER* ppDSBuffer, LPUNKNOWN pUnkOuter)
{
    HRESULT res;
    DSBUFFERDESC desc = *pcDSBufferDesc;
    if (g_sound_opt.enable_fasterBGM && (desc.dwFlags & DSBCAPS_CTRLPOSITIONNOTIFY)) {
        desc.dwFlags |= DSBCAPS_CTRLFREQUENCY;
    }
    const bool volume = AddonEnabled(Addon::Volume);
    const bool isBgm = (desc.dwFlags & DSBCAPS_CTRLPOSITIONNOTIFY) != 0;
    const bool isPrimary = (desc.dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
    if (volume && !isPrimary)
        desc.dwFlags |= DSBCAPS_CTRLVOLUME;

    SoundOpt::SB_Struct sb;
    memset(&sb, 0, sizeof(sb));
    if (desc.lpwfxFormat)
        sb.orig_format = *desc.lpwfxFormat;
    res = g_sound_opt.g_realCreateSoundBuffer(thiz, &desc, ppDSBuffer, pUnkOuter);
    if (SUCCEEDED(res)) {
        sb.p_sb = *ppDSBuffer;
        if (volume ? !isPrimary : isBgm) {
            sb.is_bgm = isBgm;
            sb.vtbl_orig = new DWORD[21];
            sb.vtbl_changed = new DWORD[21];
            DWORD* address = (DWORD*)(*(DWORD*)*ppDSBuffer);
            for (int i = 0; i < 21; i++) {
                sb.vtbl_orig[i] = (DWORD)address[i];
                sb.vtbl_changed[i] = (DWORD)address[i];
            }
            sb.vtbl_changed[2] = (DWORD)SoundBuffer_Release_Changed;
            if (volume) {
                sb.vtbl_changed[6] = (DWORD)GetVolume_Changed;
                sb.vtbl_changed[15] = (DWORD)SetVolume_Changed;
            }
            if (sb.is_bgm && g_sound_opt.enable_fasterBGM) {
                sb.vtbl_changed[14] = (DWORD)SetFormat_Changed;
                sb.vtbl_changed[10] = (DWORD)SoundBuffer_Initialize_Changed;
                sb.vtbl_changed[12] = (DWORD)Play_Changed;
                sb.vtbl_changed[19] = (DWORD)Unlock_Changed;
            }
            (*(DWORD*)*ppDSBuffer) = (DWORD)sb.vtbl_changed;
            g_sound_opt.soundbuffers.push_back(sb);
            if (volume) {
                auto realSetVolume = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(LPDIRECTSOUNDBUFFER, LONG)>(sb.vtbl_orig[15]);
                realSetVolume(sb.p_sb, AdjustedVolume(sb.base_volume, sb.is_bgm));
            }
        }
    }
    return res;
}
static void AttachEarlyGameSound()
{
    // EoSD/PCB have no native percentage fields. Adopt a BGM that predates
    // injection so the fallback mixer can still affect the currently playing WAV.
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t soundPlayer = base + (g_gameVolumeVersion == 6 ? 0x2d3f50 : 0xba0d8);
    const uintptr_t bgmSlot = base + (g_gameVolumeVersion == 6 ? 0x2d457c : 0xbda94);
    if (auto device = ReadPointer(soundPlayer))
        HookSoundDevice(reinterpret_cast<void*>(device));
    // A MIDI device may likewise have opened before injection. Its modified CC7
    // cache includes the native fade, so adopting it preserves ongoing fades.
    const auto midi = ReadPointer(base + (g_gameVolumeVersion == 6 ? 0x2c6ec8 : 0x175acc));
    const auto handle = reinterpret_cast<HMIDIOUT>(ReadPointer(midi ? midi + 0x13c : 0));
    bool adoptedMidi = false;
    if (handle && g_realMidiOutShortMsg && ReadableMemory(midi + 0x16a, 15 * 0x17 + 1)) {
        AcquireSRWLockExclusive(&g_midiVolumeLock);
        const auto existing = std::find_if(g_midiVolumeDevices.begin(), g_midiVolumeDevices.end(),
            [handle](const auto& device) { return device.handle == handle; });
        if (existing == g_midiVolumeDevices.end()) {
            MidiVolumeDevice device { handle, {} };
            for (unsigned channel = 0; channel < 16; ++channel)
                device.channels[channel] = std::min<BYTE>(*reinterpret_cast<BYTE*>(midi + 0x16a + channel * 0x17), 127);
            g_midiVolumeDevices.push_back(device);
            adoptedMidi = true;
        }
        ReleaseSRWLockExclusive(&g_midiVolumeLock);
    }
    if (adoptedMidi)
        ApplyMidiVolume();
    const auto manager = ReadPointer(bgmSlot);
    const auto array = ReadPointer(manager ? manager + 4 : 0);
    const auto pointer = ReadPointer(array);
    if (!pointer)
        return;
    auto* buffer = reinterpret_cast<LPDIRECTSOUNDBUFFER>(pointer);
    for (const auto& tracked : g_sound_opt.soundbuffers) {
        if (tracked.p_sb == buffer)
            return;
    }
    SoundOpt::SB_Struct sb {};
    sb.p_sb = buffer;
    sb.is_bgm = true;
    if (FAILED(buffer->GetVolume(&sb.base_volume)))
        return;
    buffer->GetFormat(&sb.orig_format, sizeof(sb.orig_format), nullptr);
    sb.vtbl_orig = new DWORD[21];
    sb.vtbl_changed = new DWORD[21];
    const auto* original = *reinterpret_cast<DWORD**>(buffer);
    std::copy_n(original, 21, sb.vtbl_orig);
    std::copy_n(original, 21, sb.vtbl_changed);
    sb.vtbl_changed[2] = reinterpret_cast<DWORD>(SoundBuffer_Release_Changed);
    sb.vtbl_changed[6] = reinterpret_cast<DWORD>(GetVolume_Changed);
    sb.vtbl_changed[15] = reinterpret_cast<DWORD>(SetVolume_Changed);
    *reinterpret_cast<DWORD**>(buffer) = sb.vtbl_changed;
    g_sound_opt.soundbuffers.push_back(sb);
    const auto setVolume = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(LPDIRECTSOUNDBUFFER, LONG)>(sb.vtbl_orig[15]);
    setVolume(buffer, AdjustedVolume(sb.base_volume, true));
}

void HookBGMSpeed(bool is_hook, bool no_change_pitch)
{
    g_sound_opt.enable_fasterBGM = is_hook;
    g_sound_opt.no_change_pitch = no_change_pitch;
    const bool volume = AddonEnabled(Addon::Volume);
    if (volume) {
        HookGameMidiVolume();
        HookGameDirectSoundCreate();
    } else if (!is_hook) {
        return;
    }
    // Also hook the shared device vtable: an attached game may have created its
    // device before our IAT hooks, and will reuse it for subsequent buffers.
    IDirectSound8* pdirectsound;
    HRESULT hr = DirectSoundCreate8(NULL, &pdirectsound, NULL);
    if (SUCCEEDED(hr)) {
        HookVTable(pdirectsound, 3, CreateSoundBuffer_Changed, (void**)&g_sound_opt.g_realCreateSoundBuffer);
        if (volume) HookVTable(pdirectsound, 5, DuplicateSoundBuffer_Changed, (void**)&g_sound_opt.g_realDuplicateSoundBuffer);
        pdirectsound->Release();
    }
}


void ChangeBGMSpeed(float speed,float pitch_ofs)
{
    if (g_sound_opt.enable_fasterBGM) {
        float orig_speed = g_sound_opt.bgm_speed;
        float orig_pitch = g_sound_opt.pitch_ofs;

        float new_speed = speed == -1.0f ? g_sound_opt.bgm_speed : speed;
        float new_pitch = pitch_ofs == -1.0f ? g_sound_opt.pitch_ofs : pitch_ofs;

        for (auto i = g_sound_opt.soundbuffers.begin(); i != g_sound_opt.soundbuffers.end(); i++) {
            if (i->orig_format.nChannels != 0) {
                WAVEFORMATEX fmt = i->orig_format;
                DWORD status;
                i->p_sb->GetStatus(&status);
                if (status & DSBSTATUS_PLAYING) {
                    LPVOID p1, p2;
                    DWORD n1, n2;
                    i->p_sb->Stop();
                    i->p_sb->Lock(0, 0, &p1, &n1, &p2, &n2, DSBLOCK_ENTIREBUFFER);

                    g_sound_opt.bgm_speed = new_speed / orig_speed;
                    g_sound_opt.pitch_ofs = new_pitch / orig_pitch;

                    i->p_sb->Unlock(p1, n1, p2, n2);

                    g_sound_opt.bgm_speed = new_speed;
                    g_sound_opt.pitch_ofs = new_pitch;
                    i->p_sb->SetFrequency(static_cast<DWORD>(fmt.nSamplesPerSec * g_sound_opt.bgm_speed));
                    i->p_sb->Play(0, 0, DSBPLAY_LOOPING);
                }
            }
        }
    }
}
bool BGMPitchChanger()
{
    static float pitch_2powed = 0.0f;
    static float speed = 1.0f;
    float pitch_cur = g_sound_opt.pitch_ofs;
    float speed_cur = g_sound_opt.bgm_speed;

    if (g_sound_opt.enable_fasterBGM && g_sound_opt.no_change_pitch){
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat("BGM Pitch", &pitch_2powed, 0.01f, -2.0f, 2.0f);
        pitch_2powed = std::clamp(pitch_2powed, -2.0f, 2.0f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::DragFloat("BGM SPD", &speed, 0.01f, 0.25f, 5.0f);
        speed = std::clamp(speed, 0.25f, 5.0f);

        float pitch_new = powf(2.0f, pitch_2powed);
        float pitch_2powed_old = log2f(pitch_cur);
        if (fabsf(pitch_2powed_old - pitch_2powed) > 0.0001f || fabsf(speed_cur - speed)>0.001f)
        {
            ImGui::SameLine();
            if (ImGui::Button("OK")){
                ChangeBGMSpeed(speed, pitch_new);
                return true;
            }
        }

    }
    return false;
}

#pragma endregion

#pragma region Font
struct FontOpt {
    bool g_useCustomFont = false;
    std::string g_customFont = "MS Gothic";
    HFONT(WINAPI* g_realCreateFontA)
    (int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName);
    HFONT(WINAPI* g_realCreateFontW)
    (int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic, DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName);
} g_font_opt;


HFONT WINAPI CreateFontA_Changed(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic, DWORD bUnderline,
    DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCSTR pszFaceName)
{
    static std::vector<std::string> fonts = EnumAllFonts();
    unsigned char font_Gothic[] = { 0x82, 0x6C, 0x82, 0x72, 0x20, 0x83, 0x53, 0x83, 0x56, 0x83, 0x62, 0x83, 0x4E, 0x00 }; // 俵俽 僑僔僢僋, MS Gothic
    unsigned char font_Mincho[] = { 0x82, 0x6C, 0x82, 0x72, 0x20, 0x96, 0xBE, 0x92, 0xA9, 0x00 }; // 俵俽 柧挬, MS Mincho
    if (g_font_opt.g_useCustomFont)
        return g_font_opt.g_realCreateFontA(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, g_font_opt.g_customFont.c_str());
    if (strcmp((char*)font_Gothic, pszFaceName) == 0) {
        return g_font_opt.g_realCreateFontA(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, "MS Gothic");
    } else if (strcmp((char*)font_Mincho, pszFaceName) == 0) {
        // some computer might not have mincho font
        if (std::find(fonts.begin(), fonts.end(), "MS Mincho") == fonts.end())
            return g_font_opt.g_realCreateFontA(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, "MS Gothic");
        return g_font_opt.g_realCreateFontA(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, "MS Mincho");
    }
    return g_font_opt.g_realCreateFontA(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, pszFaceName);
}

HFONT WINAPI CreateFontW_Changed(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic, DWORD bUnderline,
    DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision, DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName)
{
    static std::vector<std::string> fonts = EnumAllFonts();
    wchar_t font_YuGothic[] = { 0x6E38, 0x30B4, 0x30B7, 0x30C3, 0x30AF, 0 }; // Yu Gothic...
    wchar_t font_YuMincho[] = L"Yu Mincho";
    wchar_t font_YuMeiryo[] = { 0x30E1, 0x30A4, 0x30EA, 0x30AA, 0 }; // Meiryo
    wchar_t font_MsGothic[] = { 0xFF2D, 0xFF33, 0x0020, 0x30B4, 0x30B7, 0x30C3, 0x30AF, 0x0 }; // MS Gothic
    if (g_font_opt.g_useCustomFont) {
        // fall back to CreateFontA
        return CreateFontA(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, g_font_opt.g_customFont.c_str());
    }

    // Yu Gothic
    if (wcscmp(font_YuGothic, pszFaceName) == 0) {
        if (std::find(fonts.begin(), fonts.end(), "Yu Gothic") != fonts.end())
            return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, L"Yu Gothic");
        if (std::find(fonts.begin(), fonts.end(), "MS Gothic") != fonts.end())
            return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, L"MS Gothic");
        if (std::find(fonts.begin(), fonts.end(), "MS Mincho") != fonts.end())
            return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, L"MS Mincho");
    }
    // Yu Mincho
    if (wcscmp(font_YuMincho, pszFaceName) == 0) {
        if (std::find(fonts.begin(), fonts.end(), "Yu Mincho") != fonts.end())
            return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, L"Yu Mincho");
        if (std::find(fonts.begin(), fonts.end(), "MS Mincho") != fonts.end())
            return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, L"MS Mincho");
    }
    // Meiryo
    if (wcscmp(font_YuMeiryo, pszFaceName) == 0) {
        if (std::find(fonts.begin(), fonts.end(), "Meiryo") != fonts.end())
            return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, L"Meiryo");
    }
    // MS Gothic
    if (wcscmp(font_MsGothic, pszFaceName) == 0) {
        if (std::find(fonts.begin(), fonts.end(), "MS Gothic") != fonts.end())
            return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, L"MS Gothic");
        if (std::find(fonts.begin(), fonts.end(), "MS Mincho") != fonts.end())
            return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, L"MS Mincho");
    }
    return g_font_opt.g_realCreateFontW(cHeight, cWidth, cEscapement, cOrientation, cWeight, bItalic, bUnderline, bStrikeOut, iCharSet, iOutPrecision, iClipPrecision, iQuality, iPitchAndFamily, pszFaceName);
}

void HookCreateFont(bool useCorrectJaFont, bool useCustomFont, int fontidx)
{
    g_font_opt.g_useCustomFont = useCustomFont;
    if (useCustomFont || useCorrectJaFont) {
        HookIAT(GetModuleHandle(NULL), "GDI32.dll", "CreateFontA", CreateFontA_Changed, (void**)&g_font_opt.g_realCreateFontA);
        HookIAT(GetModuleHandle(NULL), "GDI32.dll", "CreateFontW", CreateFontW_Changed, (void**)&g_font_opt.g_realCreateFontW);
    }
    if (useCustomFont) {
        auto& all_fonts = EnumAllFonts();
        if (fontidx >= 0 && fontidx < std::ssize(all_fonts))
            g_font_opt.g_customFont = all_fonts[fontidx];
    }
}

#pragma endregion

#pragma region InputLatencyTest

InputLatencyTestOpt g_input_latency_test;

void InputLatencyTestOpt::Init()
{
    clockid = SetUpClock();
}
void InputLatencyTestOpt::Input()
{
    if (test_input_latency) {
        ResetClock(clockid);
    }
}
void InputLatencyTestOpt::Present()
{
    if (test_input_latency) {
        last_input_latency = ResetClock(clockid);
    }
}
double InputLatencyTestOpt::GetLatency()
{
    return last_input_latency;
}

void EnableInputLatencyTest(bool is_enable)
{
    g_input_latency_test.test_input_latency = is_enable;
    if (is_enable)
        g_input_latency_test.Init();
}

bool GetLatency(double* latency)
{
    if (latency && g_input_latency_test.test_input_latency)
    {
        *latency = g_input_latency_test.GetLatency();
        return true;
    }
    return false;
 }

#pragma endregion

#pragma region Input_Joy
extern FastRetryOpt g_fast_re_opt;
extern InputOpt g_input_opt;


template <typename T>
struct Ranges {
    T x_min, x_max;
    T y_min, y_max;
};
#define DEG_TO_RAD(x) ((x) * 0.0174532925199432957692369076848f)

template <typename T>
bool pov_to_xy(T& x, T& y, Ranges<T>& ranges, DWORD pov)
{
    // According to MSDN, some DirectInput drivers report the centered
    // position of the POV indicator as 65,535. This matches both that
    // behavior and JOY_POVCENTERED for WinMM.
    if (LOWORD(pov) == 0xFFFF) {
        return false;
    }
    T x_center = (ranges.x_max - ranges.x_min) / 2;
    T y_center = (ranges.y_max - ranges.y_min) / 2;

    float angle_deg = pov / 100.0f;
    float angle_rad = DEG_TO_RAD(angle_deg);
    // POV values ≠ unit circle angles, so...
    float angle_sin = 1.0f - cosf(angle_rad);
    float angle_cos = sinf(angle_rad) + 1.0f;
    x = (T)(ranges.x_min + (angle_cos * x_center));
    y = (T)(ranges.y_min + (angle_sin * y_center));
    return true;
}

struct winmm_joy_caps_t {
    // joyGetPosEx() will return bogus values on joysticks without a POV, so
    // we must check if we even have one.
    bool initialized = false;
    bool has_pov;
    Ranges<DWORD> range;
};
std::vector<winmm_joy_caps_t> joy_info;



MMRESULT WINAPI joyGetDevCapsA_Changed(UINT uJoyID, LPJOYCAPSA pjc, UINT cbjc)
{
    if (g_input_opt.g_disable_joy)
        return MMSYSERR_NODRIVER;
    return g_input_opt.g_realJoyGetDevCapsA(uJoyID, pjc, cbjc);
}
MMRESULT WINAPI joyGetPosEx_Changed(UINT uJoyID, LPJOYINFOEX pji)
{
    if (g_input_opt.g_disable_joy)
        return MMSYSERR_NODRIVER;

    pji->dwFlags |= JOY_RETURNPOV;
    auto ret_pos = g_input_opt.g_realJoyGetPosEx(uJoyID, pji);
    if (ret_pos != JOYERR_NOERROR) {
        return ret_pos;
    }

    if (uJoyID >= joy_info.size()) {
        joy_info.resize(uJoyID + 1);
    }

    auto& jc = joy_info[uJoyID];

    if (!jc.initialized) {
        JOYCAPSW caps;
        auto ret_caps = joyGetDevCapsW(uJoyID, &caps, sizeof(caps));
        (void)ret_caps; // suppress "unused variable" warning
        assert(ret_caps == JOYERR_NOERROR);

        jc.initialized = true;
        jc.has_pov = (caps.wCaps & JOYCAPS_HASPOV) != 0;
        jc.range.x_min = caps.wXmin;
        jc.range.x_max = caps.wXmax;
        jc.range.y_min = caps.wYmin;
        jc.range.y_max = caps.wYmax;
    } else if (!jc.has_pov) {
        return ret_pos;
    }
    pov_to_xy(pji->dwXpos, pji->dwYpos, jc.range, pji->dwPOV);
    return ret_pos;
}

DWORD WINAPI XInputGetState_Changed3(DWORD dwUserIndex, void* pState)
{
    if (g_input_opt.g_disable_joy)
        return ERROR_DEVICE_NOT_CONNECTED;
    return g_input_opt.g_realXInputGetState3(dwUserIndex, pState);
}

DWORD WINAPI XInputGetState_Changed4(DWORD dwUserIndex, void* pState)
{
    if (g_input_opt.g_disable_joy)
        return ERROR_DEVICE_NOT_CONNECTED;
    return g_input_opt.g_realXInputGetState4(dwUserIndex, pState);
}
void HookJoyInput()
{
    HookIAT(GetModuleHandle(NULL), "winmm.dll", "joyGetPosEx", joyGetPosEx_Changed, (void**)&g_input_opt.g_realJoyGetPosEx);
    HookIAT(GetModuleHandle(NULL), "winmm.dll", "joyGetDevCapsA", joyGetDevCapsA_Changed, (void**)&g_input_opt.g_realJoyGetDevCapsA);
    HookIAT(GetModuleHandle(NULL), "xinput1_3.dll", "XInputGetState", XInputGetState_Changed3, (void**)&g_input_opt.g_realXInputGetState3); // th18
    HookIAT(GetModuleHandle(NULL), "xinput1_4.dll", "XInputGetState", XInputGetState_Changed4, (void**)&g_input_opt.g_realXInputGetState4); // th19+
}

#pragma endregion

#define IS_KEY_DOWN(x) (((x) & 0x80) == 0x80)
enum Key {
    K_LEFT,
    K_RIGHT,
    K_UP,
    K_DOWN
};

void ClearInputData(bool inactivate)
{
    memset(g_input_opt.fake_di_State, 0, 256);
    if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_dinput8KeyAPI && g_input_opt.ddevice) {
        while (true) {
            DWORD dwItems = 32;
            DIDEVICEOBJECTDATA rgdod[32];
            auto res = g_input_opt.ddevice->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), rgdod, &dwItems, 0);
            if (FAILED(res) || dwItems == 0) {
                break;
            }
        }
        if (inactivate)
            g_input_opt.ddevice->Unacquire();
        else
            g_input_opt.ddevice->Acquire();
    }
}

#pragma region rawInput
void GetRawInput(DWORD hraw)
{
    if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_RawInput) {
        static BYTE lpb[1024];
        UINT dwSize = 1024;
        if (GetRawInputData((HRAWINPUT)hraw, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) != -1) {
            RAWINPUT* raw = (RAWINPUT*)lpb;
            if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                UINT scanCode = raw->data.keyboard.MakeCode;
                bool isUp = (raw->data.keyboard.Flags & RI_KEY_BREAK);

                if (raw->data.keyboard.Flags & RI_KEY_E0) {
                    scanCode |= 0x80;
                }
                if (scanCode < 256) {
                    g_input_opt.fake_di_State[scanCode] = isUp ? 0x00 : 0x80;
                }
            }
        }
    }
}

void ProcessRawInput(BYTE state[256])
{
    static char alignas(8) buffer[2048];
    UINT bufferSize = sizeof(buffer);
    UINT count = GetRawInputBuffer((PRAWINPUT)buffer, &bufferSize, sizeof(RAWINPUTHEADER));
    if (count == 0 || count == (UINT)-1) {
        memcpy(state, g_input_opt.fake_di_State, 256);
        return;
    }
    BYTE kb_is_down[256] = { 0 };
    BYTE kb_is_down_last[256] = { 0 };
    while (count > 0) {
        RAWINPUT* pRaw = (RAWINPUT*)buffer;
        memcpy(kb_is_down_last, g_input_opt.fake_di_State, 256);
        for (int i = 0; i < (int)count; ++i) {
            if (pRaw->header.dwType == RIM_TYPEKEYBOARD) {
                RAWKEYBOARD* pKb = &pRaw->data.keyboard;
                if (pKb->MakeCode == 0 && pKb->Flags == 0 && pRaw->header.dwSize > 32) {
                    pKb = (RAWKEYBOARD*)((LPBYTE)pRaw + 24); // WoW64 fix
                }
                UINT scanCode = pKb->MakeCode;
                bool isUp = (pKb->Flags & RI_KEY_BREAK);
                if (pKb->Flags & RI_KEY_E0) {
                    scanCode |= 0x80;
                }
                if (scanCode < 256) {
                    BYTE curstate = isUp ? 0x00 : 0x80;
                    g_input_opt.fake_di_State[scanCode] = curstate;
                    if (kb_is_down_last[scanCode] == 0x0 && !isUp)
                        kb_is_down[scanCode] = 0x80;
                }
            }
            pRaw = NEXTRAWINPUTBLOCK((PRAWINPUT)pRaw);
        }
        bufferSize = sizeof(buffer);
        count = GetRawInputBuffer((PRAWINPUT)buffer, &bufferSize, sizeof(RAWINPUTHEADER));
        if (count == 0 || count == (UINT)-1) {
            break;
        }
    }
    for (int i = 0; i < 256; i++) {
        state[i] = kb_is_down[i] | g_input_opt.fake_di_State[i];
    }
}
#pragma endregion

#pragma region dinput
HRESULT STDMETHODCALLTYPE GetDeviceState_Changed(LPDIRECTINPUTDEVICE8 thiz, DWORD num, LPVOID state);
HRESULT STDMETHODCALLTYPE SetProperty_Changed(LPDIRECTINPUTDEVICE8 thiz, REFGUID guid, LPCDIPROPHEADER header);
HRESULT STDMETHODCALLTYPE SetCooperativeLevel_Changed(LPDIRECTINPUTDEVICE8 thiz, HWND hwnd, DWORD flag)
{
    if (g_input_opt.ddevice == thiz) {
        if (g_input_opt.disable_win_key) {
            flag = DISCL_NONEXCLUSIVE | DISCL_FOREGROUND | DISCL_NOWINKEY;
        } else {
            flag = DISCL_NONEXCLUSIVE | DISCL_FOREGROUND;
        }
        auto res = g_input_opt.realSetCooperativeLevel(thiz, hwnd, flag);
        bool re_acquire = false;
        if (res == DIERR_ACQUIRED) {
            thiz->Unacquire();
            re_acquire = true;
            res = g_input_opt.realSetCooperativeLevel(thiz, hwnd, flag);
        }
        if (g_input_opt.use_get_device_data) {
            DIPROPDWORD dipdw;
            dipdw.diph.dwSize = sizeof(DIPROPDWORD);
            dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            dipdw.diph.dwObj = 0;
            dipdw.diph.dwHow = DIPH_DEVICE;
            dipdw.dwData = 64;
            SetProperty_Changed(thiz, DIPROP_BUFFERSIZE, &dipdw.diph);
        }
        if (re_acquire) {
            thiz->Acquire();
        }
        return res;
    }
    return g_input_opt.realSetCooperativeLevel(thiz, hwnd, flag);
}

HRESULT WINAPI EnumDevices_Changed(LPDIRECTINPUT8 thiz, DWORD dwDevType, LPDIENUMDEVICESCALLBACKW lpCallback, LPVOID pvRef, DWORD dwFlags)
{
    if (g_input_opt.g_disable_joy && dwDevType == DI8DEVCLASS_GAMECTRL)
        return DI_OK;
    return g_input_opt.realEnumDevices(thiz, dwDevType, lpCallback, pvRef, dwFlags);
}
HRESULT WINAPI DDevice_Release_Changed(LPDIRECTINPUTDEVICE8 thiz)
{
    if (g_input_opt.ddevice == thiz) {
        return DI_OK;
    }
    return g_input_opt.realDDRelease(thiz);
}
HRESULT WINAPI Dinput_Release_Changed(LPDIRECTINPUT8 thiz)
{
    if (g_input_opt.dinput == thiz) {
        return DI_OK;
    }
    return g_input_opt.realDIRelease(thiz);
}

HRESULT STDMETHODCALLTYPE SetDataFormat_Changed(LPDIRECTINPUTDEVICE8 thiz, LPCDIDATAFORMAT fmt)
{
    if (g_input_opt.ddevice == thiz) {
        auto res = g_input_opt.realSetDataFormat(thiz, fmt);
        if (res == DIERR_ACQUIRED) {
            thiz->Unacquire();
            res = g_input_opt.realSetDataFormat(thiz, fmt);
        }
        return res;
    }
    return g_input_opt.realSetDataFormat(thiz, fmt);
}
HRESULT WINAPI Poll_Changed(LPDIRECTINPUTDEVICE8 thiz)
{
    if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_RawInput) {
        return DI_OK;
    }
    auto res = g_input_opt.realPoll(thiz);
    return res;
}
HRESULT WINAPI Acquire_Changed(LPDIRECTINPUTDEVICE8 thiz)
{
    if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_RawInput) {
        return DI_OK;
    }
    auto res = g_input_opt.realAcquire(thiz);
    return res;
}
HRESULT STDMETHODCALLTYPE SetProperty_Changed(LPDIRECTINPUTDEVICE8 thiz, REFGUID guid, LPCDIPROPHEADER header)
{
    if (g_input_opt.ddevice == thiz && g_input_opt.use_get_device_data) {
        DIPROPDWORD dipdw;
        dipdw.diph.dwSize = sizeof(DIPROPDWORD);
        dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        dipdw.diph.dwObj = 0;
        dipdw.diph.dwHow = DIPH_DEVICE;
        dipdw.dwData = 64;
        auto res = g_input_opt.realSetProperty(thiz, DIPROP_BUFFERSIZE, &dipdw.diph);
        if (res == DIERR_ACQUIRED) {
            thiz->Unacquire();
            res = g_input_opt.realSetProperty(thiz, DIPROP_BUFFERSIZE, &dipdw.diph);
        }
        return res;
    }
    return g_input_opt.realSetProperty(thiz, guid, header);
}

HRESULT STDMETHODCALLTYPE CreateDevice_Changed(LPDIRECTINPUT8 thiz, const GUID& guid, LPDIRECTINPUTDEVICE8W* lpddevice, LPUNKNOWN lpunk)
{
    if (g_input_opt.dinput == thiz && guid == GUID_SysKeyboard) {
        if (g_input_opt.ddevice != nullptr) {
            *lpddevice = g_input_opt.ddevice;
            (*lpddevice)->Unacquire();
            return DI_OK;
        }
        auto res = g_input_opt.realCreateDevice(thiz, guid, lpddevice, lpunk);
        if (res == DI_OK) {
            HookVTable(*lpddevice, 2, DDevice_Release_Changed, (void**)&(g_input_opt.realDDRelease));
            HookVTable(*lpddevice, 7, Acquire_Changed, (void**)&(g_input_opt.realAcquire));
            HookVTable(*lpddevice, 9, GetDeviceState_Changed, (void**)&(g_input_opt.realGetDeviceState));
            HookVTable(*lpddevice, 11, SetDataFormat_Changed, (void**)&(g_input_opt.realSetDataFormat));
            HookVTable(*lpddevice, 13, SetCooperativeLevel_Changed, (void**)&(g_input_opt.realSetCooperativeLevel));
            HookVTable(*lpddevice, 6, SetProperty_Changed, (void**)&(g_input_opt.realSetProperty));
            HookVTable(*lpddevice, 25, Poll_Changed, (void**)&(g_input_opt.realPoll));
            g_input_opt.ddevice = *lpddevice;
            g_input_opt.ddevice = *lpddevice;
        }
        return res;
    }
    return g_input_opt.realCreateDevice(thiz, guid, lpddevice, lpunk);
}

HRESULT WINAPI DirectInput8Create_Changed(HINSTANCE hinst, DWORD dwVersion, const IID& riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter)
{
    if (g_input_opt.dinput != nullptr) {
        *ppvOut = g_input_opt.dinput;
        return DI_OK;
    }
    auto res = g_input_opt.g_realDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
    if (res == DI_OK) {
        HookVTable(*ppvOut, 2, Dinput_Release_Changed, (void**)&(g_input_opt.realDIRelease));
        HookVTable(*ppvOut, 3, CreateDevice_Changed, (void**)&(g_input_opt.realCreateDevice));
        HookVTable(*ppvOut, 4, EnumDevices_Changed, (void**)&(g_input_opt.realEnumDevices));
        g_input_opt.dinput = *(LPDIRECTINPUT8*)ppvOut;
    }
    return res;
}

HRESULT STDMETHODCALLTYPE GetDeviceState_Changed(LPDIRECTINPUTDEVICE8 thiz, DWORD num, LPVOID state)
{
    if (num != 256) { // no keyboard
        if (g_input_opt.g_disable_joy)
            return DIERR_INPUTLOST; // 8007001E

        HRESULT res = -1;
        res = g_input_opt.realGetDeviceState(thiz, num, state);

        if (num == sizeof(DIJOYSTATE) || num == sizeof(DIJOYSTATE2)) {
            auto* js = (DIJOYSTATE*)state;
            Ranges<long> di_range = { -1000, 1000, -1000, 1000 };
            bool ret_map = false;
            for (int i = 0; i < elementsof(js->rgdwPOV) && !ret_map; i++) {
                ret_map = pov_to_xy(js->lX, js->lY, di_range, js->rgdwPOV[i]);
            }
        }
        return res;
    }
    HRESULT res = DI_OK;
    if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_win32KeyAPI) {
        memset(state, 0, 256);
        for (auto keydef : keyBindDefine) {
            ((BYTE*)state)[keydef.dik] = (GetAsyncKeyState(keydef.vk) & 0x8000) ? 0x80 : 0;
            // though a bit slower ,,,
        }
    } else if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_RawInput) {
        memset(state, 0, 256);
        ProcessRawInput((BYTE*)state);
        if (g_input_opt.is_ri_inited) {
            res = DI_OK;
        } else {
            if (thiz == nullptr) {
                for (auto keydef : keyBindDefine) {
                    ((BYTE*)state)[keydef.dik] = (GetAsyncKeyState(keydef.vk) & 0x8000) ? 0x80 : 0;
                }
            } else {
                res = g_input_opt.realGetDeviceState(thiz, num, state);
            }
        }
    } else {
        if (!g_input_opt.use_get_device_data) {
            memset(state, 0, 256);
            res = g_input_opt.realGetDeviceState(thiz, num, state);
        } else {
            BYTE last_state[256];
            memcpy(last_state, g_input_opt.fake_di_State, 256);

            res = g_input_opt.realGetDeviceState(thiz, num, state);

            DIDEVICEOBJECTDATA rgdod[32];
            while (true) {
                DWORD dwItems = 32;
                res = thiz->GetDeviceData(sizeof(DIDEVICEOBJECTDATA), rgdod, &dwItems, 0);
                if (FAILED(res) || dwItems == 0) {
                    if (res == DIERR_INPUTLOST) {
                        res = thiz->Acquire();
                        if (FAILED(res))
                            break;
                    } else {
                        break;
                    }
                }
                for (DWORD i = 0; i < dwItems; i++) {
                    DWORD scanCode = rgdod[i].dwOfs;
                    if (rgdod[i].dwData & 0x80) {
                        g_input_opt.fake_di_State[scanCode] = 0x80;
                        if (last_state[scanCode] == 0) {
                            ((BYTE*)state)[scanCode] = 0x80;
                            // is_down[scanCode] = 0x80;
                        }
                    } else {
                        if (last_state[scanCode] == 0x80) {
                            ((BYTE*)state)[scanCode] = 0;
                        }
                        g_input_opt.fake_di_State[scanCode] = 0x00;
                    }
                }
            }
        }
    }

    if (g_keybind.size() != 0) {
        static BYTE new_keyBoardState[256] = { 0 };
        bool new_keyBoardState_changed[256] = { 0 };
        memcpy_s(new_keyBoardState, num, ((BYTE*)state), num);
        for (auto& bind : g_keybind) {
            if (IS_KEY_DOWN(((BYTE*)state)[bind.second.dik])) {
                new_keyBoardState[bind.first.dik] |= 0x80;
                if (!new_keyBoardState_changed[bind.first.dik])
                    new_keyBoardState[bind.second.dik] &= (~0x80);
                new_keyBoardState_changed[bind.first.dik] = true;
            }
        }
        memcpy_s(((BYTE*)state), num, new_keyBoardState, num);
    }
    if (g_input_opt.enable_auto_shoot) {
        bool cur_isdown = IS_KEY_DOWN(((BYTE*)state)[g_input_opt.shoot_key_DIK]);
        bool last_isdown = g_input_opt.last_is_auto_shoot_key_down;
        g_input_opt.last_is_auto_shoot_key_down = cur_isdown;
        if (cur_isdown && !last_isdown) {
            g_input_opt.is_auto_shooting = !g_input_opt.is_auto_shooting;
        }
        if (g_input_opt.is_auto_shooting) {
            bool is_other_down = false;
            is_other_down |= IS_KEY_DOWN(((BYTE*)state)[DIK_Z]);
            is_other_down |= IS_KEY_DOWN(((BYTE*)state)[DIK_X]);
            is_other_down |= IS_KEY_DOWN(((BYTE*)state)[DIK_C]);
            is_other_down |= IS_KEY_DOWN(((BYTE*)state)[DIK_D]);
            is_other_down |= IS_KEY_DOWN(((BYTE*)state)[DIK_ESCAPE]);
            is_other_down |= IS_KEY_DOWN(((BYTE*)state)[DIK_R]);
            is_other_down |= IS_KEY_DOWN(((BYTE*)state)[DIK_Q]);
            if (is_other_down) {
                g_input_opt.is_auto_shooting = false;
            } else {
                if (g_input_opt.is_th128) {
                    ((BYTE*)state)[DIK_C] = 0x80;
                } else {
                    ((BYTE*)state)[DIK_Z] = 0x80;
                }
            }
        }
    }

    if (g_input_opt.disable_xkey) {
        ((BYTE*)state)[DIK_X] = 0x0;
    }
    if (g_input_opt.disable_xkey && g_input_opt.disable_Ckey_at_same_time) {
        ((BYTE*)state)[DIK_C] = 0x0;
    }
    if (g_input_opt.disable_shiftkey) {
        ((BYTE*)state)[DIK_LSHIFT] = 0x0;
        ((BYTE*)state)[DIK_RSHIFT] = 0x0;
    }
    if (g_input_opt.force_shiftkey)
    {
        ((BYTE*)state)[DIK_LSHIFT] = 0x80;
        ((BYTE*)state)[DIK_RSHIFT] = 0x80;
    }
    if (g_input_opt.disable_zkey) {
        ((BYTE*)state)[DIK_Z] = 0x0;
    }
    if (g_input_opt.disable_f10_11_13) {
        ((BYTE*)state)[DIK_F10] = 0x0;
    }
    if (g_input_opt.g_socd_setting != InputOpt::SOCD_Setting::SOCD_Default) {
        static BYTE last_keyBoardState[256] = { 0 };
        static uint32_t cur_time = 0;
        static uint32_t keyBoard_press_time[4] = { 0 };

        BYTE* keyBoardState = (BYTE*)state;
        cur_time++;

        if (IS_KEY_DOWN(keyBoardState[DIK_LEFTARROW]) && !IS_KEY_DOWN(last_keyBoardState[DIK_LEFTARROW]))
            keyBoard_press_time[K_LEFT] = cur_time;
        if (IS_KEY_DOWN(keyBoardState[DIK_RIGHTARROW]) && !IS_KEY_DOWN(last_keyBoardState[DIK_RIGHTARROW]))
            keyBoard_press_time[K_RIGHT] = cur_time;
        if (IS_KEY_DOWN(keyBoardState[DIK_UPARROW]) && !IS_KEY_DOWN(last_keyBoardState[DIK_UPARROW]))
            keyBoard_press_time[K_UP] = cur_time;
        if (IS_KEY_DOWN(keyBoardState[DIK_DOWNARROW]) && !IS_KEY_DOWN(last_keyBoardState[DIK_DOWNARROW]))
            keyBoard_press_time[K_DOWN] = cur_time;

        memcpy_s(last_keyBoardState, num, keyBoardState, num);

        if (IS_KEY_DOWN(keyBoardState[DIK_LEFTARROW]) && IS_KEY_DOWN(keyBoardState[DIK_RIGHTARROW])) {
            if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_2) {
                if (keyBoard_press_time[K_LEFT] > keyBoard_press_time[K_RIGHT]) {
                    keyBoardState[DIK_RIGHTARROW] = 0;
                } else {
                    keyBoardState[DIK_LEFTARROW] = 0;
                }
            } else if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_N) {
                keyBoardState[DIK_RIGHTARROW] = 0;
                keyBoardState[DIK_LEFTARROW] = 0;
            }
        }
        if (IS_KEY_DOWN(keyBoardState[DIK_DOWNARROW]) && IS_KEY_DOWN(keyBoardState[DIK_UPARROW])) {
            if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_2) {
                if (keyBoard_press_time[K_UP] > keyBoard_press_time[K_DOWN]) {
                    keyBoardState[DIK_DOWNARROW] = 0;
                } else {
                    keyBoardState[DIK_UPARROW] = 0;
                }
            } else if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_N) {
                keyBoardState[DIK_DOWNARROW] = 0;
                keyBoardState[DIK_UPARROW] = 0;
            }
        }
    }
    if (g_fast_re_opt.fast_retry_count_down) {
        BYTE* keyBoardState = (BYTE*)state;
        if (g_fast_re_opt.fast_retry_count_down <= g_fast_re_opt.fast_retry_cout_down_max)
            keyBoardState[DIK_ESCAPE] = 0x80;
        if (g_fast_re_opt.fast_retry_count_down <= 1)
            keyBoardState[DIK_R] = 0x80;
    }

    g_input_latency_test.Input();
    return res;
}

#pragma endregion

#pragma region win32Keyboard


HRESULT STDMETHODCALLTYPE GetDeviceState_Changed(LPDIRECTINPUTDEVICE8 thiz, DWORD num, LPVOID state);
BOOL WINAPI GetKeyboardState_Changed(PBYTE keyBoardState)
{
    HRESULT res;
    if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_dinput8KeyAPI || g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_RawInput) {
        HRESULT res_dinpu8getState = DI_OK;
        BYTE keyboardState_dinput8[256];
        BYTE* keyboardState = keyboardState_dinput8;
        if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_dinput8KeyAPI) {
            if (g_input_opt.ddevice) {
                auto pres = g_input_opt.ddevice->Poll();
                if (FAILED(pres)) {
                    if (FAILED(g_input_opt.ddevice->Acquire())) {
                        goto LB_FINAL;
                    }
                    if (FAILED(g_input_opt.ddevice->Poll())) {
                        goto LB_FINAL;
                    }
                }
                res_dinpu8getState = g_input_opt.ddevice->GetDeviceState(256, keyboardState_dinput8);
            } else
                goto LB_FINAL;
        } else {
            if (g_input_opt.is_ri_inited) {
                GetDeviceState_Changed(nullptr, 256, keyboardState);
            } else
                goto LB_FINAL;
        }
        if (res_dinpu8getState == DI_OK) {
            memset(keyBoardState, 0, 256);
            for (auto keydef : keyBindDefine) {
                keyBoardState[keydef.vk] = keyboardState[keydef.dik];
            }
            if ((keyboardState[DIK_LSHIFT] & 0x80) || (keyboardState[DIK_RSHIFT] & 0x80))
                keyBoardState[VK_SHIFT] = 0x80;
            if ((keyboardState[DIK_LCONTROL] & 0x80) || (keyboardState[DIK_RCONTROL] & 0x80))
                keyBoardState[VK_CONTROL] = 0x80;
            if ((keyboardState[DIK_LMENU] & 0x80) || (keyboardState[DIK_RMENU] & 0x80))
                keyBoardState[VK_MENU] = 0x80;
            if ((keyboardState[DIK_RETURN] & 0x80) || (keyboardState[DIK_NUMPADENTER] & 0x80))
                keyBoardState[VK_RETURN] = 0x80;
            return TRUE;
        }
    }
LB_FINAL:
    res = g_input_opt.g_realGetKeyboardState(keyBoardState);
    if (g_keybind.size() != 0) {
        static BYTE new_keyBoardState[256] = { 0 };
        bool new_keyBoardState_changed[256] = { 0 };
        memcpy_s(new_keyBoardState, 256, keyBoardState, 256);
        for (auto& bind : g_keybind) {
            if (IS_KEY_DOWN(keyBoardState[bind.second.vk])) {
                new_keyBoardState[bind.first.vk] |= 0x80;
                if (!new_keyBoardState_changed[bind.first.vk])
                    new_keyBoardState[bind.second.vk] &= (~0x80);
                switch (bind.second.vk) {
                default:
                    break;
                case VK_LSHIFT:
                case VK_RSHIFT:
                    if (!new_keyBoardState_changed[VK_LSHIFT])
                        new_keyBoardState[VK_SHIFT] &= (~0x80);
                    break;
                case VK_LCONTROL:
                case VK_RCONTROL:
                    if (!new_keyBoardState_changed[VK_CONTROL])
                        new_keyBoardState[VK_CONTROL] &= (~0x80);
                    break;
                case VK_LMENU:
                case VK_RMENU:
                    if (!new_keyBoardState_changed[VK_MENU])
                        new_keyBoardState[VK_MENU] &= (~0x80);
                    break;
                }
                new_keyBoardState_changed[bind.first.vk] = true;
            }
        }
        if (IS_KEY_DOWN(new_keyBoardState[VK_LSHIFT]) || IS_KEY_DOWN(new_keyBoardState[VK_RSHIFT])) {
            new_keyBoardState[VK_SHIFT] |= 0x80;
        }
        if (IS_KEY_DOWN(new_keyBoardState[VK_LCONTROL]) || IS_KEY_DOWN(new_keyBoardState[VK_RCONTROL])) {
            new_keyBoardState[VK_CONTROL] |= 0x80;
        }
        if (IS_KEY_DOWN(new_keyBoardState[VK_LMENU]) || IS_KEY_DOWN(new_keyBoardState[VK_RMENU])) {
            new_keyBoardState[VK_MENU] |= 0x80;
        }
        memcpy_s(keyBoardState, 256, new_keyBoardState, 256);
    }
    if (g_input_opt.disable_xkey) {
        keyBoardState['X'] = 0x0;
    }
    if (g_input_opt.disable_xkey && g_input_opt.disable_Ckey_at_same_time) {
        keyBoardState['C'] = 0x0;
    }
    if (g_input_opt.disable_shiftkey) {
        keyBoardState[VK_LSHIFT] = keyBoardState[VK_LSHIFT] = keyBoardState[VK_SHIFT] = 0x0;
    }
    if (g_input_opt.force_shiftkey) {
        keyBoardState[VK_LSHIFT] = keyBoardState[VK_LSHIFT] = keyBoardState[VK_SHIFT] = 0x80;
    }
    if (g_input_opt.disable_zkey) {
        keyBoardState['Z'] = 0x0;
    }
    if (g_input_opt.disable_f10_11_13) {
        keyBoardState[VK_F10] = 0x0;
    }
    if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_Default) {
        g_input_latency_test.Input();
        return res;
    }
    static BYTE last_keyBoardState[256] = { 0 };
    static uint32_t cur_time = 0;
    static uint32_t keyBoard_press_time[4] = { 0 };

    cur_time++;

    if (IS_KEY_DOWN(keyBoardState[VK_LEFT]) && !IS_KEY_DOWN(last_keyBoardState[VK_LEFT]))
        keyBoard_press_time[K_LEFT] = cur_time;
    if (IS_KEY_DOWN(keyBoardState[VK_RIGHT]) && !IS_KEY_DOWN(last_keyBoardState[VK_RIGHT]))
        keyBoard_press_time[K_RIGHT] = cur_time;
    if (IS_KEY_DOWN(keyBoardState[VK_UP]) && !IS_KEY_DOWN(last_keyBoardState[VK_UP]))
        keyBoard_press_time[K_UP] = cur_time;
    if (IS_KEY_DOWN(keyBoardState[VK_DOWN]) && !IS_KEY_DOWN(last_keyBoardState[VK_DOWN]))
        keyBoard_press_time[K_DOWN] = cur_time;

    memcpy_s(last_keyBoardState, 256, keyBoardState, 256);

    if (IS_KEY_DOWN(keyBoardState[VK_LEFT]) && IS_KEY_DOWN(keyBoardState[VK_RIGHT])) {
        if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_2) {
            if (keyBoard_press_time[K_LEFT] > keyBoard_press_time[K_RIGHT]) {
                keyBoardState[VK_RIGHT] = 0;
            } else {
                keyBoardState[VK_LEFT] = 0;
            }
        } else if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_N) {
            keyBoardState[VK_RIGHT] = 0;
            keyBoardState[VK_LEFT] = 0;
        }
    }
    if (IS_KEY_DOWN(keyBoardState[VK_DOWN]) && IS_KEY_DOWN(keyBoardState[VK_UP])) {
        if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_2) {
            if (keyBoard_press_time[K_UP] > keyBoard_press_time[K_DOWN]) {
                keyBoardState[VK_DOWN] = 0;
            } else {
                keyBoardState[VK_UP] = 0;
            }
        } else if (g_input_opt.g_socd_setting == InputOpt::SOCD_Setting::SOCD_N) {
            keyBoardState[VK_DOWN] = 0;
            keyBoardState[VK_UP] = 0;
        }
    }
    g_input_latency_test.Input();
    return res;
}
#pragma endregion


void HookKeyboardInput()
{
    HookIAT(GetModuleHandle(NULL), "user32.dll", "GetKeyboardState", GetKeyboardState_Changed, (void**)&g_input_opt.g_realGetKeyboardState);
    HookIAT(GetModuleHandle(NULL), "dinput8.dll", "DirectInput8Create", DirectInput8Create_Changed, (void**)&g_input_opt.g_realDirectInput8Create);
}

void SetDinput8DeviceData()
{
    if (g_input_opt.use_get_device_data) {
        if (g_input_opt.use_get_device_data && g_input_opt.ddevice) {
            DIPROPDWORD dipdw;
            dipdw.diph.dwSize = sizeof(DIPROPDWORD);
            dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            dipdw.diph.dwObj = 0;
            dipdw.diph.dwHow = DIPH_DEVICE;
            dipdw.dwData = 64;
            SetProperty_Changed(g_input_opt.ddevice, DIPROP_BUFFERSIZE, &dipdw.diph);
        } else {
            DIPROPDWORD dipdw;
            dipdw.diph.dwSize = sizeof(DIPROPDWORD);
            dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
            dipdw.diph.dwObj = 0;
            dipdw.diph.dwHow = DIPH_DEVICE;
            dipdw.dwData = 0;
            SetProperty_Changed(g_input_opt.ddevice, DIPROP_BUFFERSIZE, &dipdw.diph);
        }
    }
}

void InitInput()
{
    if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_RawInput) {
        if (g_gameGuiHwnd == nullptr)
            return;
        if (g_input_opt.ddevice != nullptr) {
            g_input_opt.ddevice->Unacquire();
        }
        if (g_input_opt.is_ri_inited) {
            RAWINPUTDEVICE Rid;
            Rid.usUsagePage = 0x01;
            Rid.usUsage = 0x06;
            Rid.dwFlags = RIDEV_REMOVE;
            Rid.hwndTarget = NULL;
            auto res = RegisterRawInputDevices(&Rid, 1, sizeof(Rid));
            if (res) {
                g_input_opt.is_ri_inited = false;
            } else {
                return;
            }
        }
        RAWINPUTDEVICE Rid;
        Rid.usUsagePage = 0x01;
        Rid.usUsage = 0x06; // HID_USAGE_GENERIC_KEYBOARD
        Rid.dwFlags = 0;
        Rid.hwndTarget = (HWND)*g_gameGuiHwnd;

        if (!RegisterRawInputDevices(&Rid, 1, sizeof(Rid))) {
            return;
        }
        g_input_opt.is_ri_inited = true;
        memset(g_input_opt.fake_di_State, 0, 256);
        return;
    } else if (g_input_opt.g_keyboardAPI == InputOpt::KeyboardAPI::Force_dinput8KeyAPI) {
        if (g_gameGuiHwnd == nullptr)
            return;
        if (g_input_opt.is_ri_inited) {
            RAWINPUTDEVICE Rid;
            Rid.usUsagePage = 0x01;
            Rid.usUsage = 0x06;
            Rid.dwFlags = RIDEV_REMOVE;
            Rid.hwndTarget = NULL;
            auto res = RegisterRawInputDevices(&Rid, 1, sizeof(Rid));
            if (res) {
                g_input_opt.is_ri_inited = false;
            }
        }
        if (g_input_opt.ddevice != nullptr) {
            g_input_opt.ddevice->Unacquire();
            g_input_opt.ddevice->Acquire();
            return;
        }
        bool is_failed_dinput8 = false;
        // if (is_failed_dinput8)
        //     return;

        if (g_input_opt.dinput == nullptr) {
            if (g_input_opt.g_realDirectInput8Create) {
                DWORD d;
                auto res = DirectInput8Create_Changed(GetModuleHandle(0), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&d, NULL);
                if (res != DI_OK || g_input_opt.dinput == nullptr) {
                    is_failed_dinput8 = true;
                    return;
                }
            } else {
                is_failed_dinput8 = true;
                return;
            }
        }
        DWORD dev;
        auto res1 = g_input_opt.dinput->CreateDevice(GUID_SysKeyboard, (LPDIRECTINPUTDEVICE8*)&dev, NULL);
        if (res1 != DI_OK || g_input_opt.ddevice == nullptr) {
            is_failed_dinput8 = true;
            return;
        }
        is_failed_dinput8 |= FAILED(g_input_opt.ddevice->SetDataFormat(&c_dfDIKeyboard));
        is_failed_dinput8 |= FAILED(g_input_opt.ddevice->SetCooperativeLevel((HWND)*g_gameGuiHwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND));
        is_failed_dinput8 |= FAILED(g_input_opt.ddevice->Acquire());
        memset(g_input_opt.fake_di_State, 0, 256);
        return;
    }
}

}
