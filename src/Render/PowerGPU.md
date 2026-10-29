# 电力条：GPU 重画

2026-10-29。依据 gamemd-reverse Issue #12 / `5a75392` 的 `docs/zh-CN/sidebar-power-gpu-handoff.md`（静态调查与 15 个函数体恢复）。布局与快照复用 [SidebarGPU.md](SidebarGPU.md)，素材缓存复用 [FrameCache.md](FrameCache.md)。

## 采集

不新增 hook。在既有 `6A70AE` sidebar 快照（Strip、Power、Radar、tooltip 都已执行）里读取原生最终显示状态，按 `PowerClass::Draw @63FB20` 的循环重建 POWERP 帧序列。原生 dirty（`+150C`）只控制 CPU 重画，不参与 GPU 显隐。

| 输入 | 来源 | 说明 |
| --- | --- | --- |
| 显示格数 G/Y/D | `PowerClass +152C/+1530/+1534`（YRpp `unknown_152C` 等，`offsetof` 断言） | 帧 1（余量超过 100 点）、帧 2（最多 100 点余量）、帧 3（耗电）；由原生 `63FEA0` 按墙钟逐格追随目标 |
| 闪烁计数 B | `PowerClass +151C` | B>0 且为偶数时画帧 4，替换第一个非空段的一格；段为 0 时这一格顺延 |
| 布局高度 L | `[B0B504]` | 总格数 `N=(L+3)/3`（截断，L=500 时 167） |
| x | Phobos `isNODSidebar` | GDI 为 5、NOD 为 0；与 `63FB5D` 改写后的原生取值一致 |
| y0 | `SidebarBounds.Y + 69` | 每格下移 3 像素，从上往下 |
| 素材 | `[AC4E74]` POWERP.SHP 帧 0..4 | flags `0x400`（不居中，加帧头 Left/Top），亮度 1000，无 remap；Convert 为 `SIDEBAR_PAL @87F6CC` |

运行副本 `Debug/AutomationGame/gamemd.exe` 字节核对：`63FB2C..63FB43` 的 dirty/IsSidebarActive（`884B8D`）门槛，`63FB5D..63FB70` 的 x 选择，`63FB72/63FB80` 的 `[886F94]+45h`，`63FBAC..63FBD8` 的 N 与空格数，`63FBED..63FC03` 的参数。`63F7C0`（加载）只由 `6A5849` 调用、`63F7E0`（释放）只由 `6A5BF2` 调用，分别位于已有失效 hook `6A5840`、`6A5BF0` 之后，因此素材生命周期由 sidebar 快照失效覆盖。

`ShapeImage::PowerFrames` 生成序列：先 `max(N−G−Y−D,0)` 个帧 0，再按闪烁、G、Y、D 输出。L 超出 0..4096，或任一段为负、超过 N 时整层不可用，不截断。三段之和在追随中可能暂时超过 N，此时与原生一样不画空格。所需帧任一未就绪则整层不画。

层序：cameo 之后、TOP 之前，即原生 `Strip → Power → Radar(TOP、雷达按钮)`。

## 未支持与边界

- GPU 不推进追随或闪烁计时，只显示最近一次快照的原生状态；WM_PAINT 不重跑快照。
- 暂停时原生 UI Update 仍可能按墙钟推进追随/闪烁，并重画 CPU sidebar。同一暂停的 ddraw/gpu 两张截图相隔约 100ms，可能落在不同的追随步（best_effort）。
- `[Side] Sidebar.GDIPositions` 与实际阵营不一致的配置未实机验证（隔离副本没有散装规则文件）；GDI（Side=0）与 NOD（Side=8 俄国）均按默认映射验证。
- Phobos PowerDelta 计数器在资金区，不在本层。

## 验收（2026-10-29）

隔离 `Debug/AutomationGame` Spawner 遭遇战，1024×768，independent 输出；DLL SHA256 `0338b712…4787`（HEAD `883e6dd0` + 本批未提交改动）。Automation 驱动建造/放置/出售，暂停后在同一 `simulation_frame` 截取 ddraw 与 gpu；追随期间改为 ddraw → gpu → ddraw 三张。比较区 `(856,200)-(874,760)`（不含 x≥874 的 cameo 格）。证据在 `Debug/AutomationTests/power-gpu/`（`d3d12/`、`vulkan/`、`d3d12-nod/`、`scripts/`、日志）。

| 状态 | 原生序列（空/G/Y/D） | D3D12 | Vulkan | NOD（D3D12） |
| --- | --- | --- | --- | --- |
| 无建筑，K=1 | 166 空 + D1；B 10→0 时帧 4/帧 3 交替 | 最大差 1 | 1 | 1 |
| 建造厂展开 | 不变（建造厂无电力） | 1 | 1 | — |
| 一座电厂（O=200） | G28 Y28 | 1 | 1 | 1（苏军素材） |
| 加精炼厂（D=50） | G16 Y32 D17 | 1 | 1 | — |
| 卖电厂（O=0 低电） | D19 | 1 | 1 | — |
| 追随中间态 | 例：G14→G13→G12 | 两张 ddraw 自身不同（最大 255），gpu 序列严格介于两者之间 | 同左 | 同左 |

日志 `[SidebarPowerGPU]` 与交接一致：开局 `excess=0 margin=0 drain=1 height=500 cells=167 x=5 y0=227`，闪烁计数 10→1 交替帧 4/3；放电厂后追随期间 B 保持 10、G 逐格增长；NOD 局 `x=0`。cameo 格区在就緒/时钟状态最大 8，为既有状态层 alpha 舍入；NOD 局 `(856,160)-(1024,768)` 整块最大差 1。

回归：`test_render_capture_regression.cmd` 12 项通过（ShapeImageTests 增加电力条序列用例）；Debug 构建无警告。未重跑 GPU 截图读回测试与场景 smoke（本批不涉及读回、Automation 或菜单路径）。
