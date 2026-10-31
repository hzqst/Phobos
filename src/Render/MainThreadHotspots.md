# 千单位主线程热点：2026-10-31

## 结论

千 AMCV 静止场景中，正常模式最重的是原生对象层及随附 GPU 采集；屏蔽 UnitClass VXL 绘制后，
最重的是 `OverlaySnapshot::CaptureAfterDraw` 中的对象扫描与重复内存可读性检查。
剩余约 19 ms CPU 不能统称为游戏逻辑：约 13 ms 落在这段渲染采集中。

## 同一 DLL 的完整互斥记账

基于 `8d7754d0` 加本次诊断计时，Release/Win32、D3D12 independent、2560×1440、GameSpeed=1，
1000 个玩家 AMCV 均存活，采样前后位置不变。FAST/SHADOW 关闭。通过自带 Launch-Phobos.cmd 启动正式 gamemd.exe。
DLL SHA256：`22c148a0ba08644261466e661a76d2dc18f738b996cf46bba1774f58a03cb687`。

单位：ms／已完成逻辑帧。父阶段扣除已单列的子阶段；不能再把父阶段 inclusive 时间相加。

| 阶段 | 正常：独占墙钟 | 正常：CPU 估算 | 屏蔽 VXL：独占墙钟 | 屏蔽 VXL：CPU 估算 |
|---|---:|---:|---:|---:|
| 原生 RenderLayers 及其内部采集 | 25.588 | 22.910 | 0.602 | 0.549 |
| 后续对象采集 OverlaySnapshot::CaptureAfterDraw | 14.470 | 13.188 | 14.233 | 13.050 |
| 原生主循环其余部分，尚未进一步拆分 | 5.534 | 2.552 | 5.329 | 2.519 |
| GPU Present，不含下行 GPU 准备 | 1.512 | 1.311 | 0.635 | 0.561 |
| 地形／ABuffer CPU 采集 | 1.057 | 0.976 | 1.049 | 0.961 |
| GPU 准备：atlas、地形实例、侧栏实例等 | 0.708 | 0.583 | 0.501 | 0.451 |
| 原生合成截图源拷贝及 layers 收尾 | 0.492 | 0.452 | 0.522 | 0.477 |
| 侧栏／命令栏采集 | 0.439 | 0.363 | 0.406 | 0.366 |
| 所有计时作用域之外 | 0.004 | 0.002 | 0.004 | 0.002 |
| **合计** | **49.803** | **42.336** | **23.281** | **18.936** |

计时窗分别覆盖 840 帧／1680 帧（7／14 个完整窗口），所有窗口嵌套错误为 0；
逐窗验证 exclusive 墙钟与 cycles 都闭合到总量。其他主循环行包含逻辑、其他原生绘制、消息处理及等待，
并非单独 AI／寻路。GPU 行是主线程准备／提交成本，不是 GPU 执行时间。

### 测量口径

- QPC 直接测 inclusive／exclusive 墙钟；等待和被调度出去的时间也会进入墙钟。
- QueryThreadCycleTime 累计每阶段的 exclusive CPU cycles。
- **CPU 分项为估算**：在每个 120 帧窗口内，按 exclusive cycles 占比分摊 GetThreadTimes 测得的主线程 CPU 总时间。
  不能将 cycles 按标称 GHz 直接换算为毫秒；变频、迁移和 API 计时粒度会影响分摊精度。因此表中三位小数仅用于记账，不代表同等准确度。
- 只采用完全位于外部采样窗口内部的计时窗。外部整段直接测量分别为 874 帧、42.191 ms CPU／帧，
  和 1825 帧、18.938 ms CPU／帧；与完整内窗合计接近，但两种范围不能混用来相减。
- 外部整段墙钟为 49.836／23.235 ms／帧；墙钟与 CPU 的差包含等待、调度及采样暂停，不能全归因于 GPU 等待。

## 调用栈与指令地址证据

