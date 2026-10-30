# 战术区地形：GPU 重画（阶段 C2 / C2.5 / C3 采集 / C4 ABuffer / C5.1 弹坑 / C5.2a 深度缓冲 / C5.2b overlay 采集 / C5.2c GPU overlay / C6 Terrain 对象 / C7 桥格与平铺帧）

2026-10-30。依据 gamemd-reverse Issue #16 / `443a5e7` 的 `docs/zh-CN/terrain-gpu-handoff.md` 及其完整还原（`TerrainVisibleTileScan`、`TerrainCellTileDraw`、`TerrainTileBounds`、`TerrainTileVariant`、`TerrainTileFlags`、`TerrainTilePixels`、`AlphaLightingRemap`）。对照基准是 Automation 的 `source=native_terrain`（[Automation-MCP.md](../../docs/Automation-MCP.md)）。

C2 建立采集与像素规则，C2.5 把绘制改为实例化、颜色改为 PS 查整张 LightConvert 表，输出不变。C3 采集每个地块图的 Z 字节与基准深度，并用原生 ZBuffer 逐像素核对；GPU 暂不做深度测试（见“深度”）。C4 依据 Issue #17 / `5ad221c` 的 `abuffer-gpu-handoff.md` 每次采集重建 ABuffer 的 shroud/fog（C4.1 在 trace 中与原生 ABuffer 核对），C4.2 在 GPU 上画成 ABuffer 纹理，地形 PS 逐像素取 a（见“ABuffer”）。C5.1 依据 Issue #18 / `af42121` 的 `smudge-overlay-gpu-handoff.md`，在每格 TMP 之后画该格的弹坑（见“弹坑”）。C5.2a 把战术区改画进与 ViewBounds 同尺寸的离屏目标，带深度缓冲，地块按 `547CF0` 测试并写 Z（见“深度”），为 #18/#19 中会被地块 Z 遮挡的 overlay、Terrain 对象与悬崖投影做准备。C5.2b 采集 overlay 本体与阴影（`OverlaySnapshot`），只在 trace 中用 CPU 重放整个 Tile 层并与原生 Tile、ZBuffer 逐像素核对；C5.2c 把 overlay 本体与阴影画进战术目标（见“Overlay”）。C6 依据 Issue #19 / `terrain-objects-shadows-gpu-handoff.md`，在 overlay 之后按原生顺序采集静态 Terrain 对象的本体与自身阴影、悬崖投影，CPU 重放与 GPU 都复用 overlay 的绘制列表（见“Terrain 对象与悬崖投影”）。目前画 TMP 地块（主菱形与 extra 图）与逐格弹坑，按原生顺序与原生 Z，颜色按每格 `Intensity_Terrain` 与逐像素 ABuffer 值（shroud/fog）。deferred 记录、建筑底座、alpha shape 都不在本层；动画或崩塌中的 Terrain 对象由 mode 2 画在 Composite，也不在本层。

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
| 颜色 | 每格只采光照行 `q=min(254,(261·max(0,I))>>11)`（`I` 为有符号 `Intensity_Terrain`）与该格 LightConvert 的整张 `FullColorData`（`ShadeCount×256`）；PS 逐像素算 shade `=min(N−1,(N−1)·q·a/32258)&0xFF`（`420140`），a 取 ABuffer 纹理中该像素的值（见“ABuffer”），再取 `表[shade][索引]` |
| 层序 | 每格主菱形后接 extra 图，与 `547CF0` 相同 |
| 深度 | `547CF0`：piece flag bit1 有深度数据时，基准 `b=uint16(MaxValue+Bounds.Y−y−PixelHeight)+trunc0(PixelHeight·Level/−2)`（ZBuffer `[887644]` 的 `+24`/`+04`，y 为主菱形 Tile 坐标，PixelHeight 为实际绘制图像头 `+0C`），像素深度 `uint16(b+Z字节)`；extra 用同一个 b。无 bit1 时主图与 extra 都不测试、不写 Z |

主菱形每行有效区间由 `7EC4C8` 模板与 `7EC450` 行偏移决定。首次采集核对二者与解析式菱形（行宽 4,8,…,60,…,8,4 居中，共 900 像素）一致，否则整层不画（`reason=template`）。解包成 60×29 索引图，菱形外为 0。PS 对主菱形按解析式菱形覆盖决定是否 `discard`，菱形内索引 0 也画；extra 图索引 0 `discard`。

## 素材缓存

`TerrainSnapshot::Frames` 是独立的 FrameCache（64 MiB、8192 个 piece），以 piece 地址为身份，每次采集按 piece 头、900 像素、extra 与 Z 数据的哈希核对。有深度数据的 piece，每帧在索引平面下方接一个同尺寸的 Z 平面：主图 60×58（Z 字节按像素同样的菱形展开），extra 为 W×2H；GPU 只按上半部分（索引）绘制。内容变化（换 theater、重载后复用地址）时作废重建。每次采集对每个地块类型、piece、LightConvert 只做一次可读性检查。资源超预算时只清 piece 缓存，后续采集重建。atlas 以 `SourceKind::Mask`（只要索引，不带调色板）请求，Storage 为该缓存。

颜色表（`TerrainBatch::ColorTables`）按内容去重，整张放进一张 256×1024 的 `R32_UINT` 纹理，一行一个 shade；按首次适配分配行，不再被快照引用的表按最近最少使用淘汰，任何增删都换代。retint 原地改表时内容哈希变化，生成新表。

## 绘制与失效

GPU 合成顺序：地形 → 命令栏 → 侧栏 → tooltip → 光标。地形裁剪到 ViewBounds，加 Composite 画布原点（`Tooltip::CanvasOrigin`）。显示条件为 `Game::IsActive`、`ScenarioStarted`、Tactical 存在。

