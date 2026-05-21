/*
 * Recursive-descent parser.
 *
 * Scope (issue #4): chain declarations only.
 *
 *   chain_decl      := "chain" IDENT "{" subsystem_assign* "}"
 *   subsystem_assign := IDENT ":" type_expr
 *   type_expr       := IDENT type_args?
 *   type_args       := "<" type_param ("," type_param)* ","? ">"
 *   type_param      := (IDENT "=")? type_param_value
 *   type_param_value := IDENT | NUMBER | STRING | "{" set_value ("," set_value)* "}"
 *   set_value       := IDENT | NUMBER
 *
 * Module bodies, function definitions, statements, and expressions are out of
 * scope here. Those come in a later issue.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"

/* Subsystem keys are lexed as their own keyword tokens (see lexer.c). The
 * parser accepts any of these as a chain-block field key. */
static int is_subsystem_key(TokenKind k) {
    switch (k) {
    case TK_KW_CONSENSUS:
    case TK_KW_GAS:
    case TK_KW_STATE:
    case TK_KW_EXEC:
    case TK_KW_DA:
        return 1;
    default:
        return 0;
    }
}

/* ============== token utilities ============== */

static void advance(Parser *p) {
    p->current = lexer_next(p->lex);
}

static int check(const Parser *p, TokenKind k) {
    return p->current.kind == k;
}

static int match(Parser *p, TokenKind k) {
    if (!check(p, k)) return 0;
    advance(p);
    return 1;
}

static StrRef strref_from_token(Token t) {
    StrRef r;
    r.start = t.start;
    r.length = t.length;
    return r;
}

/* ============== error reporting ============== */

static void error_at(Parser *p, const Token *tok, const char *msg) {
    /* Only report the first error per parse; recovery is for a later milestone. */
    if (p->had_error) return;
    p->had_error = 1;

    fprintf(stderr, "%zu:%zu: error: %s", tok->line, tok->column, msg);
    if (tok->kind == TK_EOF) {
        fputs(" (at end of file)\n", stderr);
    } else {
        fprintf(stderr, " (at '%.*s')\n", (int)tok->length, tok->start);
    }
}

static int expect(Parser *p, TokenKind k, const char *what) {
    if (check(p, k)) {
        advance(p);
        return 1;
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "expected %s", what);
    error_at(p, &p->current, buf);
    return 0;
}

/* ============== node helpers ============== */

static AstNode *new_node(AstKind kind, Token start) {
    AstNode *n = calloc(1, sizeof(AstNode));
    if (!n) {
        fputs("fatal: out of memory in parser\n", stderr);
        exit(1);
    }
    n->kind = kind;
    n->span.start_line = start.line;
    n->span.start_col = start.column;
    n->span.end_line = start.line;
    n->span.end_col = start.column + start.length;
    return n;
}

static void node_set_end(AstNode *n, Token end) {
    n->span.end_line = end.line;
    n->span.end_col = end.column + end.length;
}

/* Append a child to a dynamically-grown array. */
static void push_child(AstNode ***arr, size_t *n, size_t *cap, AstNode *child) {
    if (*n >= *cap) {
        *cap = (*cap == 0) ? 4 : (*cap * 2);
        AstNode **next = realloc(*arr, *cap * sizeof(AstNode *));
        if (!next) {
            fputs("fatal: out of memory in parser\n", stderr);
            exit(1);
        }
        *arr = next;
    }
    (*arr)[(*n)++] = child;
}

/* ============== productions ============== */

static AstNode *parse_type_expr(Parser *p);
static AstNode *parse_type_param(Parser *p);
static AstNode *parse_type_param_value(Parser *p);

/* `{` set_value (`,` set_value)* `,`? `}`
 * Modeled as a TypeGeneric with empty name; children are positional TypeParams. */
static AstNode *parse_set_literal(Parser *p) {
    Token open = p->current;
    if (!expect(p, TK_LBRACE, "'{'")) return NULL;

    AstNode *set = new_node(AST_TYPE_GENERIC, open);
    set->as.type_generic.name.start = NULL;
    set->as.type_generic.name.length = 0;
    set->as.type_generic.args = NULL;
    set->as.type_generic.n_args = 0;

    AstNode **args = NULL;
    size_t n_args = 0, cap = 0;

    if (!check(p, TK_RBRACE)) {
        for (;;) {
            Token value_start = p->current;
            AstNode *value = parse_type_param_value(p);
            if (!value) return NULL;

            AstNode *param = new_node(AST_TYPE_PARAM, value_start);
            param->as.type_param.key.start = NULL;
            param->as.type_param.key.length = 0;
            param->as.type_param.value = value;
            node_set_end(param, p->current);

            push_child(&args, &n_args, &cap, param);

            if (!match(p, TK_COMMA)) break;
            if (check(p, TK_RBRACE)) break; /* trailing comma */
        }
    }

    Token close = p->current;
    if (!expect(p, TK_RBRACE, "'}'")) return NULL;

    set->as.type_generic.args = args;
    set->as.type_generic.n_args = n_args;
    node_set_end(set, close);
    return set;
}

