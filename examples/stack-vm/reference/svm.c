#include "svm.h"
#include "heap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool reached;
    SvmType locals[SVM_MAX_LOCALS];
    SvmType stack[SVM_MAX_STACK];
    uint32_t stack_size;
} VerifyState;

typedef enum {
    SVM_EXIT_NONE,
    SVM_EXIT_RETURN,
    SVM_EXIT_THROW
} SvmExitKind;

typedef struct {
    const SvmFunction *function;
    uint32_t function_index;
    uint32_t pc;
    SvmValue locals[SVM_MAX_LOCALS];
    SvmValue stack[SVM_MAX_STACK];
    uint32_t stack_size;
    SvmValue defers[SVM_MAX_DEFERS];
    uint32_t defer_count;
    SvmExitKind exit_kind;
    SvmValue exit_value;
    bool is_defer_call;
} SvmFrame;

typedef enum {
    SVM_TASK_RUNNABLE,
    SVM_TASK_RUNNING,
    SVM_TASK_WAITING,
    SVM_TASK_DONE,
    SVM_TASK_FAILED,
    SVM_TASK_CANCELLED
} SvmTaskState;

typedef enum {
    SVM_WAIT_NONE,
    SVM_WAIT_TASK,
    SVM_WAIT_SEND,
    SVM_WAIT_RECV,
    SVM_WAIT_SELECT,
    SVM_WAIT_HOST
} SvmWaitKind;

typedef struct {
    uint32_t id;
    SvmTaskState state;
    SvmFrame frames[SVM_MAX_FRAMES];
    uint32_t frame_count;
    SvmValue result;
    SvmValue failure;
    uint32_t entry_function;
    int32_t await_key;
    SvmWaitKind wait_kind;
    uint32_t wait_target;
    SvmValue wait_value;
    uint32_t select_count;
    uint32_t select_channels[SVM_MAX_SELECT_CASES];
    uint32_t select_cursor;
    uint32_t slice_remaining;
    bool is_host_task;
    uint32_t host_import_index;
    uint64_t host_token;
} SvmTask;

typedef struct {
    const SvmHostFunction *function;
    void *context;
} SvmBoundHostFunction;

typedef struct {
    bool used;
    bool closed;
    SvmType element_type;
    uint32_t capacity;
    uint32_t count;
    uint32_t read_pos;
    uint32_t write_pos;
    SvmValue buffer[SVM_MAX_CHANNEL_CAPACITY];
} SvmChannel;

typedef struct {
    const SvmModule *module;
    SvmTask tasks[SVM_MAX_TASKS];
    uint32_t task_count;
    uint32_t current_task_index;
    SvmChannel channels[SVM_MAX_CHANNELS];
    uint32_t channel_count;
    uint32_t task_limit;
    uint32_t channel_limit;
    uint32_t scheduler_quantum;
    SvmBoundHostFunction bound_host[SVM_MAX_HOST_IMPORTS];
    uint64_t host_budget;
    uint64_t budget;
    uint32_t depth_limit;
    SvmHeap heap;
    SvmError *error;
} Svm;

#define CURRENT_TASK(vm) (&(vm)->tasks[(vm)->current_task_index])
#define CURRENT_FRAMES(vm) (CURRENT_TASK(vm)->frames)
#define CURRENT_FRAME_COUNT(vm) (CURRENT_TASK(vm)->frame_count)

static bool is_runtime_type(SvmType type) {
    return type == SVM_TYPE_I32 || type == SVM_TYPE_BOOL ||
           type == SVM_TYPE_NULL || type == SVM_TYPE_REF ||
           type == SVM_TYPE_ANY || type == SVM_TYPE_VOID ||
           type == SVM_TYPE_TASK || type == SVM_TYPE_CHANNEL;
}

static bool is_array_element_type(SvmType type) {
    return type == SVM_TYPE_I32 || type == SVM_TYPE_BOOL || type == SVM_TYPE_REF;
}

static bool is_host_value_type(SvmType type) {
    return type == SVM_TYPE_I32 || type == SVM_TYPE_BOOL;
}

static bool await_key_result_type(
    const SvmModule *module,
    int32_t key,
    SvmType *result_type
) {
    if (key >= 0) {
        if ((uint32_t)key >= module->function_count) return false;
        *result_type = module->functions[key].result_type;
        return true;
    }
    uint32_t import_index = (uint32_t)(-(int64_t)key - 1);
    if (import_index >= module->host_import_count ||
        !module->host_imports[import_index].asynchronous) return false;
    *result_type = module->host_imports[import_index].result_type;
    return true;
}

