/*
 * Recursive-descent parser.
 *
 * Productions covered:
 *
 *   program          := top_level*
 *   top_level        := chain_decl | module_decl | protocol_decl
 *
 *   chain_decl       := "chain" IDENT "{" subsystem_assign* "}"
 *   subsystem_assign := SUBSYSTEM_KEY ":" type_expr (";"|",")?
 *
 *   module_decl      := "module" IDENT "{" module_item* "}"
 *   module_item      := state_decl | gas_decl | event_decl | fn_decl | effect_decl
 *
 *   protocol_decl    := "protocol" IDENT ("implements" type_expr)?
 *                       "{" protocol_item* "}"
 *   protocol_item    := state_decl | effect_decl | fn_decl | slash_on_decl
 *
 *   state_decl       := "state" IDENT ":" type_expr
 *   gas_decl         := "gas" IDENT "=" expr
 *   event_decl       := "event" IDENT "(" param_list? ")"
 *   effect_decl      := "effect" IDENT "(" param_list? ")"
 *                       ("->" type_expr)? "deferred"?
 *   slash_on_decl    := "slash_on" IDENT "{" (IDENT ":" type_expr ","?)* "}"
 *                       ("when" expr)? ("penalty" expr)?
 *
 *   fn_decl          := ("pure"|"view")? "fn" IDENT "(" param_list? ")"
 *                       ("->" type_expr)?
 *                       ("with" "[" (IDENT ("," IDENT)*)? "]")? block
 *   param_list       := param ("," param)*
 *   param            := IDENT ":" type_expr
 *
 *   block            := "{" stmt* expr? "}"
 *   stmt             := let_stmt | return_stmt | if_stmt | match_stmt | expr_stmt
 *   let_stmt         := "let" IDENT (":" type_expr)? "=" expr
 *   return_stmt      := "return" expr?
 *   if_stmt          := "if" expr block ("else" (block | if_stmt))?
 *   match_stmt       := "match" expr "{" match_arm* "}"
 *   match_arm        := expr "=>" expr ","?
 *   expr_stmt        := expr
 *
 *   expr             := binary expression with precedence climbing
 *   primary          := IDENT | NUMBER | STRING | CHAR | "true" | "false" | "null"
 *                       | "(" expr ")" | block | record_literal
 *                       | call | index | member | path | unary | percent
 *   record_literal   := "{" record_field ("," record_field)* ","? "}"
 *   record_field     := IDENT ":" expr
 *
 *   type_expr        := IDENT type_args?
 *   type_args        := "<" type_param ("," type_param)* ","? ">"
 *   type_param       := (IDENT "=")? type_param_value
 *   type_param_value := IDENT | NUMBER | STRING | set_literal
 *   set_literal      := "{" type_param_value ("," type_param_value)* ","? "}"
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

/* Forward declarations for the expression / statement / declaration
 * grammar added by issue #35. Defined further down this file. */
static AstNode *parse_expression(Parser *p);
static AstNode *parse_expression_with_prec(Parser *p, int min_prec);
static AstNode *parse_unary(Parser *p);
static AstNode *parse_postfix(Parser *p, AstNode *lhs);
static AstNode *parse_primary(Parser *p);
static AstNode *parse_block_or_record(Parser *p);
static AstNode *parse_block(Parser *p);
static AstNode *parse_record_literal(Parser *p);
static AstNode *parse_statement(Parser *p);
static AstNode *parse_let_stmt(Parser *p);
static AstNode *parse_return_stmt(Parser *p);
static AstNode *parse_if_stmt(Parser *p);
static AstNode *parse_match_stmt(Parser *p);
static AstNode *parse_param_list(Parser *p, AstNode ***out, size_t *n_out);
static AstNode *parse_param(Parser *p);
static AstNode *parse_fn_decl(Parser *p);
static AstNode *parse_state_decl(Parser *p);
static AstNode *parse_gas_decl(Parser *p);
static AstNode *parse_event_decl(Parser *p);
static AstNode *parse_effect_decl(Parser *p);
static AstNode *parse_slash_on_decl(Parser *p);
static AstNode *parse_module_decl(Parser *p);
static AstNode *parse_protocol_decl(Parser *p);
static AstNode *parse_top_level(Parser *p);

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

