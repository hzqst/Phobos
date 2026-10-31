# 光标事件追踪：接入核验记录

## 当前路线调整（2026-10-28）

用户已决定从纯黑背景逐项构建 GPU 画面：先独立 GPU 光标，再 sidebar，随后地图、对象和其余游戏元素。新路径消费原始资源与游戏状态，不将 CPU 已渲染的 Primary/Composite/Sidebar 作为底图；CPU 资源解码与上传仍可使用。

因此，下文去光标背景修复、跨呈现 MouseMutex 事务及补充 sidebar Primary-copy 探针的建议改为历史备选，**不再作为下一步任务**。当前优先实现黑底上的原始光标纹理绘制，验证帧、颜色、透明、热点、坐标、可见性及生命周期。Sidebar 从原始背景、按钮/cameo 到文字与动态状态逐项增加；不能以整张 CPU Sidebar 纹理上传作为迁移验收。

现有镜像保留为独立对照；主窗口继续原生绘制。已发现的线程交错不宣称已修复，但由于新路径没有软件光标底图，不再需要先把它从 Primary 像素中擦除。光标应始终在已加入的场景/UI 上方绘制，实施先后不代表层级先后。本次仅调整路线文档，没有修改配置或渲染代码。

## 黑底 + 独立 GPU 光标：实现与验证（2026-10-28）

新路径已实现为默认输出；设置进程环境变量 `PHOBOS_GPU_MIRROR=1` 才选择旧整屏镜像作诊断对照，启动后不动态切换。未修改 RA2MD.INI 的格式或默认开关。

- `CursorSnapshot.h` 在主线程尝试以零等待获取 MouseMutex，只复制当前 WWMouse 的已加载资源、帧、热点、实际绘制位置和调色板结果，不调用原生绘制器或任何 Surface Lock/Copy。释放锁后才执行 GPU API。超时/失败不读取引擎对象；abandoned 时释放已获得的所有权并隐藏光标。
- SHP reference 仅解析已经 Loaded 的 Data，不主动加载。检查帧索引、尺寸、压缩标志、偏移与后续帧偏移边界；没有通用 SHP 解压器或完整文件长度验证器。首版只支持未压缩索引帧，尺寸不超过 256×256。
- 原始索引 0 转为透明，非零索引转为当前 MOUSE_PAL 的 16 位值加独立 opaque 标记；黑色不因颜色值为 0 而消失。实际绘制位置减整图热点再加帧 bounds，避免把紧裁帧中心当热点。
- 自有像素在锁内复制，锁外比较与上一上传内容；仅图像/尺寸变化才上传，单纯移动复用纹理。没有跨帧保留可解引用的引擎资源指针，资源或 palette 地址复用不会造成仅按指针命中的旧缓存。
- `R32_UINT` 纹理使用低 16 位颜色和第 16 位透明标记；复用并扩展整数解码 shader 与绘制 pass。每帧清黑，先在逻辑画布裁剪，再使用等比 viewport 绘制对应 texel 区域。旧镜像仍用 R16_UINT 与不透明解码。
- 当前没有新增游戏 hook，也没有新增线程。光标不可用、未 captured、RefCount 非零、失焦或快照失败时不绘制旧光标，不自动补 CPU 画面。尚未迁移 sidebar/地图。

验证记录：

- `CursorImageTests.cpp` 先运行失败的裁剪实现，再完成实现并通过测试：透明索引、非透明黑色、完整区域、左上负坐标、右下裁剪、完全出界、无效尺寸及 INT_MIN/INT_MAX。
- Debug x86 构建通过，最终日志无编译警告/错误；HLSL 的 DXIL 和 SPIR-V 均已重新编译。编译中曾因地址引用宏放在块作用域失败，移到命名空间后修正。
- 用户已在 D:/Ra2 对局中确认光标出现、移动/动画、跨 sidebar 及 Alt-Tab 隐藏/恢复正常。实际后端 D3D12；Vulkan、所有分辨率/缩放组合和异常资源尚未做运行验收。
- 日志确认 `Output: black background + GPU cursor` 及首帧独立光标纹理 22×17。证据保存于 `D:/Ra2/BlackCursor-evidence-20261028/`。部署 DLL SHA-256：`bc8e1bd2eaf1e96d6f6f2049d38ce1c6932af7b56e72e89bd81b88aa1288b641`。
- 部署前 DLL/PDB 备份位于 `D:/Ra2/Phobos-backup-blackcursor-20261028-104629/`。原生主窗口继续绘制，不能将副窗口迁移解释为已经省去 CPU 场景渲染开销。

