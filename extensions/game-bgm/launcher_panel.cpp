#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <commdlg.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <imgui.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include "host_apply.h"
#include "thprac_extension_inventory.h"
#include "thprac_utils.h"

namespace THPrac {
std::wstring LauncherGameDirectory(const char* id);
namespace {
namespace fs = std::filesystem;

struct Game {
    const char* id;
    const char* label;
};
constexpr Game games[] {
    {"th06", "TH06 红魔乡"}, {"th07", "TH07 妖妖梦"}, {"th08", "TH08 永夜抄"},
    {"th09", "TH09 花映塚"}, {"th10", "TH10 风神录"}, {"th11", "TH11 地灵殿"},
    {"th12", "TH12 星莲船"}, {"th13", "TH13 神灵庙"}, {"th14", "TH14 辉针城"},
    {"th15", "TH15 绀珠传"}, {"th16", "TH16 天空璋"}, {"th17", "TH17 鬼形兽"},
    {"th18", "TH18 虹龙洞"}, {"th19", "TH19 兽王园"}, {"th20", "TH20 锦上京"},
    {"th095", "TH09.5 文花帖"}, {"th125", "TH12.5 文花帖 DS"}, {"th128", "TH12.8 妖精大战争"},
    {"th143", "TH14.3 弹幕天邪鬼"}, {"th165", "TH16.5 秘封噩梦日记"}, {"th185", "TH18.5 弹幕狂们"},
};
constexpr const char* archiveNames[] {"", "thbgm.dat", "thbgm_tr.dat"};

struct Loop {
    double start;
    double end;
};

struct Candidate {
    std::string path;
    std::optional<Loop> loop;
};

struct Replacement {
    std::vector<Candidate> candidates;
    size_t selected = 0;
    bool random = false;
    bool once = true;

    bool Empty() const { return candidates.empty(); }
};

struct Track {
    std::string name;
    std::string title;
    Replacement source;
    bool legacy;
    std::optional<Loop> nativeLoop;
    int rate;
};

struct Preset {
    std::string name;
    bool matches;
};

std::optional<Loop> ReadLoop(const rapidjson::Value& value, const char* key)
{
    const auto item = value.FindMember(key);
    if (item == value.MemberEnd() || !item->value.IsArray() || item->value.Size() != 2
        || !item->value[0].IsNumber() || !item->value[1].IsNumber()) return {};
    return Loop {item->value[0].GetDouble(), item->value[1].GetDouble()};
}

std::string String(const rapidjson::Value& value, const char* key)
{
    const auto item = value.FindMember(key);
    return item != value.MemberEnd() && item->value.IsString() ? item->value.GetString() : "";
}

Replacement ReadReplacement(const rapidjson::Value& track)
{
    Replacement result;
    const auto policy = track.FindMember("policy");
    if (policy != track.MemberEnd() && policy->value.IsObject()) {
        const auto& entry = policy->value;
        const auto choices = entry.FindMember("candidates");
        if (choices != entry.MemberEnd() && choices->value.IsArray()) {
            for (const auto& choice : choices->value.GetArray()) {
                if (choice.IsObject())
                    result.candidates.push_back({String(choice, "path"), ReadLoop(choice, "loop")});
            }
        }
        const auto selected = entry.FindMember("selected");
        if (selected != entry.MemberEnd() && selected->value.IsUint() && selected->value.GetUint() < result.candidates.size())
            result.selected = selected->value.GetUint();
        const auto random = entry.FindMember("random");
        result.random = random != entry.MemberEnd() && random->value.IsTrue();
        const auto once = entry.FindMember("once");
        result.once = once == entry.MemberEnd() || !once->value.IsFalse();
    } else {
        auto path = String(track, "source");
        if (!path.empty()) {
            result.candidates.push_back({std::move(path), ReadLoop(track, "loop")});
            result.once = false;
        }
    }
    return result;
}

std::string PickFile(const wchar_t* title, const wchar_t* filter)
{
    wchar_t file[32768] {};
    OPENFILENAMEW dialog {sizeof(dialog)};
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrTitle = title;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = file;
    dialog.nMaxFile = static_cast<DWORD>(std::size(file));
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? utf16_to_utf8(file) : "";
}

class Panel {
public:
    void Open()
    {
        if (process) return;
        status.clear();
        tracks.clear();
        changes.clear();
        selected = -1;
        folder.clear();
        fmt.clear();
        game = archive = 0;
        script.clear();
        const wchar_t* install = LauncherInstallDirectory();
        const auto local = install ? fs::path(install) / L"plugins" : fs::path();
        for (const auto& file : InstalledExtensions(local, GlobalExtensionDirectory())) {
            if (file.scope == ExtensionScope::Launcher || !file.enabled
                || _wcsicmp(file.path.filename().c_str(), L"game_bgm.thp.dll") != 0) continue;
            const auto candidate = file.path.parent_path() / L"bgm_bridge.py";
            std::error_code error;
            if (fs::is_regular_file(candidate, error)) {
                script = candidate;
                break;
            }
        }
        if (script.empty()) {
            status = "请在插件管理中启用新版背景音乐插件，并确认插件目录中有 bgm_bridge.py。";
            return;
        }
        Start("settings", "");
    }

