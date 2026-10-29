# 底部命令栏：GPU 重画

2026-10-30。依据 gamemd-reverse Issue #15 / `3e2cb23` 的 `docs/zh-CN/command-bar-gpu-handoff.md`（静态调查与 19 个完整函数体恢复）。SHP 解码、帧缓存与按钮选帧复用 [SidebarGPU.md](SidebarGPU.md)；按钮 tooltip 走已有的 [TooltipGPU.md](TooltipGPU.md)。

## 采集

不新增 hook。在已有的 `6A70AE` 回调里，侧栏快照之后由 `CommandBarSnapshot::CaptureAfterDraw` 单独采集，不受侧栏 `Visible`、`LowerButtonsReady` 或 sidebar active 的影响。`TabClass::Draw` 的命令栏段（`6D0A20`）在 `6D0E38` 调用 Sidebar 绘制之前已经画完，而 Sidebar 绘制（`6A6C30`）只有 `6A70CE` 一个返回点，每次都会经过 `6A70AE`。

| 数据 | 来源（已用运行的 `gamemd.exe` 字节核对） |
| --- | --- |
| 展开 / MissionTimerPinged / 本次重画 | `Tab+5544`（`ThumbActive`）/ `+5545` / `+5546` |
| 4 个装饰 SHP | `[B0FA3C]` LSPACER、`[B0FA90]` LENDCAP、`[B0FAA8]` BTTNBKGD、`[B0FABC]` RENDCAP；`730100` 卸载时清零 |
| 矩形 | `[B0FC58]` 底栏、`[B0FC60]`/`[B0FC64]` 展开/折叠端帽、`[B0FC68]` 第一背景格、`[B0FC6C]` 右端帽；背景片数 `[B0F9E4]` |
| 命令映射与对象 | `B0CB78[25]`：命令 k → 显示槽 j；对象 `B0C1C0 + j*60h`；素材 `B0C148[k]` |
| 端帽按钮 | 展开 `B0CCB0`（ID 240）、折叠 `B0CC40`（ID 241），都绑定 LENDCAP |
| 颜色 | 装饰用 `[87F6CC]` SIDEBAR Convert；按钮用对象自己的 `Drawer` |
| 目标与原点 | `DSurface::Composite` 的宽高；画布原点用 `Tooltip::CanvasOrigin(Composite, …)` |

ShapeButton 字段以机器码为准：`69DEB0` 读 `+1E` Disabled、`+2C` IsPressed、`+2D` IsOn、`+34` IsToFlash、`+40` UseFlash、`+44/+48` DrawPosition、`+4C` UseSidebarSurface、`+50` Drawer、`+55` IsAlpha、`+58` ShapeData，与 YRpp 的 `static_assert` 一致。交接文档第 4 节列的 `+44/+48/+55/+56` 是错的。

## 层序与位置

与 `6D0A20` 的顺序一致，flags=0、亮度 1000：

1. LSPACER frame 0，放在 `R58.(X,Y)`；裁剪为 `full(C)` 只把宽度改成 `R58.Width`。
2. LENDCAP frame 0：展开放 `R60.(X,Y)`，折叠放 `R64.(X,Y)`。
3. 展开时：BTTNBKGD 画 N 片，`(R68.X + i*R68.Width, R58.Y)`；然后按命令编号 k 遍历映射，槽 j 合法且 ShapeData 非空才画对象 j，位置为 `X/Y + DrawPosition`，帧由 `ButtonImage::SelectFrame` 选。重复映射同一槽时会画两次，与原生一致。
4. RENDCAP frame 0：X 取 `R6C.X`，Y 沿用前一层（展开为 `R58.Y`，折叠为 `R64.Y`）。
5. 展开或折叠端帽按钮，按对象实际字段选帧，覆盖第 2 步的 frame 0。

GPU 合成顺序：命令栏 → 侧栏 → tooltip → 光标。命令栏与侧栏不重叠。

## 刷新与失效

每次采集都按当前持久状态重建，不用 `+5546` 控制显隐。原生按钮状态一变就会标记重画，所以当前状态就是原生最近一次画出的内容；而 `+5546==0` 时原生只是让 Primary 保留旧栏，GPU 每次 present 都清黑重画，必须照常出图。`+5546` 与已画 surface 表 `[B0CC00]`/`[B0CD44]` 只做核对：本帧重画了但表头不是 Composite 时整栏不画，记 `reason=target`。