`TerrainPass.h` / `TerrainPass.cpp` 负责实例化绘制（着色器字节、GPU 资源与实例都在 `.cpp`；各快照模块的缓存与状态同样定义在各自的 `.cpp`，头文件只有 `extern` 声明）：

- 每个裁剪后的地块图是一个 32 字节实例（`TerrainBatch::Instance`：目标位置、尺寸、atlas 纹素、帧内偏移、颜色表首行与 shade 数、q 与是否主菱形、深度基值与 Z 平面相对索引平面的行数），放在 storage buffer 中。
- 实例按原生顺序排列，按 atlas 页与深度模式（有无 Z 数据）切成连续段，每段一次 `SDL_DrawGPUPrimitives`（每实例 6 个顶点）；同一次绘制内图元按提交顺序光栅化，顺序不变。纯地块正常只有一段；弹坑不碰 Z，与地块交替时每次切换多一段。
- `TerrainTile.vs.hlsl` 用 `View.z + SV_VertexID/6` 取实例；起始实例由顶点 uniform 传入，不依赖 base instance（D3D12 的 `SV_InstanceID` 不含起始值）。
- 战术目标（`TerrainPass::Render`）：ABuffer 通道之后、获取交换链之前单独一个渲染通道，尺寸为 ViewBounds，一个目标像素对应一个 Tile 像素。颜色为 `R16G16B16A16_FLOAT`，存各通道的整数值（5/6 位），使以后的 `(old>>1)&HalfTranslucencyMask` 暗化可用混合系数 0.5 精确复现；深度为 `D32_FLOAT`，不支持时 `D16_UNORM`（日志 `Tactical depth buffer`）。每次清成黑色与深度 1。
- 实例坐标为目标坐标：Tile 坐标减 ViewBounds 左上角。
- 合成：`PresentPass::DrawSprites` 的底层回调里，用实例缓冲末尾的一个合成实例把整张战术目标贴到画布（Tile 画布原点 X + ViewBounds.X，ViewBounds.Y），`TerrainComposite.ps` 对通道值向下取整再按位数归一；交换链与截图读回两次绘制共用同一张战术目标。
- 实例内容与上次上传相同时（没有新 GScreen 帧的 present）不重新上传；颜色表换代时整张重新上传（SDL 的 cycle 不保留旧内容）。
- 地形 PS 另绑定 `ABufferPass` 的纹理：目标像素加 ViewBounds.X 即 ABuffer 坐标（两者行相同），越界时取 127。

- `560BF0`（视频模式重建）与 `OnVideoModeSet` 清快照；`6BE1C0`（场景清理）与释放 GPU 资源时连同素材缓存一起清空。
- 光照、retint、桥毁、地形变化不需要单独的失效 hook：每次采集重读格子与颜色表并按内容核对。
- 没有新 GScreen 帧的 present 沿用上次快照。

## 日志

`PHOBOS_TERRAIN_TRACE=1` 时输出 `[TerrainGPU]`，最多 64 条，内容相同时不重复，且只在镜头停住（TacticalPos 与上次采集相同）时输出：`ready/reason`、ViewBounds、画布原点、TacticalPos、锚点与扫描行列数、遍历/合法/提交格数、extra 数、各类失败计数（`tile/image/variant/empty/palette/frame`）、本次 piece 数、颜色表数与占用行数、缓存规模，以及视口中心格的索引、子块、Level、变体、实际地块、piece、亮度与 shade。每条后接 `depth`（`[ABC55C]` 加速开关、ZBuffer 地址/Bounds/MaxValue/尺寸、无深度数据的格数、最高 Level）、`depthcheck`（见“深度”）、`abuffer` 与 `abuffercheck`（见“ABuffer”）。另每 600 次采集输出一条 `perf`：采集平均/最大耗时，以及期间每次 present 的地形绘制次数与实例数（最多 64 条）。

## 未支持与边界

- GPU 深度规则：地块 `547CF0`（旧值 ≥ 新值时画并写）与 overlay 的严格 `<`（含 ZGradient、ZShape，见“Overlay”），Terrain 对象及其阴影、悬崖投影沿用同一规则（C6）。overlay 深度小于 0 时原生总能通过并写入低 16 位，GPU 近似为写 0；未见实际出现。GPU 深度没有回读核对：纯地形 Z 不挡色，正确性靠 CPU 模型的 `depthcheck` 与反向比较实验间接确认（见“C5.2a”）。
- ABuffer 只有 shroud/fog：alpha shape（阶段 E）与地形之后写 Alpha 的 blitter 不画；每帧按当前格子完整重建，单位移动时原生增量缓存尚未重画的格子会短暂不同（见“ABuffer”）。
- ABuffer 纹理为 R8_UNORM：正常写入值不超过 255；超过 255 的 WORD 只可能来自异常输入，不能表示。原雪地图开局对照中，已揭示区域内部一致，差异只在迷雾渐变带。
- `4D1890` deferred 记录不画：开 FogOfWar 实测只出现建筑记录（阶段 D1，#22）；overlay/smudge/Terrain 记录的构造入口没有调用者（#18/#19），实测也从未出现，trace 只计数（见“Terrain 对象与悬崖投影”末与“C7”）。建筑底座不画（阶段 D）。悬崖投影只有单元测试：本 mod 的温带（838 种）与雪地（964 种）tile 类型都没有 ShadowCaster，常规区间起点全为 0，测试地图上一格都不画，无法动态核对。
- 锚点用整数逆投影，靠多扫 2 行/列吸收与原生浮点结果的一格差异；极端高度格位于原生窗口边缘时可能多画。
- 侧栏在左、observer、chrono/Armageddon 等特殊绘制路径、读档、theater 切换、光源变化未实机验证；`6D3D10` 模式记录未接入，特殊路径下仍按当前格子状态出图。
- Debug 构建每帧采集约 1 ms（下表），加 ABuffer 快照后约 1.4 ms，尚未优化；曾出现一次 15 ms 的最大值，原因未查。present 侧的 CPU 耗时未单独测量。