/* ============== expressions ============== */

/* Operator precedence table. Higher numbers bind tighter.
 *
 * The values mirror tree-sitter's grammar.js exactly so the two parsers
 * stay in lockstep. The "binary" levels are left-associative; postfix
 * and unary operators are handled in their own production functions and
 * never enter this table. */
static int binary_prec(TokenKind k) {
    switch (k) {
    case TK_STAR:
    case TK_SLASH:    return 10;
    case TK_PLUS:
    case TK_MINUS:    return 9;
    case TK_EQ:
    case TK_NEQ:
    case TK_LT:
    case TK_GT:
    case TK_LEQ:
    case TK_GEQ:      return 8;
    case TK_AND:      return 7;
    case TK_OR:       return 6;
    case TK_ASSIGN:   return 5;
    default:          return 0;
    }
}

/* Make a binary-expression node. The operator lexeme is stored as the
 * substring of source between op_tok.start and op_tok.start+op_tok.length. */
static AstNode *new_binary(Token op_tok, AstNode *lhs, AstNode *rhs) {
    AstNode *n = new_node(AST_EXPR_BINARY, op_tok);
    n->span.start_line = lhs->span.start_line;
    n->span.start_col = lhs->span.start_col;
    n->span.end_line = rhs->span.end_line;
    n->span.end_col = rhs->span.end_col;
    n->as.binary.op = strref_from_token(op_tok);
    n->as.binary.lhs = lhs;
    n->as.binary.rhs = rhs;
    return n;
}

static AstNode *parse_expression(Parser *p) {
    /* Lowest binary precedence is 5 (`=`); start one below so any operator
     * can begin a binary expression. */
    return parse_expression_with_prec(p, 1);
}

static AstNode *parse_expression_with_prec(Parser *p, int min_prec) {
    AstNode *lhs = parse_unary(p);
    if (!lhs) return NULL;

    for (;;) {
        int prec = binary_prec(p->current.kind);
        if (prec == 0 || prec < min_prec) return lhs;

        Token op_tok = p->current;
        advance(p);

        AstNode *rhs = parse_expression_with_prec(p, prec + 1);
        if (!rhs) return NULL;

        lhs = new_binary(op_tok, lhs, rhs);
    }
}

/* Unary: ("!"|"-") expr | postfix */
static AstNode *parse_unary(Parser *p) {
    if (check(p, TK_BANG) || check(p, TK_MINUS)) {
        Token op_tok = p->current;
        advance(p);
        AstNode *operand = parse_unary(p);
        if (!operand) return NULL;

        AstNode *n = new_node(AST_EXPR_UNARY, op_tok);
        n->as.unary.op = strref_from_token(op_tok);
        n->as.unary.operand = operand;
        n->span.end_line = operand->span.end_line;
        n->span.end_col = operand->span.end_col;
        return n;
    }

    AstNode *primary = parse_primary(p);
    if (!primary) return NULL;
    return parse_postfix(p, primary);
}

/* Postfix: call (...), index [...], member .name, path ::name, percent %.
 * These all bind tighter than any binary operator. */
