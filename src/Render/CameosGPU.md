# Sidebar 基础 cameo GPU 接入

后续更新：用户已澄清“Countdown 百分比”指GCLOCK2扇形遮罩，不是数字文字。该遮罩及第一批状态效果已接入，用户确认显示正常；详见 [CameoEffectsGPU.md](CameoEffectsGPU.md)。下文保留基础阶段范围与当时记录。

2026-10-28，依据 gamemd-reverse `194cd7f` 的 `docs/zh-CN/sidebar-cameos-gpu-handoff.md`。基础图标已实现，用户首轮反馈“已采集。图标正常显示（上面的 Countdown 百分比没有显示）”。Countdown 百分比属于尚未实现的文字／状态层，不计入本轮基础图标范围。

## 数据与绘制

- 沿用 `6A70AE` 完整 Sidebar Draw 后的主线程采集；不读取 Sidebar/Composite/Primary 的成品像素，不额外调用 Draw、AI、FindFactory、CanBuild 或素材加载。
- 每次读取实际活动页的完整可见列表，不以 NeedsRedraw 或生产状态决定基础图标是否存在。页签、滚动、空槽只改变引用；WM_PAINT/timer 使用最近的有效快照。场景和资源清理沿用已有失效通知。
- 普通类型按列表 `{ItemType, ItemIndex}` 验证类型数组索引。仅接受已核实的 `GetCameo @712040` 虚表槽，调用其只读选择逻辑，保留当前渗透／老兵规则；未知虚表覆盖不猜测。超武读取 SidebarImage。未加载 SHPReference 不主动加载。
- SHP frame 0 使用现有 ShapeFrameSource/FrameCache 和 CAMEO Convert 的当前16位映射，发布 FrameID、PaletteID、坐标与裁剪。GPU 不解引用引擎素材。首次出现才解码／上传；切页返回、布局改变可复用缓存。
- 可见行数复用原版 RepeatCount；滚动追加一行，每页最多60个可见槽。TopRowIndex、IsScrolling、Slid 同次读取。支持负帧 Left/Top；原版 clip 从 surface 原点到 `(x+60,y+48)`，不是以格子左上角裁剪。先按该 clip 与 surface 求交，再加 SidebarMode origin，最后沿用画布裁剪。
- 顺序为背景／下部按钮 → 基础 cameo → TOP／雷达按钮 → 维修卖出后层 → GPU 光标。可选 sidebar 叠层、电力、雷达内容仍未接入；超大 mod 图片与缺失层重叠不作完整兼容保证。
- 帧槽扩展为16个既有资产加60个可见 cameo；未就绪／不支持的项无 placement，不保留上一项的图标。

## 本机 Ares 补证与边界

静态交接的原版内嵌 `Strip.Cameos[75]` 不是本机 Ares 的实际列表。本批只读分析了安装的 Ares DLL，未修改第三方源码或 DLL。

| 证据 | 实际内容 |
| --- | --- |
| Ares SHA256 | `4cbe28df7f5e8eb4cb6383db6209b8d4537aa98518ea9ee25a0f0408fffca93a` |
| PE timestamp | `61DAA114`，Phobos 识别为 Ares 3.0p1 |
| GetCameo1 export RVA `2CF90` | 读取 `base+C1154+tab*18h` 的 Items；项步长34h |
| InitCameosList 的 helper RVA `2C7C0` | 四个24字节 vector，Count 位于 vector+10h；列表基址 `base+C1150` |
| GetCameo export RVA `5F270` | 调用 CameoIsVeteran，然后返回类型 Cameo/AltCameo；Phobos 已将其判断函数转接至自身只读实现 |
| TechnoType_PCX RVA `5F220` | 类型+2FCh 的扩展内，PCX表面在+228h/+24Ch |
| Main RVA `5F180` | 类型扩展+120h 为 cameo 自定义 Convert；为空时用 CAMEO |
| SW RVA `5F1E0/5F200` | 超武扩展+494h 为 Convert、+4D0h 为 PCX表面 |

