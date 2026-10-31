# 战术区地形：GPU 重画（阶段 C2 / C2.5 / C3 采集 / C4 ABuffer / C5.1 弹坑 / C5.2a 深度缓冲 / C5.2b overlay 采集 / C5.2c GPU overlay / C6 Terrain 对象 / C7 桥格与平铺帧 / D2a 动画 Terrain 对照 / D2b GPU 动画 Terrain / D3 对象层与体素）

2026-10-30。依据 gamemd-reverse Issue #16 / `443a5e7` 的 `docs/zh-CN/terrain-gpu-handoff.md` 及其完整还原（`TerrainVisibleTileScan`、`TerrainCellTileDraw`、`TerrainTileBounds`、`TerrainTileVariant`、`TerrainTileFlags`、`TerrainTilePixels`、`AlphaLightingRemap`）。对照基准是 Automation 的 `source=native_terrain`（[Automation-MCP.md](../../docs/Automation-MCP.md)）。

C2 建立采集与像素规则，C2.5 把绘制改为实例化、颜色改为 PS 查整张 LightConvert 表，输出不变。C3 采集每个地块图的 Z 字节与基准深度，并用原生 ZBuffer 逐像素核对；GPU 暂不做深度测试（见“深度”）。C4 依据 Issue #17 / `5ad221c` 的 `abuffer-gpu-handoff.md` 每次采集重建 ABuffer 的 shroud/fog（C4.1 在 trace 中与原生 ABuffer 核对），C4.2 在 GPU 上画成 ABuffer 纹理，地形 PS 逐像素取 a（见“ABuffer”）。C5.1 依据 Issue #18 / `af42121` 的 `smudge-overlay-gpu-handoff.md`，在每格 TMP 之后画该格的弹坑（见“弹坑”）。C5.2a 把战术区改画进与 ViewBounds 同尺寸的离屏目标，带深度缓冲，地块按 `547CF0` 测试并写 Z（见“深度”），为 #18/#19 中会被地块 Z 遮挡的 overlay、Terrain 对象与悬崖投影做准备。C5.2b 采集 overlay 本体与阴影（`OverlaySnapshot`），只在 trace 中用 CPU 重放整个 Tile 层并与原生 Tile、ZBuffer 逐像素核对；C5.2c 把 overlay 本体与阴影画进战术目标（见“Overlay”）。C6 依据 Issue #19 / `terrain-objects-shadows-gpu-handoff.md`，在 overlay 之后按原生顺序采集静态 Terrain 对象的本体与自身阴影、悬崖投影，CPU 重放与 GPU 都复用 overlay 的绘制列表（见“Terrain 对象与悬崖投影”）。目前画 TMP 地块（主菱形与 extra 图）与逐格弹坑，按原生顺序与原生 Z，颜色按每格 `Intensity_Terrain` 与逐像素 ABuffer 值（shroud/fog）。deferred 记录、建筑底座、alpha shape 都不在本层；动画或崩塌中的 Terrain 对象由 mode 2 画在 Composite，也不在本层：D2a 依据 Issue #23 / `animated-terrain-composite-gpu-handoff.md`（PR #24，未合并）在同一回调采集它们，在 trace 中重放并与 `6D4664` 的原生 Composite 逐像素核对；D2b 把它们画在战术目标的最后，使 GPU 输出对应原生 Composite 在对象层之后的样子（见“动画 Terrain（mode 2）”）。

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
- RenderLayers（D3）：经 `4AED70` 的 SHP 与经 `4373B0`/`437A10` 的体素 blit；选中框与目标线等画线、粒子与激光、RING1 仍不画，不居中或带 remap/Warp 等位的调用、blitter 不属于已知 convert 或不是 Alpha 本体/阴影的 blit 不建模（实测 GPUINF/GPUANIM/GPUDMG/GPUSHP/GPUVXL/GPUBLD 均为 0 次）。Alpha（`800`）不调制颜色。D3a-2 记下的 GPUINF/GPUSHP 顶部建筑处 32 个 `others` 像素是体素缓存命中（`707480`），D3a-3 后为 0。`others/unexplained` 的划分只是启发式：体素单位的矩形为空，闪光动画的星形可超出其 10×11 矩形几个像素。
- `4D1890` deferred 记录只画建筑（D1c）：开 FogOfWar 实测只出现建筑记录；overlay/smudge/Terrain 记录的构造入口没有调用者（#18/#19），实测也从未出现，trace 只计数（见“Terrain 对象与悬崖投影”末与“C7”）。迷雾建筑的保存动画在原生按格重画后可能缺失，快照按全量重画画出（见“迷雾建筑”）；活建筑的 tint、大门、卸载、出售未实测（见“D1a/D1b”）。悬崖投影只有单元测试：本 mod 的温带（838 种）与雪地（964 种）tile 类型都没有 ShadowCaster，常规区间起点全为 0，测试地图上一格都不画，无法动态核对。
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

## 建筑（Tile 第 8 步）

Tile 的第 8 步（`6D3870 → 6D9920`）在 Terrain 对象与悬崖投影之后、`4D1890` deferred 记录之前，正序遍历 Ground 层，对 RTTI 6、`!IsFogged || ArmageddonMode` 且 `455C20(GetRenderDimensions)` 加 ViewBounds 原点后与 clip 相交的建筑调用 `43CEA0`（跳过 `!IsOnMap || InLimbo`）→ `43D290`。每个部件经 `705E00` 先画本体、紧接自身阴影，全部走 `4AED70`，flags `6E00`/`6E01`（严格 `<` 测试并写 Z）。纯规则在 `BuildingImage`（有单元测试），采集 `OverlaySnapshot::Capture::Building` 接在同一绘制列表后（`Draw::Source` 为建筑），CPU 重放与 GPU 的 shape 管线直接复用；状态相关的量（当前帧、Foundation、闪烁亮度、配色、tint、`6D1FE0` 投影、坐标）都调原生函数取。

