# Voxel 身份观察与旁路验证（P1/P2）

2026-10-31，基于 `4d616857`，接续 [P0 调查与基线](VoxelIdentityP0.md)。

## 当前行为

新增 `PHOBOS_VOXEL_IDENTITY_SHADOW=1`，默认关闭。只观察纯 VXL 车辆的实际缓存部件与最终 scratch 源图；**不使用身份缓存返回图像，不跳过原生绘制，也不减少现有 AcquirePixels 的复制／内容哈希**。P3 尚未开启。

进入 `UnitClass::DrawAsVXL` 时建立上下文，最终 scratch 清理后结束。沿用现有 `437A10` 采集入口，只将调用者为 `70773B` 的原生缓存部件纳入候选：读取临时 BSurface 的 payload 地址，然后立即 Unlock；不保存、解引用该临时 BSurface。所有引擎采集和验证均在游戏主线程执行。

候选身份包含原生 cache epoch、有序部件 payload、blitter、源／目标／裁剪矩形和 Z／gradient／brightness／tint。最终源区域另入 key，保留桥下分段与屏幕边缘裁剪；每次提交继续使用原来的最终颜色、深度与图层顺序。不同单位只要上述实际组合相同，就可以在旁路中命中同一身份，不依赖对象地址。

普通动态部件、非已知 RLE 调用者、SHP、被采集的线／填充／字形写入均使当前组合失效。嵌套作用域保守拒绝父子双方，最多 8 层／16 部件。开始和结束以入口 ESP 配对；栈不匹配或溢出使本次 RenderLayers 剩余候选失效，下次捕获重置。绘制中发生原生 cache purge 时，起始 epoch 与最终 epoch 不同，不能产生候选。

## 旁路 oracle 与边界

原有 AcquirePixels 成功后，将得到的 FrameID 交给旁路验证器。首次身份记录 ID；再次遇到时 Resolve 两个 ID，比较尺寸和完整索引像素。ID 失效就重新建立基线，不保存索引副本、GPU texture 或 UV。映射最多 512 项，满时整体清空再采样；一次像素不一致会禁用整个当前 epoch，不能通过淘汰条目掩盖失败。下一个原生 cache epoch 才重新开始。

每帧最多检查 256 次符合条件的最终提交，每 120 次 RenderLayers 输出一行 `[VoxelP2]`，最多 16 窗。日志字段：

- `scopes/parts/unknown`：观察的 VXL 作用域、已知缓存部件、未知 scratch 写入。
- `finals/ineligible/budget`：识别的最终提交、无有效候选／图像、超出本帧检查预算。
- `seeded/equal/mismatch/missing/disabled`：建基线、全像素一致、不一致、FrameID 不存在、该 epoch 已被禁用。
- `scopeErrors`：累计作用域异常；`entries/epoch`：当前映射规模与原生缓存代次。

这里验证的是候选假设，不是任意 mod／Hook 的完整写入追踪。原生函数在末尾按 BlitterInstance 区域清 scratch；未另行扫描整张 scratch 证明首次初始值，未知直接内存写入也不一定经过现有记录入口。P3 必须明确干净初态、全部受支持写入路径、资源失效和对应实机场景，不能仅因一次 `mismatch=0` 就放行所有单位。SHP+VXL 混合路径、飞机、动态禁缓存部件均未获得快路径资格。

## Hook 核验

使用 [Check Hooks](../../.agents/skills/check-hooks/SKILL.md) 的 `discover_hooks.py` 与 `check_hook_conflicts.py`。没有 HookAnalysis.txt，冲突脚本使用 Ares 3.0p1 清单；另复用技能的解析器扫描当前 Phobos 源码，两个 Hook 均返回 `errors=[]`，各有一条 `No conflicts detected`。静态清单不能代替所有外部 DLL 的实际加载链。

IDA 单独打开正式安装 EXE 的副本，module 为 `gamemd.exe`，SHA256 `7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6`；不以 spawn EXE 的历史结论代替本次字节检查。

