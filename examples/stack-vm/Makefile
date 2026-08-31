CC ?= cc
CPPFLAGS ?= -Ireference
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -g
BUILD_DIR := build

.PHONY: all test sanitize clean milestone-01 reference demo

all: milestone-01 reference $(BUILD_DIR)/svm-run $(BUILD_DIR)/make-example

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

milestone-01: $(BUILD_DIR)/milestone-01

$(BUILD_DIR)/milestone-01: vm.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

reference: $(BUILD_DIR)/reference-tests $(BUILD_DIR)/heap-tests $(BUILD_DIR)/module-tests

$(BUILD_DIR)/reference-tests: reference/svm.c reference/heap.c reference/svm.h reference/heap.h tests/test-reference.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) reference/svm.c reference/heap.c tests/test-reference.c -o $@

$(BUILD_DIR)/heap-tests: reference/svm.c reference/heap.c reference/svm.h reference/heap.h tests/test-heap.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) reference/svm.c reference/heap.c tests/test-heap.c -o $@

$(BUILD_DIR)/module-tests: reference/svm.c reference/heap.c reference/module.c reference/svm.h reference/heap.h reference/module.h tests/test-module.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) reference/svm.c reference/heap.c reference/module.c tests/test-module.c -o $@

$(BUILD_DIR)/svm-run: reference/main.c reference/svm.c reference/heap.c reference/module.c reference/svm.h reference/heap.h reference/module.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) reference/main.c reference/svm.c reference/heap.c reference/module.c -o $@

$(BUILD_DIR)/make-example: tools/make-example.c reference/svm.c reference/heap.c reference/module.c reference/svm.h reference/heap.h reference/module.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/make-example.c reference/svm.c reference/heap.c reference/module.c -o $@

demo: $(BUILD_DIR)/svm-run $(BUILD_DIR)/make-example
	$(BUILD_DIR)/make-example $(BUILD_DIR)/answer.svm
	$(BUILD_DIR)/svm-run $(BUILD_DIR)/answer.svm

test: milestone-01 reference
	./tests/test-milestone-01.sh $(BUILD_DIR)/milestone-01
	$(BUILD_DIR)/reference-tests
	$(BUILD_DIR)/heap-tests
	$(BUILD_DIR)/module-tests

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: clean test

clean:
	rm -rf $(BUILD_DIR)
