# SVM 编译器系列设计：从文本汇编到 Mini → `.svm`

日期：2026-08-31  
状态：已评审（对话确认 §1–§4）  
相关：`content/book/stack-vm/`、`examples/stack-vm/`、`content/blog/design-a-stack-vm.md`

## 1. 目标与非目标

### 目标

面向初学者，用 **C11** 逐步实现一条可运行的编译链路：

1. **阶段 A**：文本汇编 `svm-asm`（`.sasm`）→ `SvmModule` → `.svm`
2. **阶段 B**：中性小语言 **Mini**（`.mini`）→ AST → **带基本块的独立 IR** → 同一套模块构建/编码 → `.svm`

系列覆盖与教学 VM 对齐的能力面，直到 **capability / host_call**。读者理解原理，并按章实现可测试的工具，而不是只读映射表。

成功标准：

- `examples/svm-compiler` 下 `make test` 全绿（asm、mini、若干 IR golden）。
- `make demo`：Mini 示例 → `.svm` → 在授予 mock capability 时跑通；未授予则按 VM 语义失败。
- 每章至少 1 个成功路径 + 1 个前端拒绝用例；涉及安全边界时增加 VM 侧拒绝回归。
- 正文含 Mini / IR / `.sasm` / opcode 对照，避免「只有结论没有例子」。

### 非目标

- 优化（激进常量折叠、内联、逃逸分析）、完整 SSA φ 网络、JIT。
- 兼容真实 Go / JavaScript / Java 语法或标准库。
- 自举编译器、分发生产 ABI。
- 重写或 fork 栈式 VM；编码、验证、执行仍使用 `examples/stack-vm/reference` 与 `svm-run`。

## 2. 约束与既定选择

| 项 | 选择 |
| --- | --- |
| 路径结构 | 双书脊：先完整汇编器，再 Mini+IR（方案 1） |
| 实现语言 | C11，与 VM 教程一致 |
| 源语言 | 中性 Mini（`fn` / `type` / `spawn` 等），不假装兼容真语言 |
| 功能深度 | 全覆盖到 capability |
| IR | 独立 CFG + 线性指令；跨块用显式 copy，第一版不做 φ |
| 文档自洽 | 附 ISA / `.svm` / Mini 语法 / IR 速查；不强制先读完 VM 书 |
| 汇合后端 | Asm 与 Mini 共用 `ModuleBuilder` → `svm_module_write_*` |

## 3. 系列骨架与章节地图

**书名（暂定）**：《从文本汇编到 Mini 编译器》  
**文档路径**：`content/book/svm-compiler/`  
**代码路径**：`examples/svm-compiler/`

```text
阶段 A  .sasm ──解析──► SvmModule ──encode──► .svm
阶段 B  .mini ──AST──► CFG/IR ──lower──► 同一 SvmModule / emitter
```

| 章 | 内容 | 可观察产物 |
| --- | --- | --- |
| 00 | 目标、与 VM 书关系、速查附录用法 | 心智模型 |
| 01 | 词法/位置；最小 `.sasm`（const/add/return） | `svm-as` → `answer.svm` |
| 02 | 函数、标签、局部变量、分支 | 阶乘/循环模块 |
| 03 | 完整指令表接入；与 verifier 错误对照 | 非法 asm 被拒绝 |
| 04 | 记录类型、对象/数组文本语法 | 堆上小例子 |
| 05 | 异常表、defer、任务/channel、host/capability 的 asm 语法 | 阶段 A 全 opcode 可手写 |
| 06 | Mini 语法 v0；递归下降 AST | `mini-parse` / AST 打印 |
| 07 | 名称解析与类型检查 | 语义错误诊断 |
| 08 | AST → CFG；显式 copy，说明为何暂不做 SSA | IR/CFG dump |
| 09 | IR → SVM：表达式、控制流、调用 | Mini 整数程序 → `.svm` |
| 10 | `type` / `new` / 字段 / 数组 lowering | 对象程序 |
| 11 | 闭包与受控 `any` | 闭包程序 |
| 12 | `throw` / `try` / `defer` | 异常程序 |
| 13 | `spawn` / `chan` / `select` 子集 | 并发程序 |
| 14 | `import` / capability / `host_call` 约定 | 沙箱 Demo |
| 15 | 端到端示例集 + 与 VM 书交叉索引 | `make test && make demo` |
| 附录 A | ISA / `.svm` 速查 | 自洽阅读 |
| 附录 B | Mini 语法一页纸 | |
| 附录 C | IR 操作一览 | |

