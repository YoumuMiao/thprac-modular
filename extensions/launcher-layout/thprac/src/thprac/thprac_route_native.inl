// Native clocks and ECL locations for replay routes. Included after the native
// capture adapter, so file identities and safe reads have one implementation.
// No selected practice target or player movement is used as timing evidence.
namespace {
struct RouteNativeFunction { unsigned begin, end; };
struct RouteNativeSpan { unsigned begin, end; int section, variant; unsigned mask; };
struct RouteNativeFile {
    size_t index;
    uintptr_t base;
    std::vector<RouteNativeFunction> roads;
    std::vector<RouteNativeSpan> attacks;
};
std::vector<RouteNativeFile> routeNativeFiles;
std::vector<std::pair<size_t, uintptr_t>> routeNativeObservedFiles;
int routeNativeGame = 0, routeNativeStage = -1;
uintptr_t routeNativeTable = 0;
bool routeNativeReady = false;

bool RouteNativeCopy(uintptr_t address, void* destination, size_t length)
{
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !length || !VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))
        || region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        || address + length < address
        || address + length > reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize)
        return false;
    memcpy(destination, reinterpret_cast<const void*>(address), length);
    return true;
}

void RouteNativeRefreshFiles(const BossCaptureContext& c)
{
    bool same = routeNativeReady && routeNativeGame == c.version && routeNativeStage == c.stage
        && routeNativeTable == c.fileTable;
    if (same) for (const auto& item : routeNativeObservedFiles) {
        if (CaptureRead<uintptr_t>(c.fileTable + captureFiles[item.first].ordinal * 4) != item.second) {
            same = false;
            break;
        }
    }
    if (same) return;
    routeNativeGame = c.version; routeNativeStage = c.stage; routeNativeTable = c.fileTable;
    routeNativeReady = true;
    routeNativeFiles.clear(); routeNativeObservedFiles.clear();
    for (size_t i = 0; i < _countof(captureFiles); ++i) {
        const auto& f = captureFiles[i];
        if (f.version != c.version || f.stage != c.stage) continue;
        const auto base = CaptureRead<uintptr_t>(c.fileTable + f.ordinal * 4);
        routeNativeObservedFiles.emplace_back(i, base);
        if (!base || f.headerSize < 16 || f.headerSize > 0x100000) continue;
        // Validate/copy the immutable header once, not one VirtualQuery per byte.
        std::vector<uint8_t> header(f.headerSize);
        if (!RouteNativeCopy(base, header.data(), header.size())) continue;
        uint32_t hash = 2166136261u;
        for (auto byte : header) hash = (hash ^ byte) * 16777619u;
        if (hash != f.headerHash) continue; // Unknown ECL revisions fail closed.
        RouteNativeFile file{i, base, {}, {}};
        if (c.version >= 10) {
            const auto u32 = [&](unsigned at) { uint32_t value{};
                if (at <= header.size() - 4) memcpy(&value, header.data() + at, 4);
                return value; };
            const unsigned count = u32(16);
            const unsigned table = 36 + header[6] + (static_cast<unsigned>(header[7]) << 8);
            if (count > 8192 || table + count * 4 >= f.headerSize) continue;
            unsigned name = table + count * 4;
            for (unsigned sub = 0; sub < count; ++sub) {
                const unsigned begin = u32(table + sub * 4) + 16;
                const unsigned end = sub + 1 < count ? u32(table + (sub + 1) * 4)
                    : static_cast<unsigned>(f.size);
                const unsigned nameBegin = name;
                while (name < header.size() && header[name]) ++name;
                if (name >= header.size()) break;
                const bool main = name - nameBegin >= 4
                    && _strnicmp(reinterpret_cast<const char*>(header.data() + nameBegin), "main", 4) == 0;
                ++name;
                if (main && begin < end && end <= f.size) file.roads.push_back({begin, end});
            }
        }
        // This file's canonical spans are cached once per new run/file identity.
        // Per-frame lookup never walks the global catalog or rehashes a header.
        for (const auto& span : captureSpans)
            if (span.file == i) file.attacks.push_back({static_cast<unsigned>(span.begin),
                static_cast<unsigned>(span.end), span.section, span.variant, span.difficultyMask});
        routeNativeFiles.push_back(std::move(file));
    }
}

