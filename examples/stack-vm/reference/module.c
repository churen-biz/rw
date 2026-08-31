#include "module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SVM_MODULE_VERSION 1u
#define SVM_SECTION_COUNT 3u
#define SVM_HEADER_SIZE 12u
#define SVM_DIRECTORY_ENTRY_SIZE 12u
#define SVM_MAX_MODULE_ITEMS 65535u
#define SVM_MAX_CODE_INSTRUCTIONS 1000000u
#define SVM_MAX_MODULE_STRING 4096u

enum {
    SVM_SECTION_TYPES = 1,
    SVM_SECTION_FUNCTIONS = 2,
    SVM_SECTION_IMPORTS = 3
};

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} ByteBuffer;

typedef struct {
    const uint8_t *data;
    size_t size;
    size_t position;
} Cursor;

typedef struct {
    uint32_t kind;
    uint32_t offset;
    uint32_t size;
} SectionEntry;

static bool module_error(SvmError *error, const char *message) {
    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        (void)snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return false;
}

static bool buffer_reserve(ByteBuffer *buffer, size_t additional) {
    if (additional > SIZE_MAX - buffer->size) return false;
    size_t required = buffer->size + additional;
    if (required <= buffer->capacity) return true;
    size_t capacity = buffer->capacity == 0 ? 128u : buffer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = required;
            break;
        }
        capacity *= 2u;
    }
    uint8_t *resized = malloc(capacity);
    if (resized == NULL) return false;
    if (buffer->size > 0) memcpy(resized, buffer->data, buffer->size);
    free(buffer->data);
    buffer->data = resized;
    buffer->capacity = capacity;
    return true;
}

static bool buffer_u8(ByteBuffer *buffer, uint8_t value) {
    if (!buffer_reserve(buffer, 1)) return false;
    buffer->data[buffer->size++] = value;
    return true;
}

static bool buffer_u32(ByteBuffer *buffer, uint32_t value) {
    if (!buffer_reserve(buffer, 4)) return false;
    for (uint32_t i = 0; i < 4; i++) {
        buffer->data[buffer->size++] = (uint8_t)(value >> (i * 8u));
    }
    return true;
}

static bool buffer_bytes(ByteBuffer *buffer, const uint8_t *bytes, size_t count) {
    if (!buffer_reserve(buffer, count)) return false;
    if (count > 0) memcpy(buffer->data + buffer->size, bytes, count);
    buffer->size += count;
    return true;
}

static bool buffer_string(ByteBuffer *buffer, const char *text) {
    if (text == NULL) return false;
    size_t length = strlen(text);
    if (length > UINT32_MAX || length > SVM_MAX_MODULE_STRING) return false;
    return buffer_u32(buffer, (uint32_t)length) &&
           buffer_bytes(buffer, (const uint8_t *)text, length);
}

static bool buffer_sleb32(ByteBuffer *buffer, int32_t value) {
    int64_t remaining = value;
    for (;;) {
        uint8_t byte = (uint8_t)((uint64_t)remaining & 0x7fu);
        int64_t next = remaining / 128;
        if (remaining < 0 && remaining % 128 != 0) next--;
        bool sign = (byte & 0x40u) != 0;
        bool done = (next == 0 && !sign) || (next == -1 && sign);
        if (!done) byte |= 0x80u;
        if (!buffer_u8(buffer, byte)) return false;
        if (done) return true;
        remaining = next;
    }
}

