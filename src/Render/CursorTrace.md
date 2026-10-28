# 光标事件追踪：接入核验记录

日期：2026-10-28。依据 gamemd-reverse 的 `docs/zh-CN/cursor-gpu-handoff.md`。

本阶段只观察原生路径，不获取 MouseMutex、不改光标像素、不创建 GPU 光标，也不建立跨呈现的锁事务。`frame/depth` 只是同线程调用区间编号，不能证明背景有效或跨线程同步。现有 Primary 镜像保持原有路径。

## 使用

1. 构建并部署 Debug DLL 后，在启动游戏的进程环境中设置 `PHOBOS_CURSOR_TRACE=1`。例如在 PowerShell 中先执行 `$env:PHOBOS_CURSOR_TRACE = '1'`，再从该终端启动游戏或客户端；已运行的客户端不会继承新变量。
2. 保持 GPU 副窗口初始化成功，进入需要观察的场景后按 F8。使用现有 GPU 主线程计时器检测按键，不截断游戏原有按键处理。关闭 GPU 功能或初始化失败时没有该计时器，无法用 F8 采集。
3. 计时器首次观察到满 5 秒、缓冲达到 16384 条事件或再次按 F8 时停止采集。在游戏工作目录输出 `CursorTrace-<PID>-<序号>.csv`。消息派发停顿时，时间上限的检测会延后，容量上限仍有效。正常退出也尝试导出未结束的采集。
4. 不同场景分别采集：静止光标下的单位移动、快速往返移动、跨 sidebar、四角裁剪、tooltip、隐藏/恢复、Alt-Tab、分辨率切换。

固定缓冲约 2 MiB；探针中不分配内存、不写磁盘、不调用 Surface/SDL。多个线程通过 try-lock 竞争缓冲，竞争失败直接丢弃事件，计入 CSV 头的 `dropped`。停止后由主线程一次写文件，可能造成一次可见停顿；这段写盘时间不属于采集区间。进程崩溃或消息循环一直不派发计时器时不能保证文件落盘。

## 字段与判读

- `ticks`：QPC 时间戳，CSV 头含频率；`thread` 为实际 OS 线程 ID。
- `address` 与通用寄存器保存原始 hook 现场；不改寄存器、EFLAGS、原生栈或探针调用前的 LastError。
- `frame/depth`：DrawOnTop 和 DoBlit 的线程局部嵌套区间；鼠标线程通常为 0。开始/停止采集可能截断区间，丢事件的采集也不保证可以完整配对。
- `mode`：本次进程中**采集期间观察到**的 SetVideoMode 入口次数，不是完整的 Surface/WWMouse/SHP/palette generation。
- `object/target/source` 与 `b0/b1/b2` 是地址数值，仅用于关联观测；文件使用者不得解引用，也不得凭地址相等或非零认定背景有效。
- `dx/dy/dw/dh` 和 `sx/sy/sw/sh` 是复制**请求矩形**。没有在通用 copy 内部观测实际裁剪，因此不能据此启用背景修复。

| 地址 | 事件与额外字段 |
| --- | --- |
| 4F4583 / 4F459D | 常规呈现区间开始/结束 |
| 4F4780 / 4F4BA4 | DoBlit 开始/所有出口；开始时 source=EDX，flags=CL |
| 4F4B75 | Primary copy 调用前；target=ECX、source=ESI、目的请求矩形 ESP+48h、源矩形指针 [ESP]、flags=[ESP+8] 的低字节 |
| 4F4B7E | Primary copy 返回，EAX 低字节为 bool；在原镜像回调之前记录 |
| 7B90C0 | 离屏叠加入口；object=ECX、target=[ESP+4]、flags=[ESP+8] sidebar 参数 |
| 7B9182 / 7B9193 / 7B9299 | 离屏路径等待前 / wait 返回 / release 前；wait 返回使用完整 EAX。时间差只是探针间隔估计，包含插桩开销，且不代表全部递归锁持有时间 |
| 7B91FF | B1/B2 保存返回；AL 是 copy bool，target=EBX、source=EBP、源请求矩形=*ESI；在原生覆盖 AL 前读取 |
| 7B928A | 离屏源更新 B0 后，AL 是 copy bool；两条更新分支在此汇合；target=B0、source=EBP |
| 7B9022 | Primary 背景保存调用前；target=ECX、source=[ESP]、目的请求矩形 ESP+30h、源矩形指针 [ESP+4] |
| 7B93F0 | 实际绘制入口；object=ECX、target=[ESP+4]、flags=[ESP+8] 是 xoffset，不是 sidebar 标志 |
| 7BA20B / 7BA273 / 7BA282 | 鼠标位置更新中的恢复/保存/绘制完成观测；**不能将这些点的 AL 一律解释为 copy 成功** |
| 560BF0 | 模式切换开始 |

