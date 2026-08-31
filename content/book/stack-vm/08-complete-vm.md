---
title: 第 8 章：组装、测试并运行完整 VM
linkTitle: 08 完整 VM
description: 从干净目录构建最终解释器，运行模块文件，并用普通测试、sanitizer、静态分析和恶意输入回归确认完整性。
weight: 80
draft: false
---

现在我们不再增加新机制，而是证明所有机制组成了一个闭环。本教程所说的“完整”，指一门常见语言能把高级特征降低到这些 VM 原语，并且每条安全边界都有可执行测试；它不表示已经达到 JVM、V8 或 Go runtime 的生产性能。

## 1. 从干净状态构建 {#clean-build}

```bash
cd examples/stack-vm
make clean
make all
make test
make demo
```

产物包括：

- `milestone-01`：第一章的最小解释器，便于对照成长路径；
- `reference-tests`：验证器、调用、异常、任务、channel、capability 和配额；
- `heap-tests`：句柄、对象、数组、闭包与 GC；
- `module-tests`：二进制格式与恶意模块；
- `make-example`：用 C 数据结构生成模块文件；
- `svm-run`：从磁盘加载、验证和执行模块。

## 2. 一门高级语言如何落到这些指令 {#lowering}

Java 风格对象可降低为 `new_object/get_field/set_field`，方法调用降低为普通函数调用并显式传入接收者；JavaScript 闭包降低为 `new_closure/call_closure`，动态值用 `ANY` 作为受控汇合类型；Go 风格 goroutine 和 channel 分别降低为 `task_spawn` 与 channel 指令。

VM 不需要为每种源语言复制一套指令。前端负责把语法和语言特有规则转换成少量正交原语，VM 负责验证、执行、内存、调度和隔离。

更完整的「源码 → 指令」路径见配套教程 [《从文本汇编到 Mini 编译器》](/book/svm-compiler/)：先手写 `.sasm`，再用 Mini 与 IR 生成同一套 opcode。

## 3. 最关键的端到端不变量 {#invariants}

最终实现维持以下顺序：

```text
不可信字节
  → 格式边界检查
  → 控制流与栈类型验证
  → capability 绑定
  → 带统一预算的执行/调度
  → 从全部 VM 根进行 GC
```

顺序不能交换。未经验证就执行会让解释器处理不可能状态；运行中才发现权限缺失可能已经产生副作用；GC 若看不到暂停任务、channel 和 defer 中的引用，会回收仍然存活的对象。

## 4. 四层测试各自寻找什么 {#test-layers}

普通单元和集成测试寻找语义错误。负面模块测试证明攻击者不能绕过边界。AddressSanitizer/UndefinedBehaviorSanitizer 寻找越界、释放后使用、泄漏和未定义行为。编译器静态分析寻找没有在当前测试路径触发的资源和控制流问题。

运行动态检查：

```bash
make sanitize
```

Clang 用户还可以运行：

```bash
cc --analyze -std=c11 -Wall -Wextra -Werror -pedantic \
  -Ireference reference/svm.c reference/heap.c reference/module.c
```

没有任何单一测试工具能够证明 VM 正确。可靠性来自“规格中的不变量 + 验证器 + 运行时检查 + 多层测试”相互重叠。

## 5. 配额如何形成沙箱，而不是几个零散 if {#quota-matrix}

| 风险 | 限制 |
| --- | --- |
| 死循环、过长计算 | `instruction_budget` |
| 递归耗尽 C/VM 栈 | `call_depth_limit` |
| 对象分配膨胀 | `heap_byte_limit`、`handle_limit` |
| 任务风暴 | `task_limit` |
| channel 风暴 | `channel_limit` 与固定容量上限 |
| 宿主 I/O 滥用 | capability 的 `cost` 与 `host_call_budget` |
| 单任务长期霸占 | `scheduler_quantum` |

预算必须在动作产生副作用之前扣除。异步宿主调用每次 poll 也扣预算，否则模块可以用永不完成的 future 绕过限制。

## 6. “完成”以后最值得继续的方向 {#next}

若继续扩展，建议先做文本汇编器和调试信息，再做字节串、映射、取消传播与模块签名。JIT、分代 GC、抢占和无锁 channel 都属于优化或生产化阶段；在这些复杂度之前，先保持当前解释器是清晰、可验证的语义参考实现。

到这里，你已经从一个只有 `pc + stack + switch` 的 C 文件，走到了能安全加载模块、管理对象、展开异常、回收内存、调度并发任务并隔离宿主能力的完整教学 VM。
