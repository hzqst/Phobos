# SHP 帧按需缓存、FrameID 与多页 atlas 设计

状态：阶段1资源身份与CPU帧缓存已实现，验证及限制见 [FrameCache.md](FrameCache.md)。多页atlas、PaletteID、索引shader和GPU上传/退役已接入，D3D12两局采样、稳定300次零新增解码/上传及用户画面验收已取得，自动测试及覆盖限制见 [FrameAtlas.md](FrameAtlas.md)。设计基线为 `0514cbac`，2026-10-28。首批接入现有六项 sidebar 背景与十个按钮，之后复用到 cameo。下文为完整目标契约，不代表全部性能指标或hook已验证。

## 1. 目标与边界

在资源生命周期有效、缓存预算充足的情况下，同一 SHP 帧只解码一次，同一 GPU 驻留期只上传一次。稳定画面和已经见过的按钮状态切换，不重新解码、不上传图像；调色板变化只更新颜色资源，坐标变化只更新绘制命令。

保留原版输入、选帧、闪烁 AI、dirty 和 CPU 绘制。GPU 每次 present 仍清黑底并重画全部可见层；缓存的是原素材帧，不是已合成 surface。所有引擎资源读取和 SDL_GPU 操作保持在游戏主线程；不引入渲染线程或通用 RHI。

首版不做整份 SHP 预解码、文件名搜索替代引擎加载、磁盘缓存、运行中 atlas 搬移、格子复用或跨素材内容去重。光标和诊断镜像保留当前路径，避免把光标锁、镜像格式与本次缓存混在一起。地图、Voxel、文字、复杂 remap/光照/alpha blitter 不纳入本批。

## 2. 现状与必须改变的部分

- `SidebarSnapshot.h::CopyAsset` 每次在 `6A70AE` 读取并解码当前帧，然后用当前 Convert 把索引展开成32位“颜色+透明标记”。`CaptureButtons` 每次只保存按钮当前帧。
- `GPURender.cpp::PrepareSidebar` 对16个槽逐像素比较，变化时上传整个独立纹理；按钮往返切帧仍上传。静态背景已避免重复GPU上传，但未避免重复解码、转换和比较。
- `PresentPass::Sprite` 当前携带原始纹理指针及源区域；`DecodeRGB16.ps.hlsl` 直接读取已经映射好的颜色。
- 当前 `SidebarSnapshot::Generation` 同时在资源、布局和就绪变化时递增，不能直接作为新缓存的素材版本，否则切分辨率也会触发重复解码。
- gamemd 原版缓存原始 SHP 文件，RLE blitter 在实际绘制时边解析边写目标；不存在可直接借用的通用完整解码帧缓存。证据见 gamemd-reverse 的 `SHPLifecycle.cpp`、`SHPFrameAccess.cpp`、`ShapeDrawing.cpp`、`RLEBlitTransXlat.cpp`。

## 3. 决策总表

| 项目 | 首版决策 |
| --- | --- |
| 唯一性 | 进程内递增64位ID，0为无效，不复用，不写存档；跨进程无需稳定 |
| 帧键 | `{ResourceID, FrameIndex}`，不包含调色板、位置、对象ID或atlas坐标 |
| 图像缓存 | 自有CPU 8位索引；GPU优先R8_UINT，能力不足时R32_UINT低8位存索引 |
| 配色 | 不可变PaletteID，256项当前16位映射和颜色位格式；独立GPU纹理 |
| atlas | 标准2048×2048、多页、确定性的shelf分配；只追加，不搬移、不复用洞 |
| 淘汰 | 整页LRU；当前快照和正在构建的提交所需页面不能淘汰 |
| 同步 | atlas追加区域cycle=false；提交fence管理在途引用与整页退役 |
| 缓存失效 | 素材、palette、布局、GPU设备分别管理；卸载后的旧FrameID不再解析 |
| 缺失处理 | 只跳过未就绪元素，保留其他层；不改成CPU镜像、不用错误帧替代 |
| 绘制顺序 | 保留现有顺序，不按atlas页或palette排序透明sprite |

