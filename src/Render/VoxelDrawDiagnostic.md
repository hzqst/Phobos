# UnitClass VXL 绘制负对照

`PHOBOS_DIAGNOSTIC_SKIP_UNIT_VXL_DRAW=1` 在进程首次读取时启用；其他值或未设置均保持原路径。
只用于测量移除指定绘制工作后的剩余成本，默认关闭。不能作为 GPU 渲染正确性或 GPU-only 吞吐量验收。

## 范围和所有权

- 在 `UnitClass::DrawAsVXL` 入口跳过整个函数：本体／炮塔姿态与缓存准备、scratch 绘制、最终软件合成、该函数内的 voxel 阴影及 scratch 清理均不执行。
- 跳过发生在临时全局 Surface 切换、对象临时绘制标志和身份作用域建立之前；不借用旧 scratch，不冻结旧 GPU 帧。原生与 GPU 画面都缺少这些 VXL。
- 不删除单位、不改 AI／寻路／输入规则、不伪造可见性。不接管原生对象遍历、选中标记、地形、SHP、UI、GPU 提交或呈现。计数和环境读取均在原游戏线程，环境只读一次，没有 GPU 同步或网络等待。
- 这是 **UnitClass 的 VXL** 入口，不包括 AircraftClass、VoxelAnim、建筑 voxel 附件或其他独立入口；当前 AMCV 夹具在范围内。DrawAsSHP 不受影响。其他单位类型尚未实机覆盖。
- 原函数包含渲染缓存与裁剪相关状态更新；负对照也会省掉这些。不得称结果为“纯游戏逻辑成本”，准确名称是“省去 UnitClass VXL 绘制后的剩余主线程／场景成本”。
- 开关独立于 GPU 和 Automation；测试结束需清除启动环境并重启进程。正常模式的 DirectDraw 行为不变，诊断模式的缺图是明确授权的实验行为。

## 计数与验收

`[VoxelDrawDiagnostic] enabled=1 scope=UnitClass.DrawAsVXL rendering=omitted not_gpu_only=1` 明示模式。
之后最多 16 个窗口，每个窗口 120 次原生 RenderLayers：`skipped` 是跳过次数，`nativeCompleted` 是原函数完成次数。
窗口是绘制次数，不是 simulation_frame。日志达到上限后仍继续屏蔽，不在每个单位打印日志。

同一 DLL 做开关 A/B：检查模块哈希、实例、对象总数／归属／存活／位置、逻辑帧推进、原生和 GPU 实际截图。
屏蔽组应有非零 skipped、零 nativeCompleted；VXL 原生与 GPU 绘制均消失。运动验证另检查实际位置／朝向变化，
不能以输入注入成功替代。省掉的成本还包含对应的 GPU 采集／实例提交，不是单独 CPU Draw 的精确计时。

## Hook 核验（2026-10-31）

直接分析实际 `D:/Ra2/gamemd.exe`，SHA256 `7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6`。
自动发现脚本返回空数组，因为只修改既有 Hook 函数体；按仓库规则补查以下三项。冲突脚本逐项无冲突。
IDA 健康检查确认当前模块为 gamemd.exe；没有借用 gamemd-spawn.exe 的指令核验。

| Hook | P0 长度 | P1 冲突 | P2 边界 | P3 寄存器／栈 | P4 相对指令 |
|---|---|---|---|---|---|
| 6D4664 Render_LayersEnd | 5，完整 | 无 | 6D4664→6D4669 | 保留 EBX 目标 Surface，原深度 -260 | 原有 `call 5FFFA0`，SyringeEx 重定位 |
| 73B470 IdentityBegin | 6，完整 | 无，包括新返回地址 | 73B470→73B476；73C5E6 完整 ret | 入口与跳转目标 spd 均 0；不读写参数／返回寄存器 | 无 |
| 73C5DC IdentityEnd | 10，完整 | 无 | 73C5DC→73C5E6 | 四个 pop + add esp,1B4h，保留原流程；原入口 ESP=当前+1C4h | 无 |

