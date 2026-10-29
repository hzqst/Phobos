# Phobos：GPURender 与自动化开发指南

本文件适用于本仓库，重点约束 `src/Render/`、`src/Automation/` 及其测试。
当前会话的明确要求优先；代码与文档不一致时先调查差异，不照搬历史实现或验收结论。
使用简体中文沟通，代码标识符和注释跟随所在文件的约定。

## 开始工作

1. 读取当前分支、HEAD、`git status --short` 和相关 diff，保留已有未提交修改。
2. 优先用可用的 memory／context-engine 获取定位线索，再亲自读取即将修改的源码和相关设计文档。
   Windows 本地文件读取、搜索优先使用 FastCtx；语义修改使用 `apply_patch`。
3. 多步实现先给出简短调查结论与 TODO，同一时刻只标记一项进行中；完成已授权工作，不停留在方案。
4. 先明确本次迁移的元素、来源、线程、资源所有权、失效边界和验收场景，再改 Hook 或渲染路径。
5. 有关 SDL／MCP API 的行为以当前依赖头文件和官方规范为准，必要时使用文档查询技能核验。

## 入口与参考

| 位置 | 调查内容 |
| --- | --- |
| `src/Phobos.cpp` | 参数解析、初始化、退出、Automation 模式的调试弹窗处理 |
| `src/Render/GPURender.*` | 状态机、Present／Timer／Blit、设备与提交生命周期 |
| `src/Render/SecondaryWindow.*` | SDL 展示窗口、位置、缩放、显隐与焦点 |
| `src/Render/PresentPass.*`、`src/Render/Shaders/` | 图层顺序、整数纹理解码、裁剪、混合、letterbox |
| `src/Render/FrameCache.*`、`FrameAtlas.*`、`PaletteCache.h` | 资源身份、按需解码、索引 atlas、palette、fence 与退役 |
| `src/Render/*Snapshot.h`、`ShapeFrameSource.h`、`SidebarText.*` | 安全引擎采集点、自有快照、素材与字体生命周期 |
| `src/Render/SurfaceMirror.*` | Primary Surface 捕获和独立的镜像诊断模式 |
| `src/Render/GPUReadback.*`、`CaptureImage.h` | 按需离屏截图、读回和像素所有权 |
| `src/Automation/` | 内嵌 HTTP MCP、Dispatcher、引擎输入、帧控制、截图编码 |
| `src/Commands/FrameByFrame.*`、`FrameStep.h` | 单人逐帧循环与自动化完成确认 |
| `Phobos.vcxproj`、`Phobos.props` | Win32 构建、源文件登记、SDL 与 shader 构建 |

先读与任务有关的文档：

- [Automation MCP](docs/Automation-MCP.md)：工具、协议、输入、截图、测试和当前限制。
- [FrameAtlas 设计](src/Render/FrameAtlasDesign.md)、[实现记录](src/Render/FrameAtlas.md)、[FrameCache](src/Render/FrameCache.md)。
- [Sidebar](src/Render/SidebarGPU.md)、[Cameos](src/Render/CameosGPU.md)、[Cameo 效果](src/Render/CameoEffectsGPU.md)、[Sidebar 字体](src/Render/SidebarTextGPU.md)。
- [Cursor Trace](src/Render/CursorTrace.md)：定向诊断，不应变成无界热路径日志。

设计文档包含目标契约，阶段记录也可能保留已被替换的旧路径；不能把文档中列出的目标当成已实现能力。

## GPU 渲染约束

- 保留原始 DirectDraw 渲染和输入逻辑。GPU 在独立 SDL 窗口中呈现，不抢原游戏窗口输入焦点。
- 默认 `independent` 模式清黑背景，只绘制已迁移的原生元素；未迁移地图区域为黑色是正常现象。
  `PHOBOS_GPU_MIRROR=1` 是显式诊断模式。禁止通过复制 DDraw／桌面画面掩盖 GPU 缺失或实现错误。
