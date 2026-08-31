#include "sc_diag.h"

#include <stdarg.h>
#include <stdio.h>

void sc_error_set(ScError *err, ScSpan span, const char *fmt, ...) {
    err->span = span;
    va_list args;
    va_start(args, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, args);
    va_end(args);
}

void sc_error_print(const ScError *err, FILE *out) {
    if (err->span.path != NULL) {
        fprintf(
            out,
            "%s:%u:%u: %s\n",
            err->span.path,
            err->span.line,
            err->span.column,
            err->message
        );
    } else {
        fprintf(
            out,
            "%u:%u: %s\n",
            err->span.line,
            err->span.column,
            err->message
        );
    }
}