static bool set_error(
    SvmError *error,
    uint32_t function_index,
    uint32_t pc,
    const char *format,
    ...
) {
    if (error != NULL) {
        va_list arguments;
        error->function_index = function_index;
        error->pc = pc;
        va_start(arguments, format);
        (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
        va_end(arguments);
    }
    return false;
}

SvmValue svm_i32(int32_t value) {
    SvmValue result = {.type = SVM_TYPE_I32};
    result.as.i32 = value;
    return result;
}

SvmValue svm_bool(bool value) {
    SvmValue result = {.type = SVM_TYPE_BOOL};
    result.as.boolean = value;
    return result;
}

SvmValue svm_null(void) {
    SvmValue result = {.type = SVM_TYPE_NULL};
    result.as.ref = 0;
    return result;
}

static bool verify_pop(
    VerifyState *state,
    SvmType expected,
    SvmError *error,
    uint32_t function_index,
    uint32_t pc
) {
    if (state->stack_size == 0) {
        return set_error(error, function_index, pc, "operand stack underflow");
    }
    SvmType actual = state->stack[--state->stack_size];
    bool assignable = actual == expected ||
        (expected == SVM_TYPE_REF && actual == SVM_TYPE_NULL) ||
        (expected == SVM_TYPE_ANY && actual != SVM_TYPE_UNINIT && actual != SVM_TYPE_VOID);
    if (!assignable) {
        return set_error(
            error, function_index, pc,
            "expected stack type %d but found %d", (int)expected, (int)actual
        );
    }
    return true;
}

static bool verify_push(
    VerifyState *state,
    SvmType type,
    const SvmFunction *function,
    SvmError *error,
    uint32_t function_index,
    uint32_t pc
) {
    if (type == SVM_TYPE_VOID || type == SVM_TYPE_UNINIT) {
        return set_error(error, function_index, pc, "cannot push non-value type");
    }
    if (state->stack_size >= function->max_stack ||
        state->stack_size >= SVM_MAX_STACK) {
        return set_error(error, function_index, pc, "operand stack overflow");
    }
    state->stack[state->stack_size++] = type;
    return true;
}

static bool merge_state(
    VerifyState *destination,
    const VerifyState *source,
    uint32_t local_count,
    bool *changed,
    SvmError *error,
    uint32_t function_index,
    uint32_t pc
) {
    if (!destination->reached) {
        *destination = *source;
        destination->reached = true;
        *changed = true;
        return true;
    }
    if (destination->stack_size != source->stack_size) {
        return set_error(error, function_index, pc, "different stack heights merge");
    }
    for (uint32_t i = 0; i < local_count; i++) {
        if (destination->locals[i] != source->locals[i]) {
            return set_error(error, function_index, pc, "different local types merge");
        }
    }
    for (uint32_t i = 0; i < source->stack_size; i++) {
        if (destination->stack[i] != source->stack[i]) {
            return set_error(error, function_index, pc, "different stack types merge");
        }
    }
    *changed = false;
    return true;
}

static bool queue_successor(
    VerifyState *states,
    uint32_t *worklist,
    uint32_t *work_count,
    uint32_t target,
    const VerifyState *state,
    const SvmFunction *function,
    SvmError *error,
    uint32_t function_index,
    uint32_t source_pc
) {
    if (target >= function->code_count) {
        return set_error(error, function_index, source_pc, "jump target out of bounds");
    }
    bool changed = false;
    if (!merge_state(
            &states[target], state, function->local_count, &changed,
            error, function_index, target)) {
        return false;
    }
    if (changed) {
        worklist[(*work_count)++] = target;
    }
    return true;
}

static const SvmExceptionHandler *find_handler(
    const SvmFunction *function,
    uint32_t pc
) {
    for (uint32_t i = 0; i < function->handler_count; i++) {
        const SvmExceptionHandler *handler = &function->handlers[i];
        if (pc >= handler->start_pc && pc < handler->end_pc) return handler;
    }
    return NULL;
}

static bool verify_function(
    const SvmModule *module,
    uint32_t function_index,
    SvmError *error
) {
    const SvmFunction *function = &module->functions[function_index];
    if (function->code == NULL || function->code_count == 0) {
        return set_error(error, function_index, 0, "function has no code");
    }
    if (function->parameter_count > function->local_count ||
        function->capture_count > function->parameter_count ||
        function->local_count > SVM_MAX_LOCALS ||
        function->max_stack == 0 || function->max_stack > SVM_MAX_STACK) {
        return set_error(error, function_index, 0, "invalid function limits");
    }
    if (function->parameter_count > 0 && function->parameter_types == NULL) {
        return set_error(error, function_index, 0, "missing parameter types");
    }
    if (!is_runtime_type(function->result_type)) {
        return set_error(error, function_index, 0, "invalid result type");
    }
    for (uint32_t i = 0; i < function->parameter_count; i++) {
        if (!is_runtime_type(function->parameter_types[i]) ||
            function->parameter_types[i] == SVM_TYPE_VOID) {
            return set_error(error, function_index, 0, "invalid parameter type");
        }
    }
    if (function->handler_count > 0 && function->handlers == NULL) {
        return set_error(error, function_index, 0, "missing exception handler table");
    }
    for (uint32_t i = 0; i < function->handler_count; i++) {
        const SvmExceptionHandler *handler = &function->handlers[i];
        if (handler->start_pc >= handler->end_pc ||
            handler->end_pc > function->code_count ||
            handler->handler_pc >= function->code_count) {
            return set_error(error, function_index, 0, "invalid exception handler range");
        }
    }

    VerifyState *states = calloc(function->code_count, sizeof(*states));
    uint32_t *worklist = calloc(function->code_count, sizeof(*worklist));
    bool *queued = calloc(function->code_count, sizeof(*queued));
    if (states == NULL || worklist == NULL || queued == NULL) {
        free(states);
        free(worklist);
        free(queued);
        return set_error(error, function_index, 0, "out of memory while verifying");
    }

    states[0].reached = true;
    for (uint32_t i = 0; i < function->local_count; i++) {
        states[0].locals[i] = i < function->parameter_count
            ? function->parameter_types[i]
            : SVM_TYPE_UNINIT;
    }
    uint32_t work_count = 1;
    worklist[0] = 0;
    queued[0] = true;

    bool ok = true;
    while (work_count > 0 && ok) {
        uint32_t pc = worklist[--work_count];
        queued[pc] = false;
        VerifyState state = states[pc];
        VerifyState exceptional_state = state;
        SvmInstruction instruction = function->code[pc];
        bool has_fallthrough = true;

        switch (instruction.opcode) {
        case SVM_OP_CONST_I32:
            ok = verify_push(&state, SVM_TYPE_I32, function, error, function_index, pc);
            break;
        case SVM_OP_CONST_FALSE:
        case SVM_OP_CONST_TRUE:
            ok = verify_push(&state, SVM_TYPE_BOOL, function, error, function_index, pc);
            break;
        case SVM_OP_CONST_NULL:
            ok = verify_push(&state, SVM_TYPE_NULL, function, error, function_index, pc);
            break;
        case SVM_OP_LOAD_LOCAL: {
            int32_t index = instruction.operand;
            if (index < 0 || (uint32_t)index >= function->local_count) {
                ok = set_error(error, function_index, pc, "local index out of bounds");
            } else if (state.locals[index] == SVM_TYPE_UNINIT) {
                ok = set_error(error, function_index, pc, "read of uninitialized local");
            } else {
                ok = verify_push(
                    &state, state.locals[index], function, error, function_index, pc
                );
            }
            break;
        }
        case SVM_OP_STORE_LOCAL: {
            int32_t index = instruction.operand;
            if (index < 0 || (uint32_t)index >= function->local_count ||
                state.stack_size == 0) {
                ok = set_error(error, function_index, pc, "invalid store_local");
            } else {
                state.locals[index] = state.stack[--state.stack_size];
            }
            break;
        }
        case SVM_OP_DUP:
            if (state.stack_size == 0) {
                ok = set_error(error, function_index, pc, "dup on empty stack");
            } else {
                ok = verify_push(
                    &state, state.stack[state.stack_size - 1], function,
                    error, function_index, pc
                );
            }
            break;
        case SVM_OP_POP:
            if (state.stack_size == 0) {
                ok = set_error(error, function_index, pc, "pop on empty stack");
            } else {
                state.stack_size--;
            }
            break;
        case SVM_OP_I32_ADD:
        case SVM_OP_I32_SUB:
        case SVM_OP_I32_MUL:
            ok = verify_pop(&state, SVM_TYPE_I32, error, function_index, pc) &&
                 verify_pop(&state, SVM_TYPE_I32, error, function_index, pc) &&
                 verify_push(&state, SVM_TYPE_I32, function, error, function_index, pc);
            break;
        case SVM_OP_I32_LE:
        case SVM_OP_I32_EQ:
            ok = verify_pop(&state, SVM_TYPE_I32, error, function_index, pc) &&
                 verify_pop(&state, SVM_TYPE_I32, error, function_index, pc) &&
                 verify_push(&state, SVM_TYPE_BOOL, function, error, function_index, pc);
            break;
        case SVM_OP_JUMP:
            has_fallthrough = false;
            ok = queue_successor(
                states, worklist, &work_count, (uint32_t)instruction.operand,
                &state, function, error, function_index, pc
            );
            break;
        case SVM_OP_JUMP_IF_FALSE:
            ok = verify_pop(&state, SVM_TYPE_BOOL, error, function_index, pc) &&
                 queue_successor(
                    states, worklist, &work_count, (uint32_t)instruction.operand,
                    &state, function, error, function_index, pc
                 );
            break;
        case SVM_OP_CALL: {
            int32_t target_index = instruction.operand;
            if (target_index < 0 || (uint32_t)target_index >= module->function_count) {
                ok = set_error(error, function_index, pc, "call target out of bounds");
                break;
            }
            const SvmFunction *target = &module->functions[target_index];
            if (target->capture_count != 0) {
                ok = set_error(error, function_index, pc,
                               "capturing function requires call_closure");
                break;
            }
            for (uint32_t i = target->parameter_count; i > 0 && ok; i--) {
                ok = verify_pop(
                    &state, target->parameter_types[i - 1], error, function_index, pc
                );
            }
            if (ok) {
                if (target->result_type != SVM_TYPE_VOID) {
                    ok = verify_push(
                        &state, target->result_type, function, error, function_index, pc
                    );
                }
            }
            break;
        }
        case SVM_OP_NEW_OBJECT: {
            int32_t type_index = instruction.operand;
            if (type_index < 0 || (uint32_t)type_index >= module->record_type_count) {
                ok = set_error(error, function_index, pc, "record type out of bounds");
            } else {
                ok = verify_push(&state, SVM_TYPE_REF, function, error, function_index, pc);
            }
            break;
        }
        case SVM_OP_GET_FIELD:
        case SVM_OP_SET_FIELD: {
            int32_t type_index = instruction.operand;
            int32_t field_index = instruction.operand2;
            if (type_index < 0 || (uint32_t)type_index >= module->record_type_count) {
                ok = set_error(error, function_index, pc, "record type out of bounds");
                break;
            }
            const SvmRecordType *record = &module->record_types[type_index];
            if (field_index < 0 || (uint32_t)field_index >= record->field_count) {
                ok = set_error(error, function_index, pc, "field index out of bounds");
                break;
            }
            if (instruction.opcode == SVM_OP_SET_FIELD) {
                ok = verify_pop(&state, record->field_types[field_index], error,
                                function_index, pc);
            }
            if (ok) ok = verify_pop(&state, SVM_TYPE_REF, error, function_index, pc);
            if (ok && instruction.opcode == SVM_OP_GET_FIELD) {
                ok = verify_push(&state, record->field_types[field_index], function,
                                 error, function_index, pc);
            }
            break;
        }
        case SVM_OP_NEW_ARRAY: {
            SvmType element_type = (SvmType)instruction.operand;
            if (!is_array_element_type(element_type)) {
                ok = set_error(error, function_index, pc, "invalid array element type");
            } else {
                ok = verify_pop(&state, SVM_TYPE_I32, error, function_index, pc) &&
                     verify_push(&state, SVM_TYPE_REF, function, error, function_index, pc);
            }
            break;
        }
        case SVM_OP_ARRAY_LEN:
            if (!is_array_element_type((SvmType)instruction.operand)) {
                ok = set_error(error, function_index, pc, "invalid array element type");
            } else {
                ok = verify_pop(&state, SVM_TYPE_REF, error, function_index, pc) &&
                     verify_push(&state, SVM_TYPE_I32, function, error, function_index, pc);
            }
            break;
        case SVM_OP_ARRAY_GET: {
            SvmType element_type = (SvmType)instruction.operand;
            if (!is_array_element_type(element_type)) {
                ok = set_error(error, function_index, pc, "invalid array element type");
            } else {
                ok = verify_pop(&state, SVM_TYPE_I32, error, function_index, pc) &&
                     verify_pop(&state, SVM_TYPE_REF, error, function_index, pc) &&
                     verify_push(&state, element_type, function, error, function_index, pc);
            }
            break;
        }
        case SVM_OP_ARRAY_SET: {
            SvmType element_type = (SvmType)instruction.operand;
            if (!is_array_element_type(element_type)) {
                ok = set_error(error, function_index, pc, "invalid array element type");
            } else {
                ok = verify_pop(&state, element_type, error, function_index, pc) &&
                     verify_pop(&state, SVM_TYPE_I32, error, function_index, pc) &&
                     verify_pop(&state, SVM_TYPE_REF, error, function_index, pc);
            }
            break;
        }
        case SVM_OP_NEW_CLOSURE:
        case SVM_OP_CALL_CLOSURE: {
            int32_t target_index = instruction.operand;
            if (target_index < 0 || (uint32_t)target_index >= module->function_count) {
                ok = set_error(error, function_index, pc, "closure target out of bounds");
                break;
            }
            const SvmFunction *target = &module->functions[target_index];
            if (instruction.opcode == SVM_OP_NEW_CLOSURE) {
                if (instruction.operand2 < 0 ||
                    (uint32_t)instruction.operand2 != target->capture_count) {
                    ok = set_error(error, function_index, pc, "wrong closure capture count");
                    break;
                }
                for (uint32_t i = target->capture_count; i > 0 && ok; i--) {
                    ok = verify_pop(&state, target->parameter_types[i - 1], error,
                                    function_index, pc);
                }
                if (ok) ok = verify_push(&state, SVM_TYPE_REF, function, error,
                                         function_index, pc);
            } else {
                for (uint32_t i = target->parameter_count; i > target->capture_count && ok; i--) {
                    ok = verify_pop(&state, target->parameter_types[i - 1], error,
                                    function_index, pc);
                }
                if (ok) ok = verify_pop(&state, SVM_TYPE_REF, error, function_index, pc);
                if (ok && target->result_type != SVM_TYPE_VOID) {
                    ok = verify_push(&state, target->result_type, function, error,
                                     function_index, pc);
                }
            }
            break;
        }
        case SVM_OP_GC_COLLECT:
            break;
        case SVM_OP_THROW: {
            ok = verify_pop(&state, SVM_TYPE_ANY, error, function_index, pc);
            has_fallthrough = false;
            const SvmExceptionHandler *handler = find_handler(function, pc);
            if (ok && handler != NULL) {
                state.stack_size = 0;
                ok = verify_push(&state, SVM_TYPE_ANY, function, error,
                                 function_index, pc) &&
                     queue_successor(
                        states, worklist, &work_count, handler->handler_pc,
                        &state, function, error, function_index, pc
                     );
            }
            break;
        }
        case SVM_OP_DEFER_PUSH: {
            int32_t target_index = instruction.operand;
            if (target_index < 0 || (uint32_t)target_index >= module->function_count) {
                ok = set_error(error, function_index, pc, "defer target out of bounds");
                break;
            }
            const SvmFunction *target = &module->functions[target_index];
            if (target->parameter_count != target->capture_count ||
                target->result_type != SVM_TYPE_VOID) {
                ok = set_error(error, function_index, pc,
                               "defer target must be a void closure without explicit arguments");
            } else {
                ok = verify_pop(&state, SVM_TYPE_REF, error, function_index, pc);
            }
            break;
        }
        case SVM_OP_TASK_SPAWN: {
            int32_t target_index = instruction.operand;
            if (target_index < 0 || (uint32_t)target_index >= module->function_count) {
                ok = set_error(error, function_index, pc, "task target out of bounds");
                break;
            }
            const SvmFunction *target = &module->functions[target_index];
            if (target->capture_count != 0) {
                ok = set_error(error, function_index, pc,
                               "spawn target cannot require captures");
                break;
            }
            for (uint32_t i = target->parameter_count; i > 0 && ok; i--) {
                ok = verify_pop(&state, target->parameter_types[i - 1], error,
                                function_index, pc);
            }
            if (ok) ok = verify_push(&state, SVM_TYPE_TASK, function, error,
                                     function_index, pc);
            break;
        }
        case SVM_OP_TASK_AWAIT: {
            SvmType result_type;
            if (!await_key_result_type(module, instruction.operand, &result_type)) {
                ok = set_error(error, function_index, pc, "invalid await signature key");
                break;
            }
            ok = verify_pop(&state, SVM_TYPE_TASK, error, function_index, pc);
            if (ok && result_type != SVM_TYPE_VOID) {
                ok = verify_push(&state, result_type, function, error,
                                 function_index, pc);
            }
            break;
        }
        case SVM_OP_TASK_YIELD:
            break;
        case SVM_OP_CHANNEL_NEW: {
            SvmType element_type = (SvmType)instruction.operand;
            if (!is_array_element_type(element_type)) {
                ok = set_error(error, function_index, pc, "invalid channel element type");
            } else {
                ok = verify_pop(&state, SVM_TYPE_I32, error, function_index, pc) &&
                     verify_push(&state, SVM_TYPE_CHANNEL, function, error,
                                 function_index, pc);
            }
            break;
        }
        case SVM_OP_CHANNEL_SEND: {
            SvmType element_type = (SvmType)instruction.operand;
            if (!is_array_element_type(element_type)) {
                ok = set_error(error, function_index, pc, "invalid channel element type");
            } else {
                ok = verify_pop(&state, element_type, error, function_index, pc) &&
                     verify_pop(&state, SVM_TYPE_CHANNEL, error, function_index, pc);
            }
            break;
        }
        case SVM_OP_CHANNEL_RECV: {
            SvmType element_type = (SvmType)instruction.operand;
            if (!is_array_element_type(element_type)) {
                ok = set_error(error, function_index, pc, "invalid channel element type");
            } else {
                ok = verify_pop(&state, SVM_TYPE_CHANNEL, error, function_index, pc) &&
                     verify_push(&state, element_type, function, error,
                                 function_index, pc);
            }
            break;
        }
        case SVM_OP_CHANNEL_CLOSE:
            ok = verify_pop(&state, SVM_TYPE_CHANNEL, error, function_index, pc);
            break;
        case SVM_OP_CHANNEL_SELECT: {
            int32_t case_count = instruction.operand;
            SvmType element_type = (SvmType)instruction.operand2;
            if (case_count <= 0 || case_count > (int32_t)SVM_MAX_SELECT_CASES ||
                !is_array_element_type(element_type)) {
                ok = set_error(error, function_index, pc, "invalid channel select descriptor");
                break;
            }
            for (int32_t i = 0; i < case_count && ok; i++) {
                ok = verify_pop(&state, SVM_TYPE_CHANNEL, error, function_index, pc);
            }
            if (ok) ok = verify_push(&state, SVM_TYPE_I32, function, error,
                                     function_index, pc) &&
                         verify_push(&state, element_type, function, error,
                                     function_index, pc);
            break;
        }
        case SVM_OP_HOST_CALL:
        case SVM_OP_HOST_CALL_ASYNC: {
            int32_t import_index = instruction.operand;
            if (import_index < 0 ||
                (uint32_t)import_index >= module->host_import_count) {
                ok = set_error(error, function_index, pc, "host import out of bounds");
                break;
            }
            const SvmHostImport *import = &module->host_imports[import_index];
            bool expected_async = instruction.opcode == SVM_OP_HOST_CALL_ASYNC;
            if (import->asynchronous != expected_async) {
                ok = set_error(error, function_index, pc, "wrong host call mode");
                break;
            }
            for (uint32_t i = import->parameter_count; i > 0 && ok; i--) {
                ok = verify_pop(&state, import->parameter_types[i - 1], error,
                                function_index, pc);
            }
            if (ok && (expected_async || import->result_type != SVM_TYPE_VOID)) {
                ok = verify_push(&state,
                    expected_async ? SVM_TYPE_TASK : import->result_type,
                    function, error, function_index, pc);
            }
            break;
        }
        case SVM_OP_RETURN:
            has_fallthrough = false;
            if (function->result_type != SVM_TYPE_VOID) {
                ok = verify_pop(
                    &state, function->result_type, error, function_index, pc
                );
            }
            if (ok && state.stack_size != 0) {
                ok = set_error(error, function_index, pc, "return leaves values on stack");
            }
            break;
        default:
            ok = set_error(error, function_index, pc, "unknown opcode");
            break;
        }

        const SvmExceptionHandler *implicit_handler = find_handler(function, pc);
        if (ok && instruction.opcode != SVM_OP_THROW && implicit_handler != NULL) {
            exceptional_state.stack_size = 0;
            ok = verify_push(&exceptional_state, SVM_TYPE_ANY, function, error,
                             function_index, pc) &&
                 queue_successor(
                    states, worklist, &work_count, implicit_handler->handler_pc,
                    &exceptional_state, function, error, function_index, pc
                 );
        }

        if (ok && has_fallthrough) {
            if (pc + 1 >= function->code_count) {
                ok = set_error(error, function_index, pc, "control falls off function");
            } else {
                bool changed = false;
                ok = merge_state(
                    &states[pc + 1], &state, function->local_count, &changed,
                    error, function_index, pc + 1
                );
                if (ok && changed && !queued[pc + 1]) {
                    worklist[work_count++] = pc + 1;
                    queued[pc + 1] = true;
                }
            }
        }
    }

    free(states);
    free(worklist);
    free(queued);
    return ok;
}

bool svm_verify_module(const SvmModule *module, SvmError *error) {
    if (error != NULL) memset(error, 0, sizeof(*error));
    if (module == NULL || module->functions == NULL || module->function_count == 0) {
        return set_error(error, 0, 0, "module has no functions");
    }
    if (module->record_type_count > 0 && module->record_types == NULL) {
        return set_error(error, 0, 0, "missing record type table");
    }
    for (uint32_t i = 0; i < module->record_type_count; i++) {
        const SvmRecordType *record = &module->record_types[i];
        if (record->field_count > 0 && record->field_types == NULL) {
            return set_error(error, 0, 0, "missing field type table");
        }
        for (uint32_t field = 0; field < record->field_count; field++) {
            SvmType type = record->field_types[field];
            if (type != SVM_TYPE_I32 && type != SVM_TYPE_BOOL && type != SVM_TYPE_REF) {
                return set_error(error, 0, 0, "invalid record field type");
            }
        }
    }
    if (module->host_import_count > SVM_MAX_HOST_IMPORTS ||
        (module->host_import_count > 0 && module->host_imports == NULL)) {
        return set_error(error, 0, 0, "invalid host import table");
    }
    for (uint32_t i = 0; i < module->host_import_count; i++) {
        const SvmHostImport *import = &module->host_imports[i];
        if (import->capability_name == NULL || import->function_name == NULL ||
            (import->parameter_count > 0 && import->parameter_types == NULL) ||
            (!is_host_value_type(import->result_type) &&
             import->result_type != SVM_TYPE_VOID)) {
            return set_error(error, 0, 0, "invalid host import signature");
        }
        for (uint32_t parameter = 0; parameter < import->parameter_count; parameter++) {
            if (!is_host_value_type(import->parameter_types[parameter])) {
                return set_error(error, 0, 0, "unsupported host parameter type");
            }
        }
    }
    for (uint32_t i = 0; i < module->function_count; i++) {
        if (!verify_function(module, i, error)) return false;
    }
    return true;
}

static bool runtime_error(Svm *vm, const SvmFrame *frame, const char *message) {
    return set_error(
        vm->error, frame->function_index,
        frame->pc == 0 ? 0 : frame->pc - 1, "%s", message
    );
}

static bool frame_push(Svm *vm, SvmFrame *frame, SvmValue value) {
    if (frame->stack_size >= frame->function->max_stack) {
        return runtime_error(vm, frame, "operand stack overflow");
    }
    frame->stack[frame->stack_size++] = value;
    return true;
}

static bool frame_pop(Svm *vm, SvmFrame *frame, SvmValue *value) {
    if (frame->stack_size == 0) {
        return runtime_error(vm, frame, "operand stack underflow");
    }
    *value = frame->stack[--frame->stack_size];
    return true;
}

static void collect_vm_roots(Svm *vm) {
    for (uint32_t task_index = 0; task_index < vm->task_count; task_index++) {
        SvmTask *task = &vm->tasks[task_index];
        for (uint32_t frame_index = 0; frame_index < task->frame_count; frame_index++) {
            SvmFrame *frame = &task->frames[frame_index];
            for (uint32_t i = 0; i < frame->function->local_count; i++) {
                svm_heap_mark_value(&vm->heap, frame->locals[i]);
            }
            for (uint32_t i = 0; i < frame->stack_size; i++) {
                svm_heap_mark_value(&vm->heap, frame->stack[i]);
            }
            for (uint32_t i = 0; i < frame->defer_count; i++) {
                svm_heap_mark_value(&vm->heap, frame->defers[i]);
            }
            if (frame->exit_kind != SVM_EXIT_NONE) {
                svm_heap_mark_value(&vm->heap, frame->exit_value);
            }
        }
        if (task->state == SVM_TASK_DONE) svm_heap_mark_value(&vm->heap, task->result);
        if (task->state == SVM_TASK_FAILED) svm_heap_mark_value(&vm->heap, task->failure);
        if (task->wait_kind == SVM_WAIT_SEND) {
            svm_heap_mark_value(&vm->heap, task->wait_value);
        }
    }
    for (uint32_t channel_index = 0; channel_index < vm->channel_count; channel_index++) {
        SvmChannel *channel = &vm->channels[channel_index];
        if (!channel->used) continue;
        for (uint32_t i = 0; i < channel->count; i++) {
            uint32_t slot = (channel->read_pos + i) % channel->capacity;
            svm_heap_mark_value(&vm->heap, channel->buffer[slot]);
        }
    }
    svm_heap_sweep(&vm->heap);
}

static bool runtime_heap_error(
    Svm *vm,
    const SvmFrame *frame,
    const char *heap_message
) {
    return set_error(
        vm->error, frame->function_index,
        frame->pc == 0 ? 0 : frame->pc - 1,
        "heap error: %s", heap_message == NULL ? "unknown failure" : heap_message
    );
}

static bool push_task_frame(
    Svm *vm,
    SvmTask *task,
    uint32_t function_index,
    const SvmValue *arguments,
    uint32_t argument_count
) {
    if (task->frame_count >= vm->depth_limit || task->frame_count >= SVM_MAX_FRAMES) {
        SvmFrame *caller = &task->frames[task->frame_count - 1];
        return runtime_error(vm, caller, "call depth limit exceeded");
    }
    const SvmFunction *function = &vm->module->functions[function_index];
    SvmFrame *frame = &task->frames[task->frame_count++];
    memset(frame, 0, sizeof(*frame));
    frame->function = function;
    frame->function_index = function_index;
    for (uint32_t i = 0; i < argument_count; i++) {
        frame->locals[i] = arguments[i];
    }
    return true;
}

static bool push_call_frame(
    Svm *vm,
    uint32_t function_index,
    const SvmValue *arguments,
    uint32_t argument_count
) {
    return push_task_frame(
        vm, CURRENT_TASK(vm), function_index, arguments, argument_count
    );
}

typedef enum {
    SVM_FLOW_CONTINUE,
    SVM_FLOW_FINISHED,
    SVM_FLOW_ERROR
} SvmFlow;

static bool push_defer_frame(Svm *vm, SvmValue closure, const char **heap_message) {
    uint32_t function_index;
    uint32_t capture_count;
    const SvmValue *captures;
    if (!svm_heap_closure_info(
            &vm->heap, closure, &function_index, &captures,
            &capture_count, heap_message)) {
        return false;
    }
    if (function_index >= vm->module->function_count) return false;
    const SvmFunction *function = &vm->module->functions[function_index];
    if (capture_count != function->capture_count ||
        function->parameter_count != capture_count ||
        function->result_type != SVM_TYPE_VOID) {
        return false;
    }
    SvmValue arguments[SVM_MAX_LOCALS];
    for (uint32_t i = 0; i < capture_count; i++) arguments[i] = captures[i];
    if (!push_call_frame(vm, function_index, arguments, capture_count)) return false;
    CURRENT_FRAMES(vm)[CURRENT_FRAME_COUNT(vm) - 1].is_defer_call = true;
    return true;
}

static SvmFlow continue_frame_exit(Svm *vm, SvmValue *final_result) {
    for (;;) {
        if (CURRENT_FRAME_COUNT(vm) == 0) return SVM_FLOW_ERROR;
        SvmFrame *frame = &CURRENT_FRAMES(vm)[CURRENT_FRAME_COUNT(vm) - 1];
        if (frame->exit_kind == SVM_EXIT_NONE) return SVM_FLOW_CONTINUE;

        if (frame->defer_count > 0) {
            SvmValue closure = frame->defers[--frame->defer_count];
            const char *heap_message = NULL;
            if (!push_defer_frame(vm, closure, &heap_message)) {
                (void)runtime_heap_error(vm, frame,
                    heap_message == NULL ? "invalid defer closure" : heap_message);
                return SVM_FLOW_ERROR;
            }
            return SVM_FLOW_CONTINUE;
        }

        SvmExitKind kind = frame->exit_kind;
        SvmValue value = frame->exit_value;
        bool was_defer_call = frame->is_defer_call;
        SvmType result_type = frame->function->result_type;
        CURRENT_FRAME_COUNT(vm)--;

        if (was_defer_call) {
            if (CURRENT_FRAME_COUNT(vm) == 0) {
                (void)set_error(vm->error, 0, 0, "orphan defer frame");
                return SVM_FLOW_ERROR;
            }
            SvmFrame *parent = &CURRENT_FRAMES(vm)[CURRENT_FRAME_COUNT(vm) - 1];
            if (kind == SVM_EXIT_THROW) {
                parent->exit_kind = SVM_EXIT_THROW;
                parent->exit_value = value;
            }
            continue;
        }

        if (CURRENT_FRAME_COUNT(vm) == 0) {
            SvmTask *task = CURRENT_TASK(vm);
            if (kind == SVM_EXIT_RETURN) {
                task->state = SVM_TASK_DONE;
                task->result = value;
                if (vm->current_task_index == 0 && final_result != NULL) {
                    *final_result = value;
                }
                return SVM_FLOW_FINISHED;
            }
            task->state = SVM_TASK_FAILED;
            task->failure = value;
            if (vm->current_task_index == 0) {
                (void)set_error(vm->error, 0, 0, "uncaught guest exception");
            }
            return SVM_FLOW_FINISHED;
        }

        SvmFrame *caller = &CURRENT_FRAMES(vm)[CURRENT_FRAME_COUNT(vm) - 1];
        if (kind == SVM_EXIT_RETURN) {
            if (result_type != SVM_TYPE_VOID && !frame_push(vm, caller, value)) {
                return SVM_FLOW_ERROR;
            }
            return SVM_FLOW_CONTINUE;
        }

        const SvmExceptionHandler *handler = find_handler(
            caller->function, caller->pc == 0 ? 0 : caller->pc - 1
        );
        if (caller->exit_kind == SVM_EXIT_NONE && handler != NULL) {
            caller->stack_size = 0;
            if (!frame_push(vm, caller, value)) return SVM_FLOW_ERROR;
            caller->pc = handler->handler_pc;
            return SVM_FLOW_CONTINUE;
        }
        caller->exit_kind = SVM_EXIT_THROW;
        caller->exit_value = value;
    }
}

static SvmFlow begin_throw(
    Svm *vm,
    SvmValue exception,
    uint32_t throw_pc,
    SvmValue *final_result
) {
    SvmFrame *frame = &CURRENT_FRAMES(vm)[CURRENT_FRAME_COUNT(vm) - 1];
    const SvmExceptionHandler *handler = find_handler(frame->function, throw_pc);
    if (frame->exit_kind == SVM_EXIT_NONE && handler != NULL) {
        frame->stack_size = 0;
        if (!frame_push(vm, frame, exception)) return SVM_FLOW_ERROR;
        frame->pc = handler->handler_pc;
        return SVM_FLOW_CONTINUE;
    }
    frame->exit_kind = SVM_EXIT_THROW;
    frame->exit_value = exception;
    return continue_frame_exit(vm, final_result);
}

static SvmValue task_value(uint32_t id) {
    SvmValue value = {.type = SVM_TYPE_TASK};
    value.as.ref = id;
    return value;
}

static SvmValue channel_value(uint32_t id) {
    SvmValue value = {.type = SVM_TYPE_CHANNEL};
    value.as.ref = id;
    return value;
}

static SvmValue zero_value(SvmType type) {
    if (type == SVM_TYPE_I32) return svm_i32(0);
    if (type == SVM_TYPE_BOOL) return svm_bool(false);
    return svm_null();
}

static SvmTask *resolve_task(Svm *vm, SvmValue value) {
    if (value.type != SVM_TYPE_TASK || value.as.ref == 0 ||
        value.as.ref > vm->task_count) return NULL;
    SvmTask *task = &vm->tasks[value.as.ref - 1];
    return task->id == value.as.ref ? task : NULL;
}

static SvmChannel *resolve_channel(
    Svm *vm,
    SvmValue value,
    SvmType expected_type,
    uint32_t *channel_index
) {
    if (value.type != SVM_TYPE_CHANNEL || value.as.ref == 0 ||
        value.as.ref > vm->channel_count) return NULL;
    uint32_t index = value.as.ref - 1;
    SvmChannel *channel = &vm->channels[index];
    if (!channel->used || channel->element_type != expected_type) return NULL;
    if (channel_index != NULL) *channel_index = index;
    return channel;
}

static SvmFrame *task_top_frame(SvmTask *task) {
    return task->frame_count == 0 ? NULL : &task->frames[task->frame_count - 1];
}

static void make_task_runnable(SvmTask *task) {
    task->state = SVM_TASK_RUNNABLE;
    task->wait_kind = SVM_WAIT_NONE;
    task->wait_target = 0;
}

static int32_t find_waiter(Svm *vm, SvmWaitKind kind, uint32_t target) {
    for (uint32_t i = 0; i < vm->task_count; i++) {
        SvmTask *task = &vm->tasks[i];
        if (task->state == SVM_TASK_WAITING && task->wait_kind == kind &&
            task->wait_target == target) return (int32_t)i;
    }
    return -1;
}

static int32_t select_case_for_channel(const SvmTask *task, uint32_t channel_id) {
    for (uint32_t i = 0; i < task->select_count; i++) {
        if (task->select_channels[i] == channel_id) return (int32_t)i;
    }
    return -1;
}

static int32_t find_select_waiter(Svm *vm, uint32_t channel_id, int32_t *case_index) {
    for (uint32_t i = 0; i < vm->task_count; i++) {
        SvmTask *task = &vm->tasks[i];
        if (task->state != SVM_TASK_WAITING || task->wait_kind != SVM_WAIT_SELECT) {
            continue;
        }
        int32_t selected = select_case_for_channel(task, channel_id);
        if (selected >= 0) {
            *case_index = selected;
            return (int32_t)i;
        }
    }
    return -1;
}

static bool deliver_select(
    Svm *vm,
    uint32_t task_index,
    uint32_t case_index,
    SvmValue value
) {
    SvmTask *task = &vm->tasks[task_index];
    SvmFrame *frame = task_top_frame(task);
    if (frame == NULL || !frame_push(vm, frame, svm_i32((int32_t)case_index)) ||
        !frame_push(vm, frame, value)) return false;
    task->select_cursor = (case_index + 1u) % task->select_count;
    task->select_count = 0;
    make_task_runnable(task);
    return true;
}

static bool channel_try_send(
    Svm *vm,
    uint32_t channel_index,
    SvmValue value,
    bool *sent
) {
    SvmChannel *channel = &vm->channels[channel_index];
    *sent = false;
    if (channel->closed) return true;
    uint32_t channel_id = channel_index + 1u;

    int32_t receiver_index = find_waiter(vm, SVM_WAIT_RECV, channel_id);
    if (receiver_index >= 0) {
        SvmTask *receiver = &vm->tasks[receiver_index];
        SvmFrame *frame = task_top_frame(receiver);
        if (frame == NULL || !frame_push(vm, frame, value)) return false;
        make_task_runnable(receiver);
        *sent = true;
        return true;
    }

    int32_t case_index;
    int32_t select_index = find_select_waiter(vm, channel_id, &case_index);
    if (select_index >= 0) {
        if (!deliver_select(vm, (uint32_t)select_index, (uint32_t)case_index, value)) {
            return false;
        }
        *sent = true;
        return true;
    }

    if (channel->count < channel->capacity) {
        channel->buffer[channel->write_pos] = value;
        channel->write_pos = (channel->write_pos + 1u) % channel->capacity;
        channel->count++;
        *sent = true;
    }
    return true;
}

static bool channel_try_receive(
    Svm *vm,
    uint32_t channel_index,
    SvmValue *value,
    bool *received
) {
    SvmChannel *channel = &vm->channels[channel_index];
    *received = false;
    uint32_t channel_id = channel_index + 1u;

    if (channel->count > 0) {
        *value = channel->buffer[channel->read_pos];
        channel->read_pos = (channel->read_pos + 1u) % channel->capacity;
        channel->count--;
        int32_t sender_index = find_waiter(vm, SVM_WAIT_SEND, channel_id);
        if (sender_index >= 0 && !channel->closed) {
            SvmTask *sender = &vm->tasks[sender_index];
            channel->buffer[channel->write_pos] = sender->wait_value;
            channel->write_pos = (channel->write_pos + 1u) % channel->capacity;
            channel->count++;
            make_task_runnable(sender);
        }
        *received = true;
        return true;
    }

    int32_t sender_index = find_waiter(vm, SVM_WAIT_SEND, channel_id);
    if (sender_index >= 0 && !channel->closed) {
        SvmTask *sender = &vm->tasks[sender_index];
        *value = sender->wait_value;
        make_task_runnable(sender);
        *received = true;
        return true;
    }
    if (channel->closed) {
        *value = zero_value(channel->element_type);
        *received = true;
    }
    return true;
}

static bool throw_into_task(Svm *vm, uint32_t task_index, SvmValue exception) {
    uint32_t previous = vm->current_task_index;
    vm->current_task_index = task_index;
    SvmTask *task = &vm->tasks[task_index];
    task->state = SVM_TASK_RUNNABLE;
    SvmFrame *frame = task_top_frame(task);
    SvmFlow flow = frame == NULL ? SVM_FLOW_ERROR : begin_throw(
        vm, exception, frame->pc == 0 ? 0 : frame->pc - 1, NULL
    );
    if (flow == SVM_FLOW_CONTINUE) task->state = SVM_TASK_RUNNABLE;
    vm->current_task_index = previous;
    return flow != SVM_FLOW_ERROR;
}

static bool wake_awaiters(Svm *vm, uint32_t completed_index) {
    SvmTask *completed = &vm->tasks[completed_index];
    for (uint32_t i = 0; i < vm->task_count; i++) {
        SvmTask *waiter = &vm->tasks[i];
        if (waiter->state != SVM_TASK_WAITING ||
            waiter->wait_kind != SVM_WAIT_TASK ||
            waiter->wait_target != completed->id) continue;
        if (completed->state == SVM_TASK_DONE) {
            SvmType result_type;
            if (!await_key_result_type(
                    vm->module, completed->await_key, &result_type)) return false;
            SvmFrame *frame = task_top_frame(waiter);
            if (result_type != SVM_TYPE_VOID &&
                (frame == NULL || !frame_push(vm, frame, completed->result))) return false;
            make_task_runnable(waiter);
        } else if (completed->state == SVM_TASK_FAILED) {
            waiter->wait_kind = SVM_WAIT_NONE;
            if (!throw_into_task(vm, i, completed->failure)) return false;
        }
    }
    return true;
}

static bool close_channel(Svm *vm, uint32_t channel_index) {
    SvmChannel *channel = &vm->channels[channel_index];
    if (channel->closed) return true;
    channel->closed = true;
    uint32_t channel_id = channel_index + 1u;
    for (uint32_t i = 0; i < vm->task_count; i++) {
        SvmTask *task = &vm->tasks[i];
        if (task->state != SVM_TASK_WAITING) continue;
        if (task->wait_kind == SVM_WAIT_RECV && task->wait_target == channel_id) {
            SvmFrame *frame = task_top_frame(task);
            if (frame == NULL || !frame_push(vm, frame, zero_value(channel->element_type))) {
                return false;
            }
            make_task_runnable(task);
        } else if (task->wait_kind == SVM_WAIT_SELECT) {
            int32_t case_index = select_case_for_channel(task, channel_id);
            if (case_index >= 0 &&
                !deliver_select(vm, i, (uint32_t)case_index,
                                zero_value(channel->element_type))) {
                return false;
            }
        } else if (task->wait_kind == SVM_WAIT_SEND &&
                   task->wait_target == channel_id) {
            task->wait_kind = SVM_WAIT_NONE;
            if (!throw_into_task(vm, i, svm_null())) return false;
        }
    }
    return true;
}

static int32_t next_runnable_task(Svm *vm) {
    for (uint32_t offset = 1; offset <= vm->task_count; offset++) {
        uint32_t index = (vm->current_task_index + offset) % vm->task_count;
        if (vm->tasks[index].state == SVM_TASK_RUNNABLE) return (int32_t)index;
    }
    return -1;
}

static bool host_signatures_match(
    const SvmHostImport *import,
    const SvmHostFunction *function
) {
    if (function->parameter_count != import->parameter_count ||
        function->result_type != import->result_type ||
        function->invoke == NULL || function->cost == 0 ||
        (import->asynchronous && function->poll == NULL)) return false;
    for (uint32_t i = 0; i < import->parameter_count; i++) {
        if (function->parameter_types == NULL ||
            function->parameter_types[i] != import->parameter_types[i]) return false;
    }
    return true;
}

static bool bind_capabilities(
    Svm *vm,
    const SvmCapability *capabilities,
    uint32_t capability_count
) {
    for (uint32_t import_index = 0;
         import_index < vm->module->host_import_count; import_index++) {
        const SvmHostImport *import = &vm->module->host_imports[import_index];
        const SvmCapability *matched_capability = NULL;
        for (uint32_t capability_index = 0;
             capability_index < capability_count; capability_index++) {
            const SvmCapability *candidate = &capabilities[capability_index];
            if (candidate->name != NULL &&
                strcmp(candidate->name, import->capability_name) == 0) {
                matched_capability = candidate;
                break;
            }
        }
        if (matched_capability == NULL) {
            return set_error(vm->error, 0, 0, "required capability is not granted");
        }
        const SvmHostFunction *matched_function = NULL;
        for (uint32_t function_index = 0;
             function_index < matched_capability->function_count; function_index++) {
            const SvmHostFunction *candidate = &matched_capability->functions[function_index];
            if (candidate->name != NULL &&
                strcmp(candidate->name, import->function_name) == 0) {
                matched_function = candidate;
                break;
            }
        }
        if (matched_function == NULL ||
            !host_signatures_match(import, matched_function)) {
            return set_error(vm->error, 0, 0, "capability function signature mismatch");
        }
        vm->bound_host[import_index].function = matched_function;
        vm->bound_host[import_index].context = matched_capability->context;
    }
    return true;
}

static bool charge_host_budget(Svm *vm, uint64_t cost) {
    if (cost > vm->host_budget) {
        return set_error(vm->error, 0, 0, "host call budget exhausted");
    }
    vm->host_budget -= cost;
    return true;
}

static bool host_result_type_matches(SvmType expected, SvmValue value) {
    return expected == SVM_TYPE_VOID || value.type == expected;
}

static bool poll_host_tasks(Svm *vm, bool *has_pending) {
    *has_pending = false;
    for (uint32_t task_index = 0; task_index < vm->task_count; task_index++) {
        SvmTask *task = &vm->tasks[task_index];
        if (!task->is_host_task || task->state != SVM_TASK_WAITING ||
            task->wait_kind != SVM_WAIT_HOST) continue;
        *has_pending = true;
        SvmBoundHostFunction *bound = &vm->bound_host[task->host_import_index];
        if (!charge_host_budget(vm, bound->function->cost)) return false;
        SvmValue value = svm_null();
        SvmHostStatus status = bound->function->poll(
            bound->context, task->host_token, &value
        );
        if (status == SVM_HOST_PENDING) continue;
        if (status == SVM_HOST_DONE) {
            if (!host_result_type_matches(bound->function->result_type, value)) {
                return set_error(vm->error, 0, 0, "host returned wrong value type");
            }
            task->state = SVM_TASK_DONE;
            task->wait_kind = SVM_WAIT_NONE;
            task->result = value;
        } else {
            task->state = SVM_TASK_FAILED;
            task->wait_kind = SVM_WAIT_NONE;
            task->failure = value;
        }
        if (!wake_awaiters(vm, task_index)) return false;
    }
    return true;
}

static int32_t wrapping_add(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left + (uint32_t)right);
}

