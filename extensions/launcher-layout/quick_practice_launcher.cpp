#include "quick_practice_launcher.h"
#include "host_apply.h"
#include "thprac_extension_inventory.h"
#include "thprac_extension.h"
#include "thprac/src/thprac/thprac_load_exe.h"
#include "thprac/src/thprac/thprac_launcher_games.h"
#include "thprac/src/thprac/thprac_launcher_cfg.h"
#include "thprac/src/thprac/thprac_utils.h"

#include <Windows.h>
#include <cstring>
#include <optional>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace THPrac::QuickPractice {
namespace {
std::optional<std::vector<InstalledExtension>> inventory;

HMODULE FindEnabledModule(const wchar_t* filename, ExtensionScope scope)
{
    for (const auto& plugin : *inventory) {
        if (!plugin.enabled || plugin.scope != scope
            || _wcsicmp(plugin.path.filename().c_str(), filename) != 0) continue;
        auto module = GetModuleHandleW(plugin.path.c_str());
        if (!module) module = LoadLibraryExW(plugin.path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) return module;
    }
    return nullptr;
}
}

LauncherSources GetLauncherSources(const char* game)
{
    if (!inventory) {
        const auto* root = LauncherInstallDirectory();
        if (!root) return {};
        inventory = InstalledExtensions(std::filesystem::path(root) / L"plugins", GlobalExtensionDirectory());
    }

    const auto module = FindEnabledModule(L"quick_practice.thp.dll", ExtensionScope::Shared);
    if (!module) return {};
    const auto query = reinterpret_cast<ThpracExtensionQueryFn>(GetProcAddress(module, THPRAC_EXTENSION_QUERY));
    const auto* extension = query ? query(THPRAC_EXTENSION_ABI) : nullptr;
    if (!extension || extension->size < offsetof(ThpracExtension, plugin) || !extension->id
        || std::strcmp(extension->id, pluginId) != 0) return {};
    const auto* service = static_cast<const Service*>(extension->service);
    if (!service || service->size != sizeof(Service) || !service->supports(game)) return {};

    auto adapter = std::strcmp(game, "th15") == 0
        ? FindEnabledModule(L"th15_practice.thp.dll", ExtensionScope::Game) : nullptr;
    if (!adapter) adapter = FindEnabledModule(L"game_adapter.thp.dll", ExtensionScope::Game);
    const auto editor = adapter ? reinterpret_cast<Editor>(GetProcAddress(adapter, editorExport)) : nullptr;
    return editor && editor(game, nullptr, Edit::Probe, nullptr) ? LauncherSources {service, editor} : LauncherSources {};
}

void RefreshLauncherSources()
{
    inventory.reset();
}

void RestoreLauncherSelection(const Service& service, Request& request)
{
    std::string stored;
    const auto key = std::string("quick_practice_") + request.game;
    if (!LauncherSettingGet(key.c_str(), stored)) return;
    rapidjson::Document json;
    if (json.Parse(stored.c_str()).HasParseError() || !json.IsObject()
        || !json.HasMember("difficulty") || !json["difficulty"].IsInt()
        || !json.HasMember("shot") || !json["shot"].IsInt()) return;

    Request selection = request;
    selection.difficulty = json["difficulty"].GetInt();
    selection.shot = json["shot"].GetInt();
    if (!service.entry(&selection, Title)) return;
    if (json.HasMember("selection") && json["selection"].IsObject()) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        json["selection"].Accept(writer);
        if (buffer.GetSize() <= sizeof(selection.parameters)) {
            selection.bytes = static_cast<uint32_t>(buffer.GetSize());
            std::memcpy(selection.parameters, buffer.GetString(), selection.bytes);
        }
    }
    request = selection;
}

void SaveLauncherSelection(const Request& request)
{
    if (!request.bytes || request.bytes > sizeof(request.parameters)) return;
    rapidjson::Document selection;
    if (selection.Parse(reinterpret_cast<const char*>(request.parameters), request.bytes).HasParseError()
        || !selection.IsObject()) return;

    rapidjson::Document json(rapidjson::kObjectType);
    auto& allocator = json.GetAllocator();
    json.AddMember("difficulty", request.difficulty, allocator);
    json.AddMember("shot", request.shot, allocator);
    rapidjson::Value editor;
    editor.CopyFrom(selection, allocator);
    json.AddMember("selection", editor, allocator);
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    json.Accept(writer);

    const auto key = std::string("quick_practice_") + request.game;
    std::string previous;
    if (!LauncherSettingGet(key.c_str(), previous) || previous != buffer.GetString())
        LauncherSettingSet(key.c_str(), buffer.GetString());
}

bool WaitForEntry(HANDLE process, const Service& service, void* ticket, const std::atomic_bool& abort)
{
    const auto deadline = GetTickCount64() + 20000;
    while (!abort && GetTickCount64() < deadline) {
        const auto status = service.status(ticket);
        if (status == Status::Entered) return true;
        DWORD exitCode = 0;
        if (status == Status::Rejected || !GetExitCodeProcess(process, &exitCode) || exitCode != STILL_ACTIVE)
            return false;
        Sleep(50);
    }
    return false;
}

bool Apply(HANDLE process, uintptr_t base, const Service& service, const Request& request, void*& ticket)
{
    ticket = service.create(process, &request);
    return ticket && ApplyGameRuntime(process, base)
        && service.status(ticket) != Status::Rejected;
}

bool LaunchThcrap(const std::wstring& directory, const std::wstring& configuration,
    const Service& service, const Request& request, const std::atomic_bool& abort)
{
    const auto executable = directory + L"\\thcrap_loader.exe";
    auto arguments = L"\"" + executable + L"\" \"" + configuration
        + L"\" " + utf8_to_utf16(request.game);
    const auto job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return false;
    defer(CloseHandle(job));

    // The loader can start a wrapper (vpatch) which in turn starts the
    // game. Attach exclusively to processes from this launch.
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION loader {};
    if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, directory.c_str(), &startup, &loader)) return false;
    defer(CloseHandle(loader.hThread); CloseHandle(loader.hProcess));
    if (!AssignProcessToJobObject(job, loader.hProcess) || ResumeThread(loader.hThread) == DWORD(-1)) {
        TerminateProcess(loader.hProcess, ERROR_FUNCTION_FAILED);
        return false;
    }

    // This launch consists of thcrap, an optional wrapper and the game.
    alignas(JOBOBJECT_BASIC_PROCESS_ID_LIST) unsigned char processList[
        sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 15 * sizeof(ULONG_PTR)] {};
    auto* children = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(processList);
    const auto deadline = GetTickCount64() + 20000;
    while (!abort && GetTickCount64() < deadline) {
        if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, children, sizeof(processList), nullptr))
            return false;
        for (DWORD index = 0; index < children->NumberOfProcessIdsInList; ++index) {
            uintptr_t base = 0;
            HANDLE process = nullptr;
            const auto* game = CheckOngoingGameByPID(static_cast<DWORD>(children->ProcessIdList[index]), &base, &process);
            if (!game) continue;
            defer(CloseHandle(process));
            if (strcmp(game->idStr, request.game) != 0) continue;
            void* ticket = nullptr;
            defer(if (ticket) service.close(ticket));
            return Apply(process, base, service, request, ticket)
                && WaitForEntry(process, service, ticket, abort);
        }
        if (!children->NumberOfProcessIdsInList) return false;
        Sleep(20);
    }
    return false;
}

}
