CC ?= cc
STACK_VM := ../stack-vm
REF := $(STACK_VM)/reference
CPPFLAGS ?= -Iinclude -Isrc/asm -I$(REF)
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
BUILD_DIR := build
SVM_RUN := $(STACK_VM)/build/svm-run

COMMON_SRC := src/common/diag.c src/common/arena.c
EMIT_SRC := src/emit/builder.c
ASM_SRC := src/asm/lex.c src/asm/parse.c src/asm/assemble.c
LIB_SRC := $(COMMON_SRC) $(EMIT_SRC) $(ASM_SRC)
REF_SRC := $(REF)/svm.c $(REF)/heap.c $(REF)/module.c

.PHONY: all test clean stack-vm-deps demo

all: stack-vm-deps $(BUILD_DIR)/svm-as $(BUILD_DIR)/test-asm

stack-vm-deps:
	$(MAKE) -C $(STACK_VM) all

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/svm-as: tools/svm-as.c $(LIB_SRC) $(REF_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/svm-as.c $(LIB_SRC) $(REF_SRC) -o $@

$(BUILD_DIR)/test-asm: tests/test-asm.c $(LIB_SRC) $(REF_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test-asm.c $(LIB_SRC) $(REF_SRC) -o $@

test: all
	$(BUILD_DIR)/test-asm
	./scripts/run-answer.sh

demo: $(BUILD_DIR)/svm-as stack-vm-deps
	$(BUILD_DIR)/svm-as tests/fixtures/answer.sasm $(BUILD_DIR)/answer.svm
	$(SVM_RUN) $(BUILD_DIR)/answer.svm

clean:
	rm -rf $(BUILD_DIR)