static int32_t wrapping_sub(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left - (uint32_t)right);
}

static int32_t wrapping_mul(int32_t left, int32_t right) {
    return (int32_t)((uint32_t)left * (uint32_t)right);
}

bool svm_execute_with_capabilities(
    const SvmModule *module,
    uint32_t entry_function,
    const SvmValue *arguments,
    uint32_t argument_count,
    SvmLimits limits,
    const SvmCapability *capabilities,
    uint32_t capability_count,
    SvmValue *result,
    SvmError *error
) {
    if (!svm_verify_module(module, error)) return false;
    if (capability_count > 0 && capabilities == NULL) {
        return set_error(error, 0, 0, "missing capability table");
    }
    if (entry_function >= module->function_count) {
        return set_error(error, entry_function, 0, "entry function out of bounds");
    }
    const SvmFunction *entry = &module->functions[entry_function];
    if (entry->capture_count != 0) {
        return set_error(error, entry_function, 0, "closure body cannot be an entry function");
    }
    if (argument_count != entry->parameter_count) {
        return set_error(error, entry_function, 0, "wrong entry argument count");
    }
    for (uint32_t i = 0; i < argument_count; i++) {
        if (arguments[i].type != entry->parameter_types[i]) {
            return set_error(error, entry_function, 0, "wrong entry argument type");
        }
    }

    Svm vm;
    memset(&vm, 0, sizeof(vm));
    vm.module = module;
    vm.budget = limits.instruction_budget;
    vm.depth_limit = limits.call_depth_limit == 0
        ? SVM_MAX_FRAMES : limits.call_depth_limit;
    vm.error = error;
    vm.task_count = 1;
    vm.current_task_index = 0;
    vm.tasks[0].id = 1;
    vm.tasks[0].state = SVM_TASK_RUNNABLE;
    vm.tasks[0].entry_function = entry_function;
    vm.tasks[0].await_key = (int32_t)entry_function;
    vm.task_limit = limits.task_limit == 0 ? SVM_MAX_TASKS : limits.task_limit;
    if (vm.task_limit > SVM_MAX_TASKS) vm.task_limit = SVM_MAX_TASKS;
    vm.channel_limit = limits.channel_limit == 0
        ? SVM_MAX_CHANNELS : limits.channel_limit;
    if (vm.channel_limit > SVM_MAX_CHANNELS) vm.channel_limit = SVM_MAX_CHANNELS;
    vm.scheduler_quantum = limits.scheduler_quantum == 0
        ? 64u : limits.scheduler_quantum;
    vm.host_budget = limits.host_call_budget == 0
        ? UINT64_MAX : limits.host_call_budget;
    size_t heap_limit = limits.heap_byte_limit == 0 ? 1024u * 1024u : limits.heap_byte_limit;
    uint32_t handle_limit = limits.handle_limit == 0 ? 4096u : limits.handle_limit;
    if (!svm_heap_init(&vm.heap, heap_limit, handle_limit)) {
        return set_error(error, entry_function, 0, "failed to initialize managed heap");
    }
#define SVM_EXEC_FAIL() do { svm_heap_destroy(&vm.heap); return false; } while (0)
    if (!bind_capabilities(&vm, capabilities, capability_count)) SVM_EXEC_FAIL();
    if (!push_call_frame(&vm, entry_function, arguments, argument_count)) SVM_EXEC_FAIL();

    for (;;) {
        if (vm.tasks[0].state == SVM_TASK_DONE) {
            if (result != NULL) *result = vm.tasks[0].result;
            svm_heap_destroy(&vm.heap);
            return true;
        }
        if (vm.tasks[0].state == SVM_TASK_FAILED ||
            vm.tasks[0].state == SVM_TASK_CANCELLED) {
            if (error != NULL && error->message[0] == '\0') {
                (void)set_error(error, 0, 0, "main task failed");
            }
            SVM_EXEC_FAIL();
        }
        bool host_pending = false;
        if (!poll_host_tasks(&vm, &host_pending)) SVM_EXEC_FAIL();
        if (CURRENT_TASK(&vm)->state != SVM_TASK_RUNNING) {
            int32_t next = next_runnable_task(&vm);
            if (next < 0) {
                if (host_pending) continue;
                (void)set_error(error, 0, 0, "scheduler deadlock: no runnable task");
                SVM_EXEC_FAIL();
            }
            vm.current_task_index = (uint32_t)next;
            CURRENT_TASK(&vm)->state = SVM_TASK_RUNNING;
            CURRENT_TASK(&vm)->slice_remaining = vm.scheduler_quantum;
        }
        SvmFrame *frame = &CURRENT_FRAMES(&vm)[CURRENT_FRAME_COUNT(&vm) - 1];
        if (vm.budget == 0) {
            (void)runtime_error(&vm, frame, "instruction budget exhausted");
            SVM_EXEC_FAIL();
        }
        vm.budget--;
        if (frame->pc >= frame->function->code_count) {
            (void)runtime_error(&vm, frame, "program counter out of bounds");
            SVM_EXEC_FAIL();
        }

        SvmInstruction instruction = frame->function->code[frame->pc++];
        SvmValue left = svm_null();
        SvmValue right = svm_null();
        const char *heap_message = NULL;
        switch (instruction.opcode) {
        case SVM_OP_CONST_I32:
            if (!frame_push(&vm, frame, svm_i32(instruction.operand))) SVM_EXEC_FAIL();
            break;
        case SVM_OP_CONST_FALSE:
        case SVM_OP_CONST_TRUE:
            if (!frame_push(
                    &vm, frame, svm_bool(instruction.opcode == SVM_OP_CONST_TRUE))) {
                SVM_EXEC_FAIL();
            }
            break;
        case SVM_OP_CONST_NULL:
            if (!frame_push(&vm, frame, svm_null())) SVM_EXEC_FAIL();
            break;
        case SVM_OP_LOAD_LOCAL:
            if (!frame_push(&vm, frame, frame->locals[instruction.operand])) SVM_EXEC_FAIL();
            break;
        case SVM_OP_STORE_LOCAL:
            if (!frame_pop(&vm, frame, &frame->locals[instruction.operand])) SVM_EXEC_FAIL();
            break;
        case SVM_OP_DUP:
            if (!frame_pop(&vm, frame, &left) ||
                !frame_push(&vm, frame, left) || !frame_push(&vm, frame, left)) {
                SVM_EXEC_FAIL();
            }
            break;
        case SVM_OP_POP:
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            break;
        case SVM_OP_I32_ADD:
        case SVM_OP_I32_SUB:
        case SVM_OP_I32_MUL:
            if (!frame_pop(&vm, frame, &right) || !frame_pop(&vm, frame, &left)) {
                SVM_EXEC_FAIL();
            }
            if (instruction.opcode == SVM_OP_I32_ADD) {
                left.as.i32 = wrapping_add(left.as.i32, right.as.i32);
            } else if (instruction.opcode == SVM_OP_I32_SUB) {
                left.as.i32 = wrapping_sub(left.as.i32, right.as.i32);
            } else {
                left.as.i32 = wrapping_mul(left.as.i32, right.as.i32);
            }
            if (!frame_push(&vm, frame, left)) SVM_EXEC_FAIL();
            break;
        case SVM_OP_I32_LE:
        case SVM_OP_I32_EQ:
            if (!frame_pop(&vm, frame, &right) || !frame_pop(&vm, frame, &left)) {
                SVM_EXEC_FAIL();
            }
            if (!frame_push(
                    &vm, frame,
                    svm_bool(instruction.opcode == SVM_OP_I32_LE
                        ? left.as.i32 <= right.as.i32
                        : left.as.i32 == right.as.i32))) {
                SVM_EXEC_FAIL();
            }
            break;
        case SVM_OP_JUMP:
            frame->pc = (uint32_t)instruction.operand;
            break;
        case SVM_OP_JUMP_IF_FALSE:
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            if (!left.as.boolean) frame->pc = (uint32_t)instruction.operand;
            break;
        case SVM_OP_CALL: {
            uint32_t target_index = (uint32_t)instruction.operand;
            const SvmFunction *target = &module->functions[target_index];
            SvmValue call_arguments[SVM_MAX_LOCALS];
            for (uint32_t i = target->parameter_count; i > 0; i--) {
                if (!frame_pop(&vm, frame, &call_arguments[i - 1])) SVM_EXEC_FAIL();
            }
            if (!push_call_frame(
                    &vm, target_index, call_arguments, target->parameter_count)) {
                SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_NEW_OBJECT: {
            const SvmRecordType *record = &module->record_types[instruction.operand];
            collect_vm_roots(&vm);
            if (!svm_heap_new_instance(
                    &vm.heap, (uint32_t)instruction.operand, record->field_types,
                    record->field_count, &left, &heap_message) ||
                !frame_push(&vm, frame, left)) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_GET_FIELD:
            if (!frame_pop(&vm, frame, &left) ||
                !svm_heap_get_field(
                    &vm.heap, left, (uint32_t)instruction.operand,
                    (uint32_t)instruction.operand2, &right, &heap_message) ||
                !frame_push(&vm, frame, right)) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                SVM_EXEC_FAIL();
            }
            break;
        case SVM_OP_SET_FIELD:
            if (!frame_pop(&vm, frame, &right) || !frame_pop(&vm, frame, &left) ||
                !svm_heap_set_field(
                    &vm.heap, left, (uint32_t)instruction.operand,
                    (uint32_t)instruction.operand2, right, &heap_message)) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                SVM_EXEC_FAIL();
            }
            break;
        case SVM_OP_NEW_ARRAY:
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            if (left.as.i32 < 0) {
                (void)runtime_error(&vm, frame, "negative array length");
                SVM_EXEC_FAIL();
            }
            collect_vm_roots(&vm);
            if (!svm_heap_new_array(
                    &vm.heap, (SvmType)instruction.operand, (uint32_t)left.as.i32,
                    &right, &heap_message) || !frame_push(&vm, frame, right)) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                SVM_EXEC_FAIL();
            }
            break;
        case SVM_OP_ARRAY_LEN: {
            uint32_t length;
            if (!frame_pop(&vm, frame, &left) ||
                !svm_heap_array_length(
                    &vm.heap, left, (SvmType)instruction.operand,
                    &length, &heap_message) ||
                !frame_push(&vm, frame, svm_i32((int32_t)length))) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_ARRAY_GET:
            if (!frame_pop(&vm, frame, &right) || !frame_pop(&vm, frame, &left) ||
                right.as.i32 < 0 ||
                !svm_heap_array_get(
                    &vm.heap, left, (SvmType)instruction.operand,
                    (uint32_t)right.as.i32, &right, &heap_message) ||
                !frame_push(&vm, frame, right)) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                else (void)runtime_error(&vm, frame, "negative array index");
                SVM_EXEC_FAIL();
            }
            break;
        case SVM_OP_ARRAY_SET: {
            SvmValue value = svm_null();
            if (!frame_pop(&vm, frame, &value) || !frame_pop(&vm, frame, &right) ||
                !frame_pop(&vm, frame, &left) || right.as.i32 < 0 ||
                !svm_heap_array_set(
                    &vm.heap, left, (SvmType)instruction.operand,
                    (uint32_t)right.as.i32, value, &heap_message)) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                else (void)runtime_error(&vm, frame, "negative array index");
                SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_NEW_CLOSURE: {
            uint32_t capture_count = (uint32_t)instruction.operand2;
            SvmValue captures[SVM_MAX_LOCALS];
            collect_vm_roots(&vm);
            for (uint32_t i = capture_count; i > 0; i--) {
                if (!frame_pop(&vm, frame, &captures[i - 1])) SVM_EXEC_FAIL();
            }
            if (!svm_heap_new_closure(
                    &vm.heap, (uint32_t)instruction.operand, captures,
                    capture_count, &left, &heap_message) ||
                !frame_push(&vm, frame, left)) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_CALL_CLOSURE: {
            const SvmFunction *target = &module->functions[instruction.operand];
            uint32_t explicit_count = target->parameter_count - target->capture_count;
            SvmValue call_arguments[SVM_MAX_LOCALS];
            for (uint32_t i = explicit_count; i > 0; i--) {
                if (!frame_pop(&vm, frame,
                               &call_arguments[target->capture_count + i - 1])) {
                    SVM_EXEC_FAIL();
                }
            }
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            uint32_t actual_target;
            uint32_t capture_count;
            const SvmValue *captures;
            if (!svm_heap_closure_info(
                    &vm.heap, left, &actual_target, &captures,
                    &capture_count, &heap_message) ||
                actual_target != (uint32_t)instruction.operand ||
                capture_count != target->capture_count) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                else (void)runtime_error(&vm, frame, "closure target mismatch");
                SVM_EXEC_FAIL();
            }
            for (uint32_t i = 0; i < capture_count; i++) call_arguments[i] = captures[i];
            if (!push_call_frame(
                    &vm, actual_target, call_arguments, target->parameter_count)) {
                SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_GC_COLLECT:
            collect_vm_roots(&vm);
            break;
        case SVM_OP_DEFER_PUSH: {
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            if (frame->defer_count >= SVM_MAX_DEFERS) {
                (void)runtime_error(&vm, frame, "defer stack limit exceeded");
                SVM_EXEC_FAIL();
            }
            uint32_t target_index;
            uint32_t capture_count;
            const SvmValue *captures;
            if (!svm_heap_closure_info(
                    &vm.heap, left, &target_index, &captures,
                    &capture_count, &heap_message) ||
                target_index != (uint32_t)instruction.operand) {
                if (heap_message != NULL) (void)runtime_heap_error(&vm, frame, heap_message);
                else (void)runtime_error(&vm, frame, "defer closure target mismatch");
                SVM_EXEC_FAIL();
            }
            frame->defers[frame->defer_count++] = left;
            break;
        }
        case SVM_OP_THROW: {
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            SvmFlow flow = begin_throw(
                &vm, left, frame->pc == 0 ? 0 : frame->pc - 1, result
            );
            if (flow == SVM_FLOW_ERROR) SVM_EXEC_FAIL();
            if (flow == SVM_FLOW_FINISHED) {
                if (!wake_awaiters(&vm, vm.current_task_index)) SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_TASK_SPAWN: {
            uint32_t target_index = (uint32_t)instruction.operand;
            const SvmFunction *target = &module->functions[target_index];
            SvmValue task_arguments[SVM_MAX_LOCALS];
            for (uint32_t i = target->parameter_count; i > 0; i--) {
                if (!frame_pop(&vm, frame, &task_arguments[i - 1])) SVM_EXEC_FAIL();
            }
            if (vm.task_count >= vm.task_limit) {
                (void)runtime_error(&vm, frame, "task limit exceeded");
                SVM_EXEC_FAIL();
            }
            uint32_t task_index = vm.task_count++;
            SvmTask *task = &vm.tasks[task_index];
            memset(task, 0, sizeof(*task));
            task->id = task_index + 1u;
            task->state = SVM_TASK_RUNNABLE;
            task->entry_function = target_index;
            task->await_key = (int32_t)target_index;
            if (!push_task_frame(
                    &vm, task, target_index, task_arguments,
                    target->parameter_count) ||
                !frame_push(&vm, frame, task_value(task->id))) {
                SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_TASK_AWAIT: {
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            SvmTask *target = resolve_task(&vm, left);
            if (target == NULL || target->await_key != instruction.operand ||
                target == CURRENT_TASK(&vm)) {
                (void)runtime_error(&vm, frame, "invalid task handle for await");
                SVM_EXEC_FAIL();
            }
            if (target->state == SVM_TASK_DONE) {
                SvmType target_result_type;
                if (!await_key_result_type(
                        module, target->await_key, &target_result_type)) SVM_EXEC_FAIL();
                if (target_result_type != SVM_TYPE_VOID &&
                    !frame_push(&vm, frame, target->result)) SVM_EXEC_FAIL();
            } else if (target->state == SVM_TASK_FAILED) {
                SvmFlow flow = begin_throw(
                    &vm, target->failure, frame->pc == 0 ? 0 : frame->pc - 1, result
                );
                if (flow == SVM_FLOW_ERROR) SVM_EXEC_FAIL();
                if (flow == SVM_FLOW_FINISHED &&
                    !wake_awaiters(&vm, vm.current_task_index)) SVM_EXEC_FAIL();
            } else {
                CURRENT_TASK(&vm)->state = SVM_TASK_WAITING;
                CURRENT_TASK(&vm)->wait_kind = SVM_WAIT_TASK;
                CURRENT_TASK(&vm)->wait_target = target->id;
            }
            break;
        }
        case SVM_OP_TASK_YIELD:
            CURRENT_TASK(&vm)->state = SVM_TASK_RUNNABLE;
            break;
        case SVM_OP_CHANNEL_NEW: {
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            if (left.as.i32 < 0 || left.as.i32 > (int32_t)SVM_MAX_CHANNEL_CAPACITY) {
                (void)runtime_error(&vm, frame, "invalid channel capacity");
                SVM_EXEC_FAIL();
            }
            if (vm.channel_count >= vm.channel_limit) {
                (void)runtime_error(&vm, frame, "channel limit exceeded");
                SVM_EXEC_FAIL();
            }
            uint32_t channel_index = vm.channel_count++;
            SvmChannel *channel = &vm.channels[channel_index];
            memset(channel, 0, sizeof(*channel));
            channel->used = true;
            channel->element_type = (SvmType)instruction.operand;
            channel->capacity = (uint32_t)left.as.i32;
            if (!frame_push(&vm, frame, channel_value(channel_index + 1u))) {
                SVM_EXEC_FAIL();
            }
            break;
        }
        case SVM_OP_CHANNEL_SEND: {
            if (!frame_pop(&vm, frame, &right) || !frame_pop(&vm, frame, &left)) {
                SVM_EXEC_FAIL();
            }
            uint32_t channel_index;
            SvmChannel *channel = resolve_channel(
                &vm, left, (SvmType)instruction.operand, &channel_index
            );
            if (channel == NULL) {
                (void)runtime_error(&vm, frame, "invalid channel handle for send");
                SVM_EXEC_FAIL();
            }
            if (channel->closed) {
                SvmFlow flow = begin_throw(
                    &vm, svm_null(), frame->pc == 0 ? 0 : frame->pc - 1, result
                );
                if (flow == SVM_FLOW_ERROR) SVM_EXEC_FAIL();
                if (flow == SVM_FLOW_FINISHED &&
                    !wake_awaiters(&vm, vm.current_task_index)) SVM_EXEC_FAIL();
                break;
            }
            bool sent;
            if (!channel_try_send(&vm, channel_index, right, &sent)) SVM_EXEC_FAIL();
            if (!sent) {
                CURRENT_TASK(&vm)->state = SVM_TASK_WAITING;
                CURRENT_TASK(&vm)->wait_kind = SVM_WAIT_SEND;
                CURRENT_TASK(&vm)->wait_target = channel_index + 1u;
                CURRENT_TASK(&vm)->wait_value = right;
            }
            break;
        }
        case SVM_OP_CHANNEL_RECV: {
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            uint32_t channel_index;
            if (resolve_channel(
                    &vm, left, (SvmType)instruction.operand, &channel_index) == NULL) {
                (void)runtime_error(&vm, frame, "invalid channel handle for receive");
                SVM_EXEC_FAIL();
            }
            bool received;
            if (!channel_try_receive(&vm, channel_index, &right, &received)) {
                SVM_EXEC_FAIL();
            }
            if (received) {
                if (!frame_push(&vm, frame, right)) SVM_EXEC_FAIL();
            } else {
                CURRENT_TASK(&vm)->state = SVM_TASK_WAITING;
                CURRENT_TASK(&vm)->wait_kind = SVM_WAIT_RECV;
                CURRENT_TASK(&vm)->wait_target = channel_index + 1u;
            }
            break;
        }
        case SVM_OP_CHANNEL_CLOSE: {
            if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
            uint32_t channel_index;
            if (left.type != SVM_TYPE_CHANNEL || left.as.ref == 0 ||
                left.as.ref > vm.channel_count) {
                (void)runtime_error(&vm, frame, "invalid channel handle for close");
                SVM_EXEC_FAIL();
            }
            channel_index = left.as.ref - 1u;
            if (!close_channel(&vm, channel_index)) SVM_EXEC_FAIL();
            break;
        }
        case SVM_OP_CHANNEL_SELECT: {
            uint32_t case_count = (uint32_t)instruction.operand;
            SvmTask *task = CURRENT_TASK(&vm);
            task->select_count = case_count;
            for (uint32_t i = case_count; i > 0; i--) {
                if (!frame_pop(&vm, frame, &left)) SVM_EXEC_FAIL();
                uint32_t channel_index;
                if (resolve_channel(
                        &vm, left, (SvmType)instruction.operand2,
                        &channel_index) == NULL) {
                    (void)runtime_error(&vm, frame, "invalid channel in select");
                    SVM_EXEC_FAIL();
                }
                task->select_channels[i - 1] = channel_index + 1u;
            }
            bool selected = false;
            for (uint32_t offset = 0; offset < case_count && !selected; offset++) {
                uint32_t selected_case = (task->select_cursor + offset) % case_count;
                uint32_t channel_index = task->select_channels[selected_case] - 1u;
                bool received;
                if (!channel_try_receive(&vm, channel_index, &right, &received)) {
                    SVM_EXEC_FAIL();
                }
                if (received) {
                    if (!frame_push(&vm, frame, svm_i32((int32_t)selected_case)) ||
                        !frame_push(&vm, frame, right)) SVM_EXEC_FAIL();
                    task->select_cursor = (selected_case + 1u) % case_count;
                    task->select_count = 0;
                    selected = true;
                }
            }
            if (!selected) {
                task->state = SVM_TASK_WAITING;
                task->wait_kind = SVM_WAIT_SELECT;
            }
            break;
        }
        case SVM_OP_HOST_CALL:
        case SVM_OP_HOST_CALL_ASYNC: {
            uint32_t import_index = (uint32_t)instruction.operand;
            const SvmHostImport *import = &module->host_imports[import_index];
            SvmBoundHostFunction *bound = &vm.bound_host[import_index];
            SvmValue host_arguments[SVM_MAX_LOCALS];
            for (uint32_t i = import->parameter_count; i > 0; i--) {
                if (!frame_pop(&vm, frame, &host_arguments[i - 1])) SVM_EXEC_FAIL();
            }
            bool asynchronous = instruction.opcode == SVM_OP_HOST_CALL_ASYNC;
            if (asynchronous && vm.task_count >= vm.task_limit) {
                (void)runtime_error(&vm, frame, "task limit exceeded");
                SVM_EXEC_FAIL();
            }
            if (!charge_host_budget(&vm, bound->function->cost)) SVM_EXEC_FAIL();
            SvmValue host_value = svm_null();
            uint64_t pending_token = 0;
            SvmHostStatus status = bound->function->invoke(
                bound->context, host_arguments, import->parameter_count,
                &host_value, &pending_token
            );
            if (!asynchronous) {
                if (status == SVM_HOST_PENDING) {
                    (void)runtime_error(&vm, frame,
                                        "synchronous host call returned pending");
                    SVM_EXEC_FAIL();
                }
                if (status == SVM_HOST_FAILED) {
                    SvmFlow flow = begin_throw(
                        &vm, host_value, frame->pc == 0 ? 0 : frame->pc - 1, result
                    );
                    if (flow == SVM_FLOW_ERROR) SVM_EXEC_FAIL();
                    if (flow == SVM_FLOW_FINISHED &&
                        !wake_awaiters(&vm, vm.current_task_index)) SVM_EXEC_FAIL();
                } else {
                    if (!host_result_type_matches(import->result_type, host_value)) {
                        (void)runtime_error(&vm, frame, "host returned wrong value type");
                        SVM_EXEC_FAIL();
                    }
                    if (import->result_type != SVM_TYPE_VOID &&
                        !frame_push(&vm, frame, host_value)) SVM_EXEC_FAIL();
                }
                break;
            }

            uint32_t task_index = vm.task_count++;
            SvmTask *host_task = &vm.tasks[task_index];
            memset(host_task, 0, sizeof(*host_task));
            host_task->id = task_index + 1u;
            host_task->is_host_task = true;
            host_task->host_import_index = import_index;
            host_task->await_key = -(int32_t)(import_index + 1u);
            if (status == SVM_HOST_DONE) {
                if (!host_result_type_matches(import->result_type, host_value)) {
                    (void)runtime_error(&vm, frame, "host returned wrong value type");
                    SVM_EXEC_FAIL();
                }
                host_task->state = SVM_TASK_DONE;
                host_task->result = host_value;
            } else if (status == SVM_HOST_FAILED) {
                host_task->state = SVM_TASK_FAILED;
                host_task->failure = host_value;
            } else {
                host_task->state = SVM_TASK_WAITING;
                host_task->wait_kind = SVM_WAIT_HOST;
                host_task->host_token = pending_token;
            }
            if (!frame_push(&vm, frame, task_value(host_task->id))) SVM_EXEC_FAIL();
            break;
        }
        case SVM_OP_RETURN: {
            left = svm_null();
            if (frame->function->result_type != SVM_TYPE_VOID &&
                !frame_pop(&vm, frame, &left)) {
                SVM_EXEC_FAIL();
            }
            frame->exit_kind = SVM_EXIT_RETURN;
            frame->exit_value = left;
            SvmFlow flow = continue_frame_exit(&vm, result);
            if (flow == SVM_FLOW_ERROR) SVM_EXEC_FAIL();
            if (flow == SVM_FLOW_FINISHED) {
                if (!wake_awaiters(&vm, vm.current_task_index)) SVM_EXEC_FAIL();
            }
            break;
        }
        default:
            (void)runtime_error(&vm, frame, "unknown opcode after verification");
            SVM_EXEC_FAIL();
        }
        if (CURRENT_TASK(&vm)->state == SVM_TASK_RUNNING) {
            if (CURRENT_TASK(&vm)->slice_remaining > 0) {
                CURRENT_TASK(&vm)->slice_remaining--;
            }
            if (CURRENT_TASK(&vm)->slice_remaining == 0) {
                CURRENT_TASK(&vm)->state = SVM_TASK_RUNNABLE;
            }
        }
    }
#undef SVM_EXEC_FAIL
}

bool svm_execute(
    const SvmModule *module,
    uint32_t entry_function,
    const SvmValue *arguments,
    uint32_t argument_count,
    SvmLimits limits,
    SvmValue *result,
    SvmError *error
) {
    return svm_execute_with_capabilities(
        module, entry_function, arguments, argument_count, limits,
        NULL, 0, result, error
    );
}
