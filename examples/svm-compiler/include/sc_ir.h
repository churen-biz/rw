#ifndef SC_IR_H
#define SC_IR_H

#include "sc_arena.h"
#include "sc_diag.h"
#include "sc_mini.h"
#include "svm.h"

#include <stdbool.h>

typedef enum {
    SC_IR_CONST_I32,
    SC_IR_CONST_BOOL,
    SC_IR_LOAD,
    SC_IR_STORE,
    SC_IR_BINARY,
    SC_IR_CALL,
    SC_IR_POP,
    SC_IR_GOTO,
    SC_IR_BRANCH,
    SC_IR_RET,
    SC_IR_RET_VOID
} ScIrOp;

typedef struct ScIrInst {
    ScIrOp op;
    int32_t a;
    int32_t b;
    ScBinOp bin;
    char *call_name;
    uint32_t call_argc;
} ScIrInst;

typedef struct ScIrBlock {
    uint32_t id;
    ScIrInst *insts;
    uint32_t inst_count;
    uint32_t inst_capacity;
    bool terminated;
} ScIrBlock;

typedef struct ScIrFunc {
    char *name;
    SvmType *param_types;
    uint32_t param_count;
    SvmType result_type;
    uint32_t local_count;
    ScIrBlock *blocks;
    uint32_t block_count;
    uint32_t block_capacity;
} ScIrFunc;

typedef struct ScIrModule {
    ScIrFunc *functions;
    uint32_t function_count;
} ScIrModule;

bool sc_ir_from_module(
    ScModuleAst *ast,
    ScArena *arena,
    ScIrModule **out_ir,
    ScError *err
);

bool sc_ir_lower(
    ScIrModule *ir,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);

#endif
