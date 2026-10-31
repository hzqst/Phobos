# GPURender 进度记录：从黑色背景逐项构建 GPU 画面

> Automation 阶段（2026-10-29，已推送，本地与 `origin/feature/gpu-render` 一致）：`920635c7` 内嵌 MCP Server（`-PhobosAutomation` 显式开启，127.0.0.1:18742/mcp，Token 校验）、主线程 Dispatcher、引擎队列输入（key/move/click/drag/wheel）和单人逐帧控制（pause/step/resume）；`28be640b` 增加 `game_screenshot`（`source=ddraw` 读 Primary 自有像素，`source=gpu` 按需离屏 COLOR_TARGET + 独立 fence 读回，工作线程 PNG 编码）；`b5920e73` 新增 AGENTS.md 约束 GPURender／Automation 开发。实机验证：DDraw GPU 开关、800×600→1024×768 变化，D3D12／Vulkan independent 截图，mirror best-effort 对比，菜单正常退出。未完成：拖框、sidebar 实际滚动验收、pair／复合工具、原生全屏／其他 wrapper、真实设备丢失、FPS／泄漏长测。详见 docs/Automation-MCP.md。之后渲染迭代可用 ddraw 与 gpu 截图做像素对照，减少依赖人工反馈；实机测试使用仓库内 `Debug/AutomationGame` 隔离副本，不再默认部署 D:/Ra2。
>
> 下一步（2026-10-29 用户确认）：文字链路收尾计划第 2 步，图标内 UIName 接入，并借此补 sidebar 实际滚动验收。尚未修改代码。
>
> UIName 调查（2026-10-29）：静态定位已交给 gamemd-reverse [#10](https://github.com/hzqst/gamemd-reverse/issues/10)，交付 `docs/zh-CN/sidebar-cameo-name-gpu-handoff.md`，由用户在该仓库执行。Phobos 侧初步结论：①`6AC480` 在 `[MouseClass+53A4h]==0` 时返回，即该字节非零才画名称，与 YRpp 命名 `HideObjectNameInTooltip` 相反，写入者/默认值待查；②`434CD0` 内三处 `Blit_434120`（`434EA1`/`4350DC`/`4352B5`）与 SingleLine 同参数布局，倾向于在名称作用域内扩展 `SidebarText.cpp` 的 `ObserveGlyph` 返回地址白名单，直接复用原生换行坐标，不在 Phobos 重写排版；③现有 `434CD0`/`434990` 简报 hook 的 pending 恢复可能覆盖 `6AC480` 设置的 Bounds，需证实所有路径都经过 UnLock；④`6AC480` 的 Bounds 未减 1、且返回时可能不恢复字体状态，影响 clip 换算与后续打印。等待 #10 交付后再定作用域识别方案与 hook。
>
> #10 已交付（gamemd-reverse `3b42068`，`docs/zh-CN/sidebar-cameo-name-gpu-handoff.md`）：推荐不新增游戏 hook，在 `434120` 观察器内增加独立于 Prints 的名称分支（direct return `434EA6/4350E1/4352BA` + `[B+54h]=6AC5B7` + `[B+C0h]=6A9BF1` + `B+CCh`==ItemScope.Stack），名称 clip 固定为 Sidebar surface 坐标（现有 Append 会随格子平移，需改），Fingerprint 需加入开关/名称代际；简报 hook 正常路径无 pending 泄漏。**关键发现**：开关 `884B8Ch` 原版构造默认 0（不画名称），原版无切换入口。本机二进制补扫（2026-10-29）：D:/Ra2 的 Ares.dll 唯一绝对引用 `100359E4` 为 `cmp`（SW tooltip 读取），Ares/Phobos/Spawner 的 `+53A4h` 访问均为 DWORD（其他结构）或字节读取，未发现写入者。即当前配置下图标内名称原生不显示，仅存档整块读回可带入非零值。是否继续实现待用户决定。
>
> 用户决定（2026-10-29）：**UIName 暂缓**，记为“当前配置下原生不显示的路径，未迁移”，#10 结论保留备用。若将来有写入者使开关非零，GPU 缺少名称属于未迁移元素，不得用 CPU 画面补。下一步：①用 Automation MCP 在隔离副本做 sidebar 实际滚动验收（条目足够多的页签，滚轮前后 ddraw/gpu 截图对照 cameo、文字、时钟的位置与裁剪）；②盘点 sidebar 剩余元素（资金、电力条、雷达等）的调查依据，选下一块可独立验收的元素。
>
> Sidebar 滚动验收（2026-10-29，已完成，未提交）：重建 Debug（exit 0，DLL SHA256 `e3a17403…643f`）并部署到隔离副本。用 Spawner 临时地图（`Arcana troop.mpr` 派生，放开 36 种三方建筑的归属和前提为 GACNST）展开 MCV，使建筑页达到 28 项。D3D12 与 Vulkan 均在同一暂停帧对照 ddraw/gpu：无叠层时图标格子最大色差 1，暗化/时钟/READY/HOLD 最大 8～10（已接受的 alpha 舍入）；滚轮和箭头逐行滚动、到底钳制、切页签后保持位置均一致。平滑滑动 IsScrolling 在此配置下未出现，未验证。经验：地图 `[Structures]` 写 Multi1 建筑在 Spawner 下不生效（之前的文字测试同样只是靠 MCV 展开），应改用 N/D 展开 MCV 加地图规则覆盖。临时地图、spawn.ini、RA2MD.INI 已恢复。证据 `Debug/AutomationTests/sidebar-scroll-20261029/`；文档更新了 CameosGPU.md 滚动小节、CameoEffectsGPU.md、docs/Automation-MCP.md。下一步：盘点 sidebar 剩余元素（资金数字、电力条、雷达区域）。
>
> 已提交推送 `0d43c6c5`（文档 + AGENTS.md 中滚动状态一行；用户删除 Codex 署名的改动仍留在工作区，未纳入提交）。Tooltip（2026-10-29）：GPU 侧完全未实现；滚动验收中鼠标悬停 cameo 时连 DDraw 也没有出现 tooltip，Automation 的 `WWMouse::SetCoords` 能否触发 tooltip 未知。Phobos ExtendedToolTips 在 `478E10..479029` 有 9 处 hook，`SlaveDraw` 时 cameo tooltip 画到 Composite（X 加 Sidebar 宽度）。静态定位已交给 gamemd-reverse [#11](https://github.com/hzqst/gamemd-reverse/issues/11)，交付 `docs/zh-CN/tooltip-gpu-handoff.md`。
>
> Sidebar 剩余元素盘点（2026-10-29，依据滚动证据 s0 整帧 ddraw/gpu 差异分区、gamemd-reverse `sidebar-gpu-handoff.md` §3、Phobos 源码）：
> - 未迁移且当前场景可见：资金数字（`(920,0)-(960,16)`，TabData/CreditsClass，`4A25E0` 附近，Phobos 也在这里挂了 HarvesterCounter/PowerDelta/WeedsCounter）；电力条（PowerClass::Draw `63FB20`，SHP 状态帧，低负载时只露出底端）；雷达区（`(856,48)-(1024,160)`，RadarClass::Draw `653100`，TOP.SHP 与两按钮已迁移，缺无雷达时的阵营标志、开关动画和雷达内容）；底部命令栏（`(0,736)-(856,768)`，TabClass::Draw `6D0A20` 画到 Temp 的 LSPACER/LENDCAP/BTTNBKGD/RENDCAP 及命令按钮）。
> - 未迁移、当前场景不可见：tooltip（#11）；UIName（暂缓）；`self+5398` 可选叠层（素材未识别）；Phobos ProducingProgress（`6A6EB1`，默认关）、SW 侧栏（战术区左侧）、各计数器（默认关）；observer/其他侧别。
> - 雷达内容依赖地图渲染（阶段 C），应后置。
>
> Basic-memory（2026-10-29）：新建项目 `phobos`（`D:/Phobos/memory`），13 篇笔记，入口为 `gpurender/GPURender 索引`；按主题分为架构、atlas、sidebar 状态、文字、光标、Automation、实机验收流程、gamemd-reverse 交接索引和 4 篇经验。以后优先用 `search_notes(project="phobos")` 渐进查找，本文件只作时间线日志。`memory/` 与 AGENTS.md 指引已随 `5e7759b6` 提交推送。
>
> Tooltip 第一阶段（2026-10-29，#11 `3464e14` 交付后）：Automation 移动后与转发按钮前同步调用 `ToolTipManager::ProcessMessage@724200`；新增 hook `72429E`/`724359`（各 10 字节）在虚拟光标有效时以最后虚拟点替代 `GetCursorPos`+`ScreenToClient`；`game_status.input.tooltip` 报告 active/current_id/delay_ms/hit_point。D3D12 隔离遭遇战 DDraw 原生截图确认 cameo（發電廠 $800，id 1000）、页签 204、地图建造厂（id 500，经 1s Timer）tooltip 出现，右键隐藏。Automation 单测全过，原生 smoke 两次误报（推断为实体鼠标干扰）后连续两次通过，场景 smoke D3D12 通过。第二阶段 GPU 采集/绘制待做。
>
> Tooltip 第一阶段已提交推送 `b5734a44`。第二阶段（2026-10-29，未提交）：新增 `src/Render/Tooltip.{h,cpp}`、`tests/Render/TooltipTests.cpp`，hook `478E30/7`、`479042/6`，Phobos `478FDC` 内只读 `ObserveFill`，`434120` 先判 tooltip 分支；Sidebar 目标在 `6A70AE`、Composite 目标在 `4F4583` 整体替换，画在 sidebar 之后、光标之前。D3D12/Vulkan 同帧对照：cameo、页签 204、地图建造厂、未探索格的 tooltip 矩形（含外扩 2 像素）最大差 1，右键隐藏两侧一致。render 回归（12 项）、GPU capture 双后端通过；场景 smoke 6 次前 2 次在主菜单点击失败、后 4 次通过，第一阶段 DLL 4/4，原因未定。文档 `src/Render/TooltipGPU.md`。
>
> Tooltip 第二阶段已提交推送 `883e6dd0`。下一块（2026-10-29 用户确认）：电力条。已开 gamemd-reverse [#12](https://github.com/hzqst/gamemd-reverse/issues/12)，请求交付 `docs/zh-CN/sidebar-power-gpu-handoff.md`（素材/Convert、状态帧与平滑追随/闪烁、GDI/NOD 坐标、调用者与 dirty、层序、采集方案、与 Phobos `63FB5D`/`6403DF` 的冲突）。等交付后再实现；之后依次是资金数字、雷达区非地图部分、命令栏。
>
> 电力条（2026-10-29，未提交）：#12 已交付（gamemd-reverse `5a75392`）。在 `6A70AE` 快照读 Power G/Y/D/B（`+152C/1530/1534/151C`）、L（`B0B504`）、`isNODSidebar`，用 `ShapeImage::PowerFrames` 重建 POWERP 帧序列，插在 cameo 与 TOP 之间；无新 hook、无 vcxproj 变更。顺带把 SIDE2 片数的布局判断从 `PlayerSideIndex` 改为 `isNODSidebar`（用户认可）。Debug 构建无警告（DLL `0338b712…4787`），render 回归 12 项通过。D3D12、Vulkan、NOD（Side=8）同帧对照：稳定状态（无建筑/电厂/加精炼厂/卖电厂低电）电力条区最大差 1；追随期间暂停内 UI 仍按墙钟推进，用 ddraw→gpu→ddraw 三张证明 gpu 介于两者之间。RA2MD.INI、spawn.ini 已恢复；证据 `Debug/AutomationTests/power-gpu/`；文档 `src/Render/PowerGPU.md`。未验证：GDIPositions 与阵营不一致的配置。
>
> 电力条已提交推送 `4996d87a`。下一块资金数字：已开 gamemd-reverse [#13](https://github.com/hzqst/gamemd-reverse/issues/13)，请求交付 `docs/zh-CN/sidebar-credits-gpu-handoff.md`（数值来源与平滑、字体/Print 标志/颜色、坐标、时间文字、调用与 dirty、层序、`434120` 作用域区分、与 Phobos `4A25E0`/`4A26E8`/`4A2729` 的冲突）。等交付后再实现。

> 雷达区主体（2026-10-30，未提交）：#14 已交付（gamemd-reverse `6898733`）。运行的 `gamemd.exe` 与交接所用 spawn IDB 不同，模式字段是 `+14B0`（`653385`/`656EE9`），不是 `BAD3E8`；三个 DLL 的 Syringe hook 表在 Draw 段无冲突。`SidebarSnapshot::CaptureRadar` 在 `6A70AE` 快照读 state/mode/frame、`[B04A38]`（须等于 `[B0FA68]`）与 `[B0FBF8]`，插在雷达按钮之后、维修/卖出之前；mode 2/3/4、state 4/5、observer 不画。无新 hook。Debug 构建无警告（DLL `3f79d9db…9202`），render 回归通过。D3D12 GDI/NOD/Yuri 与 Vulkan GDI：开局标志、已开启（排除地图 M+1px）、关闭后最大差 1；动画用交替连拍池匹配，D3D12 GDI 15、NOD 9、Vulkan 6 张中间帧完全一致。spawn.ini/RA2MD.INI 已还原。证据 `Debug/AutomationTests/radar-gpu/`；文档 `src/Render/RadarGPU.md`。下一块：底部命令栏。

> 雷达区已提交推送 `1065de0e`。试过把隔离运行改成本地 `gamemd-spawn.exe`（`247f72…`，与 IDB 的 `2f86e7…` 也不是同一文件），结论是不可行：窗口化依赖 Spawner DLL，移掉后变全屏；spawn 版把 `spawn.ini` 的 Scenario 改写成 `mp.dat`；Ares 有 13 个 hook 与补丁点重叠，Syringe 多报 7 处字节解码失败；游戏在 SetDisplayMode 后退出。已恢复 Spawner DLL 与 spawn.ini（逐字节一致），`gamemd.exe` 冒烟测试正常。运行模块继续用 `gamemd.exe`。证据 `Debug/AutomationTests/spawn-feasibility/`，经验写入 memory。下一块命令栏：已开 gamemd-reverse [#15](https://github.com/hzqst/gamemd-reverse/issues/15)，等交付后再实现。

> 底部命令栏（2026-10-30，未提交）：#15 已交付（gamemd-reverse `3e2cb23`）。运行 `gamemd.exe` 的 `6D0A20`–`6D0E44`、`6D0270`、`6D02B0`、`69DEB0`、`730100` 字节与交接一致，只有 §4 的 ShapeButton 字段偏移写错（机器码 ShapeData `+58`、UseSidebarSurface `+4C`、IsAlpha `+55`，与 YRpp 一致）。新增 `src/Render/CommandBarSnapshot.h`，在 `6A70AE` 回调里侧栏之后单独采集，每次按当前持久状态重建（用户认可不按 `+5546` 控显隐），`+5546` 与 `[B0CC00]` 只作核对；按钮 ShapeData 须属于 `B0C148[25]`，端帽按钮须等于 `[B0FA90]`。`GPURender` 的 SpriteRequest/AppendSprite 改为接收资源数组。无新 hook。Debug 构建无警告（DLL `c685161d…8df5`），render 回归通过。D3D12 GDI（展开、tooltip、PlanningMode、按住/拖出、折叠/展开）、NOD、Yuri、Vulkan、800×600 底栏最大差 1；按住用长 drag 期间暂停截图。spawn.ini/RA2MD.INI 已还原。证据 `Debug/AutomationTests/command-bar-gpu/`；文档 `src/Render/CommandBarGPU.md`。侧栏在左无配置项、未验证。

> 底部命令栏已提交推送 `bfef2044`。sidebar 与命令栏的静态部分已全部迁移；剩余小地图内容（依赖阶段 C）、`self+5398` 叠层与 Phobos 扩展。

> 阶段 C 规划（2026-10-30，用户认可）：C0 静态调查 → C1 对照基准（Automation 增加原生地形 surface 截图来源）→ C2 坐标与平地块 → C3 高度/斜坡/悬崖 → C4 光照 → C5 地形 surface 上的其他内容（视交接）。数据来源按格采集、每次 present 完整重画；测试场景开图方法由 #16 查。已开 gamemd-reverse [#16](https://github.com/hzqst/gamemd-reverse/issues/16)，交付 `docs/zh-CN/terrain-gpu-handoff.md`，等交付后再实现。
>
> #16 已交付（gamemd-reverse `443a5e7`，`docs/zh-CN/terrain-gpu-handoff.md`）：复用 `4F4583` 回调采集；Tile 含 overlay/弹坑/静态对象与迷雾，不是纯 TMP；颜色需完整 LightConvert shade 表与逐像素 ABuffer；TMP 悬崖/extra 用 Z。运行模块哈希与关键入口字节已抽查一致。
>
> C1（2026-10-30，未提交）：`game_screenshot` 新增 `source=native_terrain`（`SurfaceMirror::CaptureTerrainPixels`）：每次重读 `[8872FC]`，只复制 ViewBounds，放在 Composite 画布原点处，其余黑；元数据 `native_terrain`（surface/尺寸/view_bounds/placed_at/tactical_position），`cursor=not_included`；场景外 `terrain_not_in_scenario`。Debug 构建无警告，Automation 单测全过（CaptureTests 加断言）。实机 D3D12（DLL `e8819344…`）：1024×768 开局、向下滚屏（Tile 指针交换）、斜向滚屏，800×600 开局，与同帧 ddraw 战术区只差单位、光标、顶部文字。RA2MD.INI 已按字节还原；证据 `Debug/AutomationTests/stage-c/`。未实机触发场景外拒绝分支。
>
> C1 已提交推送 `4ad5b6fc`。C2（2026-10-30，未提交）：新增 `src/Render/TerrainImage.h`（纯规则 + `tests/Render/TerrainImageTests.cpp`，已加入渲染回归）与 `TerrainSnapshot.h`，在既有 `4F4583` 回调按 ViewBounds 完整重画采集：`6D7560` 扫描（整数逆投影锚点，每边多扫 2）、`47FF80` 边界、`480350`/`4814F0`/`547CF0` 变体（不触发 8×8 表 RNG 初始化、不调 lazy GetImage）、主菱形按 `7EC4C8`/`7EC450` 核对后解包、extra 图、`FullColorData[shade]`（默认 ABuffer 127）。着色器新增 `Effect 5 TileDiamond`（菱形内索引 0 也画，`Effect.w` 传帧原点）；独立 FrameCache 按 piece 地址 + 内容哈希。层序地形→命令栏→侧栏→tooltip→光标；`560BF0`/`6BE1C0` 清快照。实机：受控地图（`make_maps.py` 生成 GPUTERR/GPUFLAT，空 overlay + 揭示全图）D3D12/Vulkan 1024×768、D3D12 800×600，开局与多方向滚屏除光标外最大差 1；底栏/侧栏仍最大差 1。原雪地图已揭示区内部一致，差异只在迷雾渐变带。排查经验：缺 OverlayPack 会每格画围墙（已记 memory）。DLL `49dccdc8…`；构建无警告，渲染回归与 GPU 截图测试通过；配置已按字节还原。文档 `src/Render/TerrainGPU.md`。
>
> C2 已提交推送 `ad256099`。讨论结论（2026-10-30，用户认可）：不做整张地图大纹理（尺寸超 16384、显存、光照/迷雾/矿石频繁变化）；光照改为 PS 查整张 LightConvert 表；迷雾为独立 ABuffer 数据层由 PS 采样（不是叠黑图）；地形实例化合批。C2.5（未提交）：`TerrainBatch.h`（实例、裁剪、按页分段、颜色表缓存 + `TerrainBatchTests`）、`TerrainPass.h`（storage buffer 实例、256×1024 R32 颜色表纹理、`TerrainTile.vs/ps.hlsl`，vcxproj 新增两条 PhobosShader，用户同意）、`GPUShader.h`（共享建着色器）、`DrawSprites` 底层回调；去掉 Effect 5 与 FrameOrigin。实机 D3D12/Vulkan GPUTERR、D3D12 GPUFLAT 800×600 除光标外最大差 1，原雪地图与 C2 相同。性能（Debug）：地形绘制 739–753 次/present → 1 次；采集约 1 ms 不变（有一次 15 ms 尖峰未查）。DLL `15a45d8a…`。

> C2.5 已提交推送 `f39de890`。C3 方案确认：D32_FLOAT 主（写 d/65536）、D16 后备；两段渲染通道；深度在 PS 写（VS 插值不能还原逐像素 Z 字节）。C3.0 探测（2026-10-30，未提交）：`TerrainSnapshot.h` trace 加一行 depth（ABC55C、ZBuffer Bounds/MaxValue、无 bit1 piece 数、最高层），DLL `58f72656…`，构建无警告。`make_maps.py` 参数化，新增 GPUTOMB（ancient tomb，2436 格 4 层）/GPULIBY（全平，不用）；`relief.py` 解 IsoMapPack5 找悬崖。实测 D3D12 GPUTOMB：accelerated=0（走普通 C 循环）、Bounds=0,0,856,736、MaxValue 随滚屏变化（32768→33782）、noDepth=0。6 个视角（含正面悬崖、长悬崖线、39 个 extra）C2.5 与原生除光标外完全一致；cliff4/5 的差异只在地图上边缘的变暗扇形带（ABuffer/阴影边缘，C4 范围），不是 Z。推导：b = C − 15·(x+y)，高度层在 y 与 trunc0 两项中抵消，后画的行 b 更小，所以纯地形里 Z 很少改变结果。证据 `Debug/AutomationTests/stage-c/c30/`。spawn.ini/RA2MD.INI 已还原。

> C3.1/C3.2 + 原生 Z 对照（2026-10-30，未提交，用户选方案 1）：`TerrainImage.h` 加 `BaseDepth/PixelDepth/InsideDiamond/DrawDepth`（软件 Z 模型）及单测；`TerrainSnapshot.h` 帧改为索引平面下接 Z 平面（主图 60×58、extra W×2H，无 bit1 仍单平面），哈希含 Z；Placement 加 `UsesZ/Depth`（b 用采集时 ZBuffer MaxValue/Bounds.Y、tile y、drawn 图像 PixelHeight、Level）；ZBuffer 不可读则 reason=zbuffer；trace 加 depthcheck（CPU 按顺序模拟 Z，与原生 ZBuffer 经 GetBuffer/AdjustPointer 逐像素比），trace 改为只在镜头停住时输出。GPU 不读 Z，画面不变。Debug 构建无警告（DLL `df95642d…`），render 回归通过。实测：GPUTOMB 开局+滚屏途中 63 帧（含增量滚屏）+5 个停住视角、GPUTERR 4 个视角（含完整雪地高台、四面悬崖、坡道），全部 compared=630016 mismatched=0，且 rejected=0（纯地形里 Z 测试从不挡色）。颜色对照仍只差光标（GPUTOMB 上边缘变暗带除外）。证据 `stage-c/c31/`。spawn.ini/RA2MD.INI 已还原。

> C3 深度采集已提交推送 `3b7b2237`（memory `de06e8dd`）。用户认可：GPU 深度缓冲留到阶段 D（D32_FLOAT 主、D16 后备，地形与对象共用渲染通道）。下一步候选：C4 ABuffer 纹理（含地图边缘变暗带）、C5 弹坑与 overlay。

> C4 规划（2026-10-30，用户确认）：C4.0 已开 gamemd-reverse [#17](https://github.com/hzqst/gamemd-reverse/issues/17)（ABuffer 写入规则、SHP 解码、两条写入路径、alpha shape、`4F4583` 对照可行性），正文 `Debug/AutomationTests/stage-c/issue-17.md`；交付后做 C4.1 CPU 模型 + `abuffercheck` 原生对照，C4.2 GPU R8_UNORM ABuffer 纹理（清 127、shroud 覆盖、fog reverse-subtract，地形 PS 取 a；用户已同意 vcxproj 新增两条 PhobosShader），C4.3 验收（地图边缘、无揭示黑幕、FogOfWar=yes，D3D12/Vulkan）。alpha shape 不在 C4。

> 资金区（2026-10-29，未提交）：#13 已交付（gamemd-reverse `9002145`），抽查运行副本的调用/返回地址与栈距离一致。复用 `4A5EB0/4A5FF2/434120` 观察器：资金数字按 `[E+428]=4A25E0`、`[E+47C]=6D0BC9` 识别，observer `4A24E0` 清空；三个 Phobos 计数器在 `Hooks.UI.cpp` 的 `4A25E0` 回调中加 `CounterBatch/CounterScope` 标记（用户认可）。快照插在 CREDITS 之后、SIDE1 之前；`TooltipPlacement` 移到 `SidebarSnapshot::TextPlacement` 共用。无新 hook。Debug 构建无警告（DLL `28bba1c9…d3e7`），render 回归 12 项通过。D3D12（矿车+电力差）、Vulkan（三计数器全开，Weeds 与矿车重叠）、NOD 资金 0：稳定状态资金区最大差 1；扣款/出售滚动用三张截图，gpu 均介于两张 ddraw 之间。临时 uimd.ini 已删，spawn.ini/RA2MD.INI 已还原。证据 `Debug/AutomationTests/credits-gpu/`；文档 `src/Render/CreditsGPU.md`。下一块：雷达区非地图部分。

> 资金区已提交推送 `d3d6766c`。下一块雷达区非地图部分：已开 gamemd-reverse [#14](https://github.com/hzqst/gamemd-reverse/issues/14)，请求交付 `docs/zh-CN/sidebar-radar-gpu-handoff.md`（`653100` 中 TOP/按钮之后的外框、阵营标志、开关动画的素材/调色板/坐标、状态与计时源、dirty 与层序、小地图预留矩形、与 Phobos `652EAB`/`652F4F`/`655DDD` 的关系）。等交付后再实现。

> 最新提交推送：文字补测诊断、受控原生字体生命周期专项、实际字体缓存测试及验收记录已随 `c71f3f20` 推送至 `origin/feature/gpu-render`。下方本轮“尚未提交”为历史状态。

> 文字补测（2026-10-28）：新增有界字体生命周期/缺字/超武状态诊断和实际字体缓存测试。普通权限 Spawner 两局 PID51144/43216 用户反馈正常；第二局覆盖 Firestorm charging=0、ready=1、active=2 与断电 HOLD，U+0378 缺字经原生反色 X 校验且反复复用 GlyphID11。普通退局未触发已监测字体析构，未以两次启动冒充同进程重载。追加 Debug 独立临时字体专项 PID51252：原生构造433880/析构433940、同一对象地址两轮 FontID2→3/GlyphID1→2，旧 ID 失效、缓存归零，并恢复原字体绑定（FontID4），结果PASS。原 spawn.ini/ra2md.csf 已按SHA256恢复，测试地图已移除。证据 D:/Ra2/SidebarText-regression-evidence-20261028/；详见 src/Render/SidebarTextGPU.md。本轮诊断、测试和记录尚未提交。下一步可选 UIName 接入。

> 最新提交推送：首批 cameo 文字、GlyphID/mask 缓存、atlas 适配、测试与验收记录已随 `f592c1c1` 推送至 `origin/feature/gpu-render`。下方本轮“尚未提交”为历史状态。

## 下一轮计划：文字链路收尾（待执行）

1. 先补生命周期回归：普通权限 Spawner 直进遭遇战，退出再开第二局，核对场景文字快照清空与实际字体生命周期。字体未释放时允许复用 GlyphID；只有真实释放/替换才要求旧 ID 失效并分配新 ID。补查同址复用、缺字拒绝、超武 on-hold/charge-drain 分支；未触发的分支明确保留未验证状态。
2. 接入可选图标内 UIName：复用 Issue #9 已恢复的 `6AC480` / wrapped DrawText，先核验实际显示开关和调用来源，再观察原生换行、底部对齐、DarkRed 颜色与裁剪。复用当前 GlyphCache/atlas；层序为闪烁后、数量和 READY 前。检查现有 `434CD0` / `434990` 简报 hooks 的过滤和 Bounds 恢复顺序，避免影响简报。名称关闭时不显示；未知来源不借旧文字。
3. 验收：短名/长名/中文与缺字、名称显示开关、页签/滚动、数量/READY/HOLD/时钟叠层、Alt-Tab、第二局。重复名称或位置/颜色变化不增加已缓存字形的解码与上传。完成 Debug 构建、CPU/GPU 定向测试、hook 核验及用户对照后再提交。

边界：本轮计划不扩展替代字体 provider、PCX cameo 或地图渲染。文字收尾后，先盘点 sidebar 剩余的资金/电力/雷达内容及其调查依据，再选下一块可独立验收的元素。本计划仅准备，尚未开始下一轮代码修改。

> 文字首批（2026-10-28）：依据 Issue #9 / b63a6fe，新增独立 GlyphID、owned 0/1 mask 缓存，复用多页 atlas；采集原生排版结果，接入数量、Factory READY/HOLD、Super readiness 及底色。用户批准两 cpp/两 h 项目项。Debug、CPU、实际 D3D12 R8/R32 测试及 11 处 hook 静态/运行链核验通过。普通权限 Spawner PID38272 用户反馈“没有异常”；日志覆盖中文等待/就緒、10/15/20 数量和超武就绪，11 个 glyph 与 101 个 SHP 上传后连续区间零新增上传。证据 D:/Ra2/SidebarText-evidence-20261028-38272/，详见 src/Render/SidebarTextGPU.md。本轮尚未提交；可选 UIName、完整缺字覆盖、第二局字体重载和替代字体 provider 仍待后续验证。

> 最新提交推送：Cameo状态效果、alpha blending、GCLOCK2 RLE行尾修复、测试及验收记录已随 `2128e6c4` 推送至 `origin/feature/gpu-render`。下方本阶段“尚未提交”为历史状态；暗化/闪烁实战画面对照仍待补。

> Cameo状态效果（2026-10-28）：用户澄清Countdown指GCLOCK2扇形遮罩，并选择alpha blending、接受RGB565舍入色差。已实现hover/暗化/闪烁/时钟，补原生最终状态观察hook6A99C3并修复GCLOCK2合法RLE行尾透明游程被拒绝的问题。构建、CPU/GPU测试、IDA核验通过；Spawner PID41100用户反馈“图标countdown mask一切正常”，日志帧1–24及hover已覆盖，暗化/闪烁实战未覆盖。详见src/Render/CameoEffectsGPU.md。改动尚未提交。

> 最新提交推送：基础cameo实现、布局与生命周期测试及验收记录已随 `bfb7551c` 推送至 `origin/feature/gpu-render`。下方本阶段“尚未提交”为历史记录；Countdown百分比和其他状态层仍未实现，滚动与其他页签动态验收待补。

> 基础cameo（2026-10-28）：依据gamemd-reverse 194cd7f，实现可见列表frame0快照、CAMEO palette及现有atlas绘制；补证本机Ares3.0p1动态列表，补上711B16/711B3D直接free通知。Debug构建、布局/解码/缓存/同址复用测试、IDA及运行跳板核验通过。普通权限Spawner PID36224对局用户反馈“图标正常显示（上面的Countdown百分比没有显示）”；百分比及其他文字／状态层尚未实现。已采到连续300提交零新增解码/上传；日志未覆盖滚动和其他页签。详见src/Render/CameosGPU.md；代码尚未提交。

> 后续测试默认直进遭遇战（2026-10-28，用户要求并已验收）：使用 `D:/Ra2/Launch-Phobos-Skirmish.ps1` 或同名.cmd，以普通权限运行 `Syringe.exe gamemd.exe --args="-CD -LOG -SPAWN"`。不要再让用户从主菜单手动开局。脚本启用PHOBOS_SIDEBAR_TRACE=1，并只自动确认本次新启动gamemd的Debugger Notice。原Launch-Phobos.ps1仍保留供主菜单专项测试；临时Launch-Phobos-Sidebar.ps1已改指向新入口。

> Spawner环境：官方 https://github.com/CnCNet/yrpp-spawner/releases/tag/v0.0.0.17 普通Release版，D:/Ra2/CnCNet-Spawner.dll SHA256 `D29B0D75B5DA24B556F194D1E79438974C32FCFA66BB5D4CE9C5575D57064043`；源码只读副本D:/yrpp-spawner，下载包D:/yrpp-spawner-download。spawn.ini配置XMP30S6.MAP、玩家Side=0、1名简单AI、10000资金、固定seed=20261028。旧spawn.ini备份D:/Ra2/Spawner-backup-20261028-144153/。PID50196自动进局，用户反馈“OK一切正常”；日志确认Spawner regular Release 0.0.0.17、D3D12 Running、sidebar ready=1、atlas一页且无budgetMisses/busy。此启动改动仅在机器本地，不改Phobos项目配置或第三方源码。

> 最新提交推送：atlas实现、GPU/生命周期测试与两局动态验收记录已随 `5d65f83c` 推送至origin/feature/gpu-render。稳定连续300提交的解码、索引上传、palette上传均零增量；第二局新FrameID、旧页回收。详见src/Render/FrameAtlas.md。下方“atlas未实现”为历史记录。

> 最新提交推送（2026-10-28）：CPU帧缓存阶段1、完整atlas设计、测试及验收记录已随 `1c68e475` 推送至 `origin/feature/gpu-render`。下方本阶段“未提交”为历史状态；atlas与独立GPU调色板尚未实现。本文件及机器专用启动脚本继续只保留本地。

> CPU缓存本轮验收：用户反馈“正常”；PID32472日志第一局600采集/9600帧请求，18次解码、9582命中，索引57632B，无invalid/budgetRejected。维修0→1→0复用ID8→18→8；第二次开局同reference地址分配新帧ID25..34，重载已由日志确认。稳定300次零增量有单测/适配器测试证据，本轮动态区间含新帧，尚未单独采到。证据 D:/Ra2/FrameCache-evidence-20261028-32472/。阶段1改动未提交，下一步atlas分配/上传，尚未实施。

> CPU帧缓存阶段1（2026-10-28）：已实现FrameCache.h/.cpp、ShapeFrameSource适配器、sidebar按需索引缓存及SHP/按钮生命周期hook。两组新增测试和三组回归通过，Debug x86构建通过，IDA专项跳过但hook冲突和本机字节补证通过。仅增加已获批准的FrameCache.cpp项目编译项。已部署D:/Ra2（DLL SHA256 7de5cac757c4452973c022c415dbf8cd745c35a63f7b454aa8a8bb9eaa08dd93），用户反馈正常；日志核验进行中。代码和设计文档未提交，atlas/palette GPU阶段未实施。详见 src/Render/FrameCache.md。

> 帧缓存设计（2026-10-28）：已编写 src/Render/FrameAtlasDesign.md，覆盖ResourceID/FrameID、按需解码、索引atlas多页分配、独立PaletteID、生命周期、预算、GPU提交/fence/退役和量化验收。首版sidebar范围，未修改运行代码、未部署；实施前核验新增生命周期入口及SDL能力。设计文档尚未提交。

> 最新提交推送（2026-10-28）：十按钮GPU接入、副窗口定位修正、测试与验收记录已随 `0514cbac` 推送至 `origin/feature/gpu-render`。下方“尚未提交”为历史状态；窗口定位目视验收仍待补。本文件与机器专用启动脚本继续仅保留本地。

> 副窗口定位修正（2026-10-28）：默认置于游戏右侧并留64像素间距，消息循环跟随游戏实际矩形变化重新定位；单屏空间不足不再向左覆盖游戏。构建/diff检查通过，已部署D:/Ra2，SHA256 762968f2872d923122e54f37c47708304543f6482417689046b82e7e5d9385f3。未重启游戏，下一次启动生效，分辨率切换目视验收待补。与按钮实现均未提交。

> 按钮首轮验收完成（2026-10-28）：用户反馈“已采集，一切正常”。D3D12/side=0/1024×768日志51条按钮变化，十按钮均ready=1；建筑页签0/1/2/4、防御和步兵页签0/1/2已采到。日志未覆盖所有按钮交互，多侧别/observer/重载/Vulkan仍未验证。证据 D:/Ra2/SidebarButtonsGPU-evidence-20261028-58912/debug-accepted.log；改动未提交。下方“等待反馈”为历史状态。下一阶段建造cameo。

> 按钮阶段（2026-10-28）：依据 gamemd-reverse f7476d9，已接入十个 sidebar 按钮的原始SHP/实际Drawer/持久状态快照与GPU绘制，补充初始化及Radar/UI资源清理门控。Debug x86构建及选帧32组合、ShapeImage、CursorImage测试通过；hook冲突和本机字节窗口补证通过，IDA无会话，专项P2–P4跳过。动态验收待进行，改动尚未提交。详见 src/Render/SidebarGPU.md。

> 按钮部署：D:/Ra2/Phobos.dll SHA256 c2290346f6de1c1de339069c58bd4fead0aafe3828f6f6d02cc8e131bc466079；旧版备份 Phobos-backup-buttons-20261028-130833。普通权限启动 PID58912，D3D12 Running，PHOBOS_SIDEBAR_TRACE=1；等待用户反馈及对局采样。

> 最新推送（2026-10-28）：sidebar 静态背景实现、测试与验收记录已随 `78f928af` 推送至 `origin/feature/gpu-render`，本地与远程一致。下方阶段 B 的“尚未提交”为历史状态；本交接文件及机器专用启动脚本仍不入库。

> 阶段 B 更新（2026-10-28）：六项 sidebar 静态素材的 plain/RLE 解码、布局快照、独立 GPU texture 与有界采样已实现并部署 D:/Ra2；Debug 构建和定向测试通过。用户完成 D3D12 遭遇战/Alt-Tab 画面对照，反馈除本阶段未实现的按钮和建造图标外一切正常；本场 side=0，六帧均为 plain，其他侧别/布局及实素材 RLE 尚未验证。证据 D:/Ra2/SidebarGPU-evidence-20261028-16700/，详见 src/Render/SidebarGPU.md。代码尚未提交。Launch-Phobos.ps1 已去掉强制 RunAs，用户已确认普通权限启动正常；原脚本已备份。下一步逐项添加按钮与建造图标。

> 最新提交状态（2026-10-28）：黑底独立 GPU 光标、测试、路线调整及 sidebar 路径记录已随 `ae7aa92f` 推送至 `origin/feature/gpu-render`。以下相关“尚未提交”描述为历史状态；本交接文件与机器专用启动脚本仍仅保留在本地。

## 当前实施路线（2026-10-28，取代下方历史计划）

> 阶段 A 实现更新：默认副窗口已改为黑底 + 独立 GPU 光标，并部署 D:/Ra2。Debug 构建和光标透明/裁剪测试通过；用户确认 D3D12 对局移动、动画、跨 sidebar、Alt-Tab 隐藏恢复正常。使用 `PHOBOS_GPU_MIRROR=1` 可显式选择旧镜像诊断路径。首版支持已加载的未压缩光标帧 ≤256×256；无新增 hook/线程。细节、证据与备份见 src/Render/CursorTrace.md 的“黑底 + 独立 GPU 光标”节；代码与记录尚未提交。下一阶段是从原始素材与状态开始迁移 sidebar。

用户决定：副窗口以纯黑背景起步，从鼠标、sidebar 开始逐项增加由 SDL_GPU 绘制的游戏元素，不再以 CPU 已渲染的 Primary/Composite/Sidebar 画面作为新路径底图。

- 原游戏主窗口继续作为行为与视觉对照；当前阶段不接管主窗口输入或停止原生绘制。
- 现有整屏镜像保留为独立诊断对照路径，不作为新路径的合成层；具体切换方式在实现时确定。本轮没有修改任何配置或渲染代码。
- CPU 可以读取游戏状态、解码原始资源、准备绘制数据并上传；元素的绘制与合成在 GPU 完成。上传 CPU 已画好的整张 Sidebar 不算完成 sidebar 迁移。
- 新路径每帧从黑色背景开始，未迁移元素保持缺省；不自动混入完整 CPU 镜像遮盖缺失内容。
- 迁移顺序不等于绘制层级：先实现的光标仍在场景/UI 之后绘制，保持顶层。场景内部排序、遮挡、深度和透明关系按各阶段调查结果实现。
- 继续沿用主线程 SDL_GPU 与现有窗口/设备生命周期，不因这次路线调整增加渲染线程或通用 RHI 抽象。

| 阶段 | 实现内容 | 验收要求 |
| --- | --- | --- |
| A：黑底 + 光标（已实现，D3D12 对局已验证） | 黑色清屏；读取实际光标资源、帧、调色板、热点、绘制位置和可见状态，缓存自有纹理，由 GPU 绘制 | 不捕获 Primary/Composite/Sidebar 底图；移动、动画、透明索引、热点、边缘裁剪和隐藏恢复正确；其余缩放/分辨率组合与 Vulkan 运行验收仍待补充 |
| B：Sidebar（下一步） | 先原始背景/装饰，再按钮与 cameo，再文字、计数、建造进度和其他动态状态；按需复用已验证的纹理上传与绘制能力 | 来自原始素材与游戏状态；支持布局、裁剪、层级与状态更新；sidebar 可辨识子部分逐项验收，不能用整面 CPU 拷贝替代 |
| C：地图基础 | 地形、地表覆盖物、静态场景；建立地图到屏幕的坐标变换 | 镜头移动、裁剪、地形高度与已有 UI/光标正确共存 |
| D：场景对象 | 按资源与绘制路径逐步加入建筑、SHP 对象、步兵、载具等；Voxel 单独调查并迭代 | 原始资源、动画、方向、调色板/阵营色与遮挡关系正确；共享路径按需提取 |
| E：效果与其余覆盖 | 阴影、弹道/粒子/激光、选择与状态标记、迷雾，以及菜单、对话框、影片等未覆盖路径 | 逐项明确覆盖与限制；菜单/影片可以作为独立支线，不能用对局通过代替全游戏覆盖 |
| F：接管与优化 | 完成必要覆盖后再评估主窗口输出、输入映射、性能及逐项停用冗余 CPU 绘制 | 有完整兼容与回退验证；副窗口验证阶段不宣称已经减少全部 CPU 渲染开销 |

### 阶段 A 的具体边界

读取 `WWMouse` 实际选中的 Image/frame/热点/绘制位置，不重写 Action→cursor 分派与动画计时，也不把 OS 最新鼠标位置任意配给另一时刻的帧。索引 0 透明，颜色优先遵循当前引擎转换表。只缓存自有像素/纹理；生命周期变化时失效缓存。

必要的短时状态快照同步仍需实现，但不再要求建立跨 sidebar/Composite/Primary 的整帧 MouseMutex 事务。底图中没有软件光标，因此无需 B0/B1/B2 背景擦除或干净底图修复。

### 从当前关键路径移出的工作

- 暂停为去光标而增加 sidebar 五条 Primary copy 分支探针。
- 暂停 Primary 副本背景修复、B0/B1/B2 有效区域重建及跨呈现的大范围锁事务。
- 已有调查、日志、hook 检查继续保留，作为状态读取、资源生命周期、坐标与后续对照依据，不把此前发现的竞态描述为已修复。

## 历史记录（以下日期、下一步和验收状态仅描述当时的镜像路线）

> Sidebar 路径定位更新（2026-10-28）：跨界记录已以 `035d5b3d` 推送。Sidebar 最终写入入口为 BlitSidebar@6A70E0，五个直接 Primary copy 点：6A722C/6A7312/6A739D/6A742E/6A747E，绕过 DoBlit。常规帧经 DrawOnTop@4F4502 的虚拟 Draw 调用，WM_PAINT@777723 则在 DoBlit 之后调用它。更正：“frame=0”只说明在 4F4583 晚段探针前，并非整个 DrawOnTop 之外。定位记录已补入 CursorTrace.md，暂未提交；没有安装新 hook。

> 跨 sidebar 采集更新（2026-10-28）：CursorTrace-45208-1.csv 约 5 秒、14413 条、0 丢弃、312 次 Primary copy 全部成功。捕获 7 对跨界矩形，并在第 191 帧实际观察到 staging 释放锁后、Primary copy 前鼠标线程恢复/移动/重画。另有 11 次 B2 保存失败而 B0 更新成功；必须分别跟踪有效区域。详细证据与下一步见 src/Render/CursorTrace.md 的“跨 sidebar 边界采集”节；该新增记录尚未提交。

> 提交状态更新（2026-10-28）：镜像实现 `c7d14cce`、光标追踪与核验记录 `f388b469` 已推送至 `origin/feature/gpu-render`。下文历史段落中的“第 4 步未提交”已过时；本交接文件和机器专用 Launch-Phobos.cmd 继续留在本地。

> 2026-10-28 更新：用户已手动确认镜像画面没有问题；旧记录中的“画面尚未确认”已被此反馈取代，但不推定所有专项测试通过。光标静态调查已交付，默认关闭的事件追踪已部署到 D:/Ra2 并采集一轮遭遇战：约 5 秒、6894 条事件、0 丢弃、313 次 Primary 复制全部成功。发现 sidebar staging 在常规 DrawOnTop 区间之前，且本轮 sidebar 背景复制因矩形完全在左侧之外返回失败；下一步需要重点覆盖跨 sidebar 边界与旁路，再实现无光标底图。备份、日志、完整统计与检查表见 [src/Render/CursorTrace.md](src/Render/CursorTrace.md)。

> 记录时间：2026-10-27。分支：`feature/gpu-render`（已推送 `origin`，最新已推送提交 `ebd96f5c`）。
> 本文件仅作交接用途，未加入版本库。

## 1. 目标

在游戏主窗口之外创建一个副窗口，用 SDL3 + SDL_GPU（D3D12 优先，Vulkan 回退）把游戏画面再画一次。最终目标是游戏全部内容都改由 GPU 绘制。阶段 1 只做到 RHI 初始化和整屏镜像。

参考源码：`D:\gamemd-reverse\src\gamemd\consolidated`（`VideoMode.cpp`、`PrimarySurface.cpp`、`DirectDrawMode.cpp`、`GScreenPresentation.cpp`、`GScreenDrawing.cpp`、`DisplayResourceLifetime.cpp`）。

## 2. 已确定的设计决定

| # | 主题 | 决定 |
| --- | --- | --- |
| 1 | RHI | SDL3 + SDL_GPU，D3D12 优先，失败回退 Vulkan，都失败就关闭副窗口。NVRHI 官方不支持 32 位，已排除；SDL_GPU 没有 D3D11 后端 |
| 2 | 接入方式 | 动态链接 `SDL3.dll`，用 `/DELAYLOAD` 延迟加载；`lib/SDL3/` 放 3.4.18 官方 VC 开发包里的头文件和 x86 导入库 |
| 3 | 改动放在哪 | 只放在 fork 里；新代码集中在 `src/Render/`，原有代码的改动尽量少 |
| 4 | 开关 | `RA2MD.INI` 的 `[Phobos]` 节：`GPURender.Enable=yes`（默认）、`GPURender.Driver=auto\|direct3d12\|vulkan`；命令行 `-NoGPURender` 优先 |
| 5 | 副窗口 | 顶层窗口、不设 owner、`WS_EX_NOACTIVATE`；初始客户区和游戏 1:1，放在游戏窗口右侧，放不下就放到另一个显示器，再不行贴在游戏所在显示器的右边缘；可调整大小，等比缩放加黑边；点 ✕ 只隐藏（进入 Hidden 状态，保留设备）；用户没手动调过大小时跟随游戏分辨率 |
| 6 | 显示环境 | 游戏是窗口模式，或者 `ddraw.dll` 不在 System32/SysWOW64（说明装了包装器）时才启动；原生独占全屏不启动 |
| 7 | 阶段 1 验收 | 每帧镜像整个游戏画面，纹理用 `R16_UINT`，在着色器里按 `ColorPackData` 解码 565/555 |
| 8 | 着色器 | 只写一份 HLSL，MSBuild target 调 dxc 生成 DXIL 和 SPIR-V 头文件（`$(IntDir)Generated\Shaders`）；dxc 优先用 `PhobosDxc`，其次 `$(VULKAN_SDK)\Bin\dxc.exe`（Windows SDK 自带的 dxc 不支持 SPIR-V） |
| 9 | 线程 | 全部在主线程；present 用 immediate 模式，不支持就用 mailbox，最后才用 vsync；用非阻塞的 `SDL_AcquireGPUSwapchainTexture` |
| 10 | 错误处理 | 状态机：Disabled / Initializing / Running / Hidden / Failed / Shutdown。可跳过的错误按限频记日志；严重错误进入 Failed 并释放全部资源，游戏照常运行；不尝试重建设备 |
| 11 | 代码组织 | 不另写 RHI 抽象层；SDL 头文件只在 `src/Render/*.cpp` 里引用；`GPURender.h` 里不出现 SDL 类型 |
| 12/14 | 镜像来源 | **镜像 Primary 表面**（第 14 个问题改选了 A）。原因：`DoBlit` 会按区域拼接（Composite 632 宽、Sidebar 168 宽、屏幕震动偏移、侧边栏左右、缩放），而且光标和 FillRect 会直接写到 Primary 上 |
| 13 | 光标 | **阶段 1 不单独画光标**。光标本来就在 Primary 上，镜像时自带。原计划的 `0x7B90C0` hook 和 GPU 光标层取消，推迟到阶段 2 |
| — | 退出兜底 | 不在 `0x7CD8EA` 兜底（那个 hook 只在 `#ifdef DEBUG` 下存在），只在 `0x6BB8E0` 清理 |

## 3. Hook 清单（已核对原始字节；Ares、Phobos 在附近都没有冲突；在 gamemd.exe 和 gamemd-spawn.exe 里字节相同）

| 地址 | 大小 | 原指令 | 用途 | 状态 |
| --- | --- | --- | --- | --- |
| `0x6BDE19` | 5 | `mov al, [0x8A0DEF]` | WinMain 调用 `AllocatingNewSurfaces` 之后；**第一次显示初始化不经过 Set_Video_Mode** | 已提交、已实测 |
| `0x561163` | 5 | `push "Mode change complete"` | Set_Video_Mode 成功（之后切换分辨率） | 已提交，**还没实测走到** |
| `0x6BB8E0` | 5 | `mov al, [0x8A0DEF]` | 在 DisposeOfStuff 入口执行 Shutdown | 已提交、已实测 |
| `0x4F4B7E` | 5 | `test bl,bl; pop edi; pop ebp; pop ebx` | `DoBlit` 中 `Primary->CopyFromPart` 之后触发一次 present | 第 4 步，**还没提交** |

其他地址：
- 鼠标互斥量 `MouseMutex` 在 `0xB78168`（`HANDLE`）。
- `ColorPackData` 在 `0x8A0DD0`，字段顺序是 R_SHL、R_SHR、B_SHL、B_SHR、G_SHL、G_SHR。通道从第 SHL 位开始，位数是 8 - SHR。
- 游戏窗口句柄 `Game::hWnd` 在 `0xB73550`。

## 4. 提交历史（feature/gpu-render）

```text
ebd96f5c feat(render): create the GPU render window and device      （第 3 步）
f211dd46 feat(render): add GPURender settings, state machine and SDL3 loader（第 2 步）
6971fd8d chore(render): add SDL3 dependency and shader build step    （第 1 步，含 .gitattributes 的 *.lib -text）
```

## 5. 模块

```text
src/Render/
├── GPURender.h/.cpp        状态机、配置（自己读 RA2MD.INI，因为显示初始化比 0x5FACDF 早）、初始化/释放、present 节流、计时器
├── Hooks.cpp               0x6BDE19 / 0x561163 / 0x6BB8E0（+ 未提交的 0x4F4B7E）
├── SDLLoader.h/.cpp        先 LoadLibraryA 探测，再检查版本必须是 3.4.x，通过后才走延迟加载
├── SecondaryWindow.h/.cpp  窗口创建和定位、事件处理（关闭、尺寸变化、重新露出）、跟随游戏分辨率
├── SurfaceMirror.h/.cpp    （未提交）持有 MouseMutex 时 Lock Primary → 映射 transfer buffer（cycle）→ 上传到 R16_UINT 纹理（cycle）
├── PresentPass.h/.cpp      （未提交）管线和 sampler，用 viewport 实现等比加黑边，push uniform 解码参数，画全屏三角形
└── Shaders/
    ├── Fullscreen.vs.hlsl     （已提交）由 SV_VertexID 生成全屏三角形
    └── DecodeRGB16.ps.hlsl    （未提交）用 Texture2D<uint>.Load 读像素，按 Shift/Bits 解码；Vulkan 下加 combinedImageSampler
```

SDL 的 hint（在 `SDL_InitSubSystem(VIDEO)` 之前设置）：
- `SDL_WINDOWS_ENABLE_MESSAGELOOP=0`：不让 SDL 抢游戏的消息，包括 WM_QUIT。
- `SDL_WINDOWS_DPI_AWARENESS=unaware`：不设的话，SDL 会把整个进程改成 Per-Monitor V2。
- `TIMER_RESOLUTION=0`
- `VIDEO_ALLOW_SCREENSAVER=1`
- `WINDOW_ACTIVATE_WHEN_SHOWN=0`
- `QUIT_ON_LAST_WINDOW_CLOSE=0`

设备创建时显式设置 `DEBUGMODE=false`（默认是 true，会开校验层）和 `D3D12_ALLOW_FEWER_RESOURCE_SLOTS=true`。

## 6. 第 4 步当前状态（代码已写完，Debug 构建通过且没有警告，**还没提交**）

- `Present()` 的流程：记录时间 → `SurfaceMirror::Capture` → 获取 command buffer → 有待上传的数据就开一个 copy pass 上传 → 非阻塞获取交换链纹理 → `PresentPass::Draw`（先清成黑色，有源纹理就画全屏三角形）→ submit。
- 节流：`MinPresentIntervalNS = 1s/120`。`DoBlit` hook 和 16ms 计时器都走 `RequestPresent()`；窗口事件（尺寸变化、重新露出）直接 `Present()`。
- 读取区域：Primary 的尺寸等于 `WindowBounds` 时（包装器的情况）从 (0,0) 开始读；否则（原生窗口模式，Primary 是整个桌面）用 `ClientToScreen` 算出起点；越界就跳过这一帧。
- 实测：日志里出现了 `Mirroring the game screen at 800x600.`，说明捕获和上传至少成功了一次。**还没有截图确认副窗口的画面和游戏一致**：截图脚本用纯 Python 编码 PNG 太慢，加上游戏约 6 秒后就自己退出，没赶上。

## 7. 已实测通过（第 3 步，Debug 版，cnc-ddraw，windowed=false）

- 默认配置：D3D12，immediate 模式，`Disabled → Initializing → Running → Shutdown`。副窗口左边缘等于游戏窗口右边缘，带 `WS_EX_NOACTIVATE`，前台窗口仍是游戏。
- 计时器在约 6 秒里触发 141 次（说明游戏的消息循环会派发它）。
- `-NoGPURender`、`Driver=Vulkan`、`Enable=no` 加非法的 `Driver`、缺少 `SDL3.dll`（找到 VulkanSDK 的 64 位 DLL，错误 193 → Failed）都符合预期，进程正常退出。

## 8. 还没验证的部分 / 已知问题

1. **这个环境里直接启动 gamemd 进不了主菜单**：放完开场动画就自己正常退出（`Saving game settings`），之前的日志也一样，和本功能无关。所以下面这些都还没走到：
   - 切换分辨率（`0x561163`）
   - 关闭副窗口进入 Hidden
   - 手动调整副窗口大小
   - 对局中的镜像效果

   需要用 CnCNet 客户端或完整的 `spawn.ini` 加 `gamemd-spawn.exe` 进对局测试。现在的 `spawn.ini` 只有 `Scenario=mp.dat`。
2. **副窗口的画面还没确认**：需要能用的截图手段，或者人工看一眼。
3. **播放动画时消息被吞**：Bink 动画循环（`0x53E879`）只派发 `WM_PAINT`，副窗口收不到消息，计时器也停，可能被系统短暂判为"未响应"。
4. **退出日志会丢**：Ares 在 `0x6BB8E0` 之前就关闭了日志（`Closing log file on request`），所以 `Shutdown` 写的日志看不到。第 3 步是用临时写文件的方式确认 Shutdown 执行到了，那段代码已删除。
5. **原生 DirectDraw 窗口模式**：读取 Primary 等于读桌面，盖在游戏上的其他窗口（包括副窗口本身）也会被截进来，速度也慢。目前只测过 cnc-ddraw。
6. **缩放只有点采样**：`R16_UINT` 不能用硬件线性过滤，要线性得在着色器里手动做双线性。

## 9. 下一步

1. 截图确认第 4 步的画面：建议改用 PIL 或 GDI+ 保存截图，或者人工目测。确认后提交第 4 步。
2. 准备一个能进对局的启动方式（完整的 `spawn.ini`，或者用户手动进对局），验证 `0x561163`、Hidden、调整大小和对局中的镜像。
3. 第 6 步：CI（`.github/actions/build-phobos/action.yml`）下载固定版本的官方 DXC（`v1.9.2609`，未实测，同系列的 1.9.2602 支持 SPIR-V）并设置 `PhobosDxc`；**这是 CI 改动，需要先让用户过目 diff**。
4. 第 7 步：验收（菜单、对局、侧边栏、切换分辨率、关闭副窗口、`-NoGPURender`、缺少 DLL、每帧耗时）。
5. 发布时附带 `SDL3.dll`（x86）的打包方式还没定。

## 10. 本机环境备注

- **构建**：当前终端设置了 `NoDefaultCurrentDirectoryInExePath=1`，`build_debug.bat` 里的 `call build` 会找不到脚本。要这样调用：
  `env -u NoDefaultCurrentDirectoryInExePath MSYS_NO_PATHCONV=1 cmd.exe /c "D:\Phobos\scripts\build_debug.bat"`
- **部署**：`D:\Ra2` 里现在是第 4 步（未提交）的 Debug 构建，外加 `SDL3.dll`（来自 SDL3-devel-3.4.18-VC.zip 的 lib/x86）。原来的文件备份为 `Phobos.dll.bak` 和 `Phobos.pdb.bak`。`RA2MD.INI` 已恢复成测试前的内容。
- **启动**：`Launch-Phobos.ps1` 会请求管理员权限（UAC）。不提权直接运行 `Syringe.exe gamemd.exe --args="-CD -LOG"` 也能启动。
- **辅助脚本都在临时目录，不会长期保留**：`/tmp/syhk/*.py`（解析 `.syhks00` hook 表、延迟导入表、导入表、call 交叉引用）、`/tmp/cs/dis.py`（capstone 反汇编 `D:/Ra2/gamemd.exe`）、`/tmp/probe/*.py`（枚举窗口、截图）。
- 当前终端里 IDA MCP 打开的 `D:\Ra2\gamemd.exe.i64` 没做自动分析，函数没识别，反汇编改用了 capstone。

> C4.1 ABuffer CPU 模型 + 原生对照（2026-10-30，未提交）：#17 已交付（gamemd-reverse `5ad221c`，`docs/zh-CN/abuffer-gpu-handoff.md`）。交接修正：ABuffer 为 16 位 WORD；FogOfWar 开时第一遍覆盖也用 FOG；掩码按邻格位“未置位”；写入不含高度；邻格查询不检查合法性（InvalidCell）。新增 `ABufferImage.h`（纯规则+`ABufferImageTests`）、`ABufferSnapshot.h`（trace 中整屏重建并与原生 `[87E8A4]` 逐像素比，含 `stale`=Visibility/Foggedness 与当前结果不同的格数）；`TerrainImage::ScanCell` 共用扫描。Debug 构建无警告，render 回归全部通过。实测（DLL `a4b07dc4…`/`d89cd0f5…`）：GPUTOMB 开局/滚屏/上边缘/左上角、GPUSHRD 开局黑幕/MCV 逐格揭开/展开/滚进黑幕全部 mismatched=0；FogOfWar=Yes 下 stale=0 时全为 0，MCV 移动中 stale>0 的格子造成短暂差异（原生增量缓存滞后，停下后归零）。上边缘暗带 4721 像素确认来自外轮廓邻格=InvalidCell。证据 stage-c/c41*；spawn.ini 已还原。下一步 C4.2：GPU ABuffer 纹理（格式需按 WORD 语义重新确定）。

> C4.1 已提交推送 `a79ea5e2`（memory `32ee7cce`）。C4.2 GPU ABuffer（2026-10-30，未提交）：ABuffer 快照改为每次地形采集都做，帧进独立 FrameCache 经 FrameAtlas 上传；新 `ABufferPass.h`（R8_UNORM、独立渲染通道清 127、先全部覆盖再全部变暗 REVERSE_SUBTRACT）与 `ABufferFrame.ps.hlsl`（vcxproj 只加这一条，VS 复用 TerrainTile.vs）；TerrainPass 抽出 `InstanceBuffer`/`CreatePipeline`，地形 PS 按画布像素读 ABuffer。trace 加 `split`（GPU 顺序 vs 原生逐格顺序）。D3D12 GPUTOMB 上边缘/左上角只剩光标 188 像素（C3 为 4905）；D3D12、Vulkan GPUSHRD+FogOfWar 差异只在光标与提示框；split 全为 0。采集 1.41 ms（C2.5 约 1.0）。最终 DLL `bc86ae75…`。配置已还原。

> C4.2 已提交推送 `7ff08512`，C4.3 结论与经验笔记入 memory `d510023a`（GPURender 索引、gamemd-reverse 交接索引 #17、新经验“用 Visibility 与 Foggedness 判别 ABuffer 差异来源”）。阶段 C 下一步：C5 弹坑与 overlay。

> C5 规划（2026-10-30，用户确认）：C5.0 已发布 gamemd-reverse [#18](https://github.com/hzqst/gamemd-reverse/issues/18)（smudge 与格子 overlay 及阴影：480350 尾部、4D1890 deferred、6D6D10/47F6A0/47F510、帧选择、Convert/亮度/ABuffer、Z 读写与是否被地块 Z 遮挡、运行模块 47F71D/47F974/47F87D/47FA1F），正文 `stage-c/issue-18.md`。交付后：C5.1 smudge（CPU 模型+原生 Tile/Z 对照，再上 GPU）；C5.2 overlay（矿石/宝石/围墙/桥/静脉，阴影视交接决定）；若 overlay 会被地块 Z 挡住，则把 GPU 深度缓冲（D32_FLOAT/D16）提前到 C5。

> C6 规划（2026-10-30，用户确认）：已发布 gamemd-reverse [#19](https://github.com/hzqst/gamemd-reverse/issues/19)（Tile 上的 Terrain 对象与 TMP/悬崖投影阴影：6D3AC0→6D97D0、4D1890 Terrain 24h、6D3040→6D7C00→4802A0、TerrainClass 绘制及 Phobos 71C1FE/71C2BC，Tile 与动态层划分、阴影公式、Z），与 #18 并行，正文 `stage-c/issue-19.md`。之后阶段 C 收尾：真实对战地图 native_terrain 对 gpu。

> C5/C6 受控地图（2026-10-30，本地）：`make_maps.py` 新增 GPUOVER（tomb 原 overlay 5584 格）、GPUSMUD（tomb + 24 个 smudge：CRATER01–12、BURNT01–12，开局视野内）、GPUTREE（tomb 原 220 棵树）、GPUFULL（三者合一）、GPULIBF（libya 城市：原 overlay 3158 格含宝石/疑似围墙 + 10 棵 Tiberium 树）、GPUARCF（arcana 雪地：overlay 3997 格 + 626 棵树）；旧 5 张字节不变。新增 `scripts/overlays.py`（LCW 解 OverlayPack 统计编号/格子）。实载 GPUFULL/GPULIBF/GPUARCF 正常；GPUFULL 开局 tactical -2678,3644 有矿石+弹坑+烧痕，上滚两屏 -2678,2276 有树木/仙人掌（带影子）、岩石 overlay、悬崖与疑似投影阴影，作 C5/C6 主视角。GPUOVER/GPUSMUD/GPUTREE 是 GPUFULL 的子集，未单独载入。证据 stage-c/c5maps；spawn.ini 已还原。

> C5.1 弹坑（2026-10-30，未提交）：#18 已交付（gamemd-reverse PR #20 / `af42121`，`docs/zh-CN/smudge-overlay-gpu-handoff.md`）。交接要点：smudge 逐格穿插在本格 TMP 之后、固定帧 0、按 SmudgeData 子格号移回足迹原点、`0xE00`（TransXlatAlpha）读 ABuffer 不读写 Z，查色与 TMP 的 420140 相同；overlay 本体 `4E00` 与阴影 `4601` 严格 `<` 测试并写 Z，C5.2 必须先有 GPU 深度。实现：`TerrainImage::SmudgePosition`（+测试）、`TerrainSnapshot` 每格 TMP 后追加弹坑实例（`ShapeFrameSource` 共用缓存，`Placement::Storage`）、trace `smudges/smudgeMissing`。GPUSMUD D3D12 开局与四向滚屏、Vulkan 开局与左滚：只剩光标 188 像素；DLL `ad1859f8…`。证据 stage-c/c51、c51v；配置已还原。下一步 C5.2：GPU 深度缓冲 + overlay（本体/阴影两遍扫描、矿石变体、围墙、桥、ZShape、Deg90 梯度），先 CPU 模型对原生 Tile/ZBuffer。

> 存储全局变量移入 .cpp（2026-10-30，未提交，用户要求）：PaletteCache/ShapeFrameSource/CursorTrace/SidebarSnapshot/CommandBarSnapshot/TerrainSnapshot/ABufferSnapshot 的 `inline` 状态变量改为头文件 `extern` + 各自 `.cpp` 定义；TerrainPass/ABufferPass 照 PresentPass 拆分，着色器字节头文件（内部链接数组）、GPU 资源与实现移入 `.cpp`。vcxproj 加 9 个 ClCompile；回归脚本按测试链接 PaletteCache/ShapeFrameSource/CursorTrace.cpp（FrameAtlas 两个测试也要 PaletteCache.cpp）。构建无警告、回归与 GPU 读回测试通过；GPUSMUD D3D12 开局/左滚只剩光标 188 像素，侧栏与底栏 ddraw/gpu 最大差 1；DLL `f89bc03a…`。

> C5.1 与存储拆分已提交推送 `283df71b`（memory 经验笔记 `10948817`），feature/gpu-render。

> C5.2a GPU 深度缓冲（2026-10-30，未提交）：gamemd-reverse PR #20/#21 已 merge（`2c6231da`、`62effcbc`），#19 交接关键地址（6D97D0 筛选与逆序、71C1B0 参数、4802A0 的 58-15L）用 capstone 在原版上抽查一致。用户确认方案：战术区改画进 ViewBounds 尺寸离屏目标（RGBA16F 存通道整数值，为阴影 `(old>>1)&mask` 精确暗化铺路；D32_FLOAT，D16 后备），present 合成；不做 GPU 深度回读。实现：TerrainBatch 实例 Depth 字段（基值|Z 平面偏移）、按页+深度模式分段；TerrainTile.ps 写 SV_Depth、输出通道整数；新 TerrainComposite.ps；TerrainPass 两条地块管线（无 Z / LESS_OR_EQUAL+写）、合成管线、Render 独立通道。GPUTOMB D3D12/Vulkan 开局、滚屏、悬崖视角只剩光标 188；GPUSMUD Vulkan 同；反向比较实验 97.85% 不同证明测试生效。最终 DLL `6958cf5f…`。证据 stage-c/c52*。

> C5.2a 已提交推送 `09cf144e`。

> C5.2b overlay CPU 模型（2026-10-30，未提交）：新 `ShapeDepth.h`（`421B60`/`7BBE20` 字面裁剪、`4373B0`/`437A10` 行基值与分带、严格 `<`、暗化，+`ShapeDepthTests`）、`OverlayImage.h`（`6D6D10` 扫描、`5FDCC0` 偏移、`480110` 抬升、桥帧图案、Phobos ramp、Tiberium 变体、阴影帧，+`OverlayImageTests`）、`OverlaySnapshot.h/.cpp`（每次地形采集后采集 overlay 本体/阴影；trace 中 CPU 重放 TMP+弹坑+overlay，与原生 Tile/ZBuffer 逐像素比 `tilecheck`）。GPU 未画 overlay。发现：①越界帧/空帧/原生绘制后仍未加载的引用都不画；②ramp Tiberium 的 Z shape 裁剪起点可落在帧内，RLE 路径仍从第 0 列读并从右侧减宽，Z 字节取 Z shape 帧原始数据（RLE 编码流），按原生指针在采集时算出 `ZBytes`；③纵向滚屏后原生只重画条带，带 Z shape 的 Flag=1 绘制基值取 clip 顶行，与完整重画不同，trace 改为先 `MarkNeedsRedraw(1)` 再核对。受控地图 GPUOVX（墙/箱/岩石/宝石/TIB01 + `[Riparius] AllowRamps=yes`）、GPUARCO/GPULIBO（去树），`scripts/make_ovx.py`。GPUOVER/GPULIBF/GPUARCO/GPUOVX 共 15 个视角颜色与 Z 全部一致（含 147 墙、97 阴影、18 个 Z shape）。未覆盖：平铺帧、桥格、veins、Rubble、Vulkan。DLL `90a8aed3…`。证据 stage-c/c52b；配置已还原。下一步 C5.2c：GPU 画 overlay（严格 `<` 管线、行基值/梯度参数、ZBytes 平面、暗化混合）。

> C5.2b 已提交推送 `89773426`（memory `e5aee069`）。C5.2c GPU overlay（2026-10-30，未提交）：裁剪与行基值移到采集（`Draw` 存已裁剪的位置/帧起点/`Rows`/梯度），CPU 重放与 GPU 共用；`ShapeDepth::RowBase` 闭式行基值（+测试）；`TerrainBatch::Shape`/`PlaceShape`/`HalfMask`（+测试）；`TerrainPass` 存储缓冲模板化（`StorageBuffer<T>`），overlay 实例接在地块之后，两条 `LESS`+写管线（本体；阴影 dst·SRC_ALPHA，PS 输出 alpha 1/2），Z 字节存储缓冲；新 `TerrainShape.ps.hlsl`。D3D12 GPUOVX/GPUARCO/GPULIBF、Vulkan GPUOVX 只剩光标 188 像素（含 150 阴影、47420 次 Z 拒绝、18 个 Z shape）；trace `gpu=512/0`。DLL `12fd290c…`。证据 stage-c/c52c；配置已还原。

> C5.2c 已提交推送 `43d1f790`。

> C6 Terrain 对象（2026-10-30，未提交）：#18/#19 已是 CLOSED。新 `TerrainObjectImage.h`（静态帧、阴影帧、深度、`6D7C00` 悬崖扫描、`547230` 记录选择、悬崖中心，+`TerrainObjectImageTests`）；`OverlaySnapshot` 在 overlay 之后逆序采集 Layer[2] 的静态 Terrain（本体 `4E00`/Deg90/`-h-12`，阴影 `Frames/2`/`-h-3`/梯度 0，Phobos `71C1FE`/`71C2BC`，Tiberium Y-16），再采悬崖投影；`Draw::Source`/`Index` 区分层，GPU 与 CPU 重放自动复用。D3D12 GPUTREE/GPUFULL/GPUARCF/GPULIBF/XMP30S6、Vulkan GPUTREE：`tilecheck` 全一致，GPU 只差光标。悬崖投影：本 mod tile 类型无 ShadowCaster，无法动态核对。DLL `0ed21adc…`。证据 stage-c/c6；配置已还原。
> C6 补测（2026-10-30）：GPUDMG（GPUTREE+墙+树+`<Player @ A>` 坦克，`scripts/make_dmg.py`）强制攻击后墙受损帧/被毁/连接帧变化，`tilecheck` 一致、GPU 只差光标；原版树不开受损帧（damaged=0）。trace 加 `damaged`。DLL `04516151…`。
> 阶段 D 开始（2026-10-30）：计划 D1 Tile 静态建筑 + 4D1890 建筑分支（沿用 Tile 对照）→ D2 mode 2 SHP 单位/步兵/动画（需新对照点）→ D3 Voxel。已发布 gamemd-reverse #22（D1，附 mode 2 总览），草稿 stage-d/issue-d1.md。等交接。
> 阶段 D 调整（2026-10-30）：新增 D2 = mode 2 动画/崩塌 Terrain（矿柱等），已发布 gamemd-reverse #23（草稿 stage-d/issue-d2-terrain.md），负责 6D8DB0 流程、Layer 排序、mode 2 的 ZBuffer/ABuffer 与原生 Composite 对照点；与 #22 并行，#22 的 mode 2 总览改引用 #23（已在 #22 评论分工）。原 D2 SHP 单位/步兵/动画顺延为 D3，Voxel 为 D4。
> C7（2026-10-30，未提交）：桥格与平铺帧补测——新 GPUBRDG.MAP（scripts/make_brdg.py，sov01umd 高桥 22 格 + 宝石 GEM04/09/11 生长 8–11 阴影、TROCK02/SROCK04 阴影、SIGN03–06 本体等平铺帧），D3D12 开局/上滚/左滚、Vulkan 开局/上滚 tilecheck 颜色与 Z 全一致，GPU 只差光标。4D1890：XMP30S6 + FogOfWar 实测只有建筑记录（39–42 条，视野内 0），overlay/smudge/Terrain 记录从未出现（与 #18/#19 构造入口无调用者一致）→ 不画，trace 加 fogged/foggedBuildings/foggedInView 计数。最终 DLL dbd936ec…626c，回归通过，证据 stage-c/c7。待用户决定是否仍实现三类分支。

> D2a（2026-10-30，未提交）：#23 交接 = gamemd-reverse PR #24（用户要求先不合并），capstone 抽查 6D465B/6D465F/6D4664/6D9224 一致。实现：Hooks.cpp 新增 6D465F/6D4664 观测 hook（return 0 保留原 call）；新 CompositeSnapshot（按 #23 §9.2 复制锁定目标的 ViewBounds：Temp=Composite=EBX、LockLevel>0、16 位、pitch）；输入只在 trace 渲染复制，输出在 GPU 窗口或 MCP 运行时每帧复制；MCP game_screenshot 新增 source=native_composite（CaptureImage.Terrain 改名 Region，元数据 native_composite 含 game_frame，timestamp_semantics=render_copy_read；Automation::IsRunning）；OverlaySnapshot 新增 CompositeDraws（6D8DB0 层序遍历、5AFB80 视图范围、5865E0、与静态 Terrain 共用 71CC50/71C1B0 采集，Draw::WritesDepth=false、Phobos 71C1FE 动画帧），Replay 抽出 Model/NativeAlpha/NativeDepth/ReplayDraws，trace 新增 composite/compositedraws/compositecheck/compositesamples。GPUANIM.MAP（stage-d/scripts/make_anim.py，GPULIBF 矿柱 + 4 棵，开局 (70,70)，上滚 1.8s）：D3D12/FogOfWar/Vulkan 共 ~20 个样本 body=0 shadow=0，输入=Tile；其余差异为 MCV（unbounded）与闪光动画（others）。观察者模式会让 Automation 暂停失败。DLL 82c92fbb…；证据 stage-d/d2a；配置已还原。下一步 D2b：GPU 画动画 Terrain（只读深度管线）并与 native_composite 对比。

> D2a 已提交推送 `d507b7de`；vera20k 建筑线索已评论到 #22（issuecomment-6521687544）。D2b（2026-10-30，未提交）：TerrainPass 新增 LayerPipelines（本体/阴影，严格 < 只测不写深度），CompositeDraws 接在 Tile shape 之后共用实例/ZBytes（PrepareShapes 改为按列表返回 runs），GPURender 为 CompositeDraws 请求 atlas。GPUANIM D3D12/Vulkan 上滚、左右滚：GPU 对 native_composite 只差光标 187–188（左滚另有一个矿石闪光动画 6 像素，GPU=Tile）。DLL 933132e2…；证据 stage-d/d2b（scripts/cmpcomp.py）；配置已还原。

> D1a/D1b 已提交推送 `5851ff0b`。D2b 已提交推送 `15e41979`；PR #25 = #22 交接（D1）。D1a/D1b 活建筑（2026-10-30，未提交，方案 A：4F4583 按当前状态在 CPU 重推参数）：新 BuildingImage.h（帧/深度/ZShape 偏移/大门帧，+BuildingImageTests）；OverlaySnapshot::Capture::Building（6D9920/43CEA0/43D290/705E00，Phobos 43D363/43D6E5/43D874/70632E/706389，原生调用取当前帧、Foundation、6D1FE0、配色、闪烁亮度、tint），Ground 层正序接在悬崖之后；Draw::Tint（仅 RLE）→ TerrainBatch::Shape::Tint → TerrainShape.ps OR；按 Ares 437CCC 去掉 FlatZShape 判定；trace 新增 buildings 行（bibs/doors）。GPUBLD.MAP（make_bld.py，ancient tomb，开局 (117,75)，EXTRA 加 GAWEAP/NAREFN/GADEPT 受损/AMRADR/GAAIRC）：D3D12 原开局与新开局、MCV 展开 Buildup 中途/建成，Vulkan 开局/Buildup/上滚：tilecheck 全一致，GPU 在 composite==terrain 的像素上只差光标（scripts/cmpbld.py）。未测 tint/大门/卸载/出售。DLL 5531665f…；证据 stage-d/d1a；配置已还原。下一步 D1c 迷雾建筑（4D1890 类型 6）。

> D1c 迷雾建筑（2026-10-30，已提交推送 `09967aac`；迷雾动画的原生增量缓存差异经用户确认维持现状）：BuildingImage::Fogged*（(144,172) ZShape、-h-2 本体/阴影、动画 savedZ-h-2、clip 截到图像底，+测试）；Capture::FoggedBuilding（4D1890 类型 6：Owner 当前配色/TerrainPalette 格子 LightConvert，亮度 Intensity_Normal+ExtraLight，part0 本体+阴影（不看 drawShadow）+底座，动画 2E00 只测 Z、ANIM_PAL/ShouldUseCellDrawer、UseNormalLight、DetailLevel）；记录在 overlay 之前按排序表采集（未排序时用 4D2ED0 qsort 副本）；Call::ClipHeight；Layer::Fogged/FoggedAnim；trace 新增 foggedAnims/foggedDetail/foggedInvisible/foggedUnsorted 与 foggeddraws 行。GPUBLD+FogOfWar D3D12（4 次启动 50+ 样本）/Vulkan：tilecheck 全一致；GPU 对暂停帧 Tile 除光标外的差异全在迷雾动画矩形内（infog.py）——原生 6D3470 对 VisibleCell 以 60×30 clip 重画，RLE 行深度随 clip 顶行变化，只读 Z 的动画被本体 Z 拒绝，全量重画才出现；ddraw 同样缺。DLL 3a5f5ef7…，最终 67d520c7…（只加空类型判断）D3D12 补测 14 样本一致（d1c/final）；证据 stage-d/d1c；配置已还原。

> D3 交接（2026-10-30）：按用户建议拆成两份并行 issue：gamemd-reverse #26（单位类对象：车辆/步兵/飞行器/弹体、体素管线，并负责 mode 2 公共调用表、底层提交入口清单、Z/A 写入者与 Composite 逐提交归因的观测协议）、#27（建筑动态部件 43DA80 与 AnimClass，含建筑附着动画和独立动画）。正文互相引用编号；草稿 stage-d/issue-d3-units.md、issue-d3-anims.md。等交接文档后开始 D3。

> 2026-10-30：用户授权合并 gamemd-reverse PR #24（f88955b4，#23 随之关闭）与 #25（6e39793e）；#22 随后按用户要求以 completed 关闭。vera20k 分析文档的线索已核对 vera20k fe7f306 源码后评论到 #26（issuecomment-6524185192：遍历/准入、停靠单位重画、单位阴影次序、步兵 2E00 tint、体素 Z 种子与缓存、Z 写入者分歧、Extras、Phobos hook 表）与 #27（issuecomment-6524185888：附着动画独立 Display 与 6D91D8、423061/423420/选择器、43DA80 停靠重画/DoorAnim/体素炮塔可见性与 Z 种子、片段顺序、Phobos hook 表）；均标明为待 IDB 核对的第三方线索。

> D3b-1（2026-10-31，未提交）：#27 交接 = gamemd-reverse PR #28（已 merge `e9625f2e`，#27 关闭）。实现独立与建筑槽 AnimClass（不含 DoorAnim/体素/单位）：新 AnimImage.h（帧、时停/Translucent 分级/Phobos 曲线、标志、Z 模式、深度、Tiled 步进，+AnimImageTests）；Capture::Anim（422C70→5F4B10→422CA0，Phobos 423061/423122/423183/4232BF/423365/423420/423654/423660/4236F0/423855，Ares 422FCC/自定义 palette）；Draw::Translucency/QuarterMask/TestsDepth/Rounded；ShapeDepth::Blend 与 RoundedBlend；GPU 9 条管线（不测/只测/写 × 本体/阴影/半透明），半透明按组复制目标到 CopyTexture 后在 PS 精确整数混合。S_CLSN22 半透明 746 像素 ±1 差异的根因：Phobos FixTransparencyBlitters（BlittersFix.cpp，默认开）把 RLE AlphaZRead 25/50/75% blitter 改为向上取整平均（F7DE），新增 BlittersFix::Applied；之前把强制攻击光标误当碎片。D3D12/Vulkan：GPUANIM 只差光标，GPUBLD 与 D3D12 同数（单位+光标），GPUDMG 爆炸区 0 差异、compositecheck S_CLSN22 body=0。DLL c4072d93…；证据 stage-d/d3b；配置已还原。下一步 D3b-2：DoorAnim（43DA80）与 SHP 炮塔/Anims[9] 重画。

> D3b-1 已提交推送 `19bea7c9`（memory `27cd14d9`）；gamemd-reverse PR #29（#26 单位交接）已 merge `3e3bc710`。D3b-2 DoorAnim（2026-10-31，未提交）：抽出 `Capture::Wrapper/Wrap/WrapperPart`（705E00 的配色、亮度、tint、自身影与 43CEA0 的点，Tile 建筑与 DoorAnim 共用）；`BuildingDynamic`（43CEA0→43DA80，未 IsFogged，Unload 且 DoorAnim 时 `BuildingImage::DoorAnimFrame/DoorAnimDepth`，2E00/2E01、tint 0）；6D8DB0 Techno 分支对范围内建筑调用；trace 新增 compositebuildings。原版 YR 没有 DoorAnim，临时把 GAWEAP 的 DeployingAnim 注入为 DoorAnim 验收（GPUDOOR.MAP / make_door.py）：D3D12/Vulkan 门像素除“主要”标签边框外一致，GPU 只差 UI 与单位；GPUBLD 回归同 3217。DLL 3ba5ee73…（最终 33482699… 只删未用字段）；注入已删；配置已还原。剩余：linked 对象、VXL 炮塔与 Anims[9] 显式重画随 #26。

> D3b-2 已提交推送 `56b71a73`（memory `fa089a73`）。#26 按用户选的混合方案（SHP 在 4F4583 重推、体素在提交点抓原生光栅），分 D3a-1 步兵/弹体 → D3a-2 SHP 车辆 → D3a-3 体素。D3a-1（2026-10-31，未提交）：新 UnitImage.h（6D8DB0 外扩、705E00 visual/深度/自身影、518F90 亮度与 INFSHDW、468090 桥高与深度，+UnitImageTests；BuildingImage::ShadowDepth 转用它）；Capture::Wrapper 改为任意 Techno（Index/Source/TerrainPalette/Translucency，carryall 影上移，706389 对步兵不加自定义亮度）；Capture::Infantry（Foot 分支→5F4B10→518F90，Phobos 518FC8/51933B/51946D/5194EF，DropPod/伪装/不清晰可见计 unsupported）与 Capture::Bullet（468090，Ares 468379 helper +19A50、Phobos 4683F2，体素计 bulletsVoxel）；Phobos 的 IsOnBridge/GetLevelIntensity 由 Hooks.Tint.cpp 的 static 提升为 TechnoExt 静态成员；trace 新增 compositeunits、compositedrawn，层字母 i/p。GPUINF.MAP（make_inf.py）：D3D12/Vulkan 各 5 状态 body/shadow 全 0（Vulkan 交火样本一次 body=2，经 compositedrawn 定位为另一 GI 的目标线），GPU 只差体素车辆；GPUBLD 回归 tilecheck 全一致。DLL 3137826f…；证据 stage-d/d3a；配置已还原。下一步 D3a-2：SHP 车辆（Phobos 73C7AC、Ares 73C725/73C733/705FF3、矿车/旗帜）。

> D3a-1 已提交推送 `28b683dd`。D3a-2（2026-10-31，未提交，用户改选“RenderLayers 全部改捕获”）：6D465F/6D4664 之间（只在采集渲染）由新 hook 4AED70 `CC_Draw_Shape_RecordLayers` 逐次记录 SHP 调用（surface/convert/shape/frame/point/裁剪到 surface 的 bounds/flags/remap/ZAdjust/梯度/亮度/tint/ZShape/调用者），OverlaySnapshot::CaptureComposite 把落在目标上的调用按原参数转成 Layer::Composite 的 Draw（Centered、ModeledFlags 之外、remap、ZShapeFrame≠0 计 unmodeled），trace `composite`/`compositedraws` 改为按调用者统计。删除 D2/D3b/D3a-1 的重推代码：AnimImage.h、UnitImage.h 及其测试，TerrainObjectImage 的动画帧/视野范围，BuildingImage 的 DoorAnim；Hooks.Tint.cpp/Body.h 恢复为 fa089a73。Tile 路径不变。验收：GPUINF body=0 unmodeled=0（GPU 只差体素车辆）；GPUANIM 滚屏 0；GPUDMG 爆炸区 0（body=147 为体素坦克上的 GUNFIRE）；GPUSHP（MCV 换 DLPH SHP、移动/选中/血条）D3D12/Vulkan 只剩顶部建筑处固定 32 个 others 像素（非 4AED70 来源，已记入 TerrainGPU.md）；GPUBLD Tile 回归 tilecheck 0。未重测 GPUDOOR、SHP 炮塔、矿车、旗帜。DLL 7c7ad539…；证据 stage-d/d3a（rec-*、shp*）；配置已还原。下一步 D3a-3：体素在 73B140/4AF2A0/4373B0 抓最终像素。

> D3a-2 已提交推送 `42939514`（memory `861f712e`）。D3a-3 体素（2026-10-31，未提交）：按用户选的“在最终提交点记录”，新增 4373B0/437A10 入口 hook（7 字节，跳过 4AED70 自己的调用）记录 RenderLayers 内的体素 blit（4AF2A0 未缓存、707480 缓存命中、73B140 scratch 合成），调用时按 7BBE20 裁剪并立即取像素；blitter 反查 ConvertClass::Array 的 convert 与表位，表位 flags 由询问 490B90/490E50 得到。新 BlitFrameSource（无引擎依赖 + 单元测试）：4373B0 图按内容去重，437A10 缓存项按 (地址,宽,高,清空次数)，5F99E0 hook 递增清空次数，闲置淘汰。Draw::Storage 指明帧缓存。IDB 逐表位核对后修正 tint 规则：只有测 Z 的 Alpha 本体 blitter 应用 tint（RLE 498150/499280，非 RLE 494C40），对 4AED70 调用同样生效。验收：GPUINF/GPUDMG/GPUVXL（新，直升机 JumpjetTilt、Harrier、残血坦克）/GPUBLD/GPUANIM/GPUSHP/GPUTINT（新，MTNK 红 tint）D3D12 与 GPUVXL Vulkan：compositecheck 全 mismatched=0，GPU 对 native_composite 只差光标；GPUBLD 原 3217 像素单位差异与 GPUSHP 的 32 像素都消失；tilecheck 全一致。未实测：体素碎片、体素弹体、73B140 桥下两段、5F99E0 实际触发、铁幕/空袭 tint。DLL 5bcb6df0…；证据 stage-d/d3a3；配置已还原。

> D3a-3 已提交推送 `d6488e70`（memory `60cf9d32`：tint 由 blitter 决定）。

> D3a-3 补测（2026-10-31，本地诊断构建，测后撤掉）：73B140 桥下分两段（GPUBRV，make_brv.py）mismatched=0；出厂条件（GPUFACT）10 辆都未触发分段，原因未查。5F99E0 主动清空 20 次 mismatched=0，关掉分代后出现旧图（body=185–228），证明分代必要。体素碎片（GPUVXL2，TIRE/PIECE，749D73/749EE9）mismatched=0。体素弹体未覆盖：地图新定义的体素弹体不加载 VXL，坦克发射 BlimpBombP 原生崩溃（关 GPU 渲染同样崩，7564B4 读空 VXL，与本批无关），ZEP 未放出。铁幕 tint 未覆盖（GPUVXL3 坦克未开火）。TerrainGPU.md 已更新（未提交）；干净 DLL 563e3702… 已部署；配置已还原。

> D3a-3 补测文档已提交推送（见 git log）。

> D3a-4 画线（2026-10-31，未提交）：调研结论：没有统一画线叶函数（各线各有 Bresenham，公共只有裁剪 7BC2B0；行动线/BandBox/激光/EBolt 在 6D4664 之后）；区间内实测只有 6DBB60→4BFD30（建筑选中框）与 Ares“主要”标签的 4BB620 填充、7BA610 线框。用户确定不要求逐像素一致、只求不成瓶颈：新 hook 4BFD30/7BA610/4BB620 记录（OverlaySnapshot::RecordLine，Line 记两端深度与之后的绘制序号），GPU 每条线一个实例（TerrainBatch::Line + TerrainLine.vs/ps，主轴展开平行四边形，深度线性插值，A 规则同 4C0050），shape run 在线处断开（SplitShapes/SplitLines）；trace 新增 compositelines 与 compositecheck lines。验收：GPUFACT D3D12/Vulkan 差 382 像素（文字 138、光标 187、线 57），CPU 重放 mismatched 全归 lines；GPUINF/GPUANIM/GPUVXL 回归不变。DLL 902dfc73…；证据 stage-d/lines；配置已还原；TerrainGPU.md 已更新。

> D3a-5 文字（2026-10-31，未提交）：D3a-4 已提交推送 668e2b73（memory 19eebc7b）。区间内 BitFont 字形统一入口 434120；新 hook 4348F0（BitFont::Lock）记下目标 surface，434120 在区间内经 SidebarText::CaptureGlyph 记成 Primitive::Glyph，GPU 用 TerrainGlyph.ps 采 GlyphCache 掩码（共用 atlas），线 run 按页断开。GPUFACT D3D12/Vulkan 标签区 0 差异（剩光标 187、线 57），GPUINF 0，Sidebar 不变；hook 冲突检查通过。DLL cf68a5de…；证据 stage-d/lines/glyph*；配置已还原；TerrainGPU.md 已更新。

> D3a-6 行动线（2026-10-31，未提交）：D3a-5 已提交 02b4fca6（memory 144dd820）。行动线 7049C0 不是私有光栅器：经 FillRect(4BB620)、DrawLine(7BA610)、虚线 4C0750；画在 6D4664 之后的 Techno 循环（6D4750，IsSelected+UnitActionLines+下令后 25 帧计时）。记录窗口延到 6D4912（只记画线/填充/字形，AfterLayers，不进 trace 对照），新增虚线图元（样式/相位/方向/A 条件）。GPUFACT 攻击令 D3D12/Vulkan 线区 5/7 像素差，GPUINF/GPUANIM 回归不变。DLL b4a3e842…；证据 stage-d/lines/action*；配置已还原。memory 笔记“引擎没有统一画线函数”中 7049C0 为私有光栅器的说法需更正。