- 按钮素材：`6D0270` 直接释放 25 个 `Button%02d.SHP`（不经 SHP 析构），`6D0FD0` 之后才重新绑定。采集要求对象的 ShapeData 等于 `B0C148[0..24]` 中的某一个，否则该按钮不画（`unbound`），不读已释放的指针。端帽按钮要求 ShapeData 等于当前 `[B0FA90]`。
- 失效沿用侧栏的 `560BF0`/`6A5840`/`6A5BF0`/`6BE1C0`/`730100`/`730400` 回调与 SHP 释放通知；`6D0270`、`6D02B0` 内部分别先调 `6A5BF0`、`6A5840`。`69DE00`/`69DE70` 改动命令或端帽对象时也清掉命令栏快照。下一次采集重新建立。
- 装饰素材缺失、矩形不可读、背景片数超过 256、Composite 尺寸异常或 Armageddon 时整栏不画。

## 日志

`PHOBOS_SIDEBAR_TRACE=1` 时输出 `[CommandBarGPU]`，最多 64 条，内容相同时不重复：`ready/reason`、展开、`+5545`、`+5546` 与 surface 表（只记录不参与去重）、原点与尺寸、各矩形、背景片数、端帽按钮的原因/帧/位置、调色板；装饰素材的对象/数据/尺寸/frameID；每个命令的 `k:j:ID:原因:帧:位置:偏移与尺寸:frameID`。

## 未支持与边界

- `UseSidebarSurface` 或 `IsAlpha` 的命令按钮不画（`target`）；非 16 位 Convert 不画。
- 侧栏在左（SidebarMode=0）只按 `CanvasOrigin` 推导，YR 没有对应的配置项，未实机验证。
- 震屏、缩放、chrono、WM_PAINT 的复制范围变化，observer，读档、换玩家、分辨率切换、素材/调色板重载只靠既有失效路径，未实机验证。
- Disabled 帧与原生闪烁（帧 3/4）本次没有触发。
- 端帽按钮按下时，日志里出现过 `drawn=0` 而端帽为帧 1 的采样：Tab 本帧没重画，按下状态由之后的 Gadget DrawAll 画出。GPU 按当前状态显示，两边截图一致。
- 战术区、消息列、超武侧栏不在本层。

## 验收（2026-10-30）

隔离 `Debug/AutomationGame` Spawner 遭遇战，independent 输出；DLL SHA256 `c685161d…8df5`（HEAD `037a1eb0` + 本批未提交改动）。临时改 `spawn.ini` 的 Side 与 `RA2MD.INI` 的驱动、分辨率，测后按字节还原。比较区为底栏 `(0,H-32)-(W-168,H)`。证据在 `Debug/AutomationTests/command-bar-gpu/`。

| 后端/阵营 | 状态 | 结果 |
| --- | --- | --- |
| D3D12 GDI 1024×768 | 开局展开、tooltip（Team01）、PlanningMode 开（帧 1）与关、Deploy 按住（帧 1）、按住后拖出、折叠、再展开 | 底栏最大差 1；tooltip 区 `(0,600)-(856,768)` 最大差 1 |
| D3D12 NOD（Side=8） | 开局、折叠 | 最大差 1；素材与 GDI 全不同 |
| D3D12 Yuri（Side=9） | 开局、PlanningMode 开、折叠 | 最大差 1 |
| Vulkan GDI | 开局、PlanningMode、按住、折叠、展开 | 最大差 1 |
| D3D12 GDI 800×600 | 开局、折叠、展开 | 底栏 `(0,568)-(632,600)` 最大差 1；N=11 |

按住用 4 秒的 `game_mouse_drag`（起止同点），2.5 秒时暂停截图；按住帧与普通帧最大差 255，确认截到的是按下状态。日志所有样本 `ready=1`，六个按钮为 k=0,1,3,4,6,9 → 槽 0–5、ID 214/215/217/218/220/223。同批截图中整条 sidebar 仍为最大差 1。

回归：`test_render_capture_regression.cmd` 通过（退出码 0）；Debug 构建无警告。未新增单元测试（全部字段来自原生状态）。未重跑 GPU 截图读回测试与场景 smoke。