阶段 A（01–05）结束：读者能手写全 opcode 面。  
阶段 B（06–14）：同一面改为生成。  
第 15 章：组装与验收。

## 4. 管线、IR 与共享后端

### 共享后端

```text
AsmParser ──┐
            ├──► ModuleBuilder ──► svm_module_write_file
MiniLower ──┘         ▲
                      └── IrEmitter：CFG/IR → SvmInstruction[]
```

- 复用 `examples/stack-vm/reference` 的 `module.h` / `svm.h`（及实现），不复制 LEB128 / section 逻辑。
- 本系列实现：lexer、asm 语法、Mini 前端、IR、lowering、诊断、工具入口。
- 执行使用已构建的 `svm-run`；Makefile 声明对 `../stack-vm` 的依赖。

### IR 形态（教学向）

- 函数 = CFG：`BasicBlock` 列表；块末终结符为 `goto` / `branch` / `return` / `throw` 等。
- 块内线性指令；操作数为临时名（如 `%t0`）或局部槽。
- 跨块传值：前驱 `store` + 后继 `load`（显式 copy）。第 8 章说明真编译器常用 SSA，本教程为何先不用。
- IR 可文本 dump，用于「类型检查之后、编码之前」的测试金样。

### 阶段 B 数据流

```text
.mini → Lexer → Parser → AST
     → Resolver / Typechecker
     → AstToCfg
     → LowerToSvm → ModuleBuilder → .svm → svm-run
```

阶段 A **不经过** CFG：asm 解析后直接填 `ModuleBuilder`（标签 → 指令下标）。读者先熟悉字节码形态，再学习基本块。

### 错误分层

| 层 | 示例 | 是否生成 `.svm` |
| --- | --- | --- |
| 词法/语法 | 缺 `}` | 否 |
| 语义 | 未定义名、类型不匹配、未声明 host | 否 |
| 汇编绑定 | 标签缺失、未知助记符 | 否 |
| VM 验证器 / 绑定 | emitter bug 或未授予 capability | 加载或实例化失败 |

前端尽量报告行号。不以 verifier 为唯一正确性靠山，但保留坏模块 / 缺权限的 VM 侧回归。

## 5. Mini 语言面

### 类型（与 SVM 对齐）

| Mini | SVM |
| --- | --- |
| `i32`, `bool`, `void` | 同名语义 |
| `ref T`（`type` 记录） | `REF` + 类型表 |
| `[]T` | 数组 |
| 闭包 / 函数值 | 闭包 ref |
| `any` | `ANY`（仅显式受控汇合 / 检查转换） |
| `task T`, `chan T` | `TASK` / `CHANNEL` |

第一版不做：泛型、方法接收者糖、接口/虚表一等语法。需要动态分派时用普通函数 + 手动表，或附录点到为止。

### 语法示意

```text
module demo
import clock.now() -> i32

type Point { x: i32, y: i32 }

fn add(a: i32, b: i32) -> i32 {
  return a + b
}

fn main() -> i32 {
  let p = new Point { x = 1, y = 2 }
  p.x = p.x + 1
  let t = spawn worker(p.x)
  let ch = chan i32(1)
  send(ch, 1)
  let x = recv(ch)
  try {
    throw 99
  } catch e: i32 {
    defer cleanup()
    return e + host clock.now()
  }
}
```

