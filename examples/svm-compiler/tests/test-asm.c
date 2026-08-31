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
        .scheduler_quantum = 1,
        .host_call_budget = 100
    };
}

static void assert_i32(
    const SvmModule *module, uint32_t entry, int32_t expected
) {
    SvmValue result;
    SvmError runtime = {0};
    assert(svm_execute(
        module, entry, NULL, 0, test_limits(), &result, &runtime
    ));
    assert(result.type == SVM_TYPE_I32);
    assert(result.as.i32 == expected);
}

static void assemble_ok(const char *path, SvmModule *module, ScArena *arena) {
    ScError err = {0};
    assert(sc_assemble_file(path, arena, module, &err));
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
    assert_i32(&module, 0, 42);
    sc_arena_destroy(arena);
    puts("ok builder");
}

static void test_assemble_answer(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/answer.sasm", &module, arena);
    assert_i32(&module, 0, 42);
    sc_arena_destroy(arena);
    puts("ok assemble answer");
}

static void test_assemble_factorial(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/factorial.sasm", &module, arena);
    assert_i32(&module, 1, 120);
    sc_arena_destroy(arena);
    puts("ok assemble factorial");
}

static void test_assemble_loop_sum(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/loop_sum.sasm", &module, arena);
    assert_i32(&module, 0, 15);
    sc_arena_destroy(arena);
    puts("ok assemble loop");
}

static void test_assemble_object(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/object_box.sasm", &module, arena);
    assert_i32(&module, 0, 42);
    sc_arena_destroy(arena);
    puts("ok assemble object");
}

static void test_assemble_array(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/array_get.sasm", &module, arena);
    assert_i32(&module, 0, 7);
    sc_arena_destroy(arena);
    puts("ok assemble array");
}

static void test_assemble_catch(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/catch_throw.sasm", &module, arena);
    assert_i32(&module, 1, 42);
    sc_arena_destroy(arena);
    puts("ok assemble catch");
}

static void test_assemble_channel(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/channel_ping.sasm", &module, arena);
    assert_i32(&module, 1, 42);
    sc_arena_destroy(arena);
    puts("ok assemble channel");
}

static SvmHostStatus host_add(
    void *context,
    const SvmValue *arguments,
    uint32_t argument_count,
    SvmValue *value,
    uint64_t *pending_token
) {
    (void)context;
    (void)pending_token;
    assert(argument_count == 2);
    *value = svm_i32(arguments[0].as.i32 + arguments[1].as.i32);
    return SVM_HOST_DONE;
}

static void test_assemble_host(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/host_add.sasm", &module, arena);
    static const SvmType parameters[] = {SVM_TYPE_I32, SVM_TYPE_I32};
    static const SvmHostFunction host_functions[] = {{
        "add", parameters, 2, SVM_TYPE_I32, 1, host_add, NULL
    }};
    SvmCapability capability = {"math", NULL, host_functions, 1};
    SvmValue result;
    SvmError runtime = {0};
    assert(svm_execute_with_capabilities(
        &module, 0, NULL, 0, test_limits(), &capability, 1, &result, &runtime
    ));
    assert(result.type == SVM_TYPE_I32 && result.as.i32 == 5);
    sc_arena_destroy(arena);
    puts("ok assemble host");
}

static void test_verify_rejects_bad_types(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    assemble_ok("tests/fixtures/bad-types.sasm", &module, arena);
    SvmError err = {0};
    assert(!svm_verify_module(&module, &err));
    assert(strstr(err.message, "expected stack type") != NULL);
    sc_arena_destroy(arena);
    puts("ok verify reject");
}

static void test_assemble_bad_opcode(void) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(!sc_assemble_file(
        "tests/fixtures/bad-opcode.sasm", arena, &module, &err
    ));
    sc_arena_destroy(arena);
    puts("ok assemble reject");
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
    test_builder_answer();
    test_assemble_answer();
    test_assemble_factorial();
    test_assemble_loop_sum();
    test_assemble_object();
    test_assemble_array();
    test_assemble_catch();
    test_assemble_channel();
    test_assemble_host();
    test_verify_rejects_bad_types();
    test_assemble_bad_opcode();
    test_assemble_bad_label();
    puts("ok all");
    return 0;
}
