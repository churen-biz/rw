---
title: 从零设计一个支持多语言的栈式虚拟机
linkTitle: 设计栈式 VM
date: 2026-08-31
lastmod: 2026-08-31
description: 面向初学者，从功能完整性出发设计一套能承载 Java、JavaScript 与 Go 常见语言特征的栈式虚拟机指令集。
authors: [demo-team]
categories: [虚拟机]
tags: [VM, Bytecode, GC, Concurrency, Sandbox]
draft: false
---

## 这篇文章要设计什么 {#scope}

我们要设计的是一个**多语言编译目标**：不同语言的编译器都可以把源程序翻译成同一种字节码，再由同一个 VM 加载和执行。

它并不直接运行 JVM 的 `.class`、Node.js 的 JavaScript 源码或 Go 的原生二进制。兼容这些既有格式，意味着还要兼容各自的对象模型、标准库、ABI、调试协议和历史行为，规模远远超过“设计一套 VM 指令”。本文讨论的是另一件更适合从零实现的事：提取 Java、JavaScript 和 Go 的共同能力，为新编译器提供一个足够完整的后端。

我们只追求以下目标：

- 能表达静态类型与动态类型程序；
- 能实现对象、接口、数组、字符串、闭包和泛型擦除后的代码；
- 能实现异常、`defer`、`panic/recover`；
- 能支持线程、轻量任务、异步操作、channel 和 `select`；
- 堆对象由 GC 管理；
- 不可信程序只能在授予的权限和资源额度内运行；
- 字节码可以在执行前验证，错误程序不能破坏 VM 自身。

我们暂时不考虑 JIT、内联、逃逸分析、寄存器分配、NaN-boxing 和无锁调度器。这些会影响速度，却不决定语言功能是否能够正确实现。

> [!NOTE]
> 本文负责解释完整目标 ISA 和关键取舍。希望从空白 `vm.c` 开始写出第一版完整、安全、可运行核心的读者，请继续阅读配套的[《从第一行 C 到完整栈式 VM》](/book/stack-vm/)；系列中每章都有对应 C 源码、成功测试和失败测试。

> [!IMPORTANT]
> “支持某种语言的特征”不等于“每个特征都有一条指令”。类、Promise 和 goroutine 都是由少量 VM 原语、元数据和运行时库共同实现的。把高级语言的每个关键字直接做成 opcode，只会得到一套难以验证、难以演进的指令集。

## 先认识几个名词 {#glossary}

| 名词 | 本文中的含义 |
| --- | --- |
| VM | 读取并执行虚拟指令的软件，不是操作系统虚拟机 |
| 字节码 | 编译器生成、由 VM 执行的紧凑指令序列 |
| opcode | 标识“这是什么指令”的数字编码 |
| ISA | 指令、数据类型、错误和内存语义组成的指令集规范 |
| runtime | 与字节码配套的运行时库和内部服务，例如字符串、调度器和 GC |
| 宿主 | 运行 VM 的 C 程序和操作系统 |
| 客体 | 在 VM 中运行的模块或程序 |
| ABI | 函数参数、返回值、对象布局等二进制层面的约定 |
| capability | 宿主显式授予实例的一项受限能力 |
| 安全点 | VM 可以暂停任务并准确找到所有对象引用的位置 |

如果暂时不理解 LEB128、写屏障或内存顺序，不必先停下来补完整套编译原理。第一次出现这些概念时，本文会解释它们解决的具体问题。

## 为什么选择栈式指令集 {#why-stack-machine}

栈式 VM 把大多数指令的输入和输出放在操作数栈上。计算 `a + b * 2` 时，一段字节码可以写成：

```text
load_local 0      ; [..., a]
load_local 1      ; [..., a, b]
const_i32 2       ; [..., a, b, 2]
i32_mul           ; [..., a, b * 2]
i32_add           ; [..., a + b * 2]
return
```

方括号中的内容是每条指令执行后的操作数栈。`i32_mul` 不需要编码两个输入寄存器和一个输出寄存器：它弹出栈顶两个 `i32`，再压入结果。

这种设计适合第一版 VM，原因不是它一定更快，而是它缩小了需要同时理解的状态：

- 指令编码短，解释器的取指和解码逻辑直观；
- 编译器后端不需要先实现寄存器分配；
- 验证器只需沿控制流计算每个位置的栈高度和类型；
- 调试器可以直接展示“局部变量 + 操作数栈”；
- 函数暂停时，完整状态天然集中在调用帧中。

代价也很明确：同一个值可能被多次压栈和弹栈，指令数量通常多于寄存器式 VM。本文接受这个代价，因为目标是功能完整和容易验证，而不是吞吐量最优。

栈式并不意味着所有状态都在一个全局栈里。每个函数调用都有独立的调用帧，每个轻量任务又有自己的帧链。这样才能安全地调用函数、递归、抛异常和暂停任务。

## 开始前的准备 {#prerequisites}

教程中的可运行示例只依赖 C11 编译器和终端，不依赖第三方库。在 macOS 上可以使用 Apple Clang，在 Linux 上可以使用 GCC 或 Clang：

```bash {tab="macOS" group="vm-os" value="macos"}
xcode-select --install
cc --version
```

```bash {tab="Linux" group="vm-os" value="linux"}
sudo apt install build-essential
cc --version
```

先创建实验目录：

```bash
mkdir stack-vm-tutorial
cd stack-vm-tutorial
touch vm.c
```

本文每个里程碑都遵循同一个循环：增加少量数据结构和指令，编译，运行一个能观察结果的程序，再加入错误用例。不要在前一步还不能稳定运行时继续堆功能。

## 数据模型与执行模型 {#execution-model}

### 值类型 {#value-types}

第一版 VM 使用八种验证器可见的基础类型：

| 类型 | 含义 |
| --- | --- |
| `i32`、`i64` | 有符号整数；位运算也使用它们 |
| `f32`、`f64` | IEEE 754 浮点数 |
| `bool` | 布尔值，不与整数隐式混用 |
| `ref<T>` | 指向 GC 堆对象的受管引用，可以细化为具体类型 |
| `null` | 空引用，只能进入允许空值的位置 |
| `any` | 带运行时类型标签的装箱值，为动态语言服务 |

`any` 不是 C 的 `void *`。它必须携带类型标签，并且其中的引用必须能被 GC 识别。一个教学实现可以直接使用带标签的联合体：

