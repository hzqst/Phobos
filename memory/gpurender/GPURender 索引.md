---
title: GPURender 索引
type: note
permalink: phobos/gpurender/gpurender-索引
tags:
- gpurender
- index
---

# GPURender 索引

Phobos fork 分支 `feature/gpu-render`（主线 `develop`）。在游戏主窗口之外开一个 SDL3 + SDL_GPU 副窗口（D3D12 优先，Vulkan 回退），从纯黑背景开始，逐项用 GPU 重新绘制游戏元素；原生 DirectDraw 渲染与输入保持不变，作为行为和视觉对照。

本笔记只做导航与摘要。权威细节在仓库文档和源码里，用前先核对代码，不把文档中的目标当成已实现。

## 状态快照（截至 2026-10-30，`4c03a57d`）

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| A | 黑底 + 独立 GPU 光标 | 已实现，D3D12 实机验证 |
| B | Sidebar | 进行中：背景、按钮、cameo、状态效果、cameo 文字、滚动、tooltip、电力条、资金区、雷达区非地图部分、底部命令栏已完成；小地图未做 |
| C | 地图基础（地形、坐标变换） | 进行中：#16 已交付（`443a5e7`）；C1 对照基准 `source=native_terrain` 已提交（`4ad5b6fc`）；C2 TMP 地块（坐标、变体、主菱形与 extra、默认 ABuffer 光照，无 Z）已提交（`ad256099`）；C2.5 实例化绘制（每 atlas 页一次）+ PS 查整张 LightConvert 表已提交（`f39de890`）；C3 深度采集（Z 平面 + 基准深度 b）与 trace 中对原生 ZBuffer 的逐像素对照已实现，全部一致且纯地形里 Z 测试从不挡色，GPU 深度缓冲（D32_FLOAT 主、D16 后备）留到阶段 D（`3b7b2237`）；#17 已交付（gamemd-reverse `5ad221c`）；C4.1 ABuffer shroud/fog 的 CPU 整屏重建与原生 ABuffer 逐像素对照已提交（`a79ea5e2`）；C4.2 GPU ABuffer（R8_UNORM 纹理、独立渲染通道清 127、先全部覆盖再全部变暗 REVERSE_SUBTRACT，地形 PS 逐像素取 a）已提交（`7ff08512`）。C4.3 结论：D3D12 与 Vulkan 下地图上边缘/左上角暗带、开局黑幕、FogOfWar 迷雾的颜色与原生一致（只差光标与提示框）；GPU 顺序与原生逐格顺序实测无差（`split=0`）；原生无待重画格子（`stale=0`）时逐像素一致，单位移动时原生增量缓存滞后几帧而 GPU 每帧按当前状态完整重建，属于预期差异；采集 Debug 约 1.4 ms；alpha shape 与地形后的 Alpha 写入留到阶段 E。#18/#19 已交付并关闭。C5.1 逐格弹坑（`283df71b`）；C5.2a 战术区改画进 ViewBounds 尺寸离屏目标，带深度缓冲（D32_FLOAT，D16 后备），地块按 `547CF0` 测试并写 Z（`09cf144e`）；C5.2b overlay 本体/阴影采集，trace 中 CPU 重放整个 Tile 层并与原生 Tile、ZBuffer 逐像素核对（`89773426`）；C5.2c GPU 画 overlay，严格 `<` 管线、ZGradient/ZShape、阴影 `(c>>1)&mask` 混合（`43d1f790`）；C6 静态 Terrain 对象本体与自身阴影、悬崖投影接在同一绘制列表（`4c03a57d`）。D3D12/Vulkan 多张测试图 CPU 重放全一致、GPU 只差光标；悬崖投影因本 mod 无 ShadowCaster 无法动态核对。见 `src/Render/TerrainGPU.md`；剩余：`4D1890` deferred 记录、桥格/平铺帧/Rubble 实测，之后阶段 D |
| D | 场景对象（建筑、SHP、步兵、载具、Voxel） | 未开始 |
| E | 效果与其余覆盖（阴影、粒子、alpha shape、菜单、影片；地形迷雾已在 C4 完成） | 未开始 |
| F | 接管主窗口与优化 | 未开始 |

另有 Automation MCP（内嵌 HTTP MCP、引擎输入、逐帧、ddraw/gpu 截图），用于自动化实机验收。

## 主题笔记

- [[GPURender 架构]]：模块、状态机、配置与环境变量、hook 分组、线程与提交模型
- [[帧缓存与 atlas]]：FrameCache、FrameID/PaletteID、多页索引 atlas、预算与生命周期
- [[Sidebar 元素状态]]：每个 sidebar 元素的实现与验收状态、剩余元素盘点
- [[Sidebar 文字与字形]]：GlyphCache、文字采集 hook、UIName 暂缓及接入方案
- [[GPU 光标]]
- [[Tooltip 接入要点]]：#11 结论、Automation 触发、GPU 采集/发布实现与验收（详见 `src/Render/TooltipGPU.md`）
- [[Automation MCP]]：工具、元数据、截图语义、使用陷阱
- [[实机验收流程]]：隔离副本、构建部署、Spawner 场景构造、同帧截图对照方法
- [[gamemd-reverse 交接索引]]：静态调查 issue、交接文档与提交对照
- 经验：[[经验 Spawner 预置建筑不生效]]、[[经验 Python 脚本名遮蔽 mcp 包]]、[[经验 页签闪烁导致双源截图差异]]、[[经验 cameo 名称开关语义相反]]、[[经验 测试地图缺 OverlayPack 画出满屏围墙]]、[[经验 用 Visibility 与 Foggedness 判别 ABuffer 差异来源]]、[[经验 原生 Tile 对照要等完整重画]]

## 仓库内权威文档

- `AGENTS.md`：GPURender/Automation 开发约束（必读）
- `src/Render/*.md`：FrameAtlasDesign、FrameAtlas、FrameCache、SidebarGPU、CameosGPU、CameoEffectsGPU、SidebarTextGPU、TooltipGPU、PowerGPU、CreditsGPU、RadarGPU、CommandBarGPU、TerrainGPU、CursorTrace
- `docs/Automation-MCP.md`
- 根目录 `GPURender-Progress.md`：用户本地交接日志（未入库，按时间倒序追加，很长，优先查本 memory）

## Observations

- [goal] 最终目标是游戏全部内容改由 GPU 绘制；当前是副窗口验证阶段
- [rule] independent 模式下未迁移区域为黑色是正常的；禁止复制 DDraw/桌面画面来掩盖缺失
- [rule] 迁移顺序不等于绘制层级，GPU 光标始终在最上层

## Relations

- contains [[GPURender 架构]]
- contains [[Sidebar 元素状态]]
- contains [[Automation MCP]]
- contains [[实机验收流程]]
