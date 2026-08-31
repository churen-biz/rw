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

static bool token_text_eq(const ScToken *tok, const char *text) {
    size_t n = strlen(text);
    return tok->length == n && strncmp(tok->text, text, n) == 0;
}

static bool copy_ident(const ScToken *tok, char *buf, size_t buflen, ScError *err) {
    if (tok->length >= buflen) {
        sc_error_set(err, tok->span, "identifier too long");
        return false;
    }
    memcpy(buf, tok->text, tok->length);
    buf[tok->length] = '\0';
    return true;
}

static bool parse_type(const ScToken *tok, SvmType *out, ScError *err) {
    if (tok->kind != SC_TOK_IDENT) {
        sc_error_set(err, tok->span, "expected type name");
        return false;
    }
    if (token_text_eq(tok, "i32")) {
        *out = SVM_TYPE_I32;
        return true;
    }
    if (token_text_eq(tok, "bool")) {
        *out = SVM_TYPE_BOOL;
        return true;
    }
    sc_error_set(err, tok->span, "unsupported type (Plan 2: i32 or bool)");
    return false;
}

static bool next_tok(ScLexer *lex, ScToken *tok, ScError *err) {
    return sc_lexer_next(lex, tok, err);
}

static bool expect_kind(
    ScLexer *lex, ScToken *tok, ScTokenKind kind, ScError *err, const char *what
) {
    if (!next_tok(lex, tok, err)) {
        return false;
    }
    if (tok->kind != kind) {
        sc_error_set(err, tok->span, "expected %s", what);
        return false;
    }
    return true;
}