| 项 | 规则 |
| --- | --- |
| 位置 | 同 Terrain 对象：`GetRenderCoords` 经 `CoordsToClient` 加 ViewBounds 原点；`h = 6D20E0(GetZ())` |
| 本体 | `GetImage()`（建造/出售中为 Buildup）；帧 `min(43EF90 GetCurrentFrame, int16(Frames)/2)`；`z = NormalZAdjust−h−2`，梯度 Deg90；ZShape 为 `[89DDBC]`（BUILDNGZ），偏移 `(198,446)` 加 `ZShapePointMove`（建造/出售中不加，Phobos `43D6E5` 的 `ZShapePointMove.OnBuildup` 可保留）再减 `6D1FE0` 对 Foundation 远角 `((W−1)·256, (H−1)·256)` 的投影；Foundation 宽 ≥ 8 时无 ZShape |
| 底座 | 有 `BibShape`、`BState` 非 0 且 `ActuallyPlacedOnMap`（Phobos `43D874`）；帧为当前帧（不截断），`z=−h−3`，梯度 0 |
| 卸载（mission 16） | Phobos `43D363`：被链接单位的 `ExitThroughRoof`（默认 JumpJet 或 BalloonHover）为真时本体换 `RoofDeployingAnim`、`NormalZAdjust=−40`，否则换 `DeployingAnim`、`−20`（各自存在时）；再画 `UnderRoofDoorAnim`/`UnderDoorAnim`，帧为血量高于 `ConditionYellow` 时 0、否则 1，`z=−h−2`，梯度 0 |
| 大门（mission 24 且 UnloadTimer 处于开关状态） | 只画本体：帧 `BuildingImage::GateFrame`（进度×GateStages 截断，关闭时反向，空闲 0、全开为最后一阶，受损再加 GateStages+1），亮度只用 `Intensity_Normal` |
| 自身阴影 | 仅 `bDrawShadow` 且类型非 NoShadow：帧 `部件帧 + int16(Frames)/2`，`z=−h−4`，梯度 0，亮度 1000，无 tint，flags `6E01` |
| 颜色 | Convert：类型 `TerrainPalette` 时中心格 LightConvert（亮度改为 `int16(Intensity_Terrain)`），否则 `GetRemapColour()`；亮度 `int16(Intensity_Normal)+ExtraLight` → `GetFlashingIntensity` → 铁幕 → 空袭标记 → 加 Phobos `CustomTintIntensity`（`70632E`/`706389`） |
| tint | `GetTintColor(铁幕, 空袭, 狂暴=否) | GetCustomTintColor`，中心格 shroud 时为 0；只有测 Z 的 Alpha 本体 blitter 把它 OR 进 16 位颜色：RLE 的 AlphaZRead/AlphaZReadWrite（`498150`/`499280`）与非 RLE 的 AlphaZRead（`494C40`），其余 blitter 的 tinted 版本转调 `Blit_Copy`（D3a-3 在 IDB 逐表位核对后修正；`Draw::Tint`，GPU 在 `TerrainShape.ps` 里 OR 到查表结果）|
| 不画 | 另一种 visual（`VisualCharacter≠Normal`）、正在折跃、对当前玩家不可见、伪装中的建筑计为 `unsupported`，不画 |

Ares 的 `437CCC` 把无 ZShape 的 RLE blitter 默认读取的 Z 调整行换成与帧同宽的全 0 缓冲，所以宽阴影等无 ZShape 的 RLE 帧没有逐像素调整。trace 在 `overlay` 行后加 `buildings`：Ground 层建筑数 `buildings`、`fogged`、`outside`、`unsupported`、开关中的大门 `gates`、卸载中的 `unloading`、画出的底座 `bibs` 与门 `doors`；`overlaytypes` 的键前缀 `b` 为建筑类型号。

## 迷雾建筑（Tile 第 4 步）

Tile 第 4 步（`6D3470`）在弹坑之后、overlay 之前调 `4D1890`。快照在 overlay 之前按原生顺序采集：全局 key+record 表 `[8B3CC0]`（个数 `[8B3CC4]`）；`[8B3CCC]` 为假（本帧绘制后又生成了记录）时用原生比较函数 `4D2ED0` 对副本 qsort 并计 `foggedUnsorted`；记录须启用（`+74h`）、保存的矩形（`+40h`，减 TacticalPos 加 ViewBounds 原点）与 clip 相交，类型 6 才画（规则在 `BuildingImage::Fogged*`，有单元测试；采集 `Capture::FoggedBuilding`）。parts（`+60h`，个数 `+6Ch`，每个 10h：类型、帧、+8 标志、ZAdjust）按保存顺序：

| 项 | 规则 |
| --- | --- |
| 位置 | `CoordsToClient(Location−(128,128,0))` 加 ViewBounds 原点；`h = 6D20E0(Location.Z)`；格子为 Location 所在格（原生先画，LightConvert 已建好）|
| 颜色 | Owner（`+28h`，House 指针）当前 `ColorSchemeIndex` 的 LightConvert；类型 TerrainPalette 时用格子 LightConvert，但亮度仍为 `int16(Intensity_Normal)+int16(ExtraLight)`；没有闪烁、铁幕、tint、visual |
| part0 本体 | 类型 InvisibleInGame 时整条记录（含动画）不画。帧为保存值（不截断）；clip 高度截到 `int16(image.Height)/2 + 中心Y − clip.Y`（不大于 0 时本体与阴影都不画）。标志为假：`4E00`、`N−h−2`、梯度 2、ZShape BUILDNGZ，偏移 `(144,172)+ZShapePointMove−6D1FE0(远角)`，没有宽度 ≥8 与建造/出售的条件；标志为真（LaserFence 帧 8/12 或 FirestormWall）：`−h−2`、梯度 0、无 ZShape |
| part0 阴影 | 同一截短的 clip，帧 `本体帧 + int16(Frames)/2`（读 `GetImage()` 返回的 SHP 头），`4E01`、`−h−2`、梯度 0、亮度 1000；不看 bDrawShadow 与 NoShadow |
| 底座 | 类型有 BibShape 即画（无 BState/ActuallyPlaced 条件），原 clip，帧为保存帧，`4E00`、`−h−3`、梯度 0，没有阴影 |
| 动画 part | 类型须为 AnimType、`DetailLevel ≤ GameOptions.DetailLevel`、`GetImage()` 非空；帧为保存值；Convert：有 Owner 配色且 ShouldUseCellDrawer 时用它，否则 `[87F6C0]`（ANIM_PAL）；亮度 UseNormalLight 时 1000，否则 `int16(Intensity_Normal)`；`2E00` 只测不写 Z（`Draw::WritesDepth=false`），`保存的ZAdjust−h−2`，梯度 Flat ? 0 : 2；没有阴影 |