// Resolve a live IP to immutable file-relative coordinates. Main* controllers
// and canonical attack roots are the only accepted sources, never helper enemies.
bool RouteNativeStamp(const BossCaptureContext& c, uintptr_t instruction, int time,
    RouteSync::ScriptStamp& stamp, int& section)
{
    const int difficulty = c.version == 13 && c.difficulty == 5 ? 3 : c.difficulty;
    for (const auto& file : routeNativeFiles) {
        const auto& meta = captureFiles[file.index];
        if (instruction < file.base || instruction - file.base >= meta.size) continue;
        const auto offset = static_cast<unsigned>(instruction - file.base);
        if (offset >= 0xffffff || meta.ordinal >= 127) return false;
        section = 0;
        if (difficulty >= 0 && difficulty <= 9) for (const auto& span : file.attacks) {
            if (offset < span.begin || offset >= span.end || !(span.mask & (1u << difficulty))
                || (span.variant && span.variant != c.phaseVariant)) continue;
            if (section && section != span.section) return false;
            section = span.section;
        }
        if (!section && std::none_of(file.roads.begin(), file.roads.end(),
                [&](const auto& f) { return offset >= f.begin && offset < f.end; })) return false;
        stamp = { (static_cast<uint32_t>(meta.ordinal + 1) << 24) | (offset + 1), time };
        return true;
    }
    return false;
}

bool RouteNativeEnemyStamp(const BossCaptureContext& c, uintptr_t owner,
    RouteSync::ScriptStamp& stamp, int& section)
{
    if (!owner) return false;
    uintptr_t instruction = 0;
    int time = 0;
    if (c.version < 10) {
        const unsigned context = c.version == 6 ? 0x990 : c.version == 7 ? 0x6e4 : 0x7f8;
        struct OldContext { uintptr_t instruction; int previous; float subframe; int current; };
        const auto header = CaptureRead<OldContext>(owner + context);
        instruction = header.instruction; time = header.current;
    } else {
        // Copy the primary context header once, not a transient async context
        // left by the last helper. The native VM layout changed at TH15.
        struct Context { float time; uint32_t location; int offset; };
        const auto header = CaptureRead<Context>(owner + (c.version <= 14 ? 8 : 0x10));
        if (!std::isfinite(header.time) || header.time < -1000000.0f || header.time > 1000000.0f)
            return false;
        time = static_cast<int>(std::lround(header.time * 256.0f));
        if (c.version <= 14) instruction = header.location;
        else {
            const int sub = static_cast<int>(header.location), offset = header.offset;
            if (sub < 0 || sub > 8192 || offset < 0 || offset > 0x1000000) return false;
            const auto manager = CaptureRead<uintptr_t>(owner + (c.version == 20 ? 0x58
                : c.version == 18 ? 0x1218 : 0x11f8));
            const auto table = CaptureRead<uintptr_t>(manager + (c.version == 20 ? 0x210 : 0x8c));
            const auto base = CaptureRead<uintptr_t>(table + sub * 8 + 4);
            instruction = base ? base + 16 + offset : 0;
        }
    }
    return instruction && RouteNativeStamp(c, instruction, time, stamp, section);
}
} // namespace

// Called by the shared NewRun hook before taking a new snapshot. Reauthenticate
// immutable headers even when the allocator reuses the same file addresses.
void RouteNativeInvalidate()
{
    routeNativeReady = false;
}

