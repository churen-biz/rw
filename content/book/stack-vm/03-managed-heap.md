---
title: 第 3 章：对象、数组、闭包与第一版 GC
linkTitle: 03 受管堆与闭包
description: 用稳定句柄建立受管堆，把对象、类型化数组和闭包接入验证器与解释器，并完成标记清除。
weight: 30
draft: false
---

前两章中的 `ref` 还只是验证器认识的类型。本章让它真正指向受管堆对象，并实现三种对象：记录对象、数组和闭包。章节结束时，一段字节码可以创建对象、写字段、主动触发 GC，再从仍然存活的对象中读回结果。

## 1. 为什么客体不能保存 C 指针 {#why-handles}

如果把 `malloc` 返回的地址直接放进 `SvmValue`，客体程序就可能伪造地址，GC 移动对象时还要更新所有引用。教学版改用 32 位句柄：

```c
typedef struct {
    SvmType type;
    union {
        int32_t i32;
        bool boolean;
        uint32_t ref;
    } as;
} SvmValue;
```

句柄 `0` 保留给空引用，其他数字只用于索引 VM 私有的句柄表。客体永远看不到 `SvmHeapObject *`。

本实现还有一条看似浪费、但很重要的规则：同一个 VM 实例中，GC 释放的句柄不再复用。如果旧句柄被宿主错误地保留，复用会让它悄悄指向完全不同的新对象。生产实现可以使用“槽位编号 + generation”解决 ABA 问题；第一版用有限的句柄额度换取更容易证明的语义。

## 2. 用同一个对象头承载三种对象 {#object-header}

完整定义在 [`heap.c`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/reference/heap.c)：

```c
struct SvmHeapObject {
    SvmObjectKind kind;
    bool marked;
    uint32_t handle;
    uint32_t metadata;
    SvmType element_type;
    uint32_t value_count;
    size_t allocated_bytes;
    struct SvmHeapObject *next;
    SvmValue values[];
};
```

- 记录对象用 `metadata` 保存类型表索引，`values` 保存字段；
- 数组用 `element_type` 保存元素类型，`values` 保存元素；
- 闭包用 `metadata` 保存函数索引，`values` 保存捕获值；
- `next` 把所有对象串成链表，供 GC 清扫；
- `allocated_bytes` 让释放对象时能准确归还堆额度。

柔性数组成员 `values[]` 让对象头和数据一次分配。计算大小前必须检查乘法和加法溢出，检查配额后才允许调用 `calloc`。

## 3. 先声明记录类型，再验证字段指令 {#record-types}

模块中的记录类型描述字段，而对象实例不重复保存字段名：

```c
typedef struct {
    const char *name;
    const SvmType *field_types;
    uint32_t field_count;
} SvmRecordType;
```

三条对象指令的栈效果是：

```text
new_object type             -- ref
get_field type, field       ref -- field_type
set_field type, field       ref, field_type --
```

为什么 `get_field` 同时编码类型索引和字段索引？当前验证器只追踪粗粒度 `ref`，无法从栈类型得知它是 `Box` 还是 `User`。类型索引让验证器找到字段类型；运行时再检查实际对象的类型索引是否相同。两层检查各自解决不同问题：验证器保证栈形状，运行时阻止把合法引用当成错误的记录类型使用。

对象分配时，`i32` 和 `bool` 字段初始化为零值，`ref` 字段初始化为 `null`。不能让字段保留 `UNINIT`，否则验证器以为 `get_field` 得到声明类型，解释器却可能读出不存在的运行时值。

## 4. 数组必须同时检查长度、索引和元素类型 {#arrays}

数组指令是：

```text
new_array element_type      i32_length -- ref
array_len element_type      ref -- i32
array_get element_type      ref, i32_index -- element_type
array_set element_type      ref, i32_index, element_type --
```

验证器证明长度和索引是 `i32`，元素类型与 opcode 一致；解释器仍需检查长度非负、索引在范围内，而且引用实际指向相同元素类型的数组。后一个检查不能省略，因为两个不同数组在验证器中目前都是 `ref`。