这些数值是首版内部常量和测试入口，不新增用户INI选项。

## 4. 身份与数据模型

```cpp
// 示意契约；实现时用不同强类型包装，禁止不同ID互相隐式转换。
using ResourceID = uint64_t;
using FrameID = uint64_t;
using PaletteID = uint64_t;
using AtlasPageID = uint64_t;

struct FrameKey { ResourceID Resource; uint32_t Index; };
struct FrameRecord {
    FrameID ID;
    FrameKey Key;
    int Left, Top, Width, Height;
    std::vector<uint8_t> Indices; // 自有、不可变；不带palette
    // 可选驻留位置由GPU缓存单独维护
};
struct AtlasLocation {
    uint64_t DeviceEpoch;
    AtlasPageID Page;
    int X, Y, Width, Height;
};
struct SpriteCommand {
    FrameID Frame;
    PaletteID Palette;
    int AnchorX, AnchorY; // 当前逻辑画布坐标，尚未加frame.Left/Top
    Rect Clip;           // 已转换到同一逻辑画布
};
```

ID不是哈希。哈希只用于map查找，碰撞通过完整键/内容比较处理。各类型使用进程级计数器，GPU设备重建或新对局不重置；溢出时拒绝继续分配并报错，绝不回绕成旧ID。

FrameID对应“某个资源版本中的某一帧”，不是“某个按钮的当前帧”。两个按钮绑定同一资源帧可以共用FrameID；同一图像用不同palette仍共用FrameID。两个不同素材即使像素相同，也无需强行合并。atlas重建/淘汰不会改变有效资源的FrameID。

`ResourceRecord` 保存整体头尺寸、帧数、已校验的帧描述表以及生命周期票据。原生整体头的高度仍用于SIDE2/SIDE3布局，不能用裁切帧Height替代；整体头与帧头尺寸分别保存。布局层只读这些元数据，不依赖像素vector。

## 5. 资源注册与生命周期：先保证身份可靠

### 5.1 分离四类变化

| 变化 | 处理 |
| --- | --- |
| 原始SHP替换、卸载、原地修改 | 原ResourceID失效，新版本分配新ResourceID及FrameID |
| Convert替换或映射内容变化 | 重新获得PaletteID，FrameID和索引缓存保持 |
| 坐标、裁剪、分辨率、SidebarMode变化 | 重建SpriteCommand，帧及palette保持 |
| GPU资源/设备重建 | 只清AtlasLocation和GPU palette驻留，从自有CPU缓存重新上传 |

就绪标志不再顺带增加素材版本。设备重建只描述缓存恢复契约；当前renderer失败后进入Failed的策略保持，本批不承诺自动恢复设备故障。普通Alt-Tab也不能无条件清空缓存。

### 5.2 注册与别名

游戏线程在已确认的资源阶段读取原始绑定。引擎地址只作为生命周期内的查找线索：`{资源域代际, 已加载Data地址, 分配/绑定版本票据}`映射到ResourceID。SHPReference地址、Loaded/Data变化和帧描述元数据用于发现替换。不同reference别名只能在证明同属同一存活Data时共享注册；不能仅按文件名、地址或CRC跨代际合并。

只在Loaded且来源合法时读取，继续不调用会触发加载/覆盖共享scratch的GetData。未加载返回NotReady，不负缓存为永久损坏；以后原生加载完成可重试。同一资源完整帧表只需在注册时校验和保存，像素仍按需读取。

热路径仍读小量绑定、状态和帧描述元数据；避免每帧扫描/哈希整个SHP。稳定Data的原始字节默认不可变。所有受支持的写入方必须通过生命周期通知使缓存失效；未知模组原地改写且不通知的情形不在首版透明兼容承诺内。调试模式可低频比较已用帧内容hash发现遗漏，但不能以此证明不存在地址复用。

### 5.3 必须覆盖的边界