中性关键字方向：`fn` `type` `let` `new` `spawn` `chan` `send`/`recv`/`select` `try`/`catch`/`throw`/`defer` `import` `host`。拼写在附录 B 钉死，正文不中途改名。

### 有 / 无

| 有 | 无 |
| --- | --- |
| `if` / `while` / `return` | 继承、原型链 |
| 记录字段、数组 | 字符串/正则/map 标准库（可用 host 演示） |
| 嵌套函数 → 闭包（值捕获规则明确） | 复杂可变 upvalue 优化 |
| `try` / `catch` / `throw` / `defer` | 抢占、完整同步原语大全 |
| `spawn` / `await`、缓冲 channel、`select` 子集 | work stealing |
| `import` + `host` | 在语言内「创建」权限 |

### Capability 约定

- 源码只 **声明** `import cap.fn(args) -> ret`（可标 async）。
- 编译器写入 `SvmHostImport`；`host_call` / `host_call_async` 使用导入下标。
- **授予** capability 仅在宿主 C（demo runner）完成；文档强调权限在宿主，不在字节码。
- 类型检查强制 host 调用与 import 签名一致。

每个 Mini 特性章附「等价 `.sasm`」或 IR dump，与阶段 A 对照。

## 6. 仓库布局与教学节奏

```text
content/book/svm-compiler/
  _index.md
  00-overview.md … 15-….md
  appendix-a-isa.md
  appendix-b-mini-grammar.md
  appendix-c-ir.md

examples/svm-compiler/
  README.md
  Makefile                 # 依赖 ../stack-vm/reference 与 svm-run
  include/
  src/
    common/                # 诊断、arena、字符串
    asm/
    mini/
    ir/
    emit/                  # ModuleBuilder → write
  tools/
    svm-as.c
    mini.c                 # 可选 -emit-ir / -emit-sasm
  tests/
    asm/  mini/  ir/
  examples/                # 端到端 demo 源码
```

### 单章结构

1. 本章编译问题 + 完成后的命令  
2. 短原理 + 与手写 opcode 对比  
3. 按测试顺序实现  
4. 对照表（Mini / IR / `.sasm` / opcode）+ 预告  
5. 成功 + 失败测试；必要时 VM 拒绝用例  

### 与现有文档交叉引用

| 本系列 | 指向 |
| --- | --- |
| 附录 A、章 03 | `stack-vm` 验证器、`SPEC.md`、design blog ISA |
| 对象/闭包 | `stack-vm` 03（本系列只讲 lowering） |
| 异常/任务 | `stack-vm` 04–05 |
| capability | `stack-vm` 06；绑定示例在本系列 demo driver |
| `stack-vm` 08 §lowering | 应链到本系列，补「缺示例」缺口 |
| design blog 语言映射表 | 链到本系列 vignette / 章 |

## 7. 实现分期（供后续 plan 拆解）

建议顺序（写入 plan 时可再拆任务）：

1. 脚手架：`examples/svm-compiler` Makefile、链 reference、空 `svm-as` / 测试框架  
2. 书 `_index.md` + 章 00 + 附录 A 初稿（速查可先从 SPEC/blog 摘编）  
3. 章 01–05 与 asm 实现交错落地  
4. 章 06–09：Mini 到整数/函数闭环  
5. 章 10–14：按特性加厚 lowering + demo  
6. 章 15 + 回头改 `stack-vm/08` 与 blog 的交叉链接  

## 8. 开放但不阻塞的细节

以下可在实现 plan / 首章落地时钉死，不阻塞本设计：

- `.sasm` 具体文法（关键字、注释、标签语法）  
- IR 文本 dump 的精确语法  
- `select` / async host 在 Mini 中的最终糖衣形状  
- demo 提供哪些 mock capability 名称（如 `clock.now`）  

原则：与现有 `SvmHostImport` / opcode 一一可映射，避免为语法再发明 VM 语义。
