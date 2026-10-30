---
title: 经验 用 Visibility 与 Foggedness 判别 ABuffer 差异来源
type: note
permalink: phobos/gpurender/经验-用-visibility-与-foggedness-判别-abuffer-差异来源
tags:
- gpurender
- experience
- abuffer
- terrain
---

# 经验 用 Visibility 与 Foggedness 判别 ABuffer 差异来源

## 触发信号

按当前格子标志整屏重建的 ABuffer（`ABufferSnapshot`）与 `4F4583` 时的原生 ABuffer 逐像素比较，单位移动、迷雾变化时出现几百到上万像素的差异，停下后自行归零。

## 根因与约束

原生只对 VisibleCells 里的格子重跑 `4801F0`。一个格子自身标志不变、只是邻格被揭开（掩码改变）时不会入队，旧帧会保留几帧。`4801F0` 每次执行都把两次 `6D8700` 的原始返回值存进 Cell `Visibility+120`/`Foggedness+121`，所以这两个字节记录的是原生最后一次重画该格时的结果。

## 正确做法

每个写入格同时计算当前取帧结果，并读出存储的 `Visibility/Foggedness`；两者不同即为“原生尚未重画”（trace 的 `stale`）。差异像素所属格子的存储值与当前值不同，就是原生缓存滞后，不是模型错误。不要用 `stale>0` 的样本判定模型对错。

## 验证方法

C4 FogOfWar 两轮共 96 次采样：`stale=0` 的全部 `mismatched=0`；`stale>0` 的全部有差异，首个差异像素都落在 stale 格上（如当前 `-1/1`、存储 `-1/3`）。

## 适用范围

战术区 ABuffer shroud/fog 对照；同理适用于其他“原生增量缓存 vs 每帧完整重建”的对照，只要原生在重画时留有可读的状态记录。

## Relations

- part_of [[GPURender 索引]]
