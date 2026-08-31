---
title: 第 7 章：把字节码变成可安全加载的模块文件
linkTitle: 07 二进制模块
description: 设计带 section 目录的 SVM 文件格式，用 signed LEB128 编码操作数，并在分配和执行前拒绝截断、重叠与非法字节码。
weight: 70
draft: false
---

到目前为止，测试直接用 C 数组构造 `SvmModule`。这证明了 VM 核心，却还不能运行磁盘上的程序。本章补上真正的加载链路：

```text
SvmModule → encode → answer.svm → read → decode → verify → execute
```

这里最重要的不是“把结构体写进文件”，而是建立一条不信任输入的边界。模块文件可能来自编译器，也可能来自攻击者；加载器不能相信其中任何长度、偏移、opcode 或类型。

## 1. 为什么不能直接 fwrite 结构体 {#no-struct-dump}

C 结构体含指针、对齐填充和平台相关布局。把 `SvmFunction` 原样写入文件，会把当前进程地址写进去；换一台机器后，这些地址没有意义。32/64 位、大小端和编译器 ABI 也可能不同。

因此文件只保存稳定的标量数据：固定宽度整数、字节、UTF-8 字符串和显式长度。所有 C 指针都由解码器重新分配和连接。

## 2. 文件头和 section 目录 {#directory}

SVM 1.0 使用三个 section：记录类型、函数、宿主导入。文件前缀为：

```text
magic[4] = "SVM\0"
version:u32
section_count:u32

重复 section_count 次：
  kind:u32
  offset:u32
  size:u32
```

section 目录让加载器先验证全局布局，再解析内容。必须先检查：

1. magic、版本和 section 数量正确；
2. 每种 section 恰好出现一次；
3. `offset >= header_end`；
4. 用更宽的 `uint64_t` 计算 `offset + size`，结果不超过文件长度；
5. 任意两个 section 不重叠。

第 4 条尤其重要。若直接用 `uint32_t end = offset + size`，加法溢出可能把一个文件末尾之外的范围伪装成很小的合法数字。

## 3. 游标读取器：每次读取都证明边界 {#cursor}

解码器不做未经检查的 `bytes[position++]`。所有读取都经过 `cursor_take`：

```c
static bool cursor_take(Cursor *cursor, size_t count,
                        const uint8_t **bytes) {
    if (cursor->position > cursor->size) return false;
    if (count > cursor->size - cursor->position) return false;
    *bytes = cursor->data + cursor->position;
    cursor->position += count;
    return true;
}
```

为什么先检查 `position > size`？因为 `size_t` 是无符号数，若状态已经损坏，直接计算 `size - position` 会下溢成一个巨大的正数。这个小检查把“先减法再比较”改成了安全的证明顺序。

每个 section 解码结束后还要求 `position == size`。多余字节不是无害的：宽松解析会让签名工具与运行时对同一文件产生不同理解。

## 4. 为什么操作数使用 signed LEB128 {#sleb128}

opcode 固定为一个字节，而两个 `int32_t` 操作数使用 signed LEB128。它每个字节提供 7 位数据，最高位表示“后面还有字节”。小整数通常只占一个字节，同时能正确表示负常量。

解码器最多读取 5 字节，因为 `int32_t` 最多只需要 5 个 LEB128 字节；第 5 个字节后仍未结束就拒绝。累积时先放进 `uint64_t`，完成符号扩展后再检查是否落在 `INT32_MIN..INT32_MAX`。不能边移位边写进有符号 `int32_t`，那会把恶意输入变成未定义行为。

## 5. 解码成功不等于模块可执行 {#verify-after-decode}

解码只回答“字节能否组成数据结构”；验证器才回答“这段程序是否类型安全”。所以公开入口 `svm_module_decode` 在返回成功前总会调用：

```c
svm_verify_module(&owned->module, error)
```

未知 opcode、越界跳转、不一致的栈高度、错误字段类型等，都在 VM 执行第一条指令前被拒绝。这也解释了为什么 loader 与 verifier 必须是两个模块：格式变化不应复制控制流类型分析，而测试可以分别定位格式错误和语义错误。

## 6. 所有权与失败清理 {#ownership}

解码过程中任何一步都可能失败。`SvmOwnedModule` 明确拥有重新分配的名字、参数类型、指令和异常表；失败路径与正常路径都调用同一个 `svm_owned_module_destroy`。

字段应在分配成功后立即写进 owned 结构。若先读取两个字符串，等第三步成功后才统一赋值，那么第二步失败时，第一个字符串没有可达指针，清理器无法释放它。这是解析器里很常见的“只在坏输入上发生”的泄漏。

## 7. 运行测试和真实文件闭环 {#run}

```bash
cd examples/stack-vm
make test
make demo
```

`make demo` 会先编译 `tools/make-example.c`，生成 `build/answer.svm`，再交给 `build/svm-run` 加载执行，输出：

```text
wrote build/answer.svm
i32:42
```

模块测试同时破坏 magic、截断末尾、制造 section 重叠、替换未知 opcode。只有成功往返测试是不够的；加载器的核心价值正是它如何面对坏输入。

下一章把全部组件放进一条可重复的工程流程，并逐项对照“完整 VM”的验收标准。