bool RouteReadNativeState(int game, RouteNativeState& state)
{
    state.frame = -1; state.segment = 0; state.generation = 0; state.scripts.clear();
    if (!captureContext) return false;
    const auto c = captureContext();
    if (c.version != game || !c.gameplay || c.stage < 0 || !c.fileTable) return false;
    uintptr_t manager = 0, frameAddress = 0;
    unsigned listOffset = 0;
    const auto rva = [](uintptr_t offset) { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + offset; };
    switch (game) {
    // TH06-08 have static managers; their bullet timers tick even when the
    // player is stationary. Their current integer follows previous/subframe.
    case 6: manager = 0x4b79c8; frameAddress = 0x5a5ff8 + 0xf5c08 + 8; break;
    case 7: manager = 0x9a9b00; frameAddress = 0x62f958 + 0x37a130 + 8; break;
    case 8: manager = 0x577f20; frameAddress = 0xf54e90 + 0x6ba540 + 8; break;
    // TH10 increment is at 0x418769..0x41877f, after all pause/menu exits.
    case 10: manager = CaptureRead<uintptr_t>(0x477704); frameAddress = 0x474c88; listOffset = 0x58; break;
    case 11: manager = CaptureRead<uintptr_t>(0x4a8d7c); frameAddress = 0x4a5734; listOffset = 0x68; break;
    case 12: manager = CaptureRead<uintptr_t>(0x4b43dc); frameAddress = 0x4b0cbc; listOffset = 0x68; break;
    case 13: manager = CaptureRead<uintptr_t>(0x4c2188); frameAddress = 0x4be828; listOffset = 0xb0; break;
    case 14: manager = CaptureRead<uintptr_t>(0x4db544); frameAddress = 0x4f58b0; listOffset = 0xd0; break;
    case 16: manager = CaptureRead<uintptr_t>(0x4a6dc0); frameAddress = 0x4a579c; listOffset = 0x180; break;
    case 17: manager = CaptureRead<uintptr_t>(0x4b76a0); frameAddress = 0x4b59e8; listOffset = 0x180; break;
    case 18: manager = CaptureRead<uintptr_t>(0x4cf2d0); frameAddress = 0x4ccce8; listOffset = 0x18c; break;
    // TH20 GameThread::OnTick 0x4ba4a0 advances this+0x10 at 0x4ba84c.
    // Timer20::current is +4. The timer does not advance through pause exits.
    case 20: manager = CaptureRead<uintptr_t>(rva(0x1ba570));
        frameAddress = CaptureRead<uintptr_t>(rva(0x1ba828));
        if (frameAddress) frameAddress += 0x14;
        listOffset = 0x108; break;
    default: return false;
    }
    if (!manager || !frameAddress) return false;
    state.frame = CaptureRead<int>(frameAddress);
    if (state.frame < 0) return false;
    state.generation = CaptureRead<uintptr_t>(c.fileTable);
    RouteNativeRefreshFiles(c);

    RouteSync::ScriptStamp bossStamp{};
    int bossSection = 0;
    uintptr_t bossOwner = 0;
    bool multipleBosses = false;
    const auto addOwner = [&](uintptr_t owner) {
        RouteSync::ScriptStamp stamp;
        int section = 0;
        if (!RouteNativeEnemyStamp(c, owner, stamp, section)) return;
        if (!section) { state.scripts.push_back(stamp); return; }
        if (!bossOwner) { bossOwner = owner; bossStamp = stamp; bossSection = section; }
        else if (bossOwner != owner) multipleBosses = true;
    };
    if (game < 10) {
        for (auto owner : c.bosses) if (owner) addOwner(owner);
        // The older timeline interpreter uses independent integer clocks.
        // ECLTimeWarp writes these exact counters when installing a road warp.
        if (game == 6 && !routeNativeFiles.empty())
            state.scripts.push_back({0xf0000001u, CaptureRead<int>(manager + 0xee5e8)});
        else if (game != 6 && !routeNativeFiles.empty()) {
            const unsigned timers = game == 7 ? 0x9545f4 : 0x9dcdd0;
            for (unsigned channel = 0; channel < 16; ++channel)
                state.scripts.push_back({0xf0000001u + channel,
                    CaptureRead<int>(manager + timers + channel * 0x10 + 8)});
        }
    } else {
        // TH20 owns an intrusive list root at +0x108 (constructor 0x4a2e80;
        // begin 0x412280 -> next at +4). Older titles store a head pointer.
        uintptr_t node = game == 20 ? CaptureRead<uintptr_t>(manager + listOffset + 4)
            : CaptureRead<uintptr_t>(manager + listOffset);
        const uintptr_t sentinel = game == 20 ? manager + listOffset : 0;
        for (unsigned count = 0; node && node != sentinel && count < 2048; ++count) {
            struct Node { uintptr_t owner, next; };
            const auto link = CaptureRead<Node>(node);
            if (link.owner) addOwner(link.owner);
            const auto next = link.next;
            if (next == node) break;
            node = next;
        }
    }
    // Multiple canonical bosses can be active (Prismriver etc). Without an
    // unambiguous primary attack, retain road evidence and do not pick one.
    if (bossOwner && !multipleBosses) {
        state.scripts.insert(state.scripts.begin(), bossStamp);
        state.segment = bossSection;
    }
    return true;
}