static void buffer_destroy(ByteBuffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static void destroy_sections(ByteBuffer sections[SVM_SECTION_COUNT]) {
    /* Keep every owner explicit so both readers and static analyzers can see it. */
    buffer_destroy(&sections[2]);
    buffer_destroy(&sections[1]);
    buffer_destroy(&sections[0]);
}

static bool cursor_take(Cursor *cursor, size_t count, const uint8_t **bytes) {
    if (cursor->position > cursor->size) return false;
    if (count > cursor->size - cursor->position) return false;
    *bytes = cursor->data + cursor->position;
    cursor->position += count;
    return true;
}

static bool cursor_u8(Cursor *cursor, uint8_t *value) {
    const uint8_t *bytes;
    if (!cursor_take(cursor, 1, &bytes)) return false;
    *value = bytes[0];
    return true;
}

static bool cursor_u32(Cursor *cursor, uint32_t *value) {
    const uint8_t *bytes;
    if (!cursor_take(cursor, 4, &bytes)) return false;
    *value = (uint32_t)bytes[0] |
             ((uint32_t)bytes[1] << 8u) |
             ((uint32_t)bytes[2] << 16u) |
             ((uint32_t)bytes[3] << 24u);
    return true;
}

static bool cursor_sleb32(Cursor *cursor, int32_t *value) {
    uint64_t raw = 0;
    uint32_t shift = 0;
    uint8_t byte = 0;
    for (uint32_t count = 0; count < 5; count++) {
        if (!cursor_u8(cursor, &byte)) return false;
        raw |= (uint64_t)(byte & 0x7fu) << shift;
        shift += 7u;
        if ((byte & 0x80u) == 0) {
            int64_t signed_value = (byte & 0x40u) != 0
                ? (int64_t)(raw - (UINT64_C(1) << shift))
                : (int64_t)raw;
            if (signed_value < INT32_MIN || signed_value > INT32_MAX) return false;
            *value = (int32_t)signed_value;
            return true;
        }
    }
    return false;
}

static bool cursor_string(Cursor *cursor, char **text) {
    uint32_t length;
    if (!cursor_u32(cursor, &length) || length > SVM_MAX_MODULE_STRING) return false;
    const uint8_t *bytes;
    if (!cursor_take(cursor, length, &bytes)) return false;
    char *copy = malloc((size_t)length + 1u);
    if (copy == NULL) return false;
    if (length > 0) memcpy(copy, bytes, length);
    copy[length] = '\0';
    *text = copy;
    return true;
}

static bool encode_types(const SvmModule *module, ByteBuffer *section) {
    if (!buffer_u32(section, module->record_type_count)) return false;
    for (uint32_t i = 0; i < module->record_type_count; i++) {
        const SvmRecordType *record = &module->record_types[i];
        if (!buffer_string(section, record->name == NULL ? "" : record->name) ||
            !buffer_u32(section, record->field_count)) return false;
        for (uint32_t field = 0; field < record->field_count; field++) {
            if (!buffer_u8(section, (uint8_t)record->field_types[field])) return false;
        }
    }
    return true;
}

static bool encode_functions(const SvmModule *module, ByteBuffer *section) {
    if (!buffer_u32(section, module->function_count)) return false;
    for (uint32_t i = 0; i < module->function_count; i++) {
        const SvmFunction *function = &module->functions[i];
        if (!buffer_string(section, function->name == NULL ? "" : function->name) ||
            !buffer_u32(section, function->parameter_count)) return false;
        for (uint32_t parameter = 0; parameter < function->parameter_count; parameter++) {
            if (!buffer_u8(section, (uint8_t)function->parameter_types[parameter])) return false;
        }
        if (!buffer_u32(section, function->capture_count) ||
            !buffer_u8(section, (uint8_t)function->result_type) ||
            !buffer_u32(section, function->local_count) ||
            !buffer_u32(section, function->max_stack) ||
            !buffer_u32(section, function->code_count)) return false;
        for (uint32_t pc = 0; pc < function->code_count; pc++) {
            const SvmInstruction *instruction = &function->code[pc];
            if ((uint32_t)instruction->opcode > UINT8_MAX ||
                !buffer_u8(section, (uint8_t)instruction->opcode) ||
                !buffer_sleb32(section, instruction->operand) ||
                !buffer_sleb32(section, instruction->operand2)) return false;
        }
        if (!buffer_u32(section, function->handler_count)) return false;
        for (uint32_t handler = 0; handler < function->handler_count; handler++) {
            const SvmExceptionHandler *entry = &function->handlers[handler];
            if (!buffer_u32(section, entry->start_pc) ||
                !buffer_u32(section, entry->end_pc) ||
                !buffer_u32(section, entry->handler_pc)) return false;
        }
    }
    return true;
}

static bool encode_imports(const SvmModule *module, ByteBuffer *section) {
    if (!buffer_u32(section, module->host_import_count)) return false;
    for (uint32_t i = 0; i < module->host_import_count; i++) {
        const SvmHostImport *import = &module->host_imports[i];
        if (!buffer_string(section, import->capability_name) ||
            !buffer_string(section, import->function_name) ||
            !buffer_u32(section, import->parameter_count)) return false;
        for (uint32_t parameter = 0; parameter < import->parameter_count; parameter++) {
            if (!buffer_u8(section, (uint8_t)import->parameter_types[parameter])) return false;
        }
        if (!buffer_u8(section, (uint8_t)import->result_type) ||
            !buffer_u8(section, import->asynchronous ? 1u : 0u)) return false;
    }
    return true;
}

bool svm_module_encode(
    const SvmModule *module,
    uint8_t **bytes,
    size_t *size,
    SvmError *error
) {
    if (bytes == NULL || size == NULL || !svm_verify_module(module, error)) return false;
    *bytes = NULL;
    *size = 0;
    ByteBuffer sections[SVM_SECTION_COUNT] = {{0}};
    if (!encode_types(module, &sections[0])) {
        buffer_destroy(&sections[0]);
        return module_error(error, "failed to encode module");
    }
    if (!encode_functions(module, &sections[1])) {
        buffer_destroy(&sections[1]);
        buffer_destroy(&sections[0]);
        return module_error(error, "failed to encode module");
    }
    if (!encode_imports(module, &sections[2])) {
        buffer_destroy(&sections[2]);
        buffer_destroy(&sections[1]);
        buffer_destroy(&sections[0]);
        return module_error(error, "failed to encode module");
    }

    size_t prefix = SVM_HEADER_SIZE +
                    SVM_SECTION_COUNT * SVM_DIRECTORY_ENTRY_SIZE;
    size_t total = prefix;
    for (uint32_t i = 0; i < SVM_SECTION_COUNT; i++) {
        if (sections[i].size > UINT32_MAX || sections[i].size > SIZE_MAX - total) {
            destroy_sections(sections);
            return module_error(error, "encoded module is too large");
        }
        total += sections[i].size;
    }
    if (total > UINT32_MAX) {
        destroy_sections(sections);
        return module_error(error, "encoded module exceeds 4 GiB");
    }

    ByteBuffer output = {0};
    const uint8_t magic[] = {'S', 'V', 'M', 0};
    bool ok = buffer_bytes(&output, magic, sizeof(magic)) &&
              buffer_u32(&output, SVM_MODULE_VERSION) &&
              buffer_u32(&output, SVM_SECTION_COUNT);
    uint32_t offset = (uint32_t)prefix;
    for (uint32_t i = 0; i < SVM_SECTION_COUNT && ok; i++) {
        ok = buffer_u32(&output, i + 1u) && buffer_u32(&output, offset) &&
             buffer_u32(&output, (uint32_t)sections[i].size);
        offset += (uint32_t)sections[i].size;
    }
    for (uint32_t i = 0; i < SVM_SECTION_COUNT && ok; i++) {
        ok = buffer_bytes(&output, sections[i].data, sections[i].size);
    }
    destroy_sections(sections);
    if (!ok) {
        buffer_destroy(&output);
        return module_error(error, "failed to assemble encoded module");
    }
    *bytes = output.data;
    *size = output.size;
    return true;
}

static void destroy_function(SvmFunction *function) {
    free((char *)function->name);
    free((SvmType *)function->parameter_types);
    free((SvmInstruction *)function->code);
    free((SvmExceptionHandler *)function->handlers);
}

void svm_owned_module_destroy(SvmOwnedModule *owned) {
    if (owned == NULL) return;
    for (uint32_t i = 0; i < owned->module.function_count; i++) {
        destroy_function(&owned->owned_functions[i]);
    }
    for (uint32_t i = 0; i < owned->module.record_type_count; i++) {
        free((char *)owned->owned_record_types[i].name);
        free((SvmType *)owned->owned_record_types[i].field_types);
    }
    for (uint32_t i = 0; i < owned->module.host_import_count; i++) {
        free((char *)owned->owned_host_imports[i].capability_name);
        free((char *)owned->owned_host_imports[i].function_name);
        free((SvmType *)owned->owned_host_imports[i].parameter_types);
    }
    free(owned->owned_functions);
    free(owned->owned_record_types);
    free(owned->owned_host_imports);
    memset(owned, 0, sizeof(*owned));
}

static bool read_count(Cursor *cursor, uint32_t maximum, uint32_t *count) {
    return cursor_u32(cursor, count) && *count <= maximum;
}

static bool decode_types(Cursor *cursor, SvmOwnedModule *owned) {
    uint32_t count;
    if (!read_count(cursor, SVM_MAX_MODULE_ITEMS, &count)) return false;
    owned->owned_record_types = calloc(count, sizeof(*owned->owned_record_types));
    if (count > 0 && owned->owned_record_types == NULL) return false;
    owned->module.record_types = owned->owned_record_types;
    owned->module.record_type_count = count;
    for (uint32_t i = 0; i < count; i++) {
        SvmRecordType *record = &owned->owned_record_types[i];
        uint32_t field_count;
        char *name = NULL;
        if (!cursor_string(cursor, &name)) return false;
        record->name = name;
        if (!read_count(cursor, SVM_MAX_LOCALS, &field_count)) return false;
        SvmType *field_types = calloc(field_count, sizeof(*field_types));
        if (field_count > 0 && field_types == NULL) return false;
        record->field_types = field_types;
        record->field_count = field_count;
        for (uint32_t field = 0; field < field_count; field++) {
            uint8_t type;
            if (!cursor_u8(cursor, &type)) return false;
            field_types[field] = (SvmType)type;
        }
    }
    return cursor->position == cursor->size;
}

static bool decode_functions(Cursor *cursor, SvmOwnedModule *owned) {
    uint32_t count;
    if (!read_count(cursor, SVM_MAX_MODULE_ITEMS, &count)) return false;
    owned->owned_functions = calloc(count, sizeof(*owned->owned_functions));
    if (count > 0 && owned->owned_functions == NULL) return false;
    owned->module.functions = owned->owned_functions;
    owned->module.function_count = count;
    for (uint32_t i = 0; i < count; i++) {
        SvmFunction *function = &owned->owned_functions[i];
        uint32_t parameter_count;
        char *name = NULL;
        if (!cursor_string(cursor, &name)) return false;
        function->name = name;
        if (!read_count(cursor, SVM_MAX_LOCALS, &parameter_count)) return false;
        SvmType *parameter_types = calloc(parameter_count, sizeof(*parameter_types));
        if (parameter_count > 0 && parameter_types == NULL) return false;
        function->parameter_types = parameter_types;
        function->parameter_count = parameter_count;
        for (uint32_t parameter = 0; parameter < parameter_count; parameter++) {
            uint8_t type;
            if (!cursor_u8(cursor, &type)) return false;
            parameter_types[parameter] = (SvmType)type;
        }
        uint8_t result_type;
        if (!cursor_u32(cursor, &function->capture_count) ||
            !cursor_u8(cursor, &result_type) ||
            !cursor_u32(cursor, &function->local_count) ||
            !cursor_u32(cursor, &function->max_stack) ||
            !read_count(cursor, SVM_MAX_CODE_INSTRUCTIONS, &function->code_count)) {
            return false;
        }
        function->result_type = (SvmType)result_type;
        SvmInstruction *code = calloc(function->code_count, sizeof(*code));
        if (function->code_count > 0 && code == NULL) return false;
        function->code = code;
        for (uint32_t pc = 0; pc < function->code_count; pc++) {
            uint8_t opcode;
            if (!cursor_u8(cursor, &opcode) ||
                !cursor_sleb32(cursor, &code[pc].operand) ||
                !cursor_sleb32(cursor, &code[pc].operand2)) return false;
            code[pc].opcode = (SvmOpcode)opcode;
        }
        if (!read_count(cursor, SVM_MAX_CODE_INSTRUCTIONS, &function->handler_count)) {
            return false;
        }
        SvmExceptionHandler *handlers = calloc(
            function->handler_count, sizeof(*handlers)
        );
        if (function->handler_count > 0 && handlers == NULL) return false;
        function->handlers = handlers;
        for (uint32_t handler = 0; handler < function->handler_count; handler++) {
            if (!cursor_u32(cursor, &handlers[handler].start_pc) ||
                !cursor_u32(cursor, &handlers[handler].end_pc) ||
                !cursor_u32(cursor, &handlers[handler].handler_pc)) return false;
        }
    }
    return cursor->position == cursor->size;
}

static bool decode_imports(Cursor *cursor, SvmOwnedModule *owned) {
    uint32_t count;
    if (!read_count(cursor, SVM_MAX_HOST_IMPORTS, &count)) return false;
    owned->owned_host_imports = calloc(count, sizeof(*owned->owned_host_imports));
    if (count > 0 && owned->owned_host_imports == NULL) return false;
    owned->module.host_imports = owned->owned_host_imports;
    owned->module.host_import_count = count;
    for (uint32_t i = 0; i < count; i++) {
        SvmHostImport *import = &owned->owned_host_imports[i];
        char *capability_name = NULL;
        char *function_name = NULL;
        uint32_t parameter_count;
        if (!cursor_string(cursor, &capability_name)) return false;
        import->capability_name = capability_name;
        if (!cursor_string(cursor, &function_name)) return false;
        import->function_name = function_name;
        if (!read_count(cursor, SVM_MAX_LOCALS, &parameter_count)) return false;
        SvmType *parameter_types = calloc(parameter_count, sizeof(*parameter_types));
        if (parameter_count > 0 && parameter_types == NULL) return false;
        import->parameter_types = parameter_types;
        import->parameter_count = parameter_count;
        for (uint32_t parameter = 0; parameter < parameter_count; parameter++) {
            uint8_t type;
            if (!cursor_u8(cursor, &type)) return false;
            parameter_types[parameter] = (SvmType)type;
        }
        uint8_t result_type;
        uint8_t asynchronous;
        if (!cursor_u8(cursor, &result_type) || !cursor_u8(cursor, &asynchronous) ||
            asynchronous > 1u) return false;
        import->result_type = (SvmType)result_type;
        import->asynchronous = asynchronous != 0;
    }
    return cursor->position == cursor->size;
}

static bool ranges_overlap(const SectionEntry *left, const SectionEntry *right) {
    uint64_t left_end = (uint64_t)left->offset + left->size;
    uint64_t right_end = (uint64_t)right->offset + right->size;
    return left->offset < right_end && right->offset < left_end;
}

bool svm_module_decode(
    const uint8_t *bytes,
    size_t size,
    SvmOwnedModule *owned,
    SvmError *error
) {
    if (owned == NULL) return module_error(error, "missing output module");
    if (error != NULL) memset(error, 0, sizeof(*error));
    memset(owned, 0, sizeof(*owned));
    size_t prefix = SVM_HEADER_SIZE +
                    SVM_SECTION_COUNT * SVM_DIRECTORY_ENTRY_SIZE;
    if (bytes == NULL || size < prefix || memcmp(bytes, "SVM\0", 4) != 0) {
        return module_error(error, "invalid module header");
    }
    Cursor header = {bytes + 4, size - 4, 0};
    uint32_t version;
    uint32_t section_count;
    if (!cursor_u32(&header, &version) || !cursor_u32(&header, &section_count) ||
        version != SVM_MODULE_VERSION || section_count != SVM_SECTION_COUNT) {
        return module_error(error, "unsupported module version or section count");
    }
    SectionEntry entries[SVM_SECTION_COUNT] = {{0}};
    bool seen[SVM_SECTION_COUNT] = {false};
    for (uint32_t i = 0; i < SVM_SECTION_COUNT; i++) {
        SectionEntry entry;
        if (!cursor_u32(&header, &entry.kind) ||
            !cursor_u32(&header, &entry.offset) ||
            !cursor_u32(&header, &entry.size) ||
            entry.kind == 0 || entry.kind > SVM_SECTION_COUNT ||
            seen[entry.kind - 1u] || entry.offset < prefix ||
            (uint64_t)entry.offset + entry.size > size) {
            return module_error(error, "invalid module section directory");
        }
        seen[entry.kind - 1u] = true;
        entries[entry.kind - 1u] = entry;
    }
    for (uint32_t i = 0; i < SVM_SECTION_COUNT; i++) {
        for (uint32_t j = i + 1; j < SVM_SECTION_COUNT; j++) {
            if (ranges_overlap(&entries[i], &entries[j])) {
                return module_error(error, "overlapping module sections");
            }
        }
    }

    Cursor type_cursor = {
        bytes + entries[0].offset, entries[0].size, 0
    };
    Cursor function_cursor = {
        bytes + entries[1].offset, entries[1].size, 0
    };
    Cursor import_cursor = {
        bytes + entries[2].offset, entries[2].size, 0
    };
    if (!decode_types(&type_cursor, owned) ||
        !decode_functions(&function_cursor, owned) ||
        !decode_imports(&import_cursor, owned) ||
        !svm_verify_module(&owned->module, error)) {
        svm_owned_module_destroy(owned);
        if (error == NULL || error->message[0] == '\0') {
            return module_error(error, "invalid module section contents");
        }
        return false;
    }
    return true;
}

bool svm_module_write_file(
    const SvmModule *module,
    const char *path,
    SvmError *error
) {
    uint8_t *bytes;
    size_t size;
    if (path == NULL || !svm_module_encode(module, &bytes, &size, error)) return false;
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        free(bytes);
        return module_error(error, "cannot open module output file");
    }
    bool write_ok = fwrite(bytes, 1, size, file) == size;
    bool close_ok = fclose(file) == 0;
    free(bytes);
    return (write_ok && close_ok) ||
           module_error(error, "failed to write complete module file");
}

bool svm_module_read_file(
    const char *path,
    SvmOwnedModule *owned,
    SvmError *error
) {
    if (path == NULL || owned == NULL) return module_error(error, "missing module path");
    FILE *file = fopen(path, "rb");
    if (file == NULL) return module_error(error, "cannot open module file");
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return module_error(error, "cannot seek module file");
    }
    long file_size = ftell(file);
    if (file_size < 0 || (uint64_t)file_size > UINT32_MAX ||
        fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return module_error(error, "invalid module file size");
    }
    uint8_t *bytes = malloc((size_t)file_size);
    if (file_size > 0 && bytes == NULL) {
        (void)fclose(file);
        return module_error(error, "out of memory reading module");
    }
    bool read_ok = fread(bytes, 1, (size_t)file_size, file) == (size_t)file_size;
    bool close_ok = fclose(file) == 0;
    if (!read_ok || !close_ok) {
        free(bytes);
        return module_error(error, "failed to read complete module file");
    }
    bool ok = svm_module_decode(bytes, (size_t)file_size, owned, error);
    free(bytes);
    return ok;
}

void svm_module_free_bytes(uint8_t *bytes) {
    free(bytes);
}
