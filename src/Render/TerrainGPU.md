# 战术区地形：GPU 重画（阶段 C2 / C2.5 / C3 采集 / C4.1 ABuffer 对照）

2026-10-30。依据 gamemd-reverse Issue #16 / `443a5e7` 的 `docs/zh-CN/terrain-gpu-handoff.md` 及其完整还原（`TerrainVisibleTileScan`、`TerrainCellTileDraw`、`TerrainTileBounds`、`TerrainTileVariant`、`TerrainTileFlags`、`TerrainTilePixels`、`AlphaLightingRemap`）。对照基准是 Automation 的 `source=native_terrain`（[Automation-MCP.md](../../docs/Automation-MCP.md)）。

C2 建立采集与像素规则，C2.5 把绘制改为实例化、颜色改为 PS 查整张 LightConvert 表，输出不变。C3 采集每个地块图的 Z 字节与基准深度，并用原生 ZBuffer 逐像素核对；GPU 暂不做深度测试（见“深度”）。C4.1 依据 Issue #17 / `5ad221c` 的 `abuffer-gpu-handoff.md` 在 CPU 上重建 ABuffer 的 shroud/fog，只在 trace 中与原生 ABuffer 核对，GPU 仍用默认值（见“ABuffer”）。目前只画 TMP 地块：主菱形与 extra 图，按原生顺序、GPU 不用 Z buffer，颜色按每格 `Intensity_Terrain` 与默认 ABuffer 值。弹坑、overlay、Terrain 对象、建筑底座、地块阴影、迷雾与逐像素 ABuffer、TMP 自身的 Z 遮挡都不在本层。

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
| 颜色 | 每格只采光照行 `q=min(254,(261·max(0,I))>>11)`（`I` 为有符号 `Intensity_Terrain`）与该格 LightConvert 的整张 `FullColorData`（`ShadeCount×256`）；PS 逐像素算 shade `=min(N−1,(N−1)·q·a/32258)&0xFF`（`420140`），a 目前为默认 ABuffer 127，再取 `表[shade][索引]` |
| 层序 | 每格主菱形后接 extra 图，与 `547CF0` 相同 |
| 深度 | `547CF0`：piece flag bit1 有深度数据时，基准 `b=uint16(MaxValue+Bounds.Y−y−PixelHeight)+trunc0(PixelHeight·Level/−2)`（ZBuffer `[887644]` 的 `+24`/`+04`，y 为主菱形 Tile 坐标，PixelHeight 为实际绘制图像头 `+0C`），像素深度 `uint16(b+Z字节)`；extra 用同一个 b。无 bit1 时主图与 extra 都不测试、不写 Z |

主菱形每行有效区间由 `7EC4C8` 模板与 `7EC450` 行偏移决定。首次采集核对二者与解析式菱形（行宽 4,8,…,60,…,8,4 居中，共 900 像素）一致，否则整层不画（`reason=template`）。解包成 60×29 索引图，菱形外为 0。PS 对主菱形按解析式菱形覆盖决定是否 `discard`，菱形内索引 0 也画；extra 图索引 0 `discard`。

## 素材缓存

`TerrainSnapshot::Frames` 是独立的 FrameCache（64 MiB、8192 个 piece），以 piece 地址为身份，每次采集按 piece 头、900 像素、extra 与 Z 数据的哈希核对。有深度数据的 piece，每帧在索引平面下方接一个同尺寸的 Z 平面：主图 60×58（Z 字节按像素同样的菱形展开），extra 为 W×2H；GPU 只按上半部分（索引）绘制。内容变化（换 theater、重载后复用地址）时作废重建。每次采集对每个地块类型、piece、LightConvert 只做一次可读性检查。资源超预算时只清 piece 缓存，后续采集重建。atlas 以 `SourceKind::Mask`（只要索引，不带调色板）请求，Storage 为该缓存。

