#include "module.h"

#include <stdio.h>
#include <stdlib.h>

#define ARRAY_COUNT(values) ((uint32_t)(sizeof(values) / sizeof((values)[0])))
#define INS(opcode, operand) {opcode, operand, 0}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTPUT.svm\n", argv[0]);
        return EXIT_FAILURE;
    }
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, 40),
        INS(SVM_OP_CONST_I32, 2),
        INS(SVM_OP_I32_ADD, 0),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "main",
        .result_type = SVM_TYPE_I32,
        .max_stack = 2,
        .code = code,
        .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {
        .functions = functions,
        .function_count = ARRAY_COUNT(functions)
    };
    SvmError error = {0};
    if (!svm_module_write_file(&module, argv[1], &error)) {
        fprintf(stderr, "cannot create example: %s\n", error.message);
        return EXIT_FAILURE;
    }
    printf("wrote %s\n", argv[1]);
    return EXIT_SUCCESS;
}