static AstNode *parse_postfix(Parser *p, AstNode *lhs) {
    for (;;) {
        if (check(p, TK_LPAREN)) {
            Token open = p->current;
            (void)open;
            advance(p);
            AstNode *call = new_node(AST_EXPR_CALL, p->current);
            call->span.start_line = lhs->span.start_line;
            call->span.start_col = lhs->span.start_col;
            call->as.call.callee = lhs;
            call->as.call.args = NULL;
            call->as.call.n_args = 0;
            size_t cap = 0;
            if (!check(p, TK_RPAREN)) {
                for (;;) {
                    AstNode *arg = parse_expression(p);
                    if (!arg) return NULL;
                    push_child(&call->as.call.args, &call->as.call.n_args,
                               &cap, arg);
                    if (!match(p, TK_COMMA)) break;
                    if (check(p, TK_RPAREN)) break; /* trailing comma */
                }
            }
            Token close = p->current;
            if (!expect(p, TK_RPAREN, "')' to close call arguments")) return NULL;
            node_set_end(call, close);
            lhs = call;
            continue;
        }
        if (check(p, TK_LBRACKET)) {
            advance(p);
            AstNode *idx_expr = parse_expression(p);
            if (!idx_expr) return NULL;
            Token close = p->current;
            if (!expect(p, TK_RBRACKET, "']' to close index expression")) return NULL;

            AstNode *node = new_node(AST_EXPR_INDEX, close);
            node->span.start_line = lhs->span.start_line;
            node->span.start_col = lhs->span.start_col;
            node->as.index.array = lhs;
            node->as.index.index = idx_expr;
            node_set_end(node, close);
            lhs = node;
            continue;
        }
        if (check(p, TK_DOT) || check(p, TK_COLONCOLON)) {
            Token op_tok = p->current;
            advance(p);
            if (!check(p, TK_IDENT)) {
                error_at(p, &p->current,
                         op_tok.kind == TK_DOT
                             ? "expected an identifier after '.'"
                             : "expected an identifier after '::'");
                return NULL;
            }
            Token name_tok = p->current;
            advance(p);

            AstNode *name_node = new_node(AST_EXPR_IDENT, name_tok);
            name_node->as.ident.name = strref_from_token(name_tok);
            node_set_end(name_node, name_tok);

            lhs = new_binary(op_tok, lhs, name_node);
            continue;
        }
        if (check(p, TK_PERCENT)) {
            /* `value%` postfix percent. Modeled as binary `value % <empty>` is
             * wrong; instead use unary on the left-hand side with op "%". */
            Token op_tok = p->current;
            advance(p);
            AstNode *node = new_node(AST_EXPR_UNARY, op_tok);
            node->span.start_line = lhs->span.start_line;
            node->span.start_col = lhs->span.start_col;
            node->as.unary.op = strref_from_token(op_tok);
            node->as.unary.operand = lhs;
            node_set_end(node, op_tok);
            lhs = node;
            continue;
        }
        return lhs;
    }
}

/* Primary: literal | identifier | parenthesized | block | record */
static AstNode *parse_primary(Parser *p) {
    Token tok = p->current;

    if (check(p, TK_NUMBER)) {
        advance(p);
        AstNode *n = new_node(AST_EXPR_NUMBER, tok);
        n->as.number.text = strref_from_token(tok);
        node_set_end(n, tok);
        return n;
    }
    if (check(p, TK_STRING)) {
        advance(p);
        AstNode *n = new_node(AST_EXPR_STRING, tok);
        n->as.string.text = strref_from_token(tok);
        node_set_end(n, tok);
        return n;
    }
    if (check(p, TK_CHAR)) {
        advance(p);
        AstNode *n = new_node(AST_EXPR_CHAR, tok);
        n->as.character.text = strref_from_token(tok);
        node_set_end(n, tok);
        return n;
    }
    if (check(p, TK_KW_TRUE) || check(p, TK_KW_FALSE)) {
        advance(p);
        AstNode *n = new_node(AST_EXPR_BOOL, tok);
        n->as.boolean.value = (tok.kind == TK_KW_TRUE) ? 1 : 0;
        node_set_end(n, tok);
        return n;
    }
    if (check(p, TK_KW_NULL)) {
        advance(p);
        AstNode *n = new_node(AST_EXPR_NULL, tok);
        node_set_end(n, tok);
        return n;
    }
    if (check(p, TK_IDENT)) {
        advance(p);
        AstNode *n = new_node(AST_EXPR_IDENT, tok);
        n->as.ident.name = strref_from_token(tok);
        node_set_end(n, tok);
        return n;
    }
    if (check(p, TK_LPAREN)) {
        advance(p);
        AstNode *inner = parse_expression(p);
        if (!inner) return NULL;
        if (!expect(p, TK_RPAREN, "')'")) return NULL;
        return inner;
    }
    if (check(p, TK_LBRACE)) {
        return parse_block_or_record(p);
    }

    error_at(p, &p->current, "expected an expression");
    return NULL;
}

/* Disambiguate `{ ... }` between block and record literal.
 *
 * Rule: a record literal must begin with `IDENT ":"`. Every other shape is
 * a block. Empty `{}` is a block (an empty block evaluates to unit).
 *
 * Implementation: save lexer state, peek two tokens, restore. */