颜色表（`TerrainBatch::ColorTables`）按内容去重，整张放进一张 256×1024 的 `R32_UINT` 纹理，一行一个 shade；按首次适配分配行，不再被快照引用的表按最近最少使用淘汰，任何增删都换代。retint 原地改表时内容哈希变化，生成新表。

## 绘制与失效

GPU 合成顺序：地形 → 命令栏 → 侧栏 → tooltip → 光标。地形裁剪到 ViewBounds，加 Composite 画布原点（`Tooltip::CanvasOrigin`）。显示条件为 `Game::IsActive`、`ScenarioStarted`、Tactical 存在。

`TerrainPass.h`（只由 `GPURender.cpp` 包含）负责实例化绘制：

- 每个裁剪后的地块图是一个 32 字节实例（`TerrainBatch::Instance`：画布位置、尺寸、atlas 纹素、帧内偏移、颜色表首行与 shade 数、q 与是否主菱形），放在 storage buffer 中。
- 实例按原生顺序排列，按 atlas 页切成连续段，每段一次 `SDL_DrawGPUPrimitives`（每实例 6 个顶点）；同一次绘制内图元按提交顺序光栅化，顺序不变。正常只有一页，即一次绘制。
- `TerrainTile.vs.hlsl` 用 `View.z + SV_VertexID/6` 取实例；起始实例由顶点 uniform 传入，不依赖 base instance（D3D12 的 `SV_InstanceID` 不含起始值）。
- 在 `PresentPass::DrawSprites` 的底层回调中绘制：同一渲染通道、清屏之后、精灵之前；交换链与截图读回两次绘制都包含地形。
- 实例内容与上次上传相同时（没有新 GScreen 帧的 present）不重新上传；颜色表换代时整张重新上传（SDL 的 cycle 不保留旧内容）。

- `560BF0`（视频模式重建）与 `OnVideoModeSet` 清快照；`6BE1C0`（场景清理）与释放 GPU 资源时连同素材缓存一起清空。
- 光照、retint、桥毁、地形变化不需要单独的失效 hook：每次采集重读格子与颜色表并按内容核对。
- 没有新 GScreen 帧的 present 沿用上次快照。

## 日志

`PHOBOS_TERRAIN_TRACE=1` 时输出 `[TerrainGPU]`，最多 64 条，内容相同时不重复，且只在镜头停住（TacticalPos 与上次采集相同）时输出：`ready/reason`、ViewBounds、画布原点、TacticalPos、锚点与扫描行列数、遍历/合法/提交格数、extra 数、各类失败计数（`tile/image/variant/empty/palette/frame`）、本次 piece 数、颜色表数与占用行数、缓存规模，以及视口中心格的索引、子块、Level、变体、实际地块、piece、亮度与 shade。每条后接 `depth`（`[ABC55C]` 加速开关、ZBuffer 地址/Bounds/MaxValue/尺寸、无深度数据的格数、最高 Level）、`depthcheck`（见“深度”）、`abuffer` 与 `abuffercheck`（见“ABuffer”）。另每 600 次采集输出一条 `perf`：采集平均/最大耗时，以及期间每次 present 的地形绘制次数与实例数（最多 64 条）。

## 未支持与边界

- GPU 不做深度测试，只按绘制顺序覆盖；已测地图上原生 Z 测试从不挡色（见“深度”），对象遮挡需要的 GPU 深度留到阶段 D。
- 颜色只用默认 ABuffer 127：迷雾/未探索的渐变带、地图外轮廓暗边与 alpha shape 区域颜色不同（C4.2/阶段 E）。原雪地图开局对照中，已揭示区域内部一致，差异只在迷雾渐变带。
- 弹坑（`480350` 尾部）、overlay、Terrain 对象、建筑底座、地块阴影不画（C5/阶段 D）。
- 锚点用整数逆投影，靠多扫 2 行/列吸收与原生浮点结果的一格差异；极端高度格位于原生窗口边缘时可能多画。
- 侧栏在左、observer、chrono/Armageddon 等特殊绘制路径、读档、theater 切换、光源变化未实机验证；`6D3D10` 模式记录未接入，特殊路径下仍按当前格子状态出图。
- Debug 构建每帧采集约 1 ms（下表），尚未优化；曾出现一次 15 ms 的最大值，原因未查。present 侧的 CPU 耗时未单独测量。

