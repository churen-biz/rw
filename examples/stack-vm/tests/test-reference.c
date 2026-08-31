#include "svm.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(values) ((uint32_t)(sizeof(values) / sizeof((values)[0])))
#define INS(opcode, operand) {opcode, operand, 0}

static int failures;

typedef struct {
    uint32_t invoke_count;
    uint32_t poll_count;
} HostTestContext;

static SvmHostStatus host_add(
    void *context,
    const SvmValue *arguments,
    uint32_t argument_count,
    SvmValue *value,
    uint64_t *pending_token
) {
    HostTestContext *state = context;
    state->invoke_count++;
    (void)pending_token;
    if (argument_count != 2) return SVM_HOST_FAILED;
    *value = svm_i32(arguments[0].as.i32 + arguments[1].as.i32);
    return SVM_HOST_DONE;
}

static SvmHostStatus host_increment_later(
    void *context,
    const SvmValue *arguments,
    uint32_t argument_count,
    SvmValue *value,
    uint64_t *pending_token
) {
    HostTestContext *state = context;
    state->invoke_count++;
    (void)value;
    if (argument_count != 1) return SVM_HOST_FAILED;
    *pending_token = (uint64_t)(uint32_t)(arguments[0].as.i32 + 1);
    return SVM_HOST_PENDING;
}

static SvmHostStatus host_poll_increment(
    void *context,
    uint64_t pending_token,
    SvmValue *value
) {
    HostTestContext *state = context;
    state->poll_count++;
    if (state->poll_count < 2) return SVM_HOST_PENDING;
    *value = svm_i32((int32_t)pending_token);
    return SVM_HOST_DONE;
}

static void fail(const char *test, const char *message) {
    fprintf(stderr, "FAIL %-28s %s\n", test, message);
    failures++;
}

static SvmModule factorial_module(void) {
    static const SvmType parameters[] = {SVM_TYPE_I32};
    static const SvmInstruction code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0),
        INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_I32_LE, 0),
        INS(SVM_OP_JUMP_IF_FALSE, 6),
        INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_RETURN, 0),
        INS(SVM_OP_LOAD_LOCAL, 0),
        INS(SVM_OP_LOAD_LOCAL, 0),
        INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_I32_SUB, 0),
        INS(SVM_OP_CALL, 0),
        INS(SVM_OP_I32_MUL, 0),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "factorial",
        .parameter_types = parameters,
        .parameter_count = ARRAY_COUNT(parameters),
        .result_type = SVM_TYPE_I32,
        .local_count = 1,
        .max_stack = 3,
        .code = code,
        .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {
        .functions = functions,
        .function_count = ARRAY_COUNT(functions)
    };
    return module;
}

static void test_factorial(void) {
    const char *name = "recursive factorial";
    SvmModule module = factorial_module();
    SvmValue argument = svm_i32(5);
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 1000, .call_depth_limit = 32};

    if (!svm_execute(&module, 0, &argument, 1, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 120) {
        fail(name, "expected i32 value 120");
    }
}

static void test_rejects_type_error(void) {
    const char *name = "verifier rejects bad types";
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_TRUE, 0),
        INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_I32_ADD, 0),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "bad_types",
        .result_type = SVM_TYPE_I32,
        .local_count = 0,
        .max_stack = 2,
        .code = code,
        .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = ARRAY_COUNT(functions)};
    SvmError error;
    if (svm_verify_module(&module, &error)) {
        fail(name, "invalid module was accepted");
    } else if (strstr(error.message, "expected stack type") == NULL) {
        fail(name, "unexpected verifier error");
    }
}

