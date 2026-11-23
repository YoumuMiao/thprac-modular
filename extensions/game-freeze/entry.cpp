#include "thprac_plugin_sdk.h"
#include <Windows.h>
#include <cstdio>

namespace {
constexpr int defaultKey = 'P';
constexpr int stepKey = 'L';
constexpr char setting[] = "hotkey";

const char* KeyName(int key, char (&label)[8])
{
    if (key == stepKey) return nullptr;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) {
        label[0] = static_cast<char>(key);
        label[1] = '\0';
        return label;
    }
    if (key >= VK_F1 && key <= VK_F12) {
        std::snprintf(label, sizeof(label), "F%d", key - VK_F1 + 1);
        return label;
    }
    switch (key) {
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "Page Up";
    case VK_NEXT: return "Page Down";
    default: return nullptr;
    }
}

int ReadKey(const ThpracPluginConfig& config)
{
    const int key = config.get_int(config.plugin_id, setting, defaultKey);
    char label[8];
    return KeyName(key, label) ? key : defaultKey;
}

void __cdecl DrawSettings(const ThpracPluginUi* ui)
{
    static int recording = 0;
    static bool waitForRelease = false;
    static bool saveFailed = false;
    const auto text = [ui](const char* zh, const char* en, const char* ja) {
        return ui->locale == 0 ? zh : ui->locale == 2 ? ja : en;
    };
    const auto& config = ui->config;
    char keyLabel[8], description[96];
    std::snprintf(description, sizeof(description), text("当前快捷键 %s", "Current hotkey: %s", "現在のキー %s"),
        KeyName(ReadKey(config), keyLabel));
    ui->text(description);
    if (ui->checkbox(text("录制快捷键", "Record a hotkey", "キーを割り当てる"), &recording)) {
        waitForRelease = true;
        saveFailed = false;
    }
    ui->text(text("冻结时按 L 推进一帧。冻结键支持其他字母、数字、F1–F12 和 Insert、Delete、Home、End、Page Up、Page Down。重启游戏后生效。",
        "Press L while frozen to step one frame. The freeze key can be another letter, a digit, F1–F12, Insert, Delete, Home, End, Page Up or Page Down. Restart the game to apply.",
        "停止中に L キーで 1 フレーム進みます。停止キーには L 以外の英数字、F1～F12、Insert、Delete、Home、End、Page Up、Page Down を指定できます。ゲームの再起動後に反映されます。"));
    if (saveFailed)
        ui->text(text("快捷键保存失败。", "Could not save the hotkey.", "キーを保存できませんでした。"));
    if (!recording) return;

    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    if (process != GetCurrentProcessId()) {
        waitForRelease = true;
        return;
    }

    int pressed = 0;
    for (int key = 1; key <= 0xFF; ++key) {
        if (!KeyName(key, keyLabel) || !(GetAsyncKeyState(key) & 0x8000)) continue;
        if (pressed) {
            waitForRelease = true;
            return;
        }
        pressed = key;
    }
    if (!pressed) {
        waitForRelease = false;
    } else if (!waitForRelease) {
        saveFailed = !config.set_int(config.plugin_id, setting, pressed);
        recording = 0;
    }
}

int hotkey = defaultKey;
// The message hook and its nested pump run on the game's window thread.
enum class RunState { Running, Frozen, Stepping, FrameReached };
thread_local HWND gameWindow;
thread_local HHOOK messageHook;
thread_local RunState runState;
thread_local unsigned queuedSteps;

bool IsKey(const MSG& message, HWND window, int key)
{
    return message.hwnd == window && message.wParam == static_cast<WPARAM>(key)
        && (message.message == WM_KEYDOWN || message.message == WM_KEYUP);
}

bool IsNewPress(const MSG& message)
{
    return message.message == WM_KEYDOWN && !(message.lParam & (1L << 30));
}

void ConsumeMessage(MSG& message)
{
    message.message = WM_NULL;
    message.wParam = 0;
    message.lParam = 0;
}