## 深度

静态核对 `547CF0` 机器码：有 Z 路径为 `cmp [z], d / jb 跳过`，即旧值 ≥ d 时画色并写 d（等值覆盖）；主图对菱形内所有像素测试，extra 只对非零索引；无 Z 路径不读写 Z。`[ABC55C]` 为 1 且特殊 flags 为 0 时主图 Z 循环整段跳过、extra 改走 `7C61CF`；实测该字节为 0。

`depthcheck` 是只在 trace 中运行的对照：用 `TerrainImage::DrawDepth` 按快照顺序在 CPU 上模拟 Z（初值 `FFFF`），再经 `ZBuffer::GetBuffer(x, y−ViewBounds.Y)` 与 `AdjustPointer` 读原生环形缓冲逐像素比较，输出比较数、不一致数及范围、被 Z 测试挡住的像素数（`rejected`）、未画像素数与缺帧数。原生 ZBuffer 只在本帧整体重画或滚屏后、且没有其他东西写 Z 的区域才等于纯地形结果。

把 tile 的 y 代入 b，Level 两项抵消，`b = 常数 − 15·(x+y)`：后画的行 b 更小，原生 Z 测试几乎总放行，与按顺序覆盖结果相同。因此地形颜色对照无法区分有无 Z；GPU 深度缓冲（已定 D32_FLOAT 为主、D16_UNORM 后备，PS 写 `SV_Depth`，地形与对象共用渲染通道）在阶段 D 对象需要被悬崖遮挡时实现并验证。

## ABuffer

ABuffer（`[87E8A4]`）是 ViewBounds 大小的 16 位 WORD 环形缓冲，清空值 127；`547CF0` 用它的值 a 选 shade。mode 1 中 `6D3660` 在 `6D2DE0` 画地块之前更新它，所以地块读到的是本帧 shroud/fog 写完后的值。`ABufferImage.h` 为纯函数并有单元测试，`ABufferSnapshot.h` 采集与对照。

| 步骤 | 规则 |
| --- | --- |
| 遍历 | `6D71E0` 按整屏重画：矩形取 ViewBounds，锚点、行列与地块遍历相同（`TerrainImage::ScanCell`），只处理 `CoordinatesLegal` 的格，无窗口（`Game::hWnd` 为空）时不写 |
| 位置与裁剪 | 每格框 `(30(cx−cy)−T.X−30, 15(cx+cy)−T.Y+V.Y, 60, 30)`，不含 Level；与 ViewBounds 求交为裁剪框；绘制原点为框左上加 V.X。写入按 `GetBuffer(x, y−V.Y)` 定位，不减 V.X，因此只支持 `V.X=0`（否则 `reason=view`） |
| 取帧 | `6D8700`：第一遍看 `AltFlags+12C` bit3，第二遍看 `CellFlags+140` bit1；本格无该位时返回回退值（第一遍 `AltFlags&18h`、第二遍 `CellFlags&3` 为零得 −2，否则 −1）；否则按 8 邻格（地图坐标，邻格只读低字节）该位**未置位**组掩码查 `7F4194`。`4801F0` 把 −2 映射为 15 号帧、−1 为 0 号帧。邻格按 `TryGetCellAt` 的线性索引查，落空时读共享 `InvalidCell` 的当前标志，不写它的坐标 |
| 素材 | 第一遍用 `[89E790]`（FogOfWar 开）或 `[89E794]`（SHROUD），第二遍用 `[89E760]`；`[89E7C5]`/`[89E7C6]` 未置位或引用未加载时不采（`reason=shape`），不触发加载。按帧头取边界与 `Offset`，直接复制 Width×Height 原始字节，不做 RLE 解码 |
| 写入 | `47EFE0` 覆盖：源字节不等于 `[7E4EB8]`（须为 `FE`，否则 `reason=transparent`）就写零扩展值，0 与 `FF` 都写；`47F250` 变暗：源字节 ≤127 时写 `uint16(max(0, old+v−127))`，无上界。第二遍只在 `SpecialFlags&1000h` 且当前玩家未战败时执行。原生在通过“原点在裁剪内”的检查后若区间为空仍会进循环，模型把这种情况计为 `unsupported` |

