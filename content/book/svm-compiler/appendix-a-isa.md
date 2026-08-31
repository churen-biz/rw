---
title: 附录 A：ISA 与模块格式速查
linkTitle: 附录 A ISA
description: 供本系列自洽阅读的指令与类型速查；完整语义以 stack-vm SPEC 为准。
weight: 90
draft: false
---

本附录随章节扩充。Plan 1 仅列出当前 `svm-as` 用到的片段；完整 opcode、验证规则与二进制布局以 [`examples/stack-vm/SPEC.md`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/SPEC.md)、[`stack-vm` 教程](/book/stack-vm/) 与[设计博文](/blog/design-a-stack-vm/)为准。

## 1. 类型（摘录） {#types}

| 名称 | 含义 |
| --- | --- |
| `i32` | 32 位有符号整数 |
| `bool` | 布尔 |
| `ref` | 托管堆引用 |
| `any` | 带标签的动态值 |
| `task` / `channel` | 任务与通道句柄 |
| `void` | 无结果 |

## 2. Plan 1 助记符 {#plan1-ops}

| 助记符 | Opcode | 栈效应 |
| --- | --- | --- |
| `const_i32 IMM` | `SVM_OP_CONST_I32` | `[] → [i32]` |
| `i32_add` | `SVM_OP_I32_ADD` | `[i32,i32] → [i32]` |
| `return` | `SVM_OP_RETURN` | 返回栈顶结果 |

## 3. 模块文件 {#module}

魔数 `SVM\0`，带 section 目录；操作数为 signed LEB128。加载后必须先验证再执行。教学加载器入口：`svm_module_read_file` / `svm-run`。

后续章节会在此表中补全对象、闭包、异常、任务与 `host_call` 等助记符。
