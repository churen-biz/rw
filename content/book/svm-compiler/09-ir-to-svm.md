---
title: 第 9 章：IR 降到 SVM 并写出 .svm
linkTitle: 09 Lowering
description: 将栈式 IR 映射到 ModuleBuilder，经 mini 工具生成可运行模块。
weight: 90
draft: false
---

`sc_ir_lower` 把每个基本块标成 `bN:`，`branch` 降为 `jump_if_false` + `jump`。

```bash
cd examples/svm-compiler
make demo-mini
# i32:42 （入口为 main，下标 1）
```

`loop_sum.mini` 验证 while lowering。工具入口：`tools/mini.c`。
