# SVM Compiler Plan 1: Foundation + Minimal `svm-as`

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up `examples/svm-compiler` and `content/book/svm-compiler`, then ship a minimal C11 `svm-as` that assembles `const_i32` / `i32_add` / `return` into a `.svm` file runnable by the existing `svm-run`.

**Architecture:** Reuse `examples/stack-vm/reference` for encode/verify/execute. This plan owns diagnostics, a tiny `ModuleBuilder`, an asm lexer/parser for one function, and the book spine (index, ch00, ch01, appendix A stub). Later plans add full asm, Mini, and IR.

**Tech Stack:** C11 (`-std=c11 -Wall -Wextra -Werror -pedantic`), existing SVM reference library, Hugo book pages under `content/book/svm-compiler/`, shell-driven golden tests.

**Spec:** `docs/superpowers/specs/2026-08-31-svm-compiler-series-design.md`

**Follow-up plans (not this file):**
- Plan 2: asm ch02–05 (locals, branches, full opcode surface including capability)
- Plan 3: Mini ch06–09 (parse, types, CFG/IR, integer/function lowering)
- Plan 4: Mini ch10–14 (objects, closures, exceptions, tasks, capability)
- Plan 5: ch15 polish + cross-links from `stack-vm/08` and the design blog

## Global Constraints

- Implementation language: C11 only for compiler tools in this series.
- Do not copy LEB128 / section encode logic; call `svm_module_write_file` from `../stack-vm/reference`.
- Do not fork or modify VM semantics in Plan 1 unless a compile-blocker bug is found (then fix in `stack-vm` with a separate commit message).
- Flags: `-std=c11 -Wall -Wextra -Werror -pedantic -g`.
- Every task ends with a failing-then-passing test cycle and a commit.
- `.sasm` v0 grammar for Plan 1 only:

```text
# comment
.func NAME -> i32
  const_i32 IMM
  i32_add
  return
.end
```

- Entry function is the first `.func`; name should be `main` in fixtures.
- Book pages: Chinese prose matching `stack-vm` tone; front matter mirrors that book.

---

## File structure (Plan 1)

```text
examples/svm-compiler/
  README.md
  Makefile
  include/
    sc_diag.h          # diagnostic + source span
    sc_arena.h         # bump allocator for parse trees / strings
    sc_builder.h       # ModuleBuilder → SvmModule views
    sc_asm.h           # assemble_file / assemble_string API
  src/
    common/diag.c
    common/arena.c
    emit/builder.c
    asm/lex.c          # internal; tokens
    asm/parse.c        # .sasm → builder
    asm/assemble.c     # public entry
  tools/svm-as.c
  tests/
    test-asm.c         # unit/integration tests
    fixtures/
      answer.sasm      # 40+2 → 42
      bad-opcode.sasm
  scripts/run-answer.sh

content/book/svm-compiler/
  _index.md
  00-overview.md
  01-minimal-assembler.md
  appendix-a-isa.md       # stub + pointer to SPEC; expand in Plan 2
```

---

### Task 1: Scaffold repo layout and Makefile

**Files:**
- Create: `examples/svm-compiler/README.md`
- Create: `examples/svm-compiler/Makefile`
- Create: `examples/svm-compiler/include/.gitkeep` (removed once headers land in Task 2)
- Create: `examples/svm-compiler/scripts/run-answer.sh` (placeholder echo until Task 5)

**Interfaces:**
- Consumes: `../stack-vm/reference/{svm,heap,module}.{c,h}`
- Produces: `make -C examples/svm-compiler` builds nothing useful yet except `build/` dir rule; `make test` may fail until Task 5 — for this task only verify `make stack-vm-deps` builds `../stack-vm/build/svm-run`.

- [ ] **Step 1: Write README**

```markdown
# svm-compiler

Teaching toolchain: text assembler (`svm-as`) and later Mini → `.svm`.

Requires the stack-vm reference library:

```bash
make -C ../stack-vm all
make test
```
```

- [ ] **Step 2: Write Makefile**

