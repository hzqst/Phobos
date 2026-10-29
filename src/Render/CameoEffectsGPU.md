# Cameo 状态效果：接入前核验与第一批实现

2026-10-28。接续 [CameosGPU.md](CameosGPU.md)，复用 gamemd-reverse Issue #8 / `194cd7f` 的列表、状态、层序和生命周期依据。本批实现扇形时钟、不可用暗化、FlashEndFrame 闪烁和 hover 轮廓。普通权限 Spawner 对局中用户反馈“图标countdown mask一切正常”；其他分支的动态覆盖见末节。

后续文字阶段已接入数量、READY/HOLD 与超武 readiness，按原生顺序放在时钟前后；见 [SidebarTextGPU.md](SidebarTextGPU.md)。下文“文字未接入”描述本阶段当时的范围。

## 两个缺口的结论

### Countdown 是 GCLOCK2 遮罩

用户进一步明确“Countdown 百分比”指图标上逐渐消退的扇形进度遮罩，并非数字文字。因此此前基础阶段将其列为文字层的判断不准确，本批已修正。

读取本机实际加载资源：`B0B484 → SHPReference("GCLOCK2.SHP") → Loaded Data`，canvas 60×48、55帧；选取0/1/14/28/41/54帧解码查看，无数字文字。原版 `StripClass::Draw` 在 `6A9E97` 调用 CC_Draw_Shape，frame为原生animStage+1、SIDEBAR Convert、flags404。不新增百分比计算或文字绘制链路。

