# Sidebar FrameID 多页 atlas

依据 [完整设计](FrameAtlasDesign.md)，在阶段1 CPU索引缓存上接入GPU驻留。范围为六项背景和十按钮；光标及诊断镜像保留既有路径。没有改变引擎输入、按钮选帧、hook地址或原生CPU绘制。

## 数据与提交

- 快照保存FrameID、不可变PaletteID/颜色内容及布局。采集时每个Convert只复制一次256项映射，按全部颜色和RGB位格式比较复用版本；不再逐像素CPU颜色展开。
- 默认2048²页，优先R8_UINT，回退R32_UINT。首次驻留才上传索引，页内shelf只追加、不搬移、不循环纹理；FrameID与页位置无关。当前batch先pin全部请求，再选择整页LRU淘汰。
- IndexedSprite shader按区域整数Load索引，再查256×1 R32_UINT palette。索引0透明，非零索引的黑色仍不透明。沿用原裁剪、viewport和层序。
- 三个staging槽，每槽有提交fence；忙时跳过本次present，不等待GPU。行距256字节、起始偏移512字节，上传批次一并复制。即使swapchain为空也提交上传并取得fence。获取swapchain后绘制失败仍提交，再关闭GPU路径，不能取消该command buffer。
- 成功提交且编码上传后才发布驻留状态。取消保留预留位置，重试重新上传；创建/映射/提交失败不计成功上传。page/palette退役必须等最后引用的提交完成；Poll只推进连续完成序列。
- GPU重建清驻留映射，CPU FrameID不变。资源失效移除旧ID映射，空页退役；混合页中的空洞不复用。快照不保存GPU位置，View仅供当前batch使用，设备/页代际由FrameAtlas实例所有权隔离，无需向引擎发布额外PageID。

## 预算与限制

GPU页最多16页、合计64MiB（包括退役页），所以R32回退最多4个默认页。Palette GPU纹理最多256个，CPU不可变版本最多256个且快照持有的版本不淘汰。每个staging槽最多16MiB，三个槽最多48MiB；page预算不包含staging、palette、驱动分配与元数据开销。全部页被pin、退役未完成或上传预算耗尽时显式缺失该请求，其他已有驻留元素继续显示。

SDL 3.4.18 D3D12的SAMPLER格式能力查询使用SHADER_SAMPLE，会对支持Load的整数格式返回false。因此仅该后端用GRAPHICS_STORAGE_READ查询SHADER_LOAD能力，实际资源仍用SAMPLER绑定，shader只用Load。依据：[SDL D3D12源码](https://github.com/libsdl-org/SDL/blob/release-3.4.18/src/gpu/d3d12/SDL_gpu_d3d12.c)。本机实际R8/R32纹理上传和shader回读已核验；其他后端不套用该查询替代。

帧与palette在预算内可复用，淘汰后需要重传；缓存不保证减少draw次数。暂不覆盖cameo、世界素材、alpha blitter、在线碎片整理或未通知的原地素材改写。

## 验证记录（2026-10-28）

- AtlasTests：shelf边界、整页/溢出、pitch/offset对齐、palette A/B/A复用、内容/格式变化、pin与预算拒绝。
- FrameAtlasLifetimeTests：受控SDL替身时间线，三个在途槽Busy且不Map、乱序fence不能推进完成前缀、在途页延迟释放、创建/Map/Submit失败不发布驻留及Map失败后重试。该测试不链接SDL3.lib，不把替身结果作为驱动正确性的证明。
- FrameAtlasGPUTests：本机D3D12，分别运行R8和强制R32，4×4微型页跨三页，同页追加B后A回读不变；重复请求只上传一次；300次稳定提交零新增上传；全页pin、整页淘汰、取消复用、palette单独变化、GPU重建、CPU失效。真实DXIL shader回读核验红/蓝、带offset区域、非零黑色不透明、palette0非黑仍透明。
- Debug x86构建含DXIL/SPIR-V生成。只增加用户批准的FrameAtlas.cpp编译项和IndexedSprite.ps.hlsl ps_6_0项。
- 原有FrameCache、ShapeFrameSource、ShapeImage、ButtonImage、CursorImage回归全部通过；git diff --check通过。
- 本轮未新增/修改hook定义，沿用阶段1生命周期。用户按对局验收请求反馈“一切正常”，日志确认两局资源加载与稳定300次提交；详见下方记录。未执行Vulkan驱动测试，未量化覆盖全部按钮状态往返100次或其他侧别。

`PHOBOS_SIDEBAR_TRACE=1`启用[FrameAtlas]，首次及每300次成功提交汇总，上限64条/设备实例：页数/退役页、分配字节/矩形占用、palette数、在途数、索引/颜色/填充上传字节、解码/上传增量、淘汰/预算拒绝/Busy。设备重建重置GPU统计基线，CPU解码总计跨重建保留。稳定区间应同时看到indexDelta=0、paletteDelta=0、decodedDelta=0；首次出现新按钮帧需单列。

## 部署与对局验收

2026-10-28部署Debug DLL/PDB到D:/Ra2，DLL SHA256为`F6FE9292A72D2D40EE14CFBE8B7D37A1AF512DD4E16575AC346DADCE9D288639`，与构建输出一致。旧文件及本轮构建日志保存于`D:/Ra2/Phobos-backup-atlas-20261028-143524/`。通过原普通权限脚本启动，游戏PID37736；启动观察到Debugger Notice，尚未据此认定进入对局或完成画面验收。

随后用户反馈“一切正常”。本轮日志与构建记录保存于`D:/Ra2/Atlas-evidence-20261028-37736/`，已核对部署DLL哈希不变。实际后端D3D12、R8（bpp=1），side=0，游戏1024×768、sidebar 168×768：

- 第600→900次成功提交连续300次，decoded=17、uploads=17、indexBytes=55648、paletteUploads=1、paletteBytes=1024均不变，三项Delta全部为0；CPU采集第300→600次也保持decoded=17。
- 页签出现闪烁帧后新分配FrameID=18，回到选中帧复用17、禁用帧复用9。第1200次提交总计18帧、56404索引字节，新增756字节对应首次闪烁帧；未重复上传palette。
- 同进程两次加载XMP30S6.MAP；第二局generation31→66，按钮FrameID从7..16变为25..34，选中帧为35，旧reference地址可相同但Data地址更新。第1500次提交累计35次解码/35帧上传、112052索引字节；evictions=1，当前一页55648占用像素，旧页已回收。相同palette仍为一次上传，复用自有不可变颜色版本。
- 全部已记录区间pages=1（4MiB）、retiring=0、palettes=1、budgetMisses=0、busy=0；CPU invalid=0、budgetRejected=0。日志确认800→1024→800→1024→800的视频尺寸切换，未见GPURender失败状态。
- 用户反馈覆盖此次画面验收请求；没有逐项截图或Alt-Tab事件追踪，不能把它扩展为多后端、所有按钮状态100次或多侧别验收。