新测试可在 x86 VS 开发终端运行：

```bat
cl /nologo /std:c++20 /EHsc /DNOMINMAX /I src tests\Render\CursorImageTests.cpp /Fo%TEMP%\CursorImageTests.obj /Fe%TEMP%\CursorImageTests.exe
%TEMP%\CursorImageTests.exe
```

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
| 离屏 staging | 626 次：sidebar=1 与 sidebar=0 各 313 次；sidebar 事件在 4F4583 开始的晚段追踪区间之前，frame=0；并非在整个 DrawOnTop 函数之外 |
| B1/B2 保存 | 主画布 313 次成功；sidebar 313 次失败。失败矩形右边界范围 -480..-344，完全在目标左侧之外，符合无交集裁剪情形 |
| B0 更新 | 对应主画布 313 次成功、sidebar 313 次失败；不能将非空 B0 认定为最新有效背景 |
| 背景对象地址 | 本轮稳定为 B0=33e2c400、B1=33e2c3d0、B2=33e2c3a0；地址稳定不证明有效像素范围或生命周期 |
| 离屏 wait 返回 | 626 次均为 WAIT_OBJECT_0，无 timeout/abandoned/failed |
| wait 探针间隔 | 中位 0.7 µs，P95 1.4 µs，最大 89.2 µs |
| 获取后至 release 前间隔 | 中位 4.4 µs，P95 8.9 µs，最大 137.6 µs；含探针开销，不是全部递归锁持有时间 |

在“主画布 staging release 到 Primary copy”这个窄区间内，本轮未观测到另一线程的鼠标恢复/绘制事件。**这不是不存在竞态的证明**，也没有验证延长锁范围后的安全性。

后续接入约束：必须按目标 Surface 区分 sidebar 与 Composite 背景，并使用真实复制成功标志；目前不能只包围 4F4583–4F459D 晚段就宣称覆盖所有 staging。建议下轮重点采集光标实际进入 sidebar、跨界和 footer 的情况，再确定旁路处理。当前没有开启无光标修复或 GPU 光标，不将本轮记录解释为全场景验收。

## 2026-10-28 跨 sidebar 边界采集

沿用已部署 DLL，用户在主游戏窗口按 F8 并跨 sidebar 边界移动。地图 XMP30S6.MAP，1024×768，D3D12。证据目录：`D:/Ra2/CursorTrace-evidence-20261028-45208-sidebar/`，包含 `CursorTrace-45208-1.csv`、summary/sidebar JSON、两份分析脚本及游戏日志。

采集 4.9896802 秒、14413 条事件、0 丢弃。主线程 TID=38260，鼠标路径线程 TID=53264。312 组 DrawOnTop/DoBlit 开始结束完全配对，312 次 Primary copy 全部成功。鼠标线程记录了 1625 组恢复、保存、绘制完成事件；这是本轮实际观测频率，不沿用静态报告中的 16ms 周期推断。

| 离屏来源 | B1/B2 保存成功 | B1/B2 保存失败 | B0 更新成功 | B0 更新失败 |
| --- | --- | --- | --- | --- |
| Composite（sidebar=0） | 211 | 101 | 211 | 101 |
| Sidebar（sidebar=1） | 108 | 204 | 119 | 193 |

有 7 对请求矩形确实横跨逻辑 x=856 分界。例如第 127 帧：sidebar 请求 `(-40,227,46,34)`，Composite 请求 `(816,227,46,34)`；二者背景保存均成功。两边分别保存同一光标矩形的不同部分，不能只保留其中一个背景。

另有 11 次 sidebar 的 B2 保存返回 false、后续 B0 更新返回 true。例见 CSV 7386–7387 行，B2 源请求为 `(-45,216,41,31)`。这两个 copy 的矩形/偏移不同，bool 只能说明各自调用结果，不能用 B0 成功推断 B2 有效或当前光标区域完整有效；现有探针没有记录 B0 更新的内部裁剪范围，不能把这一差异直接定性成引擎 bug。

### 捕获到真实线程交错

第 191 帧的原始证据（CSV 行号）：