    bool Draw()
    {
        Poll();
        if (!process && changes.empty() && ImGui::Button("返回工具")) return false;
        ImGui::TextUnformatted("背景音乐");
        if (script.empty()) {
            ImGui::TextWrapped("%s", status.c_str());
            return true;
        }

        const bool changeLibrary = !process && changes.empty();
        if (changeLibrary && ImGui::BeginCombo("作品", games[game].label)) {
            for (int index = 0; index < static_cast<int>(std::size(games)); ++index) {
                if (ImGui::Selectable(games[index].label, index == game) && index != game) {
                    game = index;
                    folder.clear();
                    fmt.clear();
                    archive = 0;
                    ClearTracks();
                    Start("settings", games[game].id);
                }
            }
            ImGui::EndCombo();
        } else if (!changeLibrary) {
            ImGui::Text("作品  %s", games[game].label);
        }
        if (changeLibrary) {
            if (ImGui::Button("重新读取曲目")) SyncLibrary();
        }
        if (!folder.empty()) ImGui::TextWrapped("%s", folder.c_str());
        if (game != 0 && changeLibrary && !process && !folder.empty()) {
            if (ImGui::Combo("音乐包", &archive, "自动选择\0thbgm.dat\0thbgm_tr.dat\0")) {
                fmt.clear();
                if (!folder.empty()) Load();
            }
            if (ImGui::Button("选择曲目表…")) {
                auto path = PickFile(L"选择同一游戏版本的曲目表", L"曲目表 (*.fmt)\0*.fmt\0所有文件\0*.*\0");
                if (!path.empty()) {
                    fmt = std::move(path);
                    if (!folder.empty()) Load();
                }
            }
        }
        if (!process && ImGui::CollapsingHeader("工具路径")) {
            if (ImGui::Button("选择 FFmpeg…")) SelectTool(ffmpeg);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", ffmpeg.empty() ? "使用 PATH 中的 FFmpeg" : ffmpeg.c_str());
            if (ImGui::Button("选择 thdat…")) SelectTool(thdat);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", thdat.empty() ? "使用 PATH 中的 thdat" : thdat.c_str());
        }

        if (!tracks.empty()) {
            DrawPresets();
            DrawTracks();
        }
        ImGui::TextWrapped("%s", status.c_str());
        return true;
    }

private:
    void ClearTracks()
    {
        tracks.clear();
        presets.clear();
        selectedPreset.clear();
        selected = -1;
    }

    void Load()
    {
        ClearTracks();
        Start("list", games[game].id);
    }

