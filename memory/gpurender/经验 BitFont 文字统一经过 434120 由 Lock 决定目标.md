---
title: 经验 BitFont 文字统一经过 434120 由 Lock 决定目标
type: lesson
permalink: phobos/gpurender/经验-bitfont-文字统一经过-434120-由-lock-决定目标
tags:
- gpu-render
- text
- glyph
- lesson
---

# 经验 BitFont 文字统一经过 434120 由 Lock 决定目标

## 触发信号
要在某个绘制区间里接管文字（如主建筑的“主要”标签、耗电标签），或判断一段文字画到了哪个 surface。

## 根因 / 约束
- BitFont 文字只有一个逐字形写入函数 `434120`（`BitFont::Blit`），调用者只有 BitFont 内部的 `434CD0`（`BitText_DrawText`）与 `434500`（经 `434B90` `BitText_Print`）。`4A5EB0` Simple、`4A60E0` Fancy、`70AA60` `DrawExtraInfo`、Tooltip、`TextLabelClass`、`EditClass` 都汇到这里。
- `434120` 的参数里没有 surface：位置是 surface 坐标，写入的是字体里 `pGraphBuffer` 所指的内存，它由之前的 `BitFont::Lock`（`4348F0`，ECX 为字体，栈上 +4 为 surface）设置。按调用栈偏移取 surface 只对固定调用链有效（Tooltip 用 `434CD0` 帧的 +5C），换了 `434500` 路径就不成立。
- 主建筑标签 = `70AA60`：`FillRect(rect,0)` 黑底 + 房屋色 `DrawRect` 边框 + `4A5EB0` 写 `TXT_PRIMARY`（占地宽 1 用 `TXT_PRI`）。
- 用 SHP 画的数字（Phobos `ShapeTextPrinter`）走 `4AED70`，不经过 BitFont。

## 正确做法
hook `4348F0` 记下目标 surface，在 `434120` 用 `SidebarText::CaptureGlyph` 取位图、笔位、字体 bounds 与颜色；采集前先 `SidebarText::BindFont()`，否则 `font != BoundFont` 会取不到。区间外让 `434120` 继续走 Sidebar 和 Tooltip 原有的观察（D3a-5，`02b4fca6`）。

## 验证方法
GPUFACT 选中战车工厂：trace `glyphs=2 glyphsUnknown=0`，GPU 对 `native_composite` 在标签区域 0 个像素不同；Sidebar 区域差异与改动前相同。

## 适用范围
任何要接管 BitFont 文字的区间（RenderLayers、之后的 6D4664 后 UI、FlyingStrings 等）。

## Relations
- part_of [[GPURender 索引]]
- relates_to [[Sidebar 文字与字形]]
- relates_to [[经验 引擎没有统一画线函数]]
