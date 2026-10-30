---
title: 经验 存储全局变量定义在 cpp 而不是头文件
type: note
permalink: phobos/gpurender/经验-存储全局变量定义在-cpp-而不是头文件
tags:
- gpurender
- convention
- cpp
---

# 经验 存储全局变量定义在 cpp 而不是头文件

- **触发信号**：在 `src/Render` 头文件里写 `inline` 缓存、快照、GPU 资源等可变全局变量，或在头文件里 include 编译好的着色器字节头文件（`Shaders/*.dxil.h`、`*.spirv.h`）。
- **根因/约束**：用户要求（2026-10-30）真正负责存储的全局变量放进一个 `.cpp`。头文件被多个编译单元包含后，`inline` 虽仍只有一份，但一旦有人改成 `static`/`const` 就会悄悄每个编译单元一份；着色器字节是 `const unsigned char g_...[]`（内部链接），被几个编译单元包含就复制几份。项目规模扩大后难以察觉。
- **正确做法**：
  - 头文件只写 `extern` 声明（注释留在声明旁），定义放在同名 `.cpp`（如 `TerrainSnapshot.cpp`）；`constexpr` 常量仍在头文件。
  - GPU 通道照 `PresentPass.h/.cpp`：着色器字节头文件、私有 GPU 资源（`.cpp` 内匿名命名空间）与函数实现都在 `.cpp`，头文件只留接口与需要共享的 `extern`（如 `TerrainPass::Sampler`、`ABufferPass::Texture`）。
  - 新 `.cpp` 加进 `Phobos.vcxproj` 的 `ClCompile`；`scripts/test_render_capture_regression.cmd` 用 `EXTRA_<测试名>` 给需要的单元测试链接对应 `.cpp`（例如 Atlas、FrameAtlas 两个测试要 `PaletteCache.cpp`）。
- **验证方法**：`grep -nE "^\s*inline " src/Render/*.h` 不应出现变量定义；`grep -ln "Shaders/" src/Render/*.h` 应为空；Debug 构建无警告、渲染回归通过。
- **适用范围**：Phobos `src/Render` 及以后新增的 GPURender 模块。
