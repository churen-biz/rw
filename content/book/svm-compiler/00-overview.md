---
title: 第 0 章：目标、边界与阅读地图
linkTitle: 00 概览
description: 说明本系列要交付的编译链路、刻意不做的事，以及与栈式 VM 教程的关系。
weight: 5
draft: false
---

## 1. 我们要建成什么 {#goal}

最终读者应能：

1. 用手写 `.sasm` 描述函数与指令，经 `svm-as` 得到可验证的 `.svm`；
2. 用 Mini 书写结构化程序，经解析、类型检查、CFG/IR 与 lowering 得到同一格式；
3. 覆盖对象、闭包、异常、`defer`、任务/channel 与 capability 导入等特征的 **lowering**（权限仍由宿主授予）。

「完整」指这条前端链路可测、可演示，不表示兼容真实 Go/JS/Java。

## 2. 刻意不做 {#non-goals}

- JIT、SSA φ 网络、激进优化；
- 自举编译器或生产 ABI；
- 在本系列里再实现一遍 GC/调度器/验证器。

需要理解指令 **为何** 这样设计时，阅读 [`stack-vm`](/book/stack-vm/) 与 [`SPEC.md`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/SPEC.md)。本系列附录 A 提供速查，保证即使先读编译器书也能跟下来。

## 3. 管线鸟瞰 {#pipeline}

```text
.sasm ──► Lexer/Parser ──► ModuleBuilder ──► svm_module_write_file
.mini ──► AST ──► Typecheck ──► CFG/IR ──► Lower ──┘
```

Plan 1（当前代码）只实现虚线框左侧的最小汇编路径：`const_i32` / `i32_add` / `return`。后续章节沿同一 `ModuleBuilder` 加厚。

## 4. 如何使用附录 {#appendices}

- 附录 A：ISA 与 `.svm` 速查（随章节扩充）；
- 附录 B：Mini 语法一页纸（阶段 B）；
- 附录 C：IR 操作一览（阶段 B）。

下一章动手写出第一个 `.sasm` 并跑出 `i32:42`。
