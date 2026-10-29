---
title: 帧缓存与 atlas
type: note
permalink: phobos/gpurender/帧缓存与-atlas
tags:
- gpurender
- atlas
- framecache
- palette
---

# 帧缓存与 atlas

权威文档：`src/Render/FrameAtlasDesign.md`（设计）、`FrameCache.md`（阶段 1 CPU 缓存）、`FrameAtlas.md`（GPU atlas）。

## 数据路径

`6A70AE → SidebarSnapshot::CopyAsset → ShapeFrameSource::Acquire → FrameCache::Cache → FrameAtlas`

- `ResourceID` 和 `FrameID` 是不同的强类型，64 位进程内递增序列，不复用，Clear/新对局都不重置。0 表示无效。
- 资源身份按当前已加载的 Data 地址查找，但只在明确的生命周期通知之间有效；地址复用不会复活旧 FrameID。
- 首次请求某帧时才解码为自有 8 位索引，warm path 只检查头部和元数据，不扫描像素、不做 hash。未加载的资源不会主动 Load。
- 快照保存 FrameID、不可变 PaletteID 和布局，不保存 GPU 位置。GPU/Timer 只读自有数据，不解引用引擎指针。

## GPU atlas

- 默认 2048² 页，优先 `R8_UINT`，不支持时回退 `R32_UINT`。shelf 只追加、不搬移；FrameID 与页位置无关。
- `IndexedSprite` shader 按整数 Load 取索引，再查 256×1 的 R32_UINT palette。索引 0 透明，非零索引即使是黑色也不透明。整数纹理只能用 nearest、不能开 mipmap。
- 三个 staging 槽，各有提交 fence；忙时跳过本次 present，不等待 GPU。行距对齐 256 字节，偏移对齐 512 字节。追加区域用 `cycle=false`。
- 即使 swapchain 为空也要提交上传并取得 fence。获取 swapchain 后不能取消 command buffer。
- 预算：最多 16 页、合计 64 MiB；palette 最多 256 个；每个 staging 槽 16 MiB。超预算时明确缺失该元素，其他元素照常显示。
- SDL 3.4.18 的 D3D12 后端查询 SAMPLER 能力时会对整数格式返回 false，所以该后端改用 GRAPHICS_STORAGE_READ 查询；资源本身仍按 SAMPLER 绑定，shader 只用 Load。

## 生命周期边界

| 事件 | 行为 |
| --- | --- |
| Sidebar Init/Unload、Scenario Clear、UI SHP 清理 | 清空 CPU 资源/帧/别名表并撤销快照，ID 序列不重置 |
| Radar Dispose | 只撤销 Radar 按钮资源 |
| ShapeButton SetShape/ClearShape | owned 素材会直接 free，在入口先撤销 |
| SHP Unload/析构/删除析构 | 让该资源和所有帧失效 |
| UI palette 清理 | 撤销读取许可，不清帧索引 |
| 布局/视频模式、GPU 释放 | 撤销快照，不改 CPU ID |

GPU 重建只清驻留映射，CPU FrameID 不变；页和 palette 必须等最后引用它的提交完成后才能退役。

## 验收要点

- 稳定区间看 `[FrameAtlas]` 汇总：`indexDelta=0`、`paletteDelta=0`、`decodedDelta=0`（连续 300 次提交零新增）。
- 第二局应分配新 FrameID，旧页被回收。

## Relations

- part_of [[GPURender 索引]]
- used_by [[Sidebar 元素状态]]
- used_by [[Sidebar 文字与字形]]