```c
typedef enum {
    VAL_I32, VAL_I64, VAL_F32, VAL_F64,
    VAL_BOOL, VAL_REF, VAL_NULL
} ValueTag;

typedef struct {
    ValueTag tag;
    union {
        int32_t  i32;
        int64_t  i64;
        float    f32;
        double   f64;
        bool     boolean;
        uint32_t ref_handle;
    } as;
} VmValue;
```

这里的 `any` 是验证器使用的类型，不需要额外的 `VAL_ANY` 标签：它在运行时就是一个完整的 `VmValue`，真正的值种类由 `tag` 给出。为了让第一版解释器保持简单，即使验证器已经知道某个槽是 `i32`，运行时也可以暂时仍用 `VmValue` 保存它；以后再把静态类型槽改成紧凑布局。

这里用 `ref_handle` 而不是裸指针，原因有三个：GC 移动对象时可以只更新句柄表，沙箱代码无法伪造宿主地址，序列化和调试也更容易。以后为了性能可以改成直接指针，但字节码语义不必改变。

### 调用帧 {#call-frame}

每个调用帧保存当前函数的局部变量、操作数栈、指令位置和异常清理状态：

```c
typedef struct VmFrame {
    const struct VmFunction *function;
    uint32_t pc;

    VmValue *locals;
    uint32_t local_count;

    VmValue *stack;
    uint32_t stack_size;
    uint32_t stack_capacity;

    struct VmDefer *defers;
    struct VmFrame *caller;
} VmFrame;
```

参数在调用发生时复制到新帧的局部变量槽中，返回值压回调用者的操作数栈。任务暂停时，调度器保留帧链；任务恢复时，从保存的 `pc` 继续执行。因此，异步并不要求另一套字节码执行模型。

### 模块和元数据 {#module-metadata}

一份可加载模块至少包含：

- 常量池：字符串、数字和不可变数据；
- 类型表：字段、方法、接口实现和 GC 引用布局；
- 函数表：签名、局部变量数量、最大栈深和字节码；
- 导入导出表：模块之间可见的符号；
- 异常处理表：受保护区间、处理器地址和捕获类型；
- 栈映射：安全点处哪些局部变量和栈槽是对象引用；
- 调试信息：字节码位置到源文件、行号和函数名的映射；
- capability 导入：程序希望使用的宿主能力。

这些信息不是装饰。验证器依赖函数签名和最大栈深，GC 依赖引用布局和栈映射，沙箱依赖 capability 导入。没有元数据的“纯指令流”不足以承载现代语言。

第一版可以采用“文件头 + 分段目录 + 段内容”的布局：

```c
typedef struct {
    uint8_t magic[4];       /* 固定为 "SVM\0" */
    uint32_t version;
    uint32_t section_count;
} VmModuleHeader;

typedef struct {
    uint32_t kind;          /* 常量、类型、函数、导入等 */
    uint32_t offset;        /* 相对文件开头的偏移 */
    uint32_t size;
} VmSectionEntry;
```

这两个结构只描述逻辑字段，不能把不可信文件直接强制转换成 C 结构体指针。加载器应逐字节读取固定的小端整数，并先验证 `offset + size` 不溢出且不超过文件长度，再解析段内容。这样可以避开 C 结构体填充、宿主端序差异和越界读取。

### 字节码编码 {#bytecode-encoding}

每条指令以一个字节的 opcode 开头，后面跟零个或多个无符号或有符号 LEB128 操作数。跳转目标、常量索引、函数索引和类型索引都使用整数编号，而不是宿主指针。

LEB128 把整数每 7 位分成一组，每个字节的最高位表示后面是否还有一组；因此 `0` 到 `127` 只需一个字节，更大的编号才逐渐增加字节数。解码器必须限制最大字节数，并在移位前检查溢出，不能一直读取到攻击者伪造的“结束字节”。这种编码让常见的小编号很紧凑，同时不限制模块规模。更重要的是，加载器能在执行前检查每个索引和跳转目标是否有效。

## 最小但完整的核心指令集 {#instruction-set}

下面使用 `输入 -- 输出` 描述栈效果。例如 `i32, i32 -- i32` 表示弹出两个 `i32` 并压入一个 `i32`。

### 指令总表 {#instruction-overview}

下表是面向 Java、JavaScript/Node.js 与 Go 前端的**目标 ISA**。配套 C 教程实现其中形成安全闭环的 SVM 1.0 核心：`i32/bool/ref`、直接调用与闭包、对象/数组、异常/defer、GC、任务/channel/select、capability 和模块加载。`i64/f64`、虚方法、原子操作等保留为按相同模式扩展的指令族；不要把“目标清单”误读为当前参考代码已经逐项实现。

| 类别 | 指令或指令族 |
| --- | --- |
| 常量 | `const_i32`、`const_i64`、`const_f32`、`const_f64`、`const_true`、`const_false`、`const_null`、`const_pool` |
| 局部与栈 | `load_local`、`store_local`、`load_global`、`store_global`、`dup`、`swap`、`pop` |
| 整数运算 | `i32/i64_add`、`sub`、`mul`、`div_s`、`rem_s`、`neg`、`and`、`or`、`xor`、`shl`、`shr_s`、`shr_u` |
| 浮点运算 | `f32/f64_add`、`sub`、`mul`、`div`、`neg` |
| 比较 | `i32/i64/f32/f64_eq`、`ne`、`lt`、`le`、`gt`、`ge`、`ref_eq`、`is_null` |
| 转换 | `i32_to_i64`、`i64_to_i32_checked`、整数与浮点互转、`box`、`unbox_checked` |
| 控制流 | `jump`、`jump_if_true`、`jump_if_false`、`switch`、`trap` |
| 调用 | `call`、`call_indirect`、`invoke_virtual`、`invoke_interface`、`call_dynamic`、`return` |
| 对象 | `new_object`、`get_field`、`set_field`、`new_array`、`array_len`、`array_get`、`array_set`、`is_instance`、`cast_checked` |
| 闭包 | `new_closure`、`get_upvalue`、`set_upvalue` |
| 异常与清理 | `throw`、`rethrow`、`defer_push`、`defer_pop`、`end_finally` |
| 任务 | `task_spawn`、`task_await`、`task_yield`、`task_cancel` |
| channel | `channel_new`、`channel_send`、`channel_recv`、`channel_close`、`channel_select` |
| 原子操作 | `atomic_load`、`atomic_store`、`atomic_cas`、`atomic_rmw`、`memory_fence` |
| GC 协作 | `safepoint`、`weak_ref_new`、`weak_ref_get` |
| 宿主边界 | `host_call`、`host_call_async` |

