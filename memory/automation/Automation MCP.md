---
title: Automation MCP
type: note
permalink: phobos/automation/automation-mcp
tags:
- automation
- mcp
- screenshot
- input
---

# Automation MCP

权威文档：`docs/Automation-MCP.md`；源码 `src/Automation/`、`src/Commands/FrameByFrame.*`。

## 架构

`Agent → HTTP /mcp → Phobos.dll 内 Winsock IO 线程 → 有界 Dispatcher → 游戏主线程`

- 只有 `-PhobosAutomation` 才开启；默认监听 `127.0.0.1:18742/mcp`，端口可用 `PHOBOS_MCP_PORT` 覆盖。`PHOBOS_MCP_TOKEN` 必须是 32–256 个可打印字符，请求带 `Authorization: Bearer`。Token 不写日志、不提交。
- Automation 模式会跳过 Debug 版的 Debugger Notice 弹窗。
- Dispatcher 由游戏线程的 16ms Timer 消费，菜单、游戏内、暂停、GPU Disabled 都能工作；HTTP 线程不碰游戏对象和 SDL 资源。

## 工具

`game_status`、`game_key`、`game_mouse_move`、`game_mouse_click`、`game_mouse_drag`、`game_mouse_wheel`、`game_pause`、`game_step`、`game_resume`、`game_screenshot`

- 有副作用的请求必须带 `_meta`：`com.phobos/instance_id`（取自 game_status）和 `com.phobos/request_id`（每次新生成）。截图只需要 instance_id。结果不确定时不要用新 ID 自动重放。
- 输入走引擎队列：按键用 `WWKeyboardClass::Put_Key_Message`；鼠标按钮经私有 message-only 窗口送到原始 WndProc；移动用 `WWMouse::SetCoords`（**不移动桌面指针，也不发 WM_MOUSEMOVE**）；滚轮投递 `WM_MOUSEWHEEL`。`consumed` 恒为 null，队列排空不代表产生了游戏效果。
- `game_screenshot`：`source=ddraw` 复制 Primary 自有像素；`source=gpu` 按需离屏渲染并经独立 fence 读回。返回 PNG `ImageContent` 和元数据（`simulation_frame`、`gpu_present_id`、`backend`、`output_mode`、`letterbox` 等）。`crop` 用的是源图像素坐标。

## 使用陷阱（2026-10-29 实测）

- `game_status` **没有 `paused` 字段**；暂停状态看 `frame_step_enabled: true`。
- 暂停期间 sidebar UI 仍在处理输入：暂停时点滚动箭头一样会滚动，而且一次点击会连续滚多行（运行时也一样）。
- 页签按钮闪烁由时间驱动，暂停时也继续闪；ddraw 和 gpu 两次截图可能截到不同相位，只能按 best_effort 记录。见 [[经验 页签闪烁导致双源截图差异]]。
- 鼠标停在 cameo 上时连 DDraw 画面都没出现 tooltip；`SetCoords` 能否触发 tooltip 待 gamemd-reverse #11 查明。
- 客户端脚本不要命名为 `mcp.py`，见 [[经验 Python 脚本名遮蔽 mcp 包]]。
- 测试用 Python 环境：`Debug/AutomationTests/sdk2`（`mcp==2.3.0` + Pillow），不是仓库交付物。

## 已知缺口

pair/复合工具未实现；拖框未通过；原生全屏和其他 wrapper、真实设备丢失、FPS 与泄漏长测未完成。

## Relations

- part_of [[GPURender 索引]]
- used_by [[实机验收流程]]
