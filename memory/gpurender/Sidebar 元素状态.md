---
title: Sidebar 元素状态
type: note
permalink: phobos/gpurender/sidebar-元素状态
tags:
- gpurender
- sidebar
- status
- inventory
---

# Sidebar 元素状态

原生绘制顺序（gamemd-reverse `sidebar-gpu-handoff.md` §3）：`CREDITS.SHP → SIDE1/SIDE2×N/SIDE3/ADDON → 按钮 → 可选叠层 → Strip(cameo) → Power → Radar/TOP → Tooltip`。

## 已实现

| 元素 | 提交 | 验收 | 文档 |
| --- | --- | --- | --- |
| 静态背景（CREDITS、SIDE1-3、ADDON、TOP） | `78f928af` | D3D12 用户对照；只覆盖 side=0 | `SidebarGPU.md` |
| 十个按钮（卖/修、四页签、上下滚动、简报/外交、选项） | `0514cbac` | D3D12 用户对照 | `SidebarGPU.md` |
| 基础 cameo（Ares 3.0p1 动态列表） | `bfb7551c` | D3D12 | `CameosGPU.md` |
| 状态效果：GCLOCK2 时钟、暗化、闪烁、hover | `2128e6c4` | 时钟/hover 由用户对照；暗化在滚动验收中同帧对照 | `CameoEffectsGPU.md` |
| cameo 文字：数量、READY/HOLD、超武 readiness | `f592c1c1`、`c71f3f20` | 用户对照 + 字形生命周期专项 | `SidebarTextGPU.md` |
| 逐行滚动、页签切换 | 无代码改动，`0d43c6c5` 记录 | D3D12 与 Vulkan 同帧 ddraw/gpu 对照 | `CameosGPU.md` 滚动小节 |

滚动验收数据：无叠层时图标格子最大色差 1；暗化/时钟/READY 最大 8；HOLD 与时钟叠加处最大 10，属于已接受的 alpha blending RGB565 舍入误差。

## 未实现（2026-10-29 盘点）

| 元素 | 位置（1024×768） | 原生来源 | 备注 |
| --- | --- | --- | --- |
| 资金数字 | `(920,0)-(960,16)` | TabData；CreditsClass `4A25E0` | Phobos 在此挂了收割机/电力差值/杂草三个计数器（默认关） |
| 电力条 | 左侧细条 `x≈860–872` | `PowerClass::Draw @63FB20` | SHP 状态帧，可复用 atlas |
| 雷达区 | `(856,48)-(1024,160)` | `RadarClass::Draw @653100` | TOP 和两个按钮已做；缺阵营标志、开关动画、小地图内容（小地图依赖阶段 C） |
| 底部命令栏 | `(0,736)-(856,768)` | `TabClass::Draw @6D0A20` 画到 Temp surface | LSPACER/LENDCAP/BTTNBKGD/RENDCAP 与命令按钮 |
| Tooltip | 跟随鼠标 | CCToolTip `478E10..479029` | #11 已交付（`3464e14`），方案见 [[Tooltip 接入要点]]；Automation 目前无法触发新 tooltip |
| UIName（图标内名称） | cameo 内 | `6A9BEC → 6AC480` | 暂缓：原生开关默认 0，当前配置不显示；方案见 [[Sidebar 文字与字形]] |
| `self+5398` 可选叠层 | — | 三个 SHP `B0B478/47C/480` | 素材身份未识别 |
| Phobos 扩展 | — | ProducingProgress `6A6EB1`、SW 侧栏、计数器 | 默认关闭 |

建议顺序：电力条 → 资金数字（含 Phobos 计数器）→ 雷达区不依赖地图的部分 → 命令栏。

## 未验证

- 平滑滑动（IsScrolling/Slid）：当前配置下滚轮和箭头都是整行跳变，只有 CameoLayoutTests 覆盖
- 其他阵营、observer、步兵/载具页滚动、分辨率变化、第二局滚动
- 完成闪烁的实机对照

## Relations

- part_of [[GPURender 索引]]
- depends_on [[帧缓存与 atlas]]
- related [[Sidebar 文字与字形]]
- verified_by [[实机验收流程]]
