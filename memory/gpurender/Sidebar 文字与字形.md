---
title: Sidebar 文字与字形
type: note
permalink: phobos/gpurender/sidebar-文字与字形
tags:
- gpurender
- sidebar
- text
- glyph
- uiname
---

# Sidebar 文字与字形

权威文档：`src/Render/SidebarTextGPU.md`；静态依据 gamemd-reverse #9 `sidebar-text-gpu-handoff.md`、#10 `sidebar-cameo-name-gpu-handoff.md`。

## 采集方式（`src/Render/SidebarText.cpp`）

- 项目作用域：`6A99C3` 开始，`6AA01C` 结束；每个重画的 cameo 以空 pending 开始，结束时原子替换旧 run（包括“本次没有文字”）。
- Simple print 作用域：`4A5EB0`/`4A5FF2`。用 `entryESP+428h` 处的 Fancy 上游返回地址过滤：数量 `6A9D80`、readiness `6A9E3B`、后置 HOLD `6AA019`。
- 字形：`434120` 观察每个字符的最终 pen X/Y、颜色、`font->Bounds`（inclusive，换算时 +1）和 `field_41`。目前只接受 SingleLine 返回地址 `43464D`。
- 底色：`621B85`（避开 Ares 在 `621B80` 的 hook），调用来源 `6A9D51/6A9E11/6A9F5E/6A9FF3`；黑色底色用 alpha 176/256 近似原生 `dst*80/256`。
- 层序：数量和 readiness 在闪烁之后、GCLOCK2 之前；暂停的 HOLD 在 GCLOCK2 之后。由调用点决定，不按文字内容判断。

## GlyphCache

- 字体和字形使用各自的强类型 ID，底层存储复用 FrameCache。原生 1bpp 记录按 MSB-first 展开为自有 0/1 mask。
- 缺字不是 `?`：原生用 X 字形取反作为 fallback（`434700`），颜色 XOR `5555h`。必须校验 fallback 等于 X 的取反，未初始化时拒绝。
- 字体绑定包含对象、数据、表、位图、fallback 身份和栅格格式。加载、fallback 重建和两个析构入口在替换或释放之前通知；ID 失效后永不复活。
- 上限：8 MiB mask、65536 记录槽、8 个字体、每次打印 512 个 UTF-16 单元、每个项目 1024 条命令。
- atlas 区分索引 sprite 和 mask；mask 不分配 palette，shader mode 4 把非零值映射为完全覆盖，颜色取命令自带的前景色。

## UIName（图标内名称）：暂缓

- 开关是 `MouseClass+53A4h`（`884B8Ch`），**非零才画名称**。原版构造时设为 0，原版没有切换入口；本机 Ares/Phobos/Spawner 二进制也没有写入者。所以当前配置下原生不显示名称。详见 [[经验 cameo 名称开关语义相反]]。
- 如果以后要做，按 #10 的方案：不新增游戏 hook，在 `434120` 观察器里加一个独立于 Prints 的名称分支。识别条件：direct return 为 `434EA6/4350E1/4352BA` 之一，`[B+54h]=6AC5B7`，`[B+C0h]=6A9BF1`，且 `B+CCh` 等于 ItemScope.Stack（B 为 Blit 入口 ESP）。
- 还需要：名称 clip 固定在 Sidebar surface 坐标（现有 `Append` 会随格子平移）；Fingerprint 加入开关值和名称代际。不要覆盖 Phobos 简报 hook `434CD0/434990`；它在正常路径上不会遗留 pending。

## Relations

- part_of [[GPURender 索引]]
- uses [[帧缓存与 atlas]]
- related [[Sidebar 元素状态]]
- documented_in [[gamemd-reverse 交接索引]]
