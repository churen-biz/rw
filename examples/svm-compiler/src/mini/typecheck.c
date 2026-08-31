#include "sc_mini.h"

#include <string.h>

typedef struct {
    char *name;
    SvmType type;
} Sym;

typedef struct {
    Sym locals[SVM_MAX_LOCALS];
    uint32_t local_count;
    const ScModuleAst *module;
    SvmType current_result;
    ScError *err;
} Tc;

static int find_fn(const ScModuleAst *m, const char *name) {
    for (uint32_t i = 0; i < m->function_count; i++) {
        if (strcmp(m->functions[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_local(const Tc *tc, const char *name) {
    for (uint32_t i = 0; i < tc->local_count; i++) {
        if (strcmp(tc->locals[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool add_local(Tc *tc, char *name, SvmType type, ScSpan span) {
    if (find_local(tc, name) >= 0) {
        sc_error_set(tc->err, span, "duplicate local '%s'", name);
        return false;
    }
    if (tc->local_count >= SVM_MAX_LOCALS) {
        sc_error_set(tc->err, span, "too many locals");
        return false;
    }
    tc->locals[tc->local_count].name = name;
    tc->locals[tc->local_count].type = type;
    tc->local_count += 1;
    return true;
}

static bool tc_expr(Tc *tc, ScAst *n);
static bool tc_stmt(Tc *tc, ScAst *n);

static bool tc_expr(Tc *tc, ScAst *n) {
    switch (n->kind) {
        case SC_AST_INT:
            n->type = SVM_TYPE_I32;
            return true;
        case SC_AST_BOOL:
            n->type = SVM_TYPE_BOOL;
            return true;
        case SC_AST_VAR: {
            int idx = find_local(tc, n->as.name);
            if (idx < 0) {
                sc_error_set(tc->err, n->span, "undefined variable '%s'", n->as.name);
                return false;
            }
            n->type = tc->locals[idx].type;
            return true;
        }
        case SC_AST_BINARY:
            if (!tc_expr(tc, n->as.binary.left) || !tc_expr(tc, n->as.binary.right)) {
                return false;
            }
            if (n->as.binary.op == SC_BIN_LE || n->as.binary.op == SC_BIN_EQ) {
                if (n->as.binary.left->type != SVM_TYPE_I32 ||
                    n->as.binary.right->type != SVM_TYPE_I32) {
                    sc_error_set(tc->err, n->span, "comparison requires i32");
                    return false;
                }
                n->type = SVM_TYPE_BOOL;
                return true;
            }
            if (n->as.binary.left->type != SVM_TYPE_I32 ||
                n->as.binary.right->type != SVM_TYPE_I32) {
                sc_error_set(tc->err, n->span, "arithmetic requires i32");
                return false;
            }
            n->type = SVM_TYPE_I32;
            return true;
        case SC_AST_CALL: {
            int fi = find_fn(tc->module, n->as.call.callee);
            if (fi < 0) {
                sc_error_set(tc->err, n->span, "unknown function '%s'", n->as.call.callee);
                return false;
            }
            const ScFnDecl *fn = &tc->module->functions[fi];
            if (n->as.call.arg_count != fn->param_count) {
                sc_error_set(tc->err, n->span, "wrong arity for '%s'", n->as.call.callee);
                return false;
            }
            for (uint32_t i = 0; i < n->as.call.arg_count; i++) {
                if (!tc_expr(tc, n->as.call.args[i])) {
                    return false;
                }
                if (n->as.call.args[i]->type != fn->params[i].type) {
                    sc_error_set(tc->err, n->span, "argument type mismatch");
                    return false;
                }
            }
            n->type = fn->result_type;
            return true;
        }
        default:
            sc_error_set(tc->err, n->span, "invalid expression node");
            return false;
    }
}

static bool tc_stmt(Tc *tc, ScAst *n) {
    switch (n->kind) {
        case SC_AST_LET:
            if (!tc_expr(tc, n->as.let.init)) {
                return false;
            }
            if (n->as.let.init->type != n->as.let.type) {
                sc_error_set(tc->err, n->span, "let initializer type mismatch");
                return false;
            }
            return add_local(tc, n->as.let.name, n->as.let.type, n->span);
        case SC_AST_ASSIGN: {
            int idx = find_local(tc, n->as.assign.name);
            if (idx < 0) {
                sc_error_set(tc->err, n->span, "undefined variable '%s'", n->as.assign.name);
                return false;
            }
            if (!tc_expr(tc, n->as.assign.value)) {
                return false;
            }
            if (n->as.assign.value->type != tc->locals[idx].type) {
                sc_error_set(tc->err, n->span, "assignment type mismatch");
                return false;
            }
            return true;
        }
        case SC_AST_IF:
            if (!tc_expr(tc, n->as.if_stmt.cond)) {
                return false;
            }
            if (n->as.if_stmt.cond->type != SVM_TYPE_BOOL) {
                sc_error_set(tc->err, n->span, "if condition must be bool");
                return false;
            }
            if (!tc_stmt(tc, n->as.if_stmt.then_branch)) {
                return false;
            }
            if (n->as.if_stmt.else_branch != NULL) {
                return tc_stmt(tc, n->as.if_stmt.else_branch);
            }
            return true;
        case SC_AST_WHILE:
            if (!tc_expr(tc, n->as.while_stmt.cond)) {
                return false;
            }
            if (n->as.while_stmt.cond->type != SVM_TYPE_BOOL) {
                sc_error_set(tc->err, n->span, "while condition must be bool");
                return false;
            }
            return tc_stmt(tc, n->as.while_stmt.body);
        case SC_AST_RETURN:
            if (tc->current_result == SVM_TYPE_VOID) {
                if (n->as.ret.value != NULL) {
                    sc_error_set(tc->err, n->span, "void function returns a value");
                    return false;
                }
                return true;
            }
            if (n->as.ret.value == NULL) {
                sc_error_set(tc->err, n->span, "missing return value");
                return false;
            }
            if (!tc_expr(tc, n->as.ret.value)) {
                return false;
            }
            if (n->as.ret.value->type != tc->current_result) {
                sc_error_set(tc->err, n->span, "return type mismatch");
                return false;
            }
            return true;
        case SC_AST_BLOCK:
            for (uint32_t i = 0; i < n->as.block.count; i++) {
                if (!tc_stmt(tc, n->as.block.stmts[i])) {
                    return false;
                }
            }
            return true;
        case SC_AST_EXPR_STMT:
            return tc_expr(tc, n->as.expr);
        default:
            sc_error_set(tc->err, n->span, "invalid statement");
            return false;
    }
}

bool sc_mini_typecheck(ScModuleAst *module, ScError *err) {
    for (uint32_t i = 0; i < module->function_count; i++) {
        for (uint32_t j = i + 1; j < module->function_count; j++) {
            if (strcmp(module->functions[i].name, module->functions[j].name) == 0) {
                sc_error_set(err, module->functions[j].span, "duplicate function");
                return false;
            }
        }
    }
    for (uint32_t i = 0; i < module->function_count; i++) {
        ScFnDecl *fn = &module->functions[i];
        Tc tc;
        memset(&tc, 0, sizeof(tc));
        tc.module = module;
        tc.current_result = fn->result_type;
        tc.err = err;
        for (uint32_t p = 0; p < fn->param_count; p++) {
            if (!add_local(&tc, fn->params[p].name, fn->params[p].type, fn->span)) {
                return false;
            }
        }
        if (!tc_stmt(&tc, fn->body)) {
            return false;
        }
    }
    return true;
}
