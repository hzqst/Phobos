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
| 资金数字 | sidebar 顶部 `(856,0)-(1024,16)`，原生中心 W/2 | `TabData::Draw @4A2370`，Fancy 返回 `4A25E0` | 已实现（`d3d6766c`）：复用 `4A5EB0/4A5FF2/434120` 观察器，按栈链识别资金数字，三个 Phobos 计数器靠 `4A25E0` 回调里的 `CounterBatch/CounterScope` 标记识别，无新 hook；D3D12/Vulkan/NOD 稳定最大差 1，滚动用三张截图；见 `src/Render/CreditsGPU.md`、gamemd-reverse #13 |
| 电力条 | 左侧细条（GDI x=5、NOD x=0，y0=SidebarBounds.Y+69） | `PowerClass::Draw @63FB20` | 已实现（`4996d87a`）：`6A70AE` 快照读 G/Y/D/B（`+152C/1530/1534/151C`）与 L（`B0B504`）重建 POWERP 帧序列，无新 hook；D3D12/Vulkan/NOD 同帧最大差 1；见 `src/Render/PowerGPU.md`、gamemd-reverse #12 |
| 雷达区 | `(856,48)-(1024,160)` | `RadarClass::Draw @653100` | TOP、两个按钮、主体（阵营标志、开关动画、帧 32 外框）已实现（`1065de0e`）：`6A70AE` 快照读 `+14AC/+14B0/+14FC`、`[B04A38]`（须等于 `[B0FA68]`）与 `[B0FBF8]`，无新 hook；GDI/NOD/Yuri 与 Vulkan 稳定最大差 1，动画用连拍池匹配；见 `src/Render/RadarGPU.md`、gamemd-reverse #14。缺小地图内容、边框线、视口框（依赖阶段 C），统计/电影模式不画 |
| 底部命令栏 | `(0,736)-(856,768)` | `TabClass::Draw @6D0A20` 画到 Composite | 已实现（2026-10-30，未提交）：`6A70AE` 回调单独采集，读 `Tab+5544`、矩形 `B0FC58..6C`、映射 `B0CB78`、对象 `B0C1C0`、端帽按钮 `B0CCB0/B0CC40`，每次按当前状态重建，无新 hook；GDI/NOD/Yuri、Vulkan、800×600 的展开/折叠/按下/PlanningMode 最大差 1；见 `src/Render/CommandBarGPU.md`、gamemd-reverse #15。侧栏在左、Disabled/闪烁帧未验证 |
| Tooltip | 跟随鼠标 | CCToolTip `478E10..479029` | 已实现（2026-10-29，ExtendedToolTips 关闭）：Sidebar/Composite 两种目标，D3D12/Vulkan 同帧对照最大差 1；见 [[Tooltip 接入要点]]、`src/Render/TooltipGPU.md` |
| UIName（图标内名称） | cameo 内 | `6A9BEC → 6AC480` | 暂缓：原生开关默认 0，当前配置不显示；方案见 [[Sidebar 文字与字形]] |
| `self+5398` 可选叠层 | — | 三个 SHP `B0B478/47C/480` | 素材身份未识别 |
| Phobos 扩展 | — | ProducingProgress `6A6EB1`、SW 侧栏、计数器 | 默认关闭 |

建议顺序：sidebar 与命令栏的静态部分已全部迁移；剩余小地图内容（依赖阶段 C）、`self+5398` 叠层与 Phobos 扩展。

已知风险：SIDE2 片数此前按 `PlayerSideIndex` 计算，2026-10-29 改为 Phobos `isNODSidebar`（与 `6A51E9` 一致）；`Sidebar.GDIPositions` 与阵营不一致的配置未实机验证。

## 未验证

- 平滑滑动（IsScrolling/Slid）：当前配置下滚轮和箭头都是整行跳变，只有 CameoLayoutTests 覆盖
- 其他阵营、observer、步兵/载具页滚动、分辨率变化、第二局滚动
- 完成闪烁的实机对照

## Relations

- part_of [[GPURender 索引]]
- depends_on [[帧缓存与 atlas]]
- related [[Sidebar 文字与字形]]
- verified_by [[实机验收流程]]
