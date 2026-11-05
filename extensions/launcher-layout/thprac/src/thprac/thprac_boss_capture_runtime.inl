// Canonical native phase adapters; included once inside THPrac by thprac_games.cpp.
namespace {
struct CaptureFile { int version, stage, ordinal; size_t size; unsigned char prefix[16]; size_t headerSize; uint32_t headerHash; };
struct CaptureSpan { size_t file, begin, end; int section, kind; unsigned difficultyMask; int variant; bool hasPattern; };
#include "thprac_boss_capture_catalog.inl"
BossCaptureContextProvider captureContext = nullptr;
BossCaptureLabelProvider captureLabel = nullptr;
BossCaptureTransitionObserver captureTransitionObserver = nullptr;
BossCaptureEligibilityOverride captureEligibilityOverride = nullptr;
struct CaptureConfiguration { BossCapture::Key key; uint64_t failures; bool hasPattern; };
std::map<uintptr_t, CaptureConfiguration> captureConfigured;
std::map<std::pair<size_t,uintptr_t>, bool> captureFileIdentity;
uintptr_t captureOwner = 0, capturePrimaryHint = 0;
uint64_t captureFailures = 0, captureFrameNumber = 0;
int captureLastSection = 0;
bool capturePending = false, capturePendingFailed = false;
bool captureWasEnabled = true;
bool captureTh10MomijiDefeated = false;
bool captureTh12Checkpoint = false;
BossCapture::Outcome capturePendingOutcome = BossCapture::kCaptured;
uint64_t capturePendingFrame = 0;
struct CaptureSettlement { BossCapture::Key key; bool failed; BossCapture::Outcome outcome; uint64_t frame; bool nativeCaptured = false; };
std::vector<CaptureSettlement> captureSettlements;
template<class T> T CaptureRead(uintptr_t address) {
    MEMORY_BASIC_INFORMATION region {};
    if (!address || !VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))
        || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        || address + sizeof(T) < address
        || address + sizeof(T) > reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize)
        return {};
    T value;
    memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}
uintptr_t CapturePrimary(const BossCaptureContext& context) {
    for (auto owner : context.bosses) if (owner) return owner;
    return 0;
}

// TH08 Last Word scripts live in the per-character ECL files and keep the stage
// counter of the character's home stage, so a plain stage row would scatter one
// attack over nine different stages. They share the grouped "Last Word" row.
int CaptureStageGroup(int version, int section, int stage) {
    if (version == 8 && section >= TH08::TH08_LW_1 && section <= TH08::TH08_LW_17)
        return BossCapture::kGroupedStage;
    return stage;
}

bool CaptureIsTh07Stage6Opening(const BossCapture::Key& key) {
    return key.game == 7 && key.stage == 6 && key.attack == TH07::TH07_ST6_CHAOS
        && key.kind == BossCapture::kNonSpell;
}

bool CaptureIsTh12Road(const BossCapture::Key& key) {
    return key.game == 12 && (key.stage == 4 || key.stage == 5) && key.attack == kTh12LatterChapterAttack
        && key.kind == BossCapture::kNonSpell;
}

bool CaptureIsTh10Momiji(const BossCapture::Key& key) {
    return key.game == 10 && key.stage == 4 && key.attack == TH10::TH10_ST4_MID1
        && key.kind == BossCapture::kNonSpell;
}

bool CaptureTh10MomijiWaiting(const BossCapture::Runtime& tracker) {
    return captureTh10MomijiDefeated && tracker.Active() && CaptureIsTh10Momiji(tracker.ActiveKey());
}

