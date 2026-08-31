---
title: 第 6 章：capability 沙箱与宿主调用
linkTitle: 06 capability 沙箱
description: 让模块声明宿主导入，在实例创建时显式绑定 capability，并实现同步/异步调用、签名检查和调用成本预算。
weight: 60
draft: false
---

文件、网络、时钟和随机数不能直接成为“任何模块都能执行”的普通指令。它们必须经过宿主边界：模块声明自己需要什么，创建 VM 实例的宿主决定实际授予什么，并为每次调用设置成本。

## 1. 模块只声明导入，不拥有权限 {#imports}

```c
typedef struct {
    const char *capability_name;
    const char *function_name;
    const SvmType *parameter_types;
    uint32_t parameter_count;
    SvmType result_type;
    bool asynchronous;
} SvmHostImport;
```

例如模块可以声明 `clock.now() -> i32`，但这不代表它已经获得时钟。只有实例创建者传入名称和签名都匹配的 capability，模块才能开始执行。

当前教学边界只允许 `i32` 和 `bool` 穿过宿主调用。受管引用、任务和 channel 不能交给任意 C 回调，因为回调可能保存陈旧句柄或伪造引用。以后需要文件内容时，可以增加受检字节缓冲区 API，而不是直接暴露 `SvmHeapObject *`。

## 2. capability 同时携带策略上下文 {#capability-object}

```c
typedef struct {
    const char *name;
    void *context;
    const SvmHostFunction *functions;
    uint32_t function_count;
} SvmCapability;
```

`context` 才是 capability 模型的关键。例如两个都叫 `files.read` 的能力，可以分别绑定：

- 根目录 `/data/input` 与最大读取 1 MiB；
- 根目录 `/tmp/job-42` 与最大读取 64 KiB。

模块只拿到被绑定的函数，无法把路径改成宿主任意位置。不要把一个完整应用对象放进 context，再靠函数内部检查调用者“应该访问什么”；应尽量在创建 capability 时裁剪权限。

## 3. 实例绑定发生在第一条指令之前 {#binding}

VM 对每个导入执行：

1. 按名称寻找 capability；
2. 在 capability 内按名称寻找函数；
3. 比较参数数量、每个参数类型和结果类型；
4. 同步/异步模式必须与导入一致；
5. 回调、异步 poll 和非零成本必须存在；
6. 保存已经绑定的函数指针和 context。

缺少能力或签名不一致时，实例创建失败，客体代码一条也不执行。这样不会在程序运行一半、已经产生外部副作用后才发现关键权限缺失。

## 4. 同步调用的执行顺序 {#sync-call}

```text
host_call import_index    arguments... -- result
```

解释器严格按照以下顺序执行：

1. 验证器已经证明参数类型匹配；
2. 从操作数栈复制参数；
3. 在回调执行前扣除 `cost`；
4. 调用绑定函数；
5. 检查宿主返回值类型；
6. 成功则压入结果，失败则作为客体异常传播。

“先扣成本再调用”不可调换。若预算检查发生在回调之后，一次超额网络写入已经造成不可撤销的外部副作用。

同步回调若返回 `PENDING` 属于宿主实现错误，VM 产生 trap；只有声明为异步的导入可以暂停。

## 5. 异步调用复用任务与 await {#async-call}

```text
host_call_async import_index    arguments... -- task
task_await -(import_index + 1)  task -- result
```

异步 `invoke` 可以立即完成、立即失败，或返回 `PENDING` 和一个宿主 token。pending 情况会创建没有字节码帧的宿主任务：

```c
task->is_host_task = true;
task->host_import_index = import_index;
task->host_token = pending_token;
task->wait_kind = SVM_WAIT_HOST;
```

调度器调用绑定的 `poll(context, token, &value)` 推进它。每次 poll 都再次扣除函数成本，防止宿主任务用无限轮询绕过资源预算。完成或失败后，普通 `await` 唤醒逻辑负责传递结果或重新抛出失败。

