#include "quick_practice_adapter.h"

namespace THPrac::QuickPractice {
namespace {
const Request* preview;
const Request* launch;
const Service* service;
HookCtx hooks[HookCount];

void __fastcall Enter(PCONTEXT context, HookCtx* hook)
{
    if (service->active() && (!ImGui::GetCurrentContext() || !service->ready(launch, context))) return;
    hook->Disable();
    if (service->update(Status::Committed)) service->enter(launch, context, Title);
}

void __fastcall Continue(PCONTEXT context, HookCtx* hook)
{
    hook->Disable();
    const auto step = hook == &hooks[Display] ? Display : Confirm;
    if (service->active()) service->enter(launch, context, step);
}
}

const Request* Preview() { return preview; }
bool Active() { return launch && service->active(); }

PreviewScope::PreviewScope(const EditorContext& context, const Request& request)
    : previousContext(ImGui::GetCurrentContext()), previousLocale(Gui::LocaleGet())
{
    ImGui::GetAllocatorFunctions(&previousAlloc, &previousFree, &previousAllocContext);
    ImGui::SetAllocatorFunctions(context.alloc, context.free, context.allocContext);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context.imgui));
    Gui::__glocale_current = static_cast<Gui::locale_t>(context.locale);
    Gui::GuiNavFocus::GlobalDisable(true);
    preview = &request;
}

PreviewScope::~PreviewScope()
{
    preview = nullptr;
    Gui::GuiNavFocus::GlobalDisable(false);
    Gui::__glocale_current = previousLocale;
    ImGui::SetCurrentContext(previousContext);
    ImGui::SetAllocatorFunctions(previousAlloc, previousFree, previousAllocContext);
}

bool Claim(const ThpracGameHost& host, const char* game)
{
    const auto* extension = host.find_extension(pluginId);
    const auto* candidate = extension ? static_cast<const Service*>(extension->service) : nullptr;
    if (!candidate || candidate->size != sizeof(Service)) return false;
    launch = candidate->claim(game);
    if (!launch) return false;
    service = candidate;
    return true;
}

void Install()
{
    if (!launch || !service->active()) return;
    const auto image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const EntryPoint* entries[HookCount] {};
    for (unsigned step = 0; step < HookCount; ++step) {
        const auto* entry = entries[step] = service->entry(launch, step);
        if (entry && (!entry->length || entry->length > sizeof(entry->expected)
            || std::memcmp(reinterpret_cast<const void*>(image + entry->address), entry->expected, entry->length) != 0)) {
            service->update(Status::Rejected);
            return;
        }
    }
    for (unsigned step = 0; step < HookCount; ++step) {
        if (!entries[step]) continue;
        const auto& entry = *entries[step];
        auto& hook = hooks[step];
        hook.addr = image + entry.address - ingame_image_base;
        hook.name = "quick_practice_launch";
        hook.callback = step == Title ? Enter : Continue;
        hook.data = PatchHookImpl(static_cast<uint8_t>(entry.length));
        hook.Setup();
        hook.Enable();
    }
    if (Active()) service->complete_display(launch);
}

const Request* PendingParameters()
{
    return Active() && hooks[Title].setup && !hooks[Title].enabled ? launch : nullptr;
}

bool Apply(void* parameters, uint32_t size)
{
    const auto* request = PendingParameters();
    if (!request) return false;
    if (size != request->bytes) {
        service->update(Status::Rejected);
        return false;
    }
    std::memcpy(parameters, request->parameters, size);
    service->update(Status::Applied);
    return true;
}
}
