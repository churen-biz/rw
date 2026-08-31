---
title: 第 15 章：端到端验收与交叉索引
linkTitle: 15 验收
description: 汇总 make test / demo，并链回 stack-vm 与设计博文。
weight: 150
draft: false
---

## 命令 {#commands}

```bash
make -C examples/stack-vm test
cd examples/svm-compiler
make test
make demo
make demo-factorial
make demo-mini
```

## 已接通路径 {#done}

| 路径 | 状态 |
| --- | --- |
| `.sasm` 全 opcode 面（至 capability） | 已实现 |
| Mini → IR → `.svm`（i32 / bool / while / call） | 已实现 |
| Mini 对象/闭包/异常/任务/host 语法 | 文档已铺路，实现按 10–14 章推进 |

## 交叉链接 {#links}

- VM 实现：[《C 栈式 VM》](/book/stack-vm/)
- ISA 设计：[设计栈式 VM](/blog/design-a-stack-vm/)
- 本系列设计 spec：`docs/superpowers/specs/2026-08-31-svm-compiler-series-design.md`