## 深度

静态核对 `547CF0` 机器码：有 Z 路径为 `cmp [z], d / jb 跳过`，即旧值 ≥ d 时画色并写 d（等值覆盖）；主图对菱形内所有像素测试，extra 只对非零索引；无 Z 路径不读写 Z。`[ABC55C]` 为 1 且特殊 flags 为 0 时主图 Z 循环整段跳过、extra 改走 `7C61CF`；实测该字节为 0。

`depthcheck` 是只在 trace 中运行的对照：用 `TerrainImage::DrawDepth` 按快照顺序在 CPU 上模拟 Z（初值 `FFFF`），再经 `ZBuffer::GetBuffer(x, y−ViewBounds.Y)` 与 `AdjustPointer` 读原生环形缓冲逐像素比较，输出比较数、不一致数及范围、被 Z 测试挡住的像素数（`rejected`）、未画像素数与缺帧数。原生 ZBuffer 只在本帧整体重画或滚屏后、且没有其他东西写 Z 的区域才等于纯地形结果。

把 tile 的 y 代入 b，Level 两项抵消，`b = 常数 − 15·(x+y)`：后画的行 b 更小，原生 Z 测试几乎总放行，与按顺序覆盖结果相同。因此地形颜色对照无法区分有无 Z。#18、#19 确认 overlay、Terrain 对象及其阴影、悬崖投影都会被已有 Z 遮挡，GPU 深度提前到 C5.2a：PS 写 `SV_Depth = uint16(b + Z字节) / 65535`（`0xFFFF` 即清空值 1；D32_FLOAT 中各整数映射互不相同且保序，D16_UNORM 中恰好还原为原整数），有 Z 数据的图用 `LESS_OR_EQUAL` 测试并写入，无 Z 数据的图（及弹坑）用不测试、不写入的管线。Z 平面在 atlas 中位于索引平面下方一个图高处。

## ABuffer

ABuffer（`[87E8A4]`）是 ViewBounds 大小的 16 位 WORD 环形缓冲，清空值 127；`547CF0` 用它的值 a 选 shade。mode 1 中 `6D3660` 在 `6D2DE0` 画地块之前更新它，所以地块读到的是本帧 shroud/fog 写完后的值。`ABufferImage.h` 为纯函数并有单元测试，`ABufferSnapshot.h` 随每次地形采集整屏重建（与地形快照同一回调，结果在 `ABufferSnapshot::Current`），`ABufferPass.h` 在 GPU 上绘制。

| 步骤 | 规则 |
| --- | --- |
| 遍历 | `6D71E0` 按整屏重画：矩形取 ViewBounds，锚点、行列与地块遍历相同（`TerrainImage::ScanCell`），只处理 `CoordinatesLegal` 的格，无窗口（`Game::hWnd` 为空）时不写 |
| 位置与裁剪 | 每格框 `(30(cx−cy)−T.X−30, 15(cx+cy)−T.Y+V.Y, 60, 30)`，不含 Level；与 ViewBounds 求交为裁剪框；绘制原点为框左上加 V.X。写入按 `GetBuffer(x, y−V.Y)` 定位，不减 V.X，因此只支持 `V.X=0`（否则 `reason=view`） |
| 取帧 | `6D8700`：第一遍看 `AltFlags+12C` bit3，第二遍看 `CellFlags+140` bit1；本格无该位时返回回退值（第一遍 `AltFlags&18h`、第二遍 `CellFlags&3` 为零得 −2，否则 −1）；否则按 8 邻格（地图坐标，邻格只读低字节）该位**未置位**组掩码查 `7F4194`。`4801F0` 把 −2 映射为 15 号帧、−1 为 0 号帧。邻格按 `TryGetCellAt` 的线性索引查，落空时读共享 `InvalidCell` 的当前标志，不写它的坐标 |
| 素材 | 第一遍用 `[89E790]`（FogOfWar 开）或 `[89E794]`（SHROUD），第二遍用 `[89E760]`；`[89E7C5]`/`[89E7C6]` 未置位或引用未加载时不采（`reason=shape`），不触发加载。按帧头取边界与 `Offset`，直接复制 Width×Height 原始字节，不做 RLE 解码 |
| 写入 | `47EFE0` 覆盖：源字节不等于 `[7E4EB8]`（须为 `FE`，否则 `reason=transparent`）就写零扩展值，0 与 `FF` 都写；`47F250` 变暗：源字节 ≤127 时写 `uint16(max(0, old+v−127))`，无上界。第二遍只在 `SpecialFlags&1000h` 且当前玩家未战败时执行。原生在通过“原点在裁剪内”的检查后若区间为空仍会进循环，模型把这种情况计为 `unsupported` |

`abuffer` 行输出：FogOfWar、玩家战败、窗口、两个素材的指针与数据地址及帧数、AlphaShape 数量、写入格数、回退帧 15/0 与边缘帧的格数、落到 InvalidCell 的查询数、缺帧数、`stale`、MapRect、VisibleRect、原生 ABuffer 的地址/Bounds/尺寸/环形偏移/字节数，以及第一个非 0 号帧格子的坐标、标志、掩码与帧号。`abuffercheck`（含 `split`）把快照画到填满 127 的 ViewBounds 上，经 `ABuffer::GetBuffer(V.X, 行)` 与 `AdjustPointer` 读原生环形缓冲逐像素比较，输出比较数、不一致数及范围、模型与原生的非 127 像素数，以及首个不一致像素所属格子的标志、掩码、帧号、当前取帧结果与 `Visibility+120`/`Foggedness+121`。

