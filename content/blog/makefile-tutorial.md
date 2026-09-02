---
title: Makefile 新手教程：从概念到常见工程实践
linkTitle: Makefile 新手教程
date: 2026-09-02
lastmod: 2026-09-02
description: 面向初学者的 GNU Make 教程：目标、依赖、配方、变量与自动变量，并附带单文件、多文件 C 项目、测试、子目录与子项目等常见工程实例。
authors: [lihai03]
categories: [教程]
tags: [Makefile, C, 构建]
draft: false
---

## 为什么需要 Makefile {#why-makefile}

写过程序的人迟早会遇到这类重复劳动：

```bash
gcc -std=c11 -Wall -g -c main.c -o main.o
gcc -std=c11 -Wall -g -c util.c -o util.o
gcc main.o util.o -o app
./app
```

改了一行 `util.c`，却要重新敲一遍全部命令；忘了加 `-Wall` 时，不同人、不同终端里的编译参数还不一致。

**Makefile 的作用，是把「怎么构建」写成一份可重复执行的说明书。** 你运行 `make`，工具会：

1. 读取 `Makefile`（或 `makefile`）里的规则；
2. 比较源文件与产物的时间戳；
3. **只重新构建改过的部分**（增量构建）；
4. 按依赖顺序执行命令。

Makefile 不等于「只能编译 C」。它适合任何「输入文件 → 输出文件」的流程：打包文档、生成代码、跑测试、部署静态资源。但在 C/C++、系统编程和本仓库的 VM 示例里，Makefile 仍然是最常见、最轻量的构建方式。

> [!NOTE]
> 本文以 **GNU Make** 为准（Linux、macOS 默认的 `make` 大多是它）。BSD Make 语法略有不同；企业级 C++ 项目常用 CMake/Bazel，但读懂 Makefile 仍然有价值——很多项目最终都会生成或包装一层 Make。

## 三个核心概念：目标、依赖、配方 {#core-concepts}

Makefile 的一条规则长这样：

```makefile
目标: 依赖1 依赖2 ...
	配方命令1
	配方命令2
```

| 部分 | 含义 | 类比 |
| --- | --- | --- |
| **目标（target）** | 要生成什么 | 菜谱菜名 |
| **依赖（prerequisites）** | 需要哪些输入 | 食材 |
| **配方（recipe）** | 具体怎么做 | 烹饪步骤 |

**示例：把 `hello.c` 编译成可执行文件**

```makefile
hello: hello.c
	gcc hello.c -o hello
```

运行：

```bash
make hello    # 构建名为 hello 的目标
./hello
```

### 第一条铁律：配方行必须用 Tab 缩进

配方（命令行）**必须以 Tab 开头**，不能用空格。这是 Makefile 最著名的坑——编辑器把 Tab 显示成空格时，你会看到 `missing separator` 报错。

### 第二条铁律：make 只关心「文件是否过期」

`make` 默认逻辑：

- 若**目标不存在**，执行配方；
- 若**任一依赖比目标新**，执行配方；
- 否则打印 `is up to date`，跳过。

因此目标通常是**文件名**（`hello`、`main.o`）。后面会讲如何用 `.PHONY` 处理「不是文件」的目标（如 `clean`、`test`）。

### 默认目标

Makefile 里**第一个规则**是默认目标。只敲 `make` 时，构建的就是它：

```makefile
all: hello    # 第一个规则：默认构建 all

hello: hello.c
	gcc hello.c -o hello
```

工程里习惯把聚合目标命名为 `all`。

## 变量：少写重复，统一改参数 {#variables}

### 基本赋值

```makefile
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g
TARGET  := hello

$(TARGET): hello.c
	$(CC) $(CFLAGS) hello.c -o $(TARGET)
```

常见写法：

| 写法 | 含义 |
| --- | --- |
| `=` | 递归展开（晚绑定） |
| `:=` | 立即展开（早绑定，常用） |
| `?=` | 若未定义才赋值（方便命令行覆盖） |
| `+=` | 追加 |

本仓库 `examples/stack-vm/Makefile` 里：

```makefile
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
```

`?=` 让你可以在命令行覆盖默认值：

```bash
make CC=clang CFLAGS='-O2 -g'
```

### 注释

`#` 后面是注释。规则与变量之间空一行，可读性更好。

## 自动变量：配方里的简写 {#automatic-variables}

在配方中，GNU Make 提供一批**自动变量**：

| 变量 | 含义 |
| --- | --- |
| `$@` | 当前规则的目标名 |
| `$<` | 第一个依赖 |
| `$^` | 所有依赖（去重） |
| `$?` | 比目标新的依赖 |

**编译单个 `.c` → `.o` 的典型写法：**

```makefile
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
```

- `$<` → `main.c`
- `$@` → `main.o`

模式规则 `%.o: %.c` 表示「任意 `.o` 由同名 `.c` 生成」，后面实例会反复用到。

## 实例一：单文件 C 程序 {#example-single}

```makefile
CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -g
TARGET := hello

.PHONY: all clean run

all: $(TARGET)

$(TARGET): hello.c
	$(CC) $(CFLAGS) hello.c -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
```

