# Tooltip：GPU 采集与绘制

2026-10-29。依据 gamemd-reverse Issue #11 / `3464e14` 的 `docs/zh-CN/tooltip-gpu-handoff.md`。触发侧见 [Automation MCP](../../docs/Automation-MCP.md) 的 Tooltip 一节；字形缓存与 Blit 观察复用 [SidebarTextGPU.md](SidebarTextGPU.md)。

本批范围：ExtendedToolTips 关闭（默认）下的 cameo、sidebar 按钮与地图 tooltip，画到 Sidebar 或 Composite 两种目标。只读观察原生最终结果，不重算文本、布局、价格或本地化。

## 采集

`CCToolTip::Draw2 @478E30` 每次调用：度量 → 选取 Sidebar/Composite → 底色 → 边框 → wrapped DrawText。设 T 为 Draw2 入口 ESP：

| 位置 | 内容 |
| --- | --- |
| `478E30/7` 新 hook | `mov edx,[esp+4]` / `sub esp,2Ch`；记录 T，绑定当前 BitFont |
| `478FDC` Phobos 既有 hook 内 | 锚定与底色选择之后调用 `Tooltip::ObserveFill`。T=ESP+44h；底色矩形 T-10h（`{left,top,bw,bh}`），最终文本指针 T-24h（已经过 `478EE1` 替换），ESI 为目标 surface，EDI 为 `B0FA1C` 按 ColorPacker 打包的边框/文字色 |
| `434120` 既有观察器 | 先判定 tooltip 分支：Blit 入口 B 满足 `[B]∈{434EA6,4350E1,4352BA}`、`[B+54h]=479041`、`B+BCh=T`、`[B+5Ch]`=目标 surface；符合则不再进入 sidebar 项目分支 |
| `479042/6` 新 hook | 所有返回路径共用的 `pop edi/esi/ebx; add esp,2Ch`；T=ESP+38h。未到底色的提前返回不产生绘制 |

底色：存在 SideExt 时 Phobos 改为 `FillRectTrans(color, opacity)`，否则原生 `FillRect(rect,0)` 即不透明黑色。`4BB830` 以无符号比较把 opacity 夹到 0..100，`a=255p/100`、混合 `(a·src+(255−a)·dst)>>8`；GPU 用 `p=100 → 256/256`，否则 `a/256`。p=100 的非黑色原版为 `src·255>>8`，与 GPU 可能差一个 565 量化级；默认黑色无差异。

边框：`DrawRect` 覆盖 `x..x+w−1`、`y..y+h−1` 的一像素轮廓，拆为四段不重叠的实色矩形（`Tooltip::Border`）。实机对照扩大 2 像素的外圈确认无越界。

字形：复用 `SidebarText::CaptureGlyph`（与 sidebar 项目分支共用的字形解析、缺字 inverse-X、inclusive font Bounds → half-open clip、原生颜色）。失败的字形使整次 Draw2 记为 unknown 并丢弃，不沿用旧 run。

## 发布与层序

- 每次 Draw2 结束得到一个自有 `Tooltip::Draw`：目标、目标 surface 宽高、画布原点、底色 + 四段边框 + 字形命令、最终 UTF-16 副本。
- Sidebar 目标：`Sidebar::Draw` 在 `6A708D` 无条件调用 tooltip Draw(true)，随后到 `6A70AE` 快照点，中间无提前返回。因此在 `6A70AE` 用本次观察到的 Sidebar 目标绘制整体替换上一份，与 sidebar 快照同生命周期；隐藏后原生 `478DB0` 标记重画，下一次 sidebar 快照即为空。
- Composite 目标：`4F4583`（顶层 `4F4570` Draw(false) 返回之后）整体替换。同一帧 true/false 两个上下文都可能画到 Composite，按原生顺序全部保留，不按文本去重。
- 画布原点按 Draw2 选区反变换：SidebarMode=1 时 Sidebar 加 `ViewBounds.X+Width`、Composite 不变；其他模式 Composite 加 `SidebarBounds.X+Width`、Sidebar 不变。Y 不变。裁剪到目标 surface 尺寸，因此 SlaveDraw/扩展分支把 cameo tooltip 画到 Composite 右侧之外时（实测 x=969 于 856 宽）完全裁掉，与原生一致。
- `PrepareSidebar` 在 sidebar placement 之后、GPU 光标之前追加 tooltip；任一字形已失效则整个 tooltip 不画。仅在 sidebar 快照可见时绘制（实色命令借用 asset 0 的 atlas 纹理绑定）。
- 失效：`SidebarSnapshot::Invalidate`（分辨率、场景、侧栏资源/palette）与字体失效时清空。字体 metrics 变化只清 sidebar 项目 run，不清 tooltip（tooltip 每帧实时采集）。

## 未支持与边界

- `ToolTipBlur`：GPU 不做模糊，首次遇到时写一次日志，仍绘制底色。
- ExtendedToolTips 开启、AnchoredToolTips、MaxWidth、Phobos SW 侧栏：观察点位于这些 hook 之后，理论上读取的是最终矩形和文本，但本批未实机验证。
- WM_PAINT 的 `777717/777723` 只显示已有内容，不重跑 Draw；GPU 保留最近一次发布的自有数据。
- 计时由 USER32 Timer 驱动，与逻辑帧无关；暂停期间 Timer 仍可能到期，同一次暂停的 ddraw/gpu 两张截图相隔约 100ms，可能分别落在 tooltip 出现前后（best_effort）。

## 验收（2026-10-29）

隔离 `Debug/AutomationGame` Spawner 遭遇战，1024×768，independent 输出，ExtendedToolTips 关闭；DLL SHA256 `cd1cc53b…20840`（HEAD `b5734a44` + 本批未提交改动）。Automation 注入移动/右键，暂停后同一 `simulation_frame` 先后截取 `source=ddraw` 与 `source=gpu`。证据在 `Debug/AutomationTests/tooltip-gpu/`（`d3d12/`、`d3d12b/`、`vulkan/`、`scripts/`、日志）。

| 场景 | 目标 | D3D12 | Vulkan |
| --- | --- | --- | --- |
| cameo「發電廠 $800」（id 1000） | Sidebar，局部 x=77 → 画布 933 | 矩形外扩 2 像素内最大差 1 | 同左 |
| 页签 204「武器頁面 (功能取消)」 | Sidebar | 最大差 1 | 最大差 1 |
| 地图「盟軍建造場」（id 500，1s Timer） | Composite | 矩形内最大差 1 | 最大差 1 |
| 地图未探索格「未探索之地形」 | Composite | 矩形内最大差 1；外圈 GPU 全黑 | 未单列 |
| 右键隐藏 | — | 两侧均无 tooltip | 两侧均无 tooltip |

差 1 来自 RGB565 → 8 位换算舍入（边框 165 vs 164）。sidebar 整体仍有雷达/资金/电力条等未迁移区域的固定差异；cameo 格区 `(874,224)-(1006,730)` 在有无 tooltip 时最大差均为 1，sprite 循环重构未引入回归。

回归：`test_render_capture_regression.cmd`（新增 TooltipTests，共 12 项）通过；`test_gpu_capture.cmd` D3D12/Vulkan 通过；D3D12 场景 smoke 6 次中前 2 次在「点击主菜单单人游戏后 3 秒内对话框未切换」处失败、后 4 次通过，第一阶段 DLL 同条件 4/4 通过。静态核对本批代码在主菜单不执行（Draw2/DrawOnTop 只在游戏内，`434120` 仅多一次空栈判断），原因未定，需继续观察。
