# Voxel 图像身份优化：P0 调查与基线

2026-10-31；源码基线 `cd2990454f1af862db9b016f8d363cada22380d9`，本次增加诊断计时和独立微基准，尚未启用姿态／组合身份缓存。

## 结论与范围

首先优化车辆最终 scratch 的图像获取。即使 body/turret/barrel 已命中原生 voxel 缓存，仍先绘入 256×256×1 的车辆 scratch，再通过 `73B140 → 4373B0` 提交。当前 GPU 采集对这张组合图执行 `AcquirePixels`：临时分配、逐行复制、FNV-1a 哈希、候选图像比较。内容命中不会省掉这些工作。

`707480 → 437A10` 已由 `AcquireEncoded` 按数据地址、尺寸和 Epoch 复用解码结果，不应把该路径描述成每帧重新解码。

推荐后续方案：观察实际部件图像身份与有序合成参数，构造组合身份，在最终 blit 处复用 FrameID；未知写入、禁缓存动态部件与未闭合分支继续使用现有取图。继续执行原生绘制，不重推对象状态或改变图层顺序。

本次完成了静态入口调查、磁盘字节比对、运行入口字节采样、取图微基准和少量单位的 D3D12 现场基线。**没有完成千单位同屏实测，也没有完成新 Hook 的安装／冲突／寄存器门禁。** P1 前应继续补全所选新 Hook 的核验；P0 的负载扩展仍有待执行项。

## 证据与入口

逆向依据：`D:/gamemd-reverse/docs/zh-CN/units-composite-gpu-handoff.md`，以及 Basic Memory 项目 `gamemd-reverse` 的 Techno voxel drawing、image submission、lighting buffer reset 笔记。历史证据用于定位，本次另从 IDA session `fe4d1777` 读取了当前字节与伪代码。

| 入口 | 本次确认的作用 | 接入注意 |
| --- | --- | --- |
| `706640` | 原生 key/cache 查找；命中到 `707480`，miss 到 `706ED0`，之后可能保存图像 | key=-1、空 cache、DisableVoxelCache 都可能禁缓存；不能将入参 key 视为必然有效 |
| `707480` | 从 VoxelCacheStruct 取得 origin、尺寸、Buffer，包装临时 BSurface，最终调用 `437A10` | 身份依赖的是缓存图像／payload，不是栈 BSurface 的地址；图像可保留首次生成时的状态 |
| `73B140` | 将车辆 scratch 统一提交到保存的目标 surface | 普通一次、桥下等条件分两段；最终参数在 blit 处保留，不拆成部件分别混合 |
| `4AF2A0` | 未缓存 voxel／影子的 plain 提交包装 | 本身不能证明上游图像可复用 |
| `5F99E0` | 清各类型 body/turret/barrel/shadow cache，重置 arena 游标 | 一次绘制中也可能发生；身份必须带代次并验证最终代次 |

`73B140` 的三条 `call 4373B0` 位于 `73B377`、`73B3E5`、`73B446`，返回 PC 为 `73B37C`、`73B3EA`、`73B44B`。诊断只用这些返回 PC 分类；分类不意味着调用一定是纯 VXL，SHP+VXL scratch 也可能走这里。

`707480` 的缓存图像布局：signed16 X/Y、unsigned16 Width/Height、Buffer 位于 +8。原生函数有更新 BlitterInstance 等副作用；本方案只观测，不绕过它。

### 磁盘与运行时身份

- 隔离 `gamemd-spawn.exe` SHA256：`247f72881e1a68c8fc305e3702dc0100a72d17601305999026393c340d5daeb0`。
- IDA 对比区间：706640 起 1399 字节、707480 起 718 字节、73B140 起 814 字节、4AF2A0 起 144 字节、5F99E0 起 241 字节，均与隔离 EXE 对应磁盘字节相同。这些是明确长度的区间，不声称均覆盖全部尾部和函数 chunks。
- 用户指定实际启动入口 `D:/Phobos/Launch-Phobos.cmd → D:/Ra2/Launch-Phobos.ps1 → gamemd.exe`。
- 实测 retail EXE SHA256：`7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6`。
- 本次 Release DLL SHA256：`65554626d87bbddb5b7cb661a2580d28b8ffeeaea19e43cedf8f7dbcc22ea7f05`；HEAD 如上，dirty。
- 实测 PID 38840，instance `3dfe2d63835d155971aeae20f8d81586`。通过该次随机 Token 认证并核对进程完整路径后才读取内存／清理进程。
- 运行中读取了 706640、707480、73B140 等入口，以及 706786、73B2A2、73BA12、5F99E0、4373B0、437A10 等补丁位置的 32 字节窗口。后三类可见实际 JMP；未跟踪完整 trampoline 链，因此不声称已验证全部 Hook 安装顺序。
- `40F270` 运行字节为 `68 00 00 00 08`，与当前 `Hooks.MatrixOp.cpp` 的 128 MiB 原生 voxel cache 补丁一致。**不能把 vanilla 4,000,000 字节池直接当成本 fork 的预算。** 它与 BlitFrameSource 的 32 MiB CPU 索引预算是不同缓存。

