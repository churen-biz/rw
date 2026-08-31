#include "module.h"
#include "sc_arena.h"
#include "sc_asm.h"
#include "sc_builder.h"
#include "sc_diag.h"
#include "svm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static SvmLimits test_limits(void) {
    return (SvmLimits){
        .instruction_budget = 100000,
        .call_depth_limit = SVM_MAX_FRAMES,
        .heap_byte_limit = 1024 * 1024,
        .handle_limit = 1024,
        .task_limit = SVM_MAX_TASKS,
        .channel_limit = SVM_MAX_CHANNELS,
        .scheduler_quantum = 100,
        .host_call_budget = 0
    };
}

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

static void assert_i32(
    const SvmModule *module, uint32_t entry, const SvmValue *args,
    uint32_t argc, int32_t expected
) {
    SvmValue result;
    SvmError runtime = {0};
    assert(svm_execute(
        module, entry, args, argc, test_limits(), &result, &runtime
    ));
    assert(result.type == SVM_TYPE_I32);
    assert(result.as.i32 == expected);
}

static void test_builder_answer(void) {
    ScArena *arena = sc_arena_create();
    ScBuilder *b = sc_builder_create(arena);
    ScError err = {0};
    assert(sc_builder_begin_func(b, "main", NULL, 0, SVM_TYPE_I32, &err));
    assert(sc_builder_emit(b, SVM_OP_CONST_I32, 40, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_CONST_I32, 2, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_I32_ADD, 0, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_RETURN, 0, 0, &err));
    assert(sc_builder_end_func(b, &err));
    SvmModule module;
    assert(sc_builder_finish(b, &module, &err));
    assert_i32(&module, 0, NULL, 0, 42);
    sc_arena_destroy(arena);
    puts("ok builder");
}

static void test_builder_factorial(void) {
    ScArena *arena = sc_arena_create();
    ScBuilder *b = sc_builder_create(arena);
    ScError err = {0};
    SvmType n = SVM_TYPE_I32;
    assert(sc_builder_begin_func(b, "factorial", &n, 1, SVM_TYPE_I32, &err));
    assert(sc_builder_emit(b, SVM_OP_LOAD_LOCAL, 0, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_CONST_I32, 1, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_I32_LE, 0, 0, &err));
    assert(sc_builder_emit_jump(b, SVM_OP_JUMP_IF_FALSE, "body", &err));
    assert(sc_builder_emit(b, SVM_OP_CONST_I32, 1, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_RETURN, 0, 0, &err));
    assert(sc_builder_define_label(b, "body", &err));
    assert(sc_builder_emit(b, SVM_OP_LOAD_LOCAL, 0, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_LOAD_LOCAL, 0, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_CONST_I32, 1, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_I32_SUB, 0, 0, &err));
    assert(sc_builder_emit_call(b, "factorial", &err));
    assert(sc_builder_emit(b, SVM_OP_I32_MUL, 0, 0, &err));
    assert(sc_builder_emit(b, SVM_OP_RETURN, 0, 0, &err));
    assert(sc_builder_end_func(b, &err));

    assert(sc_builder_begin_func(b, "main", NULL, 0, SVM_TYPE_I32, &err));
    assert(sc_builder_emit(b, SVM_OP_CONST_I32, 5, 0, &err));
    assert(sc_builder_emit_call(b, "factorial", &err));
    assert(sc_builder_emit(b, SVM_OP_RETURN, 0, 0, &err));
    assert(sc_builder_end_func(b, &err));

    SvmModule module;
    assert(sc_builder_finish(b, &module, &err));
    assert_i32(&module, 1, NULL, 0, 120);
    sc_arena_destroy(arena);
    puts("ok builder factorial");
}

static void test_assemble_answer(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(sc_assemble_file("tests/fixtures/answer.sasm", arena, &module, &err));
    assert_i32(&module, 0, NULL, 0, 42);
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
    assert(
        strstr(err.message, "not_an_opcode") != NULL ||
        strstr(err.message, "unknown") != NULL
    );
    sc_arena_destroy(arena);
    puts("ok assemble reject");
}

static void test_assemble_factorial(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(sc_assemble_file(
        "tests/fixtures/factorial.sasm", arena, &module, &err
    ));
    assert_i32(&module, 1, NULL, 0, 120);
    sc_arena_destroy(arena);
    puts("ok assemble factorial");
}

static void test_assemble_loop_sum(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(sc_assemble_file(
        "tests/fixtures/loop_sum.sasm", arena, &module, &err
    ));
    assert_i32(&module, 0, NULL, 0, 15);
    sc_arena_destroy(arena);
    puts("ok assemble loop");
}

static void test_assemble_bad_label(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(!sc_assemble_file(
        "tests/fixtures/bad-label.sasm", arena, &module, &err
    ));
    assert(strstr(err.message, "label") != NULL);
    sc_arena_destroy(arena);
    puts("ok assemble bad label");
}

int main(void) {
    test_arena_strdup();
    test_error_format();
    test_builder_answer();
    test_builder_factorial();
    test_assemble_answer();
    test_assemble_bad_opcode();
    test_assemble_factorial();
    test_assemble_loop_sum();
    test_assemble_bad_label();
    puts("ok all");
    return 0;
}
