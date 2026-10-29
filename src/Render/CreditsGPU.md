# 资金区：GPU 重画

2026-10-29。依据 gamemd-reverse Issue #13 / `9002145` 的 `docs/zh-CN/sidebar-credits-gpu-handoff.md`（静态调查与 4 个完整函数体恢复）。字形缓存与颜色打包复用 [SidebarTextGPU.md](SidebarTextGPU.md)，快照与层序复用 [SidebarGPU.md](SidebarGPU.md)。

## 采集

不新增 hook 地址。复用已有的 Simple 入口 `4A5EB0`、Simple 出口 `4A5FF2`、字形入口 `434120` 观察点，在 `6A70AE` 快照中发布。GPU 只复制原生最终打印的 UTF-16 与字形命令，不读取资金 getter，不重算平滑、矿车数或电力差。

| 文字 | 识别条件（Simple 入口 `E`） | 说明 |
| --- | --- | --- |
| 资金数字 | 目标 Sidebar，`[E]=4A6143/4A619A`，`[E+428]=4A25E0`，`[E+47C]=6D0BC9` | `TabData::Draw @4A2370` 的 `%ld`，即原生 LastValue |
| 矿车/电力差/Weeds | 目标 Sidebar，`[E]` 同上，且当前有计数器标记 | Fancy 的调用者在 Phobos DLL 内，地址随构建变化，不硬编码 |
| observer 时间 | `[E+428]=4A24E0`，`[E+478]=6D0BC9` | 四项全部清为不显示，不把秒数当资金 |

字形要求 `434120` 入口 ESP+`B0` 恰好等于记录的 `E`，并核对 `[B]=43464D`、`[B+34]=434BCF`、`[B+58]=4A5FF2`。字形、缺字回退（反色 X）、裁剪（字体边界 +1 转半开）与颜色（color=-1 取当前字体颜色）都由 `SidebarText::CaptureGlyph` 处理，和 cameo 文字、tooltip 共用。

计数器标记由 `src/Misc/Hooks.UI.cpp` 的 `4A25E0` 回调提供：开头构造 `SidebarText::CounterBatch`，每个 `DrawText` 外包 `SidebarText::CounterScope`。批次结束时没有打印的计数器（开关关闭、阵亡提前返回、PowerDelta 无建筑）提交为不显示。回调原有逻辑与返回值不变。

每段文字在 `4A5FF2` 结束时整体提交；任一字形失败、文本超过 512 字或不可读时，该段提交为未知并不画，不沿用旧值。已提交结果保留到下一次原生打印覆盖，与 CPU surface 保留像素一致。字体失效、CSF 卸载、`6A5840`/`6A5BF0`/`6BE1C0` 失效都经 `SidebarText::ClearRuns` 一并清除。

## 层序

CREDITS 底图之后、SIDE1 之前，内部为资金 → 矿车 → 电力差 → Weeds，即原生 `Tab::Draw → TabData::Draw → 4A25E0 回调 → Sidebar::Draw` 的顺序。坐标是 Sidebar surface 局部坐标，与其他层一样加 OriginX。当前玩家是 observer 时不画。

## 未支持与边界

- GPU 不推进平滑，只显示最近一次快照的原生打印；`CreditsIndicator.Smooth/MaxStep` 只影响原生 LastValue，未分别实机验证。
- observer 时间面板未实现（本层不显示）。读档、切换玩家、分辨率变化、字体/CSF 重载只靠既有失效路径，未实机验证。
- ArmageddonMode 下 Tab 不画资金区，快照整体不可见（`Eligible`）。
- 计数器标签的缺字、长标签裁剪、Side Offset/Align 自定义只按原生字形命令复制，未逐项验证。

## 验收（2026-10-29）

隔离 `Debug/AutomationGame` Spawner 遭遇战，1024×768，independent 输出；DLL SHA256 `28bba1c9…d3e7`（HEAD `4996d87a` + 本批未提交改动）。临时 `uimd.ini` 打开计数器，`spawn.ini`/`RA2MD.INI` 临时改动后已按字节还原，`uimd.ini` 已删除。比较区 `(856,0)-(1024,16)`。证据在 `Debug/AutomationTests/credits-gpu/`。

| 后端/阵营 | 状态 | 结果 |
| --- | --- | --- |
| D3D12 GDI（矿车+电力差） | 开局 10000、建造厂 `⚡+0`、电厂 9200 `+200`、精炼厂 7200 `+150 ⛏1/1`、卖电厂后 8100 `-50` | 稳定状态最大差 1 |
| D3D12 GDI | 建电厂扣款滚动（三组）、卖电厂资金上涨（两组） | ddraw → gpu → ddraw 三张：9890→9880→9880、7685→7697→7700 等，gpu 均介于两者之间，与其中一张最大差 1 |
| Vulkan GDI（三个计数器全开） | 开局、扣款滚动两组、电厂 | 稳定最大差 1；滚动 9806→9792→9790；Weeds 与矿车在同一锚点重叠，两边一致 |
| D3D12 NOD（Side=8，资金 0） | 开局、建造厂 | 最大差 1；黄色 `0`，位置与 GDI 相同 |

日志 `[SidebarCreditsGPU]` 无 unknown：开局资金从 143 起逐步滚到 10000；`⛏`(26CF)、`⚡`(26A1) 在 GAME.FNT 缺字，原生与 GPU 都画反色 X 回退；电力差锚点 14（W/2−70），矿车锚点 134 居中。整块 sidebar 在 y=48–158 的差异是未迁移的雷达区。

回归：`test_render_capture_regression.cmd` 12 项通过；Debug 构建无警告。未新增单元测试（分类依赖原生栈）。未重跑 GPU 截图读回测试与场景 smoke。