- 保留已核对的 `6A5840/6A5BF0/6BE1C0`、`730100/730400` 与 `652D90` 失效边界，按资源/颜色含义拆开；现有 `6A5BDB/6530E7` 仅恢复读取就绪，不复活旧ID。
- `ShapeButton::SetShape @69DE00`、`ClearShape @69DE70`、SHP `Load/Unload`（`69E090/69E100`）和所有权释放/重新绑定是资源注册适配器的调查点。实现阶段必须确认本批所有绑定入口可被观测；必要时仅对已跟踪对象加入hook。
- SetShape重绑到同一仍存活资源允许共享，但若无法证明旧数据未被释放，则保守分配新版本。多个别名引用时，解除一个绑定不等于释放整个资源；原生卸载通知才使所有别名失效。
- 数据卸载前撤销引擎读取许可、ResourceID及其FrameID有效性，并丢弃旧快照。已经提交的GPU工作持有自有GPU资源，等待fence完成，不再触及engine指针。
- 不能拿现有hook窗口验证代替新入口验证。新增/修改hook需执行check-hooks、运行模块字节与线程/时序核验，未完成前不得宣称资源身份机制完备。

现有裸指针缺少最后一帧的完整分配长度；首次解析仍只能使用帧偏移和编码上限检查。若加载器能提供可信文件长度则记录并加强校验；本批不因引入缓存而声称已经消除此限制。

## 6. 按需解码与快照契约

`CaptureAfterDraw` 沿用原版按钮状态选择frame，依次执行：

1. 检查资源域/布局就绪，在主线程取得当前绑定的ResourceID。
2. 查找 `{ResourceID, frame}`。命中时仅取得FrameID和布局元数据，不解码、不转换、不比较整帧像素。
3. 未命中时验证帧号/尺寸/压缩数据，调用现有 `ShapeImage::Decode` 生成自有索引。plain帧也复制到自有内存，不能让timer借用engine存储。
4. 完整解码成功且未超预算后发布FrameRecord/FrameID。失败不发布半成品；帧号越界不clamp、不代换为0。
5. 取得不可变PaletteID，生成只含值和句柄的SpriteCommand。背景重复段、维修/卖出后层重复绘制都引用相同FrameID。
6. 一次替换完整快照，避免跨资源代际拼接。必需背景未就绪沿用现有整层策略；某个按钮未就绪只缺该按钮。

API建议：`FindOrDecodeFrame(SourceLease, frameIndex)` 返回 `Ready(FrameID)`、`NotReady`、`InvalidData`、`BudgetExceeded`。SourceLease仅在采集调用内有效；缓存/上传队列/PresentPass均不存引擎指针。

损坏帧在同ResourceID下记录一次错误并暂停反复解析；重载产生新ResourceID后可重试。NotReady和预算不足可以重试，但按变化/退役完成驱动，不逐present刷错误日志。首版单帧不拆分解码任务，最大尺寸2048×2048、编码上限沿用16MiB；应测量首次出现的开销，不声称完全无首次卡顿。

## 7. 调色板独立缓存与shader

每次采集对每个不同Drawer至多读取一次256×uint16映射及ColorPackData；同一采集内共用结果。小量512字节比较保留，以捕获没有重绑指针的颜色表修改；不再执行每像素颜色转换。

Palette内容键为 `{256项映射, RGB通道shift/bits}`，完整比较后复用已有PaletteID。不同Drawer内容相同可共用；A→B→A应重新找到A，不为同内容无限创建版本。引擎Drawer别名按生命周期撤销，但仍被有效快照引用的自有palette版本保持可用；失效时同时撤销不再合法的快照。

首版每个GPU palette为256×1的R32_UINT纹理，低16位保存原映射，约1KiB有效纹素；不把palette排入图像atlas，不建立可被在途绘制覆盖的可变palette槽。通道格式随PaletteID绑定，不在绘制旧快照时读取一个可能已变化的全局ColorPackData。

新增索引绘制pipeline，绑定图像atlas和palette两个整数纹理：