    void SyncLibrary()
    {
        const auto path = LauncherGameDirectory(games[game].id);
        std::error_code error;
        if (path.empty() || folder.empty()
            || !fs::equivalent(path, utf8_to_utf16(folder.c_str()), error)) {
            fmt.clear();
            archive = 0;
        }
        folder = utf16_to_utf8(path.c_str());
        if (folder.empty()) {
            ClearTracks();
            status = "请先在 THPrac 的游戏库中配置此作品的游戏路径。";
        } else {
            Load();
        }
    }

    void SelectTool(std::string& target)
    {
        auto path = PickFile(L"选择工具可执行文件", L"应用程序 (*.exe)\0*.exe\0");
        if (!path.empty()) target = std::move(path);
    }

    void DrawPresets()
    {
        if (!ImGui::CollapsingHeader("替换预设", ImGuiTreeNodeFlags_DefaultOpen)) return;
        if (process || !changes.empty()) {
            if (!process) ImGui::TextUnformatted("请先应用或撤销待修改的曲目。");
            return;
        }
        if (ImGui::BeginCombo("预设", selectedPreset.empty() ? "选择预设" : selectedPreset.c_str())) {
            for (const auto& preset : presets) {
                ImGui::PushID(preset.name.c_str());
                const auto label = preset.name + (preset.matches ? "  [与当前一致]" : "") + "###preset";
                if (ImGui::Selectable(label.c_str(), preset.name == selectedPreset))
                    selectedPreset = preset.name;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("保存为新预设…")) {
            presetName[0] = '\0';
            presetCommand = "preset_save";
            ImGui::OpenPopup("管理预设");
        }
        if (!selectedPreset.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("应用预设")) Start("preset_apply", games[game].id, selectedPreset);
            ImGui::SameLine();
            if (ImGui::Button("更新预设…")) {
                presetCommand = "preset_update";
                ImGui::OpenPopup("管理预设");
            }
            ImGui::SameLine();
            if (ImGui::Button("重命名…")) {
                strcpy_s(presetName, selectedPreset.c_str());
                presetCommand = "preset_rename";
                ImGui::OpenPopup("管理预设");
            }
            ImGui::SameLine();
            if (ImGui::Button("删除…")) {
                presetCommand = "preset_delete";
                ImGui::OpenPopup("管理预设");
            }
        }
        if (ImGui::BeginPopupModal("管理预设", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool saveNew = presetCommand == "preset_save";
            const bool rename = presetCommand == "preset_rename";
            if (saveNew || rename) ImGui::InputText("预设名称", presetName, sizeof(presetName));
            else ImGui::Text("%s「%s」？", presetCommand == "preset_delete" ? "删除预设" : "用当前方案更新预设",
                selectedPreset.c_str());
            if (ImGui::Button("确定") && (!(saveNew || rename) || presetName[0])) {
                Start(presetCommand.c_str(), games[game].id, saveNew ? presetName : selectedPreset,
                    rename ? presetName : "");
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("取消")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void DrawTracks()
    {
        const float height = ImGui::GetContentRegionAvail().y - 105.0f;
        if (ImGui::BeginChild("bgm_tracks", ImVec2(0, height > 110.0f ? height : 110.0f), true)) {
            for (int index = 0; index < static_cast<int>(tracks.size()); ++index) {
                const auto& track = tracks[index];
                const auto pending = changes.find(track.name);
                const auto state = pending != changes.end()
                    ? (pending->second.Empty() ? "待恢复" : "待替换")
                    : (!track.source.Empty() ? "已替换" : track.legacy ? "旧版替换" : "原曲");
                auto label = track.title.empty() ? track.name : track.title + "  (" + track.name + ")";
                label += std::string("  [") + state + "]###" + track.name;
                if (ImGui::Selectable(label.c_str(), selected == index, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected = index;
                    if (!process && ImGui::IsMouseDoubleClicked(0)) AddCandidate(track);
                }
            }
        }
        ImGui::EndChild();

        if (selected >= 0) {
            const auto& track = tracks[selected];
            if (!process && ImGui::Button("添加候选曲…")) AddCandidate(track);
            if (!process && (!track.source.Empty() || changes.contains(track.name))) {
                ImGui::SameLine();
                if (ImGui::Button("恢复选中曲目")) {
                    if (track.source.Empty()) changes.erase(track.name);
                    else changes[track.name] = {};
                }
            }
            const auto pending = changes.find(track.name);
            auto source = Playback(track);
            if (!process && pending == changes.end() && !source.Empty()) {
                ImGui::SameLine();
                if (ImGui::Button("重新生成替换")) changes[track.name] = source;
            }
            if (!source.Empty()) DrawCandidates(track, source);
            DrawLoop(track, Playback(track));
        }
        if (!process && !changes.empty()) {
            if (ImGui::Button("应用更改")) Start("apply", games[game].id);
            ImGui::SameLine();
            if (ImGui::Button("撤销待应用更改")) changes.clear();
        }
    }

    Replacement Playback(const Track& track) const
    {
        const auto pending = changes.find(track.name);
        return pending != changes.end() ? pending->second : track.source;
    }

    void AddCandidate(const Track& track)
    {
        auto path = PickFile(L"选择替换音频", L"音频文件\0*.wav;*.mp3;*.flac;*.m4a;*.aac;*.ogg;*.opus;*.wma;*.aiff;*.ape;*.wv;*.mp4;*.mka\0所有文件\0*.*\0");
        if (path.empty()) return;
        auto source = Playback(track);
        for (size_t i = 0; i < source.candidates.size(); ++i) {
            if (source.candidates[i].path == path) {
                source.selected = i;
                changes[track.name] = std::move(source);
                return;
            }
        }
        source.selected = source.candidates.size();
        source.candidates.push_back({std::move(path), {}});
        changes[track.name] = std::move(source);
    }

    void DrawCandidates(const Track& track, Replacement source)
    {
        if (process) return;
        const auto label = [](const Candidate& item) {
            return utf16_to_utf8(fs::path(utf8_to_utf16(item.path.c_str())).filename().c_str());
        };
        if (ImGui::BeginCombo("候选曲", label(source.candidates[source.selected]).c_str())) {
            for (size_t i = 0; i < source.candidates.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(label(source.candidates[i]).c_str(), i == source.selected)) {
                    source.selected = i;
                    changes[track.name] = source;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("移除此候选")) {
            source.candidates.erase(source.candidates.begin() + source.selected);
            if (source.selected && source.selected >= source.candidates.size()) --source.selected;
            if (source.candidates.size() < 2) source.random = false;
            if (source.Empty() && track.source.Empty()) changes.erase(track.name);
            else changes[track.name] = std::move(source);
            return;
        }
        ImGui::TextWrapped("%s", source.candidates[source.selected].path.c_str());
        if (source.candidates.size() > 1 && ImGui::Checkbox("每次启动游戏随机选一首", &source.random))
            changes[track.name] = source;
        bool looping = !source.once;
        if (ImGui::Checkbox("循环播放替换曲", &looping)) {
            source.once = !looping;
            changes[track.name] = std::move(source);
        }
    }

    void DrawLoop(const Track& track, const Replacement& source)
    {
        if (track.nativeLoop && track.rate > 0) {
            const auto original = *track.nativeLoop;
            ImGui::Text("原版循环  %.3f – %.3f 秒（%.0f – %.0f 帧）",
                original.start / track.rate, original.end / track.rate, original.start, original.end);
        } else {
            ImGui::TextUnformatted("原版循环点未找到");
        }
        if (source.Empty() || source.once || !track.nativeLoop) return;
        const auto& choice = source.candidates[source.selected];
        if (choice.loop) ImGui::Text("替换音频循环  %.3f – %.3f 秒", choice.loop->start, choice.loop->end);
        if (!process && ImGui::Button("设置替换循环…")) {
            editCustomLoop = choice.loop.has_value();
            const auto original = *track.nativeLoop;
            editLoop = choice.loop.value_or(Loop {original.start / track.rate, original.end / track.rate});
            ImGui::OpenPopup("替换音频循环点");
        }
        if (ImGui::BeginPopupModal("替换音频循环点", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Checkbox("自定义替换音频的循环区间", &editCustomLoop);
            if (editCustomLoop) {
                ImGui::InputDouble("循环起点（秒）", &editLoop.start, 0, 0, "%.3f");
                ImGui::InputDouble("循环终点（秒）", &editLoop.end, 0, 0, "%.3f");
                ImGui::TextUnformatted("循环区间会变速对齐到原版长度。");
            }
            if (ImGui::Button("确定")) {
                if (!editCustomLoop || (std::isfinite(editLoop.start) && std::isfinite(editLoop.end)
                    && editLoop.start >= 0 && editLoop.start < editLoop.end)) {
                    auto edited = source;
                    edited.candidates[edited.selected].loop = editCustomLoop ? std::optional(editLoop) : std::nullopt;
                    changes[track.name] = std::move(edited);
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("取消")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    void Start(const char* operation, const char* code, const std::string& preset = {},
        const char* newPresetName = "")
    {
        if (process) return;
        wchar_t search[32768], python[32768];
        const auto length = GetEnvironmentVariableW(L"PATH", search, static_cast<DWORD>(std::size(search)));
        const auto found = length && length < std::size(search)
            ? SearchPathW(search, L"python.exe", nullptr, static_cast<DWORD>(std::size(python)), python, nullptr) : 0;
        if (!found || found >= std::size(python)) {
            status = "找不到 Python 3.10 或更新版本。请把 Python 加入 PATH 后重试。";
            return;
        }
        wchar_t tempFolder[MAX_PATH], tempFile[MAX_PATH];
        const auto tempLength = GetTempPathW(MAX_PATH, tempFolder);
        if (!tempLength || tempLength >= MAX_PATH || !GetTempFileNameW(tempFolder, L"bgm", 0, tempFile)) {
            status = "无法创建背景音乐任务文件。";
            return;
        }
        requestFile = tempFile;
        rapidjson::StringBuffer data;
        rapidjson::Writer writer(data);
        writer.StartObject();
        auto field = [&](const char* key, const char* value) {
            writer.Key(key);
            writer.String(value);
        };
        field("mode", operation);
        field("game", code);
        field("folder", folder.c_str());
        field("fmt", fmt.c_str());
        field("archive", archiveNames[archive]);
        field("ffmpeg", ffmpeg.c_str());
        field("thdat", thdat.c_str());
        field("preset", preset.c_str());
        field("preset_new_name", newPresetName);
        writer.Key("changes");
        writer.StartObject();
        for (const auto& [name, source] : changes) {
            writer.Key(name.c_str());
            if (source.Empty()) {
                writer.Null();
            } else {
                writer.StartObject();
                writer.Key("selected");
                writer.Uint(static_cast<unsigned>(source.selected));
                writer.Key("random");
                writer.Bool(source.random);
                writer.Key("once");
                writer.Bool(source.once);
                writer.Key("candidates");
                writer.StartArray();
                for (const auto& candidate : source.candidates) {
                    writer.StartObject();
                    field("path", candidate.path.c_str());
                    if (candidate.loop) {
                        writer.Key("loop");
                        writer.StartArray();
                        writer.Double(candidate.loop->start);
                        writer.Double(candidate.loop->end);
                        writer.EndArray();
                    }
                    writer.EndObject();
                }
                writer.EndArray();
                writer.EndObject();
            }
        }
        writer.EndObject();
        writer.EndObject();
        {
            std::ofstream output(requestFile, std::ios::binary | std::ios::trunc);
            if (!output.write(data.GetString(), data.GetSize())) {
                output.close();
                DeleteFileW(requestFile.c_str());
                status = "无法写入背景音乐任务文件。";
                return;
            }
        }
        auto command = std::wstring(L"\"") + python + L"\" -B \"" + script.wstring()
            + L"\" \"" + requestFile.wstring() + L"\"";
        STARTUPINFOW startup {sizeof(startup)};
        PROCESS_INFORMATION info {};
        if (!CreateProcessW(python, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                script.parent_path().c_str(), &startup, &info)) {
            DeleteFileW(requestFile.c_str());
            status = "无法启动背景音乐处理程序。";
            return;
        }
        CloseHandle(info.hThread);
        process = info.hProcess;
        mode = operation;
        status = mode == "apply" || mode == "preset_apply" ? "正在准备换曲文件…" : "正在读取曲目…";
        requestedPreset = mode == "preset_rename" ? newPresetName : preset;
    }

    void Poll()
    {
        if (!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0) return;
        DWORD exitCode = 0;
        GetExitCodeProcess(process, &exitCode);
        CloseHandle(process);
        process = nullptr;
        std::ifstream input(requestFile, std::ios::binary);
        std::string contents {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        input.close();
        DeleteFileW(requestFile.c_str());
        rapidjson::Document result;
        if (exitCode || result.Parse(contents.c_str()).HasParseError() || !result.IsObject()) {
            status = "背景音乐处理程序执行失败，请检查 Python 版本和插件文件。";
            return;
        }
        if (result.HasMember("error")) {
            status = String(result, "error");
            return;
        }
        if (mode == "settings") {
            const auto code = String(result, "game");
            for (int index = 0; index < static_cast<int>(std::size(games)); ++index) {
                if (code == games[index].id) game = index;
            }
            folder = String(result, "folder");
            fmt = String(result, "fmt");
            ffmpeg = String(result, "ffmpeg");
            thdat = String(result, "thdat");
            const auto archiveName = String(result, "archive");
            archive = archiveName == archiveNames[1] ? 1 : archiveName == archiveNames[2] ? 2 : 0;
            SyncLibrary();
            return;
        }
        const auto items = result.FindMember("tracks");
        if (items == result.MemberEnd() || !items->value.IsArray()) {
            status = "曲目列表无法读取。";
            return;
        }
        const auto previous = selected >= 0 ? tracks[selected].name : "";
        const auto priorPreset = requestedPreset.empty() ? selectedPreset : requestedPreset;
        ClearTracks();
        const auto presetItems = result.FindMember("presets");
        if (presetItems != result.MemberEnd() && presetItems->value.IsArray()) {
            for (const auto& item : presetItems->value.GetArray()) {
                if (!item.IsObject()) continue;
                const auto matches = item.FindMember("matches");
                presets.push_back({String(item, "name"), matches != item.MemberEnd() && matches->value.IsTrue()});
                if (presets.back().name == priorPreset) selectedPreset = priorPreset;
            }
        }
        for (const auto& item : items->value.GetArray()) {
            if (!item.IsObject()) continue;
            const auto flag = item.FindMember("legacy");
            const auto rate = item.FindMember("rate");
            tracks.push_back({String(item, "name"), String(item, "title"), ReadReplacement(item),
                flag != item.MemberEnd() && flag->value.IsTrue(), ReadLoop(item, "native_loop"),
                rate != item.MemberEnd() && rate->value.IsInt() ? rate->value.GetInt() : 0});
            if (tracks.back().name == previous) selected = static_cast<int>(tracks.size()) - 1;
        }
        changes.clear();
        if (mode == "apply" || mode == "preset_apply") status = "已保存。重启游戏后应用换曲。";
        else if (mode.starts_with("preset_")) status = "预设已更新。";
        else status = "请选择曲目和替换音频。";
    }

    int game = 0, archive = 0, selected = -1;
    fs::path script, requestFile;
    HANDLE process = nullptr;
    std::string folder, fmt, ffmpeg, thdat, mode, status;
    std::vector<Track> tracks;
    std::map<std::string, Replacement> changes;
    std::vector<Preset> presets;
    std::string selectedPreset, requestedPreset, presetCommand;
    char presetName[321] {};
    Loop editLoop {};
    bool editCustomLoop = false;
};

Panel panel;
}

void OpenBgmLauncherPanel()
{
    panel.Open();
}

bool DrawBgmLauncherPanel()
{
    return panel.Draw();
}
}