参考用户提供的 [vera20k/sidebar_build.rs](https://github.com/YuriPlanet/vera20k/blob/main/src/app/presentation/sidebar_build.rs) 和 [draw_passes.rs](https://github.com/YuriPlanet/vera20k/blob/main/src/app/presentation/render/draw_passes.rs)：该实现也将基础cameo、暗化／闪烁和gclock组织为不同层。仅作架构对照，本批以本机原生状态和素材为准，没有引入该项目的代码或依赖。

### 原生混合与本轮明确取舍

已核验正式还原源码 `ConvertClassSelectors.cpp`、`BlitTransDarken.cpp`、`BlitTransLucent50.cpp` 及对应RLE实现：

| 标志 | 原版分支 | 非零索引的16位输出 |
| --- | --- | --- |
| 401 | Plain[2]／RLE[1]，Darken | `(dst >> 1) & mask` |
| 404 | Plain[5]／RLE[4]，TransLucent50 | `((palette[index] >> 1) & mask) + ((dst >> 1) & mask)` |

索引0保留目标；暗化忽略源palette颜色。运行采样 SIDEBAR Convert Plain[2] vtable=`7E59F0`、Mask=`7BEF`；Plain[5] vtable=`7E59C0`、Mask=`7BEF`，与RGB565还原一致。

用户明确选择 **alpha blending，接受RGB565舍入细微色差**。本批在已有straight-alpha pipeline中：Darken输出黑色+alpha0.5；时钟／闪烁输出实际SIDEBAR palette颜色+alpha0.5；索引0仍alpha0；hover实色alpha1。原版先舍去源／目标通道低位，与浮点50%混合不逐像素等价，例如5位通道31与31原版混合得30，浮点混合仍为31。不称为整数精确复现，也不增加目标回读、离屏合成或新shader项目项。

## 原始 RLE 行尾问题

实素材 GCLOCK2 frame14 第一行：宽60，行长32字节，编码为透明2像素、26个`50h`、透明33像素，总跨度61。旧 ShapeImage 解码器拒绝超过剩余宽度的透明游程，导致这类合法帧无法缓存。

`RLEBlitTransDarken @496250`、`RLEBlitTransLucent50 @4963F0` 在透明run时扣减remaining，下一轮检查remaining>0；透明尾部可以越过右边界，不会写那些像素。修复为在自有像素数组内截断透明run，仍按编码行长前进。长度头、截断run、字面值不足、尺寸和编码总长度检查保留。回归测试先在旧实现上失败，修复后通过，包含实素材该行、全透明尾部越界、0长度游程及截断／短行失败。

## 状态采集与绘制

- 新只读观察点 `6A99C3` 位于原版／Ares完成可用性及进度判断之后。只在原生doDraw为真时记录最终BL暗化位、drawProgress、completed、animStage及绝对列表索引；不额外调用CanBuild、FindFactory、Draw、AI，也不改变dirty或原生绘制。
- 共用已核实的原版／Ares列表provider，将观察结果按活动页与`{ItemType, ItemIndex}`关联。状态存储有上限；场景／布局／资源失效清理。CPU未重画的项目保留最近已观察状态，GPU每次仍绘制整个有效快照。只有原生已观察状态才能画暗化／时钟，不用自建计时器推测进度。
- 仍在`6A70AE`读取完整可见列表，按每项基础图标 → hover → 暗化 → 闪烁 → 时钟发布placement。FlashEndFrame和CurrentFrame、Select hover位同次读取；没有按墙钟自行闪烁。
- DARKEN与当前GCLOCK2帧复用FrameCache／PaletteCache／FrameAtlas。只解析已加载资源，命中帧不再解码或上传；发布后不保留引擎指针供GPU访问。未知／未支持的基础来源不画悬空状态层。
- 每个可见slot增加darken和clock资产槽，atlas仍按FrameID去重。hover四条不重叠的1像素边框使用现有索引shader的实色模式，不创建纹理。帧Left/Top、原生右下clip、sidebar边界和逻辑画布裁剪继续保留。
- `IndexedSprite.ps.hlsl` 增加Effect uniform，复用已有两张纹理与pipeline；DXIL/SPIR-V均生成成功。原RGB16光标／诊断镜像路径保持。
- 尚未接入名称、数量、READY/HOLD等文字。原版数量／文字先于clock，因此这些文字像素与clock的叠加也留到文字阶段；当前保持已接入层之间的顺序。

## Hook 核验

使用 Check Hooks discover/conflicts 脚本及本机EXE临时IDA数据库。旧IDA worker失联，重新打开后health正常，再完成检查。

| Hook | 地址/大小 | P0大小 | P1冲突 | P2边界 | P3参数 | P4相对指令 |
| --- | --- | --- | --- | --- | --- | --- |
| StripClass_Draw_GPUState | 6A99C3/9 | 通过 | 通过 | 通过 | 通过 | 通过 |

窗口为`33 C0`（xor eax,eax）+`89 84 24 AC 00 00 00`（mov [esp+ACh],eax），共9字节，后继`6A99CC`完整指令；没有相对跳转／CALL。回调return0，不写寄存器／栈。原生prologue `sub esp,47Ch` 加esi/ebx/ebp/edi四次push，采样时ESP=入口ESP−48Ch；观察`+12h`进度开关、`+14h`完成、`+16h`doDraw、`+38h`stage、`+3Ch`索引，BL暗化。Ares SuperDarken hook在6A99B7，空项跳转hook在6A99BE..6A99C3，均不覆盖本窗口。

本机EXE SHA256仍为 `7b8a068535d6af06845edf95ae829b113d00c02909330e16f197426cd7db94b6`。本轮日志observed=1与原生递增stage证明回调实际执行；用户退出后未再读取该新增hook的运行跳板，不把此前其他hook的跳板核验作为它的证据。

## 验证与部署

- Debug x86构建成功，DXIL/SPIR-V生成成功，未改项目配置／第三方源码／YRpp。
- ShapeImage、CameoLayout、ShapeFrameSource、FrameCache回归通过。
- 实际D3D12 GPU测试R8/R32均通过：黑色半透明忽略palette色、红色半透明叠绿色、两次暗化、实色hover、透明索引不改目标；保留原有跨页、300次稳定零上传、退役／取消测试。50%混合的RGB回读允许UNORM量化造成每通道1字节误差；正常／实色／alpha仍精确比较。
- Spawner普通权限直进`XMP30S6.MAP`，PID41100，D3D12。用户确认countdown mask正常。日志时钟帧1–24全部clockReady=1，FrameID19–42；hover开关时frame8复用ID26，frame9复用ID27，frame12复用ID30。
- 300次CPU采集：decoded41、hits5182、invalid0、budgetRejected0。该区间时钟正在进入新帧，新增解码／上传是预期行为，不声称实战稳定300提交零增量。
- 300次atlas提交：R8一页4MiB，两张palette，uploads39、indexBytes118960、budgetMisses0、busy0。
- 本轮没有触发darken/flash，未动态覆盖帧25–54、暂停／取消／完成、其他页签、滚动、超武、第二局或Vulkan。暗化／闪烁目前有混合算法与GPU回读证据，尚无本轮实战画面对照。2026-10-29 滚动实机对照触发了建造中的其余图标暗化、时钟、READY和HOLD，D3D12同帧ddraw/gpu最大色差8～10，见CameosGPU.md滚动小节；完成闪烁仍无实战对照。

部署DLL SHA256 `a89467f9936925d07dc1f1dcf4766f937c6f2a6aba4b3744fb89a1f358facb96`，与构建一致；备份`D:/Ra2/Phobos-backup-cameo-effects-20261028-175424/`。日志、构建记录、实际GCLOCK2帧图和spawn.ini保存在`D:/Ra2/CameoEffects-evidence-20261028-41100/`。