```text
local = clamp(nearest pixel within clipped frame, 0, clippedSize - 1)
index = atlas.Load(atlasOrigin + sourceCrop + local)
alpha = index == 0 ? 0 : 1
packedColor = palette.Load(index, 0)
RGB = decode packedColor using this PaletteID's channel shifts/bits
```

索引0即使映射为非黑也透明；非零索引映射为黑仍不透明。继续使用straight alpha、nearest及无mipmap，不对palette索引做线性过滤。索引保持在0..255，R32回退也只存该范围。

atlas使用整数Load并显式限制到本sprite源区域，所以首版无需padding/gutter；未来添加双线性或mipmap必须重新设计边界，不能直接打开sampler选项。

现有RGB16 pipeline继续服务光标/镜像，和索引pipeline按原始sprite顺序切换。PresentPass不能按FrameID、页或palette重排透明图层；首版仍允许一sprite一次draw，不将atlas等同于已经完成draw-call batching。

## 8. 多页atlas与预算

### 8.1 页面与分配器

- 启动时用 `SDL_GPUTextureSupportsFormat` 核对R8_UINT + 2D + SAMPLER；不支持时使用R32_UINT索引回退。两者均不支持则禁用新sidebar路径并记录，不静默回到CPU镜像。
- 标准页2048×2048、1层、1 mip，按首次需求创建。单帧上限2048可原样装入，不自动缩放或切成多个sprite。
- 采用确定性first-fit shelf：依页创建顺序查找已有shelf，满足高度且剩余宽度足够则分配；否则在页底部新增shelf；放不下再创建页。分配矩形不重叠、不移动、从不重用释放后的洞。
- 分配结果为AtlasLocation；FrameID不编码PageID或坐标。空帧/异常尺寸按解析契约处理，不创建0尺寸GPU资源。
- 页面中的未写区域允许未定义，但任何draw都只能引用已上传区域，不能读取整页或越界取相邻帧。

### 8.2 首版内部上限

| 资源 | 默认上限 | 说明 |
| --- | --- | --- |
| 图像atlas有效GPU纹素预算 | 64MiB、最多16页，取先到者 | R8每页4MiB；R32每页16MiB。包含待退役页面，不是显卡实际分配总量的保证 |
| CPU解码索引 | 64MiB | 不包含引擎原始文件；每帧只保留一份 |
| FrameRecord | 65536条活跃记录 | ID只递增，失效记录可删除；不是以ID为下标不断扩张的vector |
| PaletteRecord | 256个活跃/待退役版本 | CPU/GPU每份约512B/1KiB，驱动开销另计；不用的版本可回收 |
| 上传staging | 3个slot，每slot最多16MiB | 支持一张2048²的R32回退帧；按需扩容，最多48MiB有效缓冲 |
| 在途submission | 最多3个 | fence未完成时跳过新增present，不阻塞游戏线程 |

统计同时报告驱动不可见开销之外的这些已知分配，不能仅报“帧像素字节”掩盖页面空白和staging。首版数据量很小，但仍必须通过小预算测试验证上限。

### 8.3 淘汰与空间不足

从当前快照FrameID集合和正在构建的提交生成pin集合。页面满且预算不足时，选择未被pin的最久未用整页：撤销其所有AtlasLocation、移入Retiring，不再接受新绘制，等待最后使用它的提交完成后释放，预算届时才归还。不能为等待回收而临时突破预算。

有效FrameRecord仍保留CPU索引；被淘汰帧再次使用时不解码，只重新分配和上传，FrameID不变。CPU首版不做活跃资源帧LRU：只在资源失效时释放索引；达CPU上限则拒绝新帧并计数，避免timer为了重建被淘汰数据而访问引擎。世界素材阶段可另设计CPU二级缓存。

如果全部页面被当前快照占用，当前工作集超预算：只跳过未驻留元素并记录BudgetExceeded，不让背景/光标整体失败、不反复驱逐当前工作集。不在线整理碎片；统计页占用率，是否增加更复杂分配器由后续真实数据决定。

## 9. 上传、提交与GPU安全

### 9.1 最重要的cycle规则