void CaptureSetContext(const BossCaptureContext& c) {
    BossCapture::Context context;
    context.game = c.version; context.difficulty = c.difficulty; context.shot = c.shot;
    context.replay = c.replay; context.practice = c.practiceSection > 0;
    BossCapture::Instance().SetContext(context);
}
void CaptureConfirm() {
    auto& tracker = BossCapture::Instance();
    if (!capturePending || !tracker.Active()) return;
    BossCapture::Log("settle th{:02} stage={} attack={} kind={} failed={} outcome={}",
        tracker.ActiveKey().game, tracker.ActiveKey().stage, tracker.ActiveKey().attack,
        tracker.ActiveKey().kind, capturePendingFailed, static_cast<int>(capturePendingOutcome));
    captureSettlements.push_back({tracker.ActiveKey(), capturePendingFailed, capturePendingOutcome, captureFrameNumber});
    tracker.AbortAttack();
    capturePending = false;
    captureOwner = 0;
}
void CaptureFlush() {
    auto& store = BossCapture::Instance().Storage();
    for (const auto& item : captureSettlements) {
        const auto found = store.Session().find(item.key);
        if (found == store.Session().end()) continue;
        const auto& counts = found->second;
        if (counts.captures + counts.timeouts >= counts.attempts) continue;
        if (item.outcome == BossCapture::kTimedOut) store.RecordTimeout(item.key);
        else if (!item.failed) store.RecordCapture(item.key);
    }
    const bool hadSettlements = !captureSettlements.empty();
    captureSettlements.clear();
    if (hadSettlements) BossCapture::Instance().Flush();
}
void CaptureActivate(uintptr_t owner, const CaptureConfiguration& configured) {
    auto& tracker = BossCapture::Instance();
    const bool previousNonspell = tracker.Active() && tracker.ActiveKey().kind == BossCapture::kNonSpell;
    if (tracker.Active()) {
        if (tracker.ActiveKey() == configured.key) {
            // Reconfiguring the same named attack is an internal HP/timer phase,
            // not the end of the full nonspell or spell.
            capturePending = false; captureOwner = owner; return;
        }
        CaptureConfirm(); // A different script alone cannot turn an abort into a capture.
        tracker.AbortAttack();
        capturePending = false;
    }
    captureTh10MomijiDefeated = false;
    if (captureTransitionObserver) captureTransitionObserver(previousNonspell, configured.key.kind == BossCapture::kSpell);
    const auto* name = captureLabel ? captureLabel(configured.key.attack, configured.key.difficulty) : nullptr;
    const bool began = tracker.BeginAttack(configured.key.stage, configured.key.attack, configured.key.kind, name ? name : "");
    BossCapture::Log("activate th{:02} stage={} attack={} kind={} diff={} shot={} {}",
        configured.key.game, configured.key.stage, configured.key.attack, configured.key.kind,
        configured.key.difficulty, configured.key.shot, began ? "new" : "repeat");
    if (began) tracker.Flush(); // A retry/quit must not lose an attempt from the denominator.
    if (configured.failures != captureFailures) tracker.MarkFailure();
    captureOwner = owner;
    captureLastSection = configured.key.attack;
}
struct CaptureCandidate { int section, kind; bool hasPattern; };

// TH08's loader (0x418393..0x418415) adds the image base to all sixteen
// timeline pointers at +8 and to the subroutine table at +0x48. Compare their
// original file offsets, not the relocated process addresses, with the catalog.
// Raw images remain valid too, for callers inspecting an unloaded ECL file.
bool CaptureHeaderRelocated(const CaptureFile& file, uintptr_t base) {
    if (file.version != 8) return false;
    uint32_t original = 0;
    memcpy(&original, file.prefix + 8, sizeof(original));
    const uint32_t first = CaptureRead<uint32_t>(base + 8);
    return first != original && first - static_cast<uint32_t>(base) == original;
}
uint8_t CaptureHeaderByte(const CaptureFile& file, uintptr_t base, size_t offset, bool relocated) {
    const size_t subCount = file.prefix[4] | (static_cast<size_t>(file.prefix[5]) << 8);
    if (relocated && offset >= 8 && offset < 0x48 + subCount * 4) {
        const uint32_t relative = CaptureRead<uint32_t>(base + (offset & ~size_t(3)))
            - static_cast<uint32_t>(base);
        return static_cast<uint8_t>(relative >> ((offset & 3) * 8));
    }
    return CaptureRead<uint8_t>(base + offset);
}
bool CaptureHeaderPrefixMatches(const CaptureFile& file, uintptr_t base, bool relocated) {
    for (size_t i = 0; i < sizeof(file.prefix); ++i)
        if (CaptureHeaderByte(file, base, i, relocated) != file.prefix[i]) return false;
    return true;
}

// TH08 can only be verified in game, so a miss has to explain itself: which
// image the game loaded, where the instruction really is, and which catalog
// files were rejected (range / header prefix / header hash).
void CaptureDiagnose8(const BossCaptureContext& c, uintptr_t instruction) {
    if (c.version != 8) return;
    static uint64_t lastLog = 0;
    const uint64_t now = GetTickCount64();
    if (lastLog && now - lastLog < 2000) return;
    lastLog = now;
    const auto base = CaptureRead<uintptr_t>(c.fileTable);
    const auto offset = (base && instruction >= base) ? instruction - base : 0;
    std::string line = std::format("scan th08 stage={} instr={:#x} base={:#x} off={:#x} hdr=",
        c.stage, instruction, base, offset);
    for (int i = 0; i < 12; ++i)
        line += std::format("{:02x}", CaptureRead<uint8_t>(base + i));
    for (size_t index = 0; index < _countof(captureFiles); ++index) {
        const auto& f = captureFiles[index];
        if (f.version != 8 || f.stage != c.stage) continue;
        const bool relocated = CaptureHeaderRelocated(f, base);
        const bool prefix = CaptureHeaderPrefixMatches(f, base, relocated);
        const bool range = base && instruction >= base && instruction - base < f.size;
        line += std::format(" | f{} size={:#x} range={} prefix={} relocated={}", index, f.size, range, prefix, relocated);
    }
    BossCapture::AppendLog(line);
}

