---
title: 发布可靠性
linkTitle: 发布可靠性
description: 用一个简单公式观察 Book 的公式编号。
book_number: '2.1'
weight: 10
---

## 可用性示例 {#availability}

发布流程的可用性可以用平均故障间隔与平均恢复时间表达：

$$
A = \frac{\mathrm{MTBF}}{\mathrm{MTBF} + \mathrm{MTTR}}
$$
{#eq-2-1 num="2-1" caption="由 MTBF 与 MTTR 计算可用性。"}

生产构建命令：

```bash
hugo --gc --minify --printPathWarnings --panicOnWarning
```
