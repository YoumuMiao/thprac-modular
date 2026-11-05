# thprac 插件 SDK v1

SDK v1 运行在 RUEEE C7 插件版上。现有 ABI 2 插件沿用原入口。需要 SDK v1 的插件可在 `ThpracExtension::plugin` 里提供 `ThpracPluginCallbacks`，并通过原有的 `THPracExtensionQuery` 返回描述符。

## 编写插件

[SDK 示例](../extensions/sdk-example/entry.cpp) 展示完整接入方式。在 CMake 中用 `thprac_c7_extension` 注册目标，再编写 `extensions/descriptions/<目标名>.json`。同时发布 DLL 和说明。插件 ID 使用唯一的字母、数字、下划线、短横线或点组成。

| 接口 | 用法 |
| --- | --- |
| `games` | 以空指针结尾的游戏 ID 列表，例如 `{"th08", nullptr}`。字段本身为空表示所有受宿主支持的作品。 |
| `draw_settings` | 在说明 JSON 加上 `"settings": true`，用户即可在插件管理页展开设置。使用宿主提供的文本、复选框与整数配置接口。 |
| `game_event` | 接收游戏生命周期和绘制事件。按 `kind` 处理需要的事件。 |
| `hook_import` | 仅在 `GAME_READY` 中可用。按 DLL 名和函数名替换主游戏 EXE 的命名导入项，传回可继续调用的原函数。遇到已有 Hook 或缺少目标时返回 0。 |

插件设置按 ID 保存在本套程序的 `plugins/settings.ini`。游戏启动或附加时可读取同一配置。DLL 放在全局目录时，SDK 设置仍按当前宿主安装保存。窗口、设置、事件和绘制参数只在当前回调期间有效。插件应使用 C 调用约定，回调内处理自己的 C++ 异常。

## 游戏事件

- `GAME_READY` 在游戏适配初始化完成后发出，在初始化线程执行。可读取设置、安装 IAT Hook。
- `FRAME` 在 thprac 的游戏 GUI 帧结束前发出。可用 `overlay->draw_text_box` 画浮层。
- `WINDOW_CLOSED` 在已挂接的游戏窗口收到销毁消息时发出。
- `STAGE_STARTED` 接入 TH06–TH20 的 15 部整数作，包括 C7 支持的两个 TH19 可执行版本。在进入游戏、关卡或机体及模式变化时上报，普通帧不会重复发出同一状态。原有关卡启动钩点也会重置去重状态，让同关重试可以再收到事件。

插件可在管理页启停。正在运行的游戏会在进程退出时释放其 DLL 和 Hook。

## 试用

在插件管理中启用默认禁用的 `sdk_example.thp.dll.disabled`，展开设置并开启「显示游戏事件浮层」，再启动或附加任一支持的整数作。浮层显示最近一次关卡事件的关卡、机体和模式。TH08 还会通过 IAT Hook 统计计时调用，继续调用原始计时函数。

## 整数作字段

- 关卡是零起始的游戏索引。TH09 和 TH19 的故事模式报告故事轮次；独立对战没有故事关卡，返回 `-1`。
- 机体编号沿用该作的原生编号或现有的角色/子机组合索引。对战作报告 1P。
- `practice` 表示 thprac 练习生效，`replay` 表示回放。未识别的字段返回 `-1`。TH19 v1.10c 的回放标志当前返回 `-1`。
- SDK 提供的关卡事件范围是整数作。小数作和黄昏酒场沿用此前的 SDK 事件覆盖。