表中的 `i32/i64_add` 表示两个独立 opcode，而不是运行时检查操作数类型的一条通用 `add`。静态类型编译器直接发出对应的类型化指令；JavaScript 一类的动态语言先发出 `unbox_checked`，或调用实现动态加法语义的运行时函数。

### 为什么算术指令应该类型化 {#typed-arithmetic}

`i32_add` 的栈效果固定为 `i32, i32 -- i32`。验证器可以在程序运行前证明操作数类型正确，解释器也不必在每次加法时读取类型标签。

更重要的是，“加法”在不同语言里根本不是同一种行为：Java 的整数加法会溢出回绕，JavaScript 的 `+` 还可能连接字符串，Go 的无符号整数又有独立类型。VM 应只定义简单、可移植的数值原语；语言特有语义由编译器插入检查或调用运行时函数。

除法必须明确规定除零、最小整数除以 `-1`、NaN 和无穷大的行为。不能把结果交给 C 编译器和当前 CPU 决定，否则同一份字节码在不同平台上可能得到不同结果。

### `call`、`call_indirect` 和动态调用为什么要分开 {#call-family}

普通调用最容易验证：

```text
call function_index
```

加载器从函数表取得静态签名，检查栈上参数类型，然后创建新帧。`call` 足以实现大部分普通函数和静态方法。

函数值、回调和闭包不能把目标写死在指令里，因此需要：

```text
call_indirect signature_index
```

它从栈上取得一个可调用引用，并验证该引用的签名是否与 `signature_index` 一致。签名检查不能省略：如果攻击者把“接收一个参数”的函数伪装成“接收三个参数”的函数，调用帧就会被错误解释，沙箱边界也随之失效。

面向对象分派使用 `invoke_virtual method_slot` 或 `invoke_interface interface_method`。它们根据接收者的运行时类型查找目标，但方法签名仍由元数据确定。JavaScript 式的 `obj[name](arg)` 则需要 `call_dynamic callsite_index`：运行时负责属性查找、`this` 绑定、装箱转换和缺失方法错误。

把四种调用合并成一条万能 `call` 看起来更简单，实际上会让验证规则、缓存策略和错误语义全部混在一起。分开之后，每条指令的安全检查很明确。

### 为什么字段和数组访问必须是 VM 指令 {#managed-memory-access}

对象字段不能通过“基址加偏移再读内存”实现。字节码应使用：

```text
get_field field_index       ; ref<T> -- value
set_field field_index       ; ref<T>, value --
array_get element_type      ; ref<Array<T>>, i32 -- T
array_set element_type      ; ref<Array<T>>, i32, T --
```

这样 VM 才能统一完成空引用检查、数组边界检查、字段类型检查和 GC 写屏障。尤其是 `set_field` 与 `array_set`：当新引用被写入老对象时，分代或并发 GC 可能必须记录这次写入。如果让字节码直接写宿主内存，GC 将无法维持对象图的一致性。

写屏障是这两条指令的内部语义，不应该暴露为客体程序可选择执行的 `write_barrier`。否则恶意或错误程序只要漏掉屏障，就能破坏 GC。

### 控制流为什么必须先验证 {#verified-control-flow}

`jump` 和条件跳转使用相对字节码偏移，但目标必须恰好落在一条指令的起始位置。验证器还要保证所有到达同一位置的路径具有相同的栈高度和兼容类型。

例如，一条路径到达位置 100 时栈为 `[i32]`，另一条路径为 `[ref<String>]`，那么位置 100 的下一条指令就没有确定语义。VM 必须拒绝整个函数，而不是等某条罕见分支运行时再崩溃。

`switch` 使用只读跳转表实现密集分支。`trap reason` 则显式终止当前任务，用于编译器确认不可达的位置、整数检查失败或沙箱违规。显式 trap 比故意制造一次非法内存访问更可移植，也更容易报告错误。

### `throw` 为什么不需要 `try` 指令 {#exception-table}

`throw` 弹出异常对象并开始栈展开，但 `try/catch` 的范围存放在函数的异常处理表中：

```text
[start_pc, end_pc) -> handler_pc, catch_type
```

正常执行时，解释器不需要为进入和离开 `try` 块执行额外指令。只有抛出异常时才查询表、恢复目标栈高度并跳到处理器。这与把 `try_begin/try_end` 混进普通控制流相比，更容易验证嵌套范围，也不会给正常路径增加状态操作。

`defer_push` 则不同：Go 风格的 `defer` 是运行时按执行顺序注册的，一条语句可能位于循环或条件分支中。因此它必须把闭包压入当前帧的清理栈。函数正常返回或异常展开时，VM 都以逆序执行这些清理项。

### `safepoint` 为什么是一条重要指令 {#safepoint}

GC、任务取消和调试器暂停都需要在某个位置取得一致的执行状态。`safepoint` 表示：当前 `pc` 有对应的栈映射，VM 知道哪些槽是引用，并且可以暂停任务。

编译器至少应在函数调用、循环回边和可能长时间执行的运行时操作附近放置安全点。对象分配也隐含安全点，因为它可能触发 GC。

不能只在函数调用处停顿：一个不调用函数的无限循环将永远阻止 GC 和取消。也不能在任意机器指令中间停顿：此时某个引用可能暂存在解释器内部，GC 无法可靠找到它。

### `task_await` 为什么需要 VM 参与 {#task-await}

普通库函数可以创建任务，但等待任务会暂停当前字节码执行并在未来从同一个位置恢复。`task_await` 的语义是：

1. 弹出任务句柄；
2. 如果任务已完成，立即压入结果或抛出失败；
3. 否则保存当前帧链和恢复位置；
4. 将当前任务标记为等待状态并让出执行权；
5. 被等待任务完成后，把当前任务重新放入可运行队列。

解释器通常已经在取指时把 `pc` 移到下一条指令。等待记录因此还要标记“恢复时压入结果还是重新抛出失败”；目标完成后，调度器先完成这一动作，再把等待者放回可运行队列，不能让 `task_await` 被重复执行。

这条指令不规定调度器使用单线程事件循环还是多个 OS 线程。它只规定可观察语义，因此既能承载 JavaScript 的 `await`，也能承载更一般的 future。

### `atomic_cas` 为什么不能用普通读写代替 {#atomic-cas}