当前 Phobos 的 `73BA12` 会替换炮塔绘制区间，后坐和额外部件可传 key=-1/cache=null。不能只观察被跳过的 vanilla 调用点；宜在最终共用提交入口识别实际部件。

## 身份契约的实施边界

1. 首版以实际原生缓存图像身份为基础：payload、尺寸、资源／arena 代次；不能使用临时 BSurface 地址或对象指针独立作身份。
2. scratch 身份包含有序部件、相对位置、源区域、实际合成参数及已确认的清理规则。某个部件无稳定身份，则整个组合回退。
3. 当前普通采集只存裁剪后的区域。首版 key 必须包含精确源区域，或先证明完整有效图像边界后改成整图存储；禁止边缘半图复用为整图。
4. `753E00` 和 UseBuffer 影响共享工作缓冲。未缓存路径不能仅用模型、朝向和 HVA 帧宣称确定性；暂不开放跨帧复用。
5. 最终 palette、brightness、tint、Z、gradient 和顺序仍取实际 blit 参数。先证明某项不影响源索引才从图像 key 排除。
6. 身份映射返回 FrameID，命中时 Resolve；CPU 淘汰、场景清理与原生地址复用分别失效。GPU 页淘汰／设备重建仍由现有 FrameAtlas 恢复，不能长期保存 UV／texture 裸指针。
7. 新 Hook 的长度、指令边界、保存寄存器、早退和嵌套栈、Ares/Phobos/SyringeEx 冲突在 P1 按 check-hooks skill 单独完成。本次未修改 Hooks.cpp。

## 诊断计时

进程启动前设置 `PHOBOS_VOXEL_PROFILE=1`。默认关闭；第一次采集时读取，最多 16 个窗口，每窗 120 次 RenderLayers。统计在游戏线程的固定大小数组里累积，只在窗口结束输出；不开逐像素核验和逐 draw 日志。

日志 `[VoxelP0]`：

- `renderLayersInclusive`：BeginRecording 末尾至 RecordAfterLayers 开始，含原生 RenderLayers、Hook 记录及计时开销；不含随后 native_composite 全图副本、CaptureAfterDraw、GPU 提交，也不是完整 CPU 帧耗时。开启 Terrain trace 会污染该基线，测量时必须关闭。
- `recordBlits`：RecordBlit 中填完基本记录后到返回的累计时间；包括裁剪、blitter 识别、验证、锁定和取图，不含 Hook 汇编开销。
- `acquireImages`：AcquirePixels／AcquireEncoded 调用时间（普通路径还包含 GetPitch 调用），不含 Lock/Unlock。
- `unitScratch`、`targetRLE`、`targetPlainOther`、`offTarget`：按返回 PC、目标和压缩标志分类；offTarget 当前只记录元数据，不获取像素。
- `newImages`：FrameCache 新解码／注册计数增量，不是完整 hit/miss 分类。仅在 ready 成功的受控窗口中，零新增支持复用已有图像的判断。
- `sourcePixels`：普通路径为实际复制／哈希的像素数；RLE 路径为请求的整帧像素数，**不代表命中时读了这些像素**。
- 每窗平均、nearest-rank P95/P99 和最大值。窗口聚合／日志输出在被测区间之外，但仍可能影响后续调度。没有扣除计时器开销。

## 微基准

运行 `scripts/benchmark_blit_frame_source.cmd`。直接链接当前 BlitFrameSource.cpp 与 FrameCache.cpp，MSVC 14.44、x86、C++20、/O2、/MT，asInvoker。输出到 `Debug/RenderTests/VoxelBenchmark/results.csv`。

环境报告 CPU 为 i9-13900K，24 核／24 逻辑处理器。基准 affinity=1，与本次游戏日志的 affinity=1 对齐；未锁定频率或隔离 OS 调度。

每项 32 个预热图像、120 个测量样本，测试 100/500/1000 次取图及 32/64/128 方形图像。普通源 pitch=256；RLE 源为合法非零字节行。循环验证所有 ID 有效且测量阶段无新增解码，校验和写入 volatile 防止消除。**不包含原生体素生成、surface Lock、VirtualQuery、GPU 上传或游戏 AI，不是千单位 FPS。**

构建结束、无本任务游戏／回归构建并发时取得的一轮 affinity=1 数据：