static AstNode *parse_block_or_record(Parser *p) {
    /* peek next: do we have IDENT then COLON? */
    Lexer save_lex = *p->lex;
    Token save_current = p->current;

    advance(p); /* consume the '{' */

    int is_record = 0;
    if (check(p, TK_IDENT)) {
        Token first_ident = p->current;
        Lexer save2 = *p->lex;
        advance(p);
        if (check(p, TK_COLON)) {
            is_record = 1;
        }
        /* restore to just after the '{' */
        *p->lex = save2;
        p->current = first_ident;
    }

    /* restore to before the '{' so the dedicated parser sees the open brace */
    *p->lex = save_lex;
    p->current = save_current;

    return is_record ? parse_record_literal(p) : parse_block(p);
}

/* block := "{" stmt* expr? "}"
 *
 * Statements and a trailing expression coexist: the trailing expression
 * is whatever is left at the end of the block, with no following semicolon
 * or statement keyword. Tree-sitter punts the same decision to a later
 * semantic pass; we do enough work here to expose `result` separately. */
static AstNode *parse_block(Parser *p) {
    Token open = p->current;
    if (!expect(p, TK_LBRACE, "'{' to open a block")) return NULL;

    AstNode *block = new_node(AST_EXPR_BLOCK, open);
    block->as.block.stmts = NULL;
    block->as.block.n_stmts = 0;
    block->as.block.result = NULL;
    size_t cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        AstNode *item = parse_statement(p);
        if (!item) return NULL;

        /* Optional `;` between statements; allow but do not require. */
        (void)match(p, TK_SEMICOLON);

        /* If the parsed item was an expression-statement AND it sits right
         * before the closing brace, treat it as the trailing result. */
        if (item->kind == AST_STMT_EXPR && check(p, TK_RBRACE)) {
            block->as.block.result = item->as.expr_stmt.expr;
        } else {
            push_child(&block->as.block.stmts, &block->as.block.n_stmts,
                       &cap, item);
        }
    }

    Token close = p->current;
    if (!expect(p, TK_RBRACE, "'}' to close a block")) return NULL;
    node_set_end(block, close);
    return block;
}

static AstNode *parse_record_literal(Parser *p) {
    Token open = p->current;
    if (!expect(p, TK_LBRACE, "'{' to open a record literal")) return NULL;

    AstNode *rec = new_node(AST_EXPR_RECORD, open);
    rec->as.record.fields = NULL;
    rec->as.record.n_fields = 0;
    size_t cap = 0;

    if (!check(p, TK_RBRACE)) {
        for (;;) {
            if (!check(p, TK_IDENT)) {
                error_at(p, &p->current, "expected a record field name");
                return NULL;
            }
            Token key_tok = p->current;
            advance(p);
            if (!expect(p, TK_COLON, "':' after a record field name")) return NULL;
            AstNode *value = parse_expression(p);
            if (!value) return NULL;

            AstNode *field = new_node(AST_RECORD_FIELD, key_tok);
            field->as.record_field.key = strref_from_token(key_tok);
            field->as.record_field.value = value;
            field->span.end_line = value->span.end_line;
            field->span.end_col = value->span.end_col;
            push_child(&rec->as.record.fields, &rec->as.record.n_fields,
                       &cap, field);

            if (!match(p, TK_COMMA)) break;
            if (check(p, TK_RBRACE)) break; /* trailing comma */
        }
    }

    Token close = p->current;
    if (!expect(p, TK_RBRACE, "'}' to close a record literal")) return NULL;
    node_set_end(rec, close);
    return rec;
}

AstNode *parser_parse_expression(Parser *p) {
    return parse_expression(p);
}

/* ============== statements ============== */

static AstNode *parse_statement(Parser *p) {
    if (check(p, TK_KW_LET))    return parse_let_stmt(p);
    if (check(p, TK_KW_RETURN)) return parse_return_stmt(p);
    if (check(p, TK_KW_IF))     return parse_if_stmt(p);
    if (check(p, TK_KW_MATCH))  return parse_match_stmt(p);

    /* expression-statement */
    Token start = p->current;
    AstNode *expr = parse_expression(p);
    if (!expr) return NULL;
    AstNode *stmt = new_node(AST_STMT_EXPR, start);
    stmt->span.start_line = expr->span.start_line;
    stmt->span.start_col = expr->span.start_col;
    stmt->span.end_line = expr->span.end_line;
    stmt->span.end_col = expr->span.end_col;
    stmt->as.expr_stmt.expr = expr;
    return stmt;
}

