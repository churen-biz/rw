---
title: 附录 B：Mini 语法一页纸
linkTitle: 附录 B Mini
description: Mini v0 语法速查。
weight: 91
draft: false
---

```text
module Name
fn Name(p: T, ...)? -> T { Stmt* }

Stmt := let Name: T = Expr ;
      | Name = Expr ;
      | if (Expr) Block (else Block)?
      | while (Expr) Block
      | return Expr? ;
      | Expr ;
      | Block

Expr := 初级与 + - * <= == 以及调用
T := i32 | bool | void
```

对象/闭包/并发语法在后续章节扩展。