多个 OS 线程真正并行执行任务时，普通 `get_field` 加 `set_field` 不是不可分割操作。`atomic_cas`（compare-and-swap）以原子方式完成“值仍等于旧值时写入新值”，并返回是否成功。

原子指令必须携带内存顺序，例如 `relaxed`、`acquire`、`release` 或 `seq_cst`。如果第一版不想暴露完整内存模型，可以只支持最强的 `seq_cst`；它可能较慢，却容易得到一致、可解释的行为。不能直接继承 C 编译器的默认内存顺序，因为字节码需要跨平台语义。

### `host_call` 是沙箱真正的边界 {#host-call}

文件、网络、时钟、环境变量和随机数都不应该是普通 VM 指令。程序通过 capability 导入获得一个受限制的宿主函数编号，再执行：

```text
host_call capability_index, function_index
host_call_async capability_index, function_index
```

VM 在调用前检查模块是否持有该 capability，并验证参数和返回值能否安全跨越边界。异步版本返回任务句柄，由 `task_await` 等待。

例如，授予“只读 `/data/input`”能力不等于授予完整文件系统。路径解析、打开文件和读取内容都由宿主适配器执行，客体程序永远拿不到宿主文件描述符或任意系统调用入口。

资源配额也不能依赖客体程序主动执行某条 `check_budget`。解释器应自行统计指令、堆内存、任务数、打开资源数和宿主调用成本；到达上限时产生可捕获错误或终止实例。安全规则如果依赖不可信字节码自觉配合，就不再是沙箱。

## 对象、接口、闭包与动态类型 {#language-abstractions}

到目前为止，VM 已经能执行局部变量、算术、跳转和函数调用。接下来按四步加入现代语言需要的抽象。

### 第一步：用类型元数据描述对象 {#object-metadata}

对象实例只保存字段值；字段名、类型和方法放在共享的类型描述中：

```c
typedef struct {
    const char *name;
    uint16_t slot;
    uint16_t flags;
} VmFieldInfo;

typedef struct VmType {
    const char *name;
    uint32_t instance_size;
    VmFieldInfo *fields;
    uint16_t field_count;

    uint32_t *virtual_methods;
    uint16_t virtual_method_count;

    /* 每一位说明对应字段是否为 GC 引用。 */
    uint64_t *reference_bitmap;
} VmType;

typedef struct VmObject {
    VmType *type;
    bool marked;
    struct VmObject *next_gc_object;
    VmValue fields[];
} VmObject;
```

`new_object type_index` 根据类型表分配实例，并把所有字段初始化为零值或 `null`。`get_field` 和 `set_field` 使用经过验证的字段索引，而不是字符串名称。这样静态语言可以获得确定布局，加载器也能阻止越界字段访问。

解释器中的核心分支可以先写成最直接的形式：

```c
case OP_GET_FIELD: {
    uint32_t field = read_u32(code, &frame->pc);
    VmObject *object = require_object(vm, pop(frame));
    require_field(object->type, field);
    push(frame, object->fields[field]);
    break;
}

case OP_SET_FIELD: {
    uint32_t field = read_u32(code, &frame->pc);
    VmValue value = pop(frame);
    VmObject *object = require_object(vm, pop(frame));
    require_field_value(object->type, field, value);
    gc_write_barrier(vm, object, value);
    object->fields[field] = value;
    break;
}
```

先调用写屏障，再更新字段。第一版非分代标记清除 GC 可以让 `gc_write_barrier` 暂时为空函数，但调用点从一开始就应该存在，否则以后升级 GC 时容易遗漏。

### 第二步：用方法槽实现类和接口 {#method-dispatch}

编译器为每个虚方法分配稳定槽位。例如 `Animal.speak` 是槽 0，`Dog.speak` 覆盖同一槽。`invoke_virtual 0` 从接收者类型的 `virtual_methods[0]` 取得函数索引，再按静态签名调用。

接口不能假设所有类型共享同一张虚方法表，因此类型元数据还要保存“接口 ID → 方法槽数组”的映射。`invoke_interface` 先查接口实现，再查接口内的方法槽。

这里没有 `new_class`、`extends` 或 `implements` 指令。继承关系在加载模块时已经写入类型表；执行阶段只需要分配对象和分派方法。把声明语法放进指令集，会迫使 VM 在运行时重复编译器已经完成的工作。

### 第三步：用环境对象实现闭包 {#closures}

闭包由函数索引和环境引用组成：

```c
typedef struct {
    uint32_t function_index;
    uint32_t environment_handle;
} VmClosure;
```

编译器把被捕获的局部变量移入环境对象，随后使用：

```text
new_closure function_index, capture_count
get_upvalue slot
set_upvalue slot
```

`new_closure` 从栈上弹出捕获值，建立环境对象并返回可调用引用。环境位于 GC 堆中，因此创建闭包的函数返回后，捕获值仍然有效。多个闭包需要共享可变变量时，捕获的是同一个“变量盒”引用，而不是各自复制当前值。

### 第四步：把动态语义留给运行时 {#dynamic-values}

JavaScript 对象允许运行时添加属性，`+` 可能做数值运算或字符串连接，函数调用还涉及 `this`。这些规则不应污染静态对象的快速、可验证路径。

动态语言编译器使用 `any` 值，并调用运行时原语：

```text
runtime.any_add
runtime.get_property
runtime.set_property
call_dynamic
```

它们可以先作为普通导入函数实现，而不是固定 opcode。只有经过实际实现确认某个操作需要访问解释器内部状态时，才考虑提升为指令。这条原则能防止 ISA 被某一种语言绑死。

## 异常、defer 与资源清理 {#unwinding}

异常系统需要保证两件事：找到处理器，以及在到达处理器前正确清理每个栈帧。

### 第一步：为函数增加异常处理表 {#exception-table-implementation}

```c
typedef struct {
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t handler_pc;
    uint32_t catch_type;
    uint16_t handler_stack_height;
} VmExceptionHandler;
```

当 `throw` 在位置 `pc` 执行时，VM 查找覆盖该位置且类型匹配的最内层处理器。如果找到，就把操作数栈恢复到 `handler_stack_height`，压入异常对象，再跳转到 `handler_pc`。

### 第二步：实现跨帧展开 {#stack-unwinding}