当前独立纹理上传使用cycle=true，适用于重写整张图，不能原样用于atlas追加。SDL cycling针对整个纹理，新的内部存储内容未定义；若只上传一个新按钮，旧背景可能丢失。

atlas固定使用 `SDL_UploadToGPUTexture(..., cycle=false)`，只写从未被draw引用的新区域。已有区域不可变，因此不会覆盖在途绘制正在读取的内容。页面更换/复用必须经过退役，不能靠cycle掩盖同步问题。

### 9.2 staging与提交事务

用三个可复用staging slot，各自记录最后submission fence；仅选择空闲slot写入，不对仍在途slot做Map。这里采用显式fence复用，Map的cycle也设false，避免内部缓冲不受预算控制地增长。

每块上传数据的行pitch向上对齐256字节，起始offset对齐512字节，并按实际texel大小计算 `pixels_per_row`；使用checked arithmetic校验总长度。R8直接复制索引，R32回退只在首次上传时扩展到uint32。padding初始化为0，目标只上传有效宽高。

`PrepareSubmission` 查FrameID/PaletteID有效性，pin资源、分配未驻留区域并组成UploadBatch。本提交copy pass先上传新帧与palette，再在render pass绘制；同一提交内允许引用刚上传的区域。GPU绘制使用临时解析的纹理指针，长期快照仍只保有ID。

上传需求按当前绘制顺序去重，优先当前可见元素；本批有效及对齐后的总字节必须装入一个空闲staging slot。超过16MiB时只处理能装入的完整帧，其余保留CPUReady排队至后续present，不把帧切成跨提交半成品。已驻留sprite正常绘制，待上传sprite暂缺；过期快照的未开始请求可取消。光标仍沿旧路径更新，不被大批sidebar上传锁住。后台预取不在首版范围。

状态至少区分：CPUReady → Reserved → EncodedInBatch → Submitted。记录命令不代表上传已完成。只有提交成功才确认该设备代际的驻留状态；同批可见性由batch中的临时映射提供。后续提交使用已提交资源，遵循同一主线程的提交顺序；不要把“开始顺序”误写成CPU侧已完成。

- 获取命令、copy/render pass等失败或提交前取消：保留CPU数据和待上传状态；预留区域仍属于原FrameID，下次可重试，不分配重复区域或清掉dirty。
- 提交使用 `SDL_SubmitGPUCommandBufferAndAcquireFence`，保存序号、资源引用和staging slot。失败后命令buffer不可再用；按renderer错误路径清理，不伪造完成或盲目重提同一buffer。
- 需要取消命令时遵守swapchain获取后的SDL限制；实现中优先在获取可呈现swapchain前完成可能取消的准备，不能照抄当前所有错误分支无条件Cancel。该API约束须在实现的本机头文件核验中明确。
- 每次timer/present非阻塞Query fence，完成后释放fence、归还slot和退役资源。空swapchain不等于上传失败：若上传命令已提交，仍必须维护其fence和驻留状态。
- 达到在途上限时不获取新commandbuffer，不消耗待上传项，沿用窗口已有画面等待下次timer；不逐帧WaitForGPUIdle。
- 关闭renderer时可沿用已有WaitForGPUIdle再释放；设备不可用时走明确的失败清理，不能在旧device上继续查询或重用句柄。

### 9.3 页面与palette退役

最后使用序号包含上传和draw，不只统计“最后一次渲染”。提交前packet引用和最新快照引用也算pin。回收必须在这些CPU引用撤销且所有使用它的GPU工作已完成后执行。首版只做整页退役，不存在旧帧区域被新帧提前覆盖的问题。

## 10. 绘制与裁剪

逻辑位置 = SpriteCommand.Anchor + FrameRecord.Left/Top。先与该元素Clip求交，再与逻辑画布求交，得到sourceCrop。atlas源坐标 = AtlasLocation.X/Y + sourceCrop；缩放继续走现有letterbox viewport，不加入桌面窗口偏移。