/* type_param_value := IDENT | NUMBER | STRING | set_literal */
static AstNode *parse_type_param_value(Parser *p) {
    if (check(p, TK_LBRACE)) {
        return parse_set_literal(p);
    }
    if (check(p, TK_IDENT)) {
        Token t = p->current;
        advance(p);
        AstNode *n = new_node(AST_EXPR_IDENT, t);
        n->as.ident.name = strref_from_token(t);
        node_set_end(n, t);
        return n;
    }
    if (check(p, TK_NUMBER)) {
        Token t = p->current;
        advance(p);
        AstNode *n = new_node(AST_EXPR_NUMBER, t);
        n->as.number.text = strref_from_token(t);
        node_set_end(n, t);
        return n;
    }
    if (check(p, TK_STRING)) {
        Token t = p->current;
        advance(p);
        AstNode *n = new_node(AST_EXPR_STRING, t);
        n->as.string.text = strref_from_token(t);
        node_set_end(n, t);
        return n;
    }
    error_at(p, &p->current, "expected identifier, number, string, or '{...}'");
    return NULL;
}

/* type_param := (IDENT "=")? type_param_value */
static AstNode *parse_type_param(Parser *p) {
    Token start = p->current;

    /* Try `IDENT =` as a key. We need lookahead. Peek by checking current is
     * IDENT, then advancing tentatively. If next is '=', it's a key; otherwise
     * the IDENT was a positional value. */
    StrRef key = { NULL, 0 };
    if (check(p, TK_IDENT)) {
        Token ident = p->current;
        Lexer save = *p->lex;
        Token saved_current = p->current;
        advance(p); /* consume IDENT */
        if (check(p, TK_ASSIGN)) {
            advance(p); /* consume '=' */
            key = strref_from_token(ident);
        } else {
            /* Not a key; rewind by restoring lexer and current token. */
            *p->lex = save;
            p->current = saved_current;
        }
    }

    AstNode *value = parse_type_param_value(p);
    if (!value) return NULL;

    AstNode *param = new_node(AST_TYPE_PARAM, start);
    param->as.type_param.key = key;
    param->as.type_param.value = value;
    node_set_end(param, p->current);
    return param;
}

/* type_expr := IDENT type_args? */
static AstNode *parse_type_expr(Parser *p) {
    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected a type name");
        return NULL;
    }

    Token name_tok = p->current;
    advance(p);

    /* No generic args: TypeNamed. */
    if (!check(p, TK_LT)) {
        AstNode *n = new_node(AST_TYPE_NAMED, name_tok);
        n->as.type_named.name = strref_from_token(name_tok);
        node_set_end(n, name_tok);
        return n;
    }

    /* Generic args: TypeGeneric. */
    advance(p); /* consume '<' */

    AstNode *generic = new_node(AST_TYPE_GENERIC, name_tok);
    generic->as.type_generic.name = strref_from_token(name_tok);

    AstNode **args = NULL;
    size_t n_args = 0, cap = 0;

    if (!check(p, TK_GT)) {
        for (;;) {
            AstNode *param = parse_type_param(p);
            if (!param) return NULL;
            push_child(&args, &n_args, &cap, param);
            if (!match(p, TK_COMMA)) break;
            if (check(p, TK_GT)) break; /* trailing comma */
        }
    }

    Token close = p->current;
    if (!expect(p, TK_GT, "'>'")) return NULL;

    generic->as.type_generic.args = args;
    generic->as.type_generic.n_args = n_args;
    node_set_end(generic, close);
    return generic;
}

/* subsystem_assign := SUBSYSTEM_KEY ":" type_expr
 * SUBSYSTEM_KEY is one of the keyword tokens (TK_KW_CONSENSUS, TK_KW_GAS, ...). */
static AstNode *parse_subsystem_assign(Parser *p) {
    if (!is_subsystem_key(p->current.kind)) {
        error_at(p, &p->current,
                 "expected a subsystem key (consensus, gas, state, exec, da)");
        return NULL;
    }

    Token key_tok = p->current;
    advance(p);

    if (!expect(p, TK_COLON, "':' after subsystem name")) return NULL;

    AstNode *value = parse_type_expr(p);
    if (!value) return NULL;

    AstNode *node = new_node(AST_SUBSYSTEM_ASSIGN, key_tok);
    node->as.subsystem.key = strref_from_token(key_tok);
    node->as.subsystem.value = value;
    node_set_end(node, p->current);
    return node;
}

/* chain_decl := "chain" IDENT "{" subsystem_assign* "}" */
AstNode *parser_parse_chain(Parser *p) {
    if (!check(p, TK_KW_CHAIN)) {
        error_at(p, &p->current, "expected 'chain'");
        return NULL;
    }
    Token chain_kw = p->current;
    advance(p);

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected a chain name after 'chain'");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    if (!expect(p, TK_LBRACE, "'{' to open the chain body")) return NULL;

    AstNode **assignments = NULL;
    size_t n_assignments = 0, cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        AstNode *sub = parse_subsystem_assign(p);
        if (!sub) return NULL;
        push_child(&assignments, &n_assignments, &cap, sub);

        /* Optional `;` or `,` between assignments (we don't require them since
         * the grammar uses newlines as separators in practice). */
        (void)match(p, TK_SEMICOLON);
        (void)match(p, TK_COMMA);
    }

    Token close_brace = p->current;
    if (!expect(p, TK_RBRACE, "'}' to close the chain body")) return NULL;

    AstNode *chain = new_node(AST_CHAIN_DECL, chain_kw);
    chain->as.chain.name = strref_from_token(name_tok);
    chain->as.chain.assignments = assignments;
    chain->as.chain.n_assignments = n_assignments;
    node_set_end(chain, close_brace);
    return chain;
}

/* ============== public init ============== */

void parser_init(Parser *p, Lexer *lex) {
    p->lex = lex;
    p->had_error = 0;
    /* Prime the parser with the first token. */
    p->current = lexer_next(lex);
}

int parser_had_error(const Parser *p) {
    return p->had_error;
}
