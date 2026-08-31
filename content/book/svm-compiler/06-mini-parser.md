---
title: 第 6 章：Mini 语法与 AST
linkTitle: 06 Mini 解析
description: 递归下降解析中性小语言 Mini，得到模块与函数的 AST。
weight: 60
draft: false
---

阶段 B 从这里开始：源语言 `Mini` → AST → 类型检查 → 栈式 CFG/IR → 既有 `ModuleBuilder`。

## 最小语法 {#grammar}

```text
module demo
fn add(a: i32, b: i32) -> i32 { return a + b; }
fn main() -> i32 { return add(40, 2); }
```

支持：`let`、赋值、`if`/`else`、`while`、调用、`i32`/`bool`/`void`。注释用 `//`。

实现：`src/mini/parse.c`。被调函数仍须写在调用者之前（与 asm 相同）。
