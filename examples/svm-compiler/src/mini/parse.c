#include "sc_mini.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    MT_EOF,
    MT_MODULE,
    MT_FN,
    MT_LET,
    MT_IF,
    MT_ELSE,
    MT_WHILE,
    MT_RETURN,
    MT_TRUE,
    MT_FALSE,
    MT_IDENT,
    MT_INT,
    MT_LPAREN,
    MT_RPAREN,
    MT_LBRACE,
    MT_RBRACE,
    MT_COMMA,
    MT_COLON,
    MT_ARROW,
    MT_SEMI,
    MT_PLUS,
    MT_MINUS,
    MT_STAR,
    MT_LE,
    MT_EQEQ,
    MT_EQ
} MiniTokKind;

typedef struct {
    MiniTokKind kind;
    ScSpan span;
    const char *text;
    size_t length;
    int32_t int_value;
} MiniTok;

typedef struct {
    const char *path;
    const char *src;
    size_t len;
    size_t pos;
    uint32_t line;
    uint32_t column;
    ScArena *arena;
    ScError *err;
} MiniLex;

static void ml_init(MiniLex *L, const char *path, const char *src, ScArena *arena, ScError *err) {
    L->path = path;
    L->src = src;
    L->len = strlen(src);
    L->pos = 0;
    L->line = 1;
    L->column = 1;
    L->arena = arena;
    L->err = err;
}

static ScSpan ml_span(const MiniLex *L, uint32_t line, uint32_t column) {
    ScSpan s = {.path = L->path, .line = line, .column = column};
    return s;
}

static char ml_peek(const MiniLex *L) {
    return L->pos < L->len ? L->src[L->pos] : '\0';
}

static char ml_adv(MiniLex *L) {
    char c = ml_peek(L);
    if (c == '\0') {
        return c;
    }
    L->pos += 1;
    if (c == '\n') {
        L->line += 1;
        L->column = 1;
    } else {
        L->column += 1;
    }
    return c;
}

static void ml_skip(MiniLex *L) {
    for (;;) {
        char c = ml_peek(L);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ml_adv(L);
            continue;
        }
        if (c == '/' && L->pos + 1 < L->len && L->src[L->pos + 1] == '/') {
            while (ml_peek(L) != '\0' && ml_peek(L) != '\n') {
                ml_adv(L);
            }
            continue;
        }
        break;
    }
}

static bool ml_keyword(const char *s, size_t n, const char *kw) {
    return strlen(kw) == n && strncmp(s, kw, n) == 0;
}