```c
bool vm_throw(Vm *vm, VmTask *task, VmValue exception) {
    while (task->frame != NULL) {
        VmFrame *frame = task->frame;

        VmExceptionHandler *handler =
            find_handler(frame->function, frame->pc, exception);

        if (handler != NULL) {
            frame->stack_size = handler->handler_stack_height;
            push(frame, exception);
            frame->pc = handler->handler_pc;
            return true;
        }

        run_all_defers(vm, task, frame);
        task->frame = frame->caller;
        destroy_frame(frame);
    }

    task_fail(task, exception);
    return false;
}
```

教学版本可以线性扫描处理表；以后再换成排序表或区间索引。只有当前帧找不到处理器、即将退出该函数时，才运行这个函数已经注册的 defer。重要的是，处理表属于函数元数据，而展开逻辑只有一个入口。

### 第三步：把 Java、JavaScript 和 Go 映射到同一机制 {#unwinding-language-mapping}

- Java 的 `throw/catch/finally`：`throw` 加处理表；`finally` 编译成清理处理器。
- JavaScript 的 `throw/try/finally`：使用同一机制，异常值类型为 `any`。
- Go 的 `panic/recover`：`panic` 开始展开，`recover` 只允许在正在执行的 defer 中读取当前 panic。
- Go 的 `defer`：`defer_push` 在运行时注册闭包；正常 `return` 和异常展开都逆序执行。

`return` 不能直接销毁帧。它必须先运行 defer；defer 自身又可能抛异常或改变返回值。把清理逻辑集中在一个 `leave_frame` 函数中，比在多个 opcode 分支里复制代码可靠。

## 并发、异步、channel 与 select {#concurrency}

并发部分最容易设计过度。第一版只定义可观察语义，调度算法保持简单。

### 第一步：实现协作式轻量任务 {#tasks}

```c
typedef enum {
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_WAITING,
    TASK_DONE,
    TASK_FAILED,
    TASK_CANCELLED
} VmTaskState;

typedef struct VmTask {
    uint64_t id;
    VmTaskState state;
    VmFrame *frame;
    VmValue result;
    VmValue failure;
    struct VmTask *next;
} VmTask;
```

先使用一个 FIFO 可运行队列。解释器为每个任务执行固定数量的指令，遇到 `task_yield`、等待操作或时间片结束时，把任务放回队列尾部。

`task_spawn function_index` 创建带首个调用帧的新任务并返回任务句柄。`task_await` 的实现轮廓如下：

```c
case OP_TASK_AWAIT: {
    VmTask *target = require_task(vm, pop(frame));

    if (target->state == TASK_DONE) {
        push(frame, target->result);
    } else if (target->state == TASK_FAILED) {
        vm_throw(vm, current, target->failure);
    } else {
        current->state = TASK_WAITING;
        task_add_waiter(target, current);
        scheduler_suspend_current(vm);
    }
    break;
}
```

异步文件和网络操作由 `host_call_async` 创建宿主任务。操作完成后，宿主先把结果或失败写入等待记录，再把等待者放入可运行队列；这样 Node.js 式事件循环与 Go 式轻量任务共享同一恢复机制。

### 第二步：实现 channel {#channels}

channel 由元素类型、可选缓冲区、发送等待队列和接收等待队列组成：

```c
typedef struct VmChannelWaiter {
    VmTask *task;
    VmValue send_value;             /* 仅发送等待者使用 */
    struct VmSelectToken *select;   /* 普通等待时为 NULL */
    struct VmChannelWaiter *next;
} VmChannelWaiter;

typedef struct {
    uint32_t element_type;
    VmValue *buffer;
    uint32_t capacity;
    uint32_t count;
    uint32_t read_pos;
    uint32_t write_pos;
    bool closed;
    VmChannelWaiter *send_waiters;
    VmChannelWaiter *recv_waiters;
} VmChannel;
```

等待队列不能只保存 `VmTask *`：被阻塞的发送者还要保存待发送的值，`select` 分支还要共享一次性令牌。否则任务被唤醒时，VM 不知道应该传递哪个值，也无法阻止同一个 `select` 被多个 channel 重复唤醒。

`channel_send` 按以下顺序工作：

1. channel 已关闭则抛出错误；
2. 有接收者等待时，直接把值交给最早的接收者；
3. 缓冲区有空间时，把值写入环形缓冲区；
4. 否则保存发送值并暂停当前任务。

`channel_recv` 使用对称规则。关闭后仍可读取缓冲区中的剩余值；缓冲区耗尽后返回“已关闭”状态。是否用额外布尔值表达关闭，应由语言运行时决定。

### 第三步：实现 `select` {#channel-select}

`channel_select table_index` 引用模块中的 select 描述表。每个分支记录发送或接收、channel 所在的局部槽、值槽和成功后的跳转目标。

实现顺序是：

1. 检查所有分支是否立即可执行；
2. 若有多个可执行分支，用公平的轮转起点选择一个；
3. 没有可执行分支但存在 default 时，跳到 default；
4. 否则把同一个等待令牌注册到所有 channel 并暂停；
5. 任一 channel 唤醒任务时，原子地取消其余注册。

最后一步很关键。若每个 channel 都独立唤醒任务，同一个 select 可能执行两次。

### 第四步：再加入多线程 {#threads-and-memory-model}

单线程调度器通过测试后，可以让多个工作线程从并发队列取任务。此时才需要 `atomic_*` 和 `memory_fence`。

第一版统一使用顺序一致性 `seq_cst`，并规定普通对象不能在数据竞争下读写。至少定义以下 happens-before（先行发生）边：启动任务前的写对新任务可见，任务完成前的写对成功 `task_await` 的任务可见，channel 发送前的写对对应接收后的代码可见，锁释放对随后获取同一把锁可见。`seq_cst` 原子操作还共享一条全局可观察顺序。

这些规则保证同步之后能看到结果，但不自动修复普通字段的数据竞争。语言前端可以进一步规定竞争是错误、未指定行为或受语言专属规则约束；VM 至少要保证竞争不会让越界引用或伪造句柄逃过沙箱。语言运行时可以在此基础上实现 Java monitor、JavaScript 单事件循环隔离，以及 Go 的 mutex 和原子包。不要在尚未写出正确单线程调度器时直接挑战无锁多线程。

## GC 如何与字节码协作 {#garbage-collection}

第一版采用停止世界、非移动、标记清除 GC。它容易实现，并且足以验证 VM 与 GC 的接口是否完整。

### 第一步：建立对象表和分配入口 {#gc-allocation}

所有分配都经过 `gc_allocate`：

