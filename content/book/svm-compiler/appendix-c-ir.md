---
title: 附录 C：IR 操作一览
linkTitle: 附录 C IR
description: 栈式教学 IR 操作码。
weight: 92
draft: false
---

| IR | 含义 |
| --- | --- |
| `CONST_I32` / `CONST_BOOL` | 压栈 |
| `LOAD` / `STORE` | 局部 ↔ 栈 |
| `BINARY` | 弹出二值、压入结果 |
| `CALL` | 按参数个数弹栈并压入返回值 |
| `POP` | 丢弃栈顶 |
| `GOTO` / `BRANCH` | 终结块（branch 先弹 bool） |
| `RET` / `RET_VOID` | 返回 |
