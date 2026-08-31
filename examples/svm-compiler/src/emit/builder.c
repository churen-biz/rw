#include "sc_builder.h"

#include <stdlib.h>
#include <string.h>

enum { SC_BUILDER_INS_CAP = 64 };

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
    char *start_label;
    char *end_label;
    char *handler_label;
} ScHandlerDraft;

typedef struct {
    char *name;
    SvmType *param_types;
    uint32_t param_count;
    uint32_t capture_count;
    SvmType result_type;
    SvmInstruction *code;
    uint32_t code_count;
    uint32_t code_capacity;
    uint32_t max_stack;
    int32_t stack_depth;
    uint32_t max_local_index;
    bool saw_local;
    ScLabel *labels;
    uint32_t label_count;
    uint32_t label_capacity;
    ScJumpFixup *fixups;
    uint32_t fixup_count;
    uint32_t fixup_capacity;
    ScHandlerDraft *handlers;
    uint32_t handler_count;
    uint32_t handler_capacity;
    bool open;
} ScFuncDraft;

typedef struct {
    char *name;
    SvmType *fields;
    uint32_t field_count;
} ScRecordDraft;

typedef struct {
    char *capability_name;
    char *function_name;
    SvmType *param_types;
    uint32_t param_count;
    SvmType result_type;
    bool asynchronous;
} ScImportDraft;