static bool ml_next(MiniLex *L, MiniTok *out) {
    ml_skip(L);
    uint32_t line = L->line;
    uint32_t column = L->column;
    char c = ml_peek(L);
    if (c == '\0') {
        out->kind = MT_EOF;
        out->span = ml_span(L, line, column);
        out->text = L->src + L->pos;
        out->length = 0;
        return true;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        const char *start = L->src + L->pos;
        ml_adv(L);
        while (isalnum((unsigned char)ml_peek(L)) || ml_peek(L) == '_') {
            ml_adv(L);
        }
        size_t n = (size_t)((L->src + L->pos) - start);
        out->span = ml_span(L, line, column);
        out->text = start;
        out->length = n;
        out->int_value = 0;
        if (ml_keyword(start, n, "module")) {
            out->kind = MT_MODULE;
        } else if (ml_keyword(start, n, "fn")) {
            out->kind = MT_FN;
        } else if (ml_keyword(start, n, "let")) {
            out->kind = MT_LET;
        } else if (ml_keyword(start, n, "if")) {
            out->kind = MT_IF;
        } else if (ml_keyword(start, n, "else")) {
            out->kind = MT_ELSE;
        } else if (ml_keyword(start, n, "while")) {
            out->kind = MT_WHILE;
        } else if (ml_keyword(start, n, "return")) {
            out->kind = MT_RETURN;
        } else if (ml_keyword(start, n, "true")) {
            out->kind = MT_TRUE;
        } else if (ml_keyword(start, n, "false")) {
            out->kind = MT_FALSE;
        } else {
            out->kind = MT_IDENT;
        }
        return true;
    }
    if (isdigit((unsigned char)c)) {
        const char *start = L->src + L->pos;
        while (isdigit((unsigned char)ml_peek(L))) {
            ml_adv(L);
        }
        size_t n = (size_t)((L->src + L->pos) - start);
        char buf[32];
        if (n >= sizeof(buf)) {
            sc_error_set(L->err, ml_span(L, line, column), "integer too long");
            return false;
        }
        memcpy(buf, start, n);
        buf[n] = '\0';
        out->kind = MT_INT;
        out->span = ml_span(L, line, column);
        out->text = start;
        out->length = n;
        out->int_value = (int32_t)strtol(buf, NULL, 10);
        return true;
    }
    if (c == '-' && L->pos + 1 < L->len && L->src[L->pos + 1] == '>') {
        ml_adv(L);
        ml_adv(L);
        out->kind = MT_ARROW;
        out->span = ml_span(L, line, column);
        out->text = "->";
        out->length = 2;
        return true;
    }
    if (c == '<' && L->pos + 1 < L->len && L->src[L->pos + 1] == '=') {
        ml_adv(L);
        ml_adv(L);
        out->kind = MT_LE;
        out->span = ml_span(L, line, column);
        out->text = "<=";
        out->length = 2;
        return true;
    }
    if (c == '=' && L->pos + 1 < L->len && L->src[L->pos + 1] == '=') {
        ml_adv(L);
        ml_adv(L);
        out->kind = MT_EQEQ;
        out->span = ml_span(L, line, column);
        out->text = "==";
        out->length = 2;
        return true;
    }
    ml_adv(L);
    out->span = ml_span(L, line, column);
    out->text = L->src + (L->pos - 1);
    out->length = 1;
    out->int_value = 0;
    switch (c) {
        case '(':
            out->kind = MT_LPAREN;
            return true;
        case ')':
            out->kind = MT_RPAREN;
            return true;
        case '{':
            out->kind = MT_LBRACE;
            return true;
        case '}':
            out->kind = MT_RBRACE;
            return true;
        case ',':
            out->kind = MT_COMMA;
            return true;
        case ':':
            out->kind = MT_COLON;
            return true;
        case ';':
            out->kind = MT_SEMI;
            return true;
        case '+':
            out->kind = MT_PLUS;
            return true;
        case '-':
            out->kind = MT_MINUS;
            return true;
        case '*':
            out->kind = MT_STAR;
            return true;
        case '=':
            out->kind = MT_EQ;
            return true;
        default:
            sc_error_set(L->err, out->span, "unexpected character");
            return false;
    }
}

static char *dup_tok(MiniLex *L, const MiniTok *tok) {
    return sc_arena_strndup(L->arena, tok->text, tok->length);
}