活建筑 `IsFogged` 时第 8 步跳过它，记录与活建筑靠这个互斥（Armageddon 时两者都画），不按位置去重。trace 的 `buildings` 行另有 `foggedAnims`（画出的动画 part）、`foggedDetail`（超出细节等级）、`foggedInvisible`、`foggedUnsorted`，`bibs` 含记录的底座；`foggeddraws` 列出每次记录绘制 `f类型:帧[s]@x,y,宽x高`（建筑 part）与 `a类型:帧@…`（动画 part），坐标为 Tile 坐标；`overlaytypes` 的键前缀 `f`/`a` 同此。

**原生增量缓存的差异**：`6D3470` 在增量重画时，除滚屏条带与脏区外，还对每个 VisibleCell 以该格的 60×30 矩形为 clip 调 `4D1890`（第 8 步的 `6D3870` 同样按格重画）。RLE 帧的行深度依赖 clip 顶行（`ShapeDepth::RLE`）：带 ZShape 的本体基值随 clip.Y 以 −1 变化，不带 ZShape 的 gradient2 动画约为 −2/3，所以按格重画时，只读 Z 的动画可能被本体刚写的 Z 拒绝，而全量重画（clip 顶行为 0）时能通过。快照按当前状态整屏重建，等于全量重画的结果；暂停帧的原生 Tile 可能缺少这些动画，见“D1c”。

## RenderLayers（mode 2，D3）

`6D3D10` 的 mode 2 在 `6D465F` 调 RenderLayers（`6D8DB0`），此时目标（`EBX`，正常为 Composite）已锁定并设为 Temp，内容是 mode 1 末尾从 Tile 复制来的 ViewBounds；`6D4664` 返回后才画 Spotlight、激光、计时器等，`4F4583` 时 Composite 已含 UI，不能作对照。`src/Render/Hooks.cpp` 在这两处各加一个只观测的 hook（整条 call 被移出，`return 0` 照常执行），`CompositeSnapshot` 按 #23 §9.2 复制目标的 ViewBounds：核对目标同时是 Temp 与 Composite、`LockLevel>0`、2 字节像素、ViewBounds 在 surface 内，按 pitch 逐行复制到自有缓冲，不再 Lock，失败记原因。`6D465F` 的输入只在本次渲染要做 trace 时复制；`6D4664` 的输出在 GPU 窗口运行或 MCP 服务运行时每次渲染都复制，是 Automation `source=native_composite` 的来源（[Automation-MCP.md](../../docs/Automation-MCP.md)）。

RenderLayers 画到目标上的 SHP 与体素一律**记录原生调用**，不在 CPU 重推（#26 起由用户确定；D2/D3b/D3a-1 的逐对象重算已删除）：Ares 3.0p1 在 SHP 车辆路径上的 FacingsA/B、WaterType、ChangeType、DrawShadowEarlier、SkipUnitShadow、OreGatherAnim 与 Phobos 的 73C7AC/73CCF4/73CFA7/73D0DB/73D223/73CF46 等改写都直接体现在最终参数里，绘制顺序就是原生顺序，DrawExtras 的 pips 等 SHP 也一并得到。