```bash
make          # 构建 hello
make run      # 构建并运行
make clean    # 删除产物
```

### `.PHONY` 是什么？

`clean`、`all`、`run`、`test` 通常**不对应磁盘上的文件**。若本地恰好有个叫 `clean` 的文件，`make clean` 可能误判「已最新」而跳过。

`.PHONY: clean` 告诉 make：**这个目标没有实体文件，每次声明依赖它时都应执行配方。**

## 实例二：多文件 C 项目（基础版） {#example-multi}

目录：

```text
app/
├── Makefile
├── main.c
├── util.c
└── util.h
```

```makefile
CC     := gcc
CFLAGS := -std=c11 -Wall -Wextra -g
OBJS   := main.o util.o
TARGET := app

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
```

依赖关系：

```text
app  →  main.o  util.o
main.o  →  main.c
util.o  →  util.c
```

只改 `util.c` 时，`make` 只会重编 `util.o` 并重新链接 `app`，不会动 `main.o`。

> [!TIP]
> 改 `util.h` 时，上面这份简单 Makefile **不会**自动重编包含它的 `.c` 文件。生产项目应用 `-MMD -MP` 生成头文件依赖（见实例四），或借助 `gcc -M` 维护 `.d` 文件。

## 实例三：输出到 build/ 目录 {#example-build-dir}

把产物放进 `build/`，保持源码目录干净——本仓库 VM 示例采用这种做法。

```makefile
CC        := gcc
CFLAGS    := -std=c11 -Wall -Wextra -g
BUILD_DIR := build
OBJS      := $(BUILD_DIR)/main.o $(BUILD_DIR)/util.o
TARGET    := $(BUILD_DIR)/app

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR)
```

### 顺序依赖 `|`

`$(TARGET): $(OBJS) | $(BUILD_DIR)` 里，`|` 右侧是**顺序依赖（order-only prerequisite）**：

- `build/` 目录必须存在；
- 但目录的时间戳**不会**触发链接步骤重新执行。

没有 `| $(BUILD_DIR)` 时，每次 `touch build` 可能导致不必要的重链接。

## 实例四：头文件依赖与自动依赖生成 {#example-headers}

`main.c` 和 `util.c` 都 `#include "util.h"`。改 `util.h` 应触发两者重编。

```makefile
CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -g -MMD -MP
OBJS    := main.o util.o
TARGET  := app
DEPS    := $(OBJS:.o=.d)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)
```

- `-MMD -MP`：编译时生成 `main.d`、`util.d`，记录「这个 `.o` 实际依赖哪些头文件」；
- `-include $(DEPS)`：把 `.d` 当作 Makefile 片段纳入（文件不存在也不报错）。

这是中小型 C 项目里非常实用的模式。

## 实例五：测试与检查目标 {#example-test}

把测试纳入 Make，团队只需记 `make test`：

```makefile
CC       := gcc
CFLAGS   := -std=c11 -Wall -Wextra -g
BUILD    := build
APP      := $(BUILD)/app
TEST_BIN := $(BUILD)/test_app

.PHONY: all test clean

all: $(APP)

$(BUILD):
	mkdir -p $(BUILD)

$(APP): main.c util.c | $(BUILD)
	$(CC) $(CFLAGS) main.c util.c -o $@

$(TEST_BIN): tests/test_app.c util.c | $(BUILD)
	$(CC) $(CFLAGS) tests/test_app.c util.c -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

clean:
	rm -rf $(BUILD)
```

可继续加：

```makefile
.PHONY: sanitize
sanitize: CFLAGS += -fsanitize=address,undefined
sanitize: clean test
```

这与本仓库 `examples/stack-vm/Makefile` 里的 `sanitize` 目标思路一致：用变量追加切换编译选项，再 `clean test` 全量重测。

## 实例六：多子目录与变量列表 {#example-subdirs}

项目变大后，源文件分散在多个目录：

```text
mylib/
├── Makefile
├── include/
│   └── mylib.h
├── src/
│   ├── core.c
│   └── io.c
└── tools/
    └── cli.c
```

```makefile
CC       := gcc
CPPFLAGS := -Iinclude
CFLAGS   := -std=c11 -Wall -Wextra -g
BUILD    := build

SRCS := src/core.c src/io.c tools/cli.c
OBJS := $(SRCS:%.c=$(BUILD)/%.o)
TARGET := $(BUILD)/myapp

.PHONY: all clean

all: $(TARGET)

$(BUILD)/%.o: %.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
```

技巧说明：

- `$(SRCS:%.c=$(BUILD)/%.o)`：批量改写路径；
- `$(dir $@)`：取出目标所在目录，`mkdir -p` 创建嵌套目录；
- `@` 前缀：执行命令时不打印命令行本身（减少噪音）。

## 实例七：调用子项目 Makefile {#example-submake}

本仓库 `examples/svm-compiler` 依赖 `examples/stack-vm`，用**子 make** 构建：

```makefile
STACK_VM := ../stack-vm

.PHONY: all stack-vm-deps

all: stack-vm-deps $(BUILD_DIR)/mini

stack-vm-deps:
	$(MAKE) -C $(STACK_VM) all
```

