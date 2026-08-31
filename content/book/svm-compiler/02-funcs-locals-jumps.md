---
title: 第 2 章：函数、局部变量与跳转
linkTitle: 02 函数与跳转
description: 扩展 svm-as：带参数的多函数、标签回填、call，并汇编出递归阶乘与循环求和。
weight: 20
draft: false
---

第一章只能写「一个无参 main」。真实程序需要局部变量、条件跳转和函数调用。本章把 `.sasm` 升到 v1，并要求**被调用函数写在调用者之前**（签名在 `.func` 行就注册，便于计算 `call` 的栈效应）。

## 1. v1 语法增量 {#grammar}

```text
.func NAME (p: TYPE, ...)? -> TYPE
  load_local N
  store_local N
  jump LABEL
  jump_if_false LABEL
  call CALLEE
LABEL:
  ...
.end
```

参数占据局部变量下标 `0 .. param_count-1`。`local_count` 取「参数个数」与「出现过的最大局部下标 + 1」的较大值。

跳转操作数是 **指令下标（绝对 pc）**，与 SVM 参考实现一致。汇编器在 `.end` 时回填标签。

## 2. 阶乘示例 {#factorial}

`tests/fixtures/factorial.sasm`：先定义 `factorial`，再定义 `main`。默认 `svm-run` 从下标 0 进入，因此运行 main 需要显式入口：

```bash
cd examples/svm-compiler
make demo-factorial
# 等价于 svm-run build/factorial.svm 1
```

期望输出：`i32:120`。

循环求和见 `loop_sum.sasm`（`1+…+5 = 15`），入口仍为函数 0。

## 3. 实现要点 {#impl}

| 机制 | 位置 |
| --- | --- |
| `(` `)` `,` `:` 词法 | `src/asm/lex.c` |
| 签名注册、`emit_call`、标签回填 | `src/emit/builder.c` |
| 多 `.func`、助记符表 | `src/asm/parse.c` |

`call` 的栈深度变化为 `1 - callee.param_count`。若被调函数尚未出现，报错 `define callee before caller`。

未定义标签在 `.end` 时报错（见 `bad-label.sasm`）。

## 4. 测试 {#test}

```bash
cd examples/svm-compiler
make test
```

除第一章用例外，还包括 builder/汇编阶乘、循环求和，以及 `scripts/run-factorial.sh`。

## 5. 下一步 {#next}

第 3 章会把更多 opcode 接到同一套文本语法，并对照验证器的失败信息；对象与数组语法留到第 4 章。
