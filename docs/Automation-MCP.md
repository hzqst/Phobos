# Automation MCP（开发中）

此文档记录当前实现，不能视为最初八阶段需求的完整验收报告。

## 当前架构与启用

`Agent → HTTP /mcp → Phobos.dll 内 Winsock IO 线程 → 有界 Dispatcher → 游戏主线程`。
服务器只在 `-PhobosAutomation` 模式开启，监听 `127.0.0.1:18742/mcp`。
`PHOBOS_MCP_PORT` 可覆盖端口；`PHOBOS_MCP_TOKEN` 必须提供 32–256 个可打印 ASCII
字符，请用密码学随机数生成并通过环境传入。请求使用 `Authorization: Bearer ...`。
Token 不写入日志。未启用此参数时不启动服务、Dispatcher Timer 或输入状态覆盖。
Automation 模式跳过 Debugger Notice 启动弹窗，普通 Debug 模式保留原行为。

初始化发生在 `GPURender::OnVideoModeSet` 的独立 Automation 入口，支持 GPU Disabled。
Dispatcher 通过游戏线程的 16ms Win32 Timer 消费命令，不在 HTTP 线程读取引擎对象。
`DllMain` 不启动网络线程。退出时取消 Dispatcher、释放保持键、关闭网络服务。

编译沿用 Visual Studio 2022 / v143 / Win32 / C++20 / 静态 CRT。新增依赖仅为
`lib/Automation/json.hpp`（nlohmann/json 3.12.0，MIT），网络层使用系统 Winsock，
实例随机 ID 使用 BCrypt。构建命令：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' Phobos.vcxproj /p:Configuration=Debug /p:Platform=Win32 /m /v:minimal /nologo
```

不要把构建产物自动覆盖到实际游戏安装。本次实机测试使用 `Debug/AutomationGame` 隔离副本，
不复制测试存档；仅在该目录部署生成的 DLL/PDB。

## 协议与工具

提供 2026-07-28 无握手 `server/discover`、`tools/list`、`tools/call`，以及独立的
2025-11-25 `initialize` / `notifications/initialized` 会话路径。响应为 HTTP JSON。
不声明 SSE、任务扩展或服务端通知能力。实现 Host、Origin、Bearer、版本与
method/name header 对照校验，限制 body、JSON 深度、连接数和排队数。

当前工具的稳定顺序：

| 工具 | 参数 |
|---|---|
| `game_status` | 无 |
| `game_key` | `key`, `action: tap/press/release`, `hold_ms: 32..2000`（默认 64） |
| `game_mouse_move` | `x`, `y`, `mode: absolute/relative` |
| `game_mouse_click` | `x`, `y`, `button: left/right/middle`, `click_count: 1..2`, `hold_ms` |
| `game_mouse_drag` | `start_x`, `start_y`, `end_x`, `end_y`, `button`, `duration_ms: 64..5000`, `steps: 2..128` |
| `game_mouse_wheel` | `delta: -1200..1200`（120/格），可同时提供 `x`, `y` |
| `game_pause` | 无；等待引擎到达暂停循环后确认 |
| `game_step` | `frames: 1..10000`, `timeout_ms: 100..28000`（默认 5000） |
| `game_resume` | 无；等待正常逻辑帧完成后确认 |

拖动每步间隔至少 16ms。坐标为原始游戏逻辑画面坐标，越界拒绝。
保持键有 10 秒租期，超时、引擎不可用、模式/分辨率变化及退出时释放。
输入动作串行执行，竞争返回 `input_busy`，不会覆盖进行中的拖动。

变更请求必须携带 `_meta`：

```json
{
  "com.phobos/instance_id": "game_status 返回的本次实例 ID",
  "com.phobos/request_id": "调用方生成的唯一操作 ID"
}
```

相同 ID、相同参数返回已排队请求的结果；相同 ID 换参数会拒绝。缓存有界，不能用作永久
幂等存储。响应丢失时不要生成新 ID 自动重放点击。游戏重启后必须重新获取实例 ID。

## 内部输入路径

实现位于 `src/Automation/AutomationInputWin32.cpp`（保留早期文件名，内容已替换）。
生产代码不再使用 `SendInput`、`SetCursorPos`、`SetForegroundWindow` 或 `AttachThreadInput`。

* 游戏内按键调用 `WWKeyboardClass::Put_Key_Message`，预先检查环形队列容量。
  引擎可能返回 success 却抑制事件，因此同时检查 Tail 是否推进。
* 游戏内鼠标按钮经专用 message-only HWND 异步送到游戏线程，调用原始引擎 WndProc。
  它先执行地图选择/拖框预处理，再写入 WWKeyboard 的按钮及坐标队列；直接写入队列会绕过
  必需的预处理。私有窗口只转发白名单内的按钮和滚轮消息，不展示、不取得输入焦点。
* 自动化保持键由自己的状态表管理。引擎 `GetKeyState` / `GetAsyncKeyState` 包装函数
  在 Automation 开启时读取该表，使 CTRL/SHIFT 和持续鼠标按下不依赖系统物理按键。
  这也意味着 Automation 模式不应与人工操作混用。
* 鼠标移动在短时持有 `MouseMutex` 时调用 `WWMouse::SetCoords`，不移动桌面指针。
  鼠标绘制线程使用原有读取 XY1 的分支，避免重新从桌面坐标覆盖它；不启用录制/回放模式。
* 主菜单/单人菜单是 owner-draw HWND 控件。纯 WWKeyboard 事件会被取走却不触发按钮，
  因此点击投递到活动对话框中的目标控件，由引擎自己的消息泵处理。
  两个前端菜单的方向键/TAB 使用独立的按钮导航目标，ENTER/SPACE 和 ESC 投递原生菜单命令。
  这是显式菜单适配，不是对所有对话框冒充物理键盘。其他对话框仍使用其普通键盘消息路径。
* 滚轮投递到原始游戏窗口的 `WM_MOUSEWHEEL` 处理路径，复用原有 sidebar 命令。
  滚轮没有 WWKeyboard 环形队列编码。

结果包含 `backend: engine_queue`、`input_route: wwkeyboard/wwmouse/engine_window_queue/native_window_queue/native_dialog_navigation`、
`injected`、`queue_drained`、`consumed: null` 和最终逻辑坐标。**队列排空不等于预期游戏效果**，
场景切换也可能清空队列。`consumed` 不会仅因 API 成功或队列清空而变成 true。
tap/click 在按下事件离开队列之后才继续释放，所有等待受 Dispatcher 超时约束。

`game_status.input` 提供 `os_foreground`、引擎是否可消费输入、队列长度、实际通过
`WWKeyboard::Down` 查询的保持键，以及活动对话框 ID。`game_focused` 是引擎自己的状态，
与 OS 前台窗口不同。

后台支持需要引擎继续运行。测试使用 windowed DirectDraw wrapper，`noactivateapp=true`。
原生全屏失焦后可能停在引擎的 focus wait；此时返回 `engine_input_unavailable`，不会抢焦点
或伪造引擎激活状态。游戏自身在创建/关闭菜单时仍可能调用 SetForegroundWindow；这与注入
是否需要前台是两回事，目前没有屏蔽该游戏行为。多人对局中当前输入后端会拒绝操作。

## Hook 与地址依据

| Hook | 范围 | 自动化分支 | 检查 |
|---|---|---|---|
| `Automation_KeyState` | `53EC70..53EC75` | EAX=虚拟状态，返回 `53EC80` 的 RET | 大小、冲突、边界、ECX 参数、栈深度通过 |
| `Automation_AsyncKeyState` | `53EC90..53EC95` | 同上，返回 `53ECA0` | 同上 |
| `Automation_MouseCoordinates` | `7BA0D0..7BA0D5` | 返回 `7BA0D9`，读取 XY1 | 同上；两处栈深度均为 -20 |

普通模式全部 return 0。第三个 Hook 覆盖 `call 53E720`，由 SyringeEx 在 trampoline 中
重定位。检查使用技能目录中的 `discover_hooks.py`、`check_hook_conflicts.py`，并对
IDA 中的 gamemd.exe 逐项核对；脚本自动提取遗漏的条件返回地址已另行检查，无冲突。

新增数据引用 `B72F44` 是 UI 活动对话框栈顶：`UI::BeginDialog@622650` 在 `6226F9`
写入，`UI::EndDialog@622720` 在 `6227B7/6227DE` 恢复或清空。
`OwnerDraw::CurrentDialogHwnd` 是布局缓存，不能替代该栈顶。
菜单资源 226/256 及控件 1006/1670 分别由 `531F60`、`52D640` 的 WM_COMMAND 分支核实。
未修改 YRpp。

原始引擎窗口函数 `7775C0` 的调用约定由函数末尾 `777624: ret 10h` 核实。
绕开 wrapper 对后台 OS 输入消息的外层过滤，仍保留原始函数内部的 Hook。

## 逻辑帧控制

复用 `FrameByFrameCommandClass` 的单人暂停循环及既有 `55D360` 入口和
`55D871/55DEC1/55DED5` 返回 Hook，不新增逻辑循环地址 Hook。
只有完整执行路径在 `55DE81` 更新引擎帧号，自动化在返回处核对该增量后累计
`simulation_frame`，提前退出不计数。它与 GPU Present 次数独立；`scene_id` 标识场景代际。
暂停必须观察到暂停循环屏障，Step 必须完成指定帧数并重新进入屏障，Resume 必须观察到
至少一个正常逻辑帧。返回 `start_frame/end_frame/actual_frames/completed`。
活动 Step 期间拒绝另一个 Step，并禁止开发快捷键覆盖计数；Resume 可以取消旧请求。
超时返回实际完成数量并清空剩余步数，场景变化会终止旧请求。

IDA 检查上述入口覆盖 5 字节完整指令，三个返回处各覆盖 6 字节完整指令；
正常 return 0 继续执行原指令，暂停跳转 `55DEDB` 与入口的栈深度均为 0。
发现脚本未报告只修改函数体的既有 Hook，已补充人工核对边界及栈平衡。

## 验证与当前缺口

```powershell
cmd /c scripts\test_automation.cmd
& .\Debug\AutomationTests\sdk2\Scripts\python.exe tests\Automation\test_http.py
& .\Debug\AutomationTests\sdk2\Scripts\python.exe tests\Automation\game_input_smoke.py
# 可选、隔离副本内的遭遇战测试：
$env:PHOBOS_TEST_SCENARIO = '1'
& .\Debug\AutomationTests\sdk2\Scripts\python.exe tests\Automation\game_input_smoke.py
```

Standalone EXE 嵌入 `asInvoker` manifest，无需管理员权限。
测试覆盖协议、Dispatcher 以及按键时序/租期/取消/队列容量/鼠标三元素环绕/抑制事件。
Python 标准 MCP SDK 2.3.0 运行实际 HTTP 客户端，另有 SDK 1.25.0 兼容测试环境。

实机测试启动自己的小窗口，把游戏放到后台后再发请求；不会把游戏激活作为输入前提。
证据截图通过临时 `SWP_NOACTIVATE` 置顶显示游戏取得，是诊断图，**不是 MCP 原生截图**。
脚本只清理由自己启动且通过认证确认 PID 的游戏。遭遇战模式结束后清理隔离进程，不能据此
声称正常退出测试通过。

证据目录：`Debug/AutomationTests/game-input-native`（菜单），
`Debug/AutomationTests/game-input-scenario`（可选遭遇战）。详细调用、输入路径、调用前
前台 HWND、对话框状态、截图与退出结果保存在该目录。失败的测试不会标记为 PASS。

隔离遭遇战已验证 `N` 选中对象类型为 `AMCV`，`D` 展开后截图出现建造厂及电厂 cameo。
暂停、暂停期间输入、连续三次 Step(1)、Step(5)、暂停帧号不再增长、Resume 后帧号增长
已在 GPU Disabled 和 GPU Running 的实机运行中通过（结果文件含 `frame_control_live_passed`）。
GPU 开启时的证据在 `Debug/AutomationTests/game-input-scenario-gpu`。
拖框尚未通过；滚轮请求完成不代表 sidebar 实际滚动，仍需具有足够条目的场景验证。

尚未实现 DDraw/GPU 原生截图、pair 与复合工具；
`gpu_present_id` 等未接入字段仍为 null。正式 MCP ImageContent、
GPU readback 和不同 GPU 后端验证均未完成。请勿把此阶段菜单输入验证当作整个 MCP 项目验收。

关闭方式：移除 `-PhobosAutomation`，按原来的方式启动 Phobos。