- `$(MAKE)`：用当前 make 程序（保留命令行参数）；
- `-C dir`：先进入目录再读那里的 Makefile。

适合「仓库里有多个可独立构建的子模块」的单体仓库（monorepo）。

## 实例八：伪目标组合与「常用命令入口」 {#example-phony}

整理一组团队常用入口：

```makefile
.PHONY: all build test demo clean format help

all: build

build: $(TARGET)

test: build
	./run_tests.sh

demo: build
	./$(TARGET) --demo

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "Targets:"
	@echo "  make build   - compile"
	@echo "  make test    - run tests"
	@echo "  make demo    - run demo"
	@echo "  make clean   - remove build artifacts"
```

`help` 不是必须，但能降低 onboarding 成本。大型项目里也有人用 `make` 不带参数时默认显示 `help`。

## 调试 Makefile {#debugging}

### 打印实际会执行的命令

```bash
make -n          # dry-run：只打印，不执行
make --trace     # 打印每条规则为何被选中（GNU Make 4.x）
```

### 查看变量最终值

```makefile
debug:
	$(info CC=$(CC))
	$(info CFLAGS=$(CFLAGS))
```

或命令行：

```bash
make -p          # 打印完整数据库（冗长）
make debug       # 若定义了 debug 目标
```

### 常见报错

| 报错 | 常见原因 |
| --- | --- |
| `missing separator` | 配方行用了空格而非 Tab |
| `No rule to make target 'foo'` | 目标或依赖文件名写错 |
| 改了头文件没重编 | 未加入头文件依赖 |
| `make: Nothing to be done` | 目标已最新，或 `.PHONY` 漏写 |

## 编写 Makefile 的实用原则 {#best-practices}

1. **第一个目标是 `all`**，默认构建产物，不是 `clean`。
2. **产物进 `build/`**，`clean` 一条命令删干净。
3. **编译器、标志、目录名用变量**，便于 CI 与本地覆盖。
4. **声明 `.PHONY`**：`all`、`clean`、`test`、`install` 等非文件目标。
5. **配方里用 `$@` `$<` `$^`**，少写重复文件名。
6. **C 项目加 `-MMD -MP` 与 `-include *.d`**，头文件改动才可靠增量。
7. **配方命令失败应中止**：默认单行失败即停；多行逻辑用 `&&` 串联，或 `.ONESHELL:`（高级）。
8. **不要在一开始引入巨型通用框架**——先为当前仓库写能跑的 30 行，再随项目长大。

## 与 CMake、Ninja、Bazel 的关系 {#vs-cmake}

| 工具 | 适合场景 |
| --- | --- |
| **Makefile** | 小中型 C/C++、工具链简单、教学、快速原型 |
| **CMake** | 跨平台、多编译器、生成 Ninja/Makefiles |
| **Ninja** | 极大项目、极速增量（通常由 CMake 生成） |
| **Bazel** | 超大 monorepo、Hermetic 构建 |

学习 Makefile 的意义：**理解「目标-依赖-增量」模型**；以后读 CMake 生成的构建图、或在 CI 里改一两行编译步骤，都不会陌生。

## 从零开始：你的第一个 Makefile 检查清单 {#checklist}

新建 `Makefile` 时，按顺序自问：

- [ ] 默认目标是不是 `all`？
- [ ] 可执行文件、`.o` 是否路径清晰？
- [ ] `clean` 是否删尽产物？
- [ ] `test` / `run` 是否方便？
- [ ] 改一个 `.c` 是否只重编必要部分？
- [ ] 改 `.h` 是否会触发重编（若需要）？
- [ ] Tab 缩进是否正确？
- [ ] 队友是否只需记 `make` 和 `make test`？

## 小结 {#summary}

Makefile 并不神秘：**目标 = 要什么，依赖 = 靠什么，配方 = 怎么做。** `make` 负责根据时间戳做增量调度。

本文路径建议：

1. **实例一**：单文件，熟悉 Tab 与 `.PHONY`；
2. **实例二～三**：多文件 + `build/`，对齐日常 C 项目；
3. **实例四**：头文件依赖，增量构建才靠谱；
4. **实例五～七**：测试、子目录、子项目——接近本仓库 `examples/stack-vm` 与 `examples/svm-compiler` 的真实形态。

你可以从实例三的模板拷贝，把 `SRCS`、`TARGET` 改成自己的文件名，先让 `make` / `make clean` / `make test` 跑通，再逐步加 sanitizer、安装规则、代码生成等。构建系统的价值，不在于一次写得多炫，而在于**半年后你还能一键重现同一份二进制**。

---

**延伸阅读（本仓库）**

- [`examples/stack-vm/Makefile`](https://github.com/churen-biz/rw/blob/main/examples/stack-vm/Makefile) — 多测试目标、`sanitize`、`demo`
- [`examples/svm-compiler/Makefile`](https://github.com/churen-biz/rw/blob/main/examples/svm-compiler/Makefile) — 源文件列表、子 make、脚本化测试
