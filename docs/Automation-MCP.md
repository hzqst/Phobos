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

Voxel 采集性能调查可在启动进程前设置 `PHOBOS_VOXEL_PROFILE=1`，并关闭 Terrain trace；
它只输出有界的 `[VoxelP0]` 日志，不增加 MCP 工具。指标范围、独立取图微基准和首轮
实机证据见 [VoxelIdentityP0](../src/Render/VoxelIdentityP0.md)。2026-10-31 的 P0 测试按用户要求
使用自带 `Launch-Phobos.cmd`，在单独授权后临时部署到其实际指向的 `D:/Ra2`，测后恢复；
该次授权不代替未来任务对正式安装的部署许可。

`PHOBOS_VOXEL_IDENTITY_SHADOW=1` 开启 P1/P2 身份旁路验证，输出有界的 `[VoxelP2]`
计数。它仍执行原有像素复制／哈希，不能用其耗时代表优化后的性能；不改变 MCP 协议。
身份范围、回退规则、Hook 与验收记录见 [VoxelIdentity](../src/Render/VoxelIdentity.md)。

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
| `game_screenshot` | `source: ddraw/gpu/native_terrain/native_composite`, `max_edge: 0..4096`（默认 1280）, `include_metadata`（默认 true）, `timeout_ms: 100..28000`（默认 5000）, 可选 `crop: {x,y,width,height}` |

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
截图只要求 `com.phobos/instance_id`，每次调用创建新的 capture ID，不复用旧图片或写入输入请求缓存。

## 原生截图

`game_screenshot` 返回标准 MCP `ImageContent`（`image/png`、base64），另外返回 `structuredContent`。
例如 SDK 调用的工具参数：

```json
{"source":"ddraw","max_edge":1280,"include_metadata":true,"timeout_ms":5000}
```

```json
{"source":"gpu","max_edge":0,"crop":{"x":856,"y":0,"width":168,"height":768}}
```

裁剪坐标是**对应来源的原始图片像素**，先裁剪再缩小，不扩大图片；GPU 窗口缩放后它可能
不同于游戏逻辑坐标。`max_edge=0` 保留原尺寸。当前源图上限为单边 4096、总计
4096×2160 像素，编码后 PNG 上限 12 MiB。默认缩略图避免大量图片数据。
同一时刻只允许一个截图／编码任务，竞争返回 `capture_busy`；状态、输入请求仍可执行。

元数据包含 `capture_id`、`request_id`、`instance_id`、`source`、`valid_capture`、
返回 `width/height`、`original_width/original_height`、`game_width/game_height`、
`timestamp_unix_ms`、`simulation_frame`、`scene_id`、`gpu_present_id`、`backend`、
`output_mode`、`cursor`、`crop` 和 `pixel_format: RGBA8`。
`simulation_frame` 是采集时的已完成逻辑计数，不证明 Surface 恰好在该逻辑帧重绘；
时间戳分别记录 DDraw 拷贝开始或 GPU draw 命令记录时刻，含相应 `*_semantics` 字段。
`viewport` 是原图上的 `[x,y,width,height]`，`letterbox` 是原图的 `[left,top,right,bottom]` 边带。
`include_metadata=false` 仅保留来源、返回尺寸、有效性和请求／实例 ID。
服务端不写图片文件，`file_path=null`；客户端可将收到的完整 PNG 保存到本地。

### DirectDraw

`SurfaceMirror::ReadPrimary` 共享现有 Primary Surface 捕获处理，独立于 SDL、GPU 开关和
`PHOBOS_GPU_MIRROR`。根据游戏客户区计算桌面型 Primary 的偏移，检查区域、16-bit 格式和
实际 row pitch。取得 MouseMutex 后 Lock，逐行复制到 Phobos 自有缓冲区，立即 Unlock／释放锁。
Mutex 超时、放弃的 Mutex、Surface 不可用或 Lock 失败返回 `capture_unavailable` 及具体 `reason`。
随后根据 `Unsorted::ColorPackData` 的通道位数／偏移转换 RGBA8，支持 RGB565 和 RGB555，
不在 PNG 编码期间保留 Surface 指针。光标语义为 `surface_contents_if_drawn`：复制 Surface
中当时实际存在的光标，不额外叠加桌面光标；实机图像已观察到游戏光标。

