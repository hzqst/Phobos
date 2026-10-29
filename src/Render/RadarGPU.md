# 雷达区主体：GPU 重画

2026-10-30。依据 gamemd-reverse Issue #14 / `6898733` 的 `docs/zh-CN/sidebar-radar-gpu-handoff.md`（静态调查与 11 个完整函数体恢复）。SHP 解码、帧缓存与快照层序复用 [SidebarGPU.md](SidebarGPU.md)。小地图内容不在本层。

## 采集

不新增 hook。在已有的 `6A70AE` 快照（`SidebarSnapshot::CaptureRadar`）读取 Radar::Draw `653100` 执行后的最终状态，GPU 不推进动画，也不重算供电、建筑或干扰资格。

| 数据 | 来源 |
| --- | --- |
| 动画状态 / 显示帧 | `+14AC` RadarState / `+14FC` RadarAnimFrame |
| 模式 | `+14B0`。运行的 `gamemd.exe`（SHA256 `7b8a06…94b6`）在 `653385`、`656EE9` 直接读该字段；交接文档中的 `BAD3E8` 只属于 spawn 版 IDB |
| 主体 SHP | `[B04A38]`，要求等于 UI 素材槽 `[B0FA68]` 且雷达按钮已就绪（`6530E7` 在 Init_For_House 借用之后） |
| 颜色 | `[B0FBF8]`（`72F510` 返回值），雷达自己的 Convert，不是 TOP 的 `SIDEBAR_PAL` |
| 位置 | `(+11E4, +11EC)`，sidebar surface 坐标，与其他层一样加 OriginX |

选帧规则与原生分支一致：

| 状态 | 模式 | 帧 |
| --- | --- | --- |
| 0 Inactive、2 Deactivating、3 Activating | 0 或 1 | 原生 `+14FC` |
| 1 Active | 1（地图） | 32（`656EC0` 在地图下画的外框） |
| 1 Active | 0（default 分支 `6534B9`） | 0 |
| 其他状态（4/5 电影切换）、模式 2/3/4（统计、电影）、observer | — | 不画，日志记 `reason` |

帧越界或解码失败时整层不画，不沿用旧帧。素材与颜色失效沿用既有的 `730100`/`730400`/`6A5840`/`6A5BF0`/`6BE1C0`/`560BF0` 回调和 SHP 释放通知；另外每次采集都核对 `B04A38 == B0FA68`，UI 卸载或重载后、`652E90` 重新借用前不读悬空指针。

运行模块上 hook 冲突的核对（解析三个 DLL 的 Syringe hook 表）：`653100` Draw 段与 `656BE0..656E50` 无任何 hook；Spawner 只有地图视口 `657134`，Ares 只在素材选择（`72FA1A/72F370/72FBC0`）、资格（`508E66/508EBC`）与地图 Lock/Unlock 介入，Phobos 已有 `652EAB`/`652F4F`/`655DDD`/`508E17`。采集只读最终字段与资源，不受这些 hook 的执行顺序影响。

## 层序

TOP → 两个雷达按钮 → **雷达主体** → 维修/卖出重画，即原生 Radar::Draw 的顺序。地图像素（`+149C` 矩形 M）以及边框线、视口框属于后续地图层，当前在 GPU 中是帧 32 的透明区域，显示为黑色。

## 日志

`PHOBOS_SIDEBAR_TRACE=1` 时输出 `[SidebarRadarGPU]`，最多 64 条，内容相同时不重复：`ready/reason`、state、mode、availability、两个 dirty 标志（只记录不参与去重）、原生帧与所选帧、SHP/Convert/调色板、位置、帧尺寸、地图矩形 M。一次开启或关闭动画约占 32 条。

## 未支持与边界

- 小地图内容、地图边框线与视口框、统计（mode 2）、电影（mode 3 与 state 4/5）、observer 面板。
- 读档、切换玩家、分辨率变化、素材/调色板重载只靠既有失效路径，未实机验证。
- 非 16 位 Convert 不画（`CaptureColor` 只支持 2BPP）。
- 断电、EMP、warp、blackout、ForceRadar 都只通过最终 state/frame 反映；本次只实机走了卖雷达与卖电厂两条路径。

## 验收（2026-10-30）

隔离 `Debug/AutomationGame` Spawner 遭遇战，1024×768，independent 输出；DLL SHA256 `3f79d9db…9202`（HEAD `d3d6766c` + 本批未提交改动）。临时改 `spawn.ini` 的 Side 与 `RA2MD.INI` 的驱动，测后按字节还原。比较区为雷达带 `(856,48)-(1024,158)`；已开启状态排除 M 及外扩 1 像素的边框（GDI x 886–992，NOD x 887–993），其余部分照常比较。证据在 `Debug/AutomationTests/radar-gpu/`。

| 后端/阵营 | 状态 | 结果 |
| --- | --- | --- |
| D3D12 GDI | 开局无雷达（鹰徽，帧 0）、已开启（帧 32）、卖电厂断电后关闭（帧 0） | 最大差 1；开局整条 sidebar 最大差 1 |
| D3D12 NOD（Side=8） | 开局（锤镰）、已开启、断电关闭 | 最大差 1；M 原点 x=32（RadarOffsetFix −2） |
| D3D12 Yuri（Side=9） | 开局（RADARY.SHP + 独立调色板） | 整条 sidebar 最大差 1 |
| Vulkan GDI | 开局、已开启、卖雷达后关闭 | 最大差 1 |

动画按墙钟约 64ms 一帧推进，暂停时仍在走，而每次截图约 100ms，同一次暂停里的 ddraw 与 gpu 几乎不会截到同一帧。因此在开启、关闭时交替连拍 24 张，把同一后端所有 ddraw 雷达带汇成池，逐张查找 gpu 的完全相同帧：D3D12 GDI 15 张、NOD 9 张、Vulkan 6 张动画中间帧在整条雷达带（包括地图区）都与原生某一帧最大差 1，其余是池中没有截到的相邻帧，没有出现错帧或残留。日志显示 state=3 时帧号从 1 逐帧升到 31 后进入 state=1/帧 32，关闭时逐帧递减，所有样本 `ready=1`。

回归：`test_render_capture_regression.cmd` 通过（退出码 0）；Debug 构建无警告。未新增单元测试（选帧依赖原生状态）。未重跑 GPU 截图读回测试与场景 smoke。
