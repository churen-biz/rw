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
MINI_SRC := src/mini/parse.c src/mini/typecheck.c src/mini/compile.c
IR_SRC := src/ir/build.c src/ir/lower.c
LIB_SRC := $(COMMON_SRC) $(EMIT_SRC) $(ASM_SRC) $(MINI_SRC) $(IR_SRC)
REF_SRC := $(REF)/svm.c $(REF)/heap.c $(REF)/module.c

.PHONY: all test clean stack-vm-deps demo demo-factorial demo-mini

all: stack-vm-deps $(BUILD_DIR)/svm-as $(BUILD_DIR)/mini \
	$(BUILD_DIR)/test-asm $(BUILD_DIR)/test-mini

stack-vm-deps:
	$(MAKE) -C $(STACK_VM) all

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/svm-as: tools/svm-as.c $(LIB_SRC) $(REF_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/svm-as.c $(LIB_SRC) $(REF_SRC) -o $@

$(BUILD_DIR)/mini: tools/mini.c $(LIB_SRC) $(REF_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/mini.c $(LIB_SRC) $(REF_SRC) -o $@

$(BUILD_DIR)/test-asm: tests/test-asm.c $(LIB_SRC) $(REF_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test-asm.c $(LIB_SRC) $(REF_SRC) -o $@

$(BUILD_DIR)/test-mini: tests/test-mini.c $(LIB_SRC) $(REF_SRC) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test-mini.c $(LIB_SRC) $(REF_SRC) -o $@

test: all
	$(BUILD_DIR)/test-asm
	$(BUILD_DIR)/test-mini
	./scripts/run-answer.sh
	./scripts/run-factorial.sh
	./scripts/run-mini-answer.sh

demo: $(BUILD_DIR)/svm-as stack-vm-deps
	$(BUILD_DIR)/svm-as tests/fixtures/answer.sasm $(BUILD_DIR)/answer.svm
	$(SVM_RUN) $(BUILD_DIR)/answer.svm

demo-factorial: $(BUILD_DIR)/svm-as stack-vm-deps
	$(BUILD_DIR)/svm-as tests/fixtures/factorial.sasm $(BUILD_DIR)/factorial.svm
	$(SVM_RUN) $(BUILD_DIR)/factorial.svm 1

demo-mini: $(BUILD_DIR)/mini stack-vm-deps
	$(BUILD_DIR)/mini tests/fixtures/answer.mini $(BUILD_DIR)/answer-mini.svm
	$(SVM_RUN) $(BUILD_DIR)/answer-mini.svm 1

clean:
	rm -rf $(BUILD_DIR)
