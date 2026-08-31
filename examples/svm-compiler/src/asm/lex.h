#ifndef SC_LEX_H
#define SC_LEX_H

#include "sc_diag.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SC_TOK_EOF,
    SC_TOK_FUNC,
    SC_TOK_END,
    SC_TOK_TYPE,
    SC_TOK_IMPORT,
    SC_TOK_HANDLER,
    SC_TOK_CAPTURES,
    SC_TOK_ARROW,
    SC_TOK_IDENT,
    SC_TOK_INT,
    SC_TOK_LPAREN,
    SC_TOK_RPAREN,
    SC_TOK_COMMA,
    SC_TOK_COLON,
    SC_TOK_LBRACE,
    SC_TOK_RBRACE
} ScTokenKind;

typedef struct {
    ScTokenKind kind;
    ScSpan span;
    const char *text;
    size_t length;
    int32_t int_value;
} ScToken;

typedef struct {
    const char *path;
    const char *src;
    size_t len;
    size_t pos;
    uint32_t line;
    uint32_t column;
} ScLexer;

void sc_lexer_init(ScLexer *lex, const char *path, const char *src);
bool sc_lexer_next(ScLexer *lex, ScToken *out, ScError *err);

#endif