`abuffer` 行输出：FogOfWar、玩家战败、窗口、两个素材的指针与数据地址及帧数、AlphaShape 数量、写入格数、回退帧 15/0 与边缘帧的格数、落到 InvalidCell 的查询数、缺帧数、`stale`、MapRect、VisibleRect、原生 ABuffer 的地址/Bounds/尺寸/环形偏移/字节数，以及第一个非 0 号帧格子的坐标、标志、掩码与帧号。`abuffercheck` 把快照画到填满 127 的 ViewBounds 上，经 `ABuffer::GetBuffer(V.X, 行)` 与 `AdjustPointer` 读原生环形缓冲逐像素比较，输出比较数、不一致数及范围、模型与原生的非 127 像素数，以及首个不一致像素所属格子的标志、掩码、帧号、当前取帧结果与 `Visibility+120`/`Foggedness+121`。

`4801F0` 每次执行都会把两次取帧的原始结果存进 `Visibility`/`Foggedness`。`stale` 是这两个字节与当前结果不同的格数，即原生增量缓存尚未重画的格子。模型每次按当前标志完整重建，原生只重画 VisibleCells 中的格子；一个格子自身标志不变、只是邻格被揭开时不会进入该队列，旧帧会保留到之后被重画。这类格子上的差异属于原生缓存滞后，不是模型错误。

首期不建模 alpha shape（`AlphaShapeClass::Array` 数量记录在 `abuffer` 行）与地形之后其他写 Alpha 的 blitter；`4F4583` 时的原生 ABuffer 只在这些都不存在时等于地块读取时的值。

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

### C2.5 实例化（2026-10-30）

DLL SHA256 `15a45d8a…6549`（HEAD `ad256099` + 本批未提交改动），同上受控地图与比较方法。

| 后端/地图/分辨率 | 状态 | 结果 |
| --- | --- | --- |
| D3D12 GPUTERR 1024×768 | 开局、向上、右上斜向、向左滚屏 | 除光标外最大差 1 |
| Vulkan GPUTERR 1024×768 | 开局、右上斜向、向左滚屏 | 同上 |
| D3D12 GPUFLAT 800×600 | 开局、向上 | 同上；底栏与侧栏最大差 1 |
| D3D12 原雪地图 1024×768 | 开局（未揭示） | 已揭示区内部一致，差异只在迷雾渐变带，与 C2 相同 |

性能（Debug 构建，GPUTERR 1024×768，D3D12，同一操作序列，每窗口 600 次采集）：

| | 每次 present 地形绘制次数 | 每帧实例数 | 采集平均耗时 | 采集最大耗时 |
| --- | --- | --- | --- | --- |
| C2（每图一次绘制） | 739–753 | — | 0.99–1.14 ms | 2.9–3.5 ms |
| C2.5（实例化） | 1.0 | 739–773 | 0.98–1.11 ms | 3.3–4.1 ms（另一窗口 15.3 ms） |

采集耗时不变（C2.5 不改采集）。回归新增 `TerrainBatchTests`；`test_gpu_capture.cmd` 通过；Debug 构建无警告。

### C3 深度采集（2026-10-30）

