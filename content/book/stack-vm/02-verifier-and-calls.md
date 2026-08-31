---
title: 第 2 章：在执行前证明字节码安全
linkTitle: 02 验证器与函数
description: 加入函数元数据、调用帧和工作队列验证器，并运行递归阶乘。
weight: 20
draft: false
---

上一章的边界检查能阻止最直接的宿主内存破坏，但解释器仍要在运行途中发现栈类型错误。本章加入验证器：模块只有在所有可达路径都满足类型和栈高度规则时才会执行。

## 1. 把运行时值与验证器类型分开 {#types-and-values}

验证器只关心槽位中允许出现哪类值；解释器保存实际值：

```c
typedef enum {
    SVM_TYPE_UNINIT,
    SVM_TYPE_I32,
    SVM_TYPE_BOOL,
    SVM_TYPE_NULL,
    SVM_TYPE_REF
} SvmType;

typedef struct {
    SvmType type;
    union {
        int32_t i32;
        bool boolean;
        uint32_t ref;
    } as;
} SvmValue;
```

`UNINIT` 只存在于验证阶段，用来拒绝尚未赋值的局部变量读取。它永远不应该成为一个正常运行时值。

## 2. 为函数声明边界 {#function-metadata}

每个函数声明参数、结果、局部变量数量、最大栈深和代码：

```c
typedef struct {
    const char *name;
    const SvmType *parameter_types;
    uint32_t parameter_count;
    SvmType result_type;
    uint32_t local_count;
    uint32_t max_stack;
    const SvmInstruction *code;
    uint32_t code_count;
} SvmFunction;
```

为什么让模块声明 `max_stack`？加载器可以在分配帧之前设置上限，验证器可以证明任何路径都不会超过它，解释器因此不需要让客体程序控制宿主内存分配大小。

## 3. 验证状态是什么 {#verify-state}

对每个指令位置，验证器保存到达该位置时的局部变量类型和操作数栈类型：

```c
typedef struct {
    bool reached;
    SvmType locals[SVM_MAX_LOCALS];
    SvmType stack[SVM_MAX_STACK];
    uint32_t stack_size;
} VerifyState;
```

它不保存整数的实际数值。验证 `i32_add` 时，只需确认栈顶两个类型都是 `I32`，弹出它们，再压入一个 `I32`。

## 4. 为什么不能只线性扫描一次 {#worklist}

条件分支和循环会让同一条指令拥有多个前驱，因此使用工作队列：

```text
state[0] = 参数已经初始化、其余局部变量未初始化、操作数栈为空
worklist = [0]

while worklist 非空:
    pc = 弹出一个位置
    out = 按指令栈效果转换 state[pc]
    for 每个后继位置:
        把 out 合并进 state[后继]
        若首次到达，把后继加入 worklist
```

第一版要求汇合点的栈高、栈类型和局部变量类型完全一致。这条规则比高级类型系统保守，但容易实现和解释。完整实现位于 [`reference/svm.c`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/reference/svm.c) 的 `verify_function`。

## 5. 调用帧与递归 {#call-frames}

执行 `call target` 时：

1. 从调用者栈中按逆序弹出目标函数参数；
2. 检查调用深度额度；
3. 创建新帧，把参数复制到前几个局部槽；
4. 新帧从 `pc = 0` 开始；
5. `return` 弹出结果，销毁当前帧，并把结果压回调用者。

递归不需要特殊 opcode，它只是同一个函数在帧链中出现多次。调用深度上限防止递归耗尽宿主 C 栈或 VM 帧数组。

## 6. 运行测试 {#run-tests}

本章实现集中在 [`reference/svm.h`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/reference/svm.h) 的模块/指令定义和 [`reference/svm.c`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/reference/svm.c) 的 `verify_function`、调用帧逻辑；对应测试是 `test_factorial` 到 `test_call_depth_limit`。建议先照正文写验证器，再逐个解除这些测试的注释或单独调用它们。

```bash
cd examples/stack-vm
make test
```

参考实现会执行递归 `factorial(5)` 并得到 120，同时验证以下模块被拒绝或终止：

- 把 `bool` 和 `i32` 交给 `i32_add`；
- 跳转到函数范围之外；
- 指令预算耗尽；
- 调用深度超过上限。

## 本章验收 {#acceptance}

- 模块在执行前验证，失败模块不进入解释器；
- 循环回边和条件汇合经过同一套状态合并；
- 未初始化局部变量不能读取；
- 函数签名决定调用参数和返回值类型；
- 递归阶乘正确运行，深度超限安全失败。

下一章将把 `ref` 从一个类型名字变成真正的受管句柄，并实现对象、数组和闭包。那时 GC 会第一次参与 VM 的每次分配和字段写入。
