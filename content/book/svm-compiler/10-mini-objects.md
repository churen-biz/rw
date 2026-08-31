---
title: 第 10 章：Mini 中的 type / new / 字段
linkTitle: 10 对象 lowering
description: 将记录类型与字段读写降到 new_object/get_field/set_field；当前以 asm 对照为主，Mini 语法随后补齐。
weight: 100
draft: false
---

对象 lowering 目标与 `object_box.sasm` 相同：

```text
.type Box { x: i32 }
new_object / set_field / get_field
```

Mini 目标语法：

```text
type Box { x: i32 }
fn main() -> i32 {
  let b: Box = new Box { x = 42 };
  return b.x;
}
```

前端步骤：类型表 → `ModuleBuilder.add_record`；`new` → `NEW_OBJECT`；`b.x` → `GET_FIELD`；赋值 → `SET_FIELD`。在 Mini 解析器接上之前，请用第 4 章 asm 用例验证后端已就绪。
