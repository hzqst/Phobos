---
title: Tooltip 接入要点
type: note
permalink: phobos/gpurender/tooltip-接入要点
tags:
- gpurender
- tooltip
- automation
- sidebar
---

# Tooltip 接入要点

依据 gamemd-reverse #11 `docs/zh-CN/tooltip-gpu-handoff.md`（`3464e14`，16 个完整函数体在 `src/gamemd/consolidated/Tooltip*.cpp`）。

进度（2026-10-29）：第一阶段 Automation 触发已完成，D3D12 隔离遭遇战中 cameo、页签、地图建造厂的 tooltip 都能在 DDraw 原生截图中出现，右键可隐藏；见 [[Automation MCP]]。第二阶段 GPU 采集与绘制未开始。

## 触发与 Automation

- `ToolTipManager::ProcessMessage @724200` 在消息泵 `5D4D50` 里先于 Translate/Dispatch 调用；只在 IsActive==1 时处理。它用 `GetCursorPos`+`ScreenToClient` 取坐标（调用点 `7242A2`/`72435D`，各 6 字节 `FF15C0137E00`；随后 `7242AD`/`724368` 是 ScreenToClient），不读 lParam。
- Automation 的 `SetCoords` 不发 WM_MOUSEMOVE，按钮直接调 WndProc `7775C0`，都到不了 `724200` → **当前无法触发新 tooltip**。需要：移动/按钮后经游戏线程通知 `724200` 一次（只处理一次），并在两处坐标读取上做虚拟点适配（Timer 延迟分支必须用虚拟点）。
- 计时：WM_MOUSEMOVE（Delay≠0）重启 Timer 并隐藏；Timer `54544950h` 到期且无 CurrentToolTip 时命中并 Process；按钮 DOWN/UP 隐藏；滚轮、键盘不经此函数隐藏。默认 Delay 1000ms、LifeTime 10000ms；cameo OnMouseEnter `6AB990` 把 Delay 设 0，OnMouseLeave `6AB9E0` 恢复。Timer 走 USER32 消息时序，与逻辑帧无关。

## 文本

- `Process @724AD0` 用 CSF label 或虚表 `+14h` 取文本，`wcsncpy` 256 到 manager `+28h`（满长时不一定有终止符）。
- Phobos ExtendedToolTips（uimd `[ToolTips]`，默认 false）开启时，HelpText 只是 `L"X"`，真正文本在 `478EE1` 换成 `PhobosToolTip::TextBuffer`；所以要在替换之后复制最终文本。

## 布局与绘制（Draw2 `478E30`）

- 目标：SidebarMode=1 时，`x+w<=V` 画 Composite（x 不变）；`x>=V` 且 FullRedraw==1 画 Sidebar（局部 x−V）；其他跳过。V=ViewBounds 右边界。
- 度量 `433CF0`，`bw=mw+8`、`bh=mh+4`；横向越界时 `left=width-bw`。底色 FillRect(rect,0)，再 DrawRect(rect, packed `B0FA1C` 颜色) 画边框；文字 `47903C → 434CD0`，参数 `left+4, top+2, bw, bh, 0,0,0`。
- Phobos `478FDC` hook：只要有 SideExt 就改用 FillRectTrans（默认黑色、opacity 100 即不透明，0..100 单位），可选 blur；锚定模式会改矩形。`478E4A` 在 SlaveDraw 时强制 Composite；`478F52` 只在横向溢出分支加 Sidebar 宽。
- 每帧调用两次：Sidebar `6A708D`（true 上下文，只 Draw）和 DrawOnTop `4F4570`（false 上下文，Process 后 Draw），共享一个 CurrentToolTip，可能画到不同 surface。

## 采集方案（建议）

- `434120` 观察器加独立 tooltip 分支：direct return `434EA6/4350E1/4352BA`，`[B+54]=479041`，Draw2 入口 `T=B+BCh`；最终文本指针在 `[B+98]`。
- 新增 Draw2 作用域：入口 `478E30/7`（`8B542404 83EC2C`），出口 `479042/6`（`5F5E5B83C42C`，此时 T=ESP+38h）。在 Phobos 已有 `478EE1`、`478FDC` hook 内部加只读观察，不另覆盖。
- 每帧空 pending，DrawOnTop tooltip 之后发布（`4F4573` 之后；现有 `4F4583` hook 位置合适），GPU 光标最后。WM_PAINT 路径不重画，按代际复用或隐藏。

## Relations

- part_of [[Sidebar 元素状态]]
- related [[Automation MCP]]
- documented_in [[gamemd-reverse 交接索引]]