DLL SHA256 `df95642d…c6c6`（HEAD `f39de890` + 本批未提交改动），D3D12 1024×768。新增受控地图 `GPUTOMB.MAP`（由 `ancient tomb.mpr` 派生，2436 格 4 层高台），派生方式同上。

| 地图 | 状态 | depthcheck |
| --- | --- | --- |
| GPUTOMB | 开局与滚屏途中 64 次采集（含增量滚屏；当时 trace 尚未限制为停住） | 每次 630016 像素 `mismatched=0` |
| GPUTOMB | 5 个停住视角（正面悬崖、长悬崖线、39 个 extra） | `mismatched=0`、`rejected=0` |
| GPUTERR | 4 个视角（完整雪地高台、四面悬崖、坡道） | `mismatched=0`、`rejected=0` |

所有样本 `accelerated=0`、无深度数据格数 0；ZBuffer `Bounds=0,0,856,736`，`MaxValue` 随滚屏变化（32768 起）。颜色对照与 C2.5 相同：除光标外最大差 1；GPUTOMB 视野到达地图上边缘时，边缘的扇形变暗带（ABuffer，C4）约 4900 像素不同。回归新增 `TerrainImageTests` 深度用例，`test_gpu_capture.cmd` 通过；Debug 构建无警告。

### C4.1 ABuffer 对照（2026-10-30）

D3D12 1024×768，ViewBounds `0,0,856,736`，原生 ABuffer `Bounds=0,0,856,736`、`Width=856`、`Height=736`。新增受控地图 `GPUSHRD.MAP`：与 `GPUTOMB.MAP` 相同但不加“揭示全图”触发器。运行时 SHROUD/FOG 均经 SHPReference 加载、各 96 帧；FogOfWar 开时第一遍与第二遍绑定同一个 FOG 资源。所有样本 `alpha=0`、`missing=0`、`unsupported=0`。

| 地图/设置 | 状态 | abuffercheck |
| --- | --- | --- |
| GPUTOMB（揭示全图） | 开局；向上滚屏 3 次（环形偏移推进）；地图上边缘；左上角 | 每次 630016 像素 `mismatched=0`；上边缘 16 个边缘格（邻格落到 InvalidCell），模型与原生各 4721 个非 127 像素，左上角各 4812 个 |
| GPUSHRD（不揭示） | 开局黑幕；MCV 移动途中 5 次逐格揭开；展开基地；向右滚进黑幕 2 次 | `mismatched=0`；开局 669 格 15 号帧、48 个边缘格，非 127 像素 560598 |
| GPUSHRD + `FogOfWar=Yes` | 开局；MCV 两段移动（共 49 次采样） | `stale=0` 的全部采样 `mismatched=0`；MCV 移动途中有 `stale` 1–23 格时出现 745–18153 个不一致像素，首个不一致像素都属于 `stale` 格（如当前 `occlusion=-1/1`、存储 `stored=-1/3`），MCV 停下后回到 0 |

上边缘颜色对照：`native_terrain` 与 `gpu` 差 >1 的像素 4905 个，除光标 188 个外约 4717 个，即这些暗边像素（GPU 仍用 127，C4.2 处理）。这条暗带来自地图外轮廓：边缘格的上方邻格不存在（落到 InvalidCell），掩码取到暗边帧，原生行为如此，不是 Phobos 的问题。

DLL SHA256：GPUTOMB 与 GPUSHRD 两轮为 `a4b07dc4…ef31`，FogOfWar 一轮为 `d89cd0f5…3482`（只多了 `stale` 诊断，模型相同）；HEAD `de06e8dd` + 本批未提交改动。回归新增 `ABufferImageTests` 与 `TerrainImageTests` 的 `ScanCell` 用例，`test_gpu_capture.cmd` 通过；Debug 构建无警告。临时改 `spawn.ini` 的 Scenario 与 FogOfWar，测后按字节还原。证据 `Debug/AutomationTests/stage-c/c41`、`c41s`、`c41f`、`c41f2`。