std::vector<CaptureCandidate> CaptureSections(const BossCaptureContext& c, uintptr_t instruction, bool onlyWithoutPattern = false) {
    CaptureDiagnose8(c, instruction);
    std::vector<CaptureCandidate> result;
    if (!c.fileTable || !instruction) return result;
    for (size_t index = 0; index < _countof(captureFiles); ++index) {
        const auto& f = captureFiles[index];
        if (f.version != c.version || f.stage != c.stage) continue;
        const auto base = CaptureRead<uintptr_t>(c.fileTable + f.ordinal * sizeof(uintptr_t));
        if (!base || instruction < base || instruction - base >= f.size) continue;
        const bool relocated = CaptureHeaderRelocated(f, base);
        if (!CaptureHeaderPrefixMatches(f, base, relocated)) continue;
        const auto identityKey = std::make_pair(index,base);
        auto identity = captureFileIdentity.find(identityKey);
        if (identity == captureFileIdentity.end()) {
            uint32_t hash = 2166136261u;
            for (size_t i=0;i<f.headerSize;++i)
                hash = (hash ^ CaptureHeaderByte(f,base,i,relocated))*16777619u;
            identity = captureFileIdentity.emplace(identityKey,hash == f.headerHash).first;
        }
        if (!identity->second) continue;
        const auto offset = instruction - base;
        for (const auto& s : captureSpans) {
            if (s.file != index || offset < s.begin || offset >= s.end) continue;
            if (onlyWithoutPattern && s.hasPattern) continue;
            if (s.variant >= 16) {
                if (c.version != 6 || c.shot != s.variant - 16) continue;
            } else if (s.variant && s.variant != c.phaseVariant) continue;
            const int lookupDifficulty = c.version == 13 && c.difficulty == 5 ? 3 : c.difficulty;
            if (lookupDifficulty < 0 || lookupDifficulty > 9 || !(s.difficultyMask & (1u << lookupDifficulty))) continue;
            if (std::none_of(result.begin(), result.end(), [&](const auto& x) { return x.section == s.section; }))
                result.push_back({s.section,s.kind,s.hasPattern});
        }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) { return a.section < b.section; });
    return result;
}
} // namespace

void BossCaptureSetTransitionObserver(BossCaptureTransitionObserver observer, BossCaptureEligibilityOverride eligibility) {
    captureTransitionObserver = observer; captureEligibilityOverride = eligibility;
}
uintptr_t BossCaptureReadPointer(uintptr_t address) { return CaptureRead<uintptr_t>(address); }
void BossCaptureAttach(BossCaptureContextProvider context, BossCaptureLabelProvider label) {
    captureContext = context; captureLabel = label;
    if (!AddonEnabled(Addon::Capture)) return;
    BossCapture::SetNativeBridgeActive(true);
    CaptureSetContext(context());
    BossCapture::Instance().EnsureLoaded();
    captureWasEnabled = BossCapture::Instance().Storage().Enabled();
}
uintptr_t BossCaptureCurrentInstruction(uintptr_t owner) {
    if (!owner || !captureContext) return 0;
    const auto context = captureContext();
    // TH06-TH08 keep the current ECL instruction in a flat field of the enemy
    // object (TH08: enemy+0x7f8, read by the dispatcher at 0x4184b0). The
    // thread-relative layout below belongs to the TH10+ engines only.
    if (context.instructionOffset)
        return CaptureRead<uintptr_t>(owner + context.instructionOffset);
    const int version = context.version;
    if (version <= 14) {
        const auto thread = CaptureRead<uintptr_t>(owner + 4);
        return CaptureRead<uintptr_t>(thread + 4);
    }
    const auto thread = CaptureRead<uintptr_t>(owner + 0xc);
    const int sub = CaptureRead<int>(thread + 4), offset = CaptureRead<int>(thread + 8);
    if (!thread || sub < 0 || sub > 8192 || offset < 0 || offset > 0x1000000) return 0;
    uintptr_t table = 0;
    if (version == 20) {
        const auto host = CaptureRead<uintptr_t>(thread + 0x28);
        const auto manager = CaptureRead<uintptr_t>(host + 0x58);
        table = CaptureRead<uintptr_t>(manager + 0x210);
    } else {
        const auto host = CaptureRead<uintptr_t>(thread + 0x1018);
        const auto manager = CaptureRead<uintptr_t>(host + (version == 18 ? 0x1218 : 0x11f8));
        table = CaptureRead<uintptr_t>(manager + 0x8c);
    }
    const auto base = CaptureRead<uintptr_t>(table + sub * 8 + 4);
    return base ? base + offset + 16 : 0;
}


