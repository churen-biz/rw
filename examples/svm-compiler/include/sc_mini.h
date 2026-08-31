#ifndef SC_MINI_H
#define SC_MINI_H

#include "sc_arena.h"
#include "sc_diag.h"
#include "svm.h"

#include <stdbool.h>

typedef enum {
    SC_AST_INT,
    SC_AST_BOOL,
    SC_AST_VAR,
    SC_AST_BINARY,
    SC_AST_CALL,
    SC_AST_ASSIGN,
    SC_AST_LET,
    SC_AST_IF,
    SC_AST_WHILE,
    SC_AST_RETURN,
    SC_AST_BLOCK,
    SC_AST_EXPR_STMT
} ScAstKind;

typedef enum {
    SC_BIN_ADD,
    SC_BIN_SUB,
    SC_BIN_MUL,
    SC_BIN_LE,
    SC_BIN_EQ
} ScBinOp;

typedef struct ScAst ScAst;
typedef struct ScParam ScParam;
typedef struct ScFnDecl ScFnDecl;
typedef struct ScModuleAst ScModuleAst;

struct ScParam {
    char *name;
    SvmType type;
};

struct ScAst {
    ScAstKind kind;
    ScSpan span;
    SvmType type; /* filled by typechecker; UNINIT before */
    union {
        int32_t int_value;
        bool bool_value;
        char *name;
        struct {
            ScBinOp op;
            ScAst *left;
            ScAst *right;
        } binary;
        struct {
            char *callee;
            ScAst **args;
            uint32_t arg_count;
        } call;
        struct {
            char *name;
            ScAst *value;
        } assign;
        struct {
            char *name;
            SvmType type;
            ScAst *init;
            ScAst *body; /* unused; let is a statement */
        } let;
        struct {
            ScAst *cond;
            ScAst *then_branch;
            ScAst *else_branch; /* may be NULL */
        } if_stmt;
        struct {
            ScAst *cond;
            ScAst *body;
        } while_stmt;
        struct {
            ScAst *value; /* NULL for void return */
        } ret;
        struct {
            ScAst **stmts;
            uint32_t count;
        } block;
        ScAst *expr;
    } as;
};

struct ScFnDecl {
    char *name;
    ScParam *params;
    uint32_t param_count;
    SvmType result_type;
    ScAst *body;
    ScSpan span;
};

struct ScModuleAst {
    char *name;
    ScFnDecl *functions;
    uint32_t function_count;
};

bool sc_mini_parse(
    const char *path,
    const char *source,
    ScArena *arena,
    ScModuleAst **out_module,
    ScError *err
);

bool sc_mini_typecheck(ScModuleAst *module, ScError *err);

bool sc_mini_compile_string(
    const char *path_for_diag,
    const char *source,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);

bool sc_mini_compile_file(
    const char *path,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);

#endif
