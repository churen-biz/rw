---
title: 第 4 章：记录类型、对象与数组
linkTitle: 04 对象与数组
description: 用 .type 声明记录布局，汇编 new_object/get_field/set_field 与数组指令。
weight: 40
draft: false
---

## 1. 声明记录 {#type}

```text
.type Box {
  x: i32
}
```

字段按下标访问（`0` 起）。`new_object Box` 把类型名解析为模块类型表下标。

## 2. 对象示例 {#object}

`object_box.sasm`：创建对象、写字段、`gc_collect`、再读回 `42`。

```text
new_object Box
dup
const_i32 42
set_field Box 0
gc_collect
get_field Box 0
return
```

`get_field` / `set_field` 编码 `(type_index, field_index)`，与 SVM 验证器一致。

## 3. 数组示例 {#array}

`array_get.sasm`：长度 3 的 `i32` 数组，写入下标 1 再读出 `7`。

```text
const_i32 3
new_array i32
...
array_set i32
array_get i32
```

元素类型必须是验证器允许的数组元素类型（`i32` / `bool` / `ref`）。

## 4. 测试 {#test}

```bash
cd examples/svm-compiler
make test
```

涵盖 `ok assemble object` 与 `ok assemble array`。