### 原生地形 surface

`source=native_terrain` 是 GPURender 阶段 C 的对照来源（依据 gamemd-reverse #16 §8.3）。
每次重新读取 `DSurface::Tile`（`[8872FC]`；滚屏会交换 Tile／Composite 指针），检查 16-bit 和
pitch，Lock 后只复制 `ViewBounds` 区域。地形阶段把 Tile 的 ViewBounds 复制到 Composite 的
同一位置，因此图片尺寸与 `ddraw` 相同，该区域放在 Composite 的画布原点处，其余像素为黑。
不在场景中（或读档中）返回 `terrain_not_in_scenario`；Surface 缺失、非 16-bit、ViewBounds
越出 Surface 或游戏区、Lock 失败、pitch 异常分别返回对应 `reason`。

元数据另含 `native_terrain`：`surface`（当时 Tile 指针，仅用于观察交换）、`surface_width/height`、
`view_bounds`（Tile 坐标）、`placed_at`（区域在图片中的左上角）和 `tactical_position`
（TacticalPos）。其他来源该字段为 `null`。`cursor=not_included`。

这是原生地形**缓存**，不是纯 TMP：除地块外还含矿石／围墙等 overlay、弹坑、部分静态对象与
建筑底座，迷雾／未探索区也已烘入；单位、选择框、文字等 mode 2 动态层不在其中。
纯地块对照需用受控地图。实机（D3D12，1024×768 与 800×600）静止、向下和斜向滚屏后，与同帧
`ddraw` 的战术区仅在单位、光标和顶部文字处不同；`terrain_not_in_scenario` 未能实机触发
（Spawner 直接进入场景，MCP 可用时已在局内）。

### 原生 Composite（对象层之后）

`source=native_composite` 是 GPURender 阶段 D 的对照来源（依据 gamemd-reverse #23 §9.2）。
Tactical mode 2 在 `6D465F` 调 RenderLayers（`6D8DB0`）画五个对象层，`6D4664` 返回后才画
Spotlight、激光、计时器等；Phobos 在 `6D4664` 从当次锁定的目标（`EBX`，须同时是 Temp 与
Composite、16-bit）按 pitch 复制 `ViewBounds` 区域到自有缓冲，不另行 Lock。截图读取最近
一次渲染保存的副本，放置方式与 `native_terrain` 相同：图片尺寸同 `ddraw`，该区域放在
Composite 的画布原点处，其余像素为黑。副本只在 GPU 窗口运行（independent）或 MCP 服务
运行时保存。

元数据另含 `native_composite`：字段同 `native_terrain`（`surface` 为当时 Composite 指针），
另加 `game_frame`（复制时的引擎帧）；`timestamp_semantics=render_copy_read`（时间戳是读取
副本的时刻，不是渲染时刻），`cursor=not_included`。不在场景中返回
`composite_not_in_scenario`；尚未复制或复制失败时 `reason` 为失败原因（如
`composite_not_copied`、`composite_target_not_composite`、`composite_not_locked`）。

这一点只在受控场景下是“Tile + 对象层”的纯世界图像：RenderLayers 之前已有鼠标辅助、虚线、
选择／放置辅助和 IonBlast/Veinhole 绘制，对象层本身也画单位的 Extras（血条、选择框）。
对照动画 Terrain 时视野内不能有单位、建筑、选择和这些效果。

### SDL GPU

