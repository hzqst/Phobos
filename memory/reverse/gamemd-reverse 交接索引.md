---
title: gamemd-reverse 交接索引
type: note
permalink: phobos/reverse/gamemd-reverse-交接索引
tags:
- reverse
- gamemd-reverse
- handoff
- issues
---

# gamemd-reverse 交接索引

静态调查在 `hzqst/gamemd-reverse`（本地 `D:/gamemd-reverse`）进行：Phobos 侧把需要定位的内容写成 issue，用户在该仓库执行，产出 `docs/zh-CN/*-gpu-handoff.md` 和恢复的完整函数体（`src/gamemd/consolidated/`）。交接文档里的地址是入口线索，不是可直接部署 hook 的安全证明。

| Issue | 主题 | 交接文档 | 提交 | 状态 |
| --- | --- | --- | --- | --- |
| #5 | 光标绘制与背景恢复 | `cursor-gpu-handoff.md` | `2ee397b`、`62039f9` | 已关闭，已实现 |
| #6 | Sidebar 背景素材与布局 | `sidebar-gpu-handoff.md` | `d00548a` | 已关闭，已实现 |
| #7 | Sidebar 按钮 | `sidebar-buttons-gpu-handoff.md` | `f7476d9` | 已关闭，已实现 |
| #8 | Cameo 素材、列表、绘制链 | `sidebar-cameos-gpu-handoff.md` | `194cd7f` | 已实现 |
| #9 | Sidebar 文字、字体、字形 | `sidebar-text-gpu-handoff.md` | `b63a6fe` | 首批已实现 |
| #10 | Cameo 图标内 UIName | `sidebar-cameo-name-gpu-handoff.md` | `3b42068` | 已交付，Phobos 侧暂缓 |
| #11 | Tooltip 触发、文本、排版、目标 surface | `tooltip-gpu-handoff.md` | `3464e14` | 已交付，Phobos 侧已实现（`883e6dd0`），见 [[Tooltip 接入要点]] |
| #12 | 电力条 `PowerClass::Draw @63FB20`（素材、状态帧、坐标、时序） | `sidebar-power-gpu-handoff.md` | `5a75392` | 已交付，Phobos 侧已实现（`4996d87a`），见 `src/Render/PowerGPU.md` |
| #13 | 资金区（CreditsClass/TabData 数字、平滑滚动、Phobos 三个计数器） | `sidebar-credits-gpu-handoff.md` | `9002145` | 已交付，Phobos 侧已实现（`d3d6766c`），见 `src/Render/CreditsGPU.md` |
| #14 | 雷达区非地图部分（外框、阵营标志、开关动画；小地图内容只预留矩形） | `sidebar-radar-gpu-handoff.md` | `6898733` | 已交付，Phobos 侧已实现（未提交），见 `src/Render/RadarGPU.md`；运行的 `gamemd.exe` 模式字段是 `+14B0`，不是 spawn IDB 的 `BAD3E8` |

## issue 写法

沿用 #9/#10/#11 的结构：目标（含交付文档名，声明只做静态调查）→ 已有基线与边界（Phobos 基线提交、可复用的交接、现有 hook、范围外项）→ 调查入口表 → TODO 清单 → 文档验收要求 → 后续动态验收清单（本 issue 不执行）。issue 发布前需用户同意，因为会公开。

## 调查 IDB 与运行模块

- IDB：`gamemd-spawn.exe.i64`，输入 SHA256 `2f86e769…7d404`（来自 IDB 元数据）。
- 本机实际模块哈希与 IDB 输入不同，部署前要用运行模块字节重新核对 hook 窗口。
- Ares 为 3.0p1（SHA256 `4cbe28df…fca93a`）；静态交接里原版内嵌的 `Strip.Cameos[75]` 不是本机 Ares 的实际 cameo 列表。

## Relations

- part_of [[GPURender 索引]]
- informs [[Sidebar 元素状态]]
- informs [[Sidebar 文字与字形]]