`73B470: 81 EC B4 01 00 00` 为 `sub esp,1B4h`，诊断分支在其执行前返回固定地址。
`73C5E6: C2 20 00` 为 `ret 20h`，直接用函数入口原返回地址并清理 32 字节参数。
YRpp 声明为 void，参数 Point2D(8) + RectangleStruct(16) + brightness(4) + tint(4)，没有返回值需求。
跳过路径不进入 IdentityBegin／IdentityEnd；普通路径完整保留。没有新增 Hook 宏、没有修改第三方源码。

验证采用 Release 构建、现有 Render 回归及实机负对照；简单环境开关不另建与实现同构的单元测试。
证据目录为 `Debug/AutomationTests/voxel-p4/`，游戏资产、生成二进制、Token 和临时驱动不入库。

## 实机结果

基于 `b95e768e` 加本次未提交改动，Release/Win32 构建及 Render 回归均退出 0；回归保留已有 YRpp 编译警告。
同一 DLL SHA256 为 `7f275f40bac56e22f1a01fa89e88552e91dead25884319bff79507f6b474e6d5`。
四轮均通过自带 `Launch-Phobos.cmd` 启动正式 gamemd.exe，D3D12 independent、2560×1440、GameSpeed=1。
没有使用 gamemd-spawn。FAST、SHADOW 均关闭，保留 P0 计时；只改变本诊断开关。

千 AMCV 静止场景在开头／结尾的暂停快照中均有 1000 个属于玩家、存活的单位，位置不变。
主线程时间由 Windows GetThreadTimes 的约两秒窗口累计，除以对应逻辑帧增量；不是逐帧 profiler，
也不是 GPU 时间。统一去掉最初两个 CPU 窗口；RenderLayers 平均统一去掉首个 120 捕获窗口。

| 组别 | 主线程 CPU ms／逻辑帧 | RenderLayers 平均 ms | 逻辑帧／秒 |
|---|---:|---:|---:|
| 开关关闭 | 40.966 | 24.223 | 21.80 |
| 屏蔽轮 1 | 18.649 | 0.563 | 41.00 |
| 屏蔽轮 2 | 18.080 | 0.537 | 45.83 |

屏蔽轮 1 最后两段有推进速率和 CPU 占用同步下降的波动；未删除这些样本，重复轮未再出现。
这些数值只证明此次场景省去该绘制工作后剩余约 18 ms 主线程成本，不是纯逻辑耗时或 GPU-only 上限。
开关关闭只有一轮，不声称严格因果的长期平均速度提升；移除绘制还同时减少了采集／GPU 实例工作。

两轮屏蔽分别记录 1,800,000 和 1,920,000 次跳过，所有窗口 nativeCompleted=0；unitScratch／targetRLE 调用均为 0。
读取已加载代码确认返回处仍为 C2 20 00。原生/GPU PNG 均解码检查：VXL 本体和阴影消失，地形/UI 保留；
战术区 ROI 的暗像素由基线约 126 万降为原生 0、GPU 30（光标），作为截图检查的辅助量化，非同帧逐像素比较。
另跑 100 AMCV 屏蔽运动验证：100 个单位持续存活，98 个位置变化、100 个朝向状态变化，逻辑帧 196→955。
该轮用 N/T 选择不可见单位和 X Scatter，验证逻辑仍工作；不是千单位同时运动的验收。

| 证据子目录（runs/） | PID | instance |
|---|---:|---|
| diagnostic-1000-baseline-static | 23572 | dfb3eb21b3ad67c6d2fbba89dabf4d49 |
| diagnostic-1000-omit-static | 38388 | e16a7e0fa008694a1850452bef32d854 |
| diagnostic-1000-omit-static-r2 | 32992 | 37892678f73276adca6754a30f21162e |
| diagnostic-100-omit-moving | 45596 | 7a5c4e0b0bfb3c80d60b549cd7d8978c |

每轮备份并恢复 DLL/PDB、启动脚本和 RA2MD.INI；最后再次核对正式文件 SHA256 与备份完全一致。
三张临时地图已移除，自有游戏进程已清理。IDA 临时工作文件移到证据目录，原有 i64 保留。
未验收菜单正常退出、GPU 关闭、Vulkan、其他载具／飞机／VoxelAnim、多单位战斗或长期稳定性。