```c
VmObject *gc_allocate(Vm *vm, VmType *type) {
    size_t bytes = sizeof(VmObject)
                 + sizeof(VmValue) * type->field_count;

    if (vm->heap_bytes + bytes > vm->next_gc_threshold) {
        gc_collect(vm);
    }
    enforce_heap_quota(vm, bytes);

    VmObject *object = calloc(1, bytes);
    if (object == NULL) vm_trap(vm, "out of memory");

    object->type = type;
    object->next_gc_object = vm->objects;
    vm->objects = object;
    vm->heap_bytes += bytes;
    return object;
}
```

真实实现还要把对象放入句柄表并返回 `ref_handle`。堆配额检查必须在提交分配前执行，GC 也不能让实例突破沙箱上限。

### 第二步：从根集合开始标记 {#gc-roots}

根集合包括：

- 模块全局变量；
- 所有任务的调用帧；
- 调度器和 channel 保存的值；
- 宿主显式注册的临时句柄；
- 正在传播的异常和已完成任务的结果。

安全点栈映射告诉 GC 哪些局部变量槽和操作数栈槽是引用。最小表示可以为每个安全点保存两个位图：

```c
typedef struct {
    uint32_t pc;
    const uint64_t *local_ref_bits;
    const uint64_t *stack_ref_bits;
    uint32_t stack_height;
} VmStackMap;
```

位为 1 表示对应槽中可能包含 `ref` 或装箱引用。`pc` 必须精确对应暂停位置，`stack_height` 必须与验证器计算的高度一致；否则 GC 可能漏掉活对象或读取尚未初始化的槽。标记函数解析句柄，再按类型的 `reference_bitmap` 递归访问字段：

```c
void gc_mark_value(Vm *vm, VmValue value) {
    if (value.tag != VAL_REF) return;

    VmObject *object = handle_resolve(vm, value.as.ref_handle);
    if (object == NULL || object->marked) return;
    object->marked = true;

    for (uint32_t i = 0; i < object->type->field_count; i++) {
        if (type_field_is_reference(object->type, i)) {
            gc_mark_value(vm, object->fields[i]);
        }
    }
}
```

教学实现可以保守扫描所有带标签的 `VmValue`；但不能把任意整数猜成指针。精确标签是沙箱与移动 GC 的基础。

### 第三步：清扫不可达对象 {#gc-sweep}

遍历对象链表：有标记的对象清除标记并保留，没有标记的对象从链表和句柄表移除后释放。使用“指向当前链表链接的指针”可以在删除头节点和中间节点时共用一套逻辑：

```c
void gc_sweep(Vm *vm) {
    VmObject **link = &vm->objects;
    size_t live_bytes = 0;

    while (*link != NULL) {
        VmObject *object = *link;
        if (object->marked) {
            object->marked = false;
            live_bytes += object->allocated_bytes;
            link = &object->next_gc_object;
        } else {
            *link = object->next_gc_object;
            handle_release(vm, object->handle);
            free(object);
        }
    }

    vm->heap_bytes = live_bytes;
    vm->next_gc_threshold = choose_next_threshold(live_bytes);
}
```

这里假设对象头记录 `allocated_bytes` 和自身句柄。真实代码还必须检查阈值计算溢出，并保证阈值不超过实例的堆配额。清扫结束后，根据存活大小设置下一次收集阈值。

如果对象拥有文件或 socket 等外部资源，不要依赖 GC 终结器及时释放。标准库应提供显式 `close`，语言的 `defer/finally` 负责调用它。GC 只负责内存可达性，不保证业务资源的释放时机。

### 第四步：通过安全点停止任务 {#gc-stop-the-world}

请求 GC 时，正在执行的任务运行到下一个 `safepoint` 后暂停。所有任务暂停后，GC 才扫描根并收集。收集结束再恢复任务。

以后改成分代或并发 GC 时，字节码不需要重写：`set_field` 和 `array_set` 已经调用写屏障，安全点和精确引用布局也已经存在。先设计稳定接口，再替换算法。

## 沙箱与宿主能力 {#sandbox}

沙箱不是最后加上的一个开关，而是加载、验证、执行和宿主调用共同形成的边界。

### 第一步：验证模块 {#module-verification}

加载器在执行前完成以下检查：

1. 文件头、版本和各段长度合法；
2. 常量、函数、类型和字段索引都在范围内；
3. 每个跳转目标都是当前函数中的指令起点；
4. 每条控制流路径的栈高度不超过声明上限；
5. 汇合点的栈类型一致；
6. 调用参数和返回值符合函数签名；
7. 异常表区间合法且处理器栈状态确定；
8. 受管引用不会被转换成整数或宿主指针；
9. 导入的 capability 属于允许集合。

验证成功后，解释器可以假定基本类型安全；验证失败的模块永远不能进入执行阶段。

不要只做一次从头到尾的线性扫描，因为循环和条件分支会从多个方向到达同一条指令。可操作的验证器使用工作队列：

```text
先完整解码函数，记录每条指令的起点和下一条指令
state[入口] = 参数类型组成的局部变量状态 + 空操作数栈
把入口加入 worklist

while worklist 非空:
    pc = 取出一个位置
    out = 按该 opcode 的栈效果检查并转换 state[pc]
    for successor in 普通后继、跳转目标、异常处理器:
        merged = merge(state[successor], out)
        若类型或栈高度不兼容: 拒绝模块
        若 merged 发生变化: 写回并把 successor 加入 worklist
```

例如 `i32_add` 要求栈顶两个槽都是 `i32`，然后把它们替换为一个 `i32`；`jump_if_false` 弹出一个 `bool`，并同时传播到跳转目标和下一条指令。第一版可以要求汇合点的类型完全相同，只允许 `null` 与兼容的 `ref<T>` 合并；以后再引入更复杂的公共父类型计算。算法终止的原因是每个指令位置只有有限种、且只会逐步合并的状态。

### 第二步：在解释器中计量资源 {#resource-metering}

每执行一条指令就扣除预算：

```c
if (task->instruction_budget == 0) {
    vm_trap(vm, "instruction budget exhausted");
}
task->instruction_budget--;
```

宿主调用、对象分配和批量字符串操作要按成本额外计费，否则程序可以用一条昂贵调用绕过指令额度。还应限制：

- 堆内存总量与单次分配大小；
- 调用深度和操作数栈深度；
- 任务、channel 和等待者数量；
- 打开的宿主资源数量；
- 日志和网络响应大小；
- 单个实例的墙上时间。

### 第三步：使用 capability，而不是全局权限 {#capabilities}

宿主在创建实例时显式传入能力表：