static void test_rejects_bad_jump(void) {
    const char *name = "verifier rejects bad jump";
    static const SvmInstruction code[] = {INS(SVM_OP_JUMP, 99)};
    static const SvmFunction functions[] = {{
        .name = "bad_jump",
        .result_type = SVM_TYPE_I32,
        .max_stack = 1,
        .code = code,
        .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = ARRAY_COUNT(functions)};
    SvmError error;
    if (svm_verify_module(&module, &error)) {
        fail(name, "invalid jump was accepted");
    } else if (strstr(error.message, "jump target") == NULL) {
        fail(name, "unexpected verifier error");
    }
}

static void test_instruction_budget(void) {
    const char *name = "instruction budget";
    SvmModule module = factorial_module();
    SvmValue argument = svm_i32(5);
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 4, .call_depth_limit = 32};
    if (svm_execute(&module, 0, &argument, 1, limits, &result, &error)) {
        fail(name, "execution should have exhausted its budget");
    } else if (strstr(error.message, "budget") == NULL) {
        fail(name, "unexpected budget error");
    }
}

static void test_call_depth_limit(void) {
    const char *name = "call depth limit";
    SvmModule module = factorial_module();
    SvmValue argument = svm_i32(5);
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 1000, .call_depth_limit = 3};
    if (svm_execute(&module, 0, &argument, 1, limits, &result, &error)) {
        fail(name, "execution should have exceeded call depth");
    } else if (strstr(error.message, "depth") == NULL) {
        fail(name, "unexpected call depth error");
    }
}

static void test_object_survives_gc(void) {
    const char *name = "object survives VM GC";
    static const SvmType fields[] = {SVM_TYPE_I32};
    static const SvmRecordType records[] = {{"Box", fields, ARRAY_COUNT(fields)}};
    static const SvmInstruction code[] = {
        INS(SVM_OP_NEW_OBJECT, 0),
        INS(SVM_OP_DUP, 0),
        INS(SVM_OP_CONST_I32, 42),
        {SVM_OP_SET_FIELD, 0, 0},
        INS(SVM_OP_GC_COLLECT, 0),
        {SVM_OP_GET_FIELD, 0, 0},
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "object_gc",
        .result_type = SVM_TYPE_I32,
        .local_count = 0,
        .max_stack = 3,
        .code = code,
        .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {
        .functions = functions,
        .function_count = ARRAY_COUNT(functions),
        .record_types = records,
        .record_type_count = ARRAY_COUNT(records)
    };
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100, .heap_byte_limit = 4096,
                        .handle_limit = 16};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 42) {
        fail(name, "expected field value 42");
    }
}

static void test_array_program(void) {
    const char *name = "array bytecode";
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, 3),
        INS(SVM_OP_NEW_ARRAY, SVM_TYPE_I32),
        INS(SVM_OP_DUP, 0),
        INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_CONST_I32, 7),
        INS(SVM_OP_ARRAY_SET, SVM_TYPE_I32),
        INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_ARRAY_GET, SVM_TYPE_I32),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "array_program", .result_type = SVM_TYPE_I32,
        .max_stack = 4, .code = code, .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.as.i32 != 7) {
        fail(name, "expected array value 7");
    }
}

static void test_closure_program(void) {
    const char *name = "closure bytecode";
    static const SvmType closure_parameters[] = {SVM_TYPE_I32, SVM_TYPE_I32};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_CONST_I32, 10),
        {SVM_OP_NEW_CLOSURE, 1, 1},
        INS(SVM_OP_CONST_I32, 5),
        INS(SVM_OP_CALL_CLOSURE, 1),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction closure_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0),
        INS(SVM_OP_LOAD_LOCAL, 1),
        INS(SVM_OP_I32_ADD, 0),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "closure_entry", .result_type = SVM_TYPE_I32,
         .max_stack = 2, .code = entry_code, .code_count = ARRAY_COUNT(entry_code)},
        {.name = "add_capture", .parameter_types = closure_parameters,
         .parameter_count = ARRAY_COUNT(closure_parameters), .capture_count = 1,
         .result_type = SVM_TYPE_I32, .local_count = 2, .max_stack = 2,
         .code = closure_code, .code_count = ARRAY_COUNT(closure_code)}
    };
    SvmModule module = {.functions = functions, .function_count = ARRAY_COUNT(functions)};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.as.i32 != 15) {
        fail(name, "expected closure result 15");
    }
}

