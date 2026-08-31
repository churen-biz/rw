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
    return tok->kind == SC_TOK_IDENT && tok->length == n &&
           strncmp(tok->text, text, n) == 0;
}

static bool copy_ident(const ScToken *tok, char *buf, size_t buflen, ScError *err) {
    if (tok->kind != SC_TOK_IDENT) {
        sc_error_set(err, tok->span, "expected identifier");
        return false;
    }
    if (tok->length >= buflen) {
        sc_error_set(err, tok->span, "identifier too long");
        return false;
    }
    memcpy(buf, tok->text, tok->length);
    buf[tok->length] = '\0';
    return true;
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

static bool parse_type_token(const ScToken *tok, SvmType *out, ScError *err) {
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
    if (token_text_eq(tok, "ref")) {
        *out = SVM_TYPE_REF;
        return true;
    }
    if (token_text_eq(tok, "any")) {
        *out = SVM_TYPE_ANY;
        return true;
    }
    if (token_text_eq(tok, "void")) {
        *out = SVM_TYPE_VOID;
        return true;
    }
    if (token_text_eq(tok, "task")) {
        *out = SVM_TYPE_TASK;
        return true;
    }
    if (token_text_eq(tok, "channel")) {
        *out = SVM_TYPE_CHANNEL;
        return true;
    }
    sc_error_set(err, tok->span, "unknown type name");
    return false;
}

static bool parse_param_list(
    ScLexer *lex,
    ScToken *tok,
    SvmType *params,
    uint32_t *param_count,
    ScError *err
) {
    *param_count = 0;
    if (!next_tok(lex, tok, err)) {
        return false;
    }
    if (tok->kind == SC_TOK_RPAREN) {
        return true;
    }
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
        if (*param_count >= SVM_MAX_LOCALS) {
            sc_error_set(err, tok->span, "too many parameters");
            return false;
        }
        if (!parse_type_token(tok, &params[*param_count], err)) {
            return false;
        }
        *param_count += 1;
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
            return true;
        }
        sc_error_set(err, tok->span, "expected ',' or ')'");
        return false;
    }
}

static bool parse_type_decl(ScLexer *lex, ScBuilder *builder, ScError *err) {
    ScToken tok;
    if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "type name")) {
        return false;
    }
    char name[128];
    if (!copy_ident(&tok, name, sizeof(name), err)) {
        return false;
    }
    if (!expect_kind(lex, &tok, SC_TOK_LBRACE, err, "{")) {
        return false;
    }
    SvmType fields[SVM_MAX_LOCALS];
    uint32_t field_count = 0;
    if (!next_tok(lex, &tok, err)) {
        return false;
    }
    if (tok.kind != SC_TOK_RBRACE) {
        for (;;) {
            if (tok.kind != SC_TOK_IDENT) {
                sc_error_set(err, tok.span, "expected field name");
                return false;
            }
            if (!expect_kind(lex, &tok, SC_TOK_COLON, err, ":")) {
                return false;
            }
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "field type")) {
                return false;
            }
            if (field_count >= SVM_MAX_LOCALS) {
                sc_error_set(err, tok.span, "too many fields");
                return false;
            }
            if (!parse_type_token(&tok, &fields[field_count], err)) {
                return false;
            }
            field_count += 1;
            if (!next_tok(lex, &tok, err)) {
                return false;
            }
            if (tok.kind == SC_TOK_COMMA) {
                if (!next_tok(lex, &tok, err)) {
                    return false;
                }
                continue;
            }
            if (tok.kind == SC_TOK_RBRACE) {
                break;
            }
            sc_error_set(err, tok.span, "expected ',' or '}'");
            return false;
        }
    }
    return sc_builder_add_record(builder, name, fields, field_count, err);
}

static bool parse_import_decl(ScLexer *lex, ScBuilder *builder, ScError *err) {
    ScToken tok;
    bool asynchronous = false;
    if (!next_tok(lex, &tok, err)) {
        return false;
    }
    if (token_text_eq(&tok, "async")) {
        asynchronous = true;
        if (!next_tok(lex, &tok, err)) {
            return false;
        }
    }
    char cap[128];
    char fn[128];
    if (!copy_ident(&tok, cap, sizeof(cap), err)) {
        return false;
    }
    if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "import function name")) {
        return false;
    }
    if (!copy_ident(&tok, fn, sizeof(fn), err)) {
        return false;
    }
    if (!expect_kind(lex, &tok, SC_TOK_LPAREN, err, "(")) {
        return false;
    }
    SvmType params[SVM_MAX_LOCALS];
    uint32_t param_count = 0;
    if (!parse_param_list(lex, &tok, params, &param_count, err)) {
        return false;
    }
    if (!expect_kind(lex, &tok, SC_TOK_ARROW, err, "->")) {
        return false;
    }
    if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "result type")) {
        return false;
    }
    SvmType result;
    if (!parse_type_token(&tok, &result, err)) {
        return false;
    }
    return sc_builder_add_import(
        builder, cap, fn, params, param_count, result, asynchronous, err
    );
}

