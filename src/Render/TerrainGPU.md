# 战术区地形：GPU 重画（阶段 C2）

2026-10-30。依据 gamemd-reverse Issue #16 / `443a5e7` 的 `docs/zh-CN/terrain-gpu-handoff.md` 及其完整还原（`TerrainVisibleTileScan`、`TerrainCellTileDraw`、`TerrainTileBounds`、`TerrainTileVariant`、`TerrainTileFlags`、`TerrainTilePixels`、`AlphaLightingRemap`）。对照基准是 Automation 的 `source=native_terrain`（[Automation-MCP.md](../../docs/Automation-MCP.md)）。

本阶段只画 TMP 地块：主菱形与 extra 图，按原生顺序、不用 Z buffer，颜色按每格 `Intensity_Terrain` 与默认 ABuffer 值。弹坑、overlay、Terrain 对象、建筑底座、地块阴影、迷雾与逐像素 ABuffer、TMP 自身的 Z 遮挡都不在本层。

## 采集

不新增 hook。在已有的 `4F4583` 回调（`GPURender::OnToolTipsDrawn`，tooltip 发布之后）由 `TerrainSnapshot::CaptureAfterDraw` 采集，此时本帧地形缓存已更新。每次采集都按当前状态对整个 ViewBounds 做一次“完整重画”，不读 VisibleCells 脏格队列，也不依赖原生本帧是否重画。

| 步骤 | 规则（`TerrainImage.h` 为纯函数并有单元测试） |
| --- | --- |
| 遍历 | `6D7560`：矩形取 ViewBounds；锚点为 TacticalPos 所在格（整数逆投影，原生用浮点矩阵，可能差一格），基点 x−2；行数 `H/15+17`、列数 `W/60+4`，每边多扫 2 行/列；行按 x+y 由远到近，行内按 (+1,−1) |
| 过滤 | `CoordinatesLegal`（`568300`）与 `TryGetCellAt`；`47FF80` 边界（60×30，基础地块 piece 有 extra 时向上扩 `max(0, Y−ExtraY)`，piece 号超界取最后一个）与 ViewBounds 相交 |
| 地块类型 | `IsoTileTypeIndex==0xFFFF` 用 `[AA10B0]` 的 clear tile、子块 0；否则子块为无符号 `Height@+11A` |
| 变体 | `480350`：变体数 `+2F0` 大于 1 时，基础 TMP 中 `Height % 格数` 的 piece 有 flag bit 2 则取 `(CellFlags>>13)&1`，否则 `4814F0`；clear tile 总是 `4814F0`。`4814F0` 多格地块先退回原点格再按格宽高归一，变体数 ≤4 查 `81CCA8` 4×4 表，否则查 `89E620` 8×8 表；8×8 表未初始化（`[89E7C7]==0`）时不画该格，不触发其 RNG 初始化。`547CF0` 按 `+2BC` 链走 `variant % 数`（超界时）步 |
| 素材 | 直接读 `ObjectTypeClass::Image@+A4`，不调用 `GetImage` 的 lazy 加载；piece 为空时与原生一样整格不画 |
| 位置 | `tileX=30(cx−cy)−TacticalPos.X−30`，`tileY=15(cx+cy)−TacticalPos.Y−15·Level+ViewBounds.Y`（Tile 坐标）；extra 再加 `(ExtraX−X, ExtraY−Y)` |
| 颜色 | `q=min(254,(261·max(0,I))>>11)`，`I` 为有符号 `Intensity_Terrain`；shade `=min(N−1,(N−1)·q·127/32258)`（`420140`，默认 ABuffer 127）；复制该格 LightConvert 的 `FullColorData` 第 shade 行 256 色，按 (LightConvert, shade) 去重，再按内容进 palette 缓存（上限 160） |
| 层序 | 每格主菱形后接 extra 图，与 `547CF0` 相同 |

主菱形每行有效区间由 `7EC4C8` 模板与 `7EC450` 行偏移决定。首次采集核对二者与解析式菱形（行宽 4,8,…,60,…,8,4 居中，共 900 像素）一致，否则整层不画（`reason=template`）。解包成 60×29 索引图，菱形外为 0。着色器 `Effect.x==5`（`TileDiamond`）按菱形覆盖决定透明，菱形内索引 0 也画；`Effect.w` 传帧在 atlas 中的左上角，用于算帧内坐标。extra 图为普通索引帧，索引 0 透明。

## 素材缓存

