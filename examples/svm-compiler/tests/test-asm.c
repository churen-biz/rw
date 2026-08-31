#include "sc_arena.h"
#include "sc_diag.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_arena_strdup(void) {
    ScArena *a = sc_arena_create();
    assert(a != NULL);
    char *s = sc_arena_strndup(a, "hello", 5);
    assert(s != NULL);
    assert(strcmp(s, "hello") == 0);
    sc_arena_destroy(a);
}

static void test_error_format(void) {
    ScError err;
    ScSpan span = {.path = "x.sasm", .line = 3, .column = 5};
    sc_error_set(&err, span, "unknown opcode '%s'", "foo");
    assert(strstr(err.message, "foo") != NULL);
    assert(err.span.line == 3);
}

int main(void) {
    test_arena_strdup();
    test_error_format();
    puts("ok diag/arena");
    return 0;
}