适配以 `AresBaseAddress`、版本及 GetCameo1/Main 导出地址核对为门控；`AresDllHmodule` 在当前初始化实现里未赋值，不能用来判断 Ares 是否存在。无 Ares 时读原版数组，count限定0..75；已识别3.0p1时读动态 vector，检查 Count/Capacity 并设4096项防御上限。未知版本拒绝 cameo provider，不回退到已被替代的内嵌数组。

PCX与自定义调色板分支暂时标记不支持：类型扩展任一 PCX 绑定或自定义 Convert 非空即跳过；SW 检查相应字段；XXICON reference 配置 MissingCameo `.pcx` 时保守跳过，不调用 PCX.LoadFile。此策略可能跳过当前没有实际使用的备用PCX，也可能跳过缺失PCX回退；不声称已支持这些扩展来源。其他 Ares 版本、观察者统计、额外SW侧栏不在本批支持范围。

## 直接释放通知与 hook 核验

`TechnoType` 析构入口 `711AE0` 与 Ares 重叠，最终没有在那里新增 hook。改在两处 owned buffer 的直接 free 前通知 ShapeFrameSource：

| Hook（发现顺序） | 地址/大小 | P0 大小 | P1 冲突 | P2 指令 | P3 参数 | P4 相对指令 |
| --- | --- | --- | --- | --- | --- | --- |
| TechnoType_FreeCameo_GPUFrameCache (AGAIN) | 711B3D/6 | 通过 | 通过 | 通过 | 通过 | 信息：CALL重定位 |
| TechnoType_FreeCameo_GPUFrameCache | 711B16/6 | 通过 | 通过 | 通过 | 通过 | 信息：CALL重定位 |

使用 check-hooks discover/conflicts 脚本，参考Ares 3.0p1，无最终冲突／叠加警告。IDA MCP在本机EXE临时副本上核验：两窗口各为 `push eax` 1字节 + `call 7C8B3D` 5字节；EAX分别来自类型+710h/+6F0h，且已通过 ownership 与非空检查。回调只读取EAX、return 0，不写寄存器或栈；SyringeEx重定位相对CALL。入口三个push使原生ESP比函数入口低12字节；本hook不读栈参数。临时IDB未做完整自动分析，IDA自动spd不能用于原生栈深结论。

运行模块EXE SHA256 `7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6`，与调查IDB整体哈希不同。本机对应原始字节：`711B3D: 50 E8 FA 6F 0B 00`、`711B16: 50 E8 21 70 0B 00`。

额外只读运行核验PID50416：这两处及既有 `6A70AE` 均跳转至Syringe跳板，回调目标匹配已加载Phobos导出；free/BlitSidebar CALL目标正确。释放窗口跳板返回`addr+5`，该字节为NOP，随后进入`addr+6`，没有落入残缺指令。核验进程随后关闭。原始记录 `D:/Ra2/Cameos-evidence-20261028-36224/hook-runtime.json`。

原版SHP/reference卸载通知和场景清理继续保留。释放使所有别名及旧FrameID失效；已提交GPU工作只使用自有资源。没有声称覆盖未知模组原地改写或任意第三方释放路径。

## 验证与尚未覆盖项

- Debug x86构建成功；本轮没有改项目配置、shader或YRpp。
- 新 CameoLayoutTests 覆盖双列、顶行、平滑滚动、右侧origin、空槽、非法索引／溢出、60槽边界、负帧偏移与右下裁剪。先验证缺少实现时测试失败，补实现后通过。
- ShapeImage、FrameCache回归通过；ShapeFrameSource增加直接Data释放后别名移除、旧ID不可解析、同址新数据新ID的测试，通过。测试保存FrameID而不在失效后解引用FrameRecord。
- 普通权限 Spawner 0.0.0.17 直进 `XMP30S6.MAP`，PID36224，D3D12，side0，origin856，168×768 sidebar。用户确认基础图标正常。
- 日志记录建筑页count 0→1→3→6，六个基础图标均ready=1，frame0为60×48，FrameID18/20/21/24/25/26；本轮unsupported和pending均0。最终记录27次累计解码，含背景／按钮新帧，无invalid/budgetRejected。
- atlas提交600→900：decoded23、uploads23、indexBytes66556、paletteUploads2、paletteBytes2048均不变，连续300提交零新增解码／上传。一页R8、4MiB，两张palette，已记录区间无busy/budgetMisses。
- 用户未明确确认滚动，日志仅建筑页且scroll=0；其他页签／滚动返回复用、AltCameo渗透、超武、第二局重载、实际owned cameo释放、其他侧别、observer、Vulkan未完成动态验收。不要将既有atlas两局验收推广为本批cameo覆盖。
- 进度时钟、Countdown百分比、数量、名称、READY/HOLD、hover轮廓、暗化／闪烁均留到状态层；本批没有实现其混合或文字绘制。