static bool parse_func(
    ScLexer *lex, ScBuilder *builder, ScToken *tok, ScError *err
) {
    if (!expect_kind(lex, tok, SC_TOK_IDENT, err, "function name")) {
        return false;
    }
    char name[128];
    if (!copy_ident(tok, name, sizeof(name), err)) {
        return false;
    }

    SvmType params[SVM_MAX_LOCALS];
    uint32_t param_count = 0;
    if (!next_tok(lex, tok, err)) {
        return false;
    }
    if (tok->kind == SC_TOK_LPAREN) {
        if (!next_tok(lex, tok, err)) {
            return false;
        }
        if (tok->kind != SC_TOK_RPAREN) {
            for (;;) {
                if (tok->kind != SC_TOK_IDENT) {
                    sc_error_set(err, tok->span, "expected parameter name");
                    return false;
                }
                if (!expect_kind(lex, tok, SC_TOK_COLON, err, ":")) {
                    return false;
                }
                if (!expect_kind(lex, tok, SC_TOK_IDENT, err, "parameter type")) {
                    return false;
                }
                if (param_count >= SVM_MAX_LOCALS) {
                    sc_error_set(err, tok->span, "too many parameters");
                    return false;
                }
                if (!parse_type(tok, &params[param_count], err)) {
                    return false;
                }
                param_count += 1;
                if (!next_tok(lex, tok, err)) {
                    return false;
                }
                if (tok->kind == SC_TOK_COMMA) {
                    if (!next_tok(lex, tok, err)) {
                        return false;
                    }
                    continue;
                }
                if (tok->kind == SC_TOK_RPAREN) {
                    break;
                }
                sc_error_set(err, tok->span, "expected ',' or ')'");
                return false;
            }
        }
        if (!expect_kind(lex, tok, SC_TOK_ARROW, err, "->")) {
            return false;
        }
    } else if (tok->kind == SC_TOK_ARROW) {
        /* no params */
    } else {
        sc_error_set(err, tok->span, "expected '(' or '->'");
        return false;
    }

    if (!expect_kind(lex, tok, SC_TOK_IDENT, err, "result type")) {
        return false;
    }
    SvmType result;
    if (!parse_type(tok, &result, err)) {
        return false;
    }
    if (!sc_builder_begin_func(
            builder, name, params, param_count, result, err
        )) {
        return false;
    }

    for (;;) {
        if (!next_tok(lex, tok, err)) {
            return false;
        }
        if (tok->kind == SC_TOK_END) {
            break;
        }
        if (tok->kind == SC_TOK_EOF) {
            sc_error_set(err, tok->span, "unexpected end of file before .end");
            return false;
        }
        if (tok->kind != SC_TOK_IDENT) {
            sc_error_set(err, tok->span, "expected opcode or label");
            return false;
        }

        char ident[128];
        if (!copy_ident(tok, ident, sizeof(ident), err)) {
            return false;
        }

        /* Lookahead for label: IDENT COLON */
        ScLexer saved = *lex;
        ScToken look;
        if (!next_tok(lex, &look, err)) {
            return false;
        }
        if (look.kind == SC_TOK_COLON) {
            if (!sc_builder_define_label(builder, ident, err)) {
                return false;
            }
            continue;
        }
        *lex = saved;

        if (token_text_eq(tok, "const_i32")) {
            ScToken imm;
            if (!expect_kind(lex, &imm, SC_TOK_INT, err, "integer immediate")) {
                return false;
            }
            if (!sc_builder_emit(
                    builder, SVM_OP_CONST_I32, imm.int_value, 0, err
                )) {
                return false;
            }
        } else if (token_text_eq(tok, "const_true")) {
            if (!sc_builder_emit(builder, SVM_OP_CONST_TRUE, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "const_false")) {
            if (!sc_builder_emit(builder, SVM_OP_CONST_FALSE, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "load_local")) {
            ScToken imm;
            if (!expect_kind(lex, &imm, SC_TOK_INT, err, "local index")) {
                return false;
            }
            if (!sc_builder_emit(
                    builder, SVM_OP_LOAD_LOCAL, imm.int_value, 0, err
                )) {
                return false;
            }
        } else if (token_text_eq(tok, "store_local")) {
            ScToken imm;
            if (!expect_kind(lex, &imm, SC_TOK_INT, err, "local index")) {
                return false;
            }
            if (!sc_builder_emit(
                    builder, SVM_OP_STORE_LOCAL, imm.int_value, 0, err
                )) {
                return false;
            }
        } else if (token_text_eq(tok, "dup")) {
            if (!sc_builder_emit(builder, SVM_OP_DUP, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "pop")) {
            if (!sc_builder_emit(builder, SVM_OP_POP, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "i32_add")) {
            if (!sc_builder_emit(builder, SVM_OP_I32_ADD, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "i32_sub")) {
            if (!sc_builder_emit(builder, SVM_OP_I32_SUB, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "i32_mul")) {
            if (!sc_builder_emit(builder, SVM_OP_I32_MUL, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "i32_le")) {
            if (!sc_builder_emit(builder, SVM_OP_I32_LE, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "i32_eq")) {
            if (!sc_builder_emit(builder, SVM_OP_I32_EQ, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "jump")) {
            if (!expect_kind(lex, tok, SC_TOK_IDENT, err, "jump label")) {
                return false;
            }
            char label[128];
            if (!copy_ident(tok, label, sizeof(label), err)) {
                return false;
            }
            if (!sc_builder_emit_jump(builder, SVM_OP_JUMP, label, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "jump_if_false")) {
            if (!expect_kind(lex, tok, SC_TOK_IDENT, err, "jump label")) {
                return false;
            }
            char label[128];
            if (!copy_ident(tok, label, sizeof(label), err)) {
                return false;
            }
            if (!sc_builder_emit_jump(
                    builder, SVM_OP_JUMP_IF_FALSE, label, err
                )) {
                return false;
            }
        } else if (token_text_eq(tok, "call")) {
            if (!expect_kind(lex, tok, SC_TOK_IDENT, err, "function name")) {
                return false;
            }
            char callee[128];
            if (!copy_ident(tok, callee, sizeof(callee), err)) {
                return false;
            }
            if (!sc_builder_emit_call(builder, callee, err)) {
                return false;
            }
        } else if (token_text_eq(tok, "return")) {
            if (!sc_builder_emit(builder, SVM_OP_RETURN, 0, 0, err)) {
                return false;
            }
        } else {
            sc_error_set(err, tok->span, "unknown opcode '%s'", ident);
            return false;
        }
    }

    return sc_builder_end_func(builder, err);
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
    for (;;) {
        if (!next_tok(&lex, &tok, err)) {
            return false;
        }
        if (tok.kind == SC_TOK_EOF) {
            break;
        }
        if (tok.kind != SC_TOK_FUNC) {
            sc_error_set(err, tok.span, "expected .func");
            return false;
        }
        if (!parse_func(&lex, builder, &tok, err)) {
            return false;
        }
    }
    return sc_builder_finish(builder, out_module, err);
}
