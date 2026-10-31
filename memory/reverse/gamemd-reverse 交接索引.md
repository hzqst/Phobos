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
| #14 | 雷达区非地图部分（外框、阵营标志、开关动画；小地图内容只预留矩形） | `sidebar-radar-gpu-handoff.md` | `6898733` | 已交付，Phobos 侧已实现（`1065de0e`），见 `src/Render/RadarGPU.md`；运行的 `gamemd.exe` 模式字段是 `+14B0`，不是 spawn IDB 的 `BAD3E8` |
| #15 | 底部命令栏（LSPACER/LENDCAP/BTTNBKGD/RENDCAP、Advanced Command Bar 按钮、目标 surface 与发布点） | `command-bar-gpu-handoff.md` | `3e2cb23` | 已交付，Phobos 侧已实现（`bfef2044`），见 `src/Render/CommandBarGPU.md`；交接 §4 的 ShapeButton 字段偏移有误，以 YRpp/机器码为准（ShapeData `+58`、UseSidebarSurface `+4C`、IsAlpha `+55`） |
| #16 | 战术区地形（帧流程与地形 surface 缓存、可见格与坐标变换、TMP 地块、光照、地形 surface 上的其他内容、对照基准与开图方法） | `terrain-gpu-handoff.md` | `443a5e7` | 已交付：复用 `4F4583` 回调采集；Tile 含覆盖物/弹坑/静态对象，不是纯 TMP 真值；颜色依赖完整 LightConvert shade 表与逐像素 ABuffer，TMP 自身用 Z 遮挡（悬崖/extra）；Phobos 侧已实现 C2–C3（`ad256099`、`f39de890`、`3b7b2237`），见 `src/Render/TerrainGPU.md` |
| #17 | 战术区 ABuffer（shroud/fog 写入路径与取帧、SHP 原始字节、写入公式、地图外轮廓、alpha shape、`4F4583` 对照可行性） | `abuffer-gpu-handoff.md` | `5ad221c` | 已交付，Phobos 侧已实现 shroud/fog（C4.1 `a79ea5e2`、C4.2 `7ff08512`），见 `src/Render/TerrainGPU.md`“ABuffer”；alpha shape 留到阶段 E。交接修正：ABuffer 为 16 位 WORD；FogOfWar 开时第一遍覆盖也用 FOG；掩码按邻格位未置位；写入不含高度；邻格查询不检查合法性（InvalidCell）。实测原版经 SHPReference 加载 SHROUD/FOG（各 96 帧），`6D8700` 回退判定与 spawn 一致 |
| #18 | 弹坑与 overlay（smudge 逐格穿插、overlay 本体/阴影两遍扫描、ZGradient、ZShape、墙/桥/Tiberium 变体） | `smudge-overlay-gpu-handoff.md` | `af42121`（PR #20） | 已关闭，Phobos 侧已实现：C5.1 弹坑 `283df71b`、C5.2a GPU 深度 `09cf144e`、C5.2b CPU 模型 `89773426`、C5.2c GPU overlay `43d1f790`，见 `src/Render/TerrainGPU.md`“弹坑”“Overlay”。动态发现：Z shape 裁剪起点可落在帧内，RLE 路径仍从第 0 列读；Z 字节是 Z shape 帧原始数据；纵向滚屏后原生只重画条带，对照要等完整重画（见 [[经验 原生 Tile 对照要等完整重画]]）；Rubble、桥格、deferred 记录未覆盖 |
| #19 | 静态 Terrain 对象与自身阴影、悬崖投影（`6D97D0`/`71C1B0`、`6D7C00`/`4802A0`/`547230`） | `terrain-objects-shadows-gpu-handoff.md` | `775ea29`（PR #21，merge `62effcb`） | 已关闭，Phobos 侧已实现 C6 `4c03a57d`，见 `src/Render/TerrainGPU.md`“Terrain 对象与悬崖投影”。Phobos `71C2BC` 在两条 palette 分支汇合之后，扩展 Palette 也覆盖 SpawnsTiberium 类型；本 mod 温带/雪地 tile 类型都没有 ShadowCaster、常规区间起点为 0，悬崖投影无法动态核对；原版树不开受损帧；`4D1890` 的 Terrain 记录未接入 |
| #22 | 阶段 D1：Tile 上的静态建筑（第 8 步 `6D3870 → 6D9920`，bib、部件、配色、Foundation ZShape）与 `4D1890` 建筑分支 | `buildings-tile-gpu-handoff.md` | PR #25（merge `6e39793e`） | 已关闭，Phobos 侧已实现 D1a/D1b `5851ff0b`、D1c `09967aac`，见 `src/Render/TerrainGPU.md`“建筑”“迷雾建筑” |
| #23 | 阶段 D2：mode 2 中动画/崩塌 Terrain（`6D8DB0` 流程与 Layer 排序、mode 2 的 Z/A 状态、原生 Composite 对照点） | `animated-terrain-composite-gpu-handoff.md` | PR #24（merge `f88955b4`） | 已关闭，Phobos 侧已实现 D2a `d507b7de`、D2b `15e41979`，见“动画 Terrain（mode 2）” |
| #26 | 阶段 D3 单位类对象：车辆/步兵/飞行器/弹体、体素管线、mode 2 公共调用表与 Composite 逐提交归因的观测协议 | `units-composite-gpu-handoff.md` | PR #29（merge `3e3bc710`） | 已交付，Phobos 侧未开始；建筑 VXL 炮塔/炮管与 SHP 炮塔混 VXL 炮管的 `Anims[9]` 显式重画随它实现 |
| #27 | 阶段 D3 建筑动态部件 `43DA80` 与 AnimClass（独立、建筑槽动画） | `anims-composite-gpu-handoff.md` | PR #28（merge `e9625f2e`） | 已关闭，Phobos 侧已实现 D3b-1 动画 `19bea7c9` 与 D3b-2 DoorAnim；原版 YR 没有设 DoorAnim 的建筑（只有 UnderDoorAnim/DeployingAnim），验收靠临时注入；半透明须按 Phobos 补丁混合（[[经验 半透明对照要考虑 Phobos 的 blitter 补丁]]） |

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
