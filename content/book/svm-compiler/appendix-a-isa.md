---
title: 附录 A：ISA 与模块格式速查
linkTitle: 附录 A ISA
description: 供本系列自洽阅读的指令与类型速查；完整语义以 stack-vm SPEC 为准。
weight: 90
draft: false
---

本附录随章节扩充。完整 opcode、验证规则与二进制布局以 [`examples/stack-vm/SPEC.md`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/SPEC.md)、[`stack-vm` 教程](/book/stack-vm/) 与[设计博文](/blog/design-a-stack-vm/)为准。

## 1. 类型（摘录） {#types}

| 名称 | 含义 |
| --- | --- |
| `i32` | 32 位有符号整数 |
| `bool` | 布尔 |
| `ref` | 托管堆引用 |
| `any` | 带标签的动态值 |
| `task` / `channel` | 任务与通道句柄 |
| `void` | 无结果 |

Plan 2 汇编器结果/参数类型目前支持：`i32`、`bool`。

## 2. 助记符（Plan 1–2） {#ops}

| 助记符 | Opcode | 栈效应（摘要） |
| --- | --- | --- |
| `const_i32 IMM` | `CONST_I32` | `→ i32` |
| `const_true` / `const_false` | `CONST_TRUE` / `CONST_FALSE` | `→ bool` |
| `load_local N` | `LOAD_LOCAL` | `→ local` |
| `store_local N` | `STORE_LOCAL` | `value →` |
| `dup` / `pop` | `DUP` / `POP` | 复制 / 丢弃栈顶 |
| `i32_add` / `sub` / `mul` | 算术 | `i32,i32 → i32` |
| `i32_le` / `i32_eq` | 比较 | `i32,i32 → bool` |
| `jump L` | `JUMP` | pc ← L |
| `jump_if_false L` | `JUMP_IF_FALSE` | 弹出 bool，假则跳转 |
| `call F` | `CALL` | 按 F 的参数个数调整栈并压入结果 |
| `return` | `RETURN` | 返回栈顶结果 |

跳转目标为函数内指令下标。`call` 要求被调函数已在源文件中靠前定义。

## 3. 模块文件 {#module}

魔数 `SVM\0`，带 section 目录；操作数为 signed LEB128。加载后必须先验证再执行。教学加载器入口：`svm_module_read_file` / `svm-run`。

后续章节继续补全对象、闭包、异常、任务与 `host_call` 等助记符。
