#include "service.h"
#include "thprac_plugin_sdk.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace THPrac::QuickPractice {
const EntryPoint* __cdecl NativeEntry(const Request* request, unsigned step);
int __cdecl NativeMenuReady(const Request* request, const void* context);
void __cdecl EnterNativePractice(const Request* request, void* context, unsigned step);
void __cdecl CompleteDisplay(const Request* request);
int __cdecl SupportsGame(const char* game);

namespace {
struct SharedRequest {
    volatile LONG status;
    Request request;
};

struct Ticket {
    HANDLE mapping = nullptr;
    SharedRequest* view = nullptr;
    ~Ticket()
    {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
    }
};

Ticket gameTicket;
Request claimedRequest;

bool MappingName(HANDLE process, wchar_t (&name)[96])
{
    FILETIME creation, exit, kernel, user;
    const auto pid = GetProcessId(process);
    return pid && GetProcessTimes(process, &creation, &exit, &kernel, &user)
        && swprintf_s(name, L"Local\\THPrac.QuickPractice.%lu.%08lx%08lx",
            pid, creation.dwHighDateTime, creation.dwLowDateTime) > 0;
}

Status Read(const Ticket& ticket)
{
    return static_cast<Status>(InterlockedCompareExchange(&ticket.view->status, 0, 0));
}

bool Transition(const Ticket& ticket, Status from, Status to)
{
    return InterlockedCompareExchange(&ticket.view->status, static_cast<LONG>(to),
        static_cast<LONG>(from)) == static_cast<LONG>(from);
}

void* __cdecl Create(void* process, const Request* request)
{
    wchar_t name[96];
    if (!request || request->version != protocol || !memchr(request->game, 0, sizeof(request->game))
        || !NativeEntry(request, 0) || !request->bytes
        || request->bytes > sizeof(request->parameters) || !MappingName(process, name)) return nullptr;

    auto ticket = std::unique_ptr<Ticket>(new (std::nothrow) Ticket);
    if (!ticket) return nullptr;
    ticket->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(SharedRequest), name);
    if (!ticket->mapping || GetLastError() == ERROR_ALREADY_EXISTS) return nullptr;
    ticket->view = static_cast<SharedRequest*>(MapViewOfFile(ticket->mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedRequest)));
    if (!ticket->view) return nullptr;
    ticket->view->request = *request;
    return ticket.release();
}

Status __cdecl ReadStatus(void* ticket)
{
    return Read(*static_cast<Ticket*>(ticket));
}

void __cdecl Close(void* opaque)
{
    auto ticket = std::unique_ptr<Ticket>(static_cast<Ticket*>(opaque));
    if (ticket) {
        Transition(*ticket, Status::Pending, Status::Cancelled);
        Transition(*ticket, Status::Claimed, Status::Cancelled);
    }
}

const Request* __cdecl Claim(const char* game)
{
    if (gameTicket.mapping) return nullptr;
    wchar_t name[96];
    if (!MappingName(GetCurrentProcess(), name)) return nullptr;
    gameTicket.mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
    if (!gameTicket.mapping) return nullptr;
    gameTicket.view = static_cast<SharedRequest*>(MapViewOfFile(gameTicket.mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedRequest)));
    if (!gameTicket.view) return nullptr;
    const auto request = gameTicket.view->request;
    if (request.version != protocol || !memchr(request.game, 0, sizeof(request.game))
        || strcmp(game, request.game) != 0 || !request.bytes || request.bytes > sizeof(request.parameters)
        || !NativeEntry(&request, 0)) {
        Transition(gameTicket, Status::Pending, Status::Rejected);
        return nullptr;
    }
    if (!Transition(gameTicket, Status::Pending, Status::Claimed)) return nullptr;
    claimedRequest = request;
    return &claimedRequest;
}

int __cdecl Update(Status status)
{
    if (!gameTicket.view) return 0;
    if (status == Status::Applied) return Transition(gameTicket, Status::Committed, status);
    if (status == Status::Entered) return Transition(gameTicket, Status::Applied, status);
    if (status == Status::Rejected && Transition(gameTicket, Status::Committed, status)) return 1;
    return Transition(gameTicket, Status::Claimed, status);
}

int __cdecl Active()
{
    if (!gameTicket.view) return 0;
    const auto status = Read(gameTicket);
    return status == Status::Claimed || status == Status::Committed || status == Status::Applied;
}

void __cdecl GameEvent(const ThpracPluginGameEvent* event)
{
    if (event->kind != THPRAC_EVENT_STAGE_STARTED || !event->stage || event->stage->replay
        || event->stage->stage_index < 0) return;
    // Last Words report the stage whose ECL contains the spell.
    const bool lastWord = std::strcmp(claimedRequest.game, "th08") == 0 && claimedRequest.stage == 9;
    if (lastWord || event->stage->stage_index == claimedRequest.stage) Update(Status::Entered);
}

void __cdecl DrawHelp(const ThpracPluginUi* ui)
{
    const char* labels[] = {
        "在已添加的游戏详情页点击「快速练习」。",
        "Open Quick Practice from an installed game's details page.",
        "登録済み作品の詳細から「クイック練習」を開きます。"
    };
    ui->text(labels[ui->locale >= 0 && ui->locale < 3 ? ui->locale : 1]);
}

const Service service { sizeof(service), SupportsGame, Create, ReadStatus, Close, Claim, Active, Update,
    NativeEntry, NativeMenuReady, EnterNativePractice, CompleteDisplay };
const ThpracPluginCallbacks callbacks { sizeof(callbacks), THPRAC_PLUGIN_SDK_VERSION, nullptr, DrawHelp, GameEvent };
const ThpracExtension extension { sizeof(extension), pluginId, nullptr, nullptr, nullptr, nullptr,
    nullptr, nullptr, &service, &callbacks };
}
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &THPrac::QuickPractice::extension : nullptr;
}
