#include "module.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static bool parse_entry(const char *text, uint32_t *entry) {
    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return false;
    }
    *entry = (uint32_t)value;
    return true;
}

static void print_value(SvmValue value) {
    switch (value.type) {
        case SVM_TYPE_I32:
            printf("i32:%" PRId32 "\n", value.as.i32);
            break;
        case SVM_TYPE_BOOL:
            puts(value.as.boolean ? "bool:true" : "bool:false");
            break;
        case SVM_TYPE_NULL:
            puts("null");
            break;
        case SVM_TYPE_REF:
            printf("ref:%" PRIu32 "\n", value.as.ref);
            break;
        default:
            printf("value(type=%d)\n", (int)value.type);
            break;
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s MODULE.svm [ENTRY_INDEX]\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint32_t entry = 0;
    if (argc == 3 && !parse_entry(argv[2], &entry)) {
        fputs("invalid entry function index\n", stderr);
        return EXIT_FAILURE;
    }

    SvmOwnedModule owned;
    SvmError error = {0};
    if (!svm_module_read_file(argv[1], &owned, &error)) {
        fprintf(stderr, "load error: %s\n", error.message);
        return EXIT_FAILURE;
    }
    SvmLimits limits = {
        .instruction_budget = 1000000,
        .call_depth_limit = SVM_MAX_FRAMES,
        .heap_byte_limit = 8u * 1024u * 1024u,
        .handle_limit = 65536,
        .task_limit = SVM_MAX_TASKS,
        .channel_limit = SVM_MAX_CHANNELS,
        .scheduler_quantum = 1000,
        .host_call_budget = 10000
    };
    SvmValue result;
    bool ok = svm_execute(
        &owned.module, entry, NULL, 0, limits, &result, &error
    );
    if (ok) {
        print_value(result);
    } else {
        fprintf(
            stderr,
            "runtime error in function %" PRIu32 " at pc %" PRIu32 ": %s\n",
            error.function_index,
            error.pc,
            error.message
        );
    }
    svm_owned_module_destroy(&owned);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
