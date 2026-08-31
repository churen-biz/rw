#include "sc_ir.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    uint32_t slot;
} IrSym;

typedef struct {
    ScArena *arena;
    ScError *err;
    ScIrFunc *fn;
    ScIrBlock *block;
    IrSym syms[SVM_MAX_LOCALS];
    uint32_t sym_count;
} IrGen;

static ScIrBlock *new_block(IrGen *g) {
    if (g->fn->block_count == g->fn->block_capacity) {
        uint32_t next = g->fn->block_capacity == 0 ? 4u : g->fn->block_capacity * 2u;
        ScIrBlock *grown = realloc(g->fn->blocks, next * sizeof(ScIrBlock));
        if (grown == NULL) {
            sc_error_set(g->err, (ScSpan){0}, "out of memory");
            return NULL;
        }
        g->fn->blocks = grown;
        g->fn->block_capacity = next;
    }
    ScIrBlock *b = &g->fn->blocks[g->fn->block_count];
    memset(b, 0, sizeof(*b));
    b->id = g->fn->block_count;
    g->fn->block_count += 1;
    return b;
}

static bool emit(IrGen *g, ScIrInst inst) {
    ScIrBlock *b = g->block;
    if (b->terminated) {
        sc_error_set(g->err, (ScSpan){0}, "emit after terminator");
        return false;
    }
    if (b->inst_count == b->inst_capacity) {
        uint32_t next = b->inst_capacity == 0 ? 8u : b->inst_capacity * 2u;
        ScIrInst *grown = realloc(b->insts, next * sizeof(ScIrInst));
        if (grown == NULL) {
            sc_error_set(g->err, (ScSpan){0}, "out of memory");
            return false;
        }
        b->insts = grown;
        b->inst_capacity = next;
    }
    b->insts[b->inst_count++] = inst;
    if (inst.op == SC_IR_GOTO || inst.op == SC_IR_BRANCH ||
        inst.op == SC_IR_RET || inst.op == SC_IR_RET_VOID) {
        b->terminated = true;
    }
    return true;
}

static int find_sym(IrGen *g, const char *name) {
    for (uint32_t i = 0; i < g->sym_count; i++) {
        if (strcmp(g->syms[i].name, name) == 0) {
            return (int)g->syms[i].slot;
        }
    }
    return -1;
}

static bool add_sym(IrGen *g, char *name, uint32_t *slot_out) {
    if (g->fn->local_count >= SVM_MAX_LOCALS) {
        sc_error_set(g->err, (ScSpan){0}, "too many locals");
        return false;
    }
    uint32_t slot = g->fn->local_count++;
    g->syms[g->sym_count].name = name;
    g->syms[g->sym_count].slot = slot;
    g->sym_count += 1;
    *slot_out = slot;
    return true;
}

static bool gen_expr(IrGen *g, ScAst *n);
static bool gen_stmt(IrGen *g, ScAst *n);

static bool gen_expr(IrGen *g, ScAst *n) {
    switch (n->kind) {
        case SC_AST_INT:
            return emit(g, (ScIrInst){.op = SC_IR_CONST_I32, .a = n->as.int_value});
        case SC_AST_BOOL:
            return emit(g, (ScIrInst){
                               .op = SC_IR_CONST_BOOL, .a = n->as.bool_value ? 1 : 0
                           });
        case SC_AST_VAR: {
            int slot = find_sym(g, n->as.name);
            if (slot < 0) {
                sc_error_set(g->err, n->span, "ir undefined '%s'", n->as.name);
                return false;
            }
            return emit(g, (ScIrInst){.op = SC_IR_LOAD, .a = slot});
        }
        case SC_AST_BINARY:
            if (!gen_expr(g, n->as.binary.left) || !gen_expr(g, n->as.binary.right)) {
                return false;
            }
            return emit(g, (ScIrInst){.op = SC_IR_BINARY, .bin = n->as.binary.op});
        case SC_AST_CALL:
            for (uint32_t i = 0; i < n->as.call.arg_count; i++) {
                if (!gen_expr(g, n->as.call.args[i])) {
                    return false;
                }
            }
            return emit(g, (ScIrInst){
                               .op = SC_IR_CALL,
                               .call_name = n->as.call.callee,
                               .call_argc = n->as.call.arg_count
                           });
        default:
            sc_error_set(g->err, n->span, "cannot gen expr");
            return false;
    }
}

