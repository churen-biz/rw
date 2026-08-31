#include "module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ARRAY_COUNT(values) ((uint32_t)(sizeof(values) / sizeof((values)[0])))
#define INS(opcode, operand) {opcode, operand, 0}

static int failures;

static void fail(const char *test, const char *message) {
    fprintf(stderr, "FAIL %-28s %s\n", test, message);
    failures++;
}

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8u) |
           ((uint32_t)bytes[2] << 16u) |
           ((uint32_t)bytes[3] << 24u);
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (uint32_t i = 0; i < 4; i++) bytes[i] = (uint8_t)(value >> (i * 8u));
}

static SvmModule example_module(void) {
    static const SvmType fields[] = {SVM_TYPE_I32, SVM_TYPE_BOOL};
    static const SvmRecordType records[] = {{
        .name = "Pair",
        .field_types = fields,
        .field_count = ARRAY_COUNT(fields)
    }};
    static const SvmInstruction code[] = {
        INS(SVM_OP_CONST_I32, -123456),
        INS(SVM_OP_RETURN, 0)
    };
    static const SvmFunction functions[] = {{
        .name = "negative",
        .result_type = SVM_TYPE_I32,
        .max_stack = 1,
        .code = code,
        .code_count = ARRAY_COUNT(code)
    }};
    SvmModule module = {
        .functions = functions,
        .function_count = ARRAY_COUNT(functions),
        .record_types = records,
        .record_type_count = ARRAY_COUNT(records)
    };
    return module;
}

static bool encode_example(uint8_t **bytes, size_t *size) {
    SvmModule module = example_module();
    SvmError error = {0};
    if (svm_module_encode(&module, bytes, size, &error)) return true;
    fail("encode fixture", error.message);
    return false;
}

static void test_memory_round_trip(void) {
    const char *name = "module memory round trip";
    uint8_t *bytes = NULL;
    size_t size = 0;
    if (!encode_example(&bytes, &size)) return;

    SvmOwnedModule owned;
    SvmError error = {0};
    if (!svm_module_decode(bytes, size, &owned, &error)) {
        fail(name, error.message);
        svm_module_free_bytes(bytes);
        return;
    }
    SvmLimits limits = {.instruction_budget = 10, .call_depth_limit = 4};
    SvmValue result;
    if (!svm_execute(&owned.module, 0, NULL, 0, limits, &result, &error)) {
        fail(name, error.message);
    } else if (result.type != SVM_TYPE_I32 || result.as.i32 != -123456) {
        fail(name, "signed LEB128 operand changed during round trip");
    } else if (owned.module.record_type_count != 1 ||
               strcmp(owned.module.record_types[0].name, "Pair") != 0) {
        fail(name, "type section changed during round trip");
    }
    svm_owned_module_destroy(&owned);
    svm_module_free_bytes(bytes);
}

static void test_file_round_trip(void) {
    const char *name = "module file round trip";
    char path[128];
    (void)snprintf(path, sizeof(path), "/tmp/svm-module-%ld.svm", (long)getpid());
    SvmModule module = example_module();
    SvmError error = {0};
    if (!svm_module_write_file(&module, path, &error)) {
        fail(name, error.message);
        return;
    }
    SvmOwnedModule owned;
    if (!svm_module_read_file(path, &owned, &error)) {
        fail(name, error.message);
    } else {
        if (owned.module.function_count != 1 ||
            strcmp(owned.module.functions[0].name, "negative") != 0) {
            fail(name, "function metadata changed during file round trip");
        }
        svm_owned_module_destroy(&owned);
    }
    if (remove(path) != 0) fail(name, "could not remove temporary module");
}

static void expect_decode_failure(
    const char *name,
    const uint8_t *bytes,
    size_t size
) {
    SvmOwnedModule owned;
    SvmError error = {0};
    if (svm_module_decode(bytes, size, &owned, &error)) {
        fail(name, "malformed input was accepted");
        svm_owned_module_destroy(&owned);
    } else if (error.message[0] == '\0') {
        fail(name, "decoder did not explain the failure");
    }
}

static void test_rejects_bad_magic_and_truncation(void) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    if (!encode_example(&bytes, &size)) return;
    uint8_t saved = bytes[0];
    bytes[0] = 'X';
    expect_decode_failure("reject bad module magic", bytes, size);
    bytes[0] = saved;
    expect_decode_failure("reject truncated module", bytes, size - 1u);
    svm_module_free_bytes(bytes);
}

static void test_rejects_overlapping_sections(void) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    if (!encode_example(&bytes, &size)) return;
    uint32_t types_offset = read_u32(bytes + 16u);
    write_u32(bytes + 28u, types_offset);
    expect_decode_failure("reject section overlap", bytes, size);
    svm_module_free_bytes(bytes);
}

static void test_rejects_unknown_opcode(void) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    if (!encode_example(&bytes, &size)) return;

    uint32_t functions_offset = read_u32(bytes + 28u);
    size_t cursor = functions_offset;
    cursor += 4u; /* function count */
    uint32_t name_length = read_u32(bytes + cursor);
    cursor += 4u + name_length;
    uint32_t parameter_count = read_u32(bytes + cursor);
    cursor += 4u + parameter_count;
    cursor += 4u + 1u + 4u + 4u; /* captures, result, locals, max stack */
    uint32_t code_count = read_u32(bytes + cursor);
    cursor += 4u;
    if (code_count == 0 || cursor >= size) {
        fail("reject unknown opcode", "test could not locate first opcode");
    } else {
        bytes[cursor] = UINT8_MAX;
        expect_decode_failure("reject unknown opcode", bytes, size);
    }
    svm_module_free_bytes(bytes);
}

int main(void) {
    test_memory_round_trip();
    test_file_round_trip();
    test_rejects_bad_magic_and_truncation();
    test_rejects_overlapping_sections();
    test_rejects_unknown_opcode();

    if (failures != 0) {
        fprintf(stderr, "%d module test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    puts("all module tests passed");
    return EXIT_SUCCESS;
}
