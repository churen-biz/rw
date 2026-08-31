---
title: 第 3 章：对照验证器扩展指令面
linkTitle: 03 验证器对照
description: 说明汇编器与验证器的分工，并用非法栈类型模块证明加载前拒绝。
weight: 30
draft: false
---

汇编器负责把助记符变成 `SvmInstruction`；**类型安全仍由 VM 验证器保证**。本章强调这条边界：前端可以生成“形状合法、语义错误”的字节码，但 `svm_verify_module` / `svm-run` 必须拒绝它。

## 1. 分工 {#roles}

| 层 | 抓住什么 |
| --- | --- |
| `svm-as` | 未知助记符、缺标签、未知函数/类型名、栈深度粗检查 |
| 验证器 | 栈类型、跳转目标、调用签名、字段下标、非法控制流 |

不要指望汇编器复刻完整抽象解释；它的价值是**可手写、可调试**的文本前端。

## 2. 失败用例 {#bad-types}

`tests/fixtures/bad-types.sasm`：

```text
.func main -> i32
  const_true
  const_i32 1
  i32_add
  return
.end
```

汇编成功，但验证失败，消息含 `expected stack type`。测试 `test_verify_rejects_bad_types` 固定这一行为。

## 3. 运行 {#run}

```bash
cd examples/svm-compiler
make test
```

下一章接入 `.type` 与对象/数组助记符，用堆上的真实例子代替抽象讨论。