static bool parse_func(
    ScLexer *lex, ScBuilder *builder, ScError *err
) {
    ScToken tok;
    if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "function name")) {
        return false;
    }
    char name[128];
    if (!copy_ident(&tok, name, sizeof(name), err)) {
        return false;
    }

    SvmType params[SVM_MAX_LOCALS];
    uint32_t param_count = 0;
    if (!next_tok(lex, &tok, err)) {
        return false;
    }
    if (tok.kind == SC_TOK_LPAREN) {
        if (!parse_param_list(lex, &tok, params, &param_count, err)) {
            return false;
        }
        if (!expect_kind(lex, &tok, SC_TOK_ARROW, err, "->")) {
            return false;
        }
    } else if (tok.kind != SC_TOK_ARROW) {
        sc_error_set(err, tok.span, "expected '(' or '->'");
        return false;
    }

    if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "result type")) {
        return false;
    }
    SvmType result;
    if (!parse_type_token(&tok, &result, err)) {
        return false;
    }
    if (!sc_builder_begin_func(
            builder, name, params, param_count, result, err
        )) {
        return false;
    }

    for (;;) {
        if (!next_tok(lex, &tok, err)) {
            return false;
        }
        if (tok.kind == SC_TOK_END) {
            break;
        }
        if (tok.kind == SC_TOK_EOF) {
            sc_error_set(err, tok.span, "unexpected end of file before .end");
            return false;
        }
        if (tok.kind == SC_TOK_CAPTURES) {
            if (!expect_kind(lex, &tok, SC_TOK_INT, err, "capture count")) {
                return false;
            }
            if (!sc_builder_set_captures(builder, (uint32_t)tok.int_value, err)) {
                return false;
            }
            continue;
        }
        if (tok.kind == SC_TOK_HANDLER) {
            char start[128];
            char end[128];
            char handler[128];
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "start label")) {
                return false;
            }
            if (!copy_ident(&tok, start, sizeof(start), err)) {
                return false;
            }
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "end label")) {
                return false;
            }
            if (!copy_ident(&tok, end, sizeof(end), err)) {
                return false;
            }
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "handler label")) {
                return false;
            }
            if (!copy_ident(&tok, handler, sizeof(handler), err)) {
                return false;
            }
            if (!sc_builder_add_handler(builder, start, end, handler, err)) {
                return false;
            }
            continue;
        }
        if (tok.kind != SC_TOK_IDENT) {
            sc_error_set(err, tok.span, "expected opcode or label");
            return false;
        }

        char ident[128];
        if (!copy_ident(&tok, ident, sizeof(ident), err)) {
            return false;
        }

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

        if (token_text_eq(&tok, "const_i32")) {
            ScToken imm;
            if (!expect_kind(lex, &imm, SC_TOK_INT, err, "integer")) {
                return false;
            }
            if (!sc_builder_emit(
                    builder, SVM_OP_CONST_I32, imm.int_value, 0, err
                )) {
                return false;
            }
        } else if (token_text_eq(&tok, "const_true")) {
            if (!sc_builder_emit(builder, SVM_OP_CONST_TRUE, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "const_false")) {
            if (!sc_builder_emit(builder, SVM_OP_CONST_FALSE, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "const_null")) {
            if (!sc_builder_emit(builder, SVM_OP_CONST_NULL, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "load_local") ||
                   token_text_eq(&tok, "store_local")) {
            ScToken imm;
            if (!expect_kind(lex, &imm, SC_TOK_INT, err, "local index")) {
                return false;
            }
            SvmOpcode op = token_text_eq(&tok, "load_local")
                               ? SVM_OP_LOAD_LOCAL
                               : SVM_OP_STORE_LOCAL;
            if (!sc_builder_emit(builder, op, imm.int_value, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "dup")) {
            if (!sc_builder_emit(builder, SVM_OP_DUP, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "pop")) {
            if (!sc_builder_emit(builder, SVM_OP_POP, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "i32_add") ||
                   token_text_eq(&tok, "i32_sub") ||
                   token_text_eq(&tok, "i32_mul") ||
                   token_text_eq(&tok, "i32_le") ||
                   token_text_eq(&tok, "i32_eq")) {
            SvmOpcode op = SVM_OP_I32_ADD;
            if (token_text_eq(&tok, "i32_sub")) {
                op = SVM_OP_I32_SUB;
            } else if (token_text_eq(&tok, "i32_mul")) {
                op = SVM_OP_I32_MUL;
            } else if (token_text_eq(&tok, "i32_le")) {
                op = SVM_OP_I32_LE;
            } else if (token_text_eq(&tok, "i32_eq")) {
                op = SVM_OP_I32_EQ;
            }
            if (!sc_builder_emit(builder, op, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "jump") ||
                   token_text_eq(&tok, "jump_if_false")) {
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "label")) {
                return false;
            }
            char label[128];
            if (!copy_ident(&tok, label, sizeof(label), err)) {
                return false;
            }
            SvmOpcode op = token_text_eq(&tok, "jump") ? /* wrong - tok overwritten */
                               SVM_OP_JUMP
                                                       : SVM_OP_JUMP_IF_FALSE;
            /* fix: use ident */
            op = strcmp(ident, "jump") == 0 ? SVM_OP_JUMP
                                            : SVM_OP_JUMP_IF_FALSE;
            if (!sc_builder_emit_jump(builder, op, label, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "call") ||
                   token_text_eq(&tok, "task_spawn") ||
                   token_text_eq(&tok, "new_closure") ||
                   token_text_eq(&tok, "call_closure") ||
                   token_text_eq(&tok, "defer_push")) {
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "function name")) {
                return false;
            }
            char callee[128];
            if (!copy_ident(&tok, callee, sizeof(callee), err)) {
                return false;
            }
            SvmOpcode op = SVM_OP_CALL;
            int32_t operand2 = 0;
            if (strcmp(ident, "task_spawn") == 0) {
                op = SVM_OP_TASK_SPAWN;
            } else if (strcmp(ident, "new_closure") == 0) {
                op = SVM_OP_NEW_CLOSURE;
                ScLexer saved2 = *lex;
                ScToken maybe;
                if (!next_tok(lex, &maybe, err)) {
                    return false;
                }
                if (maybe.kind == SC_TOK_INT) {
                    operand2 = maybe.int_value;
                } else {
                    *lex = saved2;
                    operand2 = -1;
                }
            } else if (strcmp(ident, "call_closure") == 0) {
                op = SVM_OP_CALL_CLOSURE;
            } else if (strcmp(ident, "defer_push") == 0) {
                op = SVM_OP_DEFER_PUSH;
            }
            if (!sc_builder_emit_call_named(builder, op, callee, operand2, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "return")) {
            if (!sc_builder_emit(builder, SVM_OP_RETURN, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "new_object")) {
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "record type")) {
                return false;
            }
            char type_name[128];
            if (!copy_ident(&tok, type_name, sizeof(type_name), err)) {
                return false;
            }
            int32_t idx = sc_builder_find_record(builder, type_name);
            if (idx < 0) {
                sc_error_set(err, tok.span, "unknown record type '%s'", type_name);
                return false;
            }
            if (!sc_builder_emit(builder, SVM_OP_NEW_OBJECT, idx, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "get_field") ||
                   token_text_eq(&tok, "set_field")) {
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "record type")) {
                return false;
            }
            char type_name[128];
            if (!copy_ident(&tok, type_name, sizeof(type_name), err)) {
                return false;
            }
            int32_t type_index = sc_builder_find_record(builder, type_name);
            if (type_index < 0) {
                sc_error_set(err, tok.span, "unknown record type '%s'", type_name);
                return false;
            }
            ScToken field;
            if (!expect_kind(lex, &field, SC_TOK_INT, err, "field index")) {
                return false;
            }
            SvmOpcode op = strcmp(ident, "get_field") == 0 ? SVM_OP_GET_FIELD
                                                           : SVM_OP_SET_FIELD;
            if (!sc_builder_emit(
                    builder, op, type_index, field.int_value, err
                )) {
                return false;
            }
        } else if (token_text_eq(&tok, "new_array") ||
                   token_text_eq(&tok, "array_len") ||
                   token_text_eq(&tok, "array_get") ||
                   token_text_eq(&tok, "array_set") ||
                   token_text_eq(&tok, "channel_new") ||
                   token_text_eq(&tok, "channel_send") ||
                   token_text_eq(&tok, "channel_recv") ||
                   token_text_eq(&tok, "channel_select")) {
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "element type")) {
                return false;
            }
            SvmType element;
            if (!parse_type_token(&tok, &element, err)) {
                return false;
            }
            if (strcmp(ident, "new_array") == 0) {
                if (!sc_builder_emit(
                        builder, SVM_OP_NEW_ARRAY, (int32_t)element, 0, err
                    )) {
                    return false;
                }
            } else if (strcmp(ident, "array_len") == 0) {
                if (!sc_builder_emit(
                        builder, SVM_OP_ARRAY_LEN, (int32_t)element, 0, err
                    )) {
                    return false;
                }
            } else if (strcmp(ident, "array_get") == 0) {
                if (!sc_builder_emit(
                        builder, SVM_OP_ARRAY_GET, (int32_t)element, 0, err
                    )) {
                    return false;
                }
            } else if (strcmp(ident, "array_set") == 0) {
                if (!sc_builder_emit(
                        builder, SVM_OP_ARRAY_SET, (int32_t)element, 0, err
                    )) {
                    return false;
                }
            } else if (strcmp(ident, "channel_new") == 0) {
                if (!sc_builder_emit(
                        builder, SVM_OP_CHANNEL_NEW, (int32_t)element, 0, err
                    )) {
                    return false;
                }
            } else if (strcmp(ident, "channel_send") == 0) {
                if (!sc_builder_emit(
                        builder, SVM_OP_CHANNEL_SEND, (int32_t)element, 0, err
                    )) {
                    return false;
                }
            } else if (strcmp(ident, "channel_recv") == 0) {
                if (!sc_builder_emit(
                        builder, SVM_OP_CHANNEL_RECV, (int32_t)element, 0, err
                    )) {
                    return false;
                }
            } else {
                ScToken count;
                if (!expect_kind(lex, &count, SC_TOK_INT, err, "case count")) {
                    return false;
                }
                if (!sc_builder_emit(
                        builder,
                        SVM_OP_CHANNEL_SELECT,
                        count.int_value,
                        (int32_t)element,
                        err
                    )) {
                    return false;
                }
            }
        } else if (token_text_eq(&tok, "channel_close")) {
            if (!sc_builder_emit(builder, SVM_OP_CHANNEL_CLOSE, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "gc_collect")) {
            if (!sc_builder_emit(builder, SVM_OP_GC_COLLECT, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "throw")) {
            if (!sc_builder_emit(builder, SVM_OP_THROW, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "task_await")) {
            ScToken key;
            if (!expect_kind(lex, &key, SC_TOK_INT, err, "await key")) {
                return false;
            }
            if (!sc_builder_emit(builder, SVM_OP_TASK_AWAIT, key.int_value, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "task_yield")) {
            if (!sc_builder_emit(builder, SVM_OP_TASK_YIELD, 0, 0, err)) {
                return false;
            }
        } else if (token_text_eq(&tok, "host_call") ||
                   token_text_eq(&tok, "host_call_async")) {
            char cap[128];
            char fn[128];
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "capability")) {
                return false;
            }
            if (!copy_ident(&tok, cap, sizeof(cap), err)) {
                return false;
            }
            if (!expect_kind(lex, &tok, SC_TOK_IDENT, err, "host function")) {
                return false;
            }
            if (!copy_ident(&tok, fn, sizeof(fn), err)) {
                return false;
            }
            bool async = strcmp(ident, "host_call_async") == 0;
            if (!sc_builder_emit_host(builder, async, cap, fn, err)) {
                return false;
            }
        } else {
            sc_error_set(err, tok.span, "unknown opcode '%s'", ident);
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
        if (tok.kind == SC_TOK_TYPE) {
            if (!parse_type_decl(&lex, builder, err)) {
                return false;
            }
            continue;
        }
        if (tok.kind == SC_TOK_IMPORT) {
            if (!parse_import_decl(&lex, builder, err)) {
                return false;
            }
            continue;
        }
        if (tok.kind == SC_TOK_FUNC) {
            if (!parse_func(&lex, builder, err)) {
                return false;
            }
            continue;
        }
        sc_error_set(err, tok.span, "expected .type, .import, or .func");
        return false;
    }
    return sc_builder_finish(builder, out_module, err);
}