GPU（`ABufferPass.h`、`ABufferFrame.ps.hlsl`，顶点着色器复用 `TerrainTile.vs`）：

- R8_UNORM 纹理，尺寸为 ViewBounds；每次 present 在 present 渲染通道之前单独一个渲染通道，清成 127。
- 帧放进独立的 FrameCache（`ABufferSnapshot::Frames`，以 SHP 数据地址为身份，每次采集按帧头哈希核对），经同一个 FrameAtlas 以 `Mask` 上传。
- 每个写入格一遍一个实例（帧框与裁剪框求交，坐标为 `(x, y−V.Y)`）。先一次绘制所有覆盖：不混合，源字节 `FE` 时丢弃，否则输出 `v/255`；再一次绘制所有变暗：源字节 >127 时丢弃，否则输出 `(127−v)/255`，混合为 `REVERSE_SUBTRACT`（目标减源），UNORM 在 0 处截断，即 `max(0, old+v−127)`。
- 原生是逐格先覆盖再变暗。两种顺序只在相邻格的有效像素重叠时不同；`abuffercheck` 的 `split` 统计按 GPU 顺序算出的结果与按原生顺序算出的结果有多少像素不同。
- 快照未就绪或 ViewBounds 不一致时纹理保持 127，与 C4 之前的效果相同。

`4801F0` 每次执行都会把两次取帧的原始结果存进 `Visibility`/`Foggedness`。`stale` 是这两个字节与当前结果不同的格数，即原生增量缓存尚未重画的格子。模型每次按当前标志完整重建，原生只重画 VisibleCells 中的格子；一个格子自身标志不变、只是邻格被揭开时不会进入该队列，旧帧会保留到之后被重画。这类格子上的差异属于原生缓存滞后，不是模型错误。

首期不建模 alpha shape（`AlphaShapeClass::Array` 数量记录在 `abuffer` 行）与地形之后其他写 Alpha 的 blitter；`4F4583` 时的原生 ABuffer 只在这些都不存在时等于地块读取时的值。

## 弹坑

`480350` 画完本格 TMP（主菱形与 extra）后，只要 `SmudgeTypeIndex`（`+48`）不是 −1，就经类型虚槽 `+A0`（`6B55F0`）画该格的弹坑；TMP 没画出来（空 piece、无图）时也照画。是逐格穿插，不是全屏 TMP 画完再统一画弹坑。快照在每格 TMP 实例之后追加一个弹坑实例，规则在 `TerrainImage::SmudgePosition`（有单元测试）：

| 步骤 | 规则 |
| --- | --- |
| 条件 | 与 TMP 相同的扫描与 `47FF80` 边界相交检查（`6D7560` 才会调用 `480350`）；类型索引越界计入 `smudgeMissing` |
| 素材 | `SmudgeType` 的 `Image@+A4`，**固定帧 0**；`ShapeFrameSource::Acquire` 读已加载的 SHP（含 RLE），引用未加载时 NotReady，不触发 lazy load；类型无图时与原生一样不画 |
| 位置 | 中心为本格主菱形顶角 `(tileX+30, tileY)`（含 Level）。`d = SmudgeData@+11F` 非零时是多格弹坑的子格号：`q=d/Width`、`r=d%Width`（`Width@+298`，有符号截断），中心再移 `(30(q−r), −15(q+r))` 回到足迹原点，所以足迹内每格都提交整张图；`Width=0` 时原生会除零，不画并计入 `smudgeMissing`。`4AED70` 的 `0x200` 居中：左上角为中心减画布宽高的一半（截断）加帧 `Left/Top` |
| 裁剪 | 阶段 clip（整屏为 ViewBounds），不按格子菱形裁剪 |
| 颜色 | flags `0xE00` 选 `TransXlatAlpha`：索引 0 透明，其余按本格 LightConvert、`Intensity_Terrain` 的光照行与该像素的 ABuffer 值查色，与 TMP 的 `420140` 公式相同，因此复用地形管线（实例不是主菱形，索引 0 `discard`） |
| 深度 | 该 blitter 不测试也不写 Z，`ZAdjust=−1−AdjustForZ(Level·[89E7C0])` 不生效；实例 `UsesZ=false` |

弹坑帧在 `ShapeFrameSource` 的共用缓存（与侧栏相同，SHP 析构时失效），地块 piece 仍在 `TerrainSnapshot::Frames`；`Placement::Storage` 记录实例的帧来自哪个缓存，atlas 请求与 `depthcheck` 都按它解析。trace 主行新增 `smudges`（提交的弹坑实例数）与 `smudgeMissing`。

范围外：`4D1890` 的 smudge `1Dh` deferred 分支（帧取记录 `+58`、位置取保存矩形）不画；`6D7F20` 可见格队列的增量重画用完整 ViewBounds 作 clip，整屏重建不复现这种历史。

## Overlay

`6D6D10` 在 TMP/弹坑与 deferred 记录之后扫描：锚点同地块扫描，起点再移 `(−4,−2)`，`m=H/15+20`、`n=W/60+4`，行 `r=m+1…0` 由近到远，格 `= 起点 + (⌊r/2⌋+c, ⌊(r+1)/2⌋−c)`；先画全部本体（`47F6A0`），再按同序画全部阴影（`47F510`）。格合法、`OverlayTypeIndex≠−1` 且 `47FB90` 的外接矩形与 clip 相交才画。快照多扫 2 行/列，规则在 `OverlayImage`（有单元测试），采集在 `OverlaySnapshot`（每次 `4F4583` 回调、地形采集之后）。

