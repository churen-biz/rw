#ifndef SC_DIAG_H
#define SC_DIAG_H

#include <stdio.h>
#include <stdint.h>

typedef struct {
    const char *path;
    uint32_t line;
    uint32_t column;
} ScSpan;

typedef struct {
    char message[256];
    ScSpan span;
} ScError;

void sc_error_set(ScError *err, ScSpan span, const char *fmt, ...);
void sc_error_print(const ScError *err, FILE *out);

#endif
