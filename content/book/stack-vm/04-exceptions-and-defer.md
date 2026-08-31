---
title: 第 4 章：异常、跨帧展开与 defer
linkTitle: 04 异常与清理
description: 用异常处理表实现 throw/catch，把正常返回与异常传播统一为帧退出状态机，并保证 defer 逆序且只执行一次。
weight: 40
draft: false
---

本章加入两条指令：`throw` 抛出客体值，`defer_push` 注册一个无显式参数的清理闭包。困难不在于跳转，而在于三个动作必须组合正确：寻找处理器、逐帧退出、执行每帧的清理项。

## 1. 为什么 try 不需要成为指令 {#handler-table}

函数用元数据描述受保护范围：

```c
typedef struct {
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t handler_pc;
} SvmExceptionHandler;
```

范围采用半开区间 `[start_pc, end_pc)`。正常执行不需要维护“当前进入了几个 try”；只有异常发生时，VM 才查找包含抛出位置的第一条处理器记录。

加载器必须拒绝空区间、越过函数末尾的区间和无效处理器地址。处理器入口的操作数栈被重置为一个 `any` 异常值，因此验证器也会为受保护区内的指令增加异常控制流边。

## 2. any 是验证器类型，不是新的运行时标签 {#any}

JavaScript 的 `throw 42` 和 Go 的 `panic(value)` 都要求异常不局限于对象引用。验证器增加 `SVM_TYPE_ANY`，它接受任意已经初始化且有实际值的类型。

运行时仍保存 `SvmValue` 原本的 `I32`、`BOOL`、`REF` 或 `NULL` 标签，并不会把值改写成虚构的 `ANY` 标签。`ANY` 只表示“这个控制流位置允许多种运行时标签汇合”。

## 3. throw 的验证语义 {#verify-throw}

```text
throw    any -- 不存在正常后继
```

如果当前位置存在处理器，验证器创建以下异常状态：

1. 保留当前局部变量类型；
2. 丢弃原操作数栈；
3. 压入一个 `ANY`；
4. 把状态传播到 `handler_pc`。

函数调用、对象访问等操作未来也可能产生可捕获的客体异常，因此验证器保守地为受保护区中的所有指令添加同样的异常边。宿主内存损坏、非法 opcode 和验证失败不走这条路径，它们属于不可恢复 trap。

## 4. defer 保存闭包，而不是函数编号 {#defer-closure}

`defer_push function_index` 从栈中弹出闭包引用。指令中的函数索引用于静态验证和运行时二次核对；真正保存到帧里的是闭包，因为它还携带注册时已经求值的捕获参数。

第一版要求 defer 目标满足：

- `parameter_count == capture_count`，即没有调用时才提供的显式参数；
- 返回类型是 `void`；
- 实际闭包中的函数编号与指令声明一致；
- 每个帧的注册数量不超过 `SVM_MAX_DEFERS`。

这些限制足以表达 `defer close(file)`：创建一个捕获 `file` 的无参闭包，再执行 `defer_push`。

## 5. 把返回和抛出统一为退出状态 {#exit-state}

每个帧增加：

```c
SvmValue defers[SVM_MAX_DEFERS];
uint32_t defer_count;
SvmExitKind exit_kind;   /* NONE、RETURN 或 THROW */
SvmValue exit_value;     /* 返回值或异常值 */
```

`return` 和无法在当前帧捕获的 `throw` 都不立即删除帧。它们先写入退出原因，再调用同一个 `continue_frame_exit`：

```text
若还有 defer：
    取出最后注册的闭包，创建一个“清理调用帧”，暂停原退出过程
否则：
    删除当前帧
    RETURN → 把结果交给调用者
    THROW  → 在调用者的调用位置查找处理器；找不到则继续退出调用者
```

因此正常返回和异常展开天然共享 LIFO 清理规则，不需要复制两套循环。

## 6. 清理函数再次抛异常怎么办 {#defer-throws}

清理闭包本身也是普通调用帧，可以拥有自己的处理器和 defer。如果它最终抛出未捕获异常，这个新异常替换父帧原来的返回值或异常，但父帧尚未执行的 defer 仍继续运行。

“替换退出原因”和“继续剩余清理”必须分开处理。若直接从清理闭包跳到外层 catch，父帧剩余的资源清理会被跳过。

## 7. GC 根必须包括展开状态 {#unwind-roots}

暂停中的退出值和已注册 defer 闭包都可能包含引用。GC 根集合因此从“局部变量 + 操作数栈”扩展为：

- 每个活动帧的局部变量；
- 每个活动帧的操作数栈；
- 每个帧尚未执行的 defer 闭包；
- 帧正在传播的返回值或异常值。

漏掉最后两项会产生非常隐蔽的悬空句柄：对象只在异常或返回路径中仍然存活，普通执行测试往往无法发现。

## 8. 运行测试 {#tests}

本章修改 `reference/svm.c` 中的异常表验证、`begin_throw`、`begin_return` 和统一展开状态机；从 `test_cross_frame_exception` 到 `test_defer_lifo_order` 的测试就是按实现顺序排列的可执行清单。

```bash
cd examples/stack-vm
make test
make sanitize
```

参考测试证明：

- 被调用函数抛出的对象能被调用者处理器捕获；
- 正常返回时，递增清理闭包恰好执行一次；
- 异常展开时，外层处理器观察到的是清理后的对象状态；
- 两个 defer 按注册顺序的逆序执行；
- 未捕获客体异常以结构化 VM 错误终止实例；
- 静态分析、AddressSanitizer 和 UndefinedBehaviorSanitizer 不报告问题。

## 本章验收 {#acceptance}

- 处理器范围和入口在加载阶段验证；
- `throw` 没有普通控制流后继；
- 跨帧传播使用调用指令位置查找处理器；
- 每个 defer 恰好执行一次并严格 LIFO；
- 清理再次抛出时，剩余清理不会丢失；
- `return`、`throw` 和 defer 共享一个退出状态机；
- trap 与可捕获的客体异常明确分离。

下一章会把单个执行帧链扩展成多个轻量任务，并实现 `spawn`、`await`、channel 和 `select`。异常状态机仍会保留在每个任务内部，而调度器只负责决定哪条帧链现在可以前进。
