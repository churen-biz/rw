#include "sc_builder.h"

#include <stdlib.h>
#include <string.h>

enum { SC_BUILDER_INS_CAP = 64 };

typedef struct {
    char *name;
    SvmInstruction *code;
    uint32_t code_count;
    uint32_t code_capacity;
    uint32_t max_stack;
    int32_t stack_depth;
} ScFuncDraft;

struct ScBuilder {
    ScArena *arena;
    ScFuncDraft *funcs;
    uint32_t func_count;
    uint32_t func_capacity;
    ScFuncDraft current;
    bool in_func;
    SvmFunction *finished;
};

static ScSpan no_span(void) {
    ScSpan span = {0};
    return span;
}

ScBuilder *sc_builder_create(ScArena *arena) {
    ScBuilder *b = sc_arena_alloc(arena, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    memset(b, 0, sizeof(*b));
    b->arena = arena;
    return b;
}

bool sc_builder_begin_func(ScBuilder *b, const char *name, ScError *err) {
    if (b->in_func) {
        sc_error_set(err, no_span(), "nested function is not supported");
        return false;
    }
    memset(&b->current, 0, sizeof(b->current));
    b->current.name = sc_arena_strndup(b->arena, name, strlen(name));
    if (b->current.name == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    b->current.code_capacity = SC_BUILDER_INS_CAP;
    b->current.code = calloc(b->current.code_capacity, sizeof(SvmInstruction));
    if (b->current.code == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    b->in_func = true;
    return true;
}

static int32_t stack_delta(SvmOpcode op) {
    switch (op) {
        case SVM_OP_CONST_I32:
        case SVM_OP_CONST_FALSE:
        case SVM_OP_CONST_TRUE:
        case SVM_OP_CONST_NULL:
            return 1;
        case SVM_OP_I32_ADD:
        case SVM_OP_I32_SUB:
        case SVM_OP_I32_MUL:
        case SVM_OP_I32_LE:
        case SVM_OP_I32_EQ:
            return -1;
        case SVM_OP_RETURN:
        case SVM_OP_JUMP:
            return 0;
        case SVM_OP_JUMP_IF_FALSE:
        case SVM_OP_POP:
            return -1;
        case SVM_OP_DUP:
            return 1;
        case SVM_OP_LOAD_LOCAL:
            return 1;
        case SVM_OP_STORE_LOCAL:
            return -1;
        default:
            return 0;
    }
}

bool sc_builder_emit(
    ScBuilder *b,
    SvmOpcode op,
    int32_t operand,
    int32_t operand2,
    ScError *err
) {
    if (!b->in_func) {
        sc_error_set(err, no_span(), "emit outside function");
        return false;
    }
    if (b->current.code_count == b->current.code_capacity) {
        uint32_t next = b->current.code_capacity * 2u;
        SvmInstruction *grown = realloc(
            b->current.code, next * sizeof(SvmInstruction)
        );
        if (grown == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        b->current.code = grown;
        b->current.code_capacity = next;
    }

    int32_t delta = stack_delta(op);
    int32_t depth = b->current.stack_depth + delta;
    if (depth < 0) {
        sc_error_set(err, no_span(), "stack underflow while emitting");
        return false;
    }
    if ((uint32_t)depth > SVM_MAX_STACK) {
        sc_error_set(err, no_span(), "stack overflow while emitting");
        return false;
    }
    b->current.stack_depth = depth;
    if ((uint32_t)depth > b->current.max_stack) {
        b->current.max_stack = (uint32_t)depth;
    }

    b->current.code[b->current.code_count].opcode = op;
    b->current.code[b->current.code_count].operand = operand;
    b->current.code[b->current.code_count].operand2 = operand2;
    b->current.code_count += 1;
    return true;
}

bool sc_builder_end_func(ScBuilder *b, ScError *err) {
    if (!b->in_func) {
        sc_error_set(err, no_span(), "end_func without begin_func");
        return false;
    }
    if (b->func_count == b->func_capacity) {
        uint32_t next = b->func_capacity == 0 ? 4u : b->func_capacity * 2u;
        ScFuncDraft *grown = realloc(b->funcs, next * sizeof(ScFuncDraft));
        if (grown == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        b->funcs = grown;
        b->func_capacity = next;
    }
    b->funcs[b->func_count] = b->current;
    b->func_count += 1;
    memset(&b->current, 0, sizeof(b->current));
    b->in_func = false;
    return true;
}

bool sc_builder_finish(ScBuilder *b, SvmModule *out_module, ScError *err) {
    if (b->in_func) {
        sc_error_set(err, no_span(), "finish called while inside function");
        return false;
    }
    if (b->func_count == 0) {
        sc_error_set(err, no_span(), "module has no functions");
        return false;
    }
    b->finished = sc_arena_alloc(b->arena, b->func_count * sizeof(SvmFunction));
    if (b->finished == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < b->func_count; i++) {
        ScFuncDraft *draft = &b->funcs[i];
        SvmInstruction *code = sc_arena_alloc(
            b->arena, draft->code_count * sizeof(SvmInstruction)
        );
        if (code == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        memcpy(code, draft->code, draft->code_count * sizeof(SvmInstruction));
        free(draft->code);
        draft->code = NULL;

        b->finished[i] = (SvmFunction){
            .name = draft->name,
            .parameter_types = NULL,
            .parameter_count = 0,
            .capture_count = 0,
            .result_type = SVM_TYPE_I32,
            .local_count = 0,
            .max_stack = draft->max_stack,
            .code = code,
            .code_count = draft->code_count,
            .handlers = NULL,
            .handler_count = 0
        };
    }
    free(b->funcs);
    b->funcs = NULL;
    b->func_capacity = 0;

    out_module->functions = b->finished;
    out_module->function_count = b->func_count;
    out_module->record_types = NULL;
    out_module->record_type_count = 0;
    out_module->host_imports = NULL;
    out_module->host_import_count = 0;
    return true;
}