static ScAst *ast_new(MiniLex *L, ScAstKind kind, ScSpan span) {
    ScAst *n = sc_arena_alloc(L->arena, sizeof(ScAst));
    if (n == NULL) {
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->kind = kind;
    n->span = span;
    n->type = SVM_TYPE_UNINIT;
    return n;
}

typedef struct {
    MiniLex *L;
    MiniTok cur;
} MiniParser;

static bool p_adv(MiniParser *P) {
    return ml_next(P->L, &P->cur);
}

static bool p_expect(MiniParser *P, MiniTokKind kind, const char *what) {
    if (P->cur.kind != kind) {
        sc_error_set(P->L->err, P->cur.span, "expected %s", what);
        return false;
    }
    return p_adv(P);
}

static bool parse_type(MiniParser *P, SvmType *out) {
    if (P->cur.kind != MT_IDENT) {
        sc_error_set(P->L->err, P->cur.span, "expected type");
        return false;
    }
    if (P->cur.length == 3 && strncmp(P->cur.text, "i32", 3) == 0) {
        *out = SVM_TYPE_I32;
    } else if (P->cur.length == 4 && strncmp(P->cur.text, "bool", 4) == 0) {
        *out = SVM_TYPE_BOOL;
    } else if (P->cur.length == 4 && strncmp(P->cur.text, "void", 4) == 0) {
        *out = SVM_TYPE_VOID;
    } else {
        sc_error_set(P->L->err, P->cur.span, "unsupported Mini type");
        return false;
    }
    return p_adv(P);
}

static ScAst *parse_expr(MiniParser *P);
static ScAst *parse_stmt(MiniParser *P);
static ScAst *parse_block(MiniParser *P);

static ScAst *parse_primary(MiniParser *P) {
    if (P->cur.kind == MT_INT) {
        ScAst *n = ast_new(P->L, SC_AST_INT, P->cur.span);
        n->as.int_value = P->cur.int_value;
        if (!p_adv(P)) {
            return NULL;
        }
        return n;
    }
    if (P->cur.kind == MT_TRUE || P->cur.kind == MT_FALSE) {
        ScAst *n = ast_new(P->L, SC_AST_BOOL, P->cur.span);
        n->as.bool_value = P->cur.kind == MT_TRUE;
        if (!p_adv(P)) {
            return NULL;
        }
        return n;
    }
    if (P->cur.kind == MT_IDENT) {
        ScSpan span = P->cur.span;
        char *name = dup_tok(P->L, &P->cur);
        if (!p_adv(P)) {
            return NULL;
        }
        if (P->cur.kind == MT_LPAREN) {
            if (!p_adv(P)) {
                return NULL;
            }
            ScAst **args = NULL;
            uint32_t argc = 0;
            uint32_t cap = 0;
            if (P->cur.kind != MT_RPAREN) {
                for (;;) {
                    ScAst *arg = parse_expr(P);
                    if (arg == NULL) {
                        return NULL;
                    }
                    if (argc == cap) {
                        uint32_t next = cap == 0 ? 4u : cap * 2u;
                        ScAst **grown = sc_arena_alloc(P->L->arena, next * sizeof(ScAst *));
                        if (grown == NULL) {
                            return NULL;
                        }
                        if (args != NULL) {
                            memcpy(grown, args, argc * sizeof(ScAst *));
                        }
                        args = grown;
                        cap = next;
                    }
                    args[argc++] = arg;
                    if (P->cur.kind == MT_COMMA) {
                        if (!p_adv(P)) {
                            return NULL;
                        }
                        continue;
                    }
                    break;
                }
            }
            if (!p_expect(P, MT_RPAREN, ")")) {
                return NULL;
            }
            ScAst *call = ast_new(P->L, SC_AST_CALL, span);
            call->as.call.callee = name;
            call->as.call.args = args;
            call->as.call.arg_count = argc;
            return call;
        }
        ScAst *var = ast_new(P->L, SC_AST_VAR, span);
        var->as.name = name;
        return var;
    }
    if (P->cur.kind == MT_LPAREN) {
        if (!p_adv(P)) {
            return NULL;
        }
        ScAst *e = parse_expr(P);
        if (e == NULL || !p_expect(P, MT_RPAREN, ")")) {
            return NULL;
        }
        return e;
    }
    sc_error_set(P->L->err, P->cur.span, "expected expression");
    return NULL;
}

static ScAst *parse_mul(MiniParser *P) {
    ScAst *left = parse_primary(P);
    if (left == NULL) {
        return NULL;
    }
    while (P->cur.kind == MT_STAR) {
        ScSpan span = P->cur.span;
        if (!p_adv(P)) {
            return NULL;
        }
        ScAst *right = parse_primary(P);
        if (right == NULL) {
            return NULL;
        }
        ScAst *n = ast_new(P->L, SC_AST_BINARY, span);
        n->as.binary.op = SC_BIN_MUL;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static ScAst *parse_add(MiniParser *P) {
    ScAst *left = parse_mul(P);
    if (left == NULL) {
        return NULL;
    }
    while (P->cur.kind == MT_PLUS || P->cur.kind == MT_MINUS) {
        ScBinOp op = P->cur.kind == MT_PLUS ? SC_BIN_ADD : SC_BIN_SUB;
        ScSpan span = P->cur.span;
        if (!p_adv(P)) {
            return NULL;
        }
        ScAst *right = parse_mul(P);
        if (right == NULL) {
            return NULL;
        }
        ScAst *n = ast_new(P->L, SC_AST_BINARY, span);
        n->as.binary.op = op;
        n->as.binary.left = left;
        n->as.binary.right = right;
        left = n;
    }
    return left;
}

static ScAst *parse_cmp(MiniParser *P) {
    ScAst *left = parse_add(P);
    if (left == NULL) {
        return NULL;
    }
    if (P->cur.kind == MT_LE || P->cur.kind == MT_EQEQ) {
        ScBinOp op = P->cur.kind == MT_LE ? SC_BIN_LE : SC_BIN_EQ;
        ScSpan span = P->cur.span;
        if (!p_adv(P)) {
            return NULL;
        }
        ScAst *right = parse_add(P);
        if (right == NULL) {
            return NULL;
        }
        ScAst *n = ast_new(P->L, SC_AST_BINARY, span);
        n->as.binary.op = op;
        n->as.binary.left = left;
        n->as.binary.right = right;
        return n;
    }
    return left;
}

static ScAst *parse_expr(MiniParser *P) {
    return parse_cmp(P);
}

static ScAst *parse_block(MiniParser *P) {
    ScSpan span = P->cur.span;
    if (!p_expect(P, MT_LBRACE, "{")) {
        return NULL;
    }
    ScAst **stmts = NULL;
    uint32_t count = 0;
    uint32_t cap = 0;
    while (P->cur.kind != MT_RBRACE && P->cur.kind != MT_EOF) {
        ScAst *s = parse_stmt(P);
        if (s == NULL) {
            return NULL;
        }
        if (count == cap) {
            uint32_t next = cap == 0 ? 8u : cap * 2u;
            ScAst **grown = sc_arena_alloc(P->L->arena, next * sizeof(ScAst *));
            if (grown == NULL) {
                return NULL;
            }
            if (stmts) {
                memcpy(grown, stmts, count * sizeof(ScAst *));
            }
            stmts = grown;
            cap = next;
        }
        stmts[count++] = s;
    }
    if (!p_expect(P, MT_RBRACE, "}")) {
        return NULL;
    }
    ScAst *block = ast_new(P->L, SC_AST_BLOCK, span);
    block->as.block.stmts = stmts;
    block->as.block.count = count;
    return block;
}

static ScAst *parse_stmt(MiniParser *P) {
    if (P->cur.kind == MT_LET) {
        ScSpan span = P->cur.span;
        if (!p_adv(P) || P->cur.kind != MT_IDENT) {
            sc_error_set(P->L->err, P->cur.span, "expected name after let");
            return NULL;
        }
        char *name = dup_tok(P->L, &P->cur);
        if (!p_adv(P) || !p_expect(P, MT_COLON, ":")) {
            return NULL;
        }
        SvmType type;
        if (!parse_type(P, &type) || !p_expect(P, MT_EQ, "=")) {
            return NULL;
        }
        ScAst *init = parse_expr(P);
        if (init == NULL || !p_expect(P, MT_SEMI, ";")) {
            return NULL;
        }
        ScAst *n = ast_new(P->L, SC_AST_LET, span);
        n->as.let.name = name;
        n->as.let.type = type;
        n->as.let.init = init;
        return n;
    }
    if (P->cur.kind == MT_IF) {
        ScSpan span = P->cur.span;
        if (!p_adv(P) || !p_expect(P, MT_LPAREN, "(")) {
            return NULL;
        }
        ScAst *cond = parse_expr(P);
        if (cond == NULL || !p_expect(P, MT_RPAREN, ")")) {
            return NULL;
        }
        ScAst *then_b = parse_block(P);
        if (then_b == NULL) {
            return NULL;
        }
        ScAst *else_b = NULL;
        if (P->cur.kind == MT_ELSE) {
            if (!p_adv(P)) {
                return NULL;
            }
            else_b = parse_block(P);
            if (else_b == NULL) {
                return NULL;
            }
        }
        ScAst *n = ast_new(P->L, SC_AST_IF, span);
        n->as.if_stmt.cond = cond;
        n->as.if_stmt.then_branch = then_b;
        n->as.if_stmt.else_branch = else_b;
        return n;
    }
    if (P->cur.kind == MT_WHILE) {
        ScSpan span = P->cur.span;
        if (!p_adv(P) || !p_expect(P, MT_LPAREN, "(")) {
            return NULL;
        }
        ScAst *cond = parse_expr(P);
        if (cond == NULL || !p_expect(P, MT_RPAREN, ")")) {
            return NULL;
        }
        ScAst *body = parse_block(P);
        if (body == NULL) {
            return NULL;
        }
        ScAst *n = ast_new(P->L, SC_AST_WHILE, span);
        n->as.while_stmt.cond = cond;
        n->as.while_stmt.body = body;
        return n;
    }
    if (P->cur.kind == MT_RETURN) {
        ScSpan span = P->cur.span;
        if (!p_adv(P)) {
            return NULL;
        }
        ScAst *value = NULL;
        if (P->cur.kind != MT_SEMI) {
            value = parse_expr(P);
            if (value == NULL) {
                return NULL;
            }
        }
        if (!p_expect(P, MT_SEMI, ";")) {
            return NULL;
        }
        ScAst *n = ast_new(P->L, SC_AST_RETURN, span);
        n->as.ret.value = value;
        return n;
    }
    if (P->cur.kind == MT_LBRACE) {
        return parse_block(P);
    }
    if (P->cur.kind == MT_IDENT) {
        MiniTok name_tok = P->cur;
        MiniLex saved = *P->L;
        MiniTok after;
        if (!p_adv(P)) {
            return NULL;
        }
        if (P->cur.kind == MT_EQ) {
            char *name = dup_tok(P->L, &name_tok);
            if (!p_adv(P)) {
                return NULL;
            }
            ScAst *value = parse_expr(P);
            if (value == NULL || !p_expect(P, MT_SEMI, ";")) {
                return NULL;
            }
            ScAst *n = ast_new(P->L, SC_AST_ASSIGN, name_tok.span);
            n->as.assign.name = name;
            n->as.assign.value = value;
            return n;
        }
        *P->L = saved;
        P->cur = name_tok;
        (void)after;
    }
    ScAst *expr = parse_expr(P);
    if (expr == NULL || !p_expect(P, MT_SEMI, ";")) {
        return NULL;
    }
    ScAst *n = ast_new(P->L, SC_AST_EXPR_STMT, expr->span);
    n->as.expr = expr;
    return n;
}

static bool parse_fn(MiniParser *P, ScFnDecl *fn) {
    memset(fn, 0, sizeof(*fn));
    fn->span = P->cur.span;
    if (!p_expect(P, MT_FN, "fn")) {
        return false;
    }
    if (P->cur.kind != MT_IDENT) {
        sc_error_set(P->L->err, P->cur.span, "expected function name");
        return false;
    }
    fn->name = dup_tok(P->L, &P->cur);
    if (!p_adv(P) || !p_expect(P, MT_LPAREN, "(")) {
        return false;
    }
    ScParam params[SVM_MAX_LOCALS];
    uint32_t pc = 0;
    if (P->cur.kind != MT_RPAREN) {
        for (;;) {
            if (P->cur.kind != MT_IDENT) {
                sc_error_set(P->L->err, P->cur.span, "expected param name");
                return false;
            }
            params[pc].name = dup_tok(P->L, &P->cur);
            if (!p_adv(P) || !p_expect(P, MT_COLON, ":") ||
                !parse_type(P, &params[pc].type)) {
                return false;
            }
            pc += 1;
            if (P->cur.kind == MT_COMMA) {
                if (!p_adv(P)) {
                    return false;
                }
                continue;
            }
            break;
        }
    }
    if (!p_expect(P, MT_RPAREN, ")") || !p_expect(P, MT_ARROW, "->") ||
        !parse_type(P, &fn->result_type)) {
        return false;
    }
    fn->param_count = pc;
    if (pc > 0) {
        fn->params = sc_arena_alloc(P->L->arena, pc * sizeof(ScParam));
        memcpy(fn->params, params, pc * sizeof(ScParam));
    }
    fn->body = parse_block(P);
    return fn->body != NULL;
}

bool sc_mini_parse(
    const char *path,
    const char *source,
    ScArena *arena,
    ScModuleAst **out_module,
    ScError *err
) {
    MiniLex lex;
    ml_init(&lex, path, source, arena, err);
    MiniParser P = {.L = &lex};
    if (!p_adv(&P)) {
        return false;
    }
    if (!p_expect(&P, MT_MODULE, "module")) {
        return false;
    }
    if (P.cur.kind != MT_IDENT) {
        sc_error_set(err, P.cur.span, "expected module name");
        return false;
    }
    ScModuleAst *mod = sc_arena_alloc(arena, sizeof(ScModuleAst));
    memset(mod, 0, sizeof(*mod));
    mod->name = dup_tok(&lex, &P.cur);
    if (!p_adv(&P)) {
        return false;
    }
    ScFnDecl fns[64];
    uint32_t fc = 0;
    while (P.cur.kind == MT_FN) {
        if (fc >= 64) {
            sc_error_set(err, P.cur.span, "too many functions");
            return false;
        }
        if (!parse_fn(&P, &fns[fc])) {
            return false;
        }
        fc += 1;
    }
    if (P.cur.kind != MT_EOF) {
        sc_error_set(err, P.cur.span, "expected fn or end of file");
        return false;
    }
    mod->function_count = fc;
    mod->functions = sc_arena_alloc(arena, fc * sizeof(ScFnDecl));
    memcpy(mod->functions, fns, fc * sizeof(ScFnDecl));
    *out_module = mod;
    return true;
}