static AstNode *parse_let_stmt(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'let' */

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected an identifier after 'let'");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    AstNode *type_ann = NULL;
    if (match(p, TK_COLON)) {
        type_ann = parse_type_expr(p);
        if (!type_ann) return NULL;
    }

    if (!expect(p, TK_ASSIGN, "'=' in let binding")) return NULL;

    AstNode *value = parse_expression(p);
    if (!value) return NULL;

    AstNode *node = new_node(AST_STMT_LET, kw);
    node->as.let_stmt.name = strref_from_token(name_tok);
    node->as.let_stmt.type = type_ann;
    node->as.let_stmt.value = value;
    node->span.end_line = value->span.end_line;
    node->span.end_col = value->span.end_col;
    return node;
}

static AstNode *parse_return_stmt(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'return' */

    AstNode *node = new_node(AST_STMT_RETURN, kw);
    node->as.ret_stmt.value = NULL;

    /* A bare `return` is allowed: the next token belongs to the surrounding
     * grammar (typically `}` or `;`). Anything else starts an expression. */
    if (check(p, TK_RBRACE) || check(p, TK_SEMICOLON) || check(p, TK_EOF)) {
        node_set_end(node, kw);
        return node;
    }

    AstNode *value = parse_expression(p);
    if (!value) return NULL;
    node->as.ret_stmt.value = value;
    node->span.end_line = value->span.end_line;
    node->span.end_col = value->span.end_col;
    return node;
}

static AstNode *parse_if_stmt(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'if' */

    AstNode *cond = parse_expression(p);
    if (!cond) return NULL;
    AstNode *then_b = parse_block(p);
    if (!then_b) return NULL;

    AstNode *else_b = NULL;
    if (match(p, TK_KW_ELSE)) {
        if (check(p, TK_KW_IF)) {
            else_b = parse_if_stmt(p);
        } else {
            else_b = parse_block(p);
        }
        if (!else_b) return NULL;
    }

    AstNode *node = new_node(AST_STMT_IF, kw);
    node->as.if_stmt.cond = cond;
    node->as.if_stmt.then_branch = then_b;
    node->as.if_stmt.else_branch = else_b;
    AstNode *tail = else_b ? else_b : then_b;
    node->span.end_line = tail->span.end_line;
    node->span.end_col = tail->span.end_col;
    return node;
}

static AstNode *parse_match_stmt(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'match' */

    AstNode *scrut = parse_expression(p);
    if (!scrut) return NULL;
    if (!expect(p, TK_LBRACE, "'{' to open match body")) return NULL;

    AstNode *node = new_node(AST_STMT_MATCH, kw);
    node->as.match_stmt.scrutinee = scrut;
    node->as.match_stmt.arms = NULL;
    node->as.match_stmt.n_arms = 0;
    size_t cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        Token arm_start = p->current;
        AstNode *pat = parse_expression(p);
        if (!pat) return NULL;
        if (!expect(p, TK_FAT_ARROW, "'=>' between match pattern and body")) return NULL;
        AstNode *body = parse_expression(p);
        if (!body) return NULL;
        (void)match(p, TK_COMMA);

        AstNode *arm = new_node(AST_MATCH_ARM, arm_start);
        arm->as.match_arm.pattern = pat;
        arm->as.match_arm.body = body;
        arm->span.end_line = body->span.end_line;
        arm->span.end_col = body->span.end_col;
        push_child(&node->as.match_stmt.arms, &node->as.match_stmt.n_arms,
                   &cap, arm);
    }

    Token close = p->current;
    if (!expect(p, TK_RBRACE, "'}' to close match body")) return NULL;
    node_set_end(node, close);
    return node;
}

/* ============== declarations ============== */