static bool gen_stmt(IrGen *g, ScAst *n) {
    switch (n->kind) {
        case SC_AST_LET: {
            uint32_t slot = 0;
            if (!add_sym(g, n->as.let.name, &slot)) {
                return false;
            }
            return gen_expr(g, n->as.let.init) &&
                   emit(g, (ScIrInst){.op = SC_IR_STORE, .a = (int32_t)slot});
        }
        case SC_AST_ASSIGN: {
            int slot = find_sym(g, n->as.assign.name);
            if (slot < 0) {
                return false;
            }
            return gen_expr(g, n->as.assign.value) &&
                   emit(g, (ScIrInst){.op = SC_IR_STORE, .a = slot});
        }
        case SC_AST_IF: {
            if (!gen_expr(g, n->as.if_stmt.cond)) {
                return false;
            }
            ScIrBlock *then_b = new_block(g);
            ScIrBlock *else_b = new_block(g);
            ScIrBlock *join = new_block(g);
            if (!then_b || !else_b || !join) {
                return false;
            }
            if (!emit(g, (ScIrInst){
                             .op = SC_IR_BRANCH,
                             .a = (int32_t)then_b->id,
                             .b = (int32_t)else_b->id
                         })) {
                return false;
            }
            g->block = then_b;
            if (!gen_stmt(g, n->as.if_stmt.then_branch)) {
                return false;
            }
            if (!g->block->terminated &&
                !emit(g, (ScIrInst){.op = SC_IR_GOTO, .a = (int32_t)join->id})) {
                return false;
            }
            g->block = else_b;
            if (n->as.if_stmt.else_branch != NULL &&
                !gen_stmt(g, n->as.if_stmt.else_branch)) {
                return false;
            }
            if (!g->block->terminated &&
                !emit(g, (ScIrInst){.op = SC_IR_GOTO, .a = (int32_t)join->id})) {
                return false;
            }
            g->block = join;
            return true;
        }
        case SC_AST_WHILE: {
            ScIrBlock *header = new_block(g);
            ScIrBlock *body = new_block(g);
            ScIrBlock *exit = new_block(g);
            if (!header || !body || !exit) {
                return false;
            }
            if (!emit(g, (ScIrInst){.op = SC_IR_GOTO, .a = (int32_t)header->id})) {
                return false;
            }
            g->block = header;
            if (!gen_expr(g, n->as.while_stmt.cond) ||
                !emit(g, (ScIrInst){
                             .op = SC_IR_BRANCH,
                             .a = (int32_t)body->id,
                             .b = (int32_t)exit->id
                         })) {
                return false;
            }
            g->block = body;
            if (!gen_stmt(g, n->as.while_stmt.body)) {
                return false;
            }
            if (!g->block->terminated &&
                !emit(g, (ScIrInst){.op = SC_IR_GOTO, .a = (int32_t)header->id})) {
                return false;
            }
            g->block = exit;
            return true;
        }
        case SC_AST_RETURN:
            if (n->as.ret.value == NULL) {
                return emit(g, (ScIrInst){.op = SC_IR_RET_VOID});
            }
            return gen_expr(g, n->as.ret.value) &&
                   emit(g, (ScIrInst){.op = SC_IR_RET});
        case SC_AST_BLOCK:
            for (uint32_t i = 0; i < n->as.block.count; i++) {
                if (!gen_stmt(g, n->as.block.stmts[i])) {
                    return false;
                }
            }
            return true;
        case SC_AST_EXPR_STMT:
            return gen_expr(g, n->as.expr) && emit(g, (ScIrInst){.op = SC_IR_POP});
        default:
            sc_error_set(g->err, n->span, "cannot gen stmt");
            return false;
    }
}

bool sc_ir_from_module(
    ScModuleAst *ast, ScArena *arena, ScIrModule **out_ir, ScError *err
) {
    ScIrModule *ir = sc_arena_alloc(arena, sizeof(ScIrModule));
    memset(ir, 0, sizeof(*ir));
    ir->function_count = ast->function_count;
    ir->functions = sc_arena_alloc(arena, ir->function_count * sizeof(ScIrFunc));
    memset(ir->functions, 0, ir->function_count * sizeof(ScIrFunc));

    for (uint32_t i = 0; i < ast->function_count; i++) {
        ScFnDecl *src = &ast->functions[i];
        ScIrFunc *fn = &ir->functions[i];
        fn->name = src->name;
        fn->param_count = src->param_count;
        fn->result_type = src->result_type;
        if (src->param_count > 0) {
            fn->param_types =
                sc_arena_alloc(arena, src->param_count * sizeof(SvmType));
            for (uint32_t p = 0; p < src->param_count; p++) {
                fn->param_types[p] = src->params[p].type;
            }
        }
        IrGen g;
        memset(&g, 0, sizeof(g));
        g.arena = arena;
        g.err = err;
        g.fn = fn;
        for (uint32_t p = 0; p < src->param_count; p++) {
            uint32_t slot = 0;
            if (!add_sym(&g, src->params[p].name, &slot)) {
                return false;
            }
        }
        g.block = new_block(&g);
        if (g.block == NULL || !gen_stmt(&g, src->body)) {
            return false;
        }
        if (!g.block->terminated) {
            if (fn->result_type == SVM_TYPE_VOID) {
                if (!emit(&g, (ScIrInst){.op = SC_IR_RET_VOID})) {
                    return false;
                }
            } else {
                sc_error_set(err, src->span, "function may not return");
                return false;
            }
        }
    }
    *out_ir = ir;
    return true;
}
