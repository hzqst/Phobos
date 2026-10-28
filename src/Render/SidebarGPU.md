# Sidebar 原始素材 GPU 接入

缓存设计见 [FrameAtlasDesign.md](FrameAtlasDesign.md)，阶段1实现与验证见 [FrameCache.md](FrameCache.md)。当前已接入FrameID多页索引atlas与独立GPU调色板，实现与验收状态见 [FrameAtlas.md](FrameAtlas.md)。下方按钮和背景章节保留此前阶段记录。

## 按钮接入（2026-10-28）

依据 gamemd-reverse `f7476d9` 的 `docs/zh-CN/sidebar-buttons-gpu-handoff.md` 和 `SidebarButtons.cpp`，追加十个按钮：卖出、维修、四页签、下/上滚动、简报/外交、选项。以下背景章节保留上一阶段记录。

- 仍在 `6A70AE` 采集，读取全局按钮对象的持久状态，不调用 Draw/Action/AI、不修改输入、捕获或 dirty 位。`ButtonImage::SelectFrame` 按 Disabled → UseFlash/IsToFlash/IsOn → IsPressed 优先级选帧；没有独立 hover 帧，也不根据滚动端点推导 Disabled。
- 从实际 ShapeData 读取所选帧，越界或未加载时跳过该按钮；沿用 plain/RLE 解码和索引0透明规则。颜色来自各按钮实际 Drawer 的256项16位映射。当前支持 UseSidebarSurface=true、IsAlpha=false；其他目标/alpha blitter 不假装按普通透明效果绘制，而是跳过并记录 supported=0。
- 坐标取 X/Y + DrawPosition（32位加法），加所选帧 Left/Top；按完整 Sidebar 目标裁剪，再加客户区逻辑 originX。交互 Width/Height 不作图像裁剪。共享类型大小及关键字段偏移有编译期断言，未修改 YRpp。
- 背景/按钮共16个纹理槽，内容变化才上传，缺失按钮不影响其他层。顺序为 credits/四片背景 → 下部八按钮 → TOP → Radar二按钮 → 维修/卖出后层 → 光标。后层采用原版 Radar 重画时的维修/卖出顺序；可选叠层、模组超大图片重叠及原生缓存历史覆盖仍未验收，不宣称兼容这些分支。
- 初始化/资源清理使用就绪门控：`6A5840` 取消两组按钮就绪，`6A5BDB` 标记下部资源完成，`6530E7` 标记Radar二按钮绑定完成；真正复制仍等到完整Sidebar Draw之后。`652D90` 在Radar释放前取消其就绪，避免+58残留别名；`730100/730400`、`6A5BF0`、`6BE1C0` 取消所有资源就绪。视频模式变化只丢弃布局快照，下一次合法Draw重新读取。下部资源未就绪时也不读取背景裸指针。
- GPU present/timer只使用自有像素和绘制包；WM_PAINT未走完整Draw时沿用最新有效快照。没有新增渲染线程或SDL调用。CPU逐次解码尚未优化，原生CPU绘制仍保留。

`PHOBOS_SIDEBAR_TRACE=1` 同时启用 `[SidebarButtonGPU]`，按按钮状态/资源/图像变化记录，单进程独立上限64条，不需要F8。state顺序为 Disabled,UseFlash,IsToFlash,IsOn,IsPressed；记录所选帧、线程、代际、SHP/data/Drawer、映射hash、位置、帧尺寸/flags和完整clip。它是 `6A70AE` 派生快照日志，不是 `69DF96` 原生实参采样，也不能证明最终MIX来源或所有输入时序。

验证：Debug x86完整构建及最终增量检查成功；ButtonImage的32种状态组合、ShapeImage解码/异常输入/布局、CursorImage透明与裁剪测试通过。用户按按钮位置/颜色、状态切换和Alt-Tab验收请求完成首轮D3D12遭遇战，反馈“已采集，一切正常”。其他侧别、observer、多分辨率、视频/资源重载及Vulkan仍未验收。