static void test_runtime_rejects_wrong_record(void) {
    const char *name = "runtime record type check";
    static const SvmType fields[] = {SVM_TYPE_I32};
    static const SvmRecordType records[] = {
        {"A", fields, 1}, {"B", fields, 1}
    };
    static const SvmInstruction code[] = {
        INS(SVM_OP_NEW_OBJECT, 0),
        {SVM_OP_GET_FIELD, 1, 0},
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "wrong_record", .result_type = SVM_TYPE_I32,
        .max_stack = 1, .code = code, .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = 1,
                        .record_types = records, .record_type_count = 2};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100};
    if (svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, "wrong record access was accepted");
    } else if (strstr(error.message, "wrong instance type") == NULL) {
        fail(name, "unexpected record type error");
    }
}

static void test_cross_frame_exception(void) {
    const char *name = "cross-frame exception";
    static const SvmRecordType records[] = {{"Error", NULL, 0}};
    static const SvmExceptionHandler handlers[] = {{0, 1, 2}};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_CALL, 1),
        INS(SVM_OP_RETURN, 0),
        INS(SVM_OP_POP, 0),
        INS(SVM_OP_CONST_I32, 42),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction thrower_code[] = {
        INS(SVM_OP_NEW_OBJECT, 0),
        INS(SVM_OP_THROW, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "catcher", .result_type = SVM_TYPE_I32, .max_stack = 1,
         .code = entry_code, .code_count = ARRAY_COUNT(entry_code),
         .handlers = handlers, .handler_count = ARRAY_COUNT(handlers)},
        {.name = "thrower", .result_type = SVM_TYPE_I32, .max_stack = 1,
         .code = thrower_code, .code_count = ARRAY_COUNT(thrower_code)}
    };
    SvmModule module = {.functions = functions, .function_count = 2,
                        .record_types = records, .record_type_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 42) {
        fail(name, "handler did not return 42");
    }
}

static void test_defer_on_return(void) {
    const char *name = "defer runs once on return";
    static const SvmType counter_fields[] = {SVM_TYPE_I32};
    static const SvmRecordType records[] = {{"Counter", counter_fields, 1}};
    static const SvmType ref_parameter[] = {SVM_TYPE_REF};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_NEW_OBJECT, 0),
        INS(SVM_OP_STORE_LOCAL, 0),
        INS(SVM_OP_LOAD_LOCAL, 0),
        INS(SVM_OP_CALL, 1),
        INS(SVM_OP_POP, 0),
        INS(SVM_OP_LOAD_LOCAL, 0),
        {SVM_OP_GET_FIELD, 0, 0},
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction worker_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0),
        {SVM_OP_NEW_CLOSURE, 2, 1},
        INS(SVM_OP_DEFER_PUSH, 2),
        INS(SVM_OP_CONST_I32, 7),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction cleanup_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0),
        INS(SVM_OP_DUP, 0),
        {SVM_OP_GET_FIELD, 0, 0},
        INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_I32_ADD, 0),
        {SVM_OP_SET_FIELD, 0, 0},
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "entry", .result_type = SVM_TYPE_I32, .local_count = 1,
         .max_stack = 2, .code = entry_code, .code_count = ARRAY_COUNT(entry_code)},
        {.name = "worker", .parameter_types = ref_parameter, .parameter_count = 1,
         .result_type = SVM_TYPE_I32, .local_count = 1, .max_stack = 1,
         .code = worker_code, .code_count = ARRAY_COUNT(worker_code)},
        {.name = "increment", .parameter_types = ref_parameter, .parameter_count = 1,
         .capture_count = 1, .result_type = SVM_TYPE_VOID, .local_count = 1,
         .max_stack = 3, .code = cleanup_code, .code_count = ARRAY_COUNT(cleanup_code)}
    };
    SvmModule module = {.functions = functions, .function_count = 3,
                        .record_types = records, .record_type_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 200};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 1) {
        fail(name, "cleanup did not run exactly once");
    }
}