| 项 | 规则 |
| --- | --- |
| 中心 | `(tileX+30, tileY+15+b−lift)`：`b`（`5FDCC0`）Tiberium/墙/索引 126/箱子 −12，`LandType=9` 再 −1，索引 126 再 −1；`lift` 桥格（`Flags&0x80`）16、Data 9–17 再加 15，索引 239 为 15。阴影在桥格 Data 9–17 时再移 `(−15,+7)`。`4AED70` 居中后加帧 `Left/Top` |
| 本体 | 索引 167/178 无本体。`h=15·(Level+4·桥)−ZAdjust(Phobos)`。桥：帧 Data（0/9 加 `81CC30` 图案），`Color1_Blue`，`z=−2−h`，梯度 0。Tiberium：变体类型 `Image.ArrayIndex+p%NumImages`（`p=y·x`）；Phobos `AllowRamps`+ramp 图且坡 1–4 时取坡图 `+NumImages+(s−1)Q+p%Q`（`Q=max(NumSlopes,8)/4`）；`x.pal` Convert、亮度 1000、`z=−2−h`、梯度 0，坡格（`SlopeIndex≠0`）带 `AA105C[坡]` 的 Z shape。墙（Phobos `47F974`）：所有者配色（或类型 Palette）的 LightConvert，`Intensity_Normal`（零扩展），梯度 Deg90。其他：本格 LightConvert、`Intensity_Terrain`，`z=e−h−2`（DrawFlat 或岩石 `e=0`，否则 −15），DrawFlat 梯度 0 否则 Deg90，箱子帧 0；Rubble 不支持 |
| 阴影 | 类型自身图（Tiberium 也不换变体）的帧 `Frames/2+Data`（箱子 `Frames/2`），本格 LightConvert，flags `4601`，`z=−2−15·Level`，梯度 0 |
| 深度 | `ShapeDepth`：`7BBE20` 裁剪后，平铺帧按 `4373B0`、RLE 帧按 `437A10` 求每行基值（`817710` 梯度记录，Deg90 每 3 行 +1，梯度 0 每行 −1），像素 `int32 深度 < uint16 旧值` 才画并写低 16 位（相等不画）。阴影像素把旧色 `(c>>1)&HalfTranslucencyMask` |
| Z shape | 帧被裁到 Z shape 帧的范围（`421B60`）。若裁剪起点落在帧内，RLE 路径仍从帧第 0 列读、宽度从右侧减去该偏移（Ramp Tiberium 常见，可能一列都不画）；`437A10` 以 Z shape 帧的**原始数据**为 Z 字节（RLE 的 Z shape 读到的是编码流），指针还会因每行前缀跳过的零串落后。快照在采集时按原生指针算出每个像素实际读到的字节（`Draw::ZBytes`） |
| 空帧 | 帧号越界（`69E7E0` 返回空矩形）、宽或高为 0、引用在原生绘制后仍未加载（文件不存在）时原生不画，快照同样不画 |

GPU（C5.2c）：每个绘制是一个实例，接在地块实例之后、合成实例之前，按原生顺序分段（同 atlas 页、本体/阴影各一段）。实例的位置、大小与 atlas 起点直接取快照里已裁剪的结果；`TerrainBatch::Shape`（存储缓冲）给出行基值、累加器、分带参数、Z 字节偏移与阴影标志，`TerrainShape.ps` 逐像素求 `RowBase − int8(Z字节)`，写 `SV_Depth = 深度/65535`，深度 ≥ 65536 时丢弃。两条管线都是 `LESS` 测试并写入：本体输出与地块相同的查表颜色；阴影输出 alpha 1/2，混合为 `dst·1/2`，合成时的 floor 正好等于 `(c>>1)&mask`。阴影只在 `HalfTranslucencyMask` 等于色彩打包推出的掩码（`TerrainBatch::HalfMask`）时画，否则跳过（计入 `SkippedShapes`，trace 的 `overlay` 行以 `gpu=已画/跳过` 输出上一次 present 的数目）。Z 字节四个一组放在第二个存储缓冲中。

trace（`PHOBOS_TERRAIN_TRACE=1`）在地形 trace 输出的视角上调用 `MarkNeedsRedraw(1)`，下一次采集再输出 `overlay`（各类计数）、`overlaytypes`（每种 overlay 的本体/阴影/RLE/Z shape 数）、`tilecheck`（重放 TMP、弹坑、overlay 本体与阴影后的颜色与 Z 对照）、`tiledepth` 与不一致时的 `tilesamples`。等完整重画是因为纵向滚屏时原生只重画露出的条带，而带 Z shape 的 Flag=1 梯度绘制没有逐行步进、基值取自 clip 顶行，条带重画的结果与完整重画不同；GPU 每次完整重画，只与原生完整重画的结果相同。

## Terrain 对象与悬崖投影

overlay 之后，`6D97D0` 逆序遍历 `ObjectsInLayers[2]`，对 `WhatAmI=Terrain`、存活、`IsOnMap` 且不在 Limbo、类型非 `IsAnimated` 且未崩塌、`71D160` 外接矩形（frame 0 与 `Frames/2` 的并集）与 clip 相交的对象调用 `71C1B0`：先本体，紧接自身阴影。之后 `6D7C00` 扫描悬崖投影。规则在 `TerrainObjectImage`（有单元测试），采集接在 `OverlaySnapshot` 的同一绘制列表后（`Draw::Source` 区分 overlay/Terrain/悬崖），所以 CPU 重放、atlas 请求与 GPU 的 shape 管线都不需要另写。