namespace {
void CaptureIdentify(uintptr_t owner, uintptr_t instruction, const BossCaptureContext& c, bool observed, bool isBossMode) {
    auto& tracker = BossCapture::Instance();
    if (CaptureTh10MomijiWaiting(tracker)
        || (tracker.Active() && CaptureIsTh12Road(tracker.ActiveKey()))) return;
    // The "only sections without a next-pattern" rule exists to stop the TH10+
    // per-frame observer from re-entering a spell whose script is still running.
    // Older engines expose the plain script pointer, so observing them means
    // walking the catalog directly; their spans already end at the next root.
    const bool patternFilter = (observed && c.version >= 10) || isBossMode;
    const auto candidates = CaptureSections(c, instruction, patternFilter);
    if (candidates.empty()) {
        // Throttled: a boss script that never lands in the catalog must not fill
        // the log, but the first few misses are exactly what a bug report needs.
        static unsigned misses = 0;
        if (c.version == 8 && (misses++ % 120u) == 0)
            BossCapture::Log("identify-miss th08 stage={} instr={:#x} opcode={} observed={} primary={:#x}",
                c.stage, instruction, CaptureRead<uint16_t>(instruction + 4), observed, CapturePrimary(c));
        return; // Never substitute a selected practice section for an unknown script.
    }
    CaptureCandidate chosen {};
    for (auto candidate : candidates) if (!captureLastSection && candidate.section == c.practiceSection) { chosen = candidate; break; }
    // TH08 practice runs the boss's setup/nonspell script before jumping to
    // the selected attack. Do not create an attempt (or HUD entry) for that
    // bootstrap script. Start only when the real ECL span matches the target;
    // after that, ordinary subsequent attacks are recorded as usual.
    if (c.version == 8 && c.practiceSection > 0 && !captureLastSection && !chosen.section)
        return;
    if (!chosen.section && tracker.Active() && captureOwner == owner)
        for (auto candidate : candidates) if (candidate.section == tracker.ActiveKey().attack) { chosen = candidate; break; }
    if (!chosen.section && candidates.size() == 1) chosen = candidates.front();
    if (!chosen.section) {
        // Explicitly catalogued shared routines in early games are anchored by
        // the last identified attack; a raw chapter number is never used.
        for (auto candidate : candidates) if (candidate.section > captureLastSection) { chosen = candidate; break; }
    }
    if (!chosen.section) return;
    BossCapture::Key key;
    key.game = c.version;
    key.stage = CaptureStageGroup(c.version, chosen.section, c.stage + 1);
    key.attack = chosen.section;
    key.kind = chosen.kind; key.difficulty = c.difficulty; key.shot = c.shot;
    if (!key.Valid()) return;
    auto it = captureConfigured.find(owner);
    if (it == captureConfigured.end() || !(it->second.key == key))
        it = captureConfigured.insert_or_assign(owner,CaptureConfiguration{key,captureFailures,chosen.hasPattern}).first;
    if (CapturePrimary(c) == owner && !(observed && tracker.Active() && tracker.ActiveKey() == key)) CaptureActivate(owner,it->second);
}
}
void BossCaptureConfiguredCurrent(uintptr_t owner) {
    if (!AddonEnabled(Addon::Capture)) return;
    BossCaptureConfigured(owner, BossCaptureCurrentInstruction(owner));
}
void BossCaptureConfigured(uintptr_t owner, uintptr_t instruction) {
    if (!AddonEnabled(Addon::Capture)) return;
    if (!captureContext || !owner || !instruction) return;
    const auto c = captureContext();
    auto& tracker = BossCapture::Instance();
    if (c.replay || !c.gameplay || !tracker.Storage().Enabled()) return;
    CaptureSetContext(c);
    const int opcode = CaptureRead<uint16_t>(instruction + 4);
    const int setBoss = c.version == 6 ? 101 : c.version == 7 ? 99 : c.version == 8 ? 127
        : c.version <= 11 ? 332 : c.version == 12 ? 412 : 512;
    const int argumentOffset = c.version < 10 ? 12 : 16;
    if (opcode == setBoss && !(CaptureRead<uint16_t>(instruction + (c.version < 10 ? 10 : 8)) & 1)
        && CaptureRead<int32_t>(instruction + argumentOffset) == -1) {
        BossCaptureScriptExit(owner);
        return;
    }
    // TH08 subtracts one from the opcode at 0x418604 before indexing the
    // dispatcher table (0x41ecbe): boss=127, timer=132, next timer/pattern=134.
    // Table indices 126/131 are not the opcodes stored in an ECL instruction.
    const bool relevant = c.version == 6 ? opcode == 115 || opcode == 93 || opcode == setBoss
        : c.version == 7 ? opcode == 114 || opcode == 90 || opcode == setBoss
        : c.version == 8 ? opcode == 132 || opcode == 134 || opcode == 122 || opcode == setBoss
        : opcode == (c.version <= 11 ? 334 : c.version == 12 ? 414 : 514) || opcode == setBoss;
    if (!relevant) return;
    const auto primary=CapturePrimary(c);
    capturePrimaryHint=primary ? primary : (opcode==setBoss ? owner : capturePrimaryHint);
    CaptureIdentify(owner,instruction,c,false,opcode == setBoss);
}
void BossCaptureObservedCurrent(uintptr_t owner) {
    if (!AddonEnabled(Addon::Capture)) return;
    if (!captureContext || !owner) return;
    const auto c=captureContext();
    if (c.replay || !c.gameplay) return;
    const auto known=captureConfigured.find(owner);
    if (owner!=capturePrimaryHint && owner!=captureOwner && known==captureConfigured.end()) return;
    // TH10+ skips a running spell script that already set its pattern; the older
    // engines have no such marker, so they are walked every frame instead.
    if (c.version >= 10 && known!=captureConfigured.end() && known->second.hasPattern && !capturePending) return;
    if (!BossCapture::Instance().Storage().Enabled()) return;
    const auto instruction=BossCaptureCurrentInstruction(owner);
    if (!instruction) return;
    const auto opcode=CaptureRead<uint16_t>(instruction+4);
    const int setBoss = c.version == 6 ? 101 : c.version == 7 ? 99 : c.version == 8 ? 127
        : c.version <= 11 ? 332 : c.version == 12 ? 412 : 512;
    const int argumentOffset=c.version<10?12:16;
    if (opcode==setBoss && !(CaptureRead<uint16_t>(instruction+(c.version<10?10:8))&1)
        && CaptureRead<int32_t>(instruction+argumentOffset)<0) return;
    CaptureSetContext(c);
    CaptureIdentify(owner,instruction,c,true,false);
}

