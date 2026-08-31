---
title: 第 8 章：AST 到栈式 CFG/IR
linkTitle: 08 CFG 与 IR
description: 把控制流拆成基本块；块内指令按栈语义排列，跨块只用具名局部。
weight: 80
draft: false
---

IR 故意贴近 SVM：块内 `const`/`load`/`store`/`binary`/`call` 作用于**操作数栈**；`goto`/`branch`/`ret` 结束基本块。

不在此引入 SSA φ。循环回边因此不会因“临时槽类型合并”失败——表达式结果不占用长期局部槽。

实现：`src/ir/build.c`。
