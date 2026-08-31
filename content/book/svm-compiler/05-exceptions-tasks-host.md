---
title: 第 5 章：异常、任务、channel 与 capability
linkTitle: 05 异常并发与宿主
description: 补齐 .handler、throw、task/channel 与 .import/host_call，完成阶段 A 可手写全 opcode 面。
weight: 50
draft: false
---

## 1. 异常表 {#handler}

```text
.func main -> i32
.handler try_start try_end catch
try_start:
  call thrower
try_end:
  return
catch:
  pop
  const_i32 42
  return
.end
```

`start`/`end`/`handler` 均为标签；`end` 为半开区间上界（与参考 VM 一致）。`throw` 弹出 `any`（`ref` 等可汇入）。见 `catch_throw.sasm`。

## 2. 任务与 channel {#channel}

```text
.func producer(ch: channel) -> void
  load_local 0
  const_i32 42
  channel_send i32
  return
.end
```

`task_spawn producer` 弹出实参、压入 `task`；示例里 `pop` 掉句柄后 `channel_recv`。见 `channel_ping.sasm`（入口为 `main`，下标 1）。

## 3. Capability {#host}

```text
.import math add(a: i32, b: i32) -> i32

.func main -> i32
  const_i32 2
  const_i32 3
  host_call math add
  return
.end
```

源码只**声明**导入。测试用 `svm_execute_with_capabilities` 绑定名为 `math` 的宿主函数；未授予时实例化/执行失败。异步用 `.import async ...` 与 `host_call_async`。

## 4. 闭包相关助记符 {#closure}

- `.captures N`：标记函数捕获个数  
- `new_closure F [N]`、`call_closure F`、`defer_push F`  

与参考测试中的闭包/`defer` 布局一致；可按 `stack-vm` 第 3–4 章对照手写。

## 5. 阶段 A 收束 {#done}

至此 `svm-as` 可表达教学 ISA 的主路径。下一阶段进入 Mini：AST → CFG/IR → 同一 `ModuleBuilder`。

```bash
cd examples/svm-compiler
make test
```
