---
title: 从第一行 C 到完整栈式 VM
linkTitle: C 栈式 VM
description: 面向 VM 初学者，用可运行的 C11 代码逐步实现验证器、对象、异常、GC、并发和沙箱。
type: book
weight: 10
book_kind: book
sidebar_root_for: self
sidebar_root_link_self: true
outputs: [HTML, print, markdown]
cascade:
  type: book
  book_draft_banner: true
  sidebar_headings: 3
---

这不是一份只罗列 opcode 的设计文档。我们从一个空的 `vm.c` 开始，每章只增加一组相互依赖的能力，并用成功测试和失败测试证明它们真的工作。最终得到的是一台教学级但端到端完整的多语言栈式 VM。

## 最终会实现什么 {#outcome}

- 可验证的模块和类型化栈式字节码；
- 函数、对象、数组、闭包与动态值；
- 异常、`defer` 和跨帧展开；
- 停止世界标记清除 GC；
- 轻量任务、`await`、channel 与 `select`；
- capability 宿主接口和不可绕过的资源配额；
- 模块文件加载、错误诊断和自动化负面测试。

“完整”指这些路径都有真实 C 代码和端到端测试，不代表兼容 JVM、V8 或 Go runtime，也不包含 JIT 和性能优化。

## 阅读方法 {#how-to-read}

按顺序阅读。每章开始前保留上一章的可运行状态，完成本章后执行：

```bash
cd examples/stack-vm
make test
```

只有测试全部通过才继续下一章。完整功能边界记录在仓库的 [`SPEC.md`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/SPEC.md) 中。

## 章节 {#chapters}

{{< book-toc depth=3 >}}