| 项 | 规则 |
| --- | --- |
| 位置 | `6D2140(GetRenderCoords)` 的客户端坐标加 ViewBounds 原点为中心，`4AED70` 居中后加帧 `Left/Top`；SpawnsTiberium 类型 Y−16（阴影同样）。`h = 6D20E0(GetZ())`，快照直接调这两个原生纯函数 |
| 本体 | 帧（Phobos `71C1FE`）：`HasDamagedFrames` 且血量比不高于 `ConditionYellow_Terrain` 时为 1，否则 0（动画/崩塌帧只在 Composite 出现）。本格 LightConvert + `int16(Intensity_Terrain)`；SpawnsTiberium 用 `GRFTXT_TIBERIUM_PAL` + `Intensity_Normal`；有扩展 Palette 时（Phobos `71C2BC`，在两条分支汇合之后）取墙所有者（否则当前玩家）配色的 LightConvert + `Intensity_Normal`。flags `4E00`，`z=−h−12`，梯度 Deg90 |
| 自身阴影 | 仅 `bDrawShadow`（`822CF1`）开启时画：帧 `本体帧 + int16(Frames)/2`，始终用本格 LightConvert，flags `4E01`，`z=−h−3`，梯度 0 |
| 悬崖投影 | 锚点同 overlay，起点再移 `(−2,+3)`，`H/15+20` 行、`W/60+7` 列，由远到近（快照多扫 2 行/列）；不做逐格外接矩形测试。`4802A0`：`IsoTileTypeIndex=0xFFFF` 用 Clear 类型、子块 0，否则子块为 `uint8(Height)`；类型须 ShadowCaster。`547230`：类型号先查 `[ABC1F8]`、再查 `[AA1098]` 起的 10 个特殊类型（记录表 `ABC210`），否则查 `AA102C` 起 5 个区间各 40 个类型（共用记录表 `ABC2D0`）；记录为 16 字节（帧+1、子块、dx、dy），子块不同或帧为 0 不画。图为 `C_SHADOW.SHP`（`[ABC554]`），中心 `(tileX+30+dx+ViewBounds.X, tileY+15+dy)`，`z=58−15·Level`，梯度 0，flags `4601`，Convert 为 `LightConvertClass::Array[0]` |
| 深度与颜色 | 同 overlay（`ShapeDepth`）：`4E00`/`4E01`/`4601` 都是严格 `<` 测试并写低 16 位；阴影 `(c>>1)&HalfTranslucencyMask` |

trace 的 `overlay` 行加 `terrains`（层内 Terrain 数）、`dynamic`（动画或崩塌，归 Composite）、`terrainOutside`、`damaged`（取受损帧的静态对象）、`casters`（扫描到的 ShadowCaster 格）、`fogged`/`foggedBuildings`（`4D1890` 的记录数与其中的建筑记录数，pair 数组 `[8B3CC0]`、个数 `[8B3CC4]`，记录类型 `+30`）、`foggedInView=建筑/其他`（启用 `+74` 且保存矩形 `+40` 按 `rect−TacticalPos+ViewBounds` 平移后与 clip 相交的记录；“其他”非 0 说明 Tile 上有未画的 deferred 记录）；`overlaytypes` 的键前缀 `o`/`t`/`c` 分别为 overlay 类型、Terrain 类型、tile 类型。

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

### C4.2 GPU ABuffer（2026-10-30）

HEAD `32ee7cce` + 本批未提交改动，1024×768，同上受控地图与比较方法（同一暂停帧 `native_terrain` 对 `gpu`）。

| 后端/地图/设置 | 状态 | 颜色对照 | abuffercheck |
| --- | --- | --- | --- |
| D3D12 GPUTOMB（揭示全图） | 开局、上滚 4 次到上边缘、左上角 | 只有光标处 188 像素不同；C3 时上边缘为 4905 像素 | `mismatched=0`、`split=0` |
| D3D12 GPUSHRD + FogOfWar | 开局、MCV 移动途中、两段移动后 | 差异只在光标与“未探索之地形”提示框（GPU 画在画布上，原生 Tile 上没有） | 47 次：`split=0`；`stale=0` 的 24 次 `mismatched=0`，其余 23 次 `stale>0` |
| Vulkan GPUSHRD + FogOfWar | 开局、两段移动后 | 同上 | 49 次 `split=0` |

DLL SHA256：D3D12 与 Vulkan 两轮为 `5648c158…f90a`；之后改为每次采集只查一次各帧号的 FrameID，最终 `bc86ae75…0c3e` 在 D3D12 GPUSHRD + FogOfWar 复测开局与移动后，结果同上（17 次 `split=0`、`unsupported=0`）。采集平均耗时 1.41 ms（之前 1.77 ms，C2.5 为约 1.0 ms），每次 present 地形仍为 1 次绘制、775 个实例。临时改 `spawn.ini` 的 Scenario/FogOfWar 与 `RA2MD.INI` 的 `[Phobos] GPURender.Driver=vulkan`，测后按字节还原。证据 `Debug/AutomationTests/stage-c/c42`、`c42f`、`c42v`、`c42p`。回归通过，`test_gpu_capture.cmd` 通过；Debug 构建无警告（含 `ABufferFrame.ps` 的 DXIL/SPIR-V）。

### C5.1 弹坑（2026-10-30）

HEAD `d510023a` + 本批未提交改动，DLL SHA256 `ad1859f8…c3c6`，1024×768。新增受控地图 `GPUSMUD.MAP`：`GPUTOMB.MAP` 加 24 个弹坑（`CRATER01–12`、`BURNT01–12`，开局视野内，含多格大坑）。同一暂停帧比较 `native_terrain` 与 `gpu` 的 ViewBounds 区域。