双重裁剪只能累计一次源偏移。frame大小、atlas页大小和逻辑画布大小是三个独立量，不能把atlas尺寸传给现有GetLetterboxViewport作为游戏分辨率。透明顺序维持背景→下部按钮→TOP/Radar按钮→维修/卖出后层→光标；预留未接入层位置。

## 11. 模块职责与迁移位置

| 文件/模块（拟定） | 职责 |
| --- | --- |
| `FrameCache.h/.cpp` | 强类型ID、资源注册/失效、FrameRecord、纯CPU预算与按需查找；不含SDL类型 |
| `AtlasAllocator.h` | 纯矩形分配与页选择；不读引擎、不调用SDL，可独立测试 |
| `FrameAtlas.h/.cpp` | 页纹理、驻留映射、palette GPU资源、staging、上传batch、fence及退役 |
| `SidebarSnapshot.h` | 引擎适配器：状态/绑定/布局读取，发布FrameID+PaletteID绘制快照 |
| `ShapeImage.h` | 保持纯plain/RLE解码，共用；文件/内存边界解析与解码职责分开 |
| `GPURender.cpp` | 调度cache/atlas与present；替换16槽SidebarTextures、像素比较与UploadSidebar |
| `PresentPass.*`、新索引fragment shader | 双纹理pipeline、区域裁剪与原有RGB16路径共存 |
| `Hooks.cpp` | 最小生命周期通知；新增地址先验证，不在hook里调用SDL或等待GPU |

源文件及新shader接入现有项目/构建规则时需单独列出最小配置变更，遵循仓库确认要求；本次设计不修改项目配置、共享YRpp类型或第三方源码。不要为绕过配置确认而把整个缓存实现塞进一个巨大头文件。

建议接口：`AcquireResource`、`FindOrDecodeFrame`、`AcquirePalette`、`InvalidateResource/Domain`；GPU层 `PrepareSubmission`、`EncodeUploads`、`ResolveSpriteForBatch`、`CommitSubmission`、`CancelBatch`、`PollRetired`、`DropDeviceResources`。引擎和SDL错误转换成显式结果，不通过空纹理伪装命中。

## 12. 分阶段实施与门禁

1. **身份与CPU缓存**：先测试ID/代际/地址复用、帧键与按需解码，拆分布局和资源失效。核验生命周期入口，接入现有sidebar；阶段产物必须能统计decodeCount，不以肉眼正常代替缓存正确。
2. **纯atlas分配与GPU上传**：shelf、多页、预算、驻留状态、fence、退役；用人工小页和测试索引图验证，暂不替换完整原生快照。
3. **索引shader与palette缓存**：DXIL/SPIR-V构建，透明/黑色/颜色量化核对；增加R8能力探测和R32回退验证。
4. **整合sidebar**：替换旧16槽上传，保留输入/选帧/层序、光标与镜像；校验截图和计数器。
5. **部署采样**：至少实际D3D12遭遇战，静态画面、按钮往返切换/闪烁、Alt-Tab、分辨率、新对局/资源重载。Vulkan和额外侧别分别记录，不能泛化。

缓存涉及共享身份、资源寿命及GPU同步，采用Level 2：先写关键行为测试，再实现。性能与运行检查随后进行；不对设计文本、shader生成产物或UI文案编写内容锁定测试。

## 13. 验证矩阵与量化验收

### 纯CPU及故障注入

- 同资源同帧重复请求ID不变；不同帧、不同资源、卸载后同地址新资源ID不同；旧ID查找明确失败；计数器溢出不得复用。
- 两按钮共享帧、同帧不同palette；A/B/A选帧只解码A和B各一次；同帧plain/RLE输出一致；截断、越界、零run与透明黑继续覆盖。
- shelf边界、恰好整页、跨页、不重叠、容量耗尽；无在线复用/移动；FrameID不随page变化。
- palette相同内容不同指针可复用，hash碰撞仍比较全部数据；同指针内容变化产生新版本；颜色格式改变不让旧快照变色。
- 元素负位置/LeftTop、sidebar边缘、画布裁剪、多页源区域、1像素边界不会读邻接帧。
- batch取消/提交失败不能误标驻留；同batch重复FrameID只上传一次；忙staging不覆写；在途页面不能提前回收；空swapchain仍能正确结算上传提交。
- GPU页淘汰后FrameID保持、CPU数据不重新解码；设备资源重建需重传但不换FrameID；资源失效则旧FrameID不可复活。
- 全部页pin、CPU预算满、palette版本耗尽及未加载资源返回明确状态，其他已缓存元素继续绘制，内存不无限增长。