- 新元素应沿用原生选帧、状态、布局、颜色和绘制顺序。不要为截图或迁移改变原生 dirty、AI、输入或游戏规则。
- 在确认安全的游戏阶段读取引擎对象，发布自有数据或有效资源 ID；Timer／GPU 提交不借用已失效的引擎裸指针。
  不为读取素材调用可能触发加载或覆盖共享 scratch 的接口，除非已核实生命周期和调用语义。
- 保持当前主线程 SDL GPU 执行模型。不随意增加渲染线程、通用 RHI 或大规模业务 Hook。
- 分清逻辑画布坐标、素材帧偏移、atlas 区域、窗口像素和桌面坐标；裁剪源偏移只能累计一次。
- 保持透明图层顺序，不按纹理页或 palette 排序重排透明元素。索引 0 透明，非零索引的黑色不透明。
  整数索引纹理保持 nearest／无 mipmap，不能直接开启线性过滤。
- 新 `.cpp` 和 shader 按工程约定登记；shader 必须验证 DXIL 与 SPIR-V 路径，不只编译一种后端。

## 缓存、提交与截图

- 资源、palette、布局和 GPU 设备失效分别处理。引擎地址复用不能复活旧 FrameID；布局改变不应触发素材重新解码。
- 稳定素材帧复用 CPU 索引和 GPU 驻留，palette 变化独立处理。不要在每次 Present 重新解码、哈希或上传完整素材。
- Atlas 追加区域使用 `cycle=false`；整纹理 cycling 可能使旧区域内容未定义。遵循现有 pin、预算、整页退役和 staging slot fence 规则。
- 不绕过 `Atlas.Submit` 的上传、引用和 fence 管理。提交成功不等于 GPU 已完成；空 swapchain 不等于上传提交失败。
- 遵守获取 swapchain 后不能随意 Cancel command buffer 的约束；失败、取消、设备销毁都必须有明确所有权收尾。
- GPU 截图只按需创建离屏 COLOR_TARGET，复用实际 PresentPass、格式和绘制数据，再下载到 DOWNLOAD TransferBuffer。
  不读取不保证支持下载的 swapchain 纹理，不用 BitBlt／PrintWindow 代替原生 GPU 截图。
- 当前离屏绘制随正常 Atlas 路径提交，下载使用独立提交与 fence；只有 fence 完成后才能 Map。
  处理实际 pitch、RGBA／BGRA、256 字节行距与 512 字节 offset 对齐。常规路径不使用 `SDL_WaitForGPUIdle`。
- DDraw 截图共享 Primary Surface 区域、客户区偏移、MouseMutex、Lock／Unlock 和 pitch 处理。
  按实际颜色位域转换，不能固定假设 RGB565；离开锁之前复制所有 Surface 数据。
- PNG 编码只读取自有像素，放在工作线程；HTTP 线程不得访问游戏对象或 SDL GPU 资源。
- 严格区分 `simulation_frame`、`gpu_present_id`、`capture_id`。相同逻辑计数不证明两张图严格同帧。
  未验证同步的双来源采样只能称为 `best_effort`，必须保留各自时间戳和计数。
- 资源／场景／尺寸变化使旧请求失效时，返回明确失败，不返回旧缓存图片冒充本次捕获。

## Automation 与输入

- 正式 MCP Server 必须内嵌 `Phobos.dll`，不改成 Python 服务、外部代理、Named Pipe 桥接或第二个服务器进程。
- 仅 `-PhobosAutomation` 显式开启网络与自动化；普通模式不增加监听线程，也不跳过原有 Debugger Notice。
  不在 `DllMain` 初始化 Winsock、启动服务、等待线程或执行复杂游戏逻辑。
- 默认只监听 `127.0.0.1:18742/mcp`，保留 Token、Host、Origin、版本、大小及并发限制；不把 Token 写进日志或提交。
- 菜单、游戏内、暂停和 GPU Disabled 都需要 Dispatcher。不能把自动化调度只挂在 GPU Timer 上。
- 生产输入沿用引擎队列与原生菜单／窗口处理路径，不回退到抢前台、桌面 SendInput 或操纵 GPU 展示窗口。
  地图鼠标事件需要原生 WndProc 预处理；直接塞按钮坐标队列不等于完成拖框逻辑。
