---
title: 经验 Spawner 预置建筑不生效
type: note
permalink: phobos/lessons/经验-spawner-预置建筑不生效
tags:
- lesson
- spawner
- testing
---

# 经验 Spawner 预置建筑不生效

- **触发信号**：用 Spawner（`-SPAWN`）加载自制地图，`[Structures]` 中写了 `Multi1,GACNST,...` 等预置建筑，进局后只有 MCV，sidebar 的 cameo count=0。
- **根因/约束**：Spawner 遭遇战下，归属 `Multi1` 的地图预置建筑没有被分配给玩家（`MultiplayerOnly=0/1` 都试过，均不生效）。复核 2026-10-28 文字回归测试的日志，开局 count 同样是 0，当时的建造厂其实也是展开 MCV 得来的。具体机制没有做静态核实。
- **正确做法**：开局后用 Automation 按 `N` 选中 MCV（`game_status.input.selection_types == ["AMCV"]`），再按 `D` 展开（以隔离副本的 `KeyboardMD.ini` 为准）。需要更多 cameo 时，在地图里写类型规则覆盖（`Owner`/`RequiredHouses` 放开、`Prerequisite=GACNST`、`TechLevel=1`），不要依赖预置建筑。
- **验证方法**：`PHOBOS_SIDEBAR_TRACE=1` 日志中 `[SidebarCameoGPU] ... count=N` 在展开后增长，并截图确认。
- **适用范围**：Phobos 隔离副本 + CnCNet Spawner 0.0.0.17 + Ares 3.0p1 的实机测试。

## Relations

- part_of [[实机验收流程]]