static void test_defer_on_throw(void) {
    const char *name = "defer runs before catch";
    static const SvmType counter_fields[] = {SVM_TYPE_I32};
    static const SvmRecordType records[] = {
        {"Counter", counter_fields, 1}, {"Error", NULL, 0}
    };
    static const SvmType ref_parameter[] = {SVM_TYPE_REF};
    static const SvmExceptionHandler handlers[] = {{3, 4, 7}};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_NEW_OBJECT, 0),
        INS(SVM_OP_STORE_LOCAL, 0),
        INS(SVM_OP_LOAD_LOCAL, 0),
        INS(SVM_OP_CALL, 1),
        INS(SVM_OP_LOAD_LOCAL, 0),
        {SVM_OP_GET_FIELD, 0, 0},
        INS(SVM_OP_RETURN, 0),
        INS(SVM_OP_POP, 0),
        INS(SVM_OP_LOAD_LOCAL, 0),
        {SVM_OP_GET_FIELD, 0, 0},
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction worker_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0),
        {SVM_OP_NEW_CLOSURE, 2, 1},
        INS(SVM_OP_DEFER_PUSH, 2),
        INS(SVM_OP_NEW_OBJECT, 1),
        INS(SVM_OP_THROW, 0)
    };
    static const SvmInstruction cleanup_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0),
        INS(SVM_OP_DUP, 0),
        {SVM_OP_GET_FIELD, 0, 0},
        INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_I32_ADD, 0),
        {SVM_OP_SET_FIELD, 0, 0},
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "catch_after_defer", .result_type = SVM_TYPE_I32,
         .local_count = 1, .max_stack = 2, .code = entry_code,
         .code_count = ARRAY_COUNT(entry_code), .handlers = handlers,
         .handler_count = ARRAY_COUNT(handlers)},
        {.name = "throwing_worker", .parameter_types = ref_parameter,
         .parameter_count = 1, .result_type = SVM_TYPE_VOID, .local_count = 1,
         .max_stack = 1, .code = worker_code, .code_count = ARRAY_COUNT(worker_code)},
        {.name = "increment", .parameter_types = ref_parameter, .parameter_count = 1,
         .capture_count = 1, .result_type = SVM_TYPE_VOID, .local_count = 1,
         .max_stack = 3, .code = cleanup_code, .code_count = ARRAY_COUNT(cleanup_code)}
    };
    SvmModule module = {.functions = functions, .function_count = 3,
                        .record_types = records, .record_type_count = 2};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 300};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 1) {
        fail(name, "handler observed state before cleanup");
    }
}

static void test_uncaught_exception(void) {
    const char *name = "uncaught exception";
    static const SvmRecordType records[] = {{"Error", NULL, 0}};
    static const SvmInstruction code[] = {
        INS(SVM_OP_NEW_OBJECT, 0), INS(SVM_OP_THROW, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "uncaught", .result_type = SVM_TYPE_I32, .max_stack = 1,
        .code = code, .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = 1,
                        .record_types = records, .record_type_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 50};
    if (svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, "uncaught exception was reported as success");
    } else if (strstr(error.message, "uncaught") == NULL) {
        fail(name, "unexpected uncaught exception error");
    }
}

