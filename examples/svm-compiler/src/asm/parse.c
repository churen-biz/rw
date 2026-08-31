#include "lex.h"
#include "sc_asm.h"
#include "sc_builder.h"

#include <string.h>

bool sc_parse_sasm(
    const char *path_for_diag,
    const char *source,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
);

static bool token_is_ident(const ScToken *tok, const char *text) {
    size_t n = strlen(text);
    return tok->kind == SC_TOK_IDENT && tok->length == n &&
           strncmp(tok->text, text, n) == 0;
}

static bool expect(
    ScLexer *lex,
    ScToken *tok,
    ScTokenKind kind,
    ScError *err,
    const char *what
) {
    if (!sc_lexer_next(lex, tok, err)) {
        return false;
    }
    if (tok->kind != kind) {
        sc_error_set(err, tok->span, "expected %s", what);
        return false;
    }
    return true;
}

bool sc_parse_sasm(
    const char *path_for_diag,
    const char *source,
    ScArena *arena,
    SvmModule *out_module,
    ScError *err
) {
    ScLexer lex;
    sc_lexer_init(&lex, path_for_diag, source);
    ScBuilder *builder = sc_builder_create(arena);
    if (builder == NULL) {
        sc_error_set(err, (ScSpan){0}, "out of memory");
        return false;
    }

    ScToken tok;
    if (!expect(&lex, &tok, SC_TOK_FUNC, err, ".func")) {
        return false;
    }
    if (!expect(&lex, &tok, SC_TOK_IDENT, err, "function name")) {
        return false;
    }
    char name_buf[128];
    if (tok.length >= sizeof(name_buf)) {
        sc_error_set(err, tok.span, "function name too long");
        return false;
    }
    memcpy(name_buf, tok.text, tok.length);
    name_buf[tok.length] = '\0';

    if (!expect(&lex, &tok, SC_TOK_ARROW, err, "->")) {
        return false;
    }
    if (!expect(&lex, &tok, SC_TOK_IDENT, err, "result type")) {
        return false;
    }
    if (!token_is_ident(&tok, "i32")) {
        sc_error_set(err, tok.span, "Plan 1 only supports result type i32");
        return false;
    }
    if (!sc_builder_begin_func(builder, name_buf, err)) {
        return false;
    }

    for (;;) {
        if (!sc_lexer_next(&lex, &tok, err)) {
            return false;
        }
        if (tok.kind == SC_TOK_END) {
            break;
        }
        if (tok.kind == SC_TOK_EOF) {
            sc_error_set(err, tok.span, "unexpected end of file before .end");
            return false;
        }
        if (tok.kind != SC_TOK_IDENT) {
            sc_error_set(err, tok.span, "expected opcode mnemonic");
            return false;
        }

        if (token_is_ident(&tok, "const_i32")) {
            ScToken imm;
            if (!expect(&lex, &imm, SC_TOK_INT, err, "integer immediate")) {
                return false;
            }
            if (!sc_builder_emit(
                    builder, SVM_OP_CONST_I32, imm.int_value, 0, err
                )) {
                return false;
            }
        } else if (token_is_ident(&tok, "i32_add")) {
            if (!sc_builder_emit(builder, SVM_OP_I32_ADD, 0, 0, err)) {
                return false;
            }
        } else if (token_is_ident(&tok, "return")) {
            if (!sc_builder_emit(builder, SVM_OP_RETURN, 0, 0, err)) {
                return false;
            }
        } else {
            char opcode[64];
            size_t n = tok.length < sizeof(opcode) - 1 ? tok.length
                                                       : sizeof(opcode) - 1;
            memcpy(opcode, tok.text, n);
            opcode[n] = '\0';
            sc_error_set(err, tok.span, "unknown opcode '%s'", opcode);
            return false;
        }
    }

    if (!sc_builder_end_func(builder, err)) {
        return false;
    }
    if (!sc_lexer_next(&lex, &tok, err)) {
        return false;
    }
    if (tok.kind != SC_TOK_EOF) {
        sc_error_set(err, tok.span, "extra tokens after .end");
        return false;
    }
    return sc_builder_finish(builder, out_module, err);
}
