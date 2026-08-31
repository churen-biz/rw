---
title: 附录 A：ISA 与模块格式速查
linkTitle: 附录 A ISA
description: 供本系列自洽阅读的指令与类型速查；完整语义以 stack-vm SPEC 为准。
weight: 90
draft: false
---

完整语义以 [`SPEC.md`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/SPEC.md) 与 [`stack-vm` 教程](/book/stack-vm/) 为准。

## 模块级指令 {#directives}

| 指令 | 含义 |
| --- | --- |
| `.type Name { f: T, ... }` | 记录类型 |
| `.import [async] Cap Fn(args) -> T` | 宿主导入 |
| `.func Name(args)? -> T` … `.end` | 函数 |
| `.captures N` | 闭包捕获数 |
| `.handler S E H` | 异常表（标签） |

## 类型名 {#types}

`i32` `bool` `ref` `any` `void` `task` `channel`

## 常用助记符 {#ops}

控制与调用：`jump` `jump_if_false` `call` `return` `task_spawn` `task_await` `task_yield`  
堆：`new_object` `get_field` `set_field` `new_array` `array_*` `new_closure` `call_closure` `gc_collect`  
异常：`throw` `defer_push`  
并发：`channel_new` `channel_send` `channel_recv` `channel_close` `channel_select T N`  
宿主：`host_call Cap Fn` `host_call_async Cap Fn`

跳转目标为函数内 pc；`call`/`spawn`/`closure` 要求被调函数已定义。