`GPUReadback` 按需创建 COLOR_TARGET 离屏纹理，格式与当前 swapchain 一致，复用原有
`PresentPass::Draw/DrawSprites`，使用同一批 sidebar、字体、cameo、cursor 数据及 letterbox。
不读 swapchain 纹理，也不修改 independent／mirror 的含义。independent 的黑色地图区域是
当前渲染器的预期输出；mirror 才包含原始 DDraw 画面。

正常命令仍经原来的 `Atlas.Submit` 提交，保持 Atlas 的 fence、上传和资源生命周期。
随后单独提交 `SDL_DownloadFromGPUTexture`，取得自己的 fence；主线程只用
`SDL_QueryGPUFence` 非阻塞查询，完成后 Map DOWNLOAD TransferBuffer、复制并归一化
RGBA／BGRA 字节布局。行距按 256 字节对齐，offset 为 0。取消和资源释放走 SDL 延迟销毁。
常规截图不调用 `SDL_WaitForGPUIdle`；原有设备退出路径仍保留其全局等待。
对应约定：[GPU 下载](https://wiki.libsdl.org/SDL3/SDL_DownloadFromGPUTexture)、
[提交顺序及 fence](https://wiki.libsdl.org/SDL3/SDL_SubmitGPUCommandBufferAndAcquireFence)、
[传输对齐](https://wiki.libsdl.org/SDL3/SDL_GPUTextureTransferInfo)。

GPU 窗口隐藏／Disabled 返回明确不可用，不偷偷显示窗口；最小化时若取不到新的 swapchain
帧，请求受超时约束。窗口／逻辑分辨率改变、场景变化或设备销毁会使旧读回失效。
`gpu_present_id` 统计具有 swapchain 纹理的成功 GPU 提交，不表示显示器已经扫描输出，
更不等同于游戏逻辑帧。截图返回时，该截图的读回 fence 已完成。
元素透明度在原有绘制管线中完成合成；最终黑色背景是不透明的，返回 PNG 的 alpha 为 255。

两种来源的 PNG 都在 HTTP 工具工作线程通过系统 WIC 编码，无额外第三方编解码运行时。
编码只读取不可变的自有像素，不访问引擎、SDL 或 Surface。WIC 内部编码不支持中途取消，
完成后会再次检查请求截止时间和客户端断开状态，过期图片不会作为成功结果返回。

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
* Tooltip：游戏消息泵在 Dispatch 前调用 `ToolTipManager::ProcessMessage@724200`，但注入的
  移动不进队列、按钮直接调用 WndProc，原本都到不了它。移动成功后同步调用一次
  `ProcessMessage(WM_MOUSEMOVE)`，转发按钮消息前也调用一次（滚轮不调用，原版该函数对滚轮无分支）。
  `724200` 用 `GetCursorPos`+`ScreenToClient` 命中，虚拟光标有效时由两个 hook 换成最后一次
  虚拟移动的逻辑客户区点；延迟到期的 WM_TIMER 分支同样使用虚拟点。计时、命中、文本生成仍走原版。
  主菜单中管理器 `IsActive=0`，通知不产生效果。

结果包含 `backend: engine_queue`、`input_route: wwkeyboard/wwmouse/engine_window_queue/native_window_queue/native_dialog_navigation`、
`injected`、`queue_drained`、`consumed: null` 和最终逻辑坐标。**队列排空不等于预期游戏效果**，
场景切换也可能清空队列。`consumed` 不会仅因 API 成功或队列清空而变成 true。
tap/click 在按下事件离开队列之后才继续释放，所有等待受 Dispatcher 超时约束。

`game_status.input` 提供 `os_foreground`、引擎是否可消费输入、队列长度、实际通过
`WWKeyboard::Down` 查询的保持键，以及活动对话框 ID。`input.tooltip` 给出管理器状态：
`active`、`current_id`（当前注册区域 ID，如 cameo `1000+槽位`、页签 `203..206`、地图 `500`）、
`delay_ms` 和最后一次命中点 `hit_point`。这只是管理器状态，`current_id` 非空不代表 tooltip 已画出。`game_focused` 是引擎自己的状态，
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
| `ToolTipManager_ProcessMessage_MoveCursor_Automation` | `72429E..7242A8` | 写入虚拟点、EBX=`&CurrentMousePosition`，返回 `7242B3` | 大小、冲突通过；IDA 不可用，capstone 补核边界/栈/寄存器 |
| `ToolTipManager_ProcessMessage_TimerCursor_Automation` | `724359..724363` | 同上，返回 `72436E` | 同上 |

普通模式全部 return 0。第三个 Hook 覆盖 `call 53E720`，由 SyringeEx 在 trampoline 中
重定位。检查使用技能目录中的 `discover_hooks.py`、`check_hook_conflicts.py`，并对
IDA 中的 gamemd.exe 逐项核对；脚本自动提取遗漏的条件返回地址已另行检查，无冲突。

新增数据引用 `B72F44` 是 UI 活动对话框栈顶：`UI::BeginDialog@622650` 在 `6226F9`
写入，`UI::EndDialog@622720` 在 `6227B7/6227DE` 恢复或清空。
`OwnerDraw::CurrentDialogHwnd` 是布局缓存，不能替代该栈顶。
菜单资源 226/256 及控件 1006/1670 分别由 `531F60`、`52D640` 的 WM_COMMAND 分支核实。
未修改 YRpp。

两个 tooltip Hook 各覆盖 `lea ebx,[esi+10h]; push ebx; call [GetCursorPos]` 共 10 字节，
跳过其后的 ScreenToClient；整段跳过使栈净变化为 0，返回点之后立即以 EBX 读取坐标，ESI 自
`724203` 起保持为 this。被覆盖字节只有绝对间接 CALL，无相对重定位。依据 gamemd-reverse #11
`tooltip-gpu-handoff.md` §2；本机 retail `gamemd.exe`（SHA256 `7b8a0685…b94b6`）字节已核对。

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
两套 SDK 已验证 PNG `ImageContent`，包含 2026-07-28 和 2025-11-25 路径。

截图测试：

```powershell
cmd /c scripts\test_automation.cmd
& .\Debug\AutomationTests\sdk2\Scripts\python.exe tests\Automation\test_capture_pixels.py
cmd /c scripts\test_gpu_capture.cmd direct3d12
cmd /c scripts\test_gpu_capture.cmd vulkan
cmd /c scripts\test_render_capture_regression.cmd
$env:PHOBOS_TEST_SCENARIO = '1'
$env:PHOBOS_TEST_CAPTURE = '1'
$env:PHOBOS_TEST_GPU = '1' # 0 验证关闭 GPU 时 DDraw
$env:PHOBOS_TEST_DRIVER = 'vulkan' # 或 direct3d12
$env:PHOBOS_TEST_CAPTURE_COUNT = '5' # 有界 1..50 次
& .\Debug\AutomationTests\sdk2\Scripts\python.exe tests\Automation\game_input_smoke.py
# mirror 测试另设 PHOBOS_GPU_MIRROR=1；完成后清除该环境变量。
```

GPU 测试脚本从 `Debug/AutomationGame` 或 `Debug` 查找已有 SDL3.dll；未提供 DLL 时需先放置
与仓库 headers 相符的 x86 SDL3。实机脚本只临时修改隔离副本的 `GPURender.Driver`，退出后恢复。
`CaptureTests` 验证 RGB565／555、非法位域、裁剪及 WIC 编码，Python/Pillow 独立验证确切像素。
`GPUReadbackLifetimeTests` 验证 fence 未完成禁止 Map、竞争、取消及分配／Map／提交失败释放。
真实 D3D12／Vulkan 测试验证 RGBA／BGRA、非对齐宽度、16 次完整资源循环和提交后取消。
Render 回归脚本覆盖 11 个既有测试（含 Atlas lifetime 和 D3D12 GPU）；不含需要外部字体资产的
`GlyphCacheAssetTests`。既有 ShapeImage/YRpp 测试编译仍有类型转换／未使用参数等警告。

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
Tooltip 触发已在隔离遭遇战实机验证（2026-10-29，D3D12，ExtendedToolTips 关闭）：悬停 cameo
立即出现「名称+价格」，右键后消失；页签 `204` 显示「Tip:Tab2+禁用」文本；地图建造厂经 1 秒
延迟的 Timer 分支显示名称，三者均为 DDraw 原生截图。GPU 侧绘制及 D3D12/Vulkan 同帧对照见
[TooltipGPU.md](../src/Render/TooltipGPU.md)。
电力条的建造/放置/出售流程与同帧对照见 [PowerGPU.md](../src/Render/PowerGPU.md)。暂停只冻结逻辑帧：
电力条追随与闪烁按墙钟推进，暂停期间仍可能重画，因此同一次暂停的两张截图可能相差一步，
对照这类状态时按 ddraw → gpu → ddraw 截三张，确认 gpu 介于两者之间。
资金数字按每次 UI Update 一步滚动，暂停期间同样可能变化，滚动中的对照也用三张截图，
见 [CreditsGPU.md](../src/Render/CreditsGPU.md)。
雷达开关动画约 64ms 一帧，比一次截图还快，三张截图只能证明顺序；像素对照改为交替连拍，
把 ddraw 帧汇成池后逐张查找 gpu 的完全相同帧，见 [RadarGPU.md](../src/Render/RadarGPU.md)。
按钮按下状态需要按住鼠标：发起起止同点的长 `game_mouse_drag`，在拖动期间暂停并截图，
见 [CommandBarGPU.md](../src/Render/CommandBarGPU.md)。
拖框尚未通过。sidebar 实际滚动已在条目超过一页的隔离场景（建筑页 28 项）验证：滚轮和底部箭头
使 TopRowIndex 逐行变化并在底部钳制，D3D12／Vulkan 的 ddraw 与 gpu 同帧截图一致；
平滑滑动（IsScrolling）在该配置下未出现，未验证。详见 `src/Render/CameosGPU.md` 的滚动小节。

DDraw 已验证 GPU 关闭／开启、菜单 800×600 到游戏 1024×768 的分辨率变化、原生 PNG 像素和裁剪。
D3D12／Vulkan independent 已验证黑色背景、sidebar／cameo／光标、连续新 Present 截图、
窗口缩放／letterbox、关闭 GPU 展示窗口后明确拒绝 GPU 截图且 DDraw 继续可用。
原生 PNG 文件名为 `native-*.png`，与诊断桌面图分开保存。
证据目录为 `Debug/AutomationTests/game-input-scenario[-gpu-direct3d12|-gpu-vulkan]`，
mirror 模式追加 `-mirror`。mirror 的不同时间采样只做 best-effort 像素对比：允许 UNORM
转换的 1 级色差和极少量渲染动画变化，不把相同逻辑帧号当成严格图像同步。
截图后的菜单正常退出已验证，证据在 `Debug/AutomationTests/game-input-native-gpu-direct3d12`。
当前 D3D12 环境在请求最小化后仍可产生新截图，未据此宣称实机超时测试通过；
截止时间／取消清理由 Dispatcher 和 GPUReadback 的确定性测试覆盖。

尚未实现 pair 与复合工具、自动保存服务端完整图像。原生全屏 DirectDraw／其他 wrapper、
真实设备丢失／重建中截图、驱动异常时的最长阻塞时间和系统性 FPS／内存泄漏长测未完成。
设备销毁／失败的资源释放当前有独立测试，不能代替全部真实驱动故障验证。
请勿把此阶段截图验证当作整个 MCP 项目最终验收。

关闭方式：移除 `-PhobosAutomation`，按原来的方式启动 Phobos。
