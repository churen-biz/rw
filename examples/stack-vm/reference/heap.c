#include "heap.h"

#include <stdlib.h>
#include <string.h>

struct SvmHeapObject {
    SvmObjectKind kind;
    bool marked;
    uint32_t handle;
    uint32_t metadata;
    SvmType element_type;
    uint32_t value_count;
    size_t allocated_bytes;
    struct SvmHeapObject *next;
    SvmValue values[];
};

static bool heap_error(const char **error, const char *message) {
    if (error != NULL) *error = message;
    return false;
}

bool svm_heap_init(SvmHeap *heap, size_t byte_limit, uint32_t handle_limit) {
    if (heap == NULL || byte_limit == 0 || handle_limit == 0 ||
        handle_limit == UINT32_MAX) return false;
    memset(heap, 0, sizeof(*heap));
    heap->slots = calloc((size_t)handle_limit + 1u, sizeof(*heap->slots));
    if (heap->slots == NULL) return false;
    heap->slot_capacity = handle_limit + 1u;
    heap->next_handle = 1;
    heap->byte_limit = byte_limit;
    return true;
}

void svm_heap_destroy(SvmHeap *heap) {
    if (heap == NULL) return;
    SvmHeapObject *object = heap->objects;
    while (object != NULL) {
        SvmHeapObject *next = object->next;
        free(object);
        object = next;
    }
    free(heap->slots);
    memset(heap, 0, sizeof(*heap));
}

static bool allocate_object(
    SvmHeap *heap,
    SvmObjectKind kind,
    uint32_t metadata,
    SvmType element_type,
    uint32_t value_count,
    SvmValue *result,
    const char **error
) {
#if SIZE_MAX <= UINT32_MAX
    if (value_count > (SIZE_MAX - sizeof(SvmHeapObject)) / sizeof(SvmValue)) {
        return heap_error(error, "object size overflow");
    }
#endif
    size_t bytes = sizeof(SvmHeapObject) + (size_t)value_count * sizeof(SvmValue);
    if (bytes > heap->byte_limit || heap->bytes_used > heap->byte_limit - bytes) {
        return heap_error(error, "heap byte limit exceeded");
    }
    if (heap->next_handle >= heap->slot_capacity) {
        return heap_error(error, "handle limit exceeded");
    }
    uint32_t handle = heap->next_handle++;

    SvmHeapObject *object = calloc(1, bytes);
    if (object == NULL) return heap_error(error, "host allocation failed");
    object->kind = kind;
    object->handle = handle;
    object->metadata = metadata;
    object->element_type = element_type;
    object->value_count = value_count;
    object->allocated_bytes = bytes;
    object->next = heap->objects;
    heap->objects = object;
    heap->slots[handle] = object;
    heap->bytes_used += bytes;
    heap->live_objects++;

    result->type = SVM_TYPE_REF;
    result->as.ref = handle;
    return true;
}

bool svm_heap_new_instance(
    SvmHeap *heap,
    uint32_t type_index,
    const SvmType *field_types,
    uint32_t field_count,
    SvmValue *result,
    const char **error
) {
    if (field_count > 0 && field_types == NULL) {
        return heap_error(error, "missing field types");
    }
    if (!allocate_object(
        heap, SVM_OBJECT_INSTANCE, type_index, SVM_TYPE_UNINIT,
        field_count, result, error
    )) return false;
    SvmHeapObject *object = heap->slots[result->as.ref];
    for (uint32_t i = 0; i < field_count; i++) {
        object->values[i] = field_types[i] == SVM_TYPE_I32
            ? svm_i32(0)
            : field_types[i] == SVM_TYPE_BOOL ? svm_bool(false) : svm_null();
    }
    return true;
}

bool svm_heap_new_array(
    SvmHeap *heap,
    SvmType element_type,
    uint32_t length,
    SvmValue *result,
    const char **error
) {
    if (element_type == SVM_TYPE_UNINIT) {
        return heap_error(error, "invalid array element type");
    }
    if (!allocate_object(
            heap, SVM_OBJECT_ARRAY, 0, element_type, length, result, error)) {
        return false;
    }
    SvmHeapObject *object = heap->slots[result->as.ref];
    for (uint32_t i = 0; i < length; i++) {
        object->values[i] = element_type == SVM_TYPE_I32
            ? svm_i32(0)
            : element_type == SVM_TYPE_BOOL ? svm_bool(false) : svm_null();
    }
    return true;
}

bool svm_heap_new_closure(
    SvmHeap *heap,
    uint32_t function_index,
    const SvmValue *captures,
    uint32_t capture_count,
    SvmValue *result,
    const char **error
) {
    if (capture_count > 0 && captures == NULL) {
        return heap_error(error, "missing closure captures");
    }
    if (!allocate_object(
            heap, SVM_OBJECT_CLOSURE, function_index, SVM_TYPE_UNINIT,
            capture_count, result, error)) {
        return false;
    }
    SvmHeapObject *object = heap->slots[result->as.ref];
    if (capture_count > 0) {
        memcpy(object->values, captures, (size_t)capture_count * sizeof(*captures));
    }
    return true;
}