```makefile
CC ?= cc
STACK_VM := ../stack-vm
REF := $(STACK_VM)/reference
CPPFLAGS ?= -Iinclude -I$(REF)
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
BUILD_DIR := build
SVM_RUN := $(STACK_VM)/build/svm-run

COMMON_SRC := src/common/diag.c src/common/arena.c
EMIT_SRC := src/emit/builder.c
ASM_SRC := src/asm/lex.c src/asm/parse.c src/asm/assemble.c
LIB_SRC := $(COMMON_SRC) $(EMIT_SRC) $(ASM_SRC)
REF_SRC := $(REF)/svm.c $(REF)/heap.c $(REF)/module.c

.PHONY: all test clean stack-vm-deps demo

all: stack-vm-deps $(BUILD_DIR)/svm-as $(BUILD_DIR)/test-asm

stack-vm-deps:
	$(MAKE) -C $(STACK_VM) all

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/svm-as: tools/svm-as.c $(LIB_SRC) $(REF_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/svm-as.c $(LIB_SRC) $(REF_SRC) -o $@

$(BUILD_DIR)/test-asm: tests/test-asm.c $(LIB_SRC) $(REF_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test-asm.c $(LIB_SRC) $(REF_SRC) -o $@

test: all
	$(BUILD_DIR)/test-asm
	./scripts/run-answer.sh

demo: $(BUILD_DIR)/svm-as stack-vm-deps
	$(BUILD_DIR)/svm-as tests/fixtures/answer.sasm $(BUILD_DIR)/answer.svm
	$(SVM_RUN) $(BUILD_DIR)/answer.svm

clean:
	rm -rf $(BUILD_DIR)
```

- [ ] **Step 3: Create empty source stubs so `make all` can be completed in later tasks**

For now create stub `.c` files that compile:

`src/common/diag.c`, `src/common/arena.c`, `src/emit/builder.c`, `src/asm/lex.c`, `src/asm/parse.c`, `src/asm/assemble.c`, `tools/svm-as.c`, `tests/test-asm.c` each containing only:

```c
/* Plan 1 scaffold — filled in later tasks */
```

and `tools/svm-as.c` / `tests/test-asm.c`:

```c
int main(void) { return 0; }
```

- [ ] **Step 4: Verify stack-vm dependency builds**

Run: `make -C examples/svm-compiler stack-vm-deps`  
Expected: `../stack-vm/build/svm-run` exists.

- [ ] **Step 5: Commit**

```bash
git add examples/svm-compiler
git commit -m "scaffolding: add svm-compiler Makefile and empty sources"
```

---

### Task 2: Diagnostics and arena

**Files:**
- Create: `examples/svm-compiler/include/sc_diag.h`
- Create: `examples/svm-compiler/include/sc_arena.h`
- Modify: `examples/svm-compiler/src/common/diag.c`
- Modify: `examples/svm-compiler/src/common/arena.c`
- Modify: `examples/svm-compiler/tests/test-asm.c` (add arena/diag unit tests; keep `main` returning failures via asserts)

**Interfaces:**
- Produces:

```c
typedef struct {
    const char *path; /* may be NULL */
    uint32_t line;    /* 1-based */
    uint32_t column;  /* 1-based */
} ScSpan;

typedef struct {
    char message[256];
    ScSpan span;
} ScError;

void sc_error_set(ScError *err, ScSpan span, const char *fmt, ...);
void sc_error_print(const ScError *err, FILE *out);

typedef struct ScArena ScArena;
ScArena *sc_arena_create(void);
void sc_arena_destroy(ScArena *arena);
void *sc_arena_alloc(ScArena *arena, size_t size);
char *sc_arena_strndup(ScArena *arena, const char *s, size_t n);
```

- [ ] **Step 1: Write failing tests in `tests/test-asm.c`**

Replace `main` with:

```c
#include "sc_arena.h"
#include "sc_diag.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_arena_strdup(void) {
    ScArena *a = sc_arena_create();
    assert(a != NULL);
    char *s = sc_arena_strndup(a, "hello", 5);
    assert(s != NULL);
    assert(strcmp(s, "hello") == 0);
    sc_arena_destroy(a);
}

static void test_error_format(void) {
    ScError err;
    ScSpan span = {.path = "x.sasm", .line = 3, .column = 5};
    sc_error_set(&err, span, "unknown opcode '%s'", "foo");
    assert(strstr(err.message, "foo") != NULL);
    assert(err.span.line == 3);
}

int main(void) {
    test_arena_strdup();
    test_error_format();
    puts("ok diag/arena");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make -C examples/svm-compiler $(BUILD_DIR)/test-asm` will fail to compile (missing headers/symbols).  