static AstNode *parse_param(Parser *p) {
    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected a parameter name");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    if (!expect(p, TK_COLON, "':' after parameter name")) return NULL;
    AstNode *type_node = parse_type_expr(p);
    if (!type_node) return NULL;

    AstNode *node = new_node(AST_FN_PARAM, name_tok);
    node->as.fn_param.name = strref_from_token(name_tok);
    node->as.fn_param.type = type_node;
    node->span.end_line = type_node->span.end_line;
    node->span.end_col = type_node->span.end_col;
    return node;
}

/* Parse a parenthesized parameter list. The opening '(' must already have
 * been consumed by the caller; this parses zero-or-more params and the
 * closing ')'. */
static AstNode *parse_param_list(Parser *p, AstNode ***out, size_t *n_out) {
    *out = NULL;
    *n_out = 0;
    size_t cap = 0;

    if (!check(p, TK_RPAREN)) {
        for (;;) {
            AstNode *param = parse_param(p);
            if (!param) return NULL;
            push_child(out, n_out, &cap, param);
            if (!match(p, TK_COMMA)) break;
            if (check(p, TK_RPAREN)) break; /* trailing comma */
        }
    }
    if (!expect(p, TK_RPAREN, "')' to close parameter list")) return NULL;

    /* Return a non-NULL sentinel so the caller can tell success apart from
     * an out-of-memory failure without an extra flag. The actual content
     * is delivered through *out / *n_out. */
    return (AstNode *)0x1;
}

static AstNode *parse_state_decl(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'state' */

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected a state name");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    if (!expect(p, TK_COLON, "':' after state name")) return NULL;
    AstNode *type_node = parse_type_expr(p);
    if (!type_node) return NULL;

    AstNode *node = new_node(AST_STATE_DECL, kw);
    node->as.state.name = strref_from_token(name_tok);
    node->as.state.type = type_node;
    node->span.end_line = type_node->span.end_line;
    node->span.end_col = type_node->span.end_col;
    return node;
}

static AstNode *parse_gas_decl(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'gas' */

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected a gas name");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    if (!expect(p, TK_ASSIGN, "'=' after gas name")) return NULL;
    AstNode *value = parse_expression(p);
    if (!value) return NULL;

    AstNode *node = new_node(AST_GAS_DECL, kw);
    node->as.gas.name = strref_from_token(name_tok);
    node->as.gas.value = value;
    node->span.end_line = value->span.end_line;
    node->span.end_col = value->span.end_col;
    return node;
}

static AstNode *parse_event_decl(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'event' */

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected an event name");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    if (!expect(p, TK_LPAREN, "'(' after event name")) return NULL;

    AstNode *node = new_node(AST_EVENT_DECL, kw);
    node->as.event.name = strref_from_token(name_tok);
    if (!parse_param_list(p, &node->as.event.params, &node->as.event.n_params)) {
        return NULL;
    }
    node_set_end(node, p->current);
    return node;
}

static AstNode *parse_effect_decl(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'effect' */

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected an effect name");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    if (!expect(p, TK_LPAREN, "'(' after effect name")) return NULL;

    AstNode *node = new_node(AST_EFFECT_DECL, kw);
    node->as.effect.name = strref_from_token(name_tok);
    node->as.effect.return_type = NULL;
    node->as.effect.is_deferred = 0;
    if (!parse_param_list(p, &node->as.effect.params, &node->as.effect.n_params)) {
        return NULL;
    }

    if (match(p, TK_ARROW)) {
        node->as.effect.return_type = parse_type_expr(p);
        if (!node->as.effect.return_type) return NULL;
    }
    if (match(p, TK_KW_DEFERRED)) {
        node->as.effect.is_deferred = 1;
    }
    node_set_end(node, p->current);
    return node;
}

