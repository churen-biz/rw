#include "sc_builder.h"

#include <stdlib.h>
#include <string.h>

enum { SC_BUILDER_INS_CAP = 64, SC_BUILDER_LABEL_CAP = 32 };

typedef struct {
    char *name;
    uint32_t pc;
    bool defined;
} ScLabel;

typedef struct {
    uint32_t code_index;
    char *label;
} ScJumpFixup;

typedef struct {
    char *name;
    SvmType *param_types;
    uint32_t param_count;
    SvmType result_type;
    SvmInstruction *code;
    uint32_t code_count;
    uint32_t code_capacity;
    uint32_t max_stack;
    int32_t stack_depth;
    uint32_t max_local_index; /* inclusive; 0 if none used and no params */
    bool saw_local;
    ScLabel *labels;
    uint32_t label_count;
    uint32_t label_capacity;
    ScJumpFixup *fixups;
    uint32_t fixup_count;
    uint32_t fixup_capacity;
    bool open;
} ScFuncDraft;

struct ScBuilder {
    ScArena *arena;
    ScFuncDraft *funcs;
    uint32_t func_count;
    uint32_t func_capacity;
    int32_t current_index; /* -1 if none */
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
    b->current_index = -1;
    return b;
}

static ScFuncDraft *current(ScBuilder *b) {
    if (b->current_index < 0) {
        return NULL;
    }
    return &b->funcs[(uint32_t)b->current_index];
}