static void test_defer_lifo_order(void) {
    const char *name = "defer LIFO order";
    static const SvmType fields[] = {SVM_TYPE_I32};
    static const SvmRecordType records[] = {{"Counter", fields, 1}};
    static const SvmType ref_parameter[] = {SVM_TYPE_REF};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_NEW_OBJECT, 0), INS(SVM_OP_STORE_LOCAL, 0),
        INS(SVM_OP_LOAD_LOCAL, 0), INS(SVM_OP_CALL, 1), INS(SVM_OP_POP, 0),
        INS(SVM_OP_LOAD_LOCAL, 0), {SVM_OP_GET_FIELD, 0, 0},
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction worker_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0), {SVM_OP_NEW_CLOSURE, 2, 1},
        INS(SVM_OP_DEFER_PUSH, 2),
        INS(SVM_OP_LOAD_LOCAL, 0), {SVM_OP_NEW_CLOSURE, 3, 1},
        INS(SVM_OP_DEFER_PUSH, 3),
        INS(SVM_OP_CONST_I32, 0), INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction set_one[] = {
        INS(SVM_OP_LOAD_LOCAL, 0), INS(SVM_OP_CONST_I32, 1),
        {SVM_OP_SET_FIELD, 0, 0}, INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction set_two[] = {
        INS(SVM_OP_LOAD_LOCAL, 0), INS(SVM_OP_CONST_I32, 2),
        {SVM_OP_SET_FIELD, 0, 0}, INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "entry", .result_type = SVM_TYPE_I32, .local_count = 1,
         .max_stack = 2, .code = entry_code, .code_count = ARRAY_COUNT(entry_code)},
        {.name = "worker", .parameter_types = ref_parameter, .parameter_count = 1,
         .result_type = SVM_TYPE_I32, .local_count = 1, .max_stack = 1,
         .code = worker_code, .code_count = ARRAY_COUNT(worker_code)},
        {.name = "set_one", .parameter_types = ref_parameter, .parameter_count = 1,
         .capture_count = 1, .result_type = SVM_TYPE_VOID, .local_count = 1,
         .max_stack = 2, .code = set_one, .code_count = ARRAY_COUNT(set_one)},
        {.name = "set_two", .parameter_types = ref_parameter, .parameter_count = 1,
         .capture_count = 1, .result_type = SVM_TYPE_VOID, .local_count = 1,
         .max_stack = 2, .code = set_two, .code_count = ARRAY_COUNT(set_two)}
    };
    SvmModule module = {.functions = functions, .function_count = 4,
                        .record_types = records, .record_type_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 300};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 1) {
        fail(name, "deferred closures did not run in reverse order");
    }
}

static void test_spawn_and_await(void) {
    const char *name = "spawn and await";
    static const SvmType parameter[] = {SVM_TYPE_I32};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_CONST_I32, 5), INS(SVM_OP_TASK_SPAWN, 1),
        INS(SVM_OP_TASK_AWAIT, 1), INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction worker_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0), INS(SVM_OP_CONST_I32, 1),
        INS(SVM_OP_I32_ADD, 0), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "entry", .result_type = SVM_TYPE_I32, .max_stack = 1,
         .code = entry_code, .code_count = ARRAY_COUNT(entry_code)},
        {.name = "worker", .parameter_types = parameter, .parameter_count = 1,
         .result_type = SVM_TYPE_I32, .local_count = 1, .max_stack = 2,
         .code = worker_code, .code_count = ARRAY_COUNT(worker_code)}
    };
    SvmModule module = {.functions = functions, .function_count = 2};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100, .scheduler_quantum = 1};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 6) {
        fail(name, "await returned wrong result");
    }
}

static void test_unbuffered_channel(void) {
    const char *name = "unbuffered channel";
    static const SvmType channel_parameter[] = {SVM_TYPE_CHANNEL};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_CONST_I32, 0), INS(SVM_OP_CHANNEL_NEW, SVM_TYPE_I32),
        INS(SVM_OP_DUP, 0), INS(SVM_OP_TASK_SPAWN, 1),
        INS(SVM_OP_STORE_LOCAL, 0),
        INS(SVM_OP_CHANNEL_RECV, SVM_TYPE_I32),
        INS(SVM_OP_LOAD_LOCAL, 0), INS(SVM_OP_TASK_AWAIT, 1),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction producer_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0), INS(SVM_OP_CONST_I32, 42),
        INS(SVM_OP_CHANNEL_SEND, SVM_TYPE_I32), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "entry", .result_type = SVM_TYPE_I32, .local_count = 1,
         .max_stack = 2, .code = entry_code, .code_count = ARRAY_COUNT(entry_code)},
        {.name = "producer", .parameter_types = channel_parameter,
         .parameter_count = 1, .result_type = SVM_TYPE_VOID, .local_count = 1,
         .max_stack = 2, .code = producer_code,
         .code_count = ARRAY_COUNT(producer_code)}
    };
    SvmModule module = {.functions = functions, .function_count = 2};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 200, .scheduler_quantum = 1};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 42) {
        fail(name, "channel delivered wrong value");
    }
}