static AstNode *parse_fn_decl(Parser *p) {
    Token start = p->current;

    FnModifier modifier = FN_MOD_NONE;
    if (check(p, TK_KW_PURE)) {
        modifier = FN_MOD_PURE;
        advance(p);
    } else if (check(p, TK_KW_VIEW)) {
        modifier = FN_MOD_VIEW;
        advance(p);
    }

    if (!expect(p, TK_KW_FN, "'fn'")) return NULL;

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected a function name");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    if (!expect(p, TK_LPAREN, "'(' after function name")) return NULL;

    AstNode *node = new_node(AST_FN_DECL, start);
    node->as.fn.modifier = modifier;
    node->as.fn.name = strref_from_token(name_tok);
    node->as.fn.return_type = NULL;
    node->as.fn.with_effects = NULL;
    node->as.fn.n_with_effects = 0;
    node->as.fn.body = NULL;

    if (!parse_param_list(p, &node->as.fn.params, &node->as.fn.n_params)) {
        return NULL;
    }

    if (match(p, TK_ARROW)) {
        node->as.fn.return_type = parse_type_expr(p);
        if (!node->as.fn.return_type) return NULL;
    }

    if (match(p, TK_KW_WITH)) {
        if (!expect(p, TK_LBRACKET, "'[' after 'with'")) return NULL;
        size_t cap = 0;
        if (!check(p, TK_RBRACKET)) {
            for (;;) {
                if (!check(p, TK_IDENT)) {
                    error_at(p, &p->current,
                             "expected an effect name in 'with [...]'");
                    return NULL;
                }
                /* Grow as needed; use the same exponential strategy as
                 * push_child but for StrRef entries. */
                if (node->as.fn.n_with_effects >= cap) {
                    cap = cap == 0 ? 4 : cap * 2;
                    StrRef *next = realloc(node->as.fn.with_effects,
                                           cap * sizeof(StrRef));
                    if (!next) {
                        fputs("fatal: out of memory in parser\n", stderr);
                        exit(1);
                    }
                    node->as.fn.with_effects = next;
                }
                node->as.fn.with_effects[node->as.fn.n_with_effects++] =
                    strref_from_token(p->current);
                advance(p);
                if (!match(p, TK_COMMA)) break;
                if (check(p, TK_RBRACKET)) break; /* trailing comma */
            }
        }
        if (!expect(p, TK_RBRACKET, "']' to close 'with [...]'")) return NULL;
    }

    node->as.fn.body = parse_block(p);
    if (!node->as.fn.body) return NULL;
    node->span.end_line = node->as.fn.body->span.end_line;
    node->span.end_col = node->as.fn.body->span.end_col;
    return node;
}

static AstNode *parse_slash_on_decl(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'slash_on' */

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected evidence name after 'slash_on'");
        return NULL;
    }
    Token evidence = p->current;
    advance(p);

    if (!expect(p, TK_LBRACE, "'{' to open slash_on bindings")) return NULL;

    AstNode *node = new_node(AST_SLASH_ON_DECL, kw);
    node->as.slash_on.evidence_name = strref_from_token(evidence);
    node->as.slash_on.bindings = NULL;
    node->as.slash_on.n_bindings = 0;
    node->as.slash_on.when_clause = NULL;
    node->as.slash_on.penalty_clause = NULL;
    size_t cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        if (!check(p, TK_IDENT)) {
            error_at(p, &p->current,
                     "expected a binding name in slash_on body");
            return NULL;
        }
        Token name_tok = p->current;
        advance(p);
        if (!expect(p, TK_COLON, "':' after slash_on binding name")) return NULL;
        AstNode *type_node = parse_type_expr(p);
        if (!type_node) return NULL;

        AstNode *binding = new_node(AST_FN_PARAM, name_tok);
        binding->as.fn_param.name = strref_from_token(name_tok);
        binding->as.fn_param.type = type_node;
        binding->span.end_line = type_node->span.end_line;
        binding->span.end_col = type_node->span.end_col;
        push_child(&node->as.slash_on.bindings, &node->as.slash_on.n_bindings,
                   &cap, binding);
        (void)match(p, TK_COMMA);
    }
    if (!expect(p, TK_RBRACE, "'}' to close slash_on bindings")) return NULL;

    if (match(p, TK_KW_WHEN)) {
        node->as.slash_on.when_clause = parse_expression(p);
        if (!node->as.slash_on.when_clause) return NULL;
    }
    if (match(p, TK_KW_PENALTY)) {
        node->as.slash_on.penalty_clause = parse_expression(p);
        if (!node->as.slash_on.penalty_clause) return NULL;
    }
    node_set_end(node, p->current);
    return node;
}

