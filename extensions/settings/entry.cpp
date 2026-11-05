#include "thprac_feature_services.h"

namespace {
void __cdecl DrawSettings(const ThpracSettingsUi* ui)
{
    if (!ui || ui->size < sizeof(*ui)) return;
    const auto text = [&](const char* zh, const char* ja, const char* en) {
        return ui->locale == 0 ? zh : ui->locale == 2 ? ja : en;
    };
    ui->heading(text("新增功能设置（F11）", "追加機能設定（F11）", "Added features (F11)"));

    static unsigned page = 0;
    if (!(page & ui->sections)) page = ui->sections & (0u - ui->sections);
    if (!page) {
        ui->heading(text("当前作品没有已启用的扩展设置。", "有効な拡張設定がありません。", "No extension settings for this game."));
        return;
    }
    struct Tab { unsigned section; const char* title; };
    const Tab tabs[] {
        {SettingsStatus, text("状态 HUD", "ステータス HUD", "Status HUD")},
        {SettingsVolume, text("音量", "音量", "Volume")},
        {SettingsCapture, text("收率", "取得率", "Capture rates")}
    };
    bool first = true;
    for (const auto& tab : tabs) {
        if (!(tab.section & ui->sections)) continue;
        if (!first) ui->next_tab();
        if (ui->tab(tab.title, tab.section == page)) page = tab.section;
        first = false;
    }
    ui->section(page);
}

const ThpracSettingsService settings {sizeof(settings), DrawSettings};
const ThpracExtension extension {sizeof(extension), "rueee-c7.settings", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, &settings};
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
