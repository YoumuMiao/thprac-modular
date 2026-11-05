#include "capture_service.h"
#include "feature_runtime.h"

namespace THPrac::BossCapture {
namespace {
const CaptureService* service;

const CaptureService* ActiveService()
{
    return AddonEnabled(Addon::Capture) ? service : nullptr;
}
}

void SetCaptureService(const void* raw)
{
    const auto* incoming = static_cast<const CaptureService*>(raw);
    if (incoming && incoming->feature.size >= sizeof(*incoming)) service = incoming;
}

Store::Store(std::wstring pathValue) : path(std::move(pathValue)), enabled(false), showHud(false) {}
Runtime::Runtime(std::wstring pathValue) : store(std::move(pathValue)) {}

Runtime& Instance()
{
    if (const auto* capture = ActiveService()) return capture->instance();
    static Runtime disabled {L""};
    return disabled;
}

bool NativeBridgeActive() { return ActiveService() && service->nativeActive(); }
void SetNativeBridgeActive(bool value) { if (ActiveService()) service->setNative(value); }
void Runtime::SetContext(const Context& value) { if (ActiveService()) service->setContext(*this, value); }
void Runtime::EnsureLoaded() { if (ActiveService()) service->load(*this); }
bool Runtime::BeginAttack(int stage, int attack, int kind, const std::string& name)
{
    return ActiveService() && service->begin(*this, stage, attack, kind, name);
}
void Runtime::MarkFailure() { if (ActiveService()) service->fail(*this); }
bool Runtime::Settle(Outcome value) { return ActiveService() && service->settle(*this, value); }
void Runtime::AbortAttack() { if (ActiveService()) service->abort(*this); }
bool Runtime::RecordCompleted(int stage, int attack, int kind, uint32_t captures, uint32_t failures, const std::string& name)
{
    return ActiveService() && service->record(*this, stage, attack, kind, captures, failures, name);
}
void Runtime::Tick() { if (ActiveService()) service->tick(*this); }
void Runtime::Flush() { if (ActiveService()) service->flush(*this); }
double Runtime::Rate(const Counts& counts)
{
    return counts.attempts ? 100.0 * counts.captures / counts.attempts : 0.0;
}
void Store::SetEnabled(bool value) { if (ActiveService()) service->enable(*this, value); }
void Store::SetShowHud(bool value) { if (ActiveService()) service->showHud(*this, value); }
void Store::ClearSession() { if (ActiveService()) service->clearSession(*this); }
void Store::ClearTotal() { if (ActiveService()) service->clearTotal(*this); }
void Store::ClearAll() { if (ActiveService()) service->clearAll(*this); }
bool Key::Valid() const { return ActiveService() && service->valid(*this); }
bool Store::Save() { return ActiveService() && service->save(*this); }
bool Store::RecordCapture(const Key& key) { return ActiveService() && service->capture(*this, key); }
bool Store::RecordTimeout(const Key& key) { return ActiveService() && service->timeout(*this, key); }
void AppendLog(const std::string& line) { if (ActiveService()) service->log(line); }
void SetContext(const Context& value) { Instance().SetContext(value); }
bool BeginAttack(int stage, int attack, int kind, const std::string& name) { return Instance().BeginAttack(stage, attack, kind, name); }
void MarkFailure() { Instance().MarkFailure(); }
bool Settle(Outcome value) { return Instance().Settle(value); }
void AbortAttack() { Instance().AbortAttack(); }
bool RecordCompleted(int stage, int attack, int kind, uint32_t captures, uint32_t failures, const std::string& name)
{
    return Instance().RecordCompleted(stage, attack, kind, captures, failures, name);
}
}
