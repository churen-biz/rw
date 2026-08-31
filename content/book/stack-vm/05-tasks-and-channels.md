---
title: 第 5 章：轻量任务、await、channel 与 select
linkTitle: 05 并发与 channel
description: 把单一帧链重构为协作式任务调度器，实现 spawn/await、缓冲与无缓冲 channel、select 和死锁检测。
weight: 50
draft: false
---

本章把“VM 只有一条帧链”的假设移除。每个轻量任务保存自己的调用帧、异常展开状态和结果；调度器每次只推进一个任务，但任何任务都可以在 `await` 或 channel 操作处暂停，稍后从下一条指令恢复。

这是一套确定性的协作并发模型，不是多 OS 线程并行执行。它已经能表达 JavaScript 事件循环、future、goroutine 和 channel 的功能语义；原子指令和并行工作线程属于后续可替换的执行策略。

## 1. 从一条帧链变成任务表 {#task-table}

```c
typedef struct {
    uint32_t id;
    SvmTaskState state;
    SvmFrame frames[SVM_MAX_FRAMES];
    uint32_t frame_count;
    SvmValue result;
    SvmValue failure;
    uint32_t entry_function;
    SvmWaitKind wait_kind;
    uint32_t wait_target;
    SvmValue wait_value;
} SvmTask;
```

任务状态包括 `RUNNABLE`、`RUNNING`、`WAITING`、`DONE`、`FAILED` 和 `CANCELLED`。等待原因另行记录为任务、发送、接收或 select，避免把“暂时不能运行”和“已经完成”混成一个布尔值。

任务 ID 与对象句柄遵循同样的规则：`0` 无效，实例生命周期内不复用，并受到 `task_limit` 约束。

## 2. spawn 和 await 的栈语义 {#spawn-await}

```text
task_spawn function     arguments... -- task
task_await function     task -- result
task_yield              --
```

`spawn` 从当前栈弹出目标参数，为新任务建立首帧，把任务放入可运行集合，再压入任务句柄。目标函数不能要求闭包捕获；要启动闭包时可以在以后增加带签名索引的 `spawn_closure`。

`await` 已完成任务时立即取得结果，已失败任务则在等待者中重新抛出失败值。尚未完成时，等待者保存目标 ID 并暂停。目标完成后，调度器先把结果压入等待者的栈或启动异常展开，再把它标记为可运行。因此恢复时不会重复执行 `await`。

## 3. 时间片不是线程抢占 {#quantum}

调度器给每个任务固定的字节码指令额度 `scheduler_quantum`。额度用完后，任务回到可运行集合尾部。`task_yield` 可以提前让出。

这只发生在两条完整字节码指令之间，所以调用帧、操作数栈和 GC 标签始终一致。以后使用 OS 线程时，仍应只在安全点请求抢占，而不是在解释器写一半对象字段时暂停。

测试把时间片设为 1，迫使任务几乎每条指令都切换一次。若保存 `pc`、栈高或局部变量有任何错误，这种测试比默认时间片更容易暴露问题。

## 4. channel 的统一数据结构 {#channel-structure}

```c
typedef struct {
    bool used;
    bool closed;
    SvmType element_type;
    uint32_t capacity;
    uint32_t count;
    uint32_t read_pos;
    uint32_t write_pos;
    SvmValue buffer[SVM_MAX_CHANNEL_CAPACITY];
} SvmChannel;
```

容量为 0 表示无缓冲 channel。大于 0 时使用环形缓冲区。channel ID 和元素类型都在运行时检查，防止把 `channel<i32>` 当作 `channel<ref>` 使用。

## 5. 发送和接收为什么会暂停 {#send-receive}

```text
channel_new type       i32_capacity -- channel
channel_send type      channel, value --
channel_recv type      channel -- value
channel_close          channel --
```

发送按顺序尝试：

1. 若有普通接收者等待，直接交付并唤醒接收者；
2. 若有包含此 channel 的 select 等待，完成其中一个 case；
3. 若缓冲区有空间，写入环形缓冲区；
4. 否则把发送值保存在任务中并暂停。

