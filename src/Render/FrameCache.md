# 资源身份与 CPU 帧缓存（阶段 1）

依据 [完整设计](FrameAtlasDesign.md)，本阶段只接入CPU身份/索引缓存，保持现有独立GPU纹理。没有atlas、PaletteID、索引shader或GPU页淘汰；按钮切帧仍可能进行颜色映射与GPU上传，不能据本阶段宣称完整atlas优化已完成。

## 数据路径

`6A70AE → SidebarSnapshot::CopyAsset → ShapeFrameSource::Acquire → FrameCache::Cache`。

- `ResourceID`、`FrameID` 使用不同强类型。64位序列进程内递增、不复用、不因Clear/新对局/另建Cache实例重置；0无效，溢出后保持耗尽。
- Resource身份表按当前已加载Data地址查找，但只在明确生命周期通知之间有效；不以地址本身代替生命周期。reference和直接Data别名可以共享ResourceID。卸载撤销所有已知别名和帧，旧FrameID的Resolve立即失败。
- 资源首次注册复制整份帧描述表，按排序后的下一偏移建立编码界限；首次请求某帧时解码为自有8位索引。未请求帧不解码。warm path只检查原始头、当前帧元数据与身份，不扫描整个素材或执行像素hash。
- 当前帧头/文件头变化会使整个资源版本失效；纯像素原地改写且没有生命周期通知仍不支持。已加载条件不满足时不会调用原版Load/GetData。
- 缓存命中直接复用索引。sidebar现有颜色像素仅在FrameID、实际256项palette映射变化或本地像素副本丢失时重新生成；同一当前帧持续绘制不重新解码/映射。A→B→A仍会重新映射A及上传到现有槽，但A的索引不会再解码。
- 快照发布前核对各FrameID有效性，避免后处理别名使前面已采集的资源失效；GPU/timer继续只读自有像素，不访问engine数据。

## 生命周期

| 事件 | 行为 |
| --- | --- |
| Sidebar Init/Unload、Scenario Clear、UI SHP清理 | 清CPU资源/帧/别名表并撤销快照；计数器与ID序列不重置 |
| Radar Dispose | 仅撤销已采样的Radar按钮资源，再撤销对应就绪与快照 |
| ShapeButton SetShape/ClearShape | owned素材会直接free，入口先撤销其资源；非owned仅对本批按钮撤销快照，不凭解绑一个别名宣布共享数据已释放 |
| SHP Unload、析构、删除析构 | 按入口this对应的reference/Data身份使资源及所有帧失效，未跟踪素材直接返回 |
| UI palette清理 | 撤销读取许可与快照，不清帧索引；恢复需等待原初始化就绪路径 |
| 布局/视频模式、GPU资源释放 | 撤销快照，不改CPU ResourceID/FrameID；资源本身若随后被原生重载，则由资源边界另行失效 |

新增入口为 `69DE00/69DE70/69E100/69E500/69E930`，均在原函数执行前读ECX=this，return 0，不改原寄存器/栈。SetShape/ClearShape的直接free路径已通过本机指令核对：先查+5C所有权，再释放+58素材，因此不能只拦SHP析构。

当前适用范围是已调查的sidebar背景与十按钮。没有全局堆free拦截，不保证任意模组绕过这些生命周期入口释放/原地修改素材时可透明兼容；后续cameo接入仍须核对其加载/释放边界。

## 预算、失败与统计

- 索引像素有效字节64MiB、资源256个、总帧描述槽65536、reference/Data别名512个。描述槽同时限制了成功记录与损坏帧负缓存的数量；实际allocator/map开销不包含在索引字节统计中。
- 帧尺寸上限2048²、编码读取上限16MiB；没有可信完整文件长度时，末帧仍只能验证可读内存与格式界限。
- 解码成功且输出长度一致才发布FrameID；错误帧在当前ResourceID下负缓存，不反复解码。NotReady可稍后重试；超预算在解码之前拒绝；资源失效会归还活跃预算。
- `FrameCache::Cache` 不读取引擎、不依赖SDL，decoder同步执行且不得重入修改cache。测试可用小预算和人工解码器验证失败路径。
- `PHOBOS_SIDEBAR_TRACE=1` 开启 `[FrameCache]` 汇总：首次和每300次有效采集尝试记录，独立上限64条。包含hits/misses/attempts/decoded、与上次汇总相比的decodedDelta、活跃索引bytes/frames/resources、invalidations/invalid/budgetRejected及颜色转换次数。采集次数不是GPU present次数，也不保证每次原生帧都会经过此采样点。
- `[SidebarButtonGPU]` 追加frameID，可核对相同帧的复用与新对局的ID变化。计数累积跨对局，活跃数量随清理下降；首次新帧使decoded增长是正常miss。

