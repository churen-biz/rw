#include "module.h"
#include "sc_arena.h"
#include "sc_asm.h"
#include "sc_diag.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s INPUT.sasm OUTPUT.svm\n", argv[0]);
        return EXIT_FAILURE;
    }
    ScArena *arena = sc_arena_create();
    if (arena == NULL) {
        fputs("out of memory\n", stderr);
        return EXIT_FAILURE;
    }
    SvmModule module;
    ScError err = {0};
    if (!sc_assemble_file(argv[1], arena, &module, &err)) {
        sc_error_print(&err, stderr);
        sc_arena_destroy(arena);
        return EXIT_FAILURE;
    }
    SvmError write_err = {0};
    if (!svm_module_write_file(&module, argv[2], &write_err)) {
        fprintf(stderr, "write error: %s\n", write_err.message);
        sc_arena_destroy(arena);
        return EXIT_FAILURE;
    }
    sc_arena_destroy(arena);
    return EXIT_SUCCESS;
}