接收执行对称流程：先取缓冲值，再匹配等待发送者，关闭且已清空时返回元素零值，否则暂停。

发送值必须成为 GC 根。它已经离开操作数栈，却仍保存在等待任务里；若不扫描 `wait_value`，阻塞发送一个对象时对象可能被错误回收。

## 6. select 如何保证只恢复一次 {#select}

第一版指令选择多个相同元素类型的接收 case：

```text
channel_select count, type
    channel_0, ..., channel_n -- i32_selected_index, value
```

如果多个 channel 已就绪，任务从上次选中位置之后开始轮转检查，避免总是偏向第 0 个。都未就绪时，任务保存全部 channel ID 并暂停。

未来任一发送者匹配这个任务时，调度器在同一个临界步骤中完成三件事：压入 case 索引和值、清空 select 注册、把任务改为 `RUNNABLE`。其他 channel 再扫描时已经看不到等待状态，因此同一次 select 不会恢复两次。

生产 VM 通常使用共享一次性 token 和显式等待队列，以避免线性扫描；教学实现扫描固定上限的任务表，语义相同而代码更容易检查。

## 7. 关闭、失败与死锁 {#close-and-deadlock}

关闭 channel 会：

- 让等待接收者得到元素零值；
- 完成等待 select 的对应 case；
- 让等待发送者抛出客体异常；
- 拒绝之后的发送。

Go 源语言需要的 `value, ok := <-ch` 可以由前端把接收结果包装成二字段记录；核心 VM 为了保持指令集小，只直接返回值。

当主任务未结束、没有任何可运行任务、其余任务又全部等待时，调度器报告结构化死锁，而不是无限空转。外部异步 I/O 接入后，“等待宿主事件”和“内部确定死锁”还需要分成两类；下一章的异步 capability 会加入宿主事件源。

## 8. GC 必须扫描所有任务和 channel {#concurrent-roots}

停止世界 GC 现在遍历：

- 每个任务的全部活动帧；
- 已完成任务的结果和失败任务的异常；
- 阻塞发送者保存的值；
- 每个 channel 缓冲区中的活动槽；
- defer、退出值以及之前章节已有的根。

标记接口被拆成“逐个 `svm_heap_mark_value`，最后统一 `svm_heap_sweep`”。这样不需要先创建大小为“任务数 × 帧数 × 槽数”的大型临时数组。

## 9. 运行测试 {#tests}

本章在 `reference/svm.c` 中加入 `SvmTask`、`SvmChannel`、等待原因和 scheduler loop。对应测试从 `test_spawn_and_await` 开始；每写通一个状态转换就运行一次，最后再运行死锁与额度失败用例。

```bash
cd examples/stack-vm
make test
make sanitize
```

并发测试覆盖：

- 时间片为 1 时，`spawn` 后 `await` 得到正确结果；
- 子任务失败通过 `await` 在父任务重新抛出并被捕获；
- 无缓冲 channel 在先接收、后发送的顺序下完成直接交接；
- `select` 等待第二个 channel，并返回 case 1 和发送值；
- 关闭后接收得到元素零值；
- 任务数量额度不可绕过；
- channel 数量额度不可绕过；
- 所有任务等待时报告死锁；
- Clang 静态分析和 Sanitizer 均通过。

## 本章验收 {#acceptance}

- 每个任务拥有独立帧链和异常状态；
- `await` 恢复后不会重复执行等待指令；
- 缓冲、无缓冲、关闭和阻塞路径都有确定语义；
- select 一次只完成一个 case，并保留轮转公平性；
- 等待值、任务结果和 channel 缓冲值都是 GC 根；
- 任务、channel、容量和指令时间片都有明确上限；
- 无可运行任务时不会静默挂死。

下一章会实现 capability 宿主边界。文件、时钟和随机数不会成为可随意调用的 opcode；模块只能通过实例创建时注入的能力表访问宿主，并为每次调用支付资源预算。