普通权限 x86 外部采样器只访问本轮 PID/TID 和路径匹配的 gamemd.exe。约 8–12 ms 随机间隔采样一次，
每十次复制一次有限栈区；复制后立即恢复主线程，在采样结束后离线展开。只使用已捕获栈和 image 字节，
不从运行中的线程重新读取可变栈。Phobos PDB 与 DLL 匹配；旧引擎缺少完整 unwind 元数据，跨越引擎／trampoline 后的深层帧不作为可靠调用链。

| 证据 | 正常 | 屏蔽 VXL |
|---|---:|---:|
| 定点样本 | 2844 | 2774 |
| 总暂停 ms | 522.343 | 408.316 |
| 暂停占采样窗口 | 1.199% | 0.963% |
| 暂停 p95 μs | 309.0 | 242.9 |
| 叶节点 NtQueryVirtualMemory | 1133 | 1749 |
| 叶节点 NtWaitForSingleObject | 108 | 296 |

这些是**墙钟定点样本，包含等待**，不是有效的 ETW CPU 样本，样本占比不能直接乘 CPU 毫秒。
约 1% 的暂停开销没有从分段时间中强行扣除。保留原始 PC、符号偏移和暂停长度。

反复出现且有 PDB 支持的调用前缀：

```text
GPURender::OnToolTipsDrawn
  OverlaySnapshot::CaptureAfterDraw
    CursorSnapshot::Readable
      NtQueryVirtualMemory
```

源码对应 `CursorSnapshot.h:27` 的 `Readable`，每次循环调用 VirtualQuery。
`OverlaySnapshot.cpp:1837`、`:1855` 分别扫描地面层寻找 Terrain／Building，
`CaptureComposite` 在 `:1252–1268` 又遍历全部层并检查对象；VXL 屏蔽后这些扫描仍然执行。
样本中也出现 `CaptureComposite → NtQueryVirtualMemory`。不能把整个 13.05 ms 都精确归为单个 API，
但互斥阶段计时、调用栈和源码三者一致，足以把重复范围检查／对象遍历列为首要优化对象。

正常组有 260 个叶样本落在 `BlitFrameSource::AcquirePixels`；这是 scratch 复制、内容身份获取路径。
对 SHA256 `7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6` 的实际 retail EXE 副本用 IDA 核验：

| 原生函数范围 | 身份／行为 | 正常组叶样本 |
|---|---|---:|
| 494B60–494C3A | RTTI `BlitTransXlatAlphaZRead<unsigned short>`：Z 比较、ABuffer／亮度查表、写 16 位目标 | 490 |
| 491820–49189E | RTTI `RLEBlitTransXlat<unsigned char>`：缓存 RLE 部件写入 byte scratch | 92 |
| 7BB050–7BB33C | 原生 Surface 拷贝函数；不把最近 export 名称当准确函数名 | 37 |

首个函数的热点在最终软件像素合成，不是重新生成 voxel 几何。其余原生地址未全部命名，
不伪造逐个原生函数的精确 CPU 时间。DDraw／显卡驱动最近 export 名称也不当作精确私有函数名。

## 无效／受限尝试

- 初次 WPR 无系统采样权限；用户随后授权仅提升采样器，游戏保持普通权限。
- `hotspot-baseline` 丢 6696 个事件，`hotspot-baseline-r3` 丢 4918 个事件；不用于定量热点归因。
- 精简 `hotspot-omit` 无事件丢失，但定时 CPU 样本仍只覆盖开头不足一秒。
  三轮都不能提供完整持续的 CPU 栈分布，不用上下文切换栈次数冒充 CPU 消耗。
- 一次 UAC 取消／菜单选错图的测试失败后恢复，未纳入结果。
- `sample-omit` 首版在暂停期间展开栈，累计暂停 27.5 秒，性能数据作废；只用作初步调用链线索。
- `sample-omit-r2` 在离线展开阶段失败；修复读取回调的可空输出与地址边界处理后，`sample-omit-r3` 和 `sample-baseline` 完成。
  不用失败轮替代最终成功轮的截图／完成验收。

## 后续优先级

