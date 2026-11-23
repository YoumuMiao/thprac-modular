// Included inside THPrac by thprac_games.cpp, after the capture state machine.
namespace {
NativeSpellProfile nativeSpellProfile;
HookCtx nativeSpellHooks[3];
struct NativeSpellSnapshot {
    BossCaptureNativeSpellEvent event;
    uintptr_t object = 0, counter = 0;
    uint32_t id = 0, before = 0;
    bool success = false;
};
NativeSpellSnapshot nativeSpellSnapshot;
bool nativeSpellInstalled = false;

uintptr_t NativeSpellAddress(uintptr_t address)
{
    return address ? address + (nativeSpellProfile.rva ? ingame_image_base : 0) : 0;
}
uint32_t NativeSpellRead(uintptr_t address, unsigned width = 4)
{
    if (width == 1) return CaptureRead<uint8_t>(address);
    if (width == 2) return CaptureRead<uint16_t>(address);
    return CaptureRead<uint32_t>(address);
}
uintptr_t NativeSpellObject()
{
    const auto address = NativeSpellAddress(nativeSpellProfile.object);
    return nativeSpellProfile.objectIndirect ? CaptureRead<uintptr_t>(address) : address;
}
uintptr_t NativeSpellRegisterValue(const CONTEXT& context, NativeSpellRegister reg)
{
    switch (reg) {
    case NativeSpellRegister::Ebx: return context.Ebx;
    case NativeSpellRegister::Ecx: return context.Ecx;
    case NativeSpellRegister::Edi: return context.Edi;
    case NativeSpellRegister::Ebp: return context.Ebp;
    default: return 0;
    }
}
bool NativeSpellReplayActive()
{
    const auto& replay = nativeSpellProfile.replay;
    uintptr_t address = NativeSpellAddress(replay.address);
    if (!address) return false;
    if (replay.indirect) address = CaptureRead<uintptr_t>(address);
    if (!address) return false;
    const auto value = NativeSpellRead(address + replay.offset, replay.width);
    return replay.nonzero ? value != 0 : (value & replay.mask) == replay.value;
}
uintptr_t NativeSpellCounterAddress(const BossCaptureNativeSpellEvent& event, uint32_t id)
{
    const auto& counter = nativeSpellProfile.counter;
    if (!counter.base || id >= 2048) return 0;
    uintptr_t base = NativeSpellAddress(counter.base);
    if (counter.indirect) base = CaptureRead<uintptr_t>(base);
    if (!base) return 0;
    uint32_t shot = static_cast<uint32_t>(event.key.shot);
    if (counter.shotFirst) {
        shot = NativeSpellRead(NativeSpellAddress(counter.shotFirst), counter.shotWidth) * counter.shotScale;
        if (counter.shotSecond) shot += NativeSpellRead(NativeSpellAddress(counter.shotSecond), counter.shotWidth);
    }
    if (counter.shotStride && shot >= 64) return 0;
    const unsigned mode = counter.modeFlags
        && (NativeSpellRead(NativeSpellAddress(counter.modeFlags)) & counter.modeMask) == counter.modeValue;
    const uint64_t address = uint64_t(base) + counter.offset + uint64_t(id) * counter.idStride
        + uint64_t(shot) * counter.shotStride + mode * counter.modeStride;
    return address <= UINTPTR_MAX ? static_cast<uintptr_t>(address) : 0;
}
void NativeSpellReset() { nativeSpellSnapshot = {}; }
void __fastcall NativeSpellBefore(PCONTEXT context, HookCtx*)
{
    NativeSpellReset();
    if (!AddonEnabled(Addon::Capture)) return;
    const auto object = NativeSpellObject();
    if (!object || (NativeSpellRead(object + nativeSpellProfile.flagsOffset) & nativeSpellProfile.activeMask) != nativeSpellProfile.activeValue)
        return;
    if (nativeSpellProfile.objectArgument != NativeSpellRegister::Primary
        && NativeSpellRegisterValue(*context, nativeSpellProfile.objectArgument) != object) return;
    if (NativeSpellReplayActive()) return;
    uintptr_t owner = 0;
    if (nativeSpellProfile.ownerRegister != NativeSpellRegister::Primary) {
        const auto reg = NativeSpellRegisterValue(*context, nativeSpellProfile.ownerRegister);
        owner = CaptureRead<uintptr_t>(reg + nativeSpellProfile.ownerOffset);
    }
    const auto event = BossCaptureNativeSpellBefore(owner);
    if (!event.valid) return;
    nativeSpellSnapshot.event = event;
    nativeSpellSnapshot.object = object;
    nativeSpellSnapshot.id = NativeSpellRead(object + nativeSpellProfile.idOffset);
    nativeSpellSnapshot.counter = NativeSpellCounterAddress(event, nativeSpellSnapshot.id);
    if (nativeSpellSnapshot.counter)
        nativeSpellSnapshot.before = NativeSpellRead(nativeSpellSnapshot.counter, nativeSpellProfile.counter.width);
}
void __fastcall NativeSpellSuccess(PCONTEXT, HookCtx*) { BossCaptureNativeSpellSucceeded(); }
void __fastcall NativeSpellAfter(PCONTEXT, HookCtx*)
{
    const auto before = nativeSpellSnapshot;
    NativeSpellReset();
    if (!before.event.valid || NativeSpellObject() != before.object
        || NativeSpellRead(before.object + nativeSpellProfile.idOffset) != before.id || NativeSpellReplayActive()) return;
    const auto counter = NativeSpellCounterAddress(before.event, before.id);
    const bool same = counter && counter == before.counter;
    const auto after = same ? NativeSpellRead(counter, nativeSpellProfile.counter.width) : before.before;
    BossCaptureNativeSpellAfter(before.event, before.success || (same && after > before.before));
    BossCapture::Log("native-event th{:02} id={} before={} after={} success={}",
        nativeSpellProfile.game, before.id, before.before, after, before.success);
}
bool NativeSpellSiteMatches(const NativeSpellSite& site, bool existing)
{
    if (!site.address || !site.bytes || !site.Length() || site.Length() > 16) return false;
    const auto address = NativeSpellAddress(site.address);
    for (unsigned i = 0; i < site.Length(); ++i) {
        const auto actual = CaptureRead<uint8_t>(address + i);
        if (!i && existing && actual == 0xcc) continue;
        const auto expected = (char_to_nibble(site.bytes[2*i]) << 4) | char_to_nibble(site.bytes[2*i+1]);
        if (actual != expected) return false;
    }
    return true;
}
}

