#include "lex.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void sc_lexer_init(ScLexer *lex, const char *path, const char *src) {
    lex->path = path;
    lex->src = src;
    lex->len = strlen(src);
    lex->pos = 0;
    lex->line = 1;
    lex->column = 1;
}

static ScSpan span_at(const ScLexer *lex, uint32_t line, uint32_t column) {
    ScSpan span = {.path = lex->path, .line = line, .column = column};
    return span;
}

static char peek(const ScLexer *lex) {
    if (lex->pos >= lex->len) {
        return '\0';
    }
    return lex->src[lex->pos];
}

static char advance(ScLexer *lex) {
    char c = peek(lex);
    if (c == '\0') {
        return c;
    }
    lex->pos += 1;
    if (c == '\n') {
        lex->line += 1;
        lex->column = 1;
    } else {
        lex->column += 1;
    }
    return c;
}

static void skip_ws_and_comments(ScLexer *lex) {
    for (;;) {
        char c = peek(lex);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lex);
            continue;
        }
        if (c == '#') {
            while (peek(lex) != '\0' && peek(lex) != '\n') {
                advance(lex);
            }
            continue;
        }
        break;
    }
}

bool sc_lexer_next(ScLexer *lex, ScToken *out, ScError *err) {
    skip_ws_and_comments(lex);
    uint32_t line = lex->line;
    uint32_t column = lex->column;
    char c = peek(lex);
    if (c == '\0') {
        out->kind = SC_TOK_EOF;
        out->span = span_at(lex, line, column);
        out->text = lex->src + lex->pos;
        out->length = 0;
        out->int_value = 0;
        return true;
    }

    if (c == '-' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '>') {
        advance(lex);
        advance(lex);
        out->kind = SC_TOK_ARROW;
        out->span = span_at(lex, line, column);
        out->text = "->";
        out->length = 2;
        out->int_value = 0;
        return true;
    }

    if (c == '(' || c == ')' || c == ',' || c == ':') {
        advance(lex);
        out->span = span_at(lex, line, column);
        out->text = lex->src + (lex->pos - 1);
        out->length = 1;
        out->int_value = 0;
        if (c == '(') {
            out->kind = SC_TOK_LPAREN;
        } else if (c == ')') {
            out->kind = SC_TOK_RPAREN;
        } else if (c == ',') {
            out->kind = SC_TOK_COMMA;
        } else {
            out->kind = SC_TOK_COLON;
        }
        return true;
    }

    if (c == '.' || isalpha((unsigned char)c) || c == '_') {
        const char *start = lex->src + lex->pos;
        advance(lex);
        while (isalnum((unsigned char)peek(lex)) || peek(lex) == '_') {
            advance(lex);
        }
        size_t length = (size_t)((lex->src + lex->pos) - start);
        out->span = span_at(lex, line, column);
        out->text = start;
        out->length = length;
        out->int_value = 0;
        if (length == 5 && strncmp(start, ".func", 5) == 0) {
            out->kind = SC_TOK_FUNC;
        } else if (length == 4 && strncmp(start, ".end", 4) == 0) {
            out->kind = SC_TOK_END;
        } else {
            out->kind = SC_TOK_IDENT;
        }
        return true;
    }

    if (isdigit((unsigned char)c) ||
        (c == '-' && lex->pos + 1 < lex->len &&
         isdigit((unsigned char)lex->src[lex->pos + 1]))) {
        const char *start = lex->src + lex->pos;
        if (c == '-') {
            advance(lex);
        }
        while (isdigit((unsigned char)peek(lex))) {
            advance(lex);
        }
        size_t length = (size_t)((lex->src + lex->pos) - start);
        char buf[32];
        if (length >= sizeof(buf)) {
            sc_error_set(err, span_at(lex, line, column), "integer literal too long");
            return false;
        }
        memcpy(buf, start, length);
        buf[length] = '\0';
        char *end = NULL;
        long value = strtol(buf, &end, 10);
        if (end == buf || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
            sc_error_set(err, span_at(lex, line, column), "invalid integer literal");
            return false;
        }
        out->kind = SC_TOK_INT;
        out->span = span_at(lex, line, column);
        out->text = start;
        out->length = length;
        out->int_value = (int32_t)value;
        return true;
    }

    sc_error_set(err, span_at(lex, line, column), "unexpected character '%c'", c);
    return false;
}