static AstNode *parse_module_item(Parser *p) {
    switch (p->current.kind) {
    case TK_KW_STATE:  return parse_state_decl(p);
    case TK_KW_GAS:    return parse_gas_decl(p);
    case TK_KW_EVENT:  return parse_event_decl(p);
    case TK_KW_EFFECT: return parse_effect_decl(p);
    case TK_KW_PURE:
    case TK_KW_VIEW:
    case TK_KW_FN:     return parse_fn_decl(p);
    default:
        error_at(p, &p->current,
                 "expected state, gas, event, fn, or effect declaration");
        return NULL;
    }
}

static AstNode *parse_module_decl(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'module' */

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected a module name");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    if (!expect(p, TK_LBRACE, "'{' to open module body")) return NULL;

    AstNode *node = new_node(AST_MODULE_DECL, kw);
    node->as.module.name = strref_from_token(name_tok);
    node->as.module.items = NULL;
    node->as.module.n_items = 0;
    size_t cap = 0;

    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        AstNode *item = parse_module_item(p);
        if (!item) return NULL;
        push_child(&node->as.module.items, &node->as.module.n_items,
                   &cap, item);
    }

    Token close = p->current;
    if (!expect(p, TK_RBRACE, "'}' to close module body")) return NULL;
    node_set_end(node, close);
    return node;
}

static AstNode *parse_protocol_item(Parser *p) {
    switch (p->current.kind) {
    case TK_KW_STATE:    return parse_state_decl(p);
    case TK_KW_EFFECT:   return parse_effect_decl(p);
    case TK_KW_PURE:
    case TK_KW_VIEW:
    case TK_KW_FN:       return parse_fn_decl(p);
    case TK_KW_SLASH_ON: return parse_slash_on_decl(p);
    default:
        error_at(p, &p->current,
                 "expected state, effect, fn, or slash_on declaration");
        return NULL;
    }
}

static AstNode *parse_protocol_decl(Parser *p) {
    Token kw = p->current;
    advance(p); /* consume 'protocol' */

    if (!check(p, TK_IDENT)) {
        error_at(p, &p->current, "expected a protocol name");
        return NULL;
    }
    Token name_tok = p->current;
    advance(p);

    AstNode *node = new_node(AST_PROTOCOL_DECL, kw);
    node->as.protocol.name = strref_from_token(name_tok);
    node->as.protocol.implements = NULL;
    node->as.protocol.items = NULL;
    node->as.protocol.n_items = 0;

    if (match(p, TK_KW_IMPLEMENTS)) {
        node->as.protocol.implements = parse_type_expr(p);
        if (!node->as.protocol.implements) return NULL;
    }

    if (!expect(p, TK_LBRACE, "'{' to open protocol body")) return NULL;
    size_t cap = 0;
    while (!check(p, TK_RBRACE) && !check(p, TK_EOF)) {
        AstNode *item = parse_protocol_item(p);
        if (!item) return NULL;
        push_child(&node->as.protocol.items, &node->as.protocol.n_items,
                   &cap, item);
    }
    Token close = p->current;
    if (!expect(p, TK_RBRACE, "'}' to close protocol body")) return NULL;
    node_set_end(node, close);
    return node;
}

static AstNode *parse_top_level(Parser *p) {
    switch (p->current.kind) {
    case TK_KW_CHAIN:    return parser_parse_chain(p);
    case TK_KW_MODULE:   return parse_module_decl(p);
    case TK_KW_PROTOCOL: return parse_protocol_decl(p);
    default:
        error_at(p, &p->current,
                 "expected a top-level declaration: chain, module, or protocol");
        return NULL;
    }
}

AstNode *parser_parse_program(Parser *p) {
    /* The program node is a synthetic module with empty name. This keeps
     * downstream passes uniform: they always walk a single root. */
    Token start = p->current;
    AstNode *root = new_node(AST_MODULE_DECL, start);
    root->as.module.name.start = NULL;
    root->as.module.name.length = 0;
    root->as.module.items = NULL;
    root->as.module.n_items = 0;
    size_t cap = 0;

    while (!check(p, TK_EOF)) {
        AstNode *decl = parse_top_level(p);
        if (!decl) return NULL;
        push_child(&root->as.module.items, &root->as.module.n_items, &cap, decl);
    }
    node_set_end(root, p->current);
    return root;
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
