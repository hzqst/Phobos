# Voxel 身份快速复用（P3，显式启用）

2026-10-31，基于 `fe2289a0`；接续 [P1/P2](VoxelIdentity.md)。本阶段开放受限的 CPU 图像身份复用，让现有 FrameAtlas 继续复用 GPU 驻留。默认关闭，不宣称已解决完整游戏的千单位 CPU 瓶颈。

## 使用与准入

- `PHOBOS_VOXEL_IDENTITY_FAST=1`：启用快速路径。命中时跳过最终 scratch 的 Lock、AcquirePixels、临时像素分配、复制和内容哈希，返回原有 FrameID。
- 同时设置 `PHOBOS_VOXEL_IDENTITY_SHADOW=1`：逐个拟命中仍执行原有取图，比较完整像素；一致才使用候选 ID，不一致使用本次实际图像并禁用整个 epoch。此模式用于正确性验收，不用于测量省掉取图的收益。
- `PHOBOS_VOXEL_PROFILE=1`：保留 P0 分项计时。`[VoxelP3]` 另外报告 hits、verifiedHits、savedBytes、seedBytes、clearBytes、seedFailures、clearFailures。savedBytes 仅指省掉的索引复制／内容哈希长度，不是全部内存流量。

首版只接受 `73B470` 作用域内恰好一个已缓存 RLE 部件，普通最终返回 PC `73B44B`，已知 byte translator blitter，256×256×1 的原生 BSurface scratch，完整 scratch clip，且部件目标矩形完全位于 scratch 内。桥下另外两个提交 PC、多部件、SHP 混合、禁缓存动态生成、未知写入、嵌套和非法范围均使用旧采集路径。最终屏幕裁剪后的精确源区域仍在 key 中，透明绘制顺序、深度、palette 与最终 brightness/tint 均沿用原记录。

## 确定性与清理证明

本次在正式 retail EXE（SHA256 `7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6`）上用 IDA 核验：

1. `7473E0` 创建 `B1D13C` 指向的 256×256×1 BSurface，安装 vtable `7E2070`，并在 `74743E` 调用 Fill(0)。`73B470` 每次末尾仍执行 native scratch clear。
2. `707480` 把原生缓存 payload 包装成临时 BSurface，最后在 `707736` 调用 `437A10`。记录 payload 地址和 cache epoch，不把临时 Surface 地址当身份。
3. `RLEBlitTransXlat<unsigned char>` 的 vtable 为 `7E5B00`，方法为 `491820/4918A0`。非零源索引通过对象 +4 的 256 字节映射表写入目标；零 run 跳过目标。该实现不读取目标像素、Z、A 或光照缓冲。快路径要求此准确 blitter 类型，并把映射表完整内容放进 key；key 的完整相等比较处理哈希碰撞与表内容变化，不依赖地址不变假设。

另外使用运行时清理检查：每个 RenderLayers 首次作用域、buffer 改变或清理状态不再可信时，只读检查整张 scratch 是否为零；不修改原生像素。已知单部件写入记录完整目标矩形；函数末尾原生 clear 之后再检查该矩形为零，才允许后续作用域继承干净状态。未知写入通知在作用域外也会丢弃干净状态。每次捕获重置证明，不能跨帧继承未观察阶段的写入假设。失败只回退，不强行清空原生 scratch。

整行连续范围使用一次 memcmp，局部矩形逐行检查；不会每个命中再复制源图，但清理检查仍读取像素。一帧内首个单位的整张检查可由后续受支持单位摊薄；单车每帧都承担这部分成本。

这仍依赖已核验的原生／当前 Phobos 写入路径；外部模块直接写 scratch 且绕过现有采集入口不在支持契约内。因此保持显式实验开关，逐命中验证模式用于扩大现场覆盖。

## 资源生命周期

第一次身份只能建立基线，第二次旧路径采集的完整像素必须一致，第三次起才允许 Lookup。Lookup 每次 Resolve FrameID，失效后回到旧采集并重新验证；缓存上限仍为 512 项，容量满时清空映射不会复活旧 ID。epoch 改变使旧身份不再匹配。

新增 `BlitFrameSource::Touch` 记录当前捕获 Serial，Sweep 同时检查普通取图和身份命中的最近使用时间。仅更新 CPU 图像存活信息，不固定 GPU 页、不保存 UV，也不绕过 Atlas.Submit、上传、fence 或退役。旧帧淘汰和 Clear 同步移除身份使用记录。已有压力回收策略保持不变。

日志仍限 16×120 次捕获；快速路径在日志上限后继续运行。只有建立基线／纯旁路观察受每帧 256 次采样限制；验证模式对每个拟命中做实际像素核对，不让超出采样预算的命中免检。

## Hook 与测试

没有新增或改变 Hook 宏。技能 `discover_hooks.py` 输出空数组，因此手动补查调用函数体发生变化的两个现有 Hook；`check_hook_conflicts.py` 使用 Ares 3.0p1 清单，均无冲突。未新增固定返回地址。