// Called only from TH20's native integer-argument reader. These are the same
// valid VM pointers that the original reader immediately dereferences. Do the
// cheap opcode test before consulting any capture context or script catalog.
void BossCaptureEclArguments20(uintptr_t owner) {
    if (!AddonEnabled(Addon::Capture)) return;
    if (!captureContext || !owner || !BossCapture::Instance().Storage().Enabled()) return;
    const auto thread=*reinterpret_cast<const uintptr_t*>(owner+0xc);
    if (!thread) return;
    const int sub=*reinterpret_cast<const int*>(thread+4), offset=*reinterpret_cast<const int*>(thread+8);
    if (sub<0 || sub>8192 || offset<0 || offset>0x1000000) return;
    const auto host=*reinterpret_cast<const uintptr_t*>(thread+0x28);
    if (!host) return;
    const auto manager=*reinterpret_cast<const uintptr_t*>(host+0x58);
    if (!manager) return;
    const auto table=*reinterpret_cast<const uintptr_t*>(manager+0x210);
    if (!table) return;
    const auto base=*reinterpret_cast<const uintptr_t*>(table+sub*8+4);
    if (!base) return;
    const auto instruction=base+offset+16;
    const auto opcode=*reinterpret_cast<const uint16_t*>(instruction+4);
    if (opcode==514 || opcode==512) BossCaptureConfigured(owner,instruction);
    BossCaptureObservedCurrent(owner);
}
void BossCaptureCleared(uintptr_t owner, bool timeout) {
    if (!AddonEnabled(Addon::Capture)) return;
    if (!captureContext || !owner) return;
    const auto c = captureContext();
    auto& tracker = BossCapture::Instance();
    if (c.replay || !tracker.Storage().Enabled() || CapturePrimary(c) != owner) return;
    const auto configured = captureConfigured.find(owner);
    if (!tracker.Active() && configured != captureConfigured.end()) CaptureActivate(owner,configured->second);
    if (!tracker.Active() || captureOwner != owner) return;
    if (!timeout && CaptureIsTh10Momiji(tracker.ActiveKey())) {
        captureTh10MomijiDefeated = true;
        capturePending = false;
        captureOwner = 0;
        captureConfigured.erase(owner);
        BossCapture::Log("th10 Momiji defeated; waiting for reinforcements");
        return;
    }
    if (tracker.ActiveKey().kind == BossCapture::kSpell && !c.spellCapturable
        && !(captureEligibilityOverride && captureEligibilityOverride())) tracker.MarkFailure();
    capturePending = true;
    capturePendingFailed = tracker.Failed();
    capturePendingOutcome = timeout && !(tracker.ActiveKey().kind == BossCapture::kSpell && c.survival && !tracker.Failed()) ? BossCapture::kTimedOut : BossCapture::kCaptured;
    capturePendingFrame = captureFrameNumber;
}
void BossCaptureScriptExit(uintptr_t owner) {
    if (!AddonEnabled(Addon::Capture)) return;
    const auto& tracker = BossCapture::Instance();
    // A scripted departure is not an HP defeat for Momiji.
    if (!capturePending && tracker.Active() && CaptureIsTh10Momiji(tracker.ActiveKey()))
        return;
    if (!capturePending) BossCaptureCleared(owner);
    if (captureOwner == owner && capturePending) {
        CaptureConfirm();
        captureConfigured.erase(owner);
    }
}
void BossCaptureFailed() {
    if (!AddonEnabled(Addon::Capture)) return;
    if (!captureContext || captureContext().replay) return;
    ++captureFailures;
    auto& tracker = BossCapture::Instance();
    // After the HP defeat, only the TH10 reinforcement collision hook fails this attempt.
    if (!CaptureTh10MomijiWaiting(tracker))
        tracker.MarkFailure();
    if (capturePending && capturePendingFrame == captureFrameNumber) capturePendingFailed = true;
    for (auto& item : captureSettlements)
        if (item.frame == captureFrameNumber && !item.nativeCaptured) item.failed = true;
}
void BossCapturePlayerHit() {
    if (!AddonEnabled(Addon::Capture)) return;
    if (!captureContext || captureContext().replay) return;
    auto& tracker = BossCapture::Instance();
    if (captureContext().version == 8 && tracker.Active()
        && tracker.ActiveKey().kind == BossCapture::kSpell) {
        const auto& key = tracker.ActiveKey();
        // Collision processing can run after the boss has entered its spell in
        // this same frame. The outgoing nonspell still owns this boundary hit.
        for (auto it = captureSettlements.rbegin(); it != captureSettlements.rend(); ++it) {
            if (it->frame != captureFrameNumber || it->key.game != key.game
                || it->key.stage != key.stage || it->key.difficulty != key.difficulty
                || it->key.shot != key.shot || it->key.kind != BossCapture::kNonSpell) continue;
            ++captureFailures;
            it->failed = true;
            // Re-observing the new spell must not inherit the outgoing hit.
            for (auto& entry : captureConfigured)
                if (entry.second.key == key) entry.second.failures = captureFailures;
            BossCapture::Log("hit th08 attack={} kind=0 boundary=true", it->key.attack);
            return;
        }
    }
    BossCaptureFailed();
    if (tracker.Active()) BossCapture::Log("hit th{:02} attack={} kind={}",
        tracker.ActiveKey().game, tracker.ActiveKey().attack, tracker.ActiveKey().kind);
}
void BossCaptureSpellCaptured(uintptr_t owner) {
    if (!AddonEnabled(Addon::Capture)) return;
    if (!captureContext) return;
    const auto context = captureContext();
    auto& tracker = BossCapture::Instance();
    if (context.version != 8 || context.replay || !tracker.Storage().Enabled()
        || !tracker.Active() || tracker.ActiveKey().kind != BossCapture::kSpell
        || !owner || captureOwner != owner) return;
    // This callback is inside TH08's successful native capture branch. It is
    // stronger evidence than generic HP callbacks, including internal phases.
    capturePending = true;
    capturePendingFailed = false;
    capturePendingOutcome = BossCapture::kCaptured;
    capturePendingFrame = captureFrameNumber;
    CaptureConfirm();
    captureSettlements.back().nativeCaptured = true;
    captureConfigured.erase(owner); // do not restart the completed spell at Frame()
    BossCapture::Log("native-capture th08 attack={}", captureSettlements.back().key.attack);
}
void BossCaptureTh07Stage6Opening(uintptr_t instruction) {
    if (!AddonEnabled(Addon::Capture) || !captureContext || !instruction) return;
    const auto c = captureContext();
    if (c.version != 7 || c.stage != 5 || c.replay || !c.gameplay
        || c.difficulty < 0 || c.difficulty > 3 || c.shot < 0 || c.shot > 5) return;

    const auto base = CaptureRead<uintptr_t>(c.fileTable);
    if (!base || instruction < base) return;
    const auto offset = instruction - base;
    // ecldata6.ecl timeline: full opening at frame 720, Chaos practice at
    // frame 1180, and the stage's automatic clear on message 2 at frame 1914.
    const bool fullStart = offset == 0xe050 && CaptureRead<uint32_t>(instruction) == 0x000b02d0;
    const bool practiceStart = c.practiceSection == TH07::TH07_ST6_CHAOS
        && offset == 0xe150 && CaptureRead<uint32_t>(instruction) == 0x000b049c;
    const bool starts = (fullStart || practiceStart)
        && CaptureRead<uint32_t>(instruction + 4) == 0x00200000;
    const bool ends = offset == 0xe630 && CaptureRead<uint32_t>(instruction) == 0x0002077a
        && CaptureRead<uint32_t>(instruction + 4) == 0x00080008;
    if (!starts && !ends) return;

    auto& tracker = BossCapture::Instance();
    tracker.EnsureLoaded();
    if (!tracker.Storage().Enabled()) return;
    CaptureSetContext(c);
    if (starts) {
        if (tracker.BeginAttack(6, TH07::TH07_ST6_CHAOS, BossCapture::kNonSpell, {})) {
            captureOwner = 0;
            tracker.Flush();
            BossCapture::Log("activate th07 stage=6 opening diff={} shot={} shortcut={}",
                c.difficulty, c.shot, practiceStart);
        }
    } else if (tracker.Active() && CaptureIsTh07Stage6Opening(tracker.ActiveKey())) {
        const bool captured = tracker.Settle(BossCapture::kCaptured);
        tracker.Flush();
        BossCapture::Log("settle th07 stage=6 opening diff={} shot={} captured={}", c.difficulty, c.shot, captured);
    }
}
void BossCaptureTh12Road(BossCaptureTh12RoadEvent event) {
    using Event = BossCaptureTh12RoadEvent;
    if (!AddonEnabled(Addon::Capture) || !captureContext)
        return;
    const auto c = captureContext();
    if (c.version != 12 || (c.stage != 3 && c.stage != 4) || c.replay
        || (!c.gameplay && event != Event::PracticeExit)
        || c.difficulty < 0 || c.difficulty > 3 || c.shot < 0 || c.shot > 5)
        return;

    auto& tracker = BossCapture::Instance();
    if (!tracker.Storage().Enabled())
        return;
    CaptureSetContext(c);
    if (event == Event::Start) {
        CaptureConfirm();
        if (tracker.BeginAttack(c.stage + 1, kTh12LatterChapterAttack, BossCapture::kNonSpell, {})) {
            captureTh12Checkpoint = false;
            captureOwner = 0;
            captureConfigured.clear();
            tracker.Flush();
            BossCapture::Log("th12 latter chapter started stage={} diff={} shot={}", c.stage + 1, c.difficulty, c.shot);
        }
        return;
    }
    if (!tracker.Active() || !CaptureIsTh12Road(tracker.ActiveKey())
        || tracker.ActiveKey().stage != c.stage + 1)
        return;
    if (event == Event::Hit) {
        tracker.MarkFailure();
        return;
    }
    if (event == Event::Checkpoint) {
        if (!captureTh12Checkpoint)
            BossCapture::Log("th12 latter chapter checkpoint stage={}", c.stage + 1);
        captureTh12Checkpoint = true;
        return;
    }
    if (!captureTh12Checkpoint)
        return;
    const bool captured = tracker.Settle(BossCapture::kCaptured);
    captureTh12Checkpoint = false;
    tracker.Flush();
    BossCapture::Log("th12 latter chapter settled stage={} practice_exit={} captured={}",
        c.stage + 1, event == Event::PracticeExit, captured);
}

