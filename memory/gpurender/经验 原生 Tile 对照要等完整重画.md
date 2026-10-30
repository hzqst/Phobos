---
title: 经验 原生 Tile 对照要等完整重画
type: note
permalink: phobos/memory/gpurender/经验-原生-tile-对照要等完整重画
tags:
- gpurender
- tile
- overlay
- zbuffer
- trace
---

# 经验 原生 Tile 对照要等完整重画

- **触发信号**：CPU 重放的 Tile/ZBuffer 与原生只在纵向滚屏后不一致，横向滚屏、开局都一致；不一致的像素颜色相同但原生 Z 更小（同一个 overlay 被画了第二次）。
- **根因 / 约束**：纵向滚屏时原生只重画露出的水平条带（clip 为条带）。带 Z shape 的绘制走 `437A10`，Flag=1 梯度（索引 0）不逐行步进，行基值取自 clip 的顶行，因此条带重画得到的基值与完整重画不同；严格 `<` 测试让这次更近的重画覆盖掉原值。对 clip 不变的规则（TMP `547CF0`、平铺与无 Z shape 的 RLE 分带）看不出这个差别。
- **正确做法**：trace 先 `MapClass::Instance.MarkNeedsRedraw(1)`，在下一次采集时再与原生核对（`OverlaySnapshot::CaptureAfterDraw`）。GPU 每帧完整重画，本来就只应等于原生完整重画的结果，条带历史不需要复现。
- **验证方法**：GPUOVX 上滚视角，完整重画前有 8 像素颜色、34 像素 Z 不同；请求完整重画后 `tilecheck` 全部为 0（含 18 个带 Z shape 的 ramp Tiberium）。
- **适用范围**：凡是用原生 Tile/ZBuffer 作真值的 trace 对照（overlay、C6 的 Terrain 对象与阴影）。

相关：Ramp Tiberium 的 Z shape 裁剪起点可能落在帧内；`437A10` 读的是 Z shape 帧的原始数据（RLE 时就是编码流），要按原生指针算出每个像素实际读到的字节（`OverlaySnapshot` 的 `ZBytes`）。