- `injected`、队列排空和实际游戏效果分开报告。不得仅因 API 成功或队列排空就设置 `consumed=true`。
  超时、取消、分辨率变化、失去引擎可用状态和退出时必须释放保持键／鼠标按钮。
- 失去 OS 前台不等于引擎停用；原生全屏 focus wait 又可能确实阻止处理。不可通过伪造引擎焦点声称后台可用。
- 保留单人限制。Pause 等待暂停屏障，Step 按已完成逻辑帧确认，Resume 等待正常逻辑推进；不得用 Sleep 或 Present 次数模拟。
- 新工具同步维护固定参数、Schema、错误、文档和标准 MCP SDK 互操作测试。2025-11-25 握手不能伪装成 2026-07-28 无握手路径。
- 保留实例 ID 校验、请求去重和有界等待；结果不确定的副作用操作不能用新 ID 自动重放。

## Hook 与逆向核验

- 优先复用已验证的入口，不随意新增硬编码地址，不默认修改 YRpp 或第三方源码。
- 新增或修改 Hook 时使用 [.agents/skills/check-hooks/SKILL.md](.agents/skills/check-hooks/SKILL.md)。
  检查长度、完整指令边界、Phobos／Ares／SyringeEx 冲突、寄存器、调用约定、栈平衡、返回地址和相对指令重定位。
- 自动发现可能遗漏仅修改函数体的既有 Hook 和条件返回地址，必须补查；历史 IDA 地址或另一 EXE 的结论不能替代当前模块字节核验。
- 新引擎函数／数据引用也需记录来源与证据。Hook 保持小而有界，不执行网络等待或 GPU 全局同步。
- IDA／WinDbg 等验证不可用时明确列出缺失项，不能以编译通过替代运行时正确性。

## 构建与定向测试