Expected: compile error on `sc_arena_create` / missing includes.

- [ ] **Step 3: Implement headers and sources**

`include/sc_diag.h`:

```c
#ifndef SC_DIAG_H
#define SC_DIAG_H

#include <stdio.h>
#include <stdint.h>

typedef struct {
    const char *path;
    uint32_t line;
    uint32_t column;
} ScSpan;

typedef struct {
    char message[256];
    ScSpan span;
} ScError;

void sc_error_set(ScError *err, ScSpan span, const char *fmt, ...);
void sc_error_print(const ScError *err, FILE *out);

#endif
```

`include/sc_arena.h`:

```c
#ifndef SC_ARENA_H
#define SC_ARENA_H

#include <stddef.h>

typedef struct ScArena ScArena;

ScArena *sc_arena_create(void);
void sc_arena_destroy(ScArena *arena);
void *sc_arena_alloc(ScArena *arena, size_t size);
char *sc_arena_strndup(ScArena *arena, const char *s, size_t n);

#endif
```

`src/common/diag.c`: use `vsnprintf` into `err->message`; `sc_error_print` prints `path:line:column: message`.

`src/common/arena.c`: linked list of 4KiB chunks; `sc_arena_alloc` bump-pointer with 8-byte align; `sc_arena_strndup` alloc `n+1` and copy.

- [ ] **Step 4: Run tests**

Run: `make -C examples/svm-compiler build/test-asm && examples/svm-compiler/build/test-asm`  
Expected: `ok diag/arena`

- [ ] **Step 5: Commit**

```bash
git add examples/svm-compiler/include examples/svm-compiler/src/common examples/svm-compiler/tests/test-asm.c
git commit -m "feat(svm-compiler): add arena allocator and diagnostics"
```

---

### Task 3: ModuleBuilder for one i32 function

**Files:**
- Create: `examples/svm-compiler/include/sc_builder.h`
- Modify: `examples/svm-compiler/src/emit/builder.c`
- Modify: `examples/svm-compiler/tests/test-asm.c`

**Interfaces:**
- Consumes: `SvmModule`, `SvmFunction`, `SvmInstruction`, `svm_module_write_file`, `svm_execute` from reference
- Produces:

```c
typedef struct ScBuilder ScBuilder;

ScBuilder *sc_builder_create(ScArena *arena);
/* Begin a function; Plan 1 only supports result i32, 0 params, 0 locals. */
bool sc_builder_begin_func(
    ScBuilder *b, const char *name, ScError *err
);
bool sc_builder_emit(
    ScBuilder *b, SvmOpcode op, int32_t operand, int32_t operand2, ScError *err
);
bool sc_builder_end_func(ScBuilder *b, ScError *err);
/* Fill out_module with pointers owned by builder/arena (valid until destroy). */
bool sc_builder_finish(ScBuilder *b, SvmModule *out_module, ScError *err);
```

Plan 1 `max_stack`: track high-water while emitting (`const_i32` +1, `i32_add` -1, `return` 0). Reject if stack would go negative or exceed `SVM_MAX_STACK`.

- [ ] **Step 1: Write failing test `test_builder_answer`**

```c
#include "module.h"
#include "sc_arena.h"
#include "sc_builder.h"
#include "sc_diag.h"
#include "svm.h"

#include <assert.h>
#include <stdio.h>

static void test_builder_answer(void) {
    ScArena *arena = sc_arena_create();
    ScBuilder *b = sc_builder_create(arena);
    ScError err = {0};
    assert(sc_builder_begin_func(b, "main", &err));
    assert(sc_builder_emit(b, SVM_OP_CONST_I32, 40, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_CONST_I32, 2, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_I32_ADD, 0, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_RETURN, 0, 0, &err));
    assert(sc_builder_end_func(b, &err));
    SvmModule module;
    assert(sc_builder_finish(b, &module, &err));

    SvmLimits limits = {
        .instruction_budget = 1000,
        .call_depth_limit = SVM_MAX_FRAMES,
        .heap_byte_limit = 1024 * 1024,
        .handle_limit = 1024,
        .task_limit = SVM_MAX_TASKS,
        .channel_limit = SVM_MAX_CHANNELS,
        .scheduler_quantum = 100,
        .host_call_budget = 0
    };
    SvmValue result;
    assert(svm_execute(&module, 0, NULL, 0, limits, &result, &err));
    assert(result.type == SVM_TYPE_I32);
    assert(result.as.i32 == 42);
    sc_arena_destroy(arena);
    puts("ok builder");
}
```