static void test_channel_select(void) {
    const char *name = "channel select";
    static const SvmType channel_parameter[] = {SVM_TYPE_CHANNEL};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_CONST_I32, 0), INS(SVM_OP_CHANNEL_NEW, SVM_TYPE_I32),
        INS(SVM_OP_STORE_LOCAL, 0),
        INS(SVM_OP_CONST_I32, 0), INS(SVM_OP_CHANNEL_NEW, SVM_TYPE_I32),
        INS(SVM_OP_STORE_LOCAL, 1),
        INS(SVM_OP_LOAD_LOCAL, 1), INS(SVM_OP_TASK_SPAWN, 1), INS(SVM_OP_POP, 0),
        INS(SVM_OP_LOAD_LOCAL, 0), INS(SVM_OP_LOAD_LOCAL, 1),
        {SVM_OP_CHANNEL_SELECT, 2, SVM_TYPE_I32},
        INS(SVM_OP_I32_ADD, 0), INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction producer_code[] = {
        INS(SVM_OP_LOAD_LOCAL, 0), INS(SVM_OP_CONST_I32, 7),
        INS(SVM_OP_CHANNEL_SEND, SVM_TYPE_I32), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "select_entry", .result_type = SVM_TYPE_I32, .local_count = 2,
         .max_stack = 2, .code = entry_code, .code_count = ARRAY_COUNT(entry_code)},
        {.name = "select_producer", .parameter_types = channel_parameter,
         .parameter_count = 1, .result_type = SVM_TYPE_VOID, .local_count = 1,
         .max_stack = 2, .code = producer_code,
         .code_count = ARRAY_COUNT(producer_code)}
    };
    SvmModule module = {.functions = functions, .function_count = 2};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 300, .scheduler_quantum = 1};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 8) {
        fail(name, "select did not return case 1 and value 7");
    }
}

static void test_closed_channel_zero(void) {
    const char *name = "closed channel receive";
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, 0), INS(SVM_OP_CHANNEL_NEW, SVM_TYPE_I32),
        INS(SVM_OP_DUP, 0), INS(SVM_OP_CHANNEL_CLOSE, 0),
        INS(SVM_OP_CHANNEL_RECV, SVM_TYPE_I32), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "closed_receive", .result_type = SVM_TYPE_I32, .max_stack = 2,
        .code = code, .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 0) {
        fail(name, "closed channel did not return zero value");
    }
}

static void test_task_limit(void) {
    const char *name = "task quota";
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_TASK_SPAWN, 1), INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction worker_code[] = {
        INS(SVM_OP_CONST_I32, 1), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "entry", .result_type = SVM_TYPE_TASK, .max_stack = 1,
         .code = entry_code, .code_count = ARRAY_COUNT(entry_code)},
        {.name = "worker", .result_type = SVM_TYPE_I32, .max_stack = 1,
         .code = worker_code, .code_count = ARRAY_COUNT(worker_code)}
    };
    SvmModule module = {.functions = functions, .function_count = 2};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 50, .task_limit = 1};
    if (svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, "task quota was not enforced");
    } else if (strstr(error.message, "task limit") == NULL) {
        fail(name, "unexpected task quota error");
    }
}