static bool resolve(
    SvmHeap *heap,
    SvmValue reference,
    SvmObjectKind expected,
    SvmHeapObject **result,
    const char **error
) {
    if (reference.type == SVM_TYPE_NULL) return heap_error(error, "null reference");
    if (reference.type != SVM_TYPE_REF || reference.as.ref == 0 ||
        reference.as.ref >= heap->slot_capacity ||
        heap->slots[reference.as.ref] == NULL) {
        return heap_error(error, "invalid reference handle");
    }
    SvmHeapObject *object = heap->slots[reference.as.ref];
    if (object->kind != expected) return heap_error(error, "wrong object kind");
    *result = object;
    return true;
}

bool svm_heap_get_field(
    SvmHeap *heap,
    SvmValue reference,
    uint32_t expected_type_index,
    uint32_t field_index,
    SvmValue *result,
    const char **error
) {
    SvmHeapObject *object;
    if (!resolve(heap, reference, SVM_OBJECT_INSTANCE, &object, error)) return false;
    if (object->metadata != expected_type_index) {
        return heap_error(error, "wrong instance type");
    }
    if (field_index >= object->value_count) {
        return heap_error(error, "field index out of bounds");
    }
    *result = object->values[field_index];
    return true;
}

bool svm_heap_set_field(
    SvmHeap *heap,
    SvmValue reference,
    uint32_t expected_type_index,
    uint32_t field_index,
    SvmValue value,
    const char **error
) {
    SvmHeapObject *object;
    if (!resolve(heap, reference, SVM_OBJECT_INSTANCE, &object, error)) return false;
    if (object->metadata != expected_type_index) {
        return heap_error(error, "wrong instance type");
    }
    if (field_index >= object->value_count) {
        return heap_error(error, "field index out of bounds");
    }
    object->values[field_index] = value;
    return true;
}

bool svm_heap_array_get(
    SvmHeap *heap,
    SvmValue reference,
    SvmType expected_element_type,
    uint32_t index,
    SvmValue *result,
    const char **error
) {
    SvmHeapObject *object;
    if (!resolve(heap, reference, SVM_OBJECT_ARRAY, &object, error)) return false;
    if (object->element_type != expected_element_type) {
        return heap_error(error, "wrong array element type");
    }
    if (index >= object->value_count) return heap_error(error, "array index out of bounds");
    *result = object->values[index];
    return true;
}

bool svm_heap_array_length(
    SvmHeap *heap,
    SvmValue reference,
    SvmType expected_element_type,
    uint32_t *length,
    const char **error
) {
    SvmHeapObject *object;
    if (!resolve(heap, reference, SVM_OBJECT_ARRAY, &object, error)) return false;
    if (object->element_type != expected_element_type) {
        return heap_error(error, "wrong array element type");
    }
    *length = object->value_count;
    return true;
}

bool svm_heap_array_set(
    SvmHeap *heap,
    SvmValue reference,
    SvmType expected_element_type,
    uint32_t index,
    SvmValue value,
    const char **error
) {
    SvmHeapObject *object;
    if (!resolve(heap, reference, SVM_OBJECT_ARRAY, &object, error)) return false;
    if (object->element_type != expected_element_type) {
        return heap_error(error, "wrong array element type");
    }
    if (index >= object->value_count) return heap_error(error, "array index out of bounds");
    if (value.type != object->element_type &&
        !(object->element_type == SVM_TYPE_REF && value.type == SVM_TYPE_NULL)) {
        return heap_error(error, "wrong array element type");
    }
    object->values[index] = value;
    return true;
}

bool svm_heap_closure_info(
    SvmHeap *heap,
    SvmValue reference,
    uint32_t *function_index,
    const SvmValue **captures,
    uint32_t *capture_count,
    const char **error
) {
    SvmHeapObject *object;
    if (!resolve(heap, reference, SVM_OBJECT_CLOSURE, &object, error)) return false;
    *function_index = object->metadata;
    *captures = object->values;
    *capture_count = object->value_count;
    return true;
}

void svm_heap_mark_value(SvmHeap *heap, SvmValue value) {
    if (value.type != SVM_TYPE_REF || value.as.ref == 0 ||
        value.as.ref >= heap->slot_capacity) {
        return;
    }
    SvmHeapObject *object = heap->slots[value.as.ref];
    if (object == NULL || object->marked) return;
    object->marked = true;
    for (uint32_t i = 0; i < object->value_count; i++) {
        svm_heap_mark_value(heap, object->values[i]);
    }
}

void svm_heap_sweep(SvmHeap *heap) {
    SvmHeapObject **link = &heap->objects;
    while (*link != NULL) {
        SvmHeapObject *object = *link;
        if (object->marked) {
            object->marked = false;
            link = &object->next;
        } else {
            *link = object->next;
            heap->slots[object->handle] = NULL;
            heap->bytes_used -= object->allocated_bytes;
            heap->live_objects--;
            free(object);
        }
    }
}

void svm_heap_collect(SvmHeap *heap, const SvmValue *roots, uint32_t root_count) {
    for (uint32_t i = 0; i < root_count; i++) {
        svm_heap_mark_value(heap, roots[i]);
    }
    svm_heap_sweep(heap);
}

SvmHeapStats svm_heap_stats(const SvmHeap *heap) {
    SvmHeapStats stats = {heap->bytes_used, heap->live_objects};
    return stats;
}