void WaitForCommand(HWND window, const MSG* first = nullptr)
{
    runState = RunState::Frozen;
    while (IsWindow(window)) {
        MSG message;
        if (first) {
            message = *first;
            first = nullptr;
        } else if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (!IsWindow(window) || !WaitMessage()) break;
            continue;
        }
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (IsKey(message, window, hotkey)) {
            if (IsNewPress(message) && GetForegroundWindow() == window) break;
            continue;
        }
        if (IsKey(message, window, stepKey)) {
            if (IsNewPress(message) && GetForegroundWindow() == window) {
                runState = RunState::Stepping;
                return;
            }
            continue;
        }
        if (message.hwnd == window && message.message == WM_KEYDOWN) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    runState = RunState::Running;
}

LRESULT CALLBACK FilterMessages(int code, WPARAM removed, LPARAM data)
{
    const auto next = CallNextHookEx(nullptr, code, removed, data);
    if (code != HC_ACTION || removed != PM_REMOVE || runState == RunState::Frozen || !gameWindow) return next;
    auto& message = *reinterpret_cast<MSG*>(data);
    if (runState == RunState::FrameReached) {
        if (message.message == WM_QUIT) {
            runState = RunState::Running;
            queuedSteps = 0;
            return next;
        }
        if (!queuedSteps) {
            const MSG first = message;
            ConsumeMessage(message);
            WaitForCommand(gameWindow, &first);
            return next;
        }
        --queuedSteps;
        runState = RunState::Stepping;
    }

    if (message.hwnd != gameWindow || GetForegroundWindow() != gameWindow) return next;
    const bool pressed = IsNewPress(message);
    if (IsKey(message, gameWindow, hotkey)) {
        ConsumeMessage(message);
        if (pressed && runState == RunState::Running) {
            WaitForCommand(gameWindow);
        } else if (pressed) {
            runState = RunState::Running;
            queuedSteps = 0;
        }
    } else if (runState == RunState::Stepping && IsKey(message, gameWindow, stepKey)) {
        ConsumeMessage(message);
        if (pressed) ++queuedSteps;
    }
    return next;
}

void DetachWindow()
{
    if (messageHook) UnhookWindowsHookEx(messageHook);
    messageHook = nullptr;
    gameWindow = nullptr;
    runState = RunState::Running;
    queuedSteps = 0;
}

void __cdecl GameEvent(const ThpracPluginGameEvent* event)
{
    const auto window = static_cast<HWND>(event->window);
    switch (event->kind) {
    case THPRAC_EVENT_GAME_READY:
        hotkey = ReadKey(event->config);
        break;
    case THPRAC_EVENT_FRAME:
        if (!window) break;
        if (gameWindow == window) {
            if (runState == RunState::Stepping) {
                // The SDK frame event precedes drawing. Wake the next message pump
                // so this frame can finish rendering before the game freezes again.
                runState = RunState::FrameReached;
                PostMessageW(window, WM_NULL, 0, 0);
            }
            break;
        }
        if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) break;
        DetachWindow();
        gameWindow = window;
        messageHook = SetWindowsHookExW(WH_GETMESSAGE, FilterMessages, nullptr, GetCurrentThreadId());
        if (!messageHook) OutputDebugStringA("thprac game_freeze: could not hook the game message queue\n");
        break;
    case THPRAC_EVENT_WINDOW_CLOSED:
        if (window && window == gameWindow) DetachWindow();
        break;
    }
}

const ThpracPluginCallbacks callbacks {sizeof(callbacks), THPRAC_PLUGIN_SDK_VERSION, nullptr, DrawSettings, GameEvent};
const ThpracExtension extension {sizeof(extension), "rueee-c7.game-freeze", nullptr, nullptr, nullptr,
    nullptr, nullptr, nullptr, nullptr, &callbacks};
}

extern "C" const ThpracExtension* __cdecl THPracExtensionQuery(uint32_t abi)
{
    return abi == THPRAC_EXTENSION_ABI ? &extension : nullptr;
}