## 5. 闭包就是“函数编号 + 捕获值” {#closures}

闭包函数把捕获参数放在普通参数之前。例如 `x => x + base` 的函数签名可以表示为：

```text
parameter_types = [i32 base, i32 x]
capture_count = 1
```

创建和调用使用：

```text
new_closure function, 1    i32_base -- ref
call_closure function      ref, i32_x -- i32
```

`new_closure` 把捕获值复制进堆对象。`call_closure` 先核对闭包中记录的函数编号，再把捕获值与显式参数拼成新帧的局部变量 `[base, x]`。把目标函数索引编码在调用指令中，使第一版验证器仍能静态确定参数和结果类型；以后实现完全动态调用时，可以增加带签名索引的 `call_indirect`。

## 6. 从所有帧收集 GC 根 {#gc-roots}

第一版 GC 是停止世界、非移动、标记清除。执行 `gc_collect` 时，解释器扫描每个调用帧中已声明的局部变量和当前操作数栈：

```c
for (uint32_t f = 0; f < vm->frame_count; f++) {
    for (uint32_t i = 0; i < frame->function->local_count; i++)
        roots[root_count++] = frame->locals[i];
    for (uint32_t i = 0; i < frame->stack_size; i++)
        roots[root_count++] = frame->stack[i];
}
```

标记阶段解析每个 `ref` 句柄，并递归访问对象的 `values`。清扫阶段释放未标记对象、清空对应句柄槽，并扣减 `bytes_used`。

这里使用带标签的 `SvmValue`，所以可以安全扫描所有活动槽。后续若把 `i32` 和引用压缩成无标签机器字，就必须由验证器为每个安全点生成精确栈映射。

## 7. 分配过程中的临时根 {#temporary-roots}

一个容易漏掉的顺序问题出现在闭包分配：捕获值原本在操作数栈上。如果先弹出捕获值、再因分配触发 GC，它们已经不在根集合里，引用对象可能被提前回收。

本实现先收集，再弹出捕获值并分配闭包：

```text
收集当前根 → 弹出 captures → 分配闭包 → 复制 captures → 压入闭包引用
```

更成熟的 VM 会提供“宿主临时根栈”，让 C 局部变量中的受管引用也能显式注册。无论采用哪种方案，不变量都是：任何可能触发 GC 的调用发生时，每个活引用必须位于 GC 能找到的位置。

## 8. 运行端到端测试 {#tests}

先实现 [`reference/heap.h`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/reference/heap.h) 与 [`reference/heap.c`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/reference/heap.c)，用 `tests/test-heap.c` 独立验证；再把分配、访问和 GC 根扫描接入 `reference/svm.c`，运行 `tests/test-reference.c` 中三个对象程序。

```bash
cd examples/stack-vm
make test
make sanitize
```

测试覆盖：

- 父对象引用子对象时，传递标记保留两者；
- 清除全部根后，对象和堆字节归零；
- 陈旧句柄不能解析，也不会被新对象复用；
- 数组拒绝错误元素类型和越界索引；
- 闭包保留函数编号和捕获值；
- 字节码对象跨显式 GC 后仍读回字段值 42；
- 字节码闭包计算 `10 + 5 = 15`；
- 用错误记录类型访问字段时，运行时安全失败；
- 堆字节和句柄额度均不可绕过。

## 本章验收 {#acceptance}

- 客体值不包含宿主指针；
- 对象、数组和闭包都只能通过受检指令访问；
- 每次分配同时受字节额度和句柄额度约束；
- GC 能沿嵌套对象和闭包捕获传递标记；
- 活引用在可能触发 GC 时始终可见；
- 验证器错误与运行时类型错误有清楚边界。

下一章会加入异常处理表和跨帧展开。关键挑战不是简单地“跳到 catch”，而是保证每个离开的帧只运行一次清理动作，并在清理动作再次抛异常时保持 VM 状态一致。