Hook验证使用 check-hooks 的 discover/conflicts 脚本。以下按发现顺序列出新增项，末尾补查已有改动及采样点：

| Hook | 地址/长度 | P0 | P1 | P2 | P3 | P4 |
| --- | --- | --- | --- | --- | --- | --- |
| SidebarClass_InitForHouse_GPURenderButtonsReady | 6A5BDB/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| RadarClass_InitIO_GPURenderButtonsReady | 6530E7/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| RadarClass_Dispose_GPURenderButtonsInvalidate | 652D90/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| UI_UnloadShapes_GPURenderInvalidate | 730100/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| UI_UnloadPalette_GPURenderInvalidate | 730400/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SidebarClass_InitForHouse_GPURenderInvalidate | 6A5840/6 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SidebarClass_UnloadShapes_GPURenderInvalidate | 6A5BF0/7 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| Scenario_Clear_GPURenderInvalidate | 6BE1C0/8 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |
| SidebarClass_Draw_GPURenderSnapshot | 6A70AE/5 | 通过 | 通过 | IDA跳过 | IDA跳过 | IDA跳过 |

IDA health返回Session not found，idb_list为空，故P2–P4跳过。参考冲突检查均无错误/叠加警告，新增窗口另查当前源码无重叠。补充Capstone验证：上述窗口在本机retail/spawn字节一致且覆盖完整指令，回调均return 0且不读写寄存器/栈参数；新增五处均为MOV。既有6A5BF2和6A70AE覆盖相对CALL，由SyringeEx重定位。该补证不冒充IDA专项检查。

本机模块SHA256：retail `7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6`；spawn `247f72881e1a68c8fc305e3702dc0100a72d17601305999026393c340d5daeb0`。与调查IDB输入整体哈希不同，本次只确认相关hook窗口，不宣称整个模块一致。

已部署到 D:/Ra2，DLL SHA256 `c2290346f6de1c1de339069c58bd4fead0aafe3828f6f6d02cc8e131bc466079` 与构建输出一致。旧 DLL/PDB 备份于 `D:/Ra2/Phobos-backup-buttons-20261028-130833/`。普通权限通过原 Launch-Phobos.ps1 启动，gamemd PID58912，D3D12初始化进入Running；已请求用户进入遭遇战验收，此处不将初始化成功视作按钮动态验收通过。

首轮日志确认side=0、originX=856、clip=168×768、generation=11、thread=22208，十按钮全部ready=1/supported=1；建筑页签记录到0/1/2/4，防御与步兵页签0/1/2，载具页签2，卖出/维修及顶部两按钮0，上下滚动2；所有采样帧均为plain。验收时保存的日志共51条按钮变化记录，没有ready=0或supported=0。用户画面对照正常，但日志未捕获卖出/维修锁定、顶部按下或可滚动状态等全部分支，不能把未记录分支称为机器采样覆盖。初始与验收日志（debug-accepted.log）、构建日志及hook补证脚本存于 `D:/Ra2/SidebarButtonsGPU-evidence-20261028-58912/`。

## 已验收的静态背景阶段

副窗口定位修正（按钮验收后）：原先只在创建时定位，分辨率变化只改尺寸，且单屏贴右边缘回退可能覆盖主窗口。现在默认在游戏右侧留64像素间距，消息循环检测实际游戏窗口矩形变化后重新定位，以覆盖wrapper延迟调整/居中；不主动抢焦点、不阻塞等待SDL窗口操作。右侧放不下时优先其他显示器，单屏允许超出右边界而不向左覆盖游戏。手动缩放策略保留；主窗口矩形不变时不反复覆盖用户拖动的位置。

此小改动Debug x86构建和diff检查通过，无新增hook；尚未重跑菜单→对局的目视验收。已部署 DLL SHA256 `762968f2872d923122e54f37c47708304543f6482417689046b82e7e5d9385f3`，备份 `D:/Ra2/Phobos-backup-window-position-20261028-131242/`，下次启动生效。窗口移动是异步请求，日志的requested position不作为实际位置已成功应用的证明。

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