Call it from `main` before returning.

- [ ] **Step 2: Run to see failure**

Expected: missing `sc_builder_*` symbols / header.

- [ ] **Step 3: Implement builder**

Keep instructions in a growable array inside the current function (realloc is OK in Plan 1; or arena-allocate with doubling). On `end_func`, append a completed `SvmFunction` to an internal array. `sc_builder_finish` sets:

```c
out_module->functions = ...;
out_module->function_count = ...;
out_module->record_types = NULL;
out_module->record_type_count = 0;
out_module->host_imports = NULL;
out_module->host_import_count = 0;
```

Set `local_count = 0`, `parameter_count = 0`, `capture_count = 0`, `handler_count = 0`, `handlers = NULL`, `result_type = SVM_TYPE_I32`.

- [ ] **Step 4: Run `build/test-asm`**

Expected: prints `ok builder` (and earlier ok lines).

- [ ] **Step 5: Commit**

```bash
git add examples/svm-compiler/include/sc_builder.h examples/svm-compiler/src/emit/builder.c examples/svm-compiler/tests/test-asm.c
git commit -m "feat(svm-compiler): ModuleBuilder emits runnable i32 main"
```

---

### Task 4: Asm lexer + parser for v0 `.sasm`

**Files:**
- Create: `examples/svm-compiler/include/sc_asm.h`
- Modify: `examples/svm-compiler/src/asm/lex.c`
- Modify: `examples/svm-compiler/src/asm/parse.c`
- Modify: `examples/svm-compiler/src/asm/assemble.c`
- Create: `examples/svm-compiler/tests/fixtures/answer.sasm`
- Create: `examples/svm-compiler/tests/fixtures/bad-opcode.sasm`
- Modify: `examples/svm-compiler/tests/test-asm.c`

**Interfaces:**
- Produces:

```c
bool sc_assemble_string(
    const char *path_for_diag,
    const char *source,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);

bool sc_assemble_file(
    const char *path,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);
```

Lexer tokens: `FUNC` (`.func`), `END` (`.end`), `ARROW` (`->`), `IDENT`, `INT`, `NEWLINE`/ignore whitespace, `COMMENT` (`#` to EOL), EOF.

Parser algorithm:
1. Expect `.func` IDENT `->` `i32` newline
2. Until `.end`: IDENT [INT]? as opcode line
3. Map mnemonics: `const_i32` → `SVM_OP_CONST_I32` (requires int operand), `i32_add` → `SVM_OP_I32_ADD`, `return` → `SVM_OP_RETURN`
4. Unknown mnemonic → `sc_error_set` and return false
5. Call builder APIs

- [ ] **Step 1: Add fixtures**

`tests/fixtures/answer.sasm`:

```text
# 40 + 2
.func main -> i32
  const_i32 40
  const_i32 2
  i32_add
  return
.end
```

`tests/fixtures/bad-opcode.sasm`:

```text
.func main -> i32
  not_an_opcode
  return
.end
```

- [ ] **Step 2: Write failing assemble tests**

```c
static void test_assemble_answer(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(sc_assemble_file("tests/fixtures/answer.sasm", arena, &module, &err));
    /* execute as in test_builder_answer; expect 42 */
    sc_arena_destroy(arena);
    puts("ok assemble answer");
}

static void test_assemble_bad_opcode(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(!sc_assemble_file(
        "tests/fixtures/bad-opcode.sasm", arena, &module, &err
    ));
    assert(strstr(err.message, "not_an_opcode") != NULL ||
           strstr(err.message, "unknown") != NULL);
    sc_arena_destroy(arena);
    puts("ok assemble reject");
}
```