| 项 | 规则 |
| --- | --- |
| 区间 | `GPURender::OnRenderLayers`：`6D465F` 时若本次渲染要采集（GPU 窗口 Running/Hidden 且非镜像）则 `OverlaySnapshot::BeginRecording(目标)`，否则清空并不记录；`6D4664` 时 `EndRecording` |
| 记录（`4AED70` 入口，6 字节 `sub esp`） | 区间内每次调用按原样保存：ConvertClass、SHP、帧、点、clip（先与 surface 矩形求交，同 `421B60`）、flags、remap、ZAdjust、梯度、亮度、tint、ZShape 与帧/偏移、返回地址；目标不是区间的 surface（SHP 炮塔组合用的 scratch `B1D13C`）计 `elsewhere`。无 SHP、点或 clip 的调用原生不画，不记 |
| 回放（`4F4583`） | 按顺序经 `Capture::Place`：中心为 `clip 原点 + 点`，clip 用调用自己的；`Darken(1)` 为阴影，`6` 为半透明位，`2000/1000` 测 Z、`4000` 测并写，RLE 半透明且只测不写时按 Phobos 补丁取整（见下）。不建模（`unmodeled`）：无 `Centered(200)`、有 `Warp/ZRemap/Plain/40/80/MultiPass/8000` 等其他位、remap 非 0、ZShape 帧非 0 |
| 半透明与 tint | 原生 `(c>>1)&HalfTranslucencyMask`、`(c>>2)&QuatTranslucencyMask` 的整数组合（`ShapeDepth::Blend`），tinted 版本忽略 tint；tint 只由测 Z 的 Alpha 本体 blitter 应用（RLE 的 AlphaZRead/AlphaZReadWrite，非 RLE 的 AlphaZRead，见“建筑”的 tint 一行）。Phobos `FixTransparencyBlitters`（`[General]`，默认开，`BlittersFix.cpp`）把 RLE 的 AlphaZRead 25/50/75% blitter（`4984D0/4986D0/4988C0`，即 RLE 帧、只测不写 Z）改成 `(a\|b)−(((a^b)&F7DE)>>1)` 的向上取整平均：50% 平均一次，25%/75% 再与新色/旧色平均（`ShapeDepth::RoundedBlend`，`Draw::Rounded`；`BlittersFix::Applied` 记录是否已打补丁）。其余 blitter 不变 |
| 体素 blit（D3a-3） | 区间内 `4373B0`、`437A10` 的入口（各 7 字节，`sub esp` 与 `mov eax`）记录返回地址不在 `4AED70`（`4AED70..4AF295`，已由其记录覆盖）的调用：未缓存的体素与影子（`4AF2A0`，#26 §5.2）、体素缓存命中（`707480`）、车辆 scratch 合成图（`73B140` 的三处）。两者参数次序相同：目标、clip、放置矩形、源 surface、源矩形（`4373B0` 为 surface 区域，`437A10` 为整帧）、要画的部分、blitter、Z、梯度、亮度、tint（`437A10` 之前另有未用值与 Z adjust surface）。在调用时按 `7BBE20` 裁剪（`ShapeDepth::Clip`，`437A10` 为压缩方式）并立即取像素：`4373B0` 只复制画到的 8 位像素，`437A10` 解码整帧 RLE（`661590` 写的 SHP 压缩格式）。blitter 反查 `ConvertClass::Array` 里所属的 convert 与表位，表位代表的 flags 由对 `490B90`/`490E50` 逐一询问 Alpha 本体（含 `2/4/6`）与阴影 × 不测/`2000`/`4000` 得到，其余表位不建模。目标不是区间 surface（部件画进 scratch）计 `blitsElsewhere`；源不是 8 位或与目标相同、带 Z adjust surface、`437A10` 的部分与整帧不同、clip 超出 surface、取像素失败都计 `blitsUnmodeled` |
| 体素图像缓存 | `BlitFrameSource`（无引擎依赖，有单元测试）：`4373B0` 的图按内容（FNV-1a，命中后整块比较以排除碰撞）去重，`437A10` 的缓存项按 `(数据地址, 宽, 高, 清空次数)` 取用，`5F99E0`（5 字节 `mov eax`）清空全部类型的体素缓存并复位区域时清空次数加 1；每次采集计数，每 64 次淘汰 256 次采集未用的项，占用超过预算（32 MiB）四分之三时淘汰上次以来未用的项；`TerrainSnapshot::Clear` 时清空。`Draw::Storage` 指明帧所在的缓存，atlas 与 CPU 重放按它取帧 |
| 不在其中 | 选中框与目标线等画线、RING1 的 Direct3D、粒子等不经这三个入口的绘制；Alpha（`800`）不调制颜色 |

GPU：`TerrainPass` 把 `CompositeDraws` 接在 Tile 层的 shape 之后，共用 shape 实例、atlas 与 `TerrainShape.ps`。深度管线按 不测/只测/测并写 × 本体/阴影/半透明 共 9 条；半透明绘制在新的一组开始前结束渲染通道，把目标复制到 `CopyTexture`，着色器读旧值算出整数结果（`RoundedShape` 时按通道 `(a+b+1)>>1`，只在色彩打包为 565 即 `HalfMask == F7DE>>1` 时画，否则跳过），互不重叠的半透明绘制共用一次复制。战术目标因此是“Tile + RenderLayers 的 SHP”，对照基准为同一暂停帧的 `native_composite`。

trace（`PHOBOS_TERRAIN_TRACE=1`，与 `tilecheck` 同一次采集）在 `tiledepth` 之后输出：

- `composite`：层内对象数 `layerObjects`、被 `6D8DB0` 标 IsVisible 的非 Terrain/动画对象 `othersVisible`（它先清每个对象的该标志，绘制时再置位）及其中 `GetRenderDimensions` 为空的 `unbounded`（体素单位）；`calls/elsewhere/unmodeled` 与第一个不建模调用的 `返回地址:flags:remap:zshapeFrame`；`bodies/shadows/compressed/translucent/writing/tinted` 与 Place 的各失败计数；`copyFailed` 为复制失败的渲染数；`blits/blitsElsewhere/blitsUnmodeled` 与第一个不建模 blit 的 `返回地址:issue:rle:flags`（issue 1 blitter、2 源、3 clip、4 图像），`blitImages/blitBytes` 为 `BlitFrameSource` 的帧数与字节，`epoch` 为清空次数。
- `compositedraws`：每次绘制 `返回地址:帧[s][t][w|n]@x,y,宽x高`（blit 的帧为 `-1`；`s` 阴影、`t` 半透明、`w` 写 Z、`n` 不测 Z）。
- `compositecheck`：`input/output` 为两份副本的状态（`ok`、失败原因、`bounds_differ` 或 `stale`=不是本帧）；`inputMismatched` 为输入与原生 Tile 的 ViewBounds 不同的像素（应为 0）；在输入上用原生 Z、A 按顺序重放 `CompositeDraws`，与输出比较：`drawn` 为被画到的像素，`mismatched` 分为 `body/shadow`（已画像素不同）与 `untouched`，后者再按是否落在 IsVisible 对象的 `GetRenderDimensions` 矩形内分为 `others/unexplained`。`compositedrawn` 为第一个归属绘制的不同像素与其绘制，`compositesamples` 列前 24 个不同像素。

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

### D2a 动画 Terrain 的原生对照与 CPU 模型（2026-10-30）