1. 8697：主线程到达离屏 staging 的 release 前探针，QPC=1737587753493。
2. 8698–8702：另一线程执行恢复背景 → 保存 Primary 背景 → 绘制新光标。保存请求位置为 `(981,139,22,17)`；此前主画布 staging 请求位置为 `(979,139,22,17)`。
3. 8703–8705：主线程随后进入 DoBlit，在 8704 行开始 Primary copy，请求区域 `(0,0,856,736)`，QPC=1737587753678；copy 返回成功。

两探针间隔 18.5µs。实际日志证实原生 staging 与最终 copy 之间允许鼠标位置变化，因此只在 copy 返回后获取 MouseMutex 无法保证联合快照一致。该帧光标位于 sidebar，记录本身**没有证明出现视觉损坏**，也没有验证新增外层事务后的锁序安全。

### 其他观测与下一步

- 624 次离屏 WaitForSingleObject 均返回 WAIT_OBJECT_0。等待探针间隔中位 0.7µs、P95 1.4µs、最大 464.4µs；获取后至 release 前间隔中位 3.2µs、P95 8.6µs、最大 44.9µs。均含插桩开销。
- Primary copy 请求 299 次为 856×736，13 次为 856×768；底部保留区域随呈现状态变化，不能硬编码固定 footer 高度。
- Sidebar staging 位于当前 4F4583–4F459D 晚段追踪区间之外。后续静态定位确认它仍在整个 DrawOnTop 函数之内；参见下节。必须同时考虑 WM_PAINT 等旁路，不能从 frame=0 推导为另一帧或另一线程。
- 在开启 clean_frame 前，需分别跟踪 B0/B1/B2 保存成功、原始矩形、有效像素范围及资源代际。复杂或无法配对的路径继续整包镜像回退。

本次只采集与分析，没有改动渲染代码、开启背景修复或独立 GPU 光标。

## Sidebar 最终写入 Primary 的路径（2026-10-28）

### 定位结论与证据

入口为 `SidebarClass::BlitSidebar(bool force)`，地址 **6A70E0**；名称由 `gamemd-reverse/YRpp-phobos-dev/SidebarClass.h:184` 提供，行为由本机 exe 反汇编核对。它直接调用 Primary 的虚表 +08，**不经过 GScreenClass::DoBlit**。当前 Primary 所用 DSurface 虚表 `7E85D4` 的 +08 指向 `DSurface::CopyFromPart@4BB080`，后者继续调用 +0C 的扩展复制实现。

本次使用 PE/Capstone 静态调查，没有新增 hook 或运行另一轮采集。IDA 没有已打开会话，代码索引还在构建；知识库未找到此路径。反汇编留在 `D:/Ra2/SidebarPrimary-evidence-20261028/`（sidebar-draw/tab/wndproc/surface-copy.asm）。本机 retail/spawn 的以下字节范围逐一相等，两个文件 SHA-256 与上文核验记录一致：

- Sidebar Draw `[6A6C30,6A70D1)`，1185 字节。
- BlitSidebar `[6A70E0,6A7498)`，952 字节。
- Tab Draw 检查范围 `[6D0A20,6D0E48)`，1064 字节。
- DrawOnTop 检查范围 `[4F4480,4F45A9)`，297 字节。
- WndProc 检查范围 `[7775C0,777A7F)`，1215 字节。
- CopyFromPart `[4BB080,4BB0CB)`，75 字节。

### 常规帧顺序：Sidebar 先写，Composite 后写

```text
GScreenClass::DrawOnTop @4F4480
  4F4502: call [screen.vtable+40]
    MouseClass 主虚表 7E1964 的 +40 = TabClass::Draw @6D0A20
      6D0E38: SidebarClass::Draw @6A6C30
        6A70AE: BlitSidebar(force) @6A70E0
          6A71C7 或 6A7252: WWMouse +3C(Sidebar, 1)，保存/绘制离屏光标
          五个 CopyFromPart 点之一或两个：Sidebar -> Primary
          6A7481: B0B519 = 1
  4F453D: 若 B0B519 且非 Armageddon，WWMouse +40(Sidebar, 1) 恢复离屏背景
  4F4540: B0B519 = 0
  4F4583: 当前追踪的 Begin（不是 DrawOnTop 函数入口）
  4F4593: WWMouse +3C(Composite, 0)
  4F459A: screen.vtable+44 -> 4F45B0 -> DoBlit @4F4780
  4F4B7B: Composite -> Primary，4F4B7E 是现有捕获点
  4F459D: 当前追踪的 End
```

