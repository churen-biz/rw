---
title: 第 12 章：throw / try / defer
linkTitle: 12 异常
description: Mini 异常语法降低为 throw、.handler 与 defer_push。
weight: 120
draft: false
---

对照 `catch_throw.sasm`。`try { ... } catch e: T { ... }` 生成 `.handler` 标签区间；`defer` 语句降低为创建清理闭包 + `defer_push`。