> 这里验收的是逐对象 CPU 重算，已由 D3a-2 的 `4AED70` 调用记录取代并删除；地图与对照方法仍沿用。

HEAD `4a11f135` + 本批未提交改动，最终 DLL SHA256 `82c92fbb…de02c`，1024×768。

受控地图 `GPUANIM.MAP`（`scripts/make_anim.py`，只在 `Debug/AutomationGame`）取自 GPULIBF：保留其 10 棵矿柱（TIBTRE01/03，`x=39…43`、`y=40…44`），再加 TIBTRE02 两棵、TIBTRE03 与 TIBTRE01 各一棵；开局点移到 `(70,70)`，在矿柱下方约 55 行，上滚 1.8 秒后矿柱进入视野、MCV 移出视野。Spawner 的观察者模式（`IsSpectator`）会使 Automation 的暂停返回 `frame_control_requires_active_singleplayer_without_dialog`，不能用。原 GPULIBF 开局视野内没有动画 Terrain（10 棵中 5 棵超出视图范围、5 棵外接矩形不与 clip 相交）。

| 后端 | 状态 | 绘制 | `compositecheck` |
| --- | --- | --- | --- |
| D3D12 | 开局（矿柱在视野外，MCV 在中央） | 0 | 输入与 Tile 一致；不同 2300 像素全是 MCV（`unbounded=1`，`unexplained`） |
| D3D12 | 上滚后，及之后 8 次左右滚屏（间隔 8–15 秒） | 每次 9 棵（本体与阴影各 9，全为 RLE），`drawn` 8782–10170；动画帧出现 0、1、2、5、7、8 | 输入与 Tile 一致；`body=0 shadow=0 unexplained=0`，其余 0–22 像素都是闪光动画（`others`） |
| D3D12 + FogOfWar | 上滚、左滚 | 9 棵，帧 0/1/2 | `body=0 shadow=0`；6 像素来自一个超出自身矩形的闪光动画（`others=3 unexplained=3`） |
| Vulkan | 上滚、左右滚 3 次 | 9 棵（一次一个阴影在 clip 外），帧 0/3 | `body=0 shadow=0 unexplained=0` |

`tilecheck` 在所有样本中颜色、Z 都一致，说明对象层没有写 Z；Terrain 本体与阴影在原生 Z、A 下的重放与 `6D4664` 的原生 Composite 逐像素相同。MCP 的 `native_composite` 与同帧 `native_terrain` 只在矿柱与 MCV 处不同。临时改 `spawn.ini` 的 Scenario/FogOfWar 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-d/d2a`（D3D12 无迷雾的日志未保留，结果见上表；FogOfWar 与 Vulkan 的日志为 `debug-fog.log`、`debug-vk.log`）。回归新增 `TerrainObjectImageTests` 的动画帧与视图范围、`CaptureTests` 的 `native_composite` 元数据；Debug 构建无警告。

### D2b GPU 动画 Terrain（2026-10-30）

> 这里验收的是逐对象 CPU 重算，已由 D3a-2 的 `4AED70` 调用记录取代并删除；地图与对照方法仍沿用。

HEAD `d507b7de` + 本批未提交改动，DLL SHA256 `933132e2…8d7d`，1024×768。GPUANIM 同 D2a（开局点 `(70,70)`，上滚 1.8 秒后左右各滚一次），每个状态在同一暂停帧比较 `native_composite` 与 `gpu` 的 ViewBounds 区域（`scripts/cmpcomp.py`）。

| 后端 | 状态 | GPU 对 `native_composite` | 同次 `compositecheck` |
| --- | --- | --- | --- |
| D3D12 | 上滚、右滚 | 只有光标 187–188 像素 | `body=0 shadow=0 unexplained=0` |
| D3D12 | 左滚 | 光标 188 像素 + 一个矿石闪光动画 6 像素（GPU 处等于 Tile） | 同上 |
| Vulkan | 上滚、右滚 | 只有光标 187 像素 | 同上 |
| Vulkan | 左滚 | 光标 + 一个闪光动画 6 像素 | 同上 |

闪光动画属对象层的 AnimClass，不在 D2 范围。临时改 `spawn.ini` 的 Scenario 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-d/d2b`。Debug 构建无警告。

### D1a/D1b 活建筑（2026-10-30）

#22 交接 = gamemd-reverse PR #25。HEAD `15e41979` + 本批未提交改动，DLL SHA256 `5531665f…`，1024×768，D3D12 与 Vulkan。

受控地图 `GPUBLD.MAP`（`scripts/make_bld.py`，只在 `Debug/AutomationGame`）取自 `ancient tomb.mpr`：保留地形、overlay、Terrain 与中立建筑，开局点 `(117,75)`，另加中立 GAWEAP、NAREFN、GADEPT（血量 90/256，受损帧）、AMRADR、GAAIRC；视野内 26 栋建筑，6 个底座（含原有 CAOUTP），25–26 个 ZShape。再点两下 MCV 展开建造厂，在 Buildup 中途与建成后各抓一次。

对象层（单位、建筑动画、粒子）也画进 Composite，GPU 还不画，所以 GPU 与同帧 `native_terrain` 只在 `native_composite == native_terrain` 的像素上比较（`scripts/cmpbld.py`，约 60.4 万像素，其余 2.4–2.6 万像素被对象层覆盖）：

| 后端 | 状态 | `tilecheck` | GPU 对 Tile |
| --- | --- | --- | --- |
| D3D12 | 原开局 `(108,74)`（20 栋，民用建筑） | 颜色、Z 全一致 | 只有光标 188 像素 |
| D3D12 | `(117,75)` 开局、MCV 转向、Buildup 中途、建成 | 全一致 | 只有光标 0–188 像素 |
| Vulkan | 开局、Buildup、上滚 | 全一致 | 只有光标 0–188 像素 |

