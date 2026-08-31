#ifndef SVM_MODULE_H
#define SVM_MODULE_H

#include "svm.h"

typedef struct {
    SvmModule module;
    SvmFunction *owned_functions;
    SvmRecordType *owned_record_types;
    SvmHostImport *owned_host_imports;
} SvmOwnedModule;

bool svm_module_encode(
    const SvmModule *module,
    uint8_t **bytes,
    size_t *size,
    SvmError *error
);

bool svm_module_decode(
    const uint8_t *bytes,
    size_t size,
    SvmOwnedModule *owned,
    SvmError *error
);

bool svm_module_write_file(
    const SvmModule *module,
    const char *path,
    SvmError *error
);

bool svm_module_read_file(
    const char *path,
    SvmOwnedModule *owned,
    SvmError *error
);

void svm_module_free_bytes(uint8_t *bytes);
void svm_owned_module_destroy(SvmOwnedModule *owned);

#endif
