#include "thprac_boss_capture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

namespace THPrac {
namespace BossCapture {
namespace { bool nativeBridgeActive = false; }
bool NativeBridgeActive() { return nativeBridgeActive; }
void SetNativeBridgeActive(bool value) { nativeBridgeActive = value; }
namespace {

constexpr char kMagic[8] = { 'T', 'H', 'P', 'B', 'C', 'P', '1', '\0' };
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kFlagEnabled = 1u << 0;
constexpr uint32_t kFlagHud = 1u << 1;
constexpr size_t kHeaderSize = 24; // magic, version, flags, count, checksum
constexpr size_t kChecksumOffset = 20;
constexpr size_t kRecordSize = 36;
constexpr uint32_t kMaxRecords = 100000;
constexpr uint64_t kSaveIntervalMs = 3000;

uint32_t Fnv1a(const uint8_t* data, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

template<typename T>
void Put(std::vector<uint8_t>& bytes, size_t offset, T value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template<typename T>
T Get(const std::vector<uint8_t>& bytes, size_t offset)
{
    T value {};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::wstring ModuleDirectory()
{
    wchar_t buffer[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return {};
    std::wstring path(buffer, length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    return path.substr(0, slash + 1);
}

void CreateParentDirectory(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return;
    const std::wstring directory = path.substr(0, slash);
    if (GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    CreateDirectoryW(directory.c_str(), nullptr);
}

} // namespace

bool Key::Valid() const
{
    return game > 0 && game < 100 && stage >= kGroupedStage && stage <= 99
        && attack >= 1 && attack <= 999 && (kind == kNonSpell || kind == kSpell)
        && difficulty >= 0 && difficulty <= 9 && shot >= 0 && shot <= 63;
}

std::wstring Store::DefaultPath()
{
    // Same portable rule as the launcher configuration: a .thprac_data folder
    // next to the executable wins over %APPDATA%\thprac.
    const std::wstring moduleDirectory = ModuleDirectory();
    if (!moduleDirectory.empty()
        && GetFileAttributesW((moduleDirectory + L".thprac_data").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return moduleDirectory + L".thprac_data\\boss_capture_v2.dat";
    }
    wchar_t appData[MAX_PATH] {};
    if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData) == S_OK)
        return std::wstring(appData) + L"\\thprac\\boss_capture_v2.dat";
    return moduleDirectory + L"boss_capture_v2.dat";
}

Store::Store(std::wstring filePath)
    : path(std::move(filePath))
{
    if (path.empty())
        path = DefaultPath();
}

bool Store::Load()
{
    loaded = true;
    corrupt = false;
    total.clear();
    session.clear();

    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open())
        return false;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>());
    file.close();

    auto markCorrupt = [this]() {
        corrupt = true;
        if (!path.empty()) {
            const std::wstring rejected = path + L".corrupt";
            MoveFileExW(path.c_str(), rejected.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return false;
    };

    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
        return markCorrupt();
    const uint32_t version = Get<uint32_t>(bytes, 8);
    const uint32_t flags = Get<uint32_t>(bytes, 12);
    const uint32_t count = Get<uint32_t>(bytes, 16);
    if (version != kFormatVersion || count > kMaxRecords
        || bytes.size() != kHeaderSize + size_t(count) * kRecordSize)
        return markCorrupt();

    std::vector<uint8_t> payload = bytes;
    const uint32_t storedChecksum = Get<uint32_t>(payload, kChecksumOffset);
    Put<uint32_t>(payload, kChecksumOffset, 0);
    if (Fnv1a(payload.data(), payload.size()) != storedChecksum)
        return markCorrupt();

    for (uint32_t i = 0; i < count; ++i) {
        const size_t offset = kHeaderSize + size_t(i) * kRecordSize;
        Key key;
        key.game = Get<int32_t>(bytes, offset + 0);
        key.stage = Get<int32_t>(bytes, offset + 4);
        key.attack = Get<int32_t>(bytes, offset + 8);
        key.kind = Get<int32_t>(bytes, offset + 12);
        key.difficulty = Get<int32_t>(bytes, offset + 16);
        key.shot = Get<int32_t>(bytes, offset + 20);
        Counts counts;
        counts.attempts = Get<uint32_t>(bytes, offset + 24);
        counts.captures = Get<uint32_t>(bytes, offset + 28);
        counts.timeouts = Get<uint32_t>(bytes, offset + 32);
        if (!key.Valid() || counts.captures > counts.attempts)
            return markCorrupt();
        total[key] = counts;
    }

    enabled = (flags & kFlagEnabled) != 0;
    showHud = (flags & kFlagHud) != 0;
    dirty = false;
    return true;
}

bool Store::Save()
{
    std::vector<uint8_t> bytes(kHeaderSize + size_t(total.size()) * kRecordSize, 0);
    std::memcpy(bytes.data(), kMagic, sizeof(kMagic));
    Put<uint32_t>(bytes, 8, kFormatVersion);
    Put<uint32_t>(bytes, 12, (enabled ? kFlagEnabled : 0u) | (showHud ? kFlagHud : 0u));
    Put<uint32_t>(bytes, 16, uint32_t(total.size()));
    Put<uint32_t>(bytes, kChecksumOffset, 0u);
    size_t offset = kHeaderSize;
    for (const auto& entry : total) {
        Put<int32_t>(bytes, offset + 0, entry.first.game);
        Put<int32_t>(bytes, offset + 4, entry.first.stage);
        Put<int32_t>(bytes, offset + 8, entry.first.attack);
        Put<int32_t>(bytes, offset + 12, entry.first.kind);
        Put<int32_t>(bytes, offset + 16, entry.first.difficulty);
        Put<int32_t>(bytes, offset + 20, entry.first.shot);
        Put<uint32_t>(bytes, offset + 24, entry.second.attempts);
        Put<uint32_t>(bytes, offset + 28, entry.second.captures);
        Put<uint32_t>(bytes, offset + 32, entry.second.timeouts);
        offset += kRecordSize;
    }
    // The checksum covers the whole buffer with the checksum field zeroed.
    const uint32_t checksum = Fnv1a(bytes.data(), bytes.size());
    Put<uint32_t>(bytes, kChecksumOffset, checksum);

    CreateParentDirectory(path);
    const std::wstring temporary = path + L".tmp";
    std::ofstream file(temporary, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open())
        return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    const bool written = file.good();
    file.close();
    if (!written || file.fail()) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    dirty = false;
    return true;
}

void Store::ClearSession()
{
    if (session.empty())
        return;
    session.clear();
}

void Store::ClearTotal()
{
    total.clear();
    session.clear();
    dirty = true;
}

void Store::ClearAll()
{
    total.clear();
    session.clear();
    dirty = true;
}

void Store::SetEnabled(bool value)
{
    if (enabled == value)
        return;
    enabled = value;
    dirty = true;
}

void Store::SetShowHud(bool value)
{
    if (showHud == value)
        return;
    showHud = value;
    dirty = true;
}

bool Store::RecordAttempt(const Key& key)
{
    if (!enabled)
        return false;
    ++total[key].attempts;
    ++session[key].attempts;
    dirty = true;
    return true;
}

bool Store::RecordCapture(const Key& key)
{
    if (!enabled)
        return false;
    ++total[key].captures;
    ++session[key].captures;
    dirty = true;
    return true;
}

bool Store::RecordTimeout(const Key& key)
{
    if (!enabled)
        return false;
    ++total[key].timeouts;
    ++session[key].timeouts;
    dirty = true;
    return true;
}

bool Store::RecordCompleted(const Key& key, uint32_t captures, uint32_t failures)
{
    if (!enabled || (!captures && !failures))
        return false;
    const uint32_t attempts = captures + failures;
    total[key].attempts += attempts;
    total[key].captures += captures;
    total[key].timeouts += failures;
    session[key].attempts += attempts;
    session[key].captures += captures;
    session[key].timeouts += failures;
    dirty = true;
    return true;
}

Runtime::Runtime(std::wstring storePath)
    : store(std::move(storePath))
{
}

void Runtime::SetContext(const Context& value)
{
    if (context == value)
        return;
    // A difficulty change, a switch to replay or a game change in the middle of
    // an attack means the attempt cannot be attributed any more.
    if (active)
        CancelActive();
    context = value;
}

void Runtime::EnsureLoaded()
{
    if (!store.Loaded())
        store.Load();
}

bool Runtime::BeginAttack(int stage, int attack, int kind, const std::string& name)
{
    EnsureLoaded();
    if (!store.Enabled() || context.replay || context.game <= 0) {
        CancelActive();
        return false;
    }
    Key key;
    key.game = context.game;
    key.stage = stage;
    key.attack = attack;
    key.kind = kind;
    key.difficulty = context.difficulty;
    key.shot = context.shot;
    if (!key.Valid())
        return false;
    if (active && !settled && activeKey == key)
        return false; // same attack seen again by a polling adapter
    CancelActive();
    activeKey = key;
    active = true;
    failed = false;
    settled = false;
    if (!name.empty())
        names[key] = name;
    store.RecordAttempt(key);
    return true;
}

void Runtime::MarkFailure()
{
    if (active && !settled)
        failed = true;
}

bool Runtime::Settle(Outcome outcome)
{
    if (!active || settled)
        return false; // duplicate settlement protection
    settled = true;
    const Key key = activeKey;
    active = false;
    const bool captured = outcome == kCaptured && !failed;
    if (captured)
        store.RecordCapture(key);
    else if (outcome == kTimedOut)
        store.RecordTimeout(key);
    failed = false;
    return captured;
}

void Runtime::AbortAttack()
{
    if (active && !settled)
        CancelActive();
}

bool Runtime::RecordCompleted(int stage, int attack, int kind, uint32_t captures,
    uint32_t failures, const std::string& name)
{
    EnsureLoaded();
    if (!store.Enabled() || context.replay || context.game <= 0 || (!captures && !failures))
        return false;
    Key key;
    key.game = context.game;
    key.stage = stage;
    key.attack = attack;
    key.kind = kind;
    key.difficulty = context.difficulty;
    key.shot = context.shot;
    if (!key.Valid())
        return false;
    if (!name.empty())
        names[key] = name;
    return store.RecordCompleted(key, captures, failures);
}

void Runtime::CancelActive()
{
    active = false;
    failed = false;
    settled = false;
}

void Runtime::Tick()
{
    EnsureLoaded();
    if (active && !store.Enabled())
        CancelActive();
    if (!store.Dirty())
        return;
    const uint64_t now = GetTickCount64();
    if (lastSaveMs && now - lastSaveMs < kSaveIntervalMs)
        return;
    lastSaveMs = now;
    store.Save();
}

void Runtime::Flush()
{
    EnsureLoaded();
    if (!store.Dirty()) return;
    lastSaveMs = GetTickCount64();
    store.Save();
}

double Runtime::Rate(const Counts& counts)
{
    return counts.attempts ? 100.0 * counts.captures / counts.attempts : 0.0;
}

Runtime& Instance()
{
    static Runtime runtime;
    return runtime;
}

void SetContext(const Context& context)
{
    if (!NativeBridgeActive()) Instance().SetContext(context);
}

bool BeginAttack(int stage, int attack, int kind, const std::string& name)
{
    return !NativeBridgeActive() && Instance().BeginAttack(stage, attack, kind, name);
}

void MarkFailure()
{
    if (!NativeBridgeActive()) Instance().MarkFailure();
}

bool Settle(Outcome outcome)
{
    return !NativeBridgeActive() && Instance().Settle(outcome);
}

void AbortAttack()
{
    if (!NativeBridgeActive()) Instance().AbortAttack();
}

bool RecordCompleted(int stage, int attack, int kind, uint32_t captures,
    uint32_t failures, const std::string& name)
{
    return !NativeBridgeActive() && Instance().RecordCompleted(stage, attack, kind, captures, failures, name);
}

void AppendLog(const std::string& line)
{
    static std::wstring path;
    if (path.empty()) {
        path = Store::DefaultPath();
        const auto slash = path.find_last_of(L"\\/");
        path = (slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1))
            + L"boss_capture.log";
    }
    constexpr unsigned long long kLimit = 256ull * 1024ull;
    WIN32_FILE_ATTRIBUTE_DATA info {};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)
        && (unsigned long long(info.nFileSizeHigh) << 32 | info.nFileSizeLow) > kLimit) {
        const std::wstring rotated = path + L".old";
        DeleteFileW(rotated.c_str());
        MoveFileExW(path.c_str(), rotated.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    std::ofstream file(path, std::ios::out | std::ios::app | std::ios::binary);
    if (!file.is_open())
        return;
    SYSTEMTIME time {};
    GetLocalTime(&time);
    file << std::format("{:02}:{:02}:{:02}.{:03} {}", time.wHour, time.wMinute, time.wSecond,
        time.wMilliseconds, line) << '\n';
}

}
}
