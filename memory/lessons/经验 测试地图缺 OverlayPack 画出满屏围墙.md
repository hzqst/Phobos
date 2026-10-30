---
title: 经验 测试地图缺 OverlayPack 画出满屏围墙
type: note
permalink: phobos/lessons/经验-测试地图缺-overlaypack-画出满屏围墙
tags:
- lesson
- testing
- terrain
---

# 经验 测试地图缺 OverlayPack 画出满屏围墙

- **触发信号**：从现成地图删掉 `OverlayPack`/`OverlayDataPack` 做“无覆盖物”的受控地图后，clear tile 区域在原生画面（ddraw 与 `native_terrain`）里是逐格重复的灰色碎石纹理；GPU 只画 TMP，显示为平滑雪地，整片不一致。原生像素只有约 1/3 能在该格 LightConvert 的任何 shade 行里找到，ABuffer 却全为 127。悬崖格（不能放 overlay）反而一致。
- **根因/约束**：地图缺 `OverlayPack` 时格子的 overlay 不是“无”。补上全空 overlay 后问题消失，可确认原因在 overlay；推断每格带了 0 号 overlay，由地形缓存阶段 `6D6D10 → 47F6A0` 画进 Tile，用 overlay 自己的 Convert，所以颜色不在地形颜色表里。没有直接读格子的 `OverlayTypeIndex` 核对具体类型。
- **正确做法**：受控地图要显式写入全空的 overlay：`OverlayPack` 为 262144 字节全 `0xFF`、`OverlayDataPack` 为全 `0x00`。格式是每 8192 字节一块，块头为两个 uint16（压缩长度、原长），块体为 LCW 填充命令 `FE <len16> <value> 80`，整体 base64 后按 70 字符一行写 `1=`、`2=`……。生成脚本为 `Debug/AutomationTests/stage-c/scripts/make_maps.py`（输出 `GPUTERR.MAP`/`GPUFLAT.MAP` 到隔离副本）。去掉对象时同时删掉原有触发器，揭示全图可加“事件 13、参数 0 + 动作 16”的触发器。
- **验证方法**：补上空 overlay 后，`native_terrain` 与 `gpu` 的战术区除光标外最大差 1；排查时可把原生像素按候选 shade 行反查索引，再在全部地块 piece 中找 900/900 吻合项。
- **适用范围**：用 Spawner 加载自制或裁剪地图做地形/覆盖物对照的实机测试；裁剪 `IsoMapPack5` 时同理需确认格子默认值。

## Relations

- part_of [[实机验收流程]]
- relates_to [[GPURender 索引]]
