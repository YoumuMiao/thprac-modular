#include "thprac_extension_manager.h"
#include "thprac_extension_inventory.h"
#include "thprac_plugin_sdk.h"
#include <imgui.h>

namespace THPrac {
namespace {
struct PluginRow {
    InstalledExtension extension;
    ExtensionMetadata metadata;
};

std::vector<PluginRow> ReadPluginRows(int locale)
{
    std::vector<PluginRow> rows;
    for (auto& extension : InstalledExtensions(ExtensionDirectory(nullptr), GlobalExtensionDirectory())) {
        auto metadata = ReadExtensionMetadata(extension, locale);
        rows.push_back({std::move(extension), std::move(metadata)});
    }
    return rows;
}

const char* ScopeText(int locale, ExtensionScope scope)
{
    switch (scope) {
    case ExtensionScope::Launcher: return locale == 0 ? "启动器" : locale == 2 ? "ランチャー" : "Launcher";
    case ExtensionScope::Game: return locale == 0 ? "游戏内" : locale == 2 ? "ゲーム内" : "In-game";
    case ExtensionScope::Shared: return locale == 0 ? "共用" : locale == 2 ? "共用" : "Shared";
    }
    return "";
}
}

void DrawExtensionManager(int locale, bool entering, void (__cdecl *refreshLauncherPlugins)(),
    int (__cdecl *drawSettings)(const wchar_t*, const ThpracPluginUi*))
{
    static std::vector<PluginRow> rows;
    static int previousLocale = -1;
    static bool saveFailed = false;
    const auto text = [locale](const char* zh, const char* en, const char* ja) {
        return locale == 0 ? zh : locale == 2 ? ja : en;
    };
    const auto refresh = ImGui::Button(text("刷新", "Refresh", "更新"));
    if (entering || refresh || locale != previousLocale) {
        rows = ReadPluginRows(locale);
        previousLocale = locale;
        saveFailed = false;
        refreshLauncherPlugins();
    }
    ImGui::SameLine();
    ImGui::Text(text("已安装 %zu 项", "%zu installed", "%zu 件インストール済み"), rows.size());
    ImGui::TextWrapped("%s", text(
        "启停立即保存。启动器工具入口即时更新，布局需重启启动器，已挂接的游戏需重启游戏。",
        "Changes are saved immediately. Tools update live. Restart the launcher for layout changes and attached games for game changes.",
        "変更はすぐに保存されます。ツールは即時反映され、レイアウトと適用済みゲームには再起動が必要です。"));
    ImGui::TextWrapped("%s", text(
        "全局目录 %APPDATA%\\thprac\\plugins。同一插件优先使用程序目录中的启用版本。",
        "Global folder %APPDATA%\\thprac\\plugins. An enabled local copy takes priority.",
        "共通フォルダー %APPDATA%\\thprac\\plugins。有効なローカル版を優先します。"));
    if (saveFailed) {
        ImGui::TextWrapped("%s", text(
            "无法保存插件状态，请检查目录权限或是否已有同名 DLL。",
            "Could not save the plugin state. Check folder permissions and duplicate filenames.",
            "状態を保存できません。フォルダーの権限と同名の DLL を確認してください。"));
    }
    if (rows.empty()) {
        ImGui::TextDisabled("%s", text("当前未安装插件。", "No plugins installed.", "プラグインはありません。"));
        return;
    }

    const InstalledExtension* toggled = nullptr;
    constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("##installed_plugins", 4, flags)) {
        const auto unit = ImGui::GetFontSize();
        ImGui::TableSetupColumn(text("插件及说明", "Plugin and description", "プラグインと説明"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(text("范围 / 来源", "Scope / source", "範囲 / 場所"), ImGuiTableColumnFlags_WidthFixed, unit * 7.0f);
        ImGui::TableSetupColumn(text("状态", "State", "状態"), ImGuiTableColumnFlags_WidthFixed, unit * 5.5f);
        ImGui::TableSetupColumn(text("操作", "Action", "操作"), ImGuiTableColumnFlags_WidthFixed, unit * 5.5f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        int row = 0;
        for (const auto& plugin : rows) {
            const auto& extension = plugin.extension;
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto name = extension.path.filename().u8string();
            const auto* utf8 = reinterpret_cast<const char*>(name.c_str());
            ImGui::TextUnformatted(utf8);
            if (ImGui::IsItemHovered()) {
                const auto path = extension.path.u8string();
                ImGui::SetTooltip("%s", reinterpret_cast<const char*>(path.c_str()));
            }
            ImGui::TextWrapped("%s", plugin.metadata.description.empty()
                ? text("未提供说明。", "No description provided.", "説明はありません。")
                : plugin.metadata.description.c_str());
            if (drawSettings && extension.enabled && plugin.metadata.hasSettings
                && ImGui::TreeNode(text("插件设置", "Plugin settings", "プラグイン設定"))) {
                const ThpracPluginUi ui {sizeof(ui), locale,
                    [](const char* label) { ImGui::TextWrapped("%s", label); },
                    [](const char* label, int* enabled) -> int {
                        bool value = *enabled != 0;
                        if (!ImGui::Checkbox(label, &value)) return 0;
                        *enabled = value;
                        return 1;
                    }, {}};
                if (!drawSettings(extension.path.c_str(), &ui))
                    ImGui::TextDisabled("%s", text("无法打开插件设置。", "Plugin settings unavailable.", "設定を開けません。"));
                ImGui::TreePop();
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(ScopeText(locale, extension.scope));
            ImGui::TextDisabled("%s", extension.global ? text("全局目录", "Global", "共通")
                : text("程序目录", "Local", "ローカル"));
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(extension.enabled ? text("已启用", "Enabled", "有効")
                : text("已禁用", "Disabled", "無効"));
            ImGui::TableSetColumnIndex(3);
            if (ImGui::SmallButton(extension.enabled ? text("禁用", "Disable", "無効化")
                    : text("启用", "Enable", "有効化"))) {
                toggled = &extension;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (toggled) {
        saveFailed = !SetExtensionEnabled(*toggled, !toggled->enabled);
        rows = ReadPluginRows(locale);
        if (!saveFailed) refreshLauncherPlugins();
    }
}
}