## 验证

新增 `FrameCacheTests.cpp` 覆盖强身份使用、A/B/A、连续300命中、同地址重载/旧ID失效、预算归还、错误帧负缓存、未就绪重试与ID耗尽。新增 `ShapeFrameSourceTests.cpp` 用人工plain/RLE文件和reference测试别名共享、300命中、卸载、同地址重新加载及帧元数据变化；不调用引擎构造/析构，不需游戏进程。

上述两组测试及既有ShapeImage、ButtonImage、CursorImage回归通过。Debug x86最终构建通过；初次链接曾遇到Windows的FindResource宏展开冲突，已将接口更名LookupResource并重跑测试/构建。用户仅批准在Phobos.vcxproj中增加FrameCache.cpp编译项，配置diff只保留该一行。

### Hook矩阵

使用check-hooks技能规定的discover/conflicts脚本，补查回调行为受影响的既有入口。无冲突/叠加警告；新增入口另做当前源码范围重叠检查。

| Hook | 地址/长度 | P0 | P1 | P2 | P3 | P4 |
| --- | --- | --- | --- | --- | --- | --- |
| ShapeButton_SetShape_GPUFrameCache | 69DE00/6 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| ShapeButton_ClearShape_GPUFrameCache | 69DE70/6 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SHP_Unload_GPUFrameCache | 69E100/7 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SHP_Destructor_GPUFrameCache | 69E500/7 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SHP_DeletingDestructor_GPUFrameCache | 69E930/7 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| UI_UnloadPalette_GPURenderInvalidate | 730400/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SidebarClass_InitForHouse_GPURenderInvalidate | 6A5840/6 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SidebarClass_UnloadShapes_GPURenderInvalidate | 6A5BF0/7 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| Scenario_Clear_GPURenderInvalidate | 6BE1C0/8 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| RadarClass_Dispose_GPURenderButtonsInvalidate | 652D90/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| UI_UnloadShapes_GPURenderInvalidate | 730100/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SidebarClass_Draw_GPURenderSnapshot | 6A70AE/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |

IDA health=Session not found、idb_list为空，P2–P4专项跳过。补充Capstone检查本机retail/spawn窗口字节一致且覆盖完整指令，新增窗口不包含相对控制流指令，ECX通过入口mov esi/edi,ecx及下游字段访问核对；不存在栈参数提取或寄存器写入。既有6A5BF2、6A70AE覆盖相对CALL，由SyringeEx重定位。这不是IDA类型/栈专项检查通过的替代声明。

## 部署

2026-10-28部署至D:/Ra2，DLL SHA256 `7de5cac757c4452973c022c415dbf8cd745c35a63f7b454aa8a8bb9eaa08dd93` 与构建输出一致；旧DLL/PDB备份 `D:/Ra2/Phobos-backup-framecache-20261028-134833/`。通过普通权限原脚本启动，PID32472，用户对采集验收反馈“正常”。启动时短暂出现Debugger Notice，随后进入对局，未据此认定崩溃。

本轮D3D12/side=0/1024×768日志已保存于 `D:/Ra2/FrameCache-evidence-20261028-32472/debug-accepted.log`，同目录保存最终构建和hook核验记录：

- 首次采集16帧，索引54892字节；第300次采集17次解码、4783次命中；第600次18次解码、9582次命中，索引57632字节。每次新增解码均对应首次出现的状态，未见Invalid或BudgetRejected。
- 维修按钮frame0→1→0的FrameID为8→18→8，切回命中；该阶段颜色转换次数增长到19符合保留旧纹理路径的预期。
- 同进程第二次加载XMP30S6.MAP后，generation31→66，十按钮FrameID从7..16变为25..34，建筑选中帧再获得35。卖出/维修等SHP reference地址仍相同，Data地址变化，新FrameID没有复用。资源重载有机器日志证据。
- 日志中的300次区间含新帧首次出现，尚未单独取得“完整稳定300次、decodedDelta=0”的对局区间；该行为已由核心和原始帧适配器各300命中测试覆盖。闪烁100次、全部按钮状态、多侧别、Vulkan等仍未覆盖，不冒充完整atlas验收。
- 窗口定位日志随800→1024变化从x1467调整至1687，实际主窗口右边缘1392→1612；只作为定位请求已执行的证据。
