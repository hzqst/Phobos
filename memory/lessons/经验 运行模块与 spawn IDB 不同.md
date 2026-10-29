---
title: 经验 运行模块与 spawn IDB 不同
type: note
permalink: phobos/lessons/经验-运行模块与-spawn-idb-不同
tags:
- lesson
- reverse
- spawner
- deployment
---

# 经验 运行模块与 spawn IDB 不同

- **触发信号**：gamemd-reverse 交接里出现 `B7xxxx` 跳岛、`BAD3E8` 之类的全局，或某个地址在运行模块里的指令与交接描述不一致。
- **根因/约束**：gamemd-reverse 的 IDB 输入是 CnCNet 补丁版 `gamemd-spawn.exe`（SHA256 `2f86e7…`，本机没有这个文件）。隔离副本实际运行的是原版 `gamemd.exe`（`7b8a06…`），由 Syringe 加载 Ares 3.0p1、Phobos 和 CnCNet-Spawner.dll。本机另有一个 spawn 版（`247f72…`），和 IDB 版也不同：两者都补丁的 193 处跳转目标不同，另有 15 处只有 IDB 版有补丁、2 处只有本地版有。三者的原版代码部分完全一致，差异只在约 240–250 处补丁点，以及 `.p_*` 补丁段。
- **正确做法**：
  - 运行模块固定为 `gamemd.exe` + CnCNet-Spawner.dll；
  - 交接里引用的地址先用运行模块的字节核对，不在补丁点上的可以直接使用；
  - 落在补丁点上的要按原版语义重新确认。例如雷达模式：原版读 `+14B0`，spawn 版读 `BAD3E8`。
- **不要换成 spawn 版运行（2026-10-30 实测）**：
  - 窗口化原本靠 Spawner DLL 的 `Video.Windowed`，移掉 DLL 后变成全屏；
  - spawn 版会把 `spawn.ini` 的 `Scenario` 改写成 `mp.dat`；
  - Ares 有 13 个 hook 与补丁点重叠，Syringe 多报 7 处字节解码失败；
  - 游戏在 `SetDisplayMode` 之后退出，没有进入对局。
- **验证方法**：用 Python/capstone 反汇编运行模块的对应地址；解析三个 DLL 的 `.syhks00` hook 表，检查是否和补丁点重叠。证据在 `Debug/AutomationTests/spawn-feasibility/`。
- **适用范围**：所有基于 gamemd-reverse 交接的 GPURender 接入与 hook 部署。

## Relations

- part_of [[GPURender 索引]]
- related [[gamemd-reverse 交接索引]]
- related [[实机验收流程]]