同次 `compositecheck` 的 `unexplained` 只在画面顶边（`y≤3`，对象层），`body=0 shadow=0`。未实测：tint（需铁幕/空袭/Phobos Tint）、大门开关、卸载与门、出售、`TerrainPalette` 建筑、`unsupported` 各状态；第一次实测时曾因按 `[89C568]` 首字节判定无 ZShape 宽阴影为不支持而差 559 像素，按 Ares `437CCC` 去掉该判定后一致。临时改 `spawn.ini` 的 Scenario 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-d/d1a`。回归新增 `BuildingImageTests`，全部通过；Debug 构建无警告。

### D1c 迷雾建筑（2026-10-30）

HEAD `5851ff0b` + 本批未提交改动，DLL SHA256 `3a5f5ef7…`；最终 DLL `67d520c7…`（只多了动画 part 类型为空时跳过）在 D3D12 上滚、静止 4 秒补测，14 个样本 `tilecheck` 全一致，GPU 差异同下表，1024×768。GPUBLD 同 D1a（开局 `(117,75)`，加 5 栋），`spawn.ini` 加 FogOfWar：开局约 3 秒后迷雾回长，记录从 0 增至 26–50 条（全是建筑），视野内最多 10 条、13 个保存的动画（雷达盘、旗、吊臂、灯等）。

| 后端 | 状态 | `tilecheck`（强制全量重画） | GPU 对暂停帧 Tile（`scripts/infog.py`） |
| --- | --- | --- | --- |
| D3D12 | 开局、上滚、左滚、静止 3/6/10 秒、滚出再滚回，共 4 次启动 50+ 个样本 | 颜色、Z 全一致（含 foggedAnims=3–13） | 光标 188 像素；其余 43–1426 像素全部落在迷雾动画 part 的矩形内，建筑 part 内 0 |
| Vulkan | 上滚、左滚 | 12 个样本全一致 | 同上（690、43 像素在动画内，建筑 part 内 0） |

差异处原生屏幕（`ddraw`）也没有这些动画：原生按格重画时动画被 Z 拒绝（见“迷雾建筑”末），全量重画时才画出；建筑本体、阴影、底座与 GPU 一致。这是原生增量缓存的历史状态，不按当前状态复现。未实测：Armageddon、LaserFence/FirestormWall 的平铺本体、TerrainPalette 记录、InvisibleInGame、超出细节等级的动画、未排序的记录表。临时改 `spawn.ini` 的 Scenario/FogOfWar 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-d/d1c`（`fog`、`fog2`–`fog4`、`vk`、`final`；`fog3` 之前的 DLL 没有 `foggeddraws`）。回归 `BuildingImageTests` 新增迷雾规则，全部通过；Debug 构建无警告。

### D3b-1 独立与建筑槽动画（2026-10-31）

> 这里验收的是逐对象 CPU 重算，已由 D3a-2 的 `4AED70` 调用记录取代并删除；地图与对照方法仍沿用。

#27 交接 = gamemd-reverse PR #28。HEAD `09967aac` + 本批未提交改动，DLL SHA256 `c4072d93…`，1024×768，D3D12 与 Vulkan，每个状态在同一暂停帧比较 `native_composite` 与 `gpu`（`scripts/cmpcomp.py`）。

| 地图 | 状态 | `compositecheck` | GPU 对 `native_composite` |
| --- | --- | --- | --- |
| GPUANIM | 开局、上滚、左滚、右滚 | 滚动后 `mismatched=0`；开局 2302 像素全在单位上（`unexplained`） | 只有光标 187–188 像素（D2b 的 6 像素闪光差异消失） |
| GPUBLD | 开局 3 秒 | Vulkan `body=143 unexplained=2302`（D3D12 时逐像素核对过，建筑槽动画——机场、油井、雷达、火焰等——一致，差异在单位处） | 差异是单位与光标，Vulkan 与 D3D12 同为 3217 像素 |
| GPUDMG | MCV 强制攻击地面，爆炸 S_CLSN22（`Translucent=yes`，25→50→75%）| 炮口 GUNFIRE 落在单位上（`body` 来自未画的单位旧色）；S_CLSN22 `body=0` | 爆炸区域 0 差异；整帧只有单位与移开后的光标 |

S_CLSN22 第一次对照时 746 个像素全部不同，探针证实原生在 RenderLayers 内用相同参数（surface、`ANIM_PAL`、帧、位置、`2E04`、Z、亮度）画了它，且之后到 `6D4664` 没有覆盖；差在每个通道 ±1：Phobos `FixTransparencyBlitters` 已改写 RLE AlphaZRead 半透明 blitter 的机器码（内存中 `4987F7` 起为 `mov dx,F7DE` 与 or/xor/sub 序列），按向上取整平均混合，40/40 个采样只与 `(old+in+1)>>1` 吻合。按“半透明”一行改为 `RoundedBlend` 后一致。此前把强制攻击光标的绿色箭头误当作 S_CLSN22 的碎片：`gpu` 带光标、`native_composite` 不带，光标移开后该处一致。

临时改 `spawn.ini` 的 Scenario 与 `RA2MD.INI` 的驱动，测后按字节还原。证据 `Debug/AutomationTests/stage-d/d3b`（`anim`、`bld`、`dmg9`、`dmg10`、`vk-*`）。回归新增 `AnimImageTests` 与 `ShapeDepthTests` 的补丁混合，全部通过；Debug 构建无新警告。未实测：DoorAnim（D3b-2）、建筑槽动画的 tint、Tiled/HasExtras/写 Z 的动画、非 565 色彩打包。

### D3b-2 DoorAnim（2026-10-31）

> 这里验收的是逐对象 CPU 重算，已由 D3a-2 的 `4AED70` 调用记录取代并删除；地图与对照方法仍沿用。