| 后端 | 状态 | 弹坑实例 | 颜色对照 |
| --- | --- | --- | --- |
| D3D12 | 开局；向左、向右、向上、向下滚屏（弹坑在视野边缘被裁剪） | trace：开局 40、向左 32、向右 23，`smudgeMissing=0` | 每次只有光标处 188 像素不同 |
| Vulkan | 开局、向左滚屏 | 同上 | 同上 |

未单独测迷雾/黑幕下的弹坑（与 TMP 共用 ABuffer 查色）。临时改 `spawn.ini` 的 Scenario 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-c/c51`、`c51v`。回归新增 `TerrainImageTests` 弹坑位置用例；Debug 构建无警告。

### C5.2a GPU 深度缓冲（2026-10-30）

HEAD `10948817` + 本批未提交改动，1024×768，D3D12 选中 `D32_FLOAT`。同一暂停帧比较 `native_terrain` 与 `gpu` 的 ViewBounds 区域。

| 后端/地图 | 状态 | 结果 |
| --- | --- | --- |
| D3D12 GPUTOMB | 开局、两次上滚、左右滚屏；悬崖视角（`maxLevel=4`、31 个 extra）与地图上边缘 | 每次只有光标 188 像素不同；trace `depthcheck mismatched=0 rejected=0`，`noDepth=0` |
| Vulkan GPUTOMB | 开局、悬崖视角（30 个 extra） | 同上 |
| Vulkan GPUSMUD | 开局（40 个弹坑，有 Z 与无 Z 管线交替） | 同上 |
| D3D12 GPUTOMB，比较方向临时改为 `GREATER_OR_EQUAL` | 开局 | 97.85% 像素不同，证明深度测试确实生效；实验后已改回 |

最终 DLL SHA256 `6958cf5f…76a3` 复测 GPUTOMB 开局与悬崖视角，结果同上。D16_UNORM 后备路径未实测。临时改 `spawn.ini` 的 Scenario 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-c/c52`、`c52v`、`c52x`、`c52f`。回归新增 `TerrainBatchTests` 深度字段与按深度模式分段用例；Debug 构建无警告（含 `TerrainComposite.ps` 的 DXIL/SPIR-V）。

### C5.2b overlay CPU 模型（2026-10-30）

HEAD `09cf144e` + 本批未提交改动，DLL SHA256 `90a8aed3…c630`，1024×768，D3D12，只看 trace 的 `tilecheck`（每次 630016 像素）。新增受控地图（只在 `Debug/AutomationGame`）：`GPUOVX.MAP`（`GPUOVER` 加墙 `GAWALL/NAWALL/CAFNCB`、`TIB01`、箱子、岩石、`GEM01` 条带，悬崖视角坡格周围填 `TIB01`，并在地图里给 `[Riparius]` 开 `AllowRamps`）；`GPUARCO.MAP`、`GPULIBO.MAP` 为 `GPUARCF`、`GPULIBF` 去掉 Terrain 对象。

| 地图 | 状态 | 覆盖 | 结果 |
| --- | --- | --- | --- |
| GPUOVER | 开局、上、右、下 | Tiberium/宝石（RLE）、岩石本体与阴影 | 颜色、Z 全部一致 |
| GPULIBF | 开局、上、左 | 宝石 165–384 个 | 全部一致 |
| GPUARCO | 开局、上、右、下 | 宝石本体与 97 个阴影、Tiberium | 全部一致（带树的 GPUARCF 只在树处不同，属 C6） |
| GPUOVX | 开局、两次上滚、下、左（完整重画后） | 墙 147 个（Deg90）及其阴影、岩石、宝石；19 个 ramp Tiberium，其中 18 个带 Z shape | 全部一致，`rejected` 最多 47420 |

未覆盖：平铺（非 RLE）帧（测试中所有 overlay 帧都是 RLE）、桥格（这两项见“C7”）（`Flags&0x80`）、veins（本 mod 的 126 不是 veins）、Rubble、Vulkan。没有完整重画时，纵向滚屏后 ramp Tiberium 处曾有 8 像素颜色、34 像素 Z 不同，原因见“Overlay”。`89C568`（无 Z shape 时 RLE 读的 Z 字节）前 256 字节为 0，帧宽超过时计为 `unsupported`（未出现）。临时改 `spawn.ini` 的 Scenario，测后按字节还原。证据 `Debug/AutomationTests/stage-c/c52b`。回归新增 `ShapeDepthTests`、`OverlayImageTests`；Debug 构建无警告。

### C5.2c GPU overlay（2026-10-30）

HEAD `e5aee069` + 本批未提交改动，DLL SHA256 `12fd290c…627a`（复测 GPUOVX 开局与上滚，结果同下；更早的 `9eb7c915…c622` 跑了下表全部），1024×768。同一暂停帧比较 `native_terrain` 与 `gpu` 的 ViewBounds 区域（trace 打开，截图前原生已完整重画）。

| 后端/地图 | 状态 | 结果 |
| --- | --- | --- |
| D3D12 GPUOVX | 开局（墙 147、阴影 150、`rejected` 47420）；两次上滚（19 个 ramp Tiberium，18 个带 Z shape） | 每次只有光标 188 像素不同 |
| D3D12 GPUARCO | 开局、上、右（宝石与阴影） | 同上（右滚 186） |
| D3D12 GPULIBF | 开局、上（宝石 165、360 个） | 同上 |
| Vulkan GPUOVX | 开局、上滚 | 同上 |

