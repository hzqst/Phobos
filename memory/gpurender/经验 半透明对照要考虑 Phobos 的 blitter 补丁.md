---
title: 经验 半透明对照要考虑 Phobos 的 blitter 补丁
type: lesson
permalink: phobos/render/gpu-渲染对照时须考虑-phobos-对原生-blitter-的机器码补丁
tags:
- gpu-render
- blitter
- translucency
- lesson
---

# GPU 渲染对照时须考虑 Phobos 对原生 blitter 的机器码补丁

## 触发信号
CPU 模型 / GPU 与原生 Composite 在半透明 SHP（如 S_CLSN22，`Translucent=yes`）上每个通道差 ±1，参数（surface、convert、帧、位置、flags、Z、亮度）在 4AED70 入口完全一致，旧色也一致。

## 根因
Phobos `src/Misc/BlittersFix.cpp`（`[General] FixTransparencyBlitters`，默认开）在运行时改写 RLE AlphaZRead 的 25/50/75% blitter（`4984D0/4986D0/4988C0`，补丁点 4985FE/4987F7/498817/4989EE），把 `(c>>1)&mask` 的向下取整混合改为 `(a|b)-(((a^b)&0xF7DE)>>1)` 的向上取整平均（25%/75% 再与新色/旧色平均一次）。IDB 与磁盘上的 exe 都看不到改动。gamemd-reverse 的 YRpp blitter 复刻描述的是未打补丁的原版。

## 正确做法
- 模型：RLE 帧、只测不写 Z、彩色（800）且 `BlittersFix::Applied` 时用 `ShapeDepth::RoundedBlend`；其余 blitter 用 `Blend`。Tinted 版本转调 Blit_Copy，同样受补丁影响。
- GPU：`RoundedShape` 标志，按通道 `(a+b+1)>>1`；F7DE 只对 565 等价，`HalfMask != F7DE>>1` 时跳过。
- 一般性：怀疑原生行为时，在进程内读补丁点字节（如 0x4987E0 起 0x60 字节）与 vtable，而不是只看 IDB；并先 grep 本仓库是否有 `DEFINE_DYNAMIC_PATCH`/`Patch::Apply_RAW` 改到该地址。

## 验证方法
4AED70 入口与调用返回点（42382C）各快照目标区域，拟合 40 个像素：只有向上取整平均 40/40 吻合；修正后 compositecheck `body=0`，GPU 对 native_composite 在爆炸区 0 差异（D3D12/Vulkan）。

## 附带教训
强制攻击光标（绿色箭头）会出现在 `gpu` 截图而不在 `native_composite` 中，容易被误认为动画碎片；对照前把鼠标移开（pw.py `move:x,y`）。

## 适用范围
所有经 4AED70 的半透明 SHP（动画、建筑半透明、后续单位/弹体），D3b 及之后的阶段。