---
title: GPU 光标
type: note
permalink: phobos/gpurender/gpu-光标
tags:
- gpurender
- cursor
---

# GPU 光标

权威文档：`src/Render/CursorTrace.md` 的「黑底 + 独立 GPU 光标」一节；静态依据 gamemd-reverse #5 `cursor-gpu-handoff.md`。

- `CursorSnapshot.h` 在主线程以零等待尝试获取 MouseMutex，只复制当前 WWMouse 已加载的资源、帧、热点、实际绘制位置和 MOUSE_PAL 颜色；释放锁之后才调用 GPU API。获取失败或 abandoned 时不读引擎对象，隐藏光标。
- 首版只支持未压缩的索引帧，尺寸不超过 256×256。索引 0 透明，非零黑色不透明。绘制位置 = 实际位置 − 整图热点 + 帧 bounds。
- 用 `R32_UINT` 纹理：低 16 位存颜色，第 16 位为透明标记；只有图像或尺寸变化才上传，单纯移动复用纹理。
- 光标始终最后绘制、在最上层。没有新增 hook，也没有新增线程。
- 验收：D3D12 实机确认移动、动画、跨 sidebar、Alt-Tab 隐藏与恢复。Vulkan 与各种分辨率/缩放组合未验收。
- 镜像路线遗留的光标追踪 hook（`7B90C0..7BA282` 等）默认关闭，仅作定向诊断，不要变成无界的热路径日志。

## Relations

- part_of [[GPURender 索引]]
