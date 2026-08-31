# SVM Compiler Plan 2: Locals, Labels, Branches, Calls

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `svm-as` so multi-function `.sasm` with parameters, locals, labels, jumps, and `call` can express recursive factorial and a simple loop; document as book chapter 02.

**Architecture:** Register each function’s name and signature at `.func` start so `call` can resolve index and stack effect immediately. Labels and jump operands are backpatched at `.end`. Keep reusing `svm_module_write_file` / `svm-run`.

**Tech Stack:** C11, existing `examples/svm-compiler` + `stack-vm/reference`.

**Spec:** `docs/superpowers/specs/2026-08-31-svm-compiler-series-design.md` (stage A ch02)

**Out of scope (later plans):** objects/arrays (ch04), exceptions/tasks/capability (ch05), Mini (ch06+).

## Global Constraints

- C11, `-Wall -Wextra -Werror -pedantic`; do not fork encode/verify.
- Jump operands are **absolute instruction indices** (same as SVM reference).
- Parameters occupy locals `[0 .. param_count)`; `local_count = max(param_count, max_local_index + 1)`.
- Result types in Plan 2: `i32` and `bool` only.
- Work on `main` (user consented). Tests run from `examples/svm-compiler`.

### `.sasm` v1 grammar (additive)

```text
.func NAME ( IDENT : TYPE (, IDENT : TYPE)* )? -> TYPE
  # body lines: opcode | LABEL:
.end
# repeat .func ... .end
```

New tokens: `( ) , :`

New mnemonics: `const_true` `const_false` `load_local` `store_local` `dup` `pop` `i32_sub` `i32_mul` `i32_le` `i32_eq` `jump` `jump_if_false` `call`

---

### Task 1: Lexer tokens for signatures and labels

**Files:**
- Modify: `examples/svm-compiler/src/asm/lex.h`
- Modify: `examples/svm-compiler/src/asm/lex.c`
- Modify: `examples/svm-compiler/tests/test-asm.c`

**Interfaces:**
- Add `SC_TOK_LPAREN`, `SC_TOK_RPAREN`, `SC_TOK_COMMA`, `SC_TOK_COLON`

- [ ] **Step 1:** Add a small lexer unit test that tokenizes `.func f(n: i32) -> i32` and `loop:` into the expected kinds (can drive via assembling a stub later; minimal: expose nothing new publicly — cover via assemble fixtures in Task 4). Prefer integrating coverage in Task 4 fixtures; for this task only extend lexer and compile.

- [ ] **Step 2:** Implement single-char tokens `(` `)` `,` `:` in `sc_lexer_next`.

- [ ] **Step 3:** `make -C examples/svm-compiler build/test-asm && ./build/test-asm` still passes Plan 1 tests.

- [ ] **Step 4:** Commit `feat(svm-compiler): lex punctuation for func signatures and labels`

---

### Task 2: Builder — signatures, labels, jump fixups, local_count

**Files:**
- Modify: `examples/svm-compiler/include/sc_builder.h`
- Modify: `examples/svm-compiler/src/emit/builder.c`
- Modify: `examples/svm-compiler/tests/test-asm.c`

**Interfaces:**

```c
bool sc_builder_begin_func(
    ScBuilder *b,
    const char *name,
    const SvmType *param_types, /* copied into arena; may be NULL if count 0 */
    uint32_t param_count,
    SvmType result_type,
    ScError *err
);

bool sc_builder_define_label(ScBuilder *b, const char *name, ScError *err);

/* Emit jump/jump_if_false with label name; operand patched at end_func. */
bool sc_builder_emit_jump(
    ScBuilder *b, SvmOpcode op /* JUMP or JUMP_IF_FALSE */,
    const char *label, ScError *err
);

/* Resolve callee by name; sets operand to function index; stack delta = 1 - param_count. */
bool sc_builder_emit_call(ScBuilder *b, const char *func_name, ScError *err);

/* Existing sc_builder_emit for non-label ops; load/store update max local index. */
```

Change `sc_builder_begin_func` signature — update all call sites (parser + tests).

On `begin_func`: append draft slot with name/signature so later `emit_call` can find index = slot position.

On `end_func`: patch jump fixups; set `local_count`; copy `parameter_types` into finished `SvmFunction`.

