---
title: 第 7 章：名称解析与类型检查
linkTitle: 07 类型检查
description: 为 Mini AST 填充类型，拒绝错误的 arity、分支条件与返回类型。
weight: 70
draft: false
---

`sc_mini_typecheck` 在每个函数内维护局部符号表，并核对：

- 变量先定义后使用；
- 算术/比较操作数类型；
- `if`/`while` 条件为 `bool`；
- 调用参数与返回类型。

失败示例：`return true` 在 `-> i32` 函数中会被拒绝（见 `tests/test-mini.c`）。