| 1000 次热命中 | 普通内容缓存平均 | 普通 P95 | RLE 地址缓存平均 | 普通每样本复制且哈希的字节 |
| --- | ---: | ---: | ---: | ---: |
| 32×32 | 1.006 ms | 1.210 ms | 0.0090 ms | 1,024,000 |
| 64×64 | 3.844 ms | 4.592 ms | 0.0097 ms | 4,096,000 |
| 128×128 | 14.400 ms | 15.807 ms | 0.0101 ms | 16,384,000 |

每个字节至少参与一次复制和一次哈希；命中还可能比较内容，表中不是全部内存流量。RLE 是现有可复用身份路径的参照，不代表尚未实现的组合 key 一定达到同样耗时。另保留重复运行 CSV，数值受调度影响。

## 实机首轮基线

用户授权临时备份／替换正式安装的 DLL/PDB 和启动脚本后，通过自带 Launch-Phobos.cmd 启动；脚本仅临时增加环境变量控制的 Automation 参数。没有写入测试地图或手工修改正式安装 INI／存档。通过游戏菜单进入原有遭遇战，N 选中 AMCV，并通过 X 发出多次 Scatter；截图可见车辆，统计有新姿态图像。输入成功与全部移动行为验收不混同。

实机 D3D12、independent，场景截图 1024×768。采集 8 个完整窗口，共 960 次 RenderLayers，每窗 120 次 scratch 和 120 次 RLE 提交，属于单车场景。

| 样本 | scratch Acquire | scratch RecordBlit | RLE Acquire |
| --- | ---: | ---: | ---: |
| 第 2 窗，双方零新增图像 | 6.454 μs/次 | 10.295 μs/次 | 0.378 μs/次 |
| 第 2–8 窗汇总，含转向／新图像 | 6.717 μs/次 | 10.804 μs/次 | 0.550 μs/次 |

第 2 窗 scratch 平均 4200 像素，仍复制且哈希 504000 字节；RLE 请求平均 1848 像素，零新增时走地址复用。第 2–8 窗双方各新增 24 张图像，不能把整个汇总称为纯热命中。

这支持先优化 scratch 采集，但不能把单车耗时直接乘 1000 作为游戏实测。完整主线程、原生 voxel 栅格化和 GPU 提交占比尚未分离。

原生／GPU PNG 已解码查看，能观察到对应车辆。两来源顺序采集，仅 best_effort；没有据此声明严格同帧逐像素通过，也未做本次 Vulkan 实机验收。测试结束清理本次认证的游戏 PID，没有验收菜单正常退出。

### 首次失败启动

在用户指定启动入口前，曾用隔离 gamemd-spawn.exe 尝试 2560×1440 测试，启动即失败。Windows Application 1000 事件：c0000005，偏移 BA626，对应 CreateSurface 的 4BA626，尚未开始 voxel 采集。静态定位只能说明失败点，未取得寄存器／dump，**根因未确定，不能归咎于分辨率或本次计时**。此轮无有效性能数据。后续遵照用户要求只使用自带 Phobos 启动脚本。

## 验证与恢复

- Release/Win32 主工程构建退出 0，无 warning/error。
- `scripts/test_render_capture_regression.cmd` 退出 0；脚本输出有既有测试／YRpp 编译警告，不描述成全套零警告。
- 独立微基准编译／执行退出 0，预热后无新增解码检查通过。
- 本次没改 Hook 入口／函数体、shader、MCP Schema 或依赖。
- D:/Ra2 的 Phobos.dll、Phobos.pdb、Launch-Phobos.ps1 均已恢复，分别与备份前 SHA256 一致。隔离副本 DLL/PDB 和本次临时 INI 也已恢复；仅删除本次创建的临时测试地图。
- 正式安装原 DLL SHA256：`43198e860a81a9032b67efbb54d10283f14376f3dc987addcf02112d97763494`。

本机原始证据在 `Debug/AutomationTests/voxel-p0/`（不入库）：IDA 区间、磁盘比对 JSON、构建／回归日志、benchmark CSV、模块哈希、真实启动脚本运行的 results.json／日志／PNG、恢复哈希。无 Token 或游戏资产写入版本库。

## 下一步

P1/P2 先实现只观测的部件身份和 scratch 组合上下文；采样核对同身份的实际源索引，不启用快速返回。第一批只准入稳定原生缓存部件，动态／未知分支回退。优先处理裁剪、桥下分条、一次绘制内 arena 清空和嵌套调用。

尚待：在自带启动脚本路径下准备获准的 100/500/1000 可见单位场景、完整帧 CPU 分项测量、多个单位类型与动态姿态、Vulkan，以及所选新增 Hook 的完整冲突与运行验证。本轮不承诺这些负载已通过。
