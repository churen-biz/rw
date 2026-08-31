#ifndef SC_ASM_H
#define SC_ASM_H

#include "sc_arena.h"
#include "sc_diag.h"
#include "svm.h"

#include <stdbool.h>

bool sc_assemble_string(
    const char *path_for_diag,
    const char *source,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);

bool sc_assemble_file(
    const char *path,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);

#endif