| Hook | 长度／边界 | 冲突 | 寄存器／栈 | 相对指令 |
| --- | --- | --- | --- | --- |
| `73B470` | 完整 6 字节 sub | 无新增冲突 | sub 前只读 ESP | 无 |
| `73C5DC` | 4 个 pop + 6 字节 add，共 10 字节 | 无新增冲突 | 入口 ESP = 当前 ESP + 1C4h；clear 后读取 scratch | 无 |

定向测试覆盖基线不能直接复用、像素核对后准入、映射表变化、epoch 不匹配／禁用、FrameID 失效、干净初态、未知写入、buffer 替换、清理失败、嵌套、多部件拒绝、矩形越界，以及超过两轮 MaxIdle 的身份命中存活与最终回收。Render 回归已通过。

微基准新增 `verified_identity_hit`，与 plain_content_hit 使用同一批源图，统计 Lookup + Resolve + Touch；排除 key 采集、scratch 清理检查、原生绘制与 GPU。它不能当作千单位游戏 FPS。

## 本轮验证结果

Release/Win32 构建退出 0，无编译 warning/error；Render 回归退出 0，保留已有测试／YRpp 编译警告。最终只读零检查优化后再次构建，并对同一 DLL 串行完成 verify、fast、baseline 实机轮。没有修改 shader、MCP 协议或 GPU 提交流程。

使用用户指定的 `Launch-Phobos.cmd` 启动正式 `gamemd.exe`，沿用本会话临时部署授权，每轮重新备份 DLL/PDB／启动脚本并在结束后恢复；各文件 SHA256 全部与当轮备份前一致。没有启动 gamemd-spawn.exe，没有手工改正式 INI 或存档。每轮只清理随机 Token 认证、实例和进程路径匹配的自有游戏进程，未测试菜单正常退出。

最终 DLL SHA256：`a2f50295aa714dd3488d08b73d82b68066af6359be7d366b0048f8af87c757c0`，源码 `fe2289a0-dirty`。三轮均 D3D12、independent、1024×768，N 选中并断言 AMCV，再多次 X Scatter。

| 模式 | PID / instance | 16 窗内结果 |
| --- | --- | --- |
| verify | 2428 / `1d2a91fe20f0f07a2d4d9511102fb21f` | 1920 次提交，1827 次拟命中逐像素核对通过，29 次无资格；mismatch、scopeErrors、seedFailures、clearFailures 均 0 |
| fast | 30072 / `f6d3394ca80415fce53d8d2a53c1c097` | 1920 次提交，1841 次实际命中，省掉 8,030,916 字节复制／哈希；25 次无资格，33 次建基线，21 次一致观察；作用域与清理失败均 0 |
| baseline | 9468 / `1b0b7aa3154260cb3a7ccfebb836dfd1` | 同一 DLL 关闭 FAST/SHADOW，保留 P0 计时；无 P2/P3 日志 |

fast 轮另读取 125,829,120 字节用于逐帧初态检查、8,303,195 字节用于局部清理检查，并复制每个受支持部件的 256 字节映射表。不能把 savedBytes 当作总内存读取量减少。最终 PNG 已解码查看，车辆移动／朝向变化与原生画面有对应表现；两来源先后采样仅 best_effort，未声称同帧图像逐像素一致。验证模式的源索引比较是在同一次绘制内完成，和先后截图是不同证据。

本轮全部窗口的 scratch RecordBlit 平均：baseline 12.993 μs/次，fast 6.182 μs/次；稳定全命中窗口的 AcquirePixels 像素数和耗时均为 0。包含原生工作、观察及清理检查的 RenderLayers 平均分别为 81.304 μs 和 77.086 μs。不同轮次的游戏状态、姿态、调度不完全相同；早一轮单车样本还出现 baseline 67.095 μs、fast 81.921 μs，因此**只确认目标取图子路径的收益，不把这些数值解释为稳定整帧／FPS 提升**。

独立 x86 /O2、affinity=1 的 1000 次热命中微基准（120 样本，非游戏帧）：

| 图像尺寸 | 原内容路径平均 | 已验证身份查询＋续期平均 | 省掉的复制／哈希字节 |
| --- | ---: | ---: | ---: |
| 32×32 | 0.997 ms | 0.139 ms | 1,024,000 |
| 64×64 | 3.762 ms | 0.134 ms | 4,096,000 |
| 128×128 | 14.440 ms | 0.136 ms | 16,384,000 |

这些结果排除身份采集、清理检查、原生 voxel 生成、游戏逻辑和 GPU。下一阶段需实测 100/500/1000 可见单位下检查成本的摊薄与完整 CPU 分项；多部件、桥下、屏幕边缘、真实 arena purge 和 Vulkan 尚未实机验收，不能从单 AMCV 推广。

证据目录：`Debug/AutomationTests/voxel-p3/`。最终三轮在 `contiguous/`（results、原生/GPU PNG、日志、模块身份、恢复哈希、汇总 JSON），早期轮次保留用于对照；`benchmark.csv` 为独立取图基准。不提交二进制、游戏资产、Token 或临时测试驱动。
