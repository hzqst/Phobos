---
title: 经验 tint 由 blitter 决定而不是 RLE 与否
type: lesson
permalink: phobos/gpurender/经验-tint-由-blitter-决定而不是-rle-与否
tags:
- gpu-render
- blitter
- tint
- lesson
---

# 经验 tint 由 blitter 决定而不是 RLE 与否

## 触发信号
建模 `4373B0`/`437A10` 的绘制时要决定 tint 是否生效；旧规则“只有 RLE 帧应用 tint”对非 RLE 的车辆 scratch 合成图（`73B140 → 4373B0`）不成立。

## 根因
`4AED70`、`4373B0`、`437A10` 都把 tint 原样传给 blitter，tint 非 0 时调 `Blit_Copy_Tinted`（vtable +8）。是否 OR 进颜色由各 blitter 自己决定。IDB 逐表位核对（`ConvertClass::InitBlitter` 48EBF0 建表，看 +8 的实现是否用到 tint 参数）：
- RLE 表：只有 `RLEBlitTransXlatAlphaZRead`（498150）与 `RLEBlitTransXlatAlphaZReadWrite`（499280）应用；`Alpha` 不测 Z、所有半透明、非 Alpha 的都转调 `Blit_Copy`。
- 非 RLE 表：只有 `BlitTransXlatAlphaZRead`（494C40，`*dst = tint | color`）应用；连 `AlphaZReadWrite` 都忽略。
YRpp 的 `Blitters/*.h` 是近似复刻（RLE 的 Tinted 也写成忽略），不能作依据。

## 正确做法
`Draw::Tint = !shadow && !translucent && 测Z && (RLE || !写Z) ? tint : 0`（`OverlaySnapshot` 的 `Capture::Blend`）。判断原生 blitter 行为时查 IDB 的 vtable 实现，不查 YRpp。

## 验证方法
GPUTINT.MAP（GPUVXL + `[MTNK] Tint.Color=255,0,0`）：坦克的两次 `73B44B` 非 RLE 绘制带 tint，`compositecheck mismatched=0`；按旧规则这两次会不一致。GPUINF/GPUBLD 复跑无回归。

## 适用范围
所有经 `4AED70`/`4373B0`/`437A10` 的绘制（D3 起的 Composite 与 Tile 采集）。

## Relations
- part_of [[GPURender 索引]]
- relates_to [[经验 半透明对照要考虑 Phobos 的 blitter 补丁]]
- relates_to [[决策 对象层记录绘制调用而非按状态重推]]
