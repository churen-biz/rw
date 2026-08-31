#include "heap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(bool condition, const char *name, const char *detail) {
    if (!condition) {
        fprintf(stderr, "FAIL %-28s %s\n", name, detail);
        failures++;
    }
}

static void test_objects_and_gc(void) {
    const char *name = "objects and transitive GC";
    const char *error = NULL;
    SvmHeap heap;
    check(svm_heap_init(&heap, 4096, 16), name, "heap init failed");

    SvmValue parent;
    SvmValue child;
    const SvmType parent_fields[] = {SVM_TYPE_REF, SVM_TYPE_I32};
    const SvmType child_fields[] = {SVM_TYPE_I32};
    check(svm_heap_new_instance(&heap, 0, parent_fields, 2, &parent, &error), name, "parent allocation failed");
    check(svm_heap_new_instance(&heap, 0, child_fields, 1, &child, &error), name, "child allocation failed");
    uint32_t stable_handle = child.as.ref;
    check(svm_heap_set_field(&heap, parent, 0, 0, child, &error), name, "reference field write failed");
    check(svm_heap_set_field(&heap, parent, 0, 1, svm_i32(42), &error), name, "integer field write failed");

    svm_heap_collect(&heap, &parent, 1);
    SvmHeapStats stats = svm_heap_stats(&heap);
    check(stats.live_objects == 2, name, "reachable child was collected");

    SvmValue loaded;
    check(svm_heap_get_field(&heap, parent, 0, 0, &loaded, &error), name, "field read failed");
    check(loaded.as.ref == stable_handle, name, "stable handle changed");

    svm_heap_collect(&heap, NULL, 0);
    stats = svm_heap_stats(&heap);
    check(stats.live_objects == 0 && stats.bytes_used == 0, name, "garbage survived");
    check(!svm_heap_get_field(&heap, child, 0, 0, &loaded, &error), name,
          "dead handle still resolved");
    SvmValue replacement;
    check(svm_heap_new_instance(&heap, 0, child_fields, 1, &replacement, &error), name,
          "replacement allocation failed");
    check(replacement.as.ref != stable_handle, name, "stale handle was reused");
    svm_heap_destroy(&heap);
}

static void test_arrays(void) {
    const char *name = "typed arrays";
    const char *error = NULL;
    SvmHeap heap;
    check(svm_heap_init(&heap, 4096, 16), name, "heap init failed");
    SvmValue array;
    SvmValue loaded;
    check(svm_heap_new_array(&heap, SVM_TYPE_I32, 3, &array, &error), name, "array allocation failed");
    check(svm_heap_array_set(&heap, array, SVM_TYPE_I32, 1, svm_i32(7), &error), name, "array write failed");
    check(svm_heap_array_get(&heap, array, SVM_TYPE_I32, 1, &loaded, &error), name, "array read failed");
    check(loaded.type == SVM_TYPE_I32 && loaded.as.i32 == 7, name,
          "array returned wrong value");
    check(!svm_heap_array_set(&heap, array, SVM_TYPE_I32, 0, svm_bool(true), &error), name,
          "array accepted wrong element type");
    check(!svm_heap_array_get(&heap, array, SVM_TYPE_I32, 3, &loaded, &error), name,
          "array accepted out-of-bounds index");
    svm_heap_destroy(&heap);
}

static void test_closures(void) {
    const char *name = "closure captures";
    const char *error = NULL;
    SvmHeap heap;
    check(svm_heap_init(&heap, 4096, 16), name, "heap init failed");
    SvmValue captures[] = {svm_i32(9), svm_bool(true)};
    SvmValue closure;
    check(svm_heap_new_closure(&heap, 4, captures, 2, &closure, &error), name,
          "closure allocation failed");

    uint32_t function_index = 0;
    uint32_t capture_count = 0;
    const SvmValue *loaded = NULL;
    check(svm_heap_closure_info(
        &heap, closure, &function_index, &loaded, &capture_count, &error
    ), name, "closure lookup failed");
    check(function_index == 4 && capture_count == 2, name, "wrong closure metadata");
    check(loaded != NULL && loaded[0].as.i32 == 9 && loaded[1].as.boolean, name,
          "wrong closure captures");
    svm_heap_destroy(&heap);
}

static void test_quotas(void) {
    const char *name = "heap quotas";
    const char *error = NULL;
    SvmHeap heap;
    check(svm_heap_init(&heap, 4096, 2), name, "heap init failed");
    SvmValue first;
    SvmValue second;
    SvmValue third;
    check(svm_heap_new_instance(&heap, 0, NULL, 0, &first, &error), name,
          "first allocation failed");
    check(svm_heap_new_instance(&heap, 0, NULL, 0, &second, &error), name,
          "second allocation failed");
    check(!svm_heap_new_instance(&heap, 0, NULL, 0, &third, &error), name,
          "handle quota was not enforced");
    check(error != NULL && strstr(error, "handle") != NULL, name,
          "wrong quota error");
    svm_heap_destroy(&heap);

    check(svm_heap_init(&heap, 1, 4), name, "small heap init failed");
    error = NULL;
    check(!svm_heap_new_instance(&heap, 0, NULL, 0, &first, &error), name,
          "byte quota was not enforced");
    check(error != NULL && strstr(error, "byte") != NULL, name,
          "wrong byte quota error");
    svm_heap_destroy(&heap);
}

int main(void) {
    test_objects_and_gc();
    test_arrays();
    test_closures();
    test_quotas();
    if (failures != 0) {
        fprintf(stderr, "%d heap test(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("managed heap: all tests passed\n");
    return EXIT_SUCCESS;
}