CPU 重放同时保持 `tilecheck` 全部一致。D16_UNORM 后备路径未实测；平铺帧与桥格见“C7”。临时改 `spawn.ini` 的 Scenario 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-c/c52c`。回归新增 `TerrainBatchTests` 的 shape 实例与半透明掩码用例、`ShapeDepthTests` 的闭式行基值用例；Debug 构建无警告（含 `TerrainShape.ps` 的 DXIL/SPIR-V）。

### C6 Terrain 对象（2026-10-30）

HEAD `43d1f790` + 本批未提交改动，最终 DLL SHA256 `04516151…a7f5`（只加了 trace 的 `damaged` 计数，跑 GPUDMG；GPUTREE、GPUARCF 与 Vulkan 用 `0ed21adc…0689`；GPUFULL、GPULIBF、XMP30S6 用的是加临时悬崖探测日志之前的 `c177e8bd…2df9`，采集代码相同），1024×768。trace 在原生完整重画后核对 `tilecheck`，并在同一暂停帧比较 `native_terrain` 与 `gpu`。

| 后端/地图 | 状态 | `tilecheck` | GPU 对原生 |
| --- | --- | --- | --- |
| D3D12 GPUTREE | 开局（无静态树）、上滚（11 棵树本体与阴影，`rejected` 510） | 颜色、Z 全部一致 | 只有光标 188 像素 |
| D3D12 GPUFULL | 开局、上滚（同上，加 overlay） | 同上 | 同上 |
| D3D12 GPUARCF | 开局（3 棵）、上滚（6–7 棵与宝石 514 个） | 同上 | 同上（上滚 186–188） |
| D3D12 GPULIBF | 开局、上滚（10 个 Terrain 全为动画，归 Composite） | 同上 | 同上 |
| D3D12 XMP30S6 | 开局、上、左（多数 shroud） | 同上 | 光标与提示框 |
| Vulkan GPUTREE | 开局、上滚 | 同上 | 只有光标 188 像素 |
| D3D12 GPUDMG | 开局（9 段墙、3 棵树、玩家坦克）；坦克强制攻击后墙出现受损帧、再打掉两段（相邻墙段连接帧随之改变），树被打掉一棵；途中矿石生长 | 同上（打掉墙后的一次） | 只有光标（移动光标 284–411 像素） |

C5.2b 时 GPUARCF 只在树处不同，现已一致。`GPUDMG.MAP`（`scripts/make_dmg.py`，只在 `Debug/AutomationGame`）为 GPUTREE 加墙、树与 `<Player @ A>` 的坦克，trace 的 `damaged` 为 0：原版树不开受损帧（Phobos `HasDamagedFrames`）。悬崖投影无法动态核对（见“未支持与边界”）；`HasDamagedFrames`、SpawnsTiberium 与扩展 Palette 类型未在 Tile 上实测。临时改 `spawn.ini` 的 Scenario 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-c/c6`。回归新增 `TerrainObjectImageTests`（帧、深度、悬崖扫描、记录选择、中心）；Debug 构建无警告。

### C7 桥格、平铺帧与 deferred 记录（2026-10-30）

HEAD `811e3492` + 本批未提交改动，最终 DLL SHA256 `dbd936ec…626c`（GPUBRDG 的 D3D12 开局与上滚、XMP30S6 + FogOfWar；GPUBRDG 的左滚与 Vulkan 用只多了临时探测日志的 DLL，采集代码相同），1024×768。

受控地图 `GPUBRDG.MAP`（`scripts/make_brdg.py`，只在 `Debug/AutomationGame`）取自雪地图 `sov01umd.map`：保留地形、overlay 与 Terrain，开局点移到高桥旁（BRIDGE2，`x=65`、`y=73…95` 共 22 格，格子 `Flags&0x80`，Data 9），去掉单位、建筑、触发与规则覆盖并揭示全图。另在开局视野附近加平铺帧素材：探测各类型 SHP 的帧头，平铺帧只有宝石 GEM04 的 20–23 帧、GEM09 的 21–22 帧、GEM11 的 22 帧（都是 8–11 级的阴影帧），TROCK02/03/05、SROCK04/05 的 1 帧（阴影），路牌 SIGN03–06 的 0 帧（Terrain 本体），以及弹坑 CRATER03 的 0 帧（C5.1 的 GPUSMUD 已含）。于是加 127 格宝石（`x·y mod 12` 取 3/8/10 使画出的变体为 GEM04/09/11，生长 8–11）、两种岩石各两个、SIGN03–06 各一个。

| 后端/地图 | 状态 | 绘制 | `tilecheck` | GPU 对原生 |
| --- | --- | --- | --- | --- |
| D3D12 GPUBRDG | 开局 | 桥格 22；本体 170、阴影 163，其中平铺 99（宝石阴影 91、岩石阴影 4、路牌本体 4） | 颜色、Z 全部一致（`rejected` 87755） | 只有光标 188 像素 |
| D3D12 GPUBRDG | 上滚、左滚 | 上滚桥格 10–12、平铺 2–3；左滚桥格 3、平铺 0 | 同上 | 只有光标 187–188 像素 |
| Vulkan GPUBRDG | 开局、上滚 | 同 D3D12 | 同上 | 只有光标 188 像素 |
| D3D12 XMP30S6 + FogOfWar | 开局；MCV 两段移动后已探索区变为迷雾（共 44–59 次采样） | `fogged` 39–42，全部是建筑记录，视野内 0 | 同上 | — |

`4D1890` 的结论：#18/#19 指出 overlay（`4D0980`）、smudge（`4D0C40`）、Terrain（`4D1370`）记录的构造入口在原版里没有调用者；运行副本（原版 + Ares + Phobos + Spawner）开 FogOfWar 后也只有建筑记录，三类记录从未出现，所以不画，只在 trace 里计数。建筑记录随阶段 D1 接入。低桥（LOBRDG）、桥被打坏后的 Data 变化未实测。临时改 `spawn.ini` 的 Scenario/FogOfWar 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-c/c7`。回归通过；Debug 构建无警告。