BossCaptureMomijiState BossCaptureTh10MomijiStatus() {
    if (!AddonEnabled(Addon::Capture))
        return BossCaptureMomijiState::Inactive;
    const auto& tracker = BossCapture::Instance();
    if (!tracker.Storage().Enabled() || !tracker.Active() || !CaptureIsTh10Momiji(tracker.ActiveKey()))
        return BossCaptureMomijiState::Inactive;
    return captureTh10MomijiDefeated ? BossCaptureMomijiState::Reinforcements : BossCaptureMomijiState::Fighting;
}

void BossCaptureTh10MomijiFinish(bool practiceExit) {
    if (!captureContext || BossCaptureTh10MomijiStatus() != BossCaptureMomijiState::Reinforcements)
        return;
    const auto c = captureContext();
    if (c.version != 10 || c.stage != 3 || c.replay || (!practiceExit && !c.gameplay))
        return;
    CaptureSetContext(c);
    auto& tracker = BossCapture::Instance();
    if (!CaptureTh10MomijiWaiting(tracker)) return;
    const bool captured = tracker.Settle(BossCapture::kCaptured);
    captureTh10MomijiDefeated = false;
    tracker.Flush();
    BossCapture::Log("th10 Momiji settled diff={} shot={} manual={} captured={}",
        c.difficulty, c.shot, practiceExit, captured);
}

