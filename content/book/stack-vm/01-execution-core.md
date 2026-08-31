---
title: 第 1 章：让第一条指令运行起来
linkTitle: 01 执行核心
description: 从空白 vm.c 开始，实现操作数栈、局部变量、跳转、分派循环和指令预算。
weight: 10
draft: false
---

本章只做一件事：让一段手写指令可靠地计算 `sum_to(5)`，得到 15。暂时没有文件格式、函数、对象和 GC；这些能力需要建立在一个不会越界、不会因 C 有符号溢出而产生未定义行为的执行核心上。

## 1. 准备空目录 {#prepare}

需要一个 C11 编译器。在 macOS 上运行 `xcode-select --install`，Linux 可以安装 GCC 或 Clang。然后创建目录：

```bash
mkdir stack-vm
cd stack-vm
touch vm.c
```

以后始终使用严格编译选项：

```bash
cc -std=c11 -Wall -Wextra -Werror -pedantic -g vm.c -o vm
```

`-Wall -Wextra` 打开常见诊断，`-Werror` 防止我们把告警拖到后面，`-pedantic` 避免无意中依赖某个编译器扩展，`-g` 保留调试信息。

## 2. 定义第一组指令 {#opcodes}

把以下内容写入 `vm.c`：

```c
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
```

这里还不编码成真正的字节序列，而是用 C 结构体表示一条指令。这样我们可以先隔离“执行语义”，等验证器正确后再处理变长编码。

为什么 `I32_ADD` 和 `I32_LE` 带类型前缀？因为验证器以后可以在不运行程序的情况下计算栈类型；解释器也不需要猜测两个位模式究竟是整数、浮点数还是引用。

## 3. 建立 VM 状态 {#vm-state}

第一版只需要三个局部变量槽、一个操作数栈、程序计数器和指令预算：

```c
typedef struct {
    int32_t locals[3];
    int32_t stack[16];
    uint32_t stack_size;
    uint32_t pc;
    uint64_t instruction_budget;
} Vm;
```

`pc` 指向下一条要执行的指令。`stack_size` 同时表示当前栈高和下一个可写槽位。预算由解释器扣减，不能依赖不可信程序主动检查自己。

## 4. 所有栈操作都检查边界 {#checked-stack}

不要在每个 opcode 中直接写 `stack[stack_size++]`。把边界集中在两个函数里：

```c
static bool vm_error(const char *message) {
    fprintf(stderr, "VM error: %s\n", message);
    return false;
}

static bool push(Vm *vm, int32_t value) {
    if (vm->stack_size >= 16) {
        return vm_error("operand stack overflow");
    }
    vm->stack[vm->stack_size++] = value;
    return true;
}

static bool pop(Vm *vm, int32_t *result) {
    if (vm->stack_size == 0) {
        return vm_error("operand stack underflow");
    }
    *result = vm->stack[--vm->stack_size];
    return true;
}
```

这是沙箱的第一条不变量：无论客体程序多么错误，都不能让宿主 C 程序访问数组边界之外的内存。

## 5. 手写一个循环程序 {#sum-program}

三个局部槽分别表示 `n`、`total` 和 `i`：

```c
static const Instruction SUM_TO_CODE[] = {
    {OP_CONST_I32, 0}, {OP_STORE_LOCAL, 1},
    {OP_CONST_I32, 1}, {OP_STORE_LOCAL, 2},
    {OP_LOAD_LOCAL, 2}, {OP_LOAD_LOCAL, 0},
    {OP_I32_LE, 0}, {OP_JUMP_IF_FALSE, 17},
    {OP_LOAD_LOCAL, 1}, {OP_LOAD_LOCAL, 2},
    {OP_I32_ADD, 0}, {OP_STORE_LOCAL, 1},
    {OP_LOAD_LOCAL, 2}, {OP_CONST_I32, 1},
    {OP_I32_ADD, 0}, {OP_STORE_LOCAL, 2},
    {OP_JUMP, 4},
    {OP_LOAD_LOCAL, 1}, {OP_RETURN, 0}
};
```

`jump_if_false 17` 弹出布尔结果；为假时跳到第 17 条指令，否则继续循环体。跳转目标必须是指令编号，而不是 C 指针。

## 6. 写取指—解码—执行循环 {#dispatch-loop}

循环的固定顺序是：扣预算、检查 `pc`、取指并递增 `pc`、执行指令。完整代码保存在仓库的 [`vm.c`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/vm.c)。阅读其中的 `run_sum_to`，重点观察三点：

1. 每次局部变量访问和跳转都检查范围；
2. 未知 opcode 返回错误，而不是落入任意代码路径；
3. `i32_add` 先转成 `uint32_t` 相加，再转回 `int32_t`，明确规定二进制回绕并避开 C 有符号溢出的未定义行为。

## 7. 编译并验证成功与失败 {#test}

```bash
cc -std=c11 -Wall -Wextra -Werror -pedantic -g vm.c -o vm
./vm 5 1000
./vm 5 5
```

第一条输出：

```text
sum_to(5) = 15
```

第二条必须失败并输出：

```text
VM error: instruction budget exhausted
```

如果使用教程仓库，可以一次运行本章所有正反测试：

```bash
cd examples/stack-vm
make test
```

## 本章验收 {#acceptance}

- `n = 5`、`0` 和负数得到确定结果；
- 非法数字参数被拒绝；
- 小预算可以终止循环；
- 所有数组访问之前都有边界检查；
- 严格编译没有告警。

下一章会取消“相信手写指令一定正确”这个假设。模块只有通过控制流和栈类型验证，才有资格进入这里写出的分派循环。
