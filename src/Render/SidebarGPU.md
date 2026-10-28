# Sidebar 静态背景 GPU 接入

本阶段在纯黑副窗口上绘制 CREDITS、SIDE1、重复 SIDE2、SIDE3、ADDON、TOP，最后绘制独立光标。按钮、cameo、文字、电力条、雷达内容、命令栏及可选叠层不在本阶段范围内。主窗口继续原生绘制；没有复制 Sidebar/Composite/Primary 像素。

依据：gamemd-reverse `d00548a` 的 `docs/zh-CN/sidebar-gpu-handoff.md`、`SidebarDrawing.cpp`、`CopyToSurfaceRLE.cpp`、`RLEBlitTransXlat.cpp`。对应调查：https://github.com/hzqst/gamemd-reverse/issues/6 。

## 数据与生命周期

- `6A70AE`（Sidebar::Draw 调用 BlitSidebar 前）读取已加载原始素材。此时原生背景、控件、Power/Radar 绘制已经执行；每次重新复制所选帧和实际 SIDEBAR Convert 的 256 项 16-bit 映射。不会触发 SHP 加载，也不会使用共享 GetData scratch buffer。
- `SidebarSnapshot` 只保有自有像素、位置和诊断用数值身份。GPU 定时呈现不解引用素材指针。`6A5840`、`6A5BF0`、`6BE1C0`、已有视频模式入口/完成点使快照失效。退出对局或 sidebar 不活跃时不绘制旧快照。
- `ShapeImage` 处理 plain 和按行 RLE。RLE 的 WORD 行长包含自身，非零字节为索引，`0,run` 表示透明 run，允许 run=0；校验行边界和输出宽度。索引 0 透明，非零索引映射到黑色仍是不透明。
- 文件头、帧表、内存可读性、下一帧偏移和有界编码长度均检查；引擎裸 SHP 指针不提供完整分配/文件长度，末帧只能验证可读内存与格式界限，不能声称具备完整文件长度校验。首版限制单帧最大 2048×2048、sidebar surface 最大 4096×4096、重复段最多 128。
- 原布局由 SidebarBounds、ViewBounds、SidebarMode、当前 side、B0B4F8、SIDE2/SIDE3 原始头高度及 TOP 的对象坐标计算。SidebarMode 决定横向偏移，不作为可见开关。先裁剪到 sidebar surface，再变换到客户区逻辑画布并裁剪；没有加桌面坐标。
- 六项资源全部解析成功后才发布完整快照，未就绪时保留黑底和光标。可选叠层 `+5398` 仅诊断，不代表该模式已完整兼容。
- 六个独立 R32_UINT texture，内容变化才上传；重复 SIDE2 复用同一 texture。在单个 render pass 中按层绘制，最后是光标。CPU 资源解码与复制仍有成本，尚未做性能优化或停用原生 UI 绘制。

## 采样与验收

启动前设置 `PHOBOS_SIDEBAR_TRACE=1`，输出到现有 debug.log，前缀 `[SidebarGPU]`，单进程最多 64 条记录，不需要 F8。记录的是 post-draw 快照和由静态公式计算的布局，并非各原始 draw call 的逐次实参追踪。资源名表示对应全局槽位的约定身份，不证明最终命中的 MIX/loose 文件。

记录包含资源对象/data 身份、帧 0、压缩 flags、帧偏移/尺寸、原始头高度、palette 映射 hash、side、重复数、目标原点/尺寸、TOP 坐标、代际和可选叠层状态。实际 archive、原 PAL 768 字节、可选叠层素材身份仍未采样。

2026-10-28：Debug x86 构建（0 errors / 0 warnings）、ShapeImage plain/RLE/异常输入/布局测试、既有 CursorImage 透明与裁剪测试通过。用户已完成一局 D3D12 遭遇战画面对照及 Alt-Tab，反馈“除了按钮和建造图标没有之外，一切正常”；缺少这些元素符合本阶段范围。尚未验证所有侧别、分辨率、显隐组合、资源重载及 Vulkan。未修改 Issue #6 的状态；静态调查列出的其他动态分支与可选叠层仍待补证。

本轮 PID 16700 的日志：side=0、generation=5、ready=1、originX=856、sidebar=168×768、Y0=227、SIDE2 重复 10 次、TOP=(0,16)、overlay=0。六项均为 frame=0 / flags=0 / frame offset=(0,0)，对应尺寸依次为 168×16、168×69、168×50、168×26、168×63、168×32。palette 映射 hash=`F2B75DB967CFCDC9`。本场实际素材没有 RLE；RLE 支持目前只有定向测试证据，不能冒充实素材验收。

日志及构建/钩子核验脚本保存于 `D:/Ra2/SidebarGPU-evidence-20261028-16700/`。采样只出现一组稳定资源/布局记录，共 7 条；非逐帧日志。

## Hook 核验

使用 check-hooks skill 的 discover_hooks.py 和 check_hook_conflicts.py；自动发现 4 个新增钩子，补充了函数体改变的已有 560BF0 钩子。参考表和当前源码重叠检查均无冲突。

| Hook | 地址/长度 | P0 长度 | P1 冲突 | P2 边界 | P3 参数 | P4 相对指令 |
| --- | --- | --- | --- | --- | --- | --- |
| SidebarClass_Draw_GPURenderSnapshot | 6A70AE/5 | 通过 | 通过 | IDA 跳过 | IDA 跳过 | IDA 跳过 |
| SidebarClass_InitForHouse_GPURenderInvalidate | 6A5840/6 | 通过 | 通过 | IDA 跳过 | IDA 跳过 | IDA 跳过 |
| SidebarClass_UnloadShapes_GPURenderInvalidate | 6A5BF0/7 | 通过 | 通过 | IDA 跳过 | IDA 跳过 | IDA 跳过 |
| Scenario_Clear_GPURenderInvalidate | 6BE1C0/8 | 通过 | 通过 | IDA 跳过 | IDA 跳过 | IDA 跳过 |
| SetVideoMode_CursorTraceInvalidate | 560BF0/5 | 通过 | 通过 | IDA 跳过 | IDA 跳过 | IDA 跳过 |

IDA idb_list 无会话，server_health 返回 Session not found，故专项 P2–P4 如实跳过。补充 Capstone 对本机 retail/spawn EXE 核验：所有窗口字节一致，覆盖完整指令；新回调不提取参数、不写寄存器、均 return 0。6A70AE 覆盖 `call 6A70E0`，6A5BF2 覆盖 `call 63F7E0`，由 SyringeEx 重定位；其余窗口没有相对控制流指令。这不替代 IDA 的类型/栈专项核验。

## 本机启动与部署

`D:/Ra2/Launch-Phobos.ps1` 原先主动 RunAs。Syringe manifest 为 asInvoker，未发现该游戏/注入器的强制提权兼容性设置，当前会话为 Medium integrity；用户确认直接普通权限启动正常。因此移除脚本强制提权，保留工作目录、参数及 DetachDebugger 行为，旧脚本在游戏目录备份。机器专用脚本不入仓库。

已部署 DLL SHA256：`3464C850971E8E27B1DF30A53869412B83235D27DCB3A15BF9CB92D3646F5237`。上一版 DLL/PDB 备份：`D:/Ra2/Phobos-backup-sidebar-20261028-120605/`。