void BossCaptureNativeSpellSucceeded()
{
    if (nativeSpellSnapshot.event.valid && NativeSpellObject() == nativeSpellSnapshot.object
        && NativeSpellRead(nativeSpellSnapshot.object + nativeSpellProfile.idOffset) == nativeSpellSnapshot.id)
        nativeSpellSnapshot.success = true;
}
bool BossCaptureInstallNativeSpellEvents(int game)
{
    if (!AddonEnabled(Addon::Capture)) return false;
    if (nativeSpellInstalled) return nativeSpellProfile.game == game;
    nativeSpellProfile = NativeSpellProfileFor(game);
    if (!nativeSpellProfile.game) return false;
    const NativeSpellSite sites[] {nativeSpellProfile.before, nativeSpellProfile.success, nativeSpellProfile.after};
    for (unsigned i = 0; i < 3; ++i) {
        if (!NativeSpellSiteMatches(sites[i], i == 1 && nativeSpellProfile.existingSuccessHook)) {
            BossCapture::Log("native-hooks th{:02} byte mismatch at {:#x}", game, NativeSpellAddress(sites[i].address));
            return false;
        }
    }
    const char* names[] {"capture_native_before", "capture_native_success", "capture_native_after"};
    Callback* callbacks[] {NativeSpellBefore, NativeSpellSuccess, NativeSpellAfter};
    for (unsigned i = 0; i < 3; ++i) {
        if (i == 1 && nativeSpellProfile.existingSuccessHook) continue;
        auto& hook = nativeSpellHooks[i];
        hook.addr = sites[i].address;
        hook.name = names[i];
        hook.callback = callbacks[i];
        hook.addonRequirement = 1u << static_cast<unsigned>(Addon::Capture);
        hook.data = PatchHookImpl(static_cast<uint8_t>(sites[i].Length()));
        hook.Setup();
        hook.Enable();
    }
    captureNativeReset = NativeSpellReset;
    nativeSpellInstalled = true;
    BossCapture::Log("native-hooks th{:02} installed", game);
    return true;
}