| Hook | P0 长度 | P1 清单冲突 | P2 指令边界 | P3 寄存器／栈 | P4 相对指令 |
| --- | --- | --- | --- | --- | --- |
| `73B470 IdentityBegin` | 6，通过 | Ares／Phobos 无冲突 | `81 EC B4 01 00 00`，到 `73B476` | sub 前只读 ESP；无寄存器写回 | 无 |
| `73C5DC IdentityEnd` | 10，通过 | Ares／Phobos 无冲突 | `5F 5E 5D 5B 81 C4 B4 01 00 00`，到 `73C5E6` | 4 个保存寄存器与 1B4h 局部区；入口 ESP = 当前 ESP + 1C4h | 无 |

两处均 `return 0`，Syringe 执行被覆盖指令；没有新固定跳转地址。函数唯一 `ret 20h` 位于 `73C5E6`。Phobos 阴影替换返回 `73C5C9`，先执行原生 scratch 清理，再到新结束 Hook。原生 `73B55B` 使用全局 `B1D13C` 切换 Temp；缓存绘图在 `707736` 调用 `437A10`，返回 `70773B`。

补查现有 `4373B0/437A10` 的 7 字节入口边界：分别是 `sub esp,4Ch/6Ch` 与完整 `mov eax,[esp+...]`，Hook 取参仍在这些指令之前。现有 RecordCall/RecordLine 只增加未知写入通知，不改变原有寄存器／栈提取、返回地址或图像记录顺序。

BSurface vtable `7E2070` 的 Lock/Unlock 分别指向 `4115F0/411570`：前者增加 LockLevel 并返回已有 buffer 加坐标偏移，后者减少 LockLevel；没有素材加载或 scratch 生成调用。诊断的额外 Lock/Unlock 成对执行。

## 验证记录

- 定向测试先确认缺少实现时编译失败，再通过 VoxelIdentityTests：分段／源区域、稳定身份、未知写入、嵌套、容量溢出、epoch 变化、栈错配、完整像素比较、失效 FrameID、映射预算、mismatch 后 epoch 禁用。
- Render 回归脚本加入 VoxelIdentityTests；本次脚本退出 0，包含原有 CPU／FrameAtlas 生命周期／GPU 测试。日志中的既有编译警告不描述为零警告。
- Release/Win32 主工程构建退出 0。
- 两轮实机均使用用户指定的 `Launch-Phobos.cmd → D:/Ra2/Launch-Phobos.ps1 → gamemd.exe`，沿用本会话对临时备份、部署和恢复的明确授权。没有启动 gamemd-spawn.exe。
- 实测 DLL SHA256：`32a68b1961e7e516b8c8fc70fb33b4945e2c63449ce0eeffcad8f7061b605d31`；源码 `4d616857-dirty`。运行时采样两个新增入口均已安装 JMP，观测计数证明上下文配对实际执行；未逐条跟踪全部外部模块的 trampoline 链。
- 首轮 PID 36012、instance `d7e6a77bc417e3d3ce4dd3f1950560b0`：11 窗、1320 次提交，1318 次一致、1 次建基线、1 次回退；车辆未被成功选中，所以只计静态覆盖，不计运动验收。
- 第二轮等待逻辑帧至少 100 且 tactical active，再按已核对的 N 键选中并断言 `AMCV`，多次 X Scatter。PID 16612、instance `9a1253872446daf2838c7b8fdbbece6b`；D3D12、independent、1024×768，11 窗、1320 次提交，1254 次一致、34 次建基线、32 次回退，mismatch／scopeErrors／disabled 均为 0。最终保留 25 项，建基线次数包含 CPU 图像失效后的重建，不能等同于不同姿态数。
- 已解码并查看原生／GPU PNG，车辆位置与朝向发生变化，最终两图有对应车辆；顺序截图属于 best_effort，未声称严格同帧。只清理本次随机 Token 认证且核对路径的游戏进程；未验收菜单正常退出。
- 两轮测试后 D:/Ra2 的 DLL、PDB、启动脚本分别与当轮备份前 SHA256 完全一致，恢复 JSON 已保存。

尚未实机覆盖多部件炮塔／炮管、后坐、桥下分段、边缘裁剪、嵌套、arena purge、SHP+VXL、Vulkan 和 100/500/1000 单位。相关状态机测试不代替这些现场覆盖。当前没有性能收益结论；P3 开启前需补足支持范围的验收。

证据目录：`Debug/AutomationTests/voxel-p1/`，不提交游戏资产、二进制、Token 和临时测试驱动。