static bool ensure_func_capacity(ScBuilder *b, ScError *err) {
    if (b->func_count < b->func_capacity) {
        return true;
    }
    uint32_t next = b->func_capacity == 0 ? 4u : b->func_capacity * 2u;
    ScFuncDraft *grown = realloc(b->funcs, next * sizeof(ScFuncDraft));
    if (grown == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    b->funcs = grown;
    b->func_capacity = next;
    return true;
}

bool sc_builder_begin_func(
    ScBuilder *b,
    const char *name,
    const SvmType *param_types,
    uint32_t param_count,
    SvmType result_type,
    ScError *err
) {
    if (b->current_index >= 0) {
        sc_error_set(err, no_span(), "nested function is not supported");
        return false;
    }
    for (uint32_t i = 0; i < b->func_count; i++) {
        if (strcmp(b->funcs[i].name, name) == 0) {
            sc_error_set(err, no_span(), "duplicate function '%s'", name);
            return false;
        }
    }
    if (!ensure_func_capacity(b, err)) {
        return false;
    }

    ScFuncDraft *draft = &b->funcs[b->func_count];
    memset(draft, 0, sizeof(*draft));
    draft->name = sc_arena_strndup(b->arena, name, strlen(name));
    if (draft->name == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    draft->param_count = param_count;
    draft->result_type = result_type;
    if (param_count > 0) {
        draft->param_types =
            sc_arena_alloc(b->arena, param_count * sizeof(SvmType));
        if (draft->param_types == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        memcpy(draft->param_types, param_types, param_count * sizeof(SvmType));
        draft->saw_local = true;
        draft->max_local_index = param_count - 1u;
    }
    draft->code_capacity = SC_BUILDER_INS_CAP;
    draft->code = calloc(draft->code_capacity, sizeof(SvmInstruction));
    if (draft->code == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    draft->open = true;
    b->current_index = (int32_t)b->func_count;
    b->func_count += 1;
    return true;
}

static int32_t stack_delta(SvmOpcode op) {
    switch (op) {
        case SVM_OP_CONST_I32:
        case SVM_OP_CONST_FALSE:
        case SVM_OP_CONST_TRUE:
        case SVM_OP_CONST_NULL:
        case SVM_OP_LOAD_LOCAL:
        case SVM_OP_DUP:
            return 1;
        case SVM_OP_I32_ADD:
        case SVM_OP_I32_SUB:
        case SVM_OP_I32_MUL:
        case SVM_OP_I32_LE:
        case SVM_OP_I32_EQ:
        case SVM_OP_STORE_LOCAL:
        case SVM_OP_JUMP_IF_FALSE:
        case SVM_OP_POP:
            return -1;
        case SVM_OP_RETURN:
        case SVM_OP_JUMP:
            return 0;
        default:
            return 0;
    }
}

static bool apply_stack(ScFuncDraft *draft, int32_t delta, ScError *err) {
    int32_t depth = draft->stack_depth + delta;
    if (depth < 0) {
        sc_error_set(err, no_span(), "stack underflow while emitting");
        return false;
    }
    if ((uint32_t)depth > SVM_MAX_STACK) {
        sc_error_set(err, no_span(), "stack overflow while emitting");
        return false;
    }
    draft->stack_depth = depth;
    if ((uint32_t)depth > draft->max_stack) {
        draft->max_stack = (uint32_t)depth;
    }
    return true;
}

static bool ensure_code_room(ScFuncDraft *draft, ScError *err) {
    if (draft->code_count < draft->code_capacity) {
        return true;
    }
    uint32_t next = draft->code_capacity * 2u;
    SvmInstruction *grown =
        realloc(draft->code, next * sizeof(SvmInstruction));
    if (grown == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    draft->code = grown;
    draft->code_capacity = next;
    return true;
}

bool sc_builder_define_label(ScBuilder *b, const char *name, ScError *err) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "label outside function");
        return false;
    }
    for (uint32_t i = 0; i < draft->label_count; i++) {
        if (strcmp(draft->labels[i].name, name) == 0) {
            if (draft->labels[i].defined) {
                sc_error_set(err, no_span(), "duplicate label '%s'", name);
                return false;
            }
            draft->labels[i].pc = draft->code_count;
            draft->labels[i].defined = true;
            return true;
        }
    }
    if (draft->label_count == draft->label_capacity) {
        uint32_t next =
            draft->label_capacity == 0 ? 8u : draft->label_capacity * 2u;
        ScLabel *grown = realloc(draft->labels, next * sizeof(ScLabel));
        if (grown == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        draft->labels = grown;
        draft->label_capacity = next;
    }
    draft->labels[draft->label_count].name =
        sc_arena_strndup(b->arena, name, strlen(name));
    if (draft->labels[draft->label_count].name == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    draft->labels[draft->label_count].pc = draft->code_count;
    draft->labels[draft->label_count].defined = true;
    draft->label_count += 1;
    return true;
}

static ScLabel *find_or_add_label(
    ScBuilder *b, ScFuncDraft *draft, const char *name, ScError *err
) {
    for (uint32_t i = 0; i < draft->label_count; i++) {
        if (strcmp(draft->labels[i].name, name) == 0) {
            return &draft->labels[i];
        }
    }
    if (draft->label_count == draft->label_capacity) {
        uint32_t next =
            draft->label_capacity == 0 ? 8u : draft->label_capacity * 2u;
        ScLabel *grown = realloc(draft->labels, next * sizeof(ScLabel));
        if (grown == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return NULL;
        }
        draft->labels = grown;
        draft->label_capacity = next;
    }
    draft->labels[draft->label_count].name =
        sc_arena_strndup(b->arena, name, strlen(name));
    if (draft->labels[draft->label_count].name == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return NULL;
    }
    draft->labels[draft->label_count].pc = 0;
    draft->labels[draft->label_count].defined = false;
    draft->label_count += 1;
    return &draft->labels[draft->label_count - 1u];
}

bool sc_builder_emit(
    ScBuilder *b,
    SvmOpcode op,
    int32_t operand,
    int32_t operand2,
    ScError *err
) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "emit outside function");
        return false;
    }
    if (op == SVM_OP_LOAD_LOCAL || op == SVM_OP_STORE_LOCAL) {
        if (operand < 0) {
            sc_error_set(err, no_span(), "negative local index");
            return false;
        }
        uint32_t index = (uint32_t)operand;
        if (index >= SVM_MAX_LOCALS) {
            sc_error_set(err, no_span(), "local index out of range");
            return false;
        }
        if (!draft->saw_local || index > draft->max_local_index) {
            draft->max_local_index = index;
        }
        draft->saw_local = true;
    }
    if (!ensure_code_room(draft, err)) {
        return false;
    }
    if (!apply_stack(draft, stack_delta(op), err)) {
        return false;
    }
    draft->code[draft->code_count].opcode = op;
    draft->code[draft->code_count].operand = operand;
    draft->code[draft->code_count].operand2 = operand2;
    draft->code_count += 1;
    return true;
}

bool sc_builder_emit_jump(
    ScBuilder *b, SvmOpcode op, const char *label, ScError *err
) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "jump outside function");
        return false;
    }
    if (op != SVM_OP_JUMP && op != SVM_OP_JUMP_IF_FALSE) {
        sc_error_set(err, no_span(), "invalid jump opcode");
        return false;
    }
    if (!ensure_code_room(draft, err)) {
        return false;
    }
    if (!apply_stack(draft, stack_delta(op), err)) {
        return false;
    }
    if (draft->fixup_count == draft->fixup_capacity) {
        uint32_t next =
            draft->fixup_capacity == 0 ? 8u : draft->fixup_capacity * 2u;
        ScJumpFixup *grown =
            realloc(draft->fixups, next * sizeof(ScJumpFixup));
        if (grown == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        draft->fixups = grown;
        draft->fixup_capacity = next;
    }
    draft->fixups[draft->fixup_count].code_index = draft->code_count;
    draft->fixups[draft->fixup_count].label =
        sc_arena_strndup(b->arena, label, strlen(label));
    if (draft->fixups[draft->fixup_count].label == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    draft->fixup_count += 1;

    if (find_or_add_label(b, draft, label, err) == NULL) {
        return false;
    }

    draft->code[draft->code_count].opcode = op;
    draft->code[draft->code_count].operand = 0;
    draft->code[draft->code_count].operand2 = 0;
    draft->code_count += 1;
    return true;
}

bool sc_builder_emit_call(ScBuilder *b, const char *func_name, ScError *err) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "call outside function");
        return false;
    }
    int32_t target = -1;
    uint32_t param_count = 0;
    for (uint32_t i = 0; i < b->func_count; i++) {
        if (strcmp(b->funcs[i].name, func_name) == 0) {
            target = (int32_t)i;
            param_count = b->funcs[i].param_count;
            break;
        }
    }
    if (target < 0) {
        sc_error_set(
            err,
            no_span(),
            "unknown function '%s' (define callee before caller)",
            func_name
        );
        return false;
    }
    int32_t delta = 1 - (int32_t)param_count;
    if (!ensure_code_room(draft, err)) {
        return false;
    }
    if (!apply_stack(draft, delta, err)) {
        return false;
    }
    draft->code[draft->code_count].opcode = SVM_OP_CALL;
    draft->code[draft->code_count].operand = target;
    draft->code[draft->code_count].operand2 = 0;
    draft->code_count += 1;
    return true;
}

