---
title: GPURender 架构
type: note
permalink: phobos/gpurender/gpurender-架构
tags:
- gpurender
- architecture
- hooks
- sdl
---

# GPURender 架构

## 运行模型

- 全部在游戏主线程执行，没有渲染线程，也不另写 RHI 抽象层。SDL 头文件只在 `src/Render/*.cpp` 中引用，`GPURender.h` 不暴露 SDL 类型。
- SDL3 3.4.18 以 `/DELAYLOAD` 动态链接；`SDLLoader` 先 `LoadLibraryA` 探测，并要求版本为 3.4.x。头文件和 x86 导入库在 `lib/SDL3/`。
- 状态机：Disabled / Initializing / Running / Hidden / Failed / Shutdown。严重错误进入 Failed 并释放资源，游戏照常运行；不尝试重建设备。
- 提交：DoBlit hook 和 16ms Win32 Timer 都调用 `RequestPresent()`（节流 `1s/120`）；窗口事件直接 `Present()`。present 模式依次尝试 immediate、mailbox、vsync；用非阻塞的 `SDL_AcquireGPUSwapchainTexture`。
- 输出模式：默认 `independent`（每帧清黑，只画已迁移元素）。`PHOBOS_GPU_MIRROR=1` 选择旧的整屏 Primary 镜像，仅供诊断，启动后不切换。
- 副窗口（`SecondaryWindow`）：顶层窗口，不设 owner，`WS_EX_NOACTIVATE`；默认放在游戏右侧并留 64 像素间距，跟随游戏窗口矩形变化；等比缩放加黑边；点 ✕ 只隐藏（Hidden）。

## 配置与环境变量

| 项 | 作用 |
| --- | --- |
| `RA2MD.INI [Phobos] GPURender.Enable` | 默认 yes |
| `GPURender.Driver` | auto / direct3d12 / vulkan |
| 命令行 `-NoGPURender` | 优先关闭 |
| `PHOBOS_GPU_MIRROR=1` | 镜像诊断模式 |
| `PHOBOS_SIDEBAR_TRACE=1` | 有界日志：`[SidebarCameoGPU]`、`[SidebarButtonGPU]`、`[FrameCache]`、`[FrameAtlas]`、`[SidebarTextGPU]`，各自上限约 64 条 |
| `PHOBOS_CURSOR_TRACE` | 光标事件追踪（定向诊断） |
| `PHOBOS_FONT_LIFETIME_TEST=1` | 仅 Debug：受控字体构造/析构探针 |

只在窗口模式，或检测到 ddraw wrapper（System32/SysWOW64 之外的 `ddraw.dll`）时启动；原生独占全屏不启动。

## 模块（src/Render）

| 文件 | 职责 |
| --- | --- |
| `GPURender.*` | 状态机、配置、Present/Timer、设备与提交生命周期 |
| `SecondaryWindow.*` | SDL 展示窗口 |
| `PresentPass.*`、`Shaders/` | 绘制管线；`Fullscreen.vs`、`DecodeRGB16.ps`（镜像与光标 R32）、`IndexedSprite.ps`（atlas 索引 + palette，mode 4 用于字形 mask） |
| `FrameCache.*`、`FrameAtlas.*`、`PaletteCache.h`、`AtlasAllocator.h` | 见 [[帧缓存与 atlas]] |
| `SidebarSnapshot.h`、`ShapeFrameSource.h`、`ShapeImage.h`、`ButtonImage.h` | 在 `6A70AE` 采集 sidebar 快照 |
| `SidebarText.*`、`GlyphCache.*` | 见 [[Sidebar 文字与字形]] |
| `CursorSnapshot.h`、`CursorImage.h`、`CursorTrace.*` | 见 [[GPU 光标]] |
| `SurfaceMirror.*` | Primary 捕获（镜像模式与 ddraw 截图共用） |
| `GPUReadback.*`、`CaptureImage.h` | 按需离屏截图读回 |
| `Hooks.cpp` | 全部 GPURender hook |

shader 只写一份 HLSL，由 MSBuild 调 dxc 同时生成 DXIL 和 SPIR-V（`$(IntDir)Generated\Shaders`）；dxc 优先用 `PhobosDxc`，其次 `$(VULKAN_SDK)\Bin\dxc.exe`。两条路径都必须验证。

## Hook 分组（`src/Render/Hooks.cpp`）

- 显示生命周期：`6BDE19`（首次分配 surface 后）、`561163`（Set_Video_Mode 成功）、`4F4B7E`（DoBlit 后 present）、`6BB8E0`（DisposeOfStuff 时 Shutdown）
- 光标追踪（默认关闭）：`4F4583/4F459D/4F4780/4F4B75/4F4BA4`、`7B90C0..7BA282`、`560BF0`
- Sidebar 快照与状态：`6A70AE`（完整 Draw 后发布快照）、`6A99C3`（Strip 项目状态）、`6AA01C`（项目结束）
- 文字：`4A5EB0/4A5FF2`（Simple print 作用域）、`434120`（字形 Blit）、`621B85`（底色）、`434700/433940/434A40`（字体失效）、`433990`（字体加载）、`734D30`（CSF 卸载）
- 失效与就绪：`6A5840/6A5BF0/6BE1C0/730100/730400/652D90`，以及 `6A5BDB/6530E7`（恢复就绪）
- 资源生命周期：`69DE00/69DE70`（ShapeButton）、`69E100/69E500/69E930`（SHP 卸载/析构）、`711B16/711B3D`（Ares 直接 free cameo）

新增或修改 hook 必须走 `.agents/skills/check-hooks/SKILL.md`。

## 提交历史（feature/gpu-render）

`6971fd8d` SDL3 依赖 → `f211dd46` 状态机 → `ebd96f5c` 窗口与设备 → `c7d14cce` Primary 镜像 → `ae7aa92f` 黑底 + GPU 光标 → `78f928af` sidebar 背景 → `0514cbac` 按钮 → `1c68e475` FrameCache → `5d65f83c` atlas → `bfb7551c` cameo → `2128e6c4` cameo 状态效果 → `f592c1c1` cameo 文字 → `c71f3f20` 字形生命周期测试 → `920635c7`/`28be640b` Automation → `b5920e73` AGENTS.md → `0d43c6c5` 滚动验收文档

## Relations

- part_of [[GPURender 索引]]
- uses [[帧缓存与 atlas]]
