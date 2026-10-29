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

## 状态快照（截至 2026-10-29，`0d43c6c5`）

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| A | 黑底 + 独立 GPU 光标 | 已实现，D3D12 实机验证 |
| B | Sidebar | 进行中：背景、按钮、cameo、状态效果、cameo 文字、滚动已完成；资金、电力条、雷达区、命令栏、tooltip 未做 |
| C | 地图基础（地形、坐标变换） | 未开始 |
| D | 场景对象（建筑、SHP、步兵、载具、Voxel） | 未开始 |
| E | 效果与其余覆盖（阴影、粒子、迷雾、菜单、影片） | 未开始 |
| F | 接管主窗口与优化 | 未开始 |

另有 Automation MCP（内嵌 HTTP MCP、引擎输入、逐帧、ddraw/gpu 截图），用于自动化实机验收。

## 主题笔记

- [[GPURender 架构]]：模块、状态机、配置与环境变量、hook 分组、线程与提交模型
- [[帧缓存与 atlas]]：FrameCache、FrameID/PaletteID、多页索引 atlas、预算与生命周期
- [[Sidebar 元素状态]]：每个 sidebar 元素的实现与验收状态、剩余元素盘点
- [[Sidebar 文字与字形]]：GlyphCache、文字采集 hook、UIName 暂缓及接入方案
- [[GPU 光标]]
- [[Tooltip 接入要点]]：#11 结论、Automation 触发缺口、采集与发布方案
- [[Automation MCP]]：工具、元数据、截图语义、使用陷阱
- [[实机验收流程]]：隔离副本、构建部署、Spawner 场景构造、同帧截图对照方法
- [[gamemd-reverse 交接索引]]：静态调查 issue、交接文档与提交对照
- 经验：[[经验 Spawner 预置建筑不生效]]、[[经验 Python 脚本名遮蔽 mcp 包]]、[[经验 页签闪烁导致双源截图差异]]、[[经验 cameo 名称开关语义相反]]

## 仓库内权威文档

- `AGENTS.md`：GPURender/Automation 开发约束（必读）
- `src/Render/*.md`：FrameAtlasDesign、FrameAtlas、FrameCache、SidebarGPU、CameosGPU、CameoEffectsGPU、SidebarTextGPU、CursorTrace
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
