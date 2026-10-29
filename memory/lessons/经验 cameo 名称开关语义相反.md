---
title: 经验 cameo 名称开关语义相反
type: note
permalink: phobos/lessons/经验-cameo-名称开关语义相反
tags:
- lesson
- uiname
- reverse
- sidebar
---

# 经验 cameo 名称开关语义相反

- **触发信号**：准备实现图标内 UIName，或看到 YRpp 字段 `SidebarClass::HideObjectNameInTooltip` / `CCToolTip::HideName`（`884B8Ch` = `MouseClass@87F7E8 + 53A4h`）。
- **根因/约束**：同一个字节有两个用途。`6AC480`（图标内名称）在它为 0 时直接返回，即**非零才画名称**；原版 tooltip `6A9343` 在它非零时走隐藏名称的分支。YRpp 的命名只反映 tooltip 语义。原版构造 `6A4E60` 把它设为 0，`Init_Clear` 不重置，存读档整块保存；原版没有切换入口。2026-10-29 扫描本机 `D:/Ra2` 的 Ares.dll、Phobos.dll、CnCNet-Spawner.dll：Ares 唯一的绝对地址引用 `100359E4` 是 `cmp`（超武 tooltip 读取），所有 `+53A4h` 访问要么是 DWORD 操作（属于其他结构），要么是字节读取，没有写入者。
- **正确做法**：在当前配置下，图标内名称原生就不显示，不要为它投入实现；需要时按 gamemd-reverse #10 的方案接入，并同时观察开关值，关闭时立即过滤名称命令。
- **验证方法**：二进制扫描 `8C 4B 88 00`（绝对地址）和 `A4 53 00 00`（偏移），再用 capstone 反汇编确认是读取还是写入。
- **适用范围**：本机 YR + Ares 3.0p1 + Phobos + Spawner 0.0.0.17。扫描覆盖不到通过计算指针写入或读档带入的情况。

## Relations

- part_of [[Sidebar 文字与字形]]