- [ ] **Step 1:** Write `test_builder_factorial` that builds the same instruction stream as `factorial_module` in stack-vm tests via builder APIs (including labels), executes with arg 5, expects 120.

- [ ] **Step 2:** Run test — expect compile/link failure on new APIs.

- [ ] **Step 3:** Implement builder changes; keep Plan 1 `test_builder_answer` working with `param_count=0`, `result=i32`.

- [ ] **Step 4:** `make test` (asm unit tests only if script unchanged).

- [ ] **Step 5:** Commit `feat(svm-compiler): builder labels, calls, and typed funcs`

---

### Task 3: Parser for multi-func `.sasm` v1

**Files:**
- Modify: `examples/svm-compiler/src/asm/parse.c`
- Create: `examples/svm-compiler/tests/fixtures/factorial.sasm`
- Create: `examples/svm-compiler/tests/fixtures/loop_sum.sasm`
- Create: `examples/svm-compiler/tests/fixtures/bad-label.sasm`
- Modify: `examples/svm-compiler/tests/test-asm.c`
- Modify: `examples/svm-compiler/Makefile` / scripts as needed

**factorial.sasm** (entry index 1 = main, or put main first — prefer **main as function 0** for `svm-run` default):

```text
.func main -> i32
  const_i32 5
  call factorial
  return
.end

.func factorial(n: i32) -> i32
  load_local 0
  const_i32 1
  i32_le
  jump_if_false body
  const_i32 1
  return
body:
  load_local 0
  load_local 0
  const_i32 1
  i32_sub
  call factorial
  i32_mul
  return
.end
```

Note: `call factorial` from main runs before factorial’s `.end`, but signature is registered at factorial’s `.func` — **problem**: main is parsed first, so factorial is unknown at `call factorial`.

**Fix:** Either (a) require callee defined before use, and put factorial first + `svm-run` entry index 1; or (b) two-pass / deferred call resolution.

**Chosen:** (a) for Plan 2 simplicity — document that callees must appear before callers; demo uses:

```bash
svm-run build/factorial.svm 1   # entry = main at index 1
```

factorial.sasm order: factorial first, then main. Update `run-factorial.sh` accordingly.

- [ ] **Step 1:** Add fixtures + tests: assemble factorial, `svm_execute(..., entry=1, ...)`, expect 120; loop_sum expect known value; bad-label expect error containing `label`.

- [ ] **Step 2:** Implement parser loop over multiple `.func`, signature parsing, label lines (`IDENT` + `COLON`), mnemonic table.

- [ ] **Step 3:** `make test` green.

- [ ] **Step 4:** Commit `feat(svm-compiler): parse multi-func asm with jumps and calls`

---

### Task 4: CLI demo script + book chapter 02 + appendix A update

**Files:**
- Create: `examples/svm-compiler/scripts/run-factorial.sh`
- Modify: `examples/svm-compiler/Makefile` (`test` runs factorial script; add `demo-factorial` optional)
- Create: `content/book/svm-compiler/02-funcs-locals-jumps.md`
- Modify: `content/book/svm-compiler/appendix-a-isa.md` (list new mnemonics)
- Modify: `content/book/svm-compiler/01-minimal-assembler.md` (one line “next: ch02”)

- [ ] **Step 1:** Script assembles factorial.sasm, runs `svm-run OUT 1`, asserts `i32:120`.

- [ ] **Step 2:** Write chapter 02 in same tone as ch01: grammar, label backpatch, define-before-call, commands.

- [ ] **Step 3:** Expand appendix A tables for Plan 2 opcodes.

- [ ] **Step 4:** `make -C examples/svm-compiler clean test` and commit `docs+feat(svm-compiler): chapter 02 factorial path`

---

### Task 5: Verification gate

- [ ] **Step 1:** `make -C examples/stack-vm test && make -C examples/svm-compiler clean test`

- [ ] **Step 2:** Confirm book file exists.

---

## Spec coverage

| Spec ch02 item | Task |
| --- | --- |
| Functions, labels, locals, branches | 2–3 |
| Factorial/loop module | 3–4 |
| Book chapter 02 | 4 |
| Full opcode / objects / capability | Deferred |
