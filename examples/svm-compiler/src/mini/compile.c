#include "sc_ir.h"
#include "sc_mini.h"

#include <stdio.h>
#include <stdlib.h>

bool sc_mini_compile_string(
    const char *path_for_diag,
    const char *source,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
) {
    ScModuleAst *ast = NULL;
    if (!sc_mini_parse(path_for_diag, source, arena, &ast, err)) {
        return false;
    }
    if (!sc_mini_typecheck(ast, err)) {
        return false;
    }
    ScIrModule *ir = NULL;
    if (!sc_ir_from_module(ast, arena, &ir, err)) {
        return false;
    }
    return sc_ir_lower(ir, arena, out_module, err);
}

bool sc_mini_compile_file(
    const char *path, ScArena *arena, SvmModule *out_module, ScError *err
) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ScSpan span = {.path = path, .line = 0, .column = 0};
        sc_error_set(err, span, "cannot open file");
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        sc_error_set(err, (ScSpan){.path = path}, "cannot seek");
        return false;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        sc_error_set(err, (ScSpan){.path = path}, "cannot read size");
        return false;
    }
    char *buf = malloc((size_t)size + 1u);
    if (buf == NULL) {
        fclose(fp);
        sc_error_set(err, (ScSpan){.path = path}, "out of memory");
        return false;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        free(buf);
        fclose(fp);
        sc_error_set(err, (ScSpan){.path = path}, "cannot read file");
        return false;
    }
    fclose(fp);
    buf[size] = '\0';
    bool ok = sc_mini_compile_string(path, buf, arena, out_module, err);
    free(buf);
    return ok;
}