教学实现采用受计费轮询，方便在纯 C 测试中确定性复现。生产事件循环应让宿主在 fd、timer 或 completion queue 就绪时唤醒任务，而不是忙等；字节码可观察语义无需改变。

## 6. 为什么 await 使用签名键 {#await-key}

普通字节码任务用非负函数索引描述结果签名；宿主任务用 `-(import_index + 1)`。验证器因此能在只看到 task 句柄时，仍确定 `await` 成功后应该压入什么类型。

运行时还会比较任务内部保存的 `await_key`。客体不能拿一个 `task<bool>`，却用声明返回 `i32` 的 await 指令解释结果。

更成熟的类型系统可以把 `task<T>` 做成参数化验证器类型；签名键是第一版紧凑且容易编码的替代方案。

## 7. 资源预算形成多层防线 {#resource-limits}

到本章为止，一个实例同时限制：

| 资源 | 限制字段 | 检查位置 |
| --- | --- | --- |
| 客体指令 | `instruction_budget` | 每次取指之前 |
| 调用深度 | `call_depth_limit` | 创建调用帧之前 |
| 堆字节 | `heap_byte_limit` | 提交对象分配之前 |
| 引用句柄 | `handle_limit` | 分配句柄之前 |
| 任务 | `task_limit` | spawn 或异步宿主任务之前 |
| channel | `channel_limit` | 创建 channel 之前 |
| channel 容量 | `SVM_MAX_CHANNEL_CAPACITY` | 创建 channel 时 |
| 宿主工作 | `host_call_budget` | invoke 和每次 poll 之前 |

这些额度互相不能替代。一条 `host_call` 只消耗一条客体指令，却可能要求宿主处理大量数据，所以必须有独立成本。

## 8. 可捕获失败与 trap {#failures-and-traps}

宿主业务失败可以返回 `SVM_HOST_FAILED` 和一个客体失败值，由当前函数的异常表处理。以下情况则是 trap：

- capability 未授权或绑定签名不一致；
- 宿主返回了错误类型；
- 同步函数错误地返回 pending；
- 宿主预算耗尽；
- 回调或 poll 指针缺失。

trap 表示 VM 与宿主之间的契约被破坏，不能让客体捕获后继续假装实例状态可信。

## 9. 运行测试 {#tests}

公开 ABI 位于 `reference/svm.h` 的 `SvmHostFunction`、`SvmCapability` 与 `SvmHostImport`；绑定、计费和异步 poll 在 `reference/svm.c`。`test_sync_capability` 到 `test_async_capability` 依次对应同步成功、无授权、预算先扣除和异步恢复。

```bash
cd examples/stack-vm
make test
make sanitize
```

测试提供一个 `math.add` 同步 capability 和一个延迟完成的 `timer.increment` 异步 capability，并验证：

- 同步调用 `2 + 3` 返回 5，回调只执行一次；
- 未授予 capability 时，在执行第一条指令前失败；
- 调用成本超过预算时，回调调用次数仍为 0；
- 异步调用经过两次 poll 后返回 11；
- invoke 和两次 poll 的成本都被准确扣除；
- 极小调度时间片下，宿主任务与客体任务仍正确恢复。

## 本章验收 {#acceptance}

- 模块声明需求，实例创建者授予权限；
- 绑定同时检查名称、签名和同步模式；
- 客体不能访问未绑定的宿主函数；
- 所有外部副作用之前先检查成本；
- 异步宿主工作通过普通任务和 await 完成；
- poll 同样计费，不存在免费无限工作；
- 宿主返回值再次经过类型检查；
- 可恢复业务失败与破坏契约的 trap 分开。

下一章会把目前仍由 C 数组直接构造的模块编码成真正的二进制文件，加入文件头、分段目录、定长整数解析和指令解码。只有通过解码、结构检查和类型验证的模块才能进入本章的 capability 绑定阶段。