void BossCaptureCancelTracking() {
    if (!AddonEnabled(Addon::Capture)) return;
    captureTh10MomijiDefeated = false;
    captureTh12Checkpoint = false;
    BossCapture::Instance().AbortAttack();
    captureConfigured.clear(); captureSettlements.clear();
    captureOwner = 0; capturePending = false;
}
void BossCaptureNewRun() {
    if (!AddonEnabled(Addon::Capture)) return;
    CaptureFlush();
    BossCapture::Instance().Flush();
    BossCapture::Instance().AbortAttack();
    captureConfigured.clear(); captureFileIdentity.clear(); captureOwner = 0; captureLastSection = 0;
    capturePending = false; captureFailures = 0;
    captureTh10MomijiDefeated = false;
    captureTh12Checkpoint = false;
}
void BossCaptureFrame() {
    if (!AddonEnabled(Addon::Capture)) return;
    if (!captureContext) return;
    const auto c = captureContext();
    auto& tracker = BossCapture::Instance();
    CaptureSetContext(c);
    const bool enabled = tracker.Storage().Enabled();
    const bool stage6Opening = tracker.Active() && CaptureIsTh07Stage6Opening(tracker.ActiveKey());
    const bool momijiWaiting = CaptureTh10MomijiWaiting(tracker);
    const bool th12Road = tracker.Active() && CaptureIsTh12Road(tracker.ActiveKey());
    if (c.replay || !c.gameplay || !enabled || enabled != captureWasEnabled
        || (stage6Opening && c.stage != 5) || (momijiWaiting && c.stage != 3)
        || (th12Road && c.stage + 1 != tracker.ActiveKey().stage)) {
        CaptureFlush(); tracker.AbortAttack(); captureConfigured.clear();
        captureTh10MomijiDefeated = false;
        captureTh12Checkpoint = false;
        capturePending = false; captureOwner = 0;
    } else {
        const auto primary = CapturePrimary(c);
        capturePrimaryHint = primary;
        // TH08 has no proven runtime trace yet: one line every ~5 s is enough to
        // tell "no boss" from "boss without a catalog hit".
        static uint64_t lastHeartbeat = 0;
        if (c.version == 8 && captureFrameNumber - lastHeartbeat > 300) {
            lastHeartbeat = captureFrameNumber;
            const auto instruction = primary ? BossCaptureCurrentInstruction(primary) : 0;
            BossCapture::Log("tick th08 stage={} diff={} shot={} primary={:#x} instr={:#x} active={} attack={}",
                c.stage, c.difficulty, c.shot, primary, instruction, tracker.Active(),
                tracker.Active() ? tracker.ActiveKey().attack : 0);
        }
        if (tracker.Active() && !stage6Opening && !momijiWaiting && !th12Road && primary != captureOwner) {
            const auto previousOwner = captureOwner;
            if (capturePending) CaptureConfirm();
            else tracker.AbortAttack();
            captureConfigured.erase(previousOwner);
            capturePending = false; captureOwner = 0;
        }
        // TH06-08 keep their attacks in child threads: the owner's flat
        // instruction field tracks the main script, not the attack, so the
        // per-frame observer is only meaningful for the TH10+ engines. The old
        // engines are identified from their timer/spell/boss-mode opcodes.
        if (primary && c.version >= 10) BossCaptureObservedCurrent(primary);
        const auto configured = captureConfigured.find(primary);
        if (primary && configured != captureConfigured.end()
            && (!tracker.Active() || !(tracker.ActiveKey() == configured->second.key)))
            CaptureActivate(primary,configured->second);
        CaptureFlush();
    }
    captureWasEnabled = enabled;
    tracker.Tick();
    ++captureFrameNumber;
}
std::vector<BossCaptureCatalogRow> BossCaptureCatalogRows(int game, int difficulty, int shot) {
    std::vector<BossCaptureCatalogRow> rows;
    if (game == 7 && difficulty <= 3 && shot <= 5)
        rows.push_back({ 6, TH07::TH07_ST6_CHAOS, BossCapture::kNonSpell });
    if (game == 12 && difficulty <= 3 && shot <= 5) {
        rows.push_back({ 4, kTh12LatterChapterAttack, BossCapture::kNonSpell });
        rows.push_back({ 5, kTh12LatterChapterAttack, BossCapture::kNonSpell });
    }
    std::set<std::tuple<int, int, int>> seen;
    const int lookupDifficulty = game == 13 && difficulty == 5 ? 3 : difficulty;
    for (const auto& span : captureSpans) {
        const auto& file = captureFiles[span.file];
        if (file.version != game) continue;
        if (!BossCaptureStageMatchesDifficulty(game, file.stage + 1, difficulty)) continue;
        if (game == 8 && difficulty >= 0) {
            const bool lastWord = span.section >= TH08::TH08_LW_1;
            if ((difficulty == 7) != lastWord) continue;
        }
        if (lookupDifficulty >= 0 && (lookupDifficulty > 9 || !(span.difficultyMask & (1u << lookupDifficulty)))) continue;
        if (span.variant >= 16 && shot >= 0 && (game != 6 || shot != span.variant - 16)) continue;
        const BossCaptureCatalogRow row {
            CaptureStageGroup(game, span.section, file.stage + 1), span.section, span.kind };
        if (seen.emplace(row.stage, row.attack, row.kind).second) rows.push_back(row);
    }
    return rows;
}
void BossCaptureOptions() { BossCapture::DrawSettingsSection(); }
