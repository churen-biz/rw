#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    OP_CONST_I32,
    OP_LOAD_LOCAL,
    OP_STORE_LOCAL,
    OP_I32_ADD,
    OP_I32_LE,
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_RETURN
} OpCode;

typedef struct {
    OpCode op;
    int32_t operand;
} Instruction;

typedef struct {
    int32_t locals[3];
    int32_t stack[16];
    uint32_t stack_size;
    uint32_t pc;
    uint64_t instruction_budget;
} Vm;

static const Instruction SUM_TO_CODE[] = {
    {OP_CONST_I32, 0},
    {OP_STORE_LOCAL, 1},
    {OP_CONST_I32, 1},
    {OP_STORE_LOCAL, 2},
    {OP_LOAD_LOCAL, 2},
    {OP_LOAD_LOCAL, 0},
    {OP_I32_LE, 0},
    {OP_JUMP_IF_FALSE, 17},
    {OP_LOAD_LOCAL, 1},
    {OP_LOAD_LOCAL, 2},
    {OP_I32_ADD, 0},
    {OP_STORE_LOCAL, 1},
    {OP_LOAD_LOCAL, 2},
    {OP_CONST_I32, 1},
    {OP_I32_ADD, 0},
    {OP_STORE_LOCAL, 2},
    {OP_JUMP, 4},
    {OP_LOAD_LOCAL, 1},
    {OP_RETURN, 0}
};

static const uint32_t SUM_TO_CODE_COUNT =
    (uint32_t)(sizeof(SUM_TO_CODE) / sizeof(SUM_TO_CODE[0]));

static bool vm_error(const char *message) {
    fprintf(stderr, "VM error: %s\n", message);
    return false;
}

static bool push(Vm *vm, int32_t value) {
    if (vm->stack_size >= (uint32_t)(sizeof(vm->stack) / sizeof(vm->stack[0]))) {
        return vm_error("operand stack overflow");
    }
    vm->stack[vm->stack_size++] = value;
    return true;
}

static bool pop(Vm *vm, int32_t *result) {
    if (vm->stack_size == 0) {
        return vm_error("operand stack underflow");
    }
    *result = vm->stack[--vm->stack_size];
    return true;
}

static bool check_local(int32_t index) {
    if (index < 0 || index >= 3) {
        return vm_error("local variable index out of bounds");
    }
    return true;
}

static bool jump_to(Vm *vm, int32_t target) {
    if (target < 0 || (uint32_t)target >= SUM_TO_CODE_COUNT) {
        return vm_error("jump target out of bounds");
    }
    vm->pc = (uint32_t)target;
    return true;
}

static bool run_sum_to(int32_t n, uint64_t budget, int32_t *result) {
    Vm vm = {0};
    vm.locals[0] = n;
    vm.instruction_budget = budget;

    for (;;) {
        if (vm.instruction_budget == 0) {
            return vm_error("instruction budget exhausted");
        }
        vm.instruction_budget--;

        if (vm.pc >= SUM_TO_CODE_COUNT) {
            return vm_error("program counter out of bounds");
        }

        Instruction instruction = SUM_TO_CODE[vm.pc++];
        int32_t left;
        int32_t right;

        switch (instruction.op) {
        case OP_CONST_I32:
            if (!push(&vm, instruction.operand)) return false;
            break;

        case OP_LOAD_LOCAL:
            if (!check_local(instruction.operand)) return false;
            if (!push(&vm, vm.locals[instruction.operand])) return false;
            break;

        case OP_STORE_LOCAL:
            if (!check_local(instruction.operand)) return false;
            if (!pop(&vm, &vm.locals[instruction.operand])) return false;
            break;

        case OP_I32_ADD:
            if (!pop(&vm, &right) || !pop(&vm, &left)) return false;
            /* 通过无符号加法明确规定二进制补码回绕，避免 C 有符号溢出。 */
            if (!push(&vm,
                      (int32_t)((uint32_t)left + (uint32_t)right))) {
                return false;
            }
            break;

        case OP_I32_LE:
            if (!pop(&vm, &right) || !pop(&vm, &left)) return false;
            if (!push(&vm, left <= right ? 1 : 0)) return false;
            break;

        case OP_JUMP:
            if (!jump_to(&vm, instruction.operand)) return false;
            break;

        case OP_JUMP_IF_FALSE:
            if (!pop(&vm, &left)) return false;
            if (left == 0 && !jump_to(&vm, instruction.operand)) return false;
            break;

        case OP_RETURN:
            return pop(&vm, result);

        default:
            return vm_error("unknown opcode");
        }
    }
}

static bool parse_i32_arg(const char *text, int32_t *result) {
    char *end;
    errno = 0;
    intmax_t value = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < INT32_MIN || value > INT32_MAX) {
        return vm_error("n must be a signed 32-bit integer");
    }
    *result = (int32_t)value;
    return true;
}

static bool parse_budget_arg(const char *text, uint64_t *result) {
    char *end;
    errno = 0;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || text[0] == '-' ||
        value > UINT64_MAX) {
        return vm_error("budget must be a non-negative integer");
    }
    *result = (uint64_t)value;
    return true;
}

int main(int argc, char **argv) {
    int32_t n = 5;
    uint64_t budget = 1000;

    if (argc > 3) {
        fprintf(stderr, "usage: %s [n] [instruction-budget]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc >= 2 && !parse_i32_arg(argv[1], &n)) {
        return EXIT_FAILURE;
    }
    if (argc == 3 && !parse_budget_arg(argv[2], &budget)) {
        return EXIT_FAILURE;
    }

    int32_t result;
    if (!run_sum_to(n, budget, &result)) {
        return EXIT_FAILURE;
    }

    printf("sum_to(%" PRId32 ") = %" PRId32 "\n", n, result);
    return EXIT_SUCCESS;
}