Note: run tests with `cd examples/svm-compiler` so relative fixture paths work, or resolve paths relative to `argv[0]` / `SVM_COMPILER_ROOT`. Prefer documenting `make test` always runs from `examples/svm-compiler`.

- [ ] **Step 3: Implement lex/parse/assemble**

Keep lexer state: `const char *src`, `size_t pos`, current `line`/`column`.  
`sc_assemble_file`: read entire file into malloc buffer (or arena), call `sc_assemble_string`, free file buffer if not in arena.

- [ ] **Step 4: Run tests from package dir**

Run:

```bash
cd examples/svm-compiler && make test
```

Expected at this stage: `test-asm` passes; `scripts/run-answer.sh` may still be stub — update Makefile temporarily to only run `$(BUILD_DIR)/test-asm` until Task 5, **or** make `run-answer.sh` exit 0 with a message. Prefer: Task 4 Makefile `test` target only runs `test-asm`; Task 5 adds the script.

Update Makefile `test` recipe to:

```makefile
test: all
	$(BUILD_DIR)/test-asm
```

- [ ] **Step 5: Commit**

```bash
git add examples/svm-compiler
git commit -m "feat(svm-compiler): assemble v0 .sasm into SvmModule"
```

---

### Task 5: `svm-as` CLI + answer demo script

**Files:**
- Modify: `examples/svm-compiler/tools/svm-as.c`
- Modify: `examples/svm-compiler/scripts/run-answer.sh`
- Modify: `examples/svm-compiler/Makefile` (`test` includes script)

**Interfaces:**
- CLI: `svm-as INPUT.sasm OUTPUT.svm`
- Exit 0 on success; print diagnostics to stderr and exit 1 on failure
- Uses `sc_assemble_file` + `svm_module_write_file`

- [ ] **Step 1: Write `scripts/run-answer.sh`**

```bash
#!/bin/sh
set -e
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
AS="$ROOT/build/svm-as"
OUT="$ROOT/build/answer.svm"
SVM_RUN="$ROOT/../stack-vm/build/svm-run"
"$AS" "$ROOT/tests/fixtures/answer.sasm" "$OUT"
got=$("$SVM_RUN" "$OUT")
test "$got" = "i32:42"
echo "ok run-answer: $got"
```

- [ ] **Step 2: Implement `tools/svm-as.c`**

```c
#include "module.h"
#include "sc_arena.h"
#include "sc_asm.h"
#include "sc_diag.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s INPUT.sasm OUTPUT.svm\n", argv[0]);
        return EXIT_FAILURE;
    }
    ScArena *arena = sc_arena_create();
    if (!arena) {
        fputs("out of memory\n", stderr);
        return EXIT_FAILURE;
    }
    SvmModule module;
    ScError err = {0};
    if (!sc_assemble_file(argv[1], arena, &module, &err)) {
        sc_error_print(&err, stderr);
        sc_arena_destroy(arena);
        return EXIT_FAILURE;
    }
    if (!svm_module_write_file(&module, argv[2], &err)) {
        fprintf(stderr, "write error: %s\n", err.message);
        sc_arena_destroy(arena);
        return EXIT_FAILURE;
    }
    sc_arena_destroy(arena);
    return EXIT_SUCCESS;
}
```

Note: `SvmError` vs `ScError` — map write errors from `SvmError` with `fprintf` as above; assemble uses `ScError`.

- [ ] **Step 3: chmod + run full test**

```bash
chmod +x examples/svm-compiler/scripts/run-answer.sh
cd examples/svm-compiler && make test && make demo
```

Expected: `ok run-answer: i32:42` and demo prints `i32:42`.

- [ ] **Step 4: Restore Makefile `test` to run both test-asm and script**

- [ ] **Step 5: Commit**

```bash
git add examples/svm-compiler/tools/svm-as.c examples/svm-compiler/scripts examples/svm-compiler/Makefile
git commit -m "feat(svm-compiler): svm-as writes .svm and demo prints 42"
```

---

### Task 6: Book spine — `_index`, ch00, ch01, appendix A stub

