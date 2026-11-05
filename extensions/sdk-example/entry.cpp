#include "thprac_plugin_sdk.h"
#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {
void* originalCounter;
std::atomic<unsigned> calls {0};
std::atomic<bool> showOverlay {false};
bool hooked;
bool measureTimer;
ThpracPluginStage lastStage {-1, -1, -1, -1};

BOOL WINAPI CountCounterCalls(LARGE_INTEGER* value)
{
    calls.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<BOOL (WINAPI*)(LARGE_INTEGER*)>(originalCounter)(value);
}

void __cdecl DrawSettings(const ThpracPluginUi* ui)
{
    const auto& config = ui->config;
    int enabled = config.get_int(config.plugin_id, "show_overlay", 1);
    const auto label = ui->locale == 0 ? "显示游戏事件浮层"
        : ui->locale == 2 ? "ゲームイベントのオーバーレイを表示" : "Show the game event overlay";
    if (ui->checkbox(label, &enabled) && !config.set_int(config.plugin_id, "show_overlay", enabled))
        ui->text(ui->locale == 0 ? "保存失败" : ui->locale == 2 ? "保存に失敗しました" : "Could not save the setting.");
    ui->text(ui->locale == 0 ? "下次挂接时读取设置。计时调用统计仅用于 TH08。"
        : ui->locale == 2 ? "設定は次回の適用時に読み込まれます。タイマー呼び出しの集計は TH08 のみです。"
        : "Read at the next attach. Timer call counting is specific to TH08.");
}

void __cdecl GameEvent(const ThpracPluginGameEvent* event)
{
    switch (event->kind) {
    case THPRAC_EVENT_GAME_READY:
        showOverlay = event->config.get_int(event->config.plugin_id, "show_overlay", 1) != 0;
        measureTimer = std::strcmp(event->game_id, "th08") == 0;
        if (showOverlay && measureTimer) {
            hooked = event->hook_import("KERNEL32.dll", "QueryPerformanceCounter",
                reinterpret_cast<void*>(CountCounterCalls), &originalCounter) != 0;
        }
        break;
    case THPRAC_EVENT_STAGE_STARTED:
        lastStage = *event->stage;
        break;
    case THPRAC_EVENT_FRAME: {
        if (!showOverlay) return;
        char label[160];
        char stage[16] = "-";
        char shot[16] = "-";
        if (lastStage.stage_index >= 0) std::snprintf(stage, sizeof(stage), "%d", lastStage.stage_index + 1);
        if (lastStage.shot >= 0) std::snprintf(shot, sizeof(shot), "%d", lastStage.shot);
        const char* mode = lastStage.replay == 1 ? "Replay" : lastStage.practice == 1 ? "Practice"
            : lastStage.practice == 0 && lastStage.replay == 0 ? "Game" : "-";
        if (measureTimer && hooked) {
            std::snprintf(label, sizeof(label), "%s | Stage %s | Shot %s | %s | QPC %u",
                event->game_id, stage, shot, mode, calls.load(std::memory_order_relaxed));
        } else {
            std::snprintf(label, sizeof(label), "%s | Stage %s | Shot %s | %s",
                event->game_id, stage, shot, mode);
        }
        event->overlay->draw_text_box(event->overlay->drawing, 8.0f, 36.0f, 380.0f, label);
        break;
    }
    case THPRAC_EVENT_WINDOW_CLOSED:
        showOverlay = false;
        break;
    }
}

const char* const games[] {
    "th06", "th07", "th08", "th09", "th10", "th11", "th12", "th13", "th14", "th15",
    "th16", "th17", "th18", "th19", "th20", nullptr
};
const ThpracPluginCallbacks callbacks {sizeof(callbacks), THPRAC_PLUGIN_SDK_VERSION, games, DrawSettings, GameEvent};
const ThpracExtension extension {sizeof(extension), "rueee-c7.sdk-example", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, &callbacks};
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
