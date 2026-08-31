#ifndef SVM_H
#define SVM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVM_MAX_LOCALS 32u
#define SVM_MAX_STACK 64u
#define SVM_MAX_FRAMES 64u
#define SVM_MAX_DEFERS 16u
#define SVM_MAX_TASKS 32u
#define SVM_MAX_CHANNELS 32u
#define SVM_MAX_CHANNEL_CAPACITY 16u
#define SVM_MAX_SELECT_CASES 8u
#define SVM_MAX_HOST_IMPORTS 32u
#define SVM_ERROR_SIZE 256u

typedef enum {
    SVM_TYPE_UNINIT,
    SVM_TYPE_I32,
    SVM_TYPE_BOOL,
    SVM_TYPE_NULL,
    SVM_TYPE_REF,
    SVM_TYPE_ANY,
    SVM_TYPE_VOID,
    SVM_TYPE_TASK,
    SVM_TYPE_CHANNEL
} SvmType;

typedef struct {
    SvmType type;
    union {
        int32_t i32;
        bool boolean;
        uint32_t ref;
    } as;
} SvmValue;

typedef enum {
    SVM_HOST_DONE,
    SVM_HOST_PENDING,
    SVM_HOST_FAILED
} SvmHostStatus;

typedef SvmHostStatus (*SvmHostInvoke)(
    void *context,
    const SvmValue *arguments,
    uint32_t argument_count,
    SvmValue *value,
    uint64_t *pending_token
);

typedef SvmHostStatus (*SvmHostPoll)(
    void *context,
    uint64_t pending_token,
    SvmValue *value
);

typedef struct {
    const char *name;
    const SvmType *parameter_types;
    uint32_t parameter_count;
    SvmType result_type;
    uint64_t cost;
    SvmHostInvoke invoke;
    SvmHostPoll poll;
} SvmHostFunction;

typedef struct {
    const char *name;
    void *context;
    const SvmHostFunction *functions;
    uint32_t function_count;
} SvmCapability;

typedef struct {
    const char *capability_name;
    const char *function_name;
    const SvmType *parameter_types;
    uint32_t parameter_count;
    SvmType result_type;
    bool asynchronous;
} SvmHostImport;

typedef enum {
    SVM_OP_CONST_I32,
    SVM_OP_CONST_FALSE,
    SVM_OP_CONST_TRUE,
    SVM_OP_CONST_NULL,
    SVM_OP_LOAD_LOCAL,
    SVM_OP_STORE_LOCAL,
    SVM_OP_DUP,
    SVM_OP_POP,
    SVM_OP_I32_ADD,
    SVM_OP_I32_SUB,
    SVM_OP_I32_MUL,
    SVM_OP_I32_LE,
    SVM_OP_I32_EQ,
    SVM_OP_JUMP,
    SVM_OP_JUMP_IF_FALSE,
    SVM_OP_CALL,
    SVM_OP_NEW_OBJECT,
    SVM_OP_GET_FIELD,
    SVM_OP_SET_FIELD,
    SVM_OP_NEW_ARRAY,
    SVM_OP_ARRAY_LEN,
    SVM_OP_ARRAY_GET,
    SVM_OP_ARRAY_SET,
    SVM_OP_NEW_CLOSURE,
    SVM_OP_CALL_CLOSURE,
    SVM_OP_GC_COLLECT,
    SVM_OP_THROW,
    SVM_OP_DEFER_PUSH,
    SVM_OP_TASK_SPAWN,
    SVM_OP_TASK_AWAIT,
    SVM_OP_TASK_YIELD,
    SVM_OP_CHANNEL_NEW,
    SVM_OP_CHANNEL_SEND,
    SVM_OP_CHANNEL_RECV,
    SVM_OP_CHANNEL_CLOSE,
    SVM_OP_CHANNEL_SELECT,
    SVM_OP_HOST_CALL,
    SVM_OP_HOST_CALL_ASYNC,
    SVM_OP_RETURN
} SvmOpcode;

typedef struct {
    SvmOpcode opcode;
    int32_t operand;
    int32_t operand2;
} SvmInstruction;

typedef struct {
    const char *name;
    const SvmType *field_types;
    uint32_t field_count;
} SvmRecordType;

typedef struct {
    uint32_t start_pc;
    uint32_t end_pc;
    uint32_t handler_pc;
} SvmExceptionHandler;

typedef struct {
    const char *name;
    const SvmType *parameter_types;
    uint32_t parameter_count;
    uint32_t capture_count;
    SvmType result_type;
    uint32_t local_count;
    uint32_t max_stack;
    const SvmInstruction *code;
    uint32_t code_count;
    const SvmExceptionHandler *handlers;
    uint32_t handler_count;
} SvmFunction;

typedef struct {
    const SvmFunction *functions;
    uint32_t function_count;
    const SvmRecordType *record_types;
    uint32_t record_type_count;
    const SvmHostImport *host_imports;
    uint32_t host_import_count;
} SvmModule;

typedef struct {
    uint64_t instruction_budget;
    uint32_t call_depth_limit;
    size_t heap_byte_limit;
    uint32_t handle_limit;
    uint32_t task_limit;
    uint32_t channel_limit;
    uint32_t scheduler_quantum;
    uint64_t host_call_budget;
} SvmLimits;

typedef struct {
    char message[SVM_ERROR_SIZE];
    uint32_t function_index;
    uint32_t pc;
} SvmError;

SvmValue svm_i32(int32_t value);
SvmValue svm_bool(bool value);
SvmValue svm_null(void);

bool svm_verify_module(const SvmModule *module, SvmError *error);

bool svm_execute(
    const SvmModule *module,
    uint32_t entry_function,
    const SvmValue *arguments,
    uint32_t argument_count,
    SvmLimits limits,
    SvmValue *result,
    SvmError *error
);

bool svm_execute_with_capabilities(
    const SvmModule *module,
    uint32_t entry_function,
    const SvmValue *arguments,
    uint32_t argument_count,
    SvmLimits limits,
    const SvmCapability *capabilities,
    uint32_t capability_count,
    SvmValue *result,
    SvmError *error
);

#endif