`TerrainSnapshot::Frames` 是独立的 FrameCache（64 MiB、8192 个 piece），以 piece 地址为身份，每次采集按 piece 头、900 像素与 extra 内容的哈希核对；内容变化（换 theater、重载后复用地址）时作废重建。每次采集对每个地块类型、piece、LightConvert 只做一次可读性检查。资源超预算时清空缓存，后续采集重建。atlas 请求带该缓存作为 Storage。

## 绘制与失效

GPU 合成顺序：地形 → 命令栏 → 侧栏 → tooltip → 光标。地形裁剪到 ViewBounds，加 Composite 画布原点（`Tooltip::CanvasOrigin`）。显示条件为 `Game::IsActive`、`ScenarioStarted`、Tactical 存在。

- `560BF0`（视频模式重建）与 `OnVideoModeSet` 清快照；`6BE1C0`（场景清理）与释放 GPU 资源时连同素材缓存一起清空。
- 光照、retint、桥毁、地形变化不需要单独的失效 hook：每次采集重读格子与颜色表并按内容核对。
- 没有新 GScreen 帧的 present 沿用上次快照。

## 日志

`PHOBOS_TERRAIN_TRACE=1` 时输出 `[TerrainGPU]`，最多 64 条，内容相同时不重复：`ready/reason`、ViewBounds、画布原点、TacticalPos、锚点与扫描行列数、遍历/合法/提交格数、extra 数、各类失败计数（`tile/image/variant/empty/palette/frame`）、本次 piece 与 palette 数、缓存规模，以及视口中心格的索引、子块、Level、变体、实际地块、piece、亮度与 shade。

## 未支持与边界

- 无 Z：悬崖、高地 extra 与相邻格重叠时只按绘制顺序覆盖（C3）。
- 颜色只用默认 ABuffer 127：迷雾/未探索的渐变带与 alpha shape 区域颜色不同（C4/阶段 E）。原雪地图开局对照中，已揭示区域内部一致，差异只在迷雾渐变带。
- 弹坑（`480350` 尾部）、overlay、Terrain 对象、建筑底座、地块阴影不画（C5/阶段 D）。
- 锚点用整数逆投影，靠多扫 2 行/列吸收与原生浮点结果的一格差异；极端高度格位于原生窗口边缘时可能多画。
- 侧栏在左、observer、chrono/Armageddon 等特殊绘制路径、读档、theater 切换、光源变化未实机验证；`6D3D10` 模式记录未接入，特殊路径下仍按当前格子状态出图。
- 每帧采集耗时未测量。

## 验收（2026-10-30）

隔离 `Debug/AutomationGame` Spawner 遭遇战，independent 输出，DLL SHA256 `49dccdc8…5ddb`（HEAD `4ad5b6fc` + 本批未提交改动）。受控地图由 `Arcana troop.mpr` 派生：`GPUTERR.MAP` 保留 `IsoMapPack5`（含悬崖），`GPUFLAT.MAP` 不带（全图 clear tile）；都去掉对象和原有触发器，补全空的 `OverlayPack`/`OverlayDataPack`，加开局即触发的“揭示全图”（事件 13、动作 16）。同一暂停帧比较 `native_terrain` 与 `gpu` 的 ViewBounds 区域。

| 后端/地图/分辨率 | 状态 | 结果 |
| --- | --- | --- |
| D3D12 GPUTERR 1024×768 | 开局、向上、右上斜向、向左滚屏 | 除 GPU 光标处 188 像素外最大差 1 |
| Vulkan GPUTERR 1024×768 | 开局、右上斜向 | 同上 |
| D3D12 GPUFLAT 800×600 | 开局、向上 | ViewBounds 632×568，除光标外最大差 1 |

同批截图中底栏与侧栏区域 ddraw/gpu 最大差 1。日志所有样本 `ready=1`，各失败计数为 0。临时改 `spawn.ini` 的 Scenario 与 `RA2MD.INI` 的驱动、分辨率，测后按字节还原。证据在 `Debug/AutomationTests/stage-c/`。

测试地图注意：缺 `OverlayPack` 时 `native_terrain` 的 clear 格整片是逐格重复的覆盖物纹理（推断为每格默认带 0 号 overlay），补上全空 overlay 后一致。

回归：`test_render_capture_regression.cmd` 通过（新增 `TerrainImageTests`），`test_gpu_capture.cmd` 通过；Debug 构建无警告。未跑场景 smoke。
