#include "sc_builder.h"
#include "sc_ir.h"

#include <stdio.h>

static bool lower_func(ScBuilder *b, ScIrFunc *fn, ScError *err) {
    if (!sc_builder_begin_func(
            b, fn->name, fn->param_types, fn->param_count, fn->result_type, err
        )) {
        return false;
    }

    char labels[128][32];
    if (fn->block_count > 128) {
        sc_error_set(err, (ScSpan){0}, "too many blocks");
        return false;
    }
    for (uint32_t i = 0; i < fn->block_count; i++) {
        snprintf(labels[i], sizeof(labels[i]), "b%u", i);
    }

    for (uint32_t bi = 0; bi < fn->block_count; bi++) {
        ScIrBlock *block = &fn->blocks[bi];
        if (!sc_builder_define_label(b, labels[bi], err)) {
            return false;
        }
        for (uint32_t ii = 0; ii < block->inst_count; ii++) {
            ScIrInst *ins = &block->insts[ii];
            switch (ins->op) {
                case SC_IR_CONST_I32:
                    if (!sc_builder_emit(b, SVM_OP_CONST_I32, ins->a, 0, err)) {
                        return false;
                    }
                    break;
                case SC_IR_CONST_BOOL:
                    if (!sc_builder_emit(
                            b,
                            ins->a ? SVM_OP_CONST_TRUE : SVM_OP_CONST_FALSE,
                            0,
                            0,
                            err
                        )) {
                        return false;
                    }
                    break;
                case SC_IR_LOAD:
                    if (!sc_builder_emit(b, SVM_OP_LOAD_LOCAL, ins->a, 0, err)) {
                        return false;
                    }
                    break;
                case SC_IR_STORE:
                    if (!sc_builder_emit(b, SVM_OP_STORE_LOCAL, ins->a, 0, err)) {
                        return false;
                    }
                    break;
                case SC_IR_BINARY: {
                    SvmOpcode op = SVM_OP_I32_ADD;
                    if (ins->bin == SC_BIN_SUB) {
                        op = SVM_OP_I32_SUB;
                    } else if (ins->bin == SC_BIN_MUL) {
                        op = SVM_OP_I32_MUL;
                    } else if (ins->bin == SC_BIN_LE) {
                        op = SVM_OP_I32_LE;
                    } else if (ins->bin == SC_BIN_EQ) {
                        op = SVM_OP_I32_EQ;
                    }
                    if (!sc_builder_emit(b, op, 0, 0, err)) {
                        return false;
                    }
                    break;
                }
                case SC_IR_CALL:
                    if (!sc_builder_emit_call(b, ins->call_name, err)) {
                        return false;
                    }
                    break;
                case SC_IR_POP:
                    if (!sc_builder_emit(b, SVM_OP_POP, 0, 0, err)) {
                        return false;
                    }
                    break;
                case SC_IR_GOTO:
                    if (!sc_builder_emit_jump(b, SVM_OP_JUMP, labels[ins->a], err)) {
                        return false;
                    }
                    break;
                case SC_IR_BRANCH:
                    /* a=then, b=else; jump_if_false else; jump then */
                    if (!sc_builder_emit_jump(
                            b, SVM_OP_JUMP_IF_FALSE, labels[ins->b], err
                        ) ||
                        !sc_builder_emit_jump(b, SVM_OP_JUMP, labels[ins->a], err)) {
                        return false;
                    }
                    break;
                case SC_IR_RET:
                    if (!sc_builder_emit(b, SVM_OP_RETURN, 0, 0, err)) {
                        return false;
                    }
                    break;
                case SC_IR_RET_VOID:
                    if (!sc_builder_emit(b, SVM_OP_RETURN, 0, 0, err)) {
                        return false;
                    }
                    break;
            }
        }
    }
    return sc_builder_end_func(b, err);
}

bool sc_ir_lower(
    ScIrModule *ir, ScArena *arena, SvmModule *out_module, ScError *err
) {
    ScBuilder *b = sc_builder_create(arena);
    if (b == NULL) {
        sc_error_set(err, (ScSpan){0}, "out of memory");
        return false;
    }
    for (uint32_t i = 0; i < ir->function_count; i++) {
        if (!lower_func(b, &ir->functions[i], err)) {
            return false;
        }
    }
    return sc_builder_finish(b, out_module, err);
}