部署DLL SHA256 `f201f72c9204158872aee89017790b87c7e1e9080ae3b5763ec2c2ae4cde9d9a`。旧版DLL/PDB备份 `D:/Ra2/Phobos-backup-cameos-20261028-170314/`。用户验收日志、构建日志、spawn.ini及运行hook核验存于 `D:/Ra2/Cameos-evidence-20261028-36224/`。机器路径仅用于本机证据定位。

## 滚动与页签实机对照（2026-10-29）

隔离副本 `Debug/AutomationGame`，Spawner 直进 `Arcana troop.mpr` 派生的临时地图：地图规则把三方 36 种建筑的 Owner/RequiredHouses 放开，Prerequisite 改为 GACNST、TechLevel=1。`N`/`D` 展开 MCV 后建筑页 count=28（14 行），1024×768 下 RepeatCount 10 行、20 槽。地图及 spawn.ini/RA2MD.INI 改动均为临时，测试后已删除并恢复。DLL SHA256 `e3a1740399cffd03002ae869206ef4588e2996f3152fc208ed6c68a07647643f`（`b5920e73`，工作区仅 AGENTS.md 有非代码改动）。

每组先 `game_pause`，在同一暂停帧依次取 `source=ddraw` 与 `source=gpu`，两者 simulation_frame 相同。比较区为图标格子 `(874,224)-(1006,730)`，排除未迁移的电力条列和页签按钮底边。

| 场景 | D3D12（PID30136） | Vulkan（PID61060） |
| --- | --- | --- |
| top=0 无叠层 | 最大色差 1 | 最大色差 1 |
| 建造中时钟 + 其余图标暗化 | 最大 8，>8 为 0 | — |
| 滚轮逐行 top 0→1→…→4（到底钳制） | 最大 5～8，>8 为 0 | top=4 最大 1 |
| 底部箭头按钮滚动、回到顶部 READY | 最大 8，>8 为 0 | — |
| HOLD + 时钟在 top=2，再滚到 top=1 | >8 共 58 像素，最大 10，全部在 HOLD 图标的时钟与文字底色叠加区，随图标整行平移 | 实际 top=3→2；>8 共 142 像素，最大 10，在 HOLD 文字底色区 |
| 切到防御页（count=8）再切回 | 防御页最大 1；切回保持 top=1 与 HOLD | — |

色差来自已接受的 alpha blending/RGB565 舍入：暗化、时钟、文字底色 176/256 近似 `dst*80/256`；叠加两层时红通道差约一个 RGB565 量化步长。滚动后图标、数量位置、时钟和 READY/HOLD 的位置与原生一致，没有残留旧行。

未覆盖：
- 本配置下滚轮和箭头均整行跳变，trace 30 次采样 `scroll=0`，连拍 DDraw/GPU 也未见半行偏移。IsScrolling/Slid 的部分可见行裁剪只有 CameoLayoutTests 覆盖，无实机证据。
- 页签按钮闪烁随时间继续（暂停时也在闪），两次截图的相位可能不同，只能作 best_effort；交替连拍时两个来源都出现过两种帧。
- 暂停期间点一次箭头会连续滚多行；运行中单击也会滚多行。这里只记录现象，未判断原生按住重复的节奏，不计入 GPU 结论。
- 未覆盖其他侧别、步兵/载具页滚动、数量文字滚动、超武页滚动、第二局和分辨率变化。

证据：`Debug/AutomationTests/sidebar-scroll-20261029/`（`d3d12/`、`vulkan/` 下的 `native-*.png`、`pairs.jsonl`、`pairs-summary.json`、debug 日志和临时脚本副本）。