```c
typedef VmResult (*HostFunction)(
    Vm *vm,
    void *capability_context,
    const VmValue *args,
    uint32_t arg_count
);

typedef struct {
    const char *name;
    void *context;
    HostFunction *functions;
    uint32_t function_count;
} VmCapability;
```

一个 `read-config` capability 的 context 可以固定根目录和最大读取字节数。模块即使猜到另一个 capability 的编号，加载器和 `host_call` 也会拒绝越权访问。

### 第四步：处理失败与取消 {#sandbox-failure}

预算耗尽、越界访问、非法 opcode 和 capability 违规都产生结构化 trap。trap 至少包含原因、模块、函数和字节码位置，但不能把宿主地址、密钥或其他实例的数据写入错误消息。

可恢复的业务错误可以成为普通异常；破坏验证器假设的错误必须终止当前实例。两者要分开，否则客体程序可能捕获并忽略一次已经让 VM 状态不可信的故障。

## Java、Node.js 与 Go 特征映射 {#language-mapping}

现在可以检查这套设计是否真的覆盖目标语言。下表中的“运行时”表示普通库或 VM 内部服务，而不是新增 opcode。

| 语言特征 | 主要指令 | 元数据或运行时 |
| --- | --- | --- |
| Java 类和字段 | `new_object`、`get_field`、`set_field` | 类型表、字段布局 |
| Java 虚方法与接口 | `invoke_virtual`、`invoke_interface` | 虚方法槽、接口实现表 |
| Java 异常与 `finally` | `throw`、`rethrow`、`end_finally` | 异常处理表 |
| Java 静态字段与类初始化 | `load_global`、`store_global`、`call` | 模块初始化状态机、初始化锁 |
| Java monitor | `host_call` 或运行时函数、原子指令 | monitor 表、任务阻塞队列 |
| JavaScript 动态值 | `box`、`unbox_checked`、`call_dynamic` | `any`、属性表、原型链运行时 |
| JavaScript 闭包 | `new_closure`、`get/set_upvalue` | 环境对象 |
| Promise 与 `async/await` | `task_await` | Promise 运行时、事件循环适配器 |
| Node.js 文件和网络 | `host_call_async` | 受 capability 限制的宿主 API |
| Go goroutine | `task_spawn`、`task_yield` | 轻量任务调度器 |
| Go channel 与 `select` | `channel_*`、`channel_select` | channel 等待队列 |
| Go `defer` | `defer_push`、`defer_pop` | 帧内清理栈 |
| Go `panic/recover` | `throw`、`rethrow` | 当前展开状态、defer 运行时 |
| Go interface | `invoke_interface`、`is_instance` | 接口实现表、动态类型 |
| 字符串、map 与 Go slice | 数组和对象指令、运行时调用 | 字符串池、哈希表、slice 描述对象 |
| 反射与运行时类型查询 | `is_instance`、`cast_checked`、运行时调用 | 只读类型元数据、访问权限策略 |
| 多语言 GC | `safepoint`，隐式分配点 | 栈映射、类型引用布局、收集器 |

这张表也是设计验收清单：每个高级特征都能拆成稳定原语，但没有任何一门语言独占整个执行模型。

## 哪些能力不应该成为指令 {#not-an-opcode}

判断一个能力是否应成为 opcode，可以依次问四个问题：

1. 它是否必须直接改变调用帧、操作数栈或控制流？
2. 验证器是否必须理解它才能保证类型或内存安全？
3. 暂停和恢复任务时，VM 是否必须保存它的内部状态？
4. 多种语言是否共享同一组稳定语义？

大多数答案为“否”时，应优先实现为运行时函数。以下能力通常不应该成为指令：

- 字符串查找、正则表达式、JSON、日期和大整数；
- 文件路径、HTTP、DNS、数据库和压缩算法；
- 具体 GC 算法，例如 `minor_gc` 或 `compact_heap`；
- 具体调度策略，例如 work stealing；
- Java 类加载协议、JavaScript 原型链策略或 Go 的包初始化流程；
- 日志、指标和追踪后端；
- JIT 编译、内联和热点计数策略。

相反，`call_indirect`、`throw`、`task_await` 和 `safepoint` 会改变 VM 无法从普通函数调用中完整恢复的执行状态，因此值得成为指令或强制性的内部原语。

## 一个完整的字节码示例 {#complete-example}

先用一个没有对象和并发的程序验证执行核心。源函数计算从 1 到 `n` 的和：

```c
int32_t sum_to(int32_t n) {
    int32_t total = 0;
    for (int32_t i = 1; i <= n; i++) {
        total = total + i;
    }
    return total;
}
```

为三个局部变量分配槽位：`0 = n`、`1 = total`、`2 = i`。对应字节码是：

```text
00  const_i32 0
01  store_local 1
02  const_i32 1
03  store_local 2

04  load_local 2       ; loop:
05  load_local 0
06  i32_le
07  jump_if_false 17

08  load_local 1
09  load_local 2
10  i32_add
11  store_local 1

12  load_local 2
13  const_i32 1
14  i32_add
15  store_local 2
16  jump 4

17  load_local 1       ; end:
18  return
```

### 第一步：实现最小分派循环 {#minimal-dispatch-loop}

为了让代码容易运行，下面先把一条指令表示成 `{opcode, operand}`，暂不压缩成字节流。执行语义与压缩编码相同：