当前没有覆盖 Show/Hide/Capture 内联的全部背景复制，也没有实现 B0/B1 的成功保存代际及有效像素区域跟踪。这是明确的下一阶段门槛；本次日志只能验证调用交错、已覆盖复制的返回值和候选呈现区间。

## 二进制核验

实际文件 SHA-256：

- `D:/Ra2/gamemd.exe`：`7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6`
- `D:/Ra2/gamemd-spawn.exe`：`247f72881e1a68c8fc305e3702dc0100a72d17601305999026393c340d5daeb0`

两者的交接报告 25 个候选窗口均匹配报告字节。本次使用的 18 个窗口也在两文件中逐项比对并经 Capstone 解码：长度覆盖完整指令，没有相对调用/跳转。新增的 7B9182、7B9193、7B9299、7B928A、7B9022 同样完成两文件比对。已结合报告的函数反汇编核对参数；当前未证明所有扩展版本兼容。

另将报告中的 004f4480、004f4780、007b90c0、007b8f30、007b93f0、007ba090 六份函数反汇编与两个本机 exe 逐条比对，共 1950 条指令，字节差异为 0；这为所用调用约定和下游现场提供了超出短序言的证据。

特别处理：4F4BA4 覆盖 `pop esi; add esp,5Ch; ret 4`，回调必须返回 0 执行 trampoline，不能跳到窗口后面；4F4B7E 同样保留 `test/pop`。7B9022 时四个参数已入栈，目的矩形尚待 LEA/PUSH；4F4B75 时三个参数已入栈，目的矩形和 source 尚待入栈。两个函数均没有动态栈对齐，不使用 EBP 推导参数。

## Check Hooks 检查表

使用技能提供的 discover_hooks.py 和 check_hook_conflicts.py。P0=最小长度，P1=冲突，P2=IDA 指令边界，P3=IDA 参数，P4=IDA 相对指令。

IDA session 列表为空，server_health 无可用 database；按技能规则，P2–P4 全部跳过。本机 PE/Capstone 和报告核验是补充证据，不将其填作 IDA 通过。

| Hook | 地址 | P0 | P1 | P2 | P3 | P4 |
| --- | --- | --- | --- | --- | --- | --- |
| GScreenClass_DoBlit_GPURender | 4F4B7E | ✓ | ✓ | — | — | — |
| DrawOnTop_CursorTraceBegin | 4F4583 | ✓ | ⚠️ | — | — | — |
| DrawOnTop_CursorTraceEnd | 4F459D | ✓ | ✓ | — | — | — |
| DoBlit_CursorTraceBegin | 4F4780 | ✓ | ✓ | — | — | — |
| DoBlit_CursorTraceCopy | 4F4B75 | ✓ | ✓ | — | — | — |
| DoBlit_CursorTraceEnd | 4F4BA4 | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceStage | 7B90C0 | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceWait | 7B9182 | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceWaitResult | 7B9193 | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceStageSaved | 7B91FF | ✓ | ✓ | — | — | — |
| WWMouse_CursorTracePrimaryUpdated | 7B928A | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceRelease | 7B9299 | ✓ | ✓ | — | — | — |
| WWMouse_CursorTracePrimarySave | 7B9022 | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceDraw | 7B93F0 | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceRestored | 7BA20B | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceSaved | 7BA273 | ✓ | ✓ | — | — | — |
| WWMouse_CursorTraceDrawn | 7BA282 | ✓ | ✓ | — | — | — |
| SetVideoMode_CursorTraceInvalidate | 560BF0 | ✓ | ✓ | — | — | — |

全部发现：

- 脚本参考表对上述 18 个 hook 均报告无冲突，尺寸均至少 5 字节；无不足尺寸、部分覆盖或返回地址冲突。
- 对当前工作树补充搜索发现 4F4583 与 `src/Phobos.cpp` 的 `GScreenClass_DrawText` 完全重叠。保留有意叠加：两个 hook 均返回 0，观测不依赖两者先后，只要求发生在原生 staging 前。它仍作为叠加警告记录；运行时 hook 链尚待验证。
- P2–P4 共 18 行因 IDA 不可用跳过；未隐藏任何该项检查缺口。补充二进制解码未发现相对指令。

## 验证状态

