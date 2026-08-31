---
title: 第 11 章：闭包与 any
linkTitle: 11 闭包
description: 嵌套函数降低为 new_closure/call_closure；动态值用受控 any。
weight: 110
draft: false
---

对照 `stack-vm` 闭包测试与 asm `.captures` / `new_closure` / `call_closure`。Mini 侧将嵌套 `fn` 转为带捕获的函数声明，调用点生成闭包创建与 `call_closure`。`any` 仅允许显式汇合，避免无声类型变宽。