1. 合并一次采集中的重复对象遍历和内存范围检查，保留明确的对象生命周期／失效边界；不要直接全局删除安全检查或永久缓存地址可读性。
2. 对正常路径继续拆出原生最终软件合成与 scratch 工作；GPU 资源复用命中后，验证可安全跳过这些工作。
3. 优化后重跑相同基线及负对照，再扩展到真实千单位同时运动。当前结果只覆盖静止 AMCV。

> 2026-10-31 用户决定：彻底废弃 `CursorSnapshot::Readable`（VirtualQuery 探测），取代第 1 条中“不要全局删除安全检查”的约束。
> 理由：VirtualQuery 只能证明页面已提交，不能证明对象存活，释放后访问与类型混淆照样通过，反而把真正的违规访问掩盖成“不画”。
> 现在采集层只保留判空与数值边界（计数、帧索引、偏移溢出、编码上限）；若存在越界或悬空访问，应作为崩溃暴露并修正根因。
> 已知待查的跨帧裸指针：`OverlaySnapshot.cpp` `FindBlitter` 的 `BlitterOwners` 缓存 `ConvertClass*`，Convert 释放后会读到失效内存。

### 废弃 Readable 后重测（2026-10-31）

同一 runner、地图、分辨率与采样器，DLL SHA256 `7a64a85bb59738cb726f4234d55b73c6f4bfda2e0348be78319dc25b2e73db2f`
（HEAD `8f1be491` + 未提交的 Readable 移除）。证据 `Debug/AutomationTests/voxel-p4/runs/sample-noreadable-baseline`、`sample-noreadable-omit`。

| 指标（ms／逻辑帧） | 正常：旧 → 新 | 屏蔽 VXL：旧 → 新 |
|---|---:|---:|
| 外部整段主线程 CPU | 42.191 → 24.019 | 18.938 → 4.756 |
| 外部整段墙钟 | 49.836 → 28.831 | 23.235 → 16.020 |
| 逻辑帧／秒 | 20.07 → 34.69 | 43.04 → 62.42 |
| RenderLayers 及其内部采集（墙钟／CPU） | 25.588／22.910 → 20.353／18.581 | 0.602／0.549 → 0.536／0.493 |
| OverlaySnapshot::CaptureAfterDraw（墙钟／CPU） | 14.470／13.188 → 0.267／0.248 | 14.233／13.050 → 0.128／0.117 |
| 叶节点 NtQueryVirtualMemory 样本 | 1133 → 0 | 1749 → 0 |

- 两轮 1000 个 AMCV 均存活、可见、位置不变；文件按 SHA256 还原，临时地图移除，无崩溃。
- 同局 GPU 与 native_composite 战术区差异与旧轮一致：正常 11187／11187 像素，屏蔽 VXL 187／188（只在光标处）。
  跨局截图本身就不同（地形变体等），不能拿来判断画面对错。
- 屏蔽 VXL 新轮最大叶节点为 `Sleep`→`ZwDelayExecution`（1834 个），`main_loop_other` 墙钟 13.3 ms 大多是帧率限制等待，
  主线程已不是瓶颈。正常模式剩余热点为原生 VXL 软件合成及 `BlitFrameSource::AcquirePixels`（后续优先级第 2 条）。
- 单轮静止场景，未覆盖千单位同时运动。

## 验证与恢复

诊断开关 `PHOBOS_MAIN_THREAD_PROFILE=1` 默认关闭；复用既有主循环与 RenderLayers 调用边界，
没有新增 Hook 宏、地址或改变返回路径。最多 64 个 120 帧窗口。
嵌套独占记账、关闭路径、线程归属、窗口重置定向测试通过；Release 构建及 Render 回归通过，保留既有 YRpp 编译警告。
暂停／恢复、截图和对象快照均在最终两轮实机执行。所有源数据在 `Debug/AutomationTests/voxel-p4/runs/sample-baseline`、
`sample-omit-r3`，包括 `hotspots.json`、debug.log、原始采样、实例／帧号、模块哈希与恢复记录。

正式安装 DLL/PDB、启动脚本、RA2MD.INI 均恢复，最终 SHA256 与备份一致；临时地图移除，
自有游戏和采样进程均退出，WPR 未在录制。二进制、ETL、游戏资产及 Token 不入库。
