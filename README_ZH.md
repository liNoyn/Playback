# Playback（实时串流分支）

[English](README.md) | [日本語](README.ja.md) | [简体中文](README_ZH.md)

<p>
  <a href="https://github.com/liNoyn/Playback/releases"><img src="https://img.shields.io/github/v/release/liNoyn/Playback?include_prereleases&style=flat-square" alt="最新发行版"></a>
  <img src="https://img.shields.io/badge/Minecraft%20Bedrock-1.26.51%20(Windows%20x64)-62b47a?style=flat-square" alt="Minecraft Bedrock 1.26.51, Windows x64">
  <img src="https://img.shields.io/badge/LeviLamina-26.51.*-7b68ee?style=flat-square" alt="LeviLamina 26.51">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-AGPL--3.0-blue?style=flat-square" alt="AGPL-3.0 license"></a>
</p>

这是 [wo55555/Playback](https://github.com/wo55555/Playback)（基于 LeviLamina 的 Minecraft **基岩版**回放录制与镜头编辑 MOD）的分支。
它让**录制可以在游戏内完成**，并在你游玩的同时把录像转换成 **Java 版 [Flashback](https://github.com/Moulberry/Flashback) 回放**。

- **不离开游戏即可录制**：除命令外，还有快捷键、屏幕指示和控制面板。
- **实时生成 Flashback 回放**：录像被送到 [FlashBridge](https://github.com/liNoyn/FlashBridge)，录制的同时就在转换。停止录制时，Flashback 的列表里已经有这段回放。
- **即时回放**：像 NVIDIA 即时回放一样持续保留最近 15 秒到 20 分钟，一个快捷键即可保存。
- **自动录制**：按世界名、服务器名、维度的规则自动开始录制。
- **回放库**：收藏、标签、备注、Flashback 状态，以及事后转换。
- **卡顿修正**：即使服务器卡顿，也按正常速度播放。

回放的播放与编辑（时间线、镜头关键帧、视频导出）沿用上游，没有改动。本分支与上游作者无关。

> [!WARNING]
> 预发布版本。下面的录制功能较新，在游戏内的测试还很有限，请为重要的世界和录像做好备份。仅支持 Minecraft 基岩版 1.26.51 / LeviLamina 26.51 线（上游的 `mc26.50` 线），Windows x64。

## 安装

1. 先按[上游指南](docs/getting-started.md)在 LeviLamina 26.51 客户端实例中安装上游 Playback（本分支会就地替换其文件）。
2. *（需要 Flashback 回放时）* 安装 [FlashBridge](https://github.com/liNoyn/FlashBridge/releases)：解压并运行 `install.ps1`，选择接收回放的、装有 Flashback 的实例。
3. 关闭 Minecraft，从 [Releases](https://github.com/liNoyn/Playback/releases) 下载最新 zip，解压后运行：
   ```
   powershell -ExecutionPolicy Bypass -File install-live.ps1
   ```
   这会替换所有装有 Playback 的 LeviLauncher 游戏版本中的 `playback.dll`、`manifest.json` 和语言文件。`mods\playback\data` 中的录像与设置会保留，原文件备份在 `%USERPROFILE%\.flashbridge\backup`（`install-live.ps1 -Restore` 可还原）。

没有 FlashBridge 时，除 Flashback 转换外的功能都可使用。

## 使用

| 操作 | 默认按键 | 命令 |
|---|---|---|
| 开始 / 停止录制 | `F8` | `/record start` / `/record stop` |
| 打开面板（先选择输出） | `Ctrl+F8`、`F6` | `/record panel`、`/record start playback\|both\|flashback` |
| 暂停 / 继续 | `Shift+F8` | `/record pause` |
| 添加标记 | `F7` | `/record marker [名称]` |
| 保存即时回放 | `F9` | |
| 即时回放 开 / 关 | `Shift+F9` | |
| 查看状态 | | `/record status` |
| 版本 | | `/playback version` |

在聊天输入或打开菜单时按键不会响应，所有按键都可在 `settings.json` 中修改。

录制时右上角会显示小型指示（时间、剩余空间、服务器卡顿时的 TPS、FlashBridge 状态、标记数），下方显示通知。位置、不透明度和内容可调，也可以隐藏指示以免出现在录屏里。

**控制面板（`F6`）**
- 录制：开始、停止、暂停、标记；本次录制的输出与转换时机；状态与剩余空间。
- 设置：默认输出、原录像的处理、时间修正、即时回放、自动录制规则、屏幕显示、磁盘上限、语言。
- 状态：FlashBridge 状态与最近结果、恢复未保存的录像、回收站、复制诊断信息。

### 输出模式

| 输出 | 保留内容 |
|---|---|
| 仅 Playback | Playback 录像（不转换） |
| 两者（默认） | Playback 录像和 Flashback 回放 |
| 仅 Flashback | Flashback 回放。Playback 录像只有在 FlashBridge 重新打开并验证了 Flashback 回放之后才会移入 `data\trash`，7 天后删除（也可改为一直保留或验证后立即删除）。转换失败或未验证时，原录像一定保留 |

转换时机可选**录制时**（停止的瞬间 Flashback 回放就完成）或**停止后**（游玩时没有额外负担）。未设置 FlashBridge 时只录制 Playback。

### 即时回放

在面板（`F6` →“设置”）或用 `Shift+F9` 开启。游玩时持续保留最近一段（默认 2 分钟），按 **`F9` 保存**为 `…-instant.zip`（标签 `instant`，按输出模式转换）。

- **可与录制并行**：录制中按 `F9` 只截取最近一段，录制不会停止；若在录制途中开启，则从下一次录制起生效。
- 保存在后台进行。保存的片段最多比设定长度长一个分段（分段为长度的 1/3 与 30 秒中较短者），因为每个分段都从完整的世界快照开始。
- 每个分段都会取一次世界快照，会带来周期性的短暂负载。如有影响，请调大 `instantReplay.chunkSeconds`。
- 磁盘占用上限为 `instantReplay.maxMegabytes`（默认 2048MB）。离开世界或退出游戏时缓冲会被丢弃，只保留你用 `F9` 保存的内容。

### 自动录制

在面板“设置”→“自动录制”中，按**世界名、服务器名、维度**添加规则（可用 `*` 和 `?`，如 `*.example.net`）。进入世界约 3 秒后，应用第一条匹配的规则：

| 动作 | 效果 |
|---|---|
| 录制 | 开始录制（输出按规则，未指定则用默认） |
| 询问 | 只通知“按 F8 开始录制” |
| 不录制 | 此世界不自动录制，也不运行即时回放 |

“为当前世界/服务器添加规则”可按所在位置生成规则。离开世界时录制照常停止。服务器名取自游戏的连接处理；取不到时，带服务器条件的规则不会匹配。

### 回放库

回放浏览器（主菜单 → Playback）新增**收藏**（星标）、**标签**和**备注**（可搜索，保存在 `data\library.json`，不修改回放文件）、**Flashback 徽标**和事后转换的 **Flashback 按钮**（可多选，原回放保留），以及收藏/已转换/未转换筛选。重命名或删除回放时，这些数据也会随之转移或删除。

### 卡顿修正

录像的每个 tick 都带有实际时间和网络包上标注的服务器 tick（`tick_info`）。FlashBridge 据此按**游戏时间**排列 Flashback 回放：10 TPS 的服务器也按正常速度播放，追赶的部分会被插值，卡顿区间会加上标记（也可选实际时间或按录制原样）。修正发生在转换时，录像本身不变。

含 `tick_info` 的录像无法被上游 Playback 读取。如需上游可读的录像，请开启“兼容格式”（`recording.compatibleFormat`），此时无法修正卡顿。

### 其他

- **标记**保存在录像中，会成为 Flashback 的标记。
- **崩溃恢复**：可发现崩溃遗留的录像（`data\temp`）并在面板中恢复（最多丢失最近 5 分钟）。
- **磁盘保护**：剩余空间低于 3GB 时警告，低于 1GB 时停止并保存录制。
- **语言**：英语、日语、简体中文（跟随游戏语言）。

## 设置

`mods\playback\data\settings.json`。在面板中的修改会立即保存，未知的键会保留。FlashBridge 的 `setup-live` 会写入 `liveStream`（旧版的 `live.json` 会在首次启动时导入）。示例见[英文说明](README.md#settings)。

## 已知限制

- 目前还没有暂停菜单中的按钮，请用 `F6` 打开面板。
- 新功能在游戏内的测试还不充分。请连同 `logs\latest.log`、FlashBridge 日志（`%USERPROFILE%\.flashbridge\logs\live.log`）和所用版本一起反馈问题。
- 只构建基岩版 1.26.51 / LeviLamina 26.51 线。用其他游戏版本录制的回放与上游一样不兼容。
- 播放与导出沿用上游的限制（回放是对客户端所见内容的重建，视频导出为实验性且无声，不支持 Vibrant Visuals）。
- 服务器附加包的自定义物品不会转换到 Flashback（仅支持原版物品表）。

## 与上游的区别

- 录像通过本机回环 TCP 实时送往本地转换器，录制从不等待网络（协议 2，转换器回报状态与验证结果）。转换器缺失或太慢时录制照常，改为转换导出的文件。
- 新动作 `tick_info`（位于每个 `next_tick` 之前），以及 `metadata.json` 中的 `markers` / `features` / `tickSource`。
- Recorder 缓冲模式、`ReplayCompactor`（从缓冲中截取即时回放）、`settings.json`、快捷键/屏幕指示/面板、回放库数据、自动录制规则。
- 以 C++23 构建（LeviLamina 26.51.7 的头文件使用 `std::to_underlying`）；`--bundle_ffmpeg=n` 可跳过 FFmpeg 的构建。

协议与构建说明见 [docs/live-stream.md](docs/live-stream.md)（英文），设计见 [docs/design/recording-ux-and-time-normalization.md](docs/design/recording-ux-and-time-normalization.md)（日文）。

## 从源码构建

需要 Windows x64 与 Visual Studio 2022 Build Tools（C++）；Git、xmake、LLVM 使用 `%USERPROFILE%\.toolchains` 中的版本。

```
powershell -ExecutionPolicy Bypass -File build-live.ps1
```

输出在 `bin\playback`。详见 [docs/building.zh-CN.md](docs/building.zh-CN.md)（上游）。

## 致谢与许可证

Playback 由 [wo555](https://github.com/wo55555) 及贡献者开发。本分支以其为基础，感谢上游、[LeviLamina](https://github.com/LiteLDev/LeviLamina) 项目，以及其回放格式被 FlashBridge 写出的 [Flashback](https://github.com/Moulberry/Flashback)。

Copyright (C) 2026 [wo555](https://github.com/wo55555) 及本分支贡献者。以 [GNU Affero General Public License v3.0](LICENSE) 发布，修改后的版本须继续以 AGPL-3.0 提供源码。第三方组件遵循各自的许可证，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和 `licenses/`。