```c
typedef enum {
    OP_CONST_I32,
    OP_LOAD_LOCAL,
    OP_STORE_LOCAL,
    OP_I32_ADD,
    OP_I32_LE,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_RETURN
} OpCode;

typedef struct {
    OpCode op;
    int32_t operand;
} Instruction;

int32_t run_sum_to(int32_t n) {
    int32_t locals[3] = {n, 0, 0};
    int32_t stack[16];
    uint32_t sp = 0;
    uint32_t pc = 0;

    static const Instruction code[] = {
        {OP_CONST_I32, 0},
        {OP_STORE_LOCAL, 1},
        {OP_CONST_I32, 1},
        {OP_STORE_LOCAL, 2},
        {OP_LOAD_LOCAL, 2},
        {OP_LOAD_LOCAL, 0},
        {OP_I32_LE, 0},
        {OP_JUMP_IF_FALSE, 17},
        {OP_LOAD_LOCAL, 1},
        {OP_LOAD_LOCAL, 2},
        {OP_I32_ADD, 0},
        {OP_STORE_LOCAL, 1},
        {OP_LOAD_LOCAL, 2},
        {OP_CONST_I32, 1},
        {OP_I32_ADD, 0},
        {OP_STORE_LOCAL, 2},
        {OP_JUMP, 4},
        {OP_LOAD_LOCAL, 1},
        {OP_RETURN, 0}
    };

    for (;;) {
        Instruction instruction = code[pc++];

        switch (instruction.op) {
        case OP_CONST_I32:
            stack[sp++] = instruction.operand;
            break;
        case OP_LOAD_LOCAL:
            stack[sp++] = locals[instruction.operand];
            break;
        case OP_STORE_LOCAL:
            locals[instruction.operand] = stack[--sp];
            break;
        case OP_I32_ADD: {
            int32_t right = stack[--sp];
            int32_t left = stack[--sp];
            stack[sp++] = (int32_t)((uint32_t)left + (uint32_t)right);
            break;
        }
        case OP_I32_LE: {
            int32_t right = stack[--sp];
            int32_t left = stack[--sp];
            stack[sp++] = left <= right;
            break;
        }
        case OP_JUMP:
            pc = (uint32_t)instruction.operand;
            break;
        case OP_JUMP_IF_FALSE:
            if (!stack[--sp]) pc = (uint32_t)instruction.operand;
            break;
        case OP_RETURN:
            return stack[--sp];
        }
    }
}
```

### 第二步：编译并运行 {#compile-and-run}

在 `main` 中调用它：

```c
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    int32_t result = run_sum_to(5);
    printf("sum_to(5) = %" PRId32 "\n", result);
    return result == 15 ? 0 : 1;
}
```

```bash
cc -std=c11 -Wall -Wextra -Werror vm.c -o vm
./vm
```

仓库中的[完整可运行源码](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/vm.c)额外加入了边界检查、非法 opcode 检查和指令预算，可以直接下载实验。它还接受 `n` 和指令预算两个参数，因此可以同时观察成功与受控失败：

```bash
cc -std=c11 -Wall -Wextra -Werror examples/stack-vm/vm.c -o /tmp/stack-vm
/tmp/stack-vm 5 1000
/tmp/stack-vm 5 5       # 预期失败：instruction budget exhausted
```

预期输出：

```text
sum_to(5) = 15
```

### 第三步：把教学原型升级为真正 VM {#upgrade-prototype}

这个小程序故意省略了生产 VM 必须具备的检查。按以下顺序补齐：

1. 将 `int32_t stack[]` 改为 `VmValue stack[]`；
2. 在加载阶段验证局部槽、跳转目标和栈效果；
3. 把指令数组编码成 opcode 加 LEB128 操作数；
4. 加入函数表和真正的 `VmFrame`；
5. 为每个任务维护独立帧链和指令预算；
6. 加入对象句柄、类型表和标记清除 GC；
7. 最后接入 capability 控制的宿主函数。

不要一开始就实现 Java 类、Promise 和 channel。如果连这个循环程序都无法被验证、暂停、恢复和计费，更高级的功能只会掩盖执行核心中的错误。

## 分阶段实现路线图 {#implementation-roadmap}

可以把完整 VM 拆成八个每一步都可运行的里程碑。

### 里程碑 1：整数表达式 {#milestone-1}

实现常量、局部变量、整数算术和 `return`。用 C 数组手写指令，运行 `1 + 2 * 3`。

验收：结果为 7；栈下溢和上溢能报告明确错误。

### 里程碑 2：控制流和验证器 {#milestone-2}

加入比较、跳转、`switch` 和函数级验证。运行阶乘、求和与循环。

验收：非法跳转、不同栈高度汇合和错误操作数类型在执行前被拒绝。

### 里程碑 3：函数和模块 {#milestone-3}

加入常量池、函数表、调用帧、静态调用、间接调用和导入导出。

验收：递归函数正确运行；超过调用深度限制时安全终止。

### 里程碑 4：对象、数组和闭包 {#milestone-4}

加入类型表、受管引用、字段、数组、虚方法、接口和闭包环境。

验收：数组越界、空引用、错误转换和不兼容间接调用都无法破坏 VM。

### 里程碑 5：GC {#milestone-5}

实现句柄表、停止世界标记清除、栈映射、安全点和写屏障调用点。

验收：循环分配后堆内存保持稳定；多个任务和闭包中的存活对象不会被误回收。

### 里程碑 6：异常和清理 {#milestone-6}

加入异常处理表、栈展开、defer 和结构化 trap。

验收：跨多层函数抛出后处理器正确收到异常，所有 defer 恰好逆序执行一次。

### 里程碑 7：任务和 channel {#milestone-7}

先实现单线程协作调度，再实现 `await`、channel、`select` 和取消。

验收：生产者消费者、多个等待者、关闭 channel 和取消竞态都有确定结果。

### 里程碑 8：沙箱和宿主集成 {#milestone-8}

加入 capability、指令与资源配额、异步宿主调用和错误信息脱敏。最后才考虑多 OS 线程和原子操作。

验收：不授予文件能力时模块无法读取文件；无限循环、递归、内存膨胀和宿主调用滥用都会在额度内终止。

完成这八步后，VM 已经具备承载多种新语言的功能基础。后续的 JIT、压缩对象布局、分代 GC 和并行调度器都是可替换的优化层，而不是重新设计语言语义。

## 完成标准：怎样知道第一版真的做完了 {#definition-of-done}

不要用“指令数量够多”判断 VM 是否完整。第一版达到以下标准，才算形成了可继续演进的闭环：

- 同一份模块先解码、再验证，验证失败时绝不执行；
- 每条指令都有明确的栈效果、错误语义和跨平台数值语义；
- 函数、对象、闭包、异常和动态值都能由至少一个小程序端到端验证；
- GC 能从全局变量、所有任务、channel、异常和宿主句柄中找到根；
- 任务可以在 `await`、channel 和安全点暂停，并且只恢复一次；
- 指令、内存、调用深度、任务和宿主资源都受到不可绕过的配额约束；
- 没有 capability 的模块无法接触对应宿主资源；
- 非法模块、预算耗尽、取消和宿主失败都有自动化负面测试。

做到这里，你构建的还不是 JVM、V8 或 Go runtime 的替代品，而是一个语义边界清楚、能承载多种语言前端的教学 VM。下一步最有价值的工作不是立即写 JIT，而是为每个里程碑保存最小测试模块，让以后更换 GC、调度器或解释器实现时仍能证明行为没有改变。