static void test_channel_limit(void) {
    const char *name = "channel quota";
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, 0), INS(SVM_OP_CHANNEL_NEW, SVM_TYPE_I32),
        INS(SVM_OP_POP, 0),
        INS(SVM_OP_CONST_I32, 0), INS(SVM_OP_CHANNEL_NEW, SVM_TYPE_I32),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "too_many_channels", .result_type = SVM_TYPE_CHANNEL,
        .max_stack = 1, .code = code, .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 50, .channel_limit = 1};
    if (svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, "channel quota was not enforced");
    } else if (strstr(error.message, "channel limit") == NULL) {
        fail(name, "unexpected channel quota error");
    }
}

static void test_await_propagates_failure(void) {
    const char *name = "await propagates failure";
    static const SvmRecordType records[] = {{"Error", NULL, 0}};
    static const SvmExceptionHandler handlers[] = {{1, 2, 3}};
    static const SvmInstruction entry_code[] = {
        INS(SVM_OP_TASK_SPAWN, 1), INS(SVM_OP_TASK_AWAIT, 1),
        INS(SVM_OP_RETURN, 0), INS(SVM_OP_POP, 0),
        INS(SVM_OP_CONST_I32, 9), INS(SVM_OP_RETURN, 0)
    };
    static const SvmInstruction worker_code[] = {
        INS(SVM_OP_NEW_OBJECT, 0), INS(SVM_OP_THROW, 0)
    };
    static const SvmFunction functions[] = {
        {.name = "await_catcher", .result_type = SVM_TYPE_I32, .max_stack = 1,
         .code = entry_code, .code_count = ARRAY_COUNT(entry_code),
         .handlers = handlers, .handler_count = 1},
        {.name = "failing_task", .result_type = SVM_TYPE_I32, .max_stack = 1,
         .code = worker_code, .code_count = ARRAY_COUNT(worker_code)}
    };
    SvmModule module = {.functions = functions, .function_count = 2,
                        .record_types = records, .record_type_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100, .scheduler_quantum = 1};
    if (!svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 9) {
        fail(name, "await failure did not enter handler");
    }
}

static void test_scheduler_deadlock(void) {
    const char *name = "scheduler deadlock";
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, 0), INS(SVM_OP_CHANNEL_NEW, SVM_TYPE_I32),
        INS(SVM_OP_CHANNEL_RECV, SVM_TYPE_I32), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "deadlock", .result_type = SVM_TYPE_I32, .max_stack = 1,
        .code = code, .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 50};
    if (svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, "deadlocked program reported success");
    } else if (strstr(error.message, "deadlock") == NULL) {
        fail(name, "unexpected deadlock error");
    }
}

static void test_sync_capability(void) {
    const char *name = "synchronous capability";
    static const SvmType parameters[] = {SVM_TYPE_I32, SVM_TYPE_I32};
    static const SvmHostImport imports[] = {{
        "math", "add", parameters, 2, SVM_TYPE_I32, false
    }};
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, 2), INS(SVM_OP_CONST_I32, 3),
        INS(SVM_OP_HOST_CALL, 0), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "host_add", .result_type = SVM_TYPE_I32, .max_stack = 2,
        .code = code, .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = 1,
                        .host_imports = imports, .host_import_count = 1};
    HostTestContext context = {0};
    static const SvmHostFunction host_functions[] = {{
        "add", parameters, 2, SVM_TYPE_I32, 3, host_add, NULL
    }};
    SvmCapability capability = {"math", &context, host_functions, 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100, .host_call_budget = 3};
    if (!svm_execute_with_capabilities(
            &module, 0, NULL, 0, limits, &capability, 1, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 5 ||
               context.invoke_count != 1) {
        fail(name, "host add returned wrong result or call count");
    }
}