- 最终 Debug x86 构建成功，构建日志无编译警告或错误。
- 独立 C++ harness 验证通过：寄存器/EFLAGS/LastError 保留、嵌套区间、竞争丢弃、复制参数提取、多线程写入及容量边界。
- 首次 harness 编译误用了 C++17；改为项目所需 C++20 和 NOMINMAX 后编译运行成功，未修改第三方头文件。
- 已部署并采集一轮遭遇战，见下节。仅证明本轮覆盖路径的观测结果；没有完成背景有效代际、全部生命周期和跨 sidebar 边界的验证。

可在仓库根目录的 x86 VS 开发终端运行 harness：

```bat
cl /nologo /std:c++20 /EHsc /DDEBUG /DNOMINMAX /DSYR_VER=2 /DHAS_EXCEPTIONS=0 /I src /I YRpp /I lib /I D:\vcpkg\installed\x86-windows\include tests\Render\CursorTraceTests.cpp /Fo%TEMP%\CursorTraceTests.obj /Fe%TEMP%\CursorTraceTests.exe /link user32.lib
%TEMP%\CursorTraceTests.exe
```

## 2026-10-28 对局采集

用户授权部署至 `D:/Ra2`，旧 DLL/PDB 备份于 `D:/Ra2/Phobos-backup-cursortrace-20261028-095748`。部署后 DLL/PDB SHA-256 与构建产物一致；DLL SHA-256 为 `50640c2f0c220e6bc7e3420dd83910de01f46720e34525c6c3eb4c5ac09413bb`。使用原 `Launch-Phobos.ps1` 管理员启动流程，临时外层脚本只为该进程设置追踪环境，未改原启动脚本或 INI。

用户进入遭遇战并按 F8 采集。游戏日志显示地图 `XMP30S6.MAP`、cnc-ddraw、D3D12 immediate；画面由 800×600 切到 1024×768，对局后回到 800×600，最后正常保存设置/关闭日志。没有终止游戏进程。

证据保存在 `D:/Ra2/CursorTrace-evidence-20261028-31356/`：原始 CSV、summary JSON、分析脚本、debug.log、syringe.log。原始文件是 `D:/Ra2/CursorTrace-31356-1.csv`。

| 项目 | 本轮观测 |
| --- | --- |
| 捕获 | 4.9909628 秒，6894 条，dropped=0，时间戳单调 |
| 主呈现线程 | TID 51392；DrawOnTop、DoBlit 开始/结束各 313 次，嵌套配对无遗漏 |
| 鼠标更新线程 | TID 55500；恢复/保存/绘制完成各 111 次 |
| Primary copy | 313 次请求/返回完整配对，AL 全部非零 |
| Primary 请求区域 | 所有请求均为 `(0,0,856,736)` → `(0,0,856,736)`，并非完整 1024×768 画面；这是请求范围，不是内部有效裁剪证明 |
| 离屏 staging | 626 次：sidebar=1 与 sidebar=0 各 313 次；sidebar 事件在本轮常规 DrawOnTop 区间之前，frame=0 |
| B1/B2 保存 | 主画布 313 次成功；sidebar 313 次失败。失败矩形右边界范围 -480..-344，完全在目标左侧之外，符合无交集裁剪情形 |
| B0 更新 | 对应主画布 313 次成功、sidebar 313 次失败；不能将非空 B0 认定为最新有效背景 |
| 背景对象地址 | 本轮稳定为 B0=33e2c400、B1=33e2c3d0、B2=33e2c3a0；地址稳定不证明有效像素范围或生命周期 |
| 离屏 wait 返回 | 626 次均为 WAIT_OBJECT_0，无 timeout/abandoned/failed |
| wait 探针间隔 | 中位 0.7 µs，P95 1.4 µs，最大 89.2 µs |
| 获取后至 release 前间隔 | 中位 4.4 µs，P95 8.9 µs，最大 137.6 µs；含探针开销，不是全部递归锁持有时间 |

在“主画布 staging release 到 Primary copy”这个窄区间内，本轮未观测到另一线程的鼠标恢复/绘制事件。**这不是不存在竞态的证明**，也没有验证延长锁范围后的安全性。

后续接入约束：必须按目标 Surface 区分 sidebar 与 Composite 背景，并使用真实复制成功标志；目前不能只包围 DrawOnTop 就宣称覆盖所有 staging。建议下轮重点采集光标实际进入 sidebar、跨界和 footer 的情况，再确定旁路处理。当前没有开启无光标修复或 GPU 光标，不将本轮记录解释为全场景验收。
