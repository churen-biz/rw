---
title: 第 13 章：spawn / chan / select
linkTitle: 13 并发
description: 将 spawn 与 channel 操作降到任务与 channel 指令。
weight: 130
draft: false
---

对照 `channel_ping.sasm`。`spawn f(args)` → 实参压栈 + `task_spawn`；`chan T(cap)` → `channel_new`；`send`/`recv`/`select` 映射同名助记符。
