#include "service.h"
#include "thprac_extension.h"
#include "quick_practice_launcher.h"
#include "host_apply.h"
#include "thprac_launcher_cfg.h"

#include <Windows.h>
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace THPrac::QuickPractice;

bool SelectionSettings(const Service& service, Editor editor, Editor editor15,
    const EditorContext& context, bool restoring)
{
    wchar_t executable[MAX_PATH] {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto directory = std::filesystem::path(executable).parent_path().wstring();
    std::filesystem::create_directory(std::filesystem::path(directory) / L".thprac_data");
    ThpracLauncherHost host { sizeof(host) };
    host.install_directory = directory.c_str();
    THPrac::SetLauncherExtensionHost(&host);
    if (!THPrac::LauncherCfgInit()) return false;
    const char* games[] = { "th06", "th07", "th08", "th10", "th11", "th12", "th13",
        "th14", "th15", "th16", "th17", "th18", "th20" };
    bool result = true;
    for (const auto* game : games) {
        Request request;
        strcpy_s(request.game, game);
        const auto gameEditor = strcmp(game, "th15") == 0 ? editor15 : editor;
        if (restoring) RestoreLauncherSelection(service, request);
        ImGui::NewFrame();
        ImGui::Begin("Saved practice selection");
        bool valid = gameEditor(game, &context, Edit::Reset, &request) != 0;
        if (restoring) {
            valid &= request.difficulty == 2 && request.shot == 1;
            valid &= gameEditor(game, &context, Edit::RestoreSettings, &request) != 0;
            const auto saved = request;
            valid &= gameEditor(game, &context, Edit::SnapshotSettings, &request) != 0;
            valid &= request.bytes == saved.bytes
                && std::memcmp(request.parameters, saved.parameters, saved.bytes) == 0;
        }
        valid &= gameEditor(game, &context, Edit::Draw, &request) != 0;
        valid &= gameEditor(game, &context, Edit::SnapshotSettings, &request) != 0;
        if (!restoring) {
            rapidjson::Document settings;
            valid &= !settings.Parse(reinterpret_cast<char*>(request.parameters), request.bytes).HasParseError();
            if (valid) {
                settings["values"][1].SetInt(2);
                settings["values"][2].SetInt(0);
                for (auto& value : settings["values"].GetArray()) {
                    if (!value.IsBool()) continue;
                    value.SetBool(!value.GetBool());
                    break;
                }
                rapidjson::StringBuffer buffer;
                rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                settings.Accept(writer);
                request.bytes = static_cast<uint32_t>(buffer.GetSize());
                std::memcpy(request.parameters, buffer.GetString(), request.bytes);
                valid &= gameEditor(game, &context, Edit::RestoreSettings, &request) != 0;
                request.difficulty = 2;
                request.shot = 1;
                SaveLauncherSelection(request);
            }
        }
        valid &= gameEditor(game, &context, Edit::Save, &request) != 0 && request.stage == 2;
        ImGui::End();
        ImGui::Render();
        if (!valid) printf("%s selection settings failed (%s)\n", game, restoring ? "restore" : "save");
        result &= valid;
    }
    THPrac::LauncherCfgClose();
    THPrac::SetLauncherExtensionHost(nullptr);
    return result;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 4 && argc != 5) return 1;
    if (Request {}.difficulty != 3) return 9;
    const auto plugin = LoadLibraryW(argv[1]);
    const auto adapter = LoadLibraryW(argv[2]);
    const auto th15 = LoadLibraryW(argv[3]);
    if (!plugin || !adapter || !th15) return 2;
    const auto query = reinterpret_cast<ThpracExtensionQueryFn>(GetProcAddress(plugin, THPRAC_EXTENSION_QUERY));
    const auto editor = reinterpret_cast<Editor>(GetProcAddress(adapter, editorExport));
    const auto editor15 = reinterpret_cast<Editor>(GetProcAddress(th15, editorExport));
    if (!query || !editor || !editor15) return 3;
    const auto* extension = query(THPRAC_EXTENSION_ABI);
    const auto* service = extension ? static_cast<const Service*>(extension->service) : nullptr;
    if (!service || service->size != sizeof(Service) || service->supports("th09") || service->supports("th19"))
        return 4;

    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {850, 700};
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    EditorContext context { ImGui::GetCurrentContext(), 0 };
    ImGui::GetAllocatorFunctions(&context.alloc, &context.free, &context.allocContext);
    if (argc == 5) {
        const bool restored = SelectionSettings(*service, editor, editor15, context, true);
        ImGui::DestroyContext();
        return restored ? 0 : 10;
    }

    struct Game { const char* id; int shots; };
    constexpr Game games[] = {
        {"th18", 4}, {"th06", 4}, {"th07", 6}, {"th08", 12}, {"th10", 6}, {"th11", 6}, {"th12", 6},
        {"th13", 4}, {"th14", 6}, {"th15", 4}, {"th16", 16}, {"th17", 9}, {"th20", 16}
    };
    Request packet;
    for (const auto& game : games) {
        const auto gameEditor = std::strcmp(game.id, "th15") == 0 ? editor15 : editor;
        for (int rank = 0; rank < 4; ++rank) {
            for (int shot = 0; shot < game.shots; ++shot) {
                packet = {};
                strcpy_s(packet.game, game.id);
                packet.difficulty = rank;
                packet.shot = shot;
                ImGui::NewFrame();
                ImGui::Begin("Quick practice");
                const bool edited = gameEditor(game.id, &context, Edit::Reset, &packet)
                    && gameEditor(game.id, &context, Edit::Draw, &packet)
                    && gameEditor(game.id, &context, Edit::Save, &packet);
                ImGui::End();
                ImGui::Render();
                if (!edited || !packet.bytes || !service->entry(&packet, 0)) {
                    printf("%s editor failed for rank %d, shot %d\n", game.id, rank, shot);
                    return 5;
                }
            }
        }
    }
    if (!SelectionSettings(*service, editor, editor15, context, false)) return 11;
    std::wstring command = L"\"" + std::wstring(argv[0]) + L"\"";
    for (int index = 1; index < 4; ++index)
        command += L" \"" + std::wstring(argv[index]) + L"\"";
    command += L" --restore-settings";
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION child {};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &child)) return 12;
    const auto wait = WaitForSingleObject(child.hProcess, 15000);
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &exitCode);
    else TerminateProcess(child.hProcess, 13);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    if (exitCode) return 13;
    ImGui::DestroyContext();

    if (service->claim(packet.game)) return 6;
    auto* ticket = service->create(GetCurrentProcess(), &packet);
    if (!ticket || service->status(ticket) != Status::Pending || service->create(GetCurrentProcess(), &packet))
        return 7;
    const auto* received = service->claim(packet.game);
    const bool result = received && received->bytes == packet.bytes
        && std::memcmp(received->parameters, packet.parameters, packet.bytes) == 0
        && service->status(ticket) == Status::Claimed && !service->claim(packet.game)
        && !service->update(Status::Entered)
        && service->update(Status::Committed) && service->update(Status::Applied)
        && service->update(Status::Entered) && service->status(ticket) == Status::Entered;
    service->close(ticket);
    return result ? 0 : 8;
}