目标：Visual Studio 2022、v143、Win32、C++20、静态 CRT；沿用项目的调用约定与异常配置。
下列 VS 路径是当前机器约定，其他机器先确认安装位置。shader 编译还需项目要求的 Vulkan SDK DXC
或有效 `PhobosDxc`；GPU 测试需与仓库 headers 相符的 x86 SDL3.dll。

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' Phobos.vcxproj /p:Configuration=Debug /p:Platform=Win32 /m /v:minimal /nologo
cmd /c scripts\test_automation.cmd
cmd /c scripts\test_gpu_capture.cmd direct3d12
cmd /c scripts\test_gpu_capture.cmd vulkan
cmd /c scripts\test_render_capture_regression.cmd
& .\Debug\AutomationTests\sdk2\Scripts\python.exe tests\Automation\test_http.py
& .\Debug\AutomationTests\sdk2\Scripts\python.exe tests\Automation\test_capture_pixels.py
```

- 按改动范围选择测试：协议／调度改动跑 Automation；像素／截图改动增加 PNG 解码及 GPU 生命周期；渲染改动增加 Render 回归与实机画面对照。
- SDK2 环境不是仓库交付物。缺失时建立隔离 Python 环境，按测试声明安装 `mcp==2.3.0` 和 Pillow；
  旧版兼容测试使用另一个 `mcp==1.25.0` 环境运行 `test_http.py`，不要相互覆盖 SDK。
- `test_automation.cmd` 先生成 HTTPHarness 和 PNG fixtures。HTTPHarness 是端口 18743 的测试夹具，不是正式服务器。
  使用同一端口的协议测试顺序执行，不并发启动多个夹具。
- Render 回归脚本依赖主工程生成的 shader，当前覆盖 11 项既有测试；不包含需要外部字体资产的 GlyphCacheAssetTests。
- GPU 同步、资源身份和帧控制等高风险行为先补关键测试；文档小改只做定向检查，不为 AGENTS 文本或生成产物写内容锁定测试。
- 测试程序用普通权限和 `asInvoker` manifest。遇到 UAC 提示先调查 manifest／兼容性设置，不用管理员启动掩盖问题。
- 构建失败必须停止部署与后续实机验收；确认退出码及 DLL 身份，防止用旧 DLL 得到假的通过结果。

## 隔离实机测试

- 使用仓库内 `Debug/AutomationGame` 隔离副本，准备合法游戏资产、Syringe 和匹配的 SDL3／wrapper。
  未经明确授权，不覆盖实际游戏安装（例如 `D:/Ra2`）的 DLL/PDB，不修改用户 INI、存档或删除用户数据。
  旧文档中的部署许可和备份记录不是本次任务的授权。可写测试配置不得通过链接落到实际安装。
- 只在构建成功、隔离游戏已退出且路径确认后，将当前 DLL/PDB 部署到隔离目录。记录 DLL 哈希、提交及 dirty 状态。
- 实机脚本使用随机 Token、等待内嵌 MCP 就绪并核对进程／实例；只清理该次测试拥有的进程。
  不杀掉端口占用者或用户已运行的游戏。遭遇战脚本清理自己的进程不等于正常退出通过。
- 用快捷键选中测试 MCV：先核实隔离副本 `KeyboardMD.ini`，当前夹具为 `N` 选中下一对象、`D` 展开。
  断言选中类型 `AMCV` 并检查展开画面，不用脆弱的地图鼠标坐标替代该流程；拖框应另设验收。

```powershell
$env:PHOBOS_TEST_SCENARIO = '1'
$env:PHOBOS_TEST_CAPTURE = '1'
$env:PHOBOS_TEST_GPU = '1'
$env:PHOBOS_TEST_DRIVER = 'direct3d12'
$env:PHOBOS_TEST_CAPTURE_COUNT = '5'
Remove-Item Env:PHOBOS_GPU_MIRROR -ErrorAction SilentlyContinue
& .\Debug\AutomationTests\sdk2\Scripts\python.exe tests\Automation\game_input_smoke.py
```

另行运行 GPU 关闭（`PHOBOS_TEST_GPU=0`）、Vulkan、mirror（`PHOBOS_GPU_MIRROR=1`）和菜单正常退出
（`PHOBOS_TEST_SCENARIO=0`）组合。明确设置每轮环境，避免继承前轮 mirror／后端配置；结束后恢复任务设置的环境变量。
同一隔离游戏目录和 18742 端口的实机测试串行运行。脚本会临时修改隔离 INI 的驱动配置并恢复，不部署 DLL。

## 验收与交付

- 检查解码后的实际像素、尺寸、颜色、透明合成、坐标、裁剪、letterbox、sidebar／cameo／字体／光标，以及新增元素的行为；PNG 存在不是通过。
- `native-*.png` 是 MCP 原生截图；脚本中的 ImageGrab 等桌面图仅为诊断证据，不能混淆来源。
  independent 中未迁移区域的黑色、最终合成 alpha=255 均可能正确，不应为了“好看”改变渲染语义。
- 图像容差必须由格式量化或已观察的渲染更新解释，并记录差异；不能为了通过断言随意放宽。
  最小化不保证后端停止提供纹理，不能把成功的新截图写成超时测试通过。
- 测试有界，记录后端、输出模式、分辨率、PID／instance、帧号、实际调用结果和证据路径。
  默认证据目录会复用；需保留历史时先复制到独立证据目录，不用旧结果证明新二进制通过。
- 历史测试不是当前修改的验收。明确区分“实机验证”“仅编译／单测”“未实现”“受环境限制未验证”。
  另一后端、侧别、wrapper、设备故障或长期性能结论不能从一次成功泛化。
- 截至 `28be640b`，pair／复合操作工具尚未实现；拖框未完成验收；sidebar 逐行滚动已实机对照，平滑滑动未验证；
  原生全屏／其他 wrapper、真实设备丢失重建、系统性 FPS 与泄漏长测仍有缺口。后续以源码和新增证据更新状态。
- 更新对应模块文档及 `docs/Automation-MCP.md`，检查 diff，排除游戏资产、Token、存档、生成 DLL/PDB 和无关修改。
  根目录已有的 `GPURender-Progress.md`、`Launch-Phobos.cmd` 等用户文件不因本指南而自动纳入提交。
- 未经本次任务授权不自动提交／推送。授权后使用 `<type>(scope): <summary>`，追加
  `Co-Authored-By: Codex <codex@openai.com>`，推送后核对本地与远端提交。
