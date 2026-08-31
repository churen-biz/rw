---
title: 第 14 章：import 与 host 调用
linkTitle: 14 capability
description: Mini 的 import/host 与 asm .import/host_call 对齐；权限仍由宿主授予。
weight: 140
draft: false
---

对照 `host_add.sasm`。源码声明 `import math add(...)`；生成 `SvmHostImport` 与 `host_call`。运行时绑定见测试中的 `svm_execute_with_capabilities`。