相关已恢复源码：`GScreenDrawing.cpp:24,29,47` 与 `TabDrawing.cpp:164`。**修正旧结论：frame=0 的 sidebar 事件位于晚段探针之前，但仍嵌套在 DrawOnTop 的虚拟 Draw 里。**

### 五个直接复制点

所有调用均为 `Primary->CopyFromPart(destination, Sidebar, source, false, true)`。下表描述的是调用请求，不是经过底层裁剪后的有效区域。

| call 地址 | 请求分支 | 返回点 |
| --- | --- | --- |
| 6A722C | 只更新顶部：源 `(0,0,SidebarBounds.Width,16)` | 6A722F |
| 6A7312 | 更新顶部 16 像素，随后继续下部复制 | 6A7315 |
| 6A739D | 下部：源从 `SidebarBounds.Y` 开始；目的高度来自 Sidebar surface height 减该 Y | 6A73A0 |
| 6A742E | 全量：取 Sidebar 的 GetRect，源起点设为 `(0,0)` | 6A7431 |
| 6A747E | 局部矩形：源为全局 Rectangle `8809F4`，尺寸与位置用于构造目的矩形 | 6A7481 |

本函数先以 GetClientRect/ClientToScreen 取得客户区屏幕原点；目的 x 加上 `SidebarMode ? ViewBounds.Width : 0`，部分分支再加源矩形位置。不可硬编码右侧 x=856 或把这些屏幕坐标直接当逻辑 Primary 坐标。包装器的坐标语义仍需动态核实。

前置条件包括 `self+53A5`（IsSidebarActive）、Game::IsActive@A8E9A0、ScenarioStarted@A8ED5C。分支还检查 `B0B518`、force、矩形 `8809F4` 与 `B07DC8` 的比较及 `self+53A8`。每次可执行零次、一次或两次 Primary copy。

关键限制：`6A7481` 无条件设置 B0B519=1，原路径不检查 copy 的 AL。它表示进入了相应呈现路径，**不是复制成功证明**。该函数也没有建立跨 staging 和 copy 的外层 MouseMutex。

### WM_PAINT 旁路：顺序相反

从 WndProc@7775C0 的消息表核对：消息 0x0F 映射到 7776B5。满足对局/显示条件时：

```text
777717: DoBlit(captured, Composite, nullptr)
777723: BlitSidebar(true)
77772A: ValidateRect
```

因此 WM_PAINT 的 4F4B7E 捕获发生在 sidebar 更新之前。只有后续 timer 或其他捕获才可能看到这次 sidebar 写入；不能将 4F4B7E 泛化为所有路径的整帧结束点。对 .text 的 E8 目标扫描找到 BlitSidebar 的两个直接调用点 6A70AE、777723，并逐点反汇编确认；该扫描不排除间接调用或扩展 DLL 的调用。

### 接入建议与验证边界

1. 下一步先观测 BlitSidebar 入口、两个 staging 分支、五个 copy 请求/返回及公共退出 6A7488，并关联 WM_PAINT。记录每次实际调用，而不是假定 sidebar 一帧只复制一次。
2. 五个 copy 前都有完整 5 字节 `lea; push` 窗口：6A7227、6A730D、6A7398、6A7429、6A7479。此时 ECX=Primary，`[ESP]`=Sidebar，`[ESP+4]`=源矩形指针，`[ESP+8/+C]`=两个标志；目的矩形的 LEA 偏移依次为 38h、28h、28h、28h、38h。这些只是候选点，尚未完成扩展 hook 冲突及内部入边检查，不能直接安装。
3. 不要直接在返回点 6A7431 安装普通 5 字节跳转：那里是两字节短跳，后面的 6A7433 是另一条合法分支入口，覆盖会破坏它。6A7481 又是多个分支汇合点，不能仅凭到达它判断具体哪个 copy 完成。返回值采集应选择经过验证的调用包装/替代点。
4. 常规帧若要做单一事务，起点至少要早于 4F4502 的虚拟 Draw，并持续到最终 Composite copy；这会把 sidebar、部分战术渲染与 UI 都包进锁内，不能未经性能与锁序验证直接采用。更短的分段事务则需要显式维护多次 Primary 写入及干净底图状态，不能仅给现有晚段再加一份 B2。
5. WM_PAINT 需要独立的整包完成边界；首版未覆盖时继续原镜像回退。当前尚未动态观测这五个具体调用点、有效裁剪区域或新增锁范围的安全性。
