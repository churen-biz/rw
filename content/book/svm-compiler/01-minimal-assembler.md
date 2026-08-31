---
title: 第 1 章：最小文本汇编器
linkTitle: 01 最小汇编
description: 实现 v0 .sasm 语法、ModuleBuilder 与 svm-as，把 40+2 编成可运行的 .svm。
weight: 10
draft: false
---

本章对应仓库 `examples/svm-compiler` 的 Plan 1 代码：能把下面这段汇编编成模块，并用 `svm-run` 得到 `i32:42`。

## 1. v0 语法 {#grammar}

```text
# comment
.func NAME -> i32
  const_i32 IMM
  i32_add
  return
.end
```

Plan 1 只允许一个函数、结果类型 `i32`、三种助记符。空白与 `#` 行注释被词法器跳过。

示例 `tests/fixtures/answer.sasm`：

```text
# 40 + 2
.func main -> i32
  const_i32 40
  const_i32 2
  i32_add
  return
.end
```

栈效应：`const_i32` 压入一个 `i32`；`i32_add` 弹出两个再压入和；`return` 把栈顶结果交给调用约定。`ModuleBuilder` 在发射时维护栈深度高水位，写入 `SvmFunction.max_stack`，以便验证器接受该函数。

## 2. 组件怎么分工 {#parts}

| 路径 | 职责 |
| --- | --- |
| `src/common/diag.c` / `arena.c` | 带行列号的诊断；解析期 bump 分配 |
| `src/emit/builder.c` | 累积 `SvmInstruction`，组装 `SvmModule` 视图 |
| `src/asm/lex.c` / `parse.c` | `.sasm` → builder 调用 |
| `src/asm/assemble.c` | 读文件 + 对外 `sc_assemble_*` |
| `tools/svm-as.c` | CLI：输入 `.sasm`，调用 `svm_module_write_file` |

编码、验证、执行全部复用 `examples/stack-vm/reference`，本系列不复制 LEB128 或 section 逻辑。

## 3. 构建与测试 {#build}

```bash
cd examples/svm-compiler
make test
make demo
```

`make test` 会：

1. 跑 `build/test-asm`（arena、builder、汇编成功/失败用例）；
2. 跑 `scripts/run-answer.sh`：`svm-as` → `.svm` → `svm-run`，断言输出为 `i32:42`。

失败用例 `tests/fixtures/bad-opcode.sasm` 使用未知助记符，汇编器必须在写出文件前报错。

## 4. 下一步 {#next}

下一章引入局部变量、标签跳转与多函数 `call`。对象、异常与宿主能力在第 4–5 章接上。