static void test_missing_capability(void) {
    const char *name = "missing capability";
    static const SvmHostImport imports[] = {{
        "secret", "read", NULL, 0, SVM_TYPE_I32, false
    }};
    static const SvmInstruction code[] = {
        INS(SVM_OP_HOST_CALL, 0), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "missing", .result_type = SVM_TYPE_I32, .max_stack = 1,
        .code = code, .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {.functions = functions, .function_count = 1,
                        .host_imports = imports, .host_import_count = 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 20};
    if (svm_execute(&module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, "module ran without a granted capability");
    } else if (strstr(error.message, "not granted") == NULL) {
        fail(name, "unexpected missing capability error");
    }
}

static void test_host_budget_before_callback(void) {
    const char *name = "host budget before callback";
    static const SvmType parameters[] = {SVM_TYPE_I32, SVM_TYPE_I32};
    static const SvmHostImport imports[] = {{
        "math", "add", parameters, 2, SVM_TYPE_I32, false
    }};
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, 1), INS(SVM_OP_CONST_I32, 2),
        INS(SVM_OP_HOST_CALL, 0), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "over_budget", .result_type = SVM_TYPE_I32, .max_stack = 2,
        .code = code, .code_count = ARRAY_COUNT(code)
    }};
    static const SvmHostFunction host_functions[] = {{
        "add", parameters, 2, SVM_TYPE_I32, 5, host_add, NULL
    }};
    SvmModule module = {.functions = functions, .function_count = 1,
                        .host_imports = imports, .host_import_count = 1};
    HostTestContext context = {0};
    SvmCapability capability = {"math", &context, host_functions, 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 50, .host_call_budget = 4};
    if (svm_execute_with_capabilities(
            &module, 0, NULL, 0, limits, &capability, 1, &result, &error)) {
        fail(name, "over-budget host call succeeded");
    } else if (strstr(error.message, "host call budget") == NULL ||
               context.invoke_count != 0) {
        fail(name, "callback ran before budget enforcement");
    }
}

static void test_async_capability(void) {
    const char *name = "asynchronous capability";
    static const SvmType parameters[] = {SVM_TYPE_I32};
    static const SvmHostImport imports[] = {{
        "timer", "increment", parameters, 1, SVM_TYPE_I32, true
    }};
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, 10), INS(SVM_OP_HOST_CALL_ASYNC, 0),
        INS(SVM_OP_TASK_AWAIT, -1), INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "async_host", .result_type = SVM_TYPE_I32, .max_stack = 1,
        .code = code, .code_count = ARRAY_COUNT(code)
    }};
    static const SvmHostFunction host_functions[] = {{
        "increment", parameters, 1, SVM_TYPE_I32, 2,
        host_increment_later, host_poll_increment
    }};
    SvmModule module = {.functions = functions, .function_count = 1,
                        .host_imports = imports, .host_import_count = 1};
    HostTestContext context = {0};
    SvmCapability capability = {"timer", &context, host_functions, 1};
    SvmValue result;
    SvmError error;
    SvmLimits limits = {.instruction_budget = 100, .host_call_budget = 6,
                        .scheduler_quantum = 1};
    if (!svm_execute_with_capabilities(
            &module, 0, NULL, 0, limits, &capability, 1, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != 11 ||
               context.invoke_count != 1 || context.poll_count != 2) {
        fail(name, "async host task resumed incorrectly");
    }
}

int main(void) {
    test_factorial();
    test_rejects_type_error();
    test_rejects_bad_jump();
    test_instruction_budget();
    test_call_depth_limit();
    test_object_survives_gc();
    test_array_program();
    test_closure_program();
    test_runtime_rejects_wrong_record();
    test_cross_frame_exception();
    test_defer_on_return();
    test_defer_on_throw();
    test_uncaught_exception();
    test_defer_lifo_order();
    test_spawn_and_await();
    test_unbuffered_channel();
    test_channel_select();
    test_closed_channel_zero();
    test_task_limit();
    test_channel_limit();
    test_await_propagates_failure();
    test_scheduler_deadlock();
    test_sync_capability();
    test_missing_capability();
    test_host_budget_before_callback();
    test_async_capability();

    if (failures != 0) {
        fprintf(stderr, "%d reference test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("reference core: all tests passed\n");
    return EXIT_SUCCESS;
}
