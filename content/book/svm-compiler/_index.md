---
title: 从文本汇编到 Mini 编译器
linkTitle: SVM 编译器
description: 用 C11 实现 svm-as 与 Mini，把源程序编成可在教学栈式 VM 上运行的 .svm 模块。
type: book
weight: 20
book_kind: book
sidebar_root_for: self
sidebar_root_link_self: true
outputs: [HTML, print, markdown]
cascade:
  type: book
  book_draft_banner: true
  sidebar_headings: 3
---

这不是又一份 opcode 清单。我们先实现文本汇编器 `svm-as`，再实现中性小语言 Mini：经 AST 与带基本块的 IR，降低到同一套 SVM 指令并写出 `.svm`，由现有的 `svm-run` 加载执行。

设计说明见仓库内 [`docs/superpowers/specs/2026-08-31-svm-compiler-series-design.md`](/docs/superpowers/specs/2026-08-31-svm-compiler-series-design.md)（若站点未挂载该路径，请直接在 Git 仓库中打开）。

## 两条书脊 {#two-spines}

```text
阶段 A  .sasm  ──►  svm-as  ──►  .svm  ──►  svm-run
阶段 B  .mini  ──►  AST → CFG/IR → lower  ──►  同一编码后端
```

阶段 A 让你看清「指令与模块长什么样」；阶段 B 解释「高级语法如何落到这些指令」。执行与验证仍由 [`《从第一行 C 到完整栈式 VM》`](/book/stack-vm/) 中的参考实现负责——本系列不重写 VM。

## 阅读前准备 {#prereq}

```bash
make -C examples/stack-vm all test
cd examples/svm-compiler
make test
```

## 章节 {#chapters}

{{< book-toc depth=3 >}}