HEAD `27cd14d9` + 本批未提交改动，DLL SHA256 `3ba5ee73…`（最终 `33482699…` 只删了一个未用字段），1024×768。原版 YR 的建筑类型都没有设 `DoorAnim`（运行时列出只有 GAWEAP/NAWEAP/YAWEAP 有 `UnderDoorAnim` 与 `DeployingAnim`，DoorStages=0），所以验收用临时注入：第一次卸载时把 GAWEAP 的 `DeployingAnim` 设为 `DoorAnim`、`DoorStages=int16(帧数)/2`（=2），原生 `43DA80` 与采集读同一字段。受控地图 `GPUDOOR.MAP`（`scripts/make_door.py`，GPUANIM 加规则：MCV 展开成不耗电的 GAWEAP，灰熊坦克 Cost=10）：展开、造一辆后再造一辆，卸载期间左右轻点滚屏触发 trace。

| 后端 | `compositebuildings` | `compositecheck` | GPU 对 `native_composite`（门附近） |
| --- | --- | --- | --- |
| D3D12 | `unloading=1 doors=1`，门本体约 390 像素通过、约 1.2 万被建筑本体的 Z 拒绝 | 归属门本体/影子的不同像素全部在建筑的“主要”标签边框行（y=329–331，DrawExtras 后画） | 只有选择框、血条、标签与单位 |
| Vulkan | 同上 | 同上（182 个全在标签行） | 同上 |

不加注入时 GPUBLD（D3D12）`tilecheck` 全一致，GPU 差异与 D3b-1 同为 3217 像素（重构的 `Wrap`/`WrapperPart` 不改 Tile 建筑）。临时改 `spawn.ini` 与 `RA2MD.INI`，测后按字节还原；注入与像素日志已删除。证据 `Debug/AutomationTests/stage-d/d3b`（`door*`、`vk-door`、`bld3`）。回归 `BuildingImageTests` 新增 DoorAnim 帧与深度，全部通过；Debug 构建无新警告。未实测：DamagedDoor、关闭阶段的反向帧（注入时 DoorStages 只有 2）、TerrainPalette 建筑的 DoorAnim。

### D3a-1 步兵与 SHP 弹体（2026-10-31）

> 这里验收的是逐对象 CPU 重算，已由 D3a-2 的 `4AED70` 调用记录取代并删除；地图与对照方法仍沿用。

#26 交接 = gamemd-reverse PR #29。HEAD `fa089a73` + 本批未提交改动，1024×768。受控地图 `GPUINF.MAP`（`scripts/make_inf.py`，GPUBLD 开局 (117,75) 附近加 Neutral 的 CIV1/CIV2/CIV3/E1/DOG/ENGINEER 与玩家的火箭飞行兵和两个 GI；地图规则让 E1 的武器发射 `Arcing=yes`、`Shadow=yes`、图像借 GI.SHP 的弹体）：Neutral 与玩家的 GI 开局即交火，火箭飞行兵起飞（覆盖 jumpjet 亮度、离地深度与 INFSHDW）。鼠标移出战术区，每个状态在同一暂停帧比较 `native_composite` 与 `gpu`（`scripts/cmpcomp.py`）。

| 后端 | `compositeunits` | `compositecheck` | GPU 对 `native_composite` |
| --- | --- | --- | --- |
| D3D12（加 `compositedrawn` 前的构建，5 个状态） | `infantry=9→8`、`infshdw=1`、交火时 `bullets=1`（影子与本体） | 每个样本 `body=0 shadow=0`；`others+unexplained` 全在体素车辆处 | 每个状态都是 2334 像素，全在体素车辆矩形内，与 `others+unexplained` 相等 |
| Vulkan（5 个状态） | 同上 | 同上，交火样本一次 `body=2` | 2141–2334 像素，全在体素车辆矩形内（车辆动画使数字变化） |

偶发的 `body=2` 由新的 `compositedrawn` 定位（DLL `3137826f…` 复跑 3 次，抓到一次）：GI 本体的像素原生是纯绿 `0460`，是另一个 GI 在其后 DrawBehind 画的目标线压在前一个 GI 身上，属 UI。回归：GPUBLD（D3D12）`tilecheck` 全一致；`compositecheck` 的 `body=143` 属于与体素车辆重叠的建筑动画 n314，与 Wrapper 重构无关。临时改 `spawn.ini` 与 `RA2MD.INI`，测后按字节还原。证据 `Debug/AutomationTests/stage-d/d3a`（`inf-d3d12`、`inf-vk`、`inf-diag*`、`bld-reg`）。回归新增 `UnitImageTests`，全部通过；Debug 构建无新警告。未实测：GPUDOOR 复跑（Wrapper 对建筑只换了字段来源）、步兵的半透明 visual/warping/tint、桥上与 OnBridge 的步兵、DropPod、Ares/AnimPalette/FirersPalette 弹体配色。

### D3a-2 RenderLayers 改为记录 4AED70 调用，SHP 车辆（2026-10-31）

HEAD `28b683dd` + 本批未提交改动，DLL SHA256 `7c7ad539…`，1024×768。读完 `73CEC0/73C5F0` 后发现 Ares 3.0p1 与 Phobos 在 SHP 车辆路径上的改写很多，经用户确认 RenderLayers 期间画到 Composite 的 SHP 全部改为记录 `4AED70` 的最终调用（见“RenderLayers”），删除动画 Terrain、动画、DoorAnim、步兵、弹体的重算（`AnimImage.h`、`UnitImage.h` 及其测试，`TerrainObjectImage` 的动态帧与视图范围、`BuildingImage` 的 DoorAnim 规则，`TechnoExt` 提升的两个 helper 还原为 `Hooks.Tint.cpp` 的 static）。SHP 车辆用 `GPUSHP.MAP`（`scripts/make_shp.py`，GPUINF 加 `[AMCV] Image=DLPH`：开局 MCV 换成海豚的 SHP 美术），选中后移动。每个状态在同一暂停帧比较 `native_composite` 与 `gpu`（`scripts/cmpcomp.py`，鼠标移出战术区）。

