#ifndef SC_BUILDER_H
#define SC_BUILDER_H

#include "sc_arena.h"
#include "sc_diag.h"
#include "svm.h"

#include <stdbool.h>

typedef struct ScBuilder ScBuilder;

ScBuilder *sc_builder_create(ScArena *arena);

bool sc_builder_begin_func(
    ScBuilder *b,
    const char *name,
    const SvmType *param_types,
    uint32_t param_count,
    SvmType result_type,
    ScError *err
);

bool sc_builder_define_label(ScBuilder *b, const char *name, ScError *err);

bool sc_builder_emit(
    ScBuilder *b,
    SvmOpcode op,
    int32_t operand,
    int32_t operand2,
    ScError *err
);

bool sc_builder_emit_jump(
    ScBuilder *b,
    SvmOpcode op,
    const char *label,
    ScError *err
);

bool sc_builder_emit_call(ScBuilder *b, const char *func_name, ScError *err);

bool sc_builder_end_func(ScBuilder *b, ScError *err);

bool sc_builder_finish(ScBuilder *b, SvmModule *out_module, ScError *err);

#endif