### GPU与实画面对照

- 人工微型页强制跨至少3页；先画A，再在同页追加B，确认A持续正确，专门捕获错误cycle导致的旧区域丢失。
- R8_UINT与R32_UINT回退输出一致；palette0非黑仍透明、非零索引黑色不透明；最近邻缩放无相邻帧串色。
- 稳定画面完成首次上传后，连续300次采集/present：`frameDecodeCount`、`indexUploadBytes`、`paletteUploadBytes`增量均为0，状态和坐标读取仍可发生。
- 所需按钮帧都首次出现后，往返选中/禁用/闪烁100次：索引解码/上传增量为0；场景新产生的帧必须单列，不能算成缓存回归。
- 仅palette内容变动：FrameID、decodeCount及indexUploadBytes不变，仅新增/复用palette版本。仅布局变化：图像及palette上传增量均为0。
- 记录cache hit/miss、decode耗时、页面数/占用率/预算、CPU索引字节、上传有效/填充字节、palette版本、淘汰/退役字节、最大在途数、失败原因。计数器始终便宜可读，日志只按阶段汇总或有界事件输出。
- 验收截图/日志标注后端、侧别、分辨率、代际和模块身份；普通present、WM_PAINT/Alt-Tab使用最新合法快照，资源重载不残留旧图。

## 14. 风险、限制和完成定义

主要风险是漏掉素材生命周期事件导致“地址相同但内容已换”、atlas局部上传误用cycling、在途资源过早释放以及旧快照配新palette。前三者必须以生命周期/故障注入/GPU测试补证，不能用增加hash或一次画面正常替代。

shelf存在碎片且不可在线整理；CPU索引保留提高内存占用；小纹理变atlas不自动减少draw次数；首次解码/上传仍有成本。首版预算不足采用显式缺失，不承诺当前设计可直接覆盖整个世界资源工作集。

设计可以进入实施。完成实施必须同时满足：核心测试、构建、hook检查、至少一种后端实画面对照和上述缓存计数目标；未覆盖后端/侧别如实保留。原始文件最终archive来源、末帧完整文件长度和模组未通知的原地写入仍是独立限制。不要在只做完ID map或纹理拼接时宣布整个缓存方案完成。

## 15. SDL依据

本次通过find-docs/Context7查阅SDL官方wiki，并与当前本地代码对照：

- [GPU cycling](https://wiki.libsdl.org/SDL3/CategoryGPU)：cycling使整资源新存储未定义；允许对已绑定资源中未被引用的区域写入。
- [SDL_UploadToGPUTexture](https://wiki.libsdl.org/SDL3/SDL_UploadToGPUTexture)：copy发生于GPU时间线，后续命令可使用上传结果。
- [SDL_GPUTextureTransferInfo](https://wiki.libsdl.org/SDL3/SDL_GPUTextureTransferInfo)：texel对齐及D3D12的256字节row pitch/512字节offset优化要求。
- [SDL_SubmitGPUCommandBufferAndAcquireFence](https://wiki.libsdl.org/SDL3/SDL_SubmitGPUCommandBufferAndAcquireFence)、[SDL_QueryGPUFence](https://wiki.libsdl.org/SDL3/SDL_QueryGPUFence)：提交所有权、提交顺序、非阻塞完成查询与fence释放义务。

具体实现前仍须对本机SDL 3.4.18头文件、DXIL/SPIR-V资源绑定及格式能力做定向核验；文档设计不代替运行时能力探测。
