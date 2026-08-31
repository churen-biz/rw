#include "sc_asm.h"

#include <stdio.h>
#include <stdlib.h>

bool sc_parse_sasm(
    const char *path_for_diag,
    const char *source,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);

bool sc_assemble_string(
    const char *path_for_diag,
    const char *source,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
) {
    return sc_parse_sasm(path_for_diag, source, arena, out_module, err);
}

bool sc_assemble_file(
    const char *path,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
) {
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ScSpan span = {.path = path, .line = 0, .column = 0};
        sc_error_set(err, span, "cannot open file");
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        ScSpan span = {.path = path, .line = 0, .column = 0};
        sc_error_set(err, span, "cannot seek file");
        return false;
    }
    long size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        ScSpan span = {.path = path, .line = 0, .column = 0};
        sc_error_set(err, span, "cannot tell file size");
        return false;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        ScSpan span = {.path = path, .line = 0, .column = 0};
        sc_error_set(err, span, "cannot rewind file");
        return false;
    }
    char *buffer = malloc((size_t)size + 1u);
    if (buffer == NULL) {
        fclose(fp);
        ScSpan span = {.path = path, .line = 0, .column = 0};
        sc_error_set(err, span, "out of memory");
        return false;
    }
    size_t read_n = fread(buffer, 1, (size_t)size, fp);
    fclose(fp);
    if (read_n != (size_t)size) {
        free(buffer);
        ScSpan span = {.path = path, .line = 0, .column = 0};
        sc_error_set(err, span, "cannot read file");
        return false;
    }
    buffer[size] = '\0';
    bool ok = sc_assemble_string(path, buffer, arena, out_module, err);
    free(buffer);
    return ok;
}
