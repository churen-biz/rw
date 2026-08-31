#include "sc_arena.h"
#include "sc_diag.h"
#include "sc_mini.h"
#include "svm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static SvmLimits limits(void) {
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

static void expect_i32(const char *path, uint32_t entry, int32_t want) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(sc_mini_compile_file(path, arena, &module, &err));
    SvmValue result;
    SvmError runtime = {0};
    assert(svm_execute(&module, entry, NULL, 0, limits(), &result, &runtime));
    assert(result.type == SVM_TYPE_I32);
    assert(result.as.i32 == want);
    sc_arena_destroy(arena);
}

static void expect_type_error(const char *source) {
    ScArena *arena = sc_arena_create();
    SvmModule module;
    ScError err = {0};
    assert(!sc_mini_compile_string("bad.mini", source, arena, &module, &err));
    sc_arena_destroy(arena);
}

int main(void) {
    expect_i32("tests/fixtures/answer.mini", 1, 42);
    expect_i32("tests/fixtures/loop_sum.mini", 0, 15);
    expect_type_error(
        "module m\nfn main() -> i32 {\n  return true;\n}\n"
    );
    puts("ok mini");
    return 0;
}