| 地图（后端） | 状态 | `composite`/`compositecheck` | GPU 对 `native_composite` |
| --- | --- | --- | --- |
| GPUINF（D3D12） | 开局与交火、左右滚 | 每帧 65–96 次调用，`unmodeled=0`；`body=0 shadow=0`，`others+unexplained`=2334 全在体素车辆 | 2325–2334 像素，全在体素车辆矩形内 |
| GPUANIM（D3D12） | 开局、上滚、左滚 | 滚动后 13–19 次调用，`mismatched=0`；开局 2301 像素为体素 MCV | 滚动后 0；开局只差 MCV |
| GPUDMG（D3D12） | MCV 对地强制攻击，S_CLSN22 半透明爆炸 | `translucent=1`，S_CLSN22 无差异；`body=147` 归属炮口 GUNFIRE（`42382C`），落在未画的体素坦克上（同 D3b） | 爆炸区域 0；整帧只有体素单位与移开后的光标 |
| GPUSHP（D3D12） | 开局、选中并移动（血条）、取消选择、右滚 | `unmodeled=0`，`unexplained=0`，只剩 32 个 `others` | 每个状态 32 像素（顶部建筑处，见“未支持与边界”）；SHP 车辆及其影子、血条 pips 一致 |
| GPUSHP（Vulkan） | 开局、选中并移动、取消选择 | 同上 | 同上 |

临时改 `spawn.ini` 与 `RA2MD.INI`，测后按字节还原。证据 `Debug/AutomationTests/stage-d/d3a`（`rec-inf`、`rec-anim`、`rec-dmg`、`shp1`、`shp2`、`vk-shp`）。回归脚本去掉 `AnimImageTests`、`UnitImageTests`，其余全部通过；Debug 构建无新警告。未实测：带 VXL 炮管的 SHP 车辆（走 scratch 组合，归 D3a-3）、SHP 炮塔、采矿动画与旗帜、GPUDOOR 的 DoorAnim。

### D3a-3 体素：记录 4373B0/437A10 的 blit（2026-10-31）

HEAD `861f712e` + 本批未提交改动，DLL SHA256 `a98a601b…`（tint 规则修正后 `5bcb6df0…`，下表最后两行与 GPUINF/GPUBLD 复跑），1024×768。体素按用户选的方式在最终提交点记录（见“RenderLayers”的“体素 blit”“体素图像缓存”）；原生缓存命中本来就不重新光栅化，我们这边命中的缓存项与内容相同的 scratch 图不再上传。新地图 `GPUVXL.MAP`（`scripts/make_vxl.py`，GPUDMG 加玩家的攻城直升机、两架 Harrier、一辆残血坦克，`[AudioVisual] JumpjetTilt=yes`）。每个状态在同一暂停帧比较 `native_composite` 与 `gpu`（`scripts/cmpcomp.py`）。

| 地图（后端） | 状态 | `composite`/`compositecheck` | GPU 对 `native_composite` |
| --- | --- | --- | --- |
| GPUINF（D3D12） | 开局与交火、左右滚 | 每帧 3 次 blit（`707480` 本体与影子、`73B140` scratch），`blitsUnmodeled=0`，`mismatched=0`（原 2334 个 `others` 消失） | 0（光标在战术区外） |
| GPUDMG（D3D12） | 坦克强制攻击、爆炸 | `mismatched=0`，`blitImages` 4→23 | 只有光标（188–411 像素，随鼠标位置） |
| GPUVXL（D3D12） | 直升机与 Harrier 起飞移动、打掉残血坦克 | 每帧 8–12 次 blit，出现过 `4AF2A0` 直接提交，`blitImages` 增到 136（倾斜时键失效），`mismatched=0` | 只有光标 |
| GPUVXL（Vulkan） | 直升机与 Harrier 起飞移动 | `mismatched=0` | 只有光标 |
| GPUBLD（D3D12） | 开局、上滚、左滚 | `tilecheck` 颜色与 Z 全一致，`compositecheck mismatched=0` | 只有光标（原约 3217 像素的单位差异消失） |
| GPUANIM（D3D12） | 开局、上滚 | `mismatched=0` | 只有光标 |
| GPUSHP（D3D12） | 开局 | `mismatched=0`（D3a-2 的 32 像素消失） | 只有光标 |
| GPUTINT（D3D12） | GPUVXL 加 `[MTNK] Tint.Color=255,0,0`（`scripts/make_tint.py`），坦克移动 | 每帧 `tinted=2`，都是 `73B140` 的非 RLE scratch 合成图，`mismatched=0`（按旧规则会丢掉这两次的 tint） | 只有光标 |
| GPUINF、GPUBLD（D3D12，复跑） | 开局、滚动 | `mismatched=0`，`tilecheck` 全一致 | GPUINF 0，GPUBLD 只有光标 |

临时改 `spawn.ini` 与 `RA2MD.INI`，测后按字节还原。证据 `Debug/AutomationTests/stage-d/d3a3`。回归脚本新增 `BlitFrameSourceTests`，全部通过；Debug 构建无新警告。未实测：体素碎片（VoxelAnim `749B70`，原版 MTNK 死亡不产生）、体素弹体（`46B0C0`）、`73B140` 桥下分两段提交的两处、`5F99E0` 实际触发（本轮 `epoch=0`）、铁幕与空袭的 tint、体素的半透明 visual、RLE 缓存项带 tint 的绘制（GPUTINT 里坦克只经 scratch）。tint 规则按 IDB 逐表位核对后改为“只有测 Z 的 Alpha 本体 blitter 应用”（原为“只有 RLE 帧应用”），对 `4AED70` 的调用同样生效。