struct ScBuilder {
    ScArena *arena;
    ScFuncDraft *funcs;
    uint32_t func_count;
    uint32_t func_capacity;
    ScRecordDraft *records;
    uint32_t record_count;
    uint32_t record_capacity;
    ScImportDraft *imports;
    uint32_t import_count;
    uint32_t import_capacity;
    int32_t current_index;
    SvmFunction *finished;
    SvmRecordType *finished_records;
    SvmHostImport *finished_imports;
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

int32_t sc_builder_find_record(const ScBuilder *b, const char *name) {
    for (uint32_t i = 0; i < b->record_count; i++) {
        if (strcmp(b->records[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

int32_t sc_builder_find_func(const ScBuilder *b, const char *name) {
    for (uint32_t i = 0; i < b->func_count; i++) {
        if (strcmp(b->funcs[i].name, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

bool sc_builder_add_record(
    ScBuilder *b,
    const char *name,
    const SvmType *fields,
    uint32_t field_count,
    ScError *err
) {
    if (sc_builder_find_record(b, name) >= 0) {
        sc_error_set(err, no_span(), "duplicate record type '%s'", name);
        return false;
    }
    if (b->record_count == b->record_capacity) {
        uint32_t next = b->record_capacity == 0 ? 4u : b->record_capacity * 2u;
        ScRecordDraft *grown = realloc(b->records, next * sizeof(*grown));
        if (grown == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        b->records = grown;
        b->record_capacity = next;
    }
    ScRecordDraft *rec = &b->records[b->record_count];
    memset(rec, 0, sizeof(*rec));
    rec->name = sc_arena_strndup(b->arena, name, strlen(name));
    rec->field_count = field_count;
    if (field_count > 0) {
        rec->fields = sc_arena_alloc(b->arena, field_count * sizeof(SvmType));
        if (rec->fields == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        memcpy(rec->fields, fields, field_count * sizeof(SvmType));
    }
    if (rec->name == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    b->record_count += 1;
    return true;
}

bool sc_builder_add_import(
    ScBuilder *b,
    const char *capability_name,
    const char *function_name,
    const SvmType *param_types,
    uint32_t param_count,
    SvmType result_type,
    bool asynchronous,
    ScError *err
) {
    if (b->import_count == b->import_capacity) {
        uint32_t next = b->import_capacity == 0 ? 4u : b->import_capacity * 2u;
        ScImportDraft *grown = realloc(b->imports, next * sizeof(*grown));
        if (grown == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        b->imports = grown;
        b->import_capacity = next;
    }
    ScImportDraft *imp = &b->imports[b->import_count];
    memset(imp, 0, sizeof(*imp));
    imp->capability_name =
        sc_arena_strndup(b->arena, capability_name, strlen(capability_name));
    imp->function_name =
        sc_arena_strndup(b->arena, function_name, strlen(function_name));
    imp->param_count = param_count;
    imp->result_type = result_type;
    imp->asynchronous = asynchronous;
    if (param_count > 0) {
        imp->param_types =
            sc_arena_alloc(b->arena, param_count * sizeof(SvmType));
        if (imp->param_types == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        memcpy(imp->param_types, param_types, param_count * sizeof(SvmType));
    }
    if (imp->capability_name == NULL || imp->function_name == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    b->import_count += 1;
    return true;
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
    if (sc_builder_find_func(b, name) >= 0) {
        sc_error_set(err, no_span(), "duplicate function '%s'", name);
        return false;
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

bool sc_builder_set_captures(ScBuilder *b, uint32_t capture_count, ScError *err) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "captures outside function");
        return false;
    }
    if (capture_count > draft->param_count) {
        sc_error_set(err, no_span(), "capture_count exceeds parameter_count");
        return false;
    }
    draft->capture_count = capture_count;
    return true;
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

static int32_t default_stack_delta(SvmOpcode op) {
    switch (op) {
        case SVM_OP_CONST_I32:
        case SVM_OP_CONST_FALSE:
        case SVM_OP_CONST_TRUE:
        case SVM_OP_CONST_NULL:
        case SVM_OP_LOAD_LOCAL:
        case SVM_OP_DUP:
        case SVM_OP_NEW_OBJECT:
            return 1;
        case SVM_OP_I32_ADD:
        case SVM_OP_I32_SUB:
        case SVM_OP_I32_MUL:
        case SVM_OP_I32_LE:
        case SVM_OP_I32_EQ:
        case SVM_OP_STORE_LOCAL:
        case SVM_OP_JUMP_IF_FALSE:
        case SVM_OP_POP:
        case SVM_OP_THROW:
        case SVM_OP_CHANNEL_CLOSE:
        case SVM_OP_DEFER_PUSH:
            return -1;
        case SVM_OP_GET_FIELD:
        case SVM_OP_ARRAY_LEN:
        case SVM_OP_ARRAY_GET:
        case SVM_OP_GC_COLLECT:
        case SVM_OP_RETURN:
        case SVM_OP_JUMP:
        case SVM_OP_TASK_YIELD:
            return 0;
        case SVM_OP_SET_FIELD:
            return -2;
        case SVM_OP_NEW_ARRAY:
            return 0; /* pop len, push ref */
        case SVM_OP_ARRAY_SET:
            return -3;
        case SVM_OP_CHANNEL_NEW:
            return 0; /* pop capacity, push channel */
        case SVM_OP_CHANNEL_SEND:
            return -2;
        case SVM_OP_CHANNEL_RECV:
            return 0; /* pop channel, push value */
        case SVM_OP_TASK_AWAIT:
            return 0; /* pop task, push result (assume non-void) */
        default:
            return 0;
    }
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

static bool resolve_label_pc(
    ScFuncDraft *draft, const char *name, uint32_t *pc, ScError *err
) {
    for (uint32_t i = 0; i < draft->label_count; i++) {
        if (strcmp(draft->labels[i].name, name) == 0 && draft->labels[i].defined) {
            *pc = draft->labels[i].pc;
            return true;
        }
    }
    sc_error_set(err, no_span(), "undefined label '%s'", name);
    return false;
}

bool sc_builder_add_handler(
    ScBuilder *b,
    const char *start_label,
    const char *end_label,
    const char *handler_label,
    ScError *err
) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "handler outside function");
        return false;
    }
    if (draft->handler_count == draft->handler_capacity) {
        uint32_t next =
            draft->handler_capacity == 0 ? 4u : draft->handler_capacity * 2u;
        ScHandlerDraft *grown =
            realloc(draft->handlers, next * sizeof(ScHandlerDraft));
        if (grown == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        draft->handlers = grown;
        draft->handler_capacity = next;
    }
    ScHandlerDraft *h = &draft->handlers[draft->handler_count];
    h->start_label = sc_arena_strndup(b->arena, start_label, strlen(start_label));
    h->end_label = sc_arena_strndup(b->arena, end_label, strlen(end_label));
    h->handler_label =
        sc_arena_strndup(b->arena, handler_label, strlen(handler_label));
    if (h->start_label == NULL || h->end_label == NULL ||
        h->handler_label == NULL) {
        sc_error_set(err, no_span(), "out of memory");
        return false;
    }
    draft->handler_count += 1;
    return true;
}

static bool push_ins(
    ScFuncDraft *draft, SvmOpcode op, int32_t operand, int32_t operand2,
    int32_t delta, ScError *err
) {
    if (!ensure_code_room(draft, err)) {
        return false;
    }
    if (!apply_stack(draft, delta, err)) {
        return false;
    }
    draft->code[draft->code_count].opcode = op;
    draft->code[draft->code_count].operand = operand;
    draft->code[draft->code_count].operand2 = operand2;
    draft->code_count += 1;
    return true;
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
        if (operand < 0 || (uint32_t)operand >= SVM_MAX_LOCALS) {
            sc_error_set(err, no_span(), "local index out of range");
            return false;
        }
        uint32_t index = (uint32_t)operand;
        if (!draft->saw_local || index > draft->max_local_index) {
            draft->max_local_index = index;
        }
        draft->saw_local = true;
    }
    if (op == SVM_OP_CHANNEL_SELECT) {
        int32_t delta = 2 - operand; /* pop N channels, push i32 + value */
        return push_ins(draft, op, operand, operand2, delta, err);
    }
    if (op == SVM_OP_TASK_SPAWN) {
        /* delta filled by emit_call_named */
        sc_error_set(err, no_span(), "use sc_builder_emit_call_named for spawn");
        return false;
    }
    return push_ins(draft, op, operand, operand2, default_stack_delta(op), err);
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
    return push_ins(draft, op, 0, 0, default_stack_delta(op), err);
}

bool sc_builder_emit_call(ScBuilder *b, const char *func_name, ScError *err) {
    return sc_builder_emit_call_named(b, SVM_OP_CALL, func_name, 0, err);
}

bool sc_builder_emit_call_named(
    ScBuilder *b,
    SvmOpcode op,
    const char *func_name,
    int32_t operand2,
    ScError *err
) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "call outside function");
        return false;
    }
    int32_t target = sc_builder_find_func(b, func_name);
    if (target < 0) {
        sc_error_set(
            err, no_span(),
            "unknown function '%s' (define callee before caller)", func_name
        );
        return false;
    }
    const ScFuncDraft *callee = &b->funcs[(uint32_t)target];
    int32_t delta = 0;
    if (op == SVM_OP_CALL || op == SVM_OP_TASK_SPAWN) {
        if (op == SVM_OP_CALL) {
            delta = (callee->result_type == SVM_TYPE_VOID ? 0 : 1) -
                    (int32_t)callee->param_count;
        } else {
            delta = 1 - (int32_t)callee->param_count; /* spawn → task */
        }
    } else if (op == SVM_OP_NEW_CLOSURE) {
        if (operand2 < 0) {
            operand2 = (int32_t)callee->capture_count;
        }
        delta = 1 - operand2;
    } else if (op == SVM_OP_CALL_CLOSURE) {
        uint32_t explicit_count =
            callee->param_count - callee->capture_count;
        delta = (callee->result_type == SVM_TYPE_VOID ? 0 : 1) -
                (int32_t)explicit_count - 1;
    } else if (op == SVM_OP_DEFER_PUSH) {
        delta = -1;
    } else {
        sc_error_set(err, no_span(), "unsupported named call opcode");
        return false;
    }
    return push_ins(draft, op, target, operand2, delta, err);
}

bool sc_builder_emit_host(
    ScBuilder *b,
    bool asynchronous,
    const char *capability_name,
    const char *function_name,
    ScError *err
) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "host call outside function");
        return false;
    }
    int32_t index = -1;
    for (uint32_t i = 0; i < b->import_count; i++) {
        if (strcmp(b->imports[i].capability_name, capability_name) == 0 &&
            strcmp(b->imports[i].function_name, function_name) == 0) {
            index = (int32_t)i;
            break;
        }
    }
    if (index < 0) {
        sc_error_set(err, no_span(), "unknown host import '%s %s'",
                     capability_name, function_name);
        return false;
    }
    const ScImportDraft *imp = &b->imports[(uint32_t)index];
    if (imp->asynchronous != asynchronous) {
        sc_error_set(err, no_span(), "host call async mismatch for '%s %s'",
                     capability_name, function_name);
        return false;
    }
    int32_t push = (asynchronous || imp->result_type != SVM_TYPE_VOID) ? 1 : 0;
    int32_t delta = push - (int32_t)imp->param_count;
    SvmOpcode op = asynchronous ? SVM_OP_HOST_CALL_ASYNC : SVM_OP_HOST_CALL;
    return push_ins(draft, op, index, 0, delta, err);
}

bool sc_builder_end_func(ScBuilder *b, ScError *err) {
    ScFuncDraft *draft = current(b);
    if (draft == NULL) {
        sc_error_set(err, no_span(), "end_func without begin_func");
        return false;
    }
    for (uint32_t i = 0; i < draft->fixup_count; i++) {
        ScJumpFixup *fix = &draft->fixups[i];
        uint32_t pc = 0;
        if (!resolve_label_pc(draft, fix->label, &pc, err)) {
            return false;
        }
        draft->code[fix->code_index].operand = (int32_t)pc;
    }
    free(draft->fixups);
    draft->fixups = NULL;
    draft->fixup_count = 0;
    draft->fixup_capacity = 0;
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

    if (b->record_count > 0) {
        b->finished_records = sc_arena_alloc(
            b->arena, b->record_count * sizeof(SvmRecordType)
        );
        if (b->finished_records == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        for (uint32_t i = 0; i < b->record_count; i++) {
            b->finished_records[i] = (SvmRecordType){
                .name = b->records[i].name,
                .field_types = b->records[i].fields,
                .field_count = b->records[i].field_count
            };
        }
    }
    free(b->records);
    b->records = NULL;

    if (b->import_count > 0) {
        b->finished_imports = sc_arena_alloc(
            b->arena, b->import_count * sizeof(SvmHostImport)
        );
        if (b->finished_imports == NULL) {
            sc_error_set(err, no_span(), "out of memory");
            return false;
        }
        for (uint32_t i = 0; i < b->import_count; i++) {
            b->finished_imports[i] = (SvmHostImport){
                .capability_name = b->imports[i].capability_name,
                .function_name = b->imports[i].function_name,
                .parameter_types = b->imports[i].param_types,
                .parameter_count = b->imports[i].param_count,
                .result_type = b->imports[i].result_type,
                .asynchronous = b->imports[i].asynchronous
            };
        }
    }
    free(b->imports);
    b->imports = NULL;

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

        SvmExceptionHandler *handlers = NULL;
        if (draft->handler_count > 0) {
            handlers = sc_arena_alloc(
                b->arena, draft->handler_count * sizeof(SvmExceptionHandler)
            );
            if (handlers == NULL) {
                sc_error_set(err, no_span(), "out of memory");
                return false;
            }
            for (uint32_t h = 0; h < draft->handler_count; h++) {
                uint32_t start_pc = 0;
                uint32_t end_pc = 0;
                uint32_t handler_pc = 0;
                if (!resolve_label_pc(
                        draft, draft->handlers[h].start_label, &start_pc, err
                    ) ||
                    !resolve_label_pc(
                        draft, draft->handlers[h].end_label, &end_pc, err
                    ) ||
                    !resolve_label_pc(
                        draft, draft->handlers[h].handler_label, &handler_pc,
                        err
                    )) {
                    return false;
                }
                handlers[h] = (SvmExceptionHandler){
                    .start_pc = start_pc,
                    .end_pc = end_pc,
                    .handler_pc = handler_pc
                };
            }
        }
        free(draft->handlers);
        draft->handlers = NULL;
        free(draft->labels);
        draft->labels = NULL;

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
            .capture_count = draft->capture_count,
            .result_type = draft->result_type,
            .local_count = local_count,
            .max_stack = draft->max_stack == 0 ? 1u : draft->max_stack,
            .code = code,
            .code_count = draft->code_count,
            .handlers = handlers,
            .handler_count = draft->handler_count
        };
    }
    free(b->funcs);
    b->funcs = NULL;

    out_module->functions = b->finished;
    out_module->function_count = b->func_count;
    out_module->record_types = b->finished_records;
    out_module->record_type_count = b->record_count;
    out_module->host_imports = b->finished_imports;
    out_module->host_import_count = b->import_count;
    return true;
}
