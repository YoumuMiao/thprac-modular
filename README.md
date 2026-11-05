# thprac-modular

RUEEE C7 练习器与启动器的插件版。

本项目以 [RUEEE C7](https://github.com/RUEEE/thprac/releases/tag/C7) 的提交 `17780056107a014e88d35fceb3ad86c121d94287` 为基线，程序版本为 2.2.2.7。EXE 提供 C7 的原有功能和插件加载接口。没有适用的扩展时，继续走 C7 自带的启动器和游戏适配。

## 插件启停

| DLL | 目录 | 功能 |
| --- | --- | --- |
| `game_adapter.thp.dll` | `plugins/game` | 各作的游戏适配、Hook 与界面渲染，供跨作品插件共用 |
| `launcher_layout.thp.dll` | `plugins/launcher` | 新版侧栏、封面、标题栏、窗口尺寸和 UI 缩放 |
| `status_hud.thp.dll` | `plugins/game` | 阶段血量、Boss 坐标、自机坐标，跟随各作 C7 的游戏内信息开关 |
| `capture.thp.dll` | `plugins/game` | 跨作品符卡及非符收率，沿用现有 `boss_capture_v2.dat` |
| `route.thp.dll` | `plugins/game` | F10 路线录制、回放参考、原生时钟对齐和定位，沿用 `thprac_route` 数据 |
| `game_volume.thp.dll` | `plugins/shared` | 启动器全局广播、游戏内 BGM 和音效音量、支持版本的原生音量同步 |
| `f11_settings.thp.dll` | `plugins/game` | F11 游戏内设置和窗口互斥，按已启用功能显示 HUD、音量、收率页 |
| `game_repairs.thp.dll` | `plugins/game` | TH06–TH08 的 DX8 与设置页重绘修补、TH07/TH08 的 OILP 补帧界面合成，以及 TH14 的旧弹收率归因修补 |
| `th15_practice.thp.dll` | `plugins/game` | TH15 的章节记录与重试、固定种子、专项路线和判定框 |

打开启动器的 **插件管理**。加载器同时扫描 EXE 旁边的 `plugins` 和当前用户的全局目录 `%APPDATA%\thprac\plugins`。两个目录都使用 `launcher`、`game`、`shared` 子目录。便携分发与 CMake 构建输出都把随包插件放在 EXE 同级的 `plugins`。完整解压后运行 `thprac.exe`，即可在启动器中管理插件。

同一插件 ID 只运行一个启用版本，程序目录优先。禁用程序目录中的版本后，全局版本可接替。管理页显示文件来源，启停分别保存到各目录的 `enabled.ini`。全局插件的开关由使用同一全局目录的程序共享。插件设置仍按本套 EXE 保存。旧的 `.thp.dll.disabled` 文件可在列表中恢复原名。命令行 `--list-plugins` 列出当前可加载的启动器插件 ID。

- 启动器工具入口随页面中的开关即时更新。
- 启动器布局在下次启动时切换。已挂接游戏中的插件和 Hook 随游戏进程退出，下次启动或首次挂接时读取新配置。
- 进程中已经加载的 DLL 会保持到进程退出。替换这些 DLL 文件时，先退出加载了它们的启动器或游戏。

保留 `plugins/game/game_adapter.thp.dll` 以使用跨作品的游戏内插件。它共用各作的原生地址、Hook、渲染和设备接线；收率计数与保存、路线对齐、音量广播、设置选页和修补裁剪分别由对应 DLL 提供。一个进程只初始化一份游戏适配。TH14 的旧弹归因修补在启用收率插件时参与计算。F11 负责设置界面，关闭它后其他已启用插件继续运行。

跨作品收率与路线覆盖 TH06、TH07、TH08、TH10–14、TH16–18、TH20。TH15 的记录与路线由专项 DLL 提供。全局音量按钮在启动器的工具页，面板数值保存在 `thprac_volume.ini`，便携模式存于 `.thprac_data`，否则存于 `%APPDATA%\thprac`。每次点应用才向运行中的游戏广播。

## 构建

在 MSVC x86 Native Tools PowerShell 中运行：

```powershell
./build-win32.ps1
ctest --test-dir build-win32 --output-on-failure
```

需要 CMake 3.25+、Ninja 和 Windows SDK。脚本可用 `-MsvcRoot` 指定本地工具链。构建输出位于 `build-win32/bin`，包含 `thprac.exe`、`thprac_compatible.exe` 和本地 `plugins`。分发时一起保留这些文件及插件说明。本版沿用扩展 ABI 2，新增 SDK v1 可选接口。开发方式见 [插件 SDK](docs/plugin-sdk.md)。

## 新插件的说明

调用 `thprac_c7_extension` 注册插件时，在 `extensions/descriptions/<目标名>.json` 写明用途和重要的操作条件。文件使用 UTF-8，大小不超过 8 KiB，必须包含非空的 `zh`、`en` 和 `ja` 字符串。构建会校验这些字段，并生成与 DLL 同目录的 `<DLL 文件名>.json`。发布插件时将这两个文件一起交付。插件管理页可在 DLL 禁用时读取说明。