bool sc_builder_end_func(ScBuilder *b, ScError *err) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "end_func without begin_func");
        return false;
    }
    for (uint32_t i = 0; i < draft->fixup_count; i++) {
        ScJumpFixup *fix = &draft->fixups[i];
        ScLabel *label = NULL;
        for (uint32_t j = 0; j < draft->label_count; j++) {
            if (strcmp(draft->labels[j].name, fix->label) == 0) {
                label = &draft->labels[j];
                break;
            }
        }
        if (label == NULL || !label->defined) {
            sc_error_set(err, no_span(), "undefined label '%s'", fix->label);
            return false;
        }
        draft->code[fix->code_index].operand = (int32_t)label->pc;
    }
    free(draft->fixups);
    draft->fixups = NULL;
    draft->fixup_count = 0;
    draft->fixup_capacity = 0;
    free(draft->labels);
    draft->labels = NULL;
    draft->label_count = 0;
    draft->label_capacity = 0;
    draft->open = false;
    b->current_index = -1;
    return true;
}

bool sc_builder_finish(ScBuilder *b, SvmModule *out_module, ScError *err) {
    if (b->current_index >= 0) {
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
        if (draft->open) {
            sc_error_set(err, no_span(), "function '%s' not closed", draft->name);
            return false;
        }
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

        uint32_t local_count = 0;
        if (draft->saw_local) {
            local_count = draft->max_local_index + 1u;
        }
        if (local_count < draft->param_count) {
            local_count = draft->param_count;
        }

        b->finished[i] = (SvmFunction){
            .name = draft->name,
            .parameter_types = draft->param_types,
            .parameter_count = draft->param_count,
            .capture_count = 0,
            .result_type = draft->result_type,
            .local_count = local_count,
            .max_stack = draft->max_stack == 0 ? 1u : draft->max_stack,
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
