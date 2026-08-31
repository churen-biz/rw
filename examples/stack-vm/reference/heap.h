#ifndef SVM_HEAP_H
#define SVM_HEAP_H

#include "svm.h"

typedef enum {
    SVM_OBJECT_INSTANCE,
    SVM_OBJECT_ARRAY,
    SVM_OBJECT_CLOSURE
} SvmObjectKind;

typedef struct SvmHeapObject SvmHeapObject;

typedef struct {
    SvmHeapObject **slots;
    uint32_t slot_capacity;
    uint32_t next_handle;
    SvmHeapObject *objects;
    size_t bytes_used;
    size_t byte_limit;
    uint32_t live_objects;
} SvmHeap;

typedef struct {
    size_t bytes_used;
    uint32_t live_objects;
} SvmHeapStats;

bool svm_heap_init(SvmHeap *heap, size_t byte_limit, uint32_t handle_limit);
void svm_heap_destroy(SvmHeap *heap);

bool svm_heap_new_instance(
    SvmHeap *heap,
    uint32_t type_index,
    const SvmType *field_types,
    uint32_t field_count,
    SvmValue *result,
    const char **error
);

bool svm_heap_new_array(
    SvmHeap *heap,
    SvmType element_type,
    uint32_t length,
    SvmValue *result,
    const char **error
);

bool svm_heap_new_closure(
    SvmHeap *heap,
    uint32_t function_index,
    const SvmValue *captures,
    uint32_t capture_count,
    SvmValue *result,
    const char **error
);

bool svm_heap_get_field(
    SvmHeap *heap,
    SvmValue reference,
    uint32_t expected_type_index,
    uint32_t field_index,
    SvmValue *result,
    const char **error
);

bool svm_heap_set_field(
    SvmHeap *heap,
    SvmValue reference,
    uint32_t expected_type_index,
    uint32_t field_index,
    SvmValue value,
    const char **error
);

bool svm_heap_array_get(
    SvmHeap *heap,
    SvmValue reference,
    SvmType expected_element_type,
    uint32_t index,
    SvmValue *result,
    const char **error
);

bool svm_heap_array_length(
    SvmHeap *heap,
    SvmValue reference,
    SvmType expected_element_type,
    uint32_t *length,
    const char **error
);

bool svm_heap_array_set(
    SvmHeap *heap,
    SvmValue reference,
    SvmType expected_element_type,
    uint32_t index,
    SvmValue value,
    const char **error
);

bool svm_heap_closure_info(
    SvmHeap *heap,
    SvmValue reference,
    uint32_t *function_index,
    const SvmValue **captures,
    uint32_t *capture_count,
    const char **error
);

void svm_heap_collect(SvmHeap *heap, const SvmValue *roots, uint32_t root_count);
void svm_heap_mark_value(SvmHeap *heap, SvmValue value);
void svm_heap_sweep(SvmHeap *heap);
SvmHeapStats svm_heap_stats(const SvmHeap *heap);

#endif