**Files:**
- Create: `content/book/svm-compiler/_index.md`
- Create: `content/book/svm-compiler/00-overview.md`
- Create: `content/book/svm-compiler/01-minimal-assembler.md`
- Create: `content/book/svm-compiler/appendix-a-isa.md`
- Modify: `content/book/stack-vm/08-complete-vm.md` — add one paragraph + link under §2 pointing to this book (lowering examples live here); do not expand full vignettes yet (Plan 5).

**Interfaces:**
- Hugo book front matter pattern from `content/book/stack-vm/_index.md`
- weight: book `weight: 20` (after stack-vm's 10) or similar so sidebar orders VM then compiler

- [ ] **Step 1: Write `_index.md`**

Front matter:

```yaml
---
title: 从文本汇编到 Mini 编译器
linkTitle: SVM 编译器
description: 用 C11 实现 svm-as 与 Mini，把源程序编成可在教学栈式 VM 上运行的 .svm 模块。
type: book
weight: 20
book_kind: book
sidebar_root_for: self
sidebar_root_link_self: true
outputs: [HTML, print, markdown]
cascade:
  type: book
  book_draft_banner: true
  sidebar_headings: 3
---
```

Body: explain two spines (asm then Mini), require `examples/stack-vm` built, link to design spec path, `{{< book-toc depth=3 >}}`.

- [ ] **Step 2: Write `00-overview.md`**

Cover: goals/non-goals from spec §1, pipeline diagram, how to use appendix A, relationship to `stack-vm` (self-contained but overlapping). weight: 5.

- [ ] **Step 3: Write `01-minimal-assembler.md`**

Match what Plan 1 implemented: `.sasm` v0 grammar, stack effect of const/add/return, walk through `answer.sasm`, commands:

```bash
cd examples/svm-compiler
make test
make demo
```

Explain ModuleBuilder + reuse of `svm_module_write_file`. Point to source files under `src/asm` and `src/emit`. weight: 10.

- [ ] **Step 4: Write `appendix-a-isa.md` stub**

State Plan 1 only documents three opcodes + `SvmType` list pointer; full table filled in Plan 2. Link `examples/stack-vm/SPEC.md` and blog. weight: 90.

- [ ] **Step 5: Patch `stack-vm/08-complete-vm.md` §2**

After the existing two paragraphs, add:

```markdown
更完整的「源码 → 指令」路径见配套教程 [《从文本汇编到 Mini 编译器》](/book/svm-compiler/)：先手写 `.sasm`，再用 Mini 与 IR 生成同一套 opcode。
```

- [ ] **Step 6: Commit**

```bash
git add content/book/svm-compiler content/book/stack-vm/08-complete-vm.md
git commit -m "docs: add svm-compiler book spine and link from VM ch08"
```

---

### Task 7: Plan 1 verification gate

**Files:** none new

- [ ] **Step 1: Clean rebuild**

```bash
make -C examples/stack-vm clean all test
make -C examples/svm-compiler clean test demo
```

Expected: all tests pass; demo prints `i32:42`.

- [ ] **Step 2: Sanity-check book files exist**

```bash
test -f content/book/svm-compiler/_index.md
test -f content/book/svm-compiler/01-minimal-assembler.md
```

- [ ] **Step 3: Final commit only if Step 1 left dirty fixes; otherwise done**

If fixes were needed:

```bash
git add -A examples/svm-compiler content/book/svm-compiler
git commit -m "fix: Plan 1 verification clean build"
```

---

## Spec coverage (Plan 1 slice)

| Spec item | Task |
| --- | --- |
| `examples/svm-compiler` layout | 1 |
| Reuse stack-vm reference encode | 3, 5 |
| Stage A starts with minimal asm | 4–5 |
| Book `content/book/svm-compiler` | 6 |
| Ch00 overview + appendix A | 6 |
| Ch01 minimal assembler | 6 |
| Link from stack-vm 08 lowering | 6 |
| Full opcode asm / Mini / IR / capability | **Deferred to Plans 2–5** |

## Self-review notes

- No TBD steps; `.sasm` v0 grammar pinned in Global Constraints.
- `ScError` vs `SvmError` kept distinct at CLI boundary (Task 5).
- Makefile fixture cwd documented (`cd examples/svm-compiler`).
- Builder stack high-water required so verifier accepts `max_stack`.
