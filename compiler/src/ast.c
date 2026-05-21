/*
 * AST node implementation.
 *
 * Memory ownership
 * ----------------
 * Nodes are individually heap-allocated by the parser via malloc(). The
 * compiler runs as a single-shot process (lex -> parse -> typecheck ->
 * codegen -> exit), and the OS reclaims all process memory on exit, so
 * we never explicitly free individual nodes.
 *
 * This is a conscious v0 choice. An arena allocator (or refcounting)
 * can replace this when profiling shows malloc churn matters, which is
 * unlikely for a compiler that runs for milliseconds. The first thing
 * to check before changing this is `time make test` on the full suite:
 * if it stops being instant, revisit.
 */

#include <stdio.h>
#include <stdlib.h>

#include "ast.h"

/* ============== kind names ============== */

const char *ast_kind_name(AstKind kind) {
    switch (kind) {
    case AST_CHAIN_DECL:       return "ChainDecl";
    case AST_MODULE_DECL:      return "ModuleDecl";
    case AST_PROTOCOL_DECL:    return "ProtocolDecl";
    case AST_SUBSYSTEM_ASSIGN: return "SubsystemAssign";
    case AST_STATE_DECL:       return "StateDecl";
    case AST_GAS_DECL:         return "GasDecl";
    case AST_FN_DECL:          return "FnDecl";
    case AST_FN_PARAM:         return "FnParam";
    case AST_EFFECT_DECL:      return "EffectDecl";
    case AST_SLASH_ON_DECL:    return "SlashOnDecl";
    case AST_TYPE_NAMED:       return "TypeNamed";
    case AST_TYPE_GENERIC:     return "TypeGeneric";
    case AST_TYPE_PARAM:       return "TypeParam";
    case AST_EXPR_IDENT:       return "ExprIdent";
    case AST_EXPR_NUMBER:      return "ExprNumber";
    case AST_EXPR_STRING:      return "ExprString";
    case AST_EXPR_CHAR:        return "ExprChar";
    case AST_EXPR_BOOL:        return "ExprBool";
    case AST_EXPR_NULL:        return "ExprNull";
    case AST_EXPR_BINARY:      return "ExprBinary";
    case AST_EXPR_UNARY:       return "ExprUnary";
    case AST_EXPR_CALL:        return "ExprCall";
    case AST_EXPR_INDEX:       return "ExprIndex";
    case AST_EXPR_BLOCK:       return "ExprBlock";
    case AST_STMT_LET:         return "StmtLet";
    case AST_STMT_RETURN:      return "StmtReturn";
    case AST_STMT_IF:          return "StmtIf";
    case AST_STMT_EXPR:        return "StmtExpr";
    }
    return "Unknown";
}

/* ============== dump helpers ============== */

static void put_indent(FILE *out, int indent) {
    for (int i = 0; i < indent; ++i) fputc(' ', out);
}

static void put_strref(FILE *out, StrRef s) {
    fprintf(out, "\"%.*s\"", (int)s.length, s.start);
}

static void put_field_strref(FILE *out, int indent, const char *name, StrRef value) {
    put_indent(out, indent);
    fprintf(out, "%s: ", name);
    put_strref(out, value);
    fputc('\n', out);
}

static void put_field_child(FILE *out, int indent, const char *name, const AstNode *child) {
    put_indent(out, indent);
    fprintf(out, "%s:\n", name);
    ast_dump(child, out, indent + 2);
}

static void put_field_children(FILE *out, int indent, const char *name,
                               AstNode *const *children, size_t n) {
    put_indent(out, indent);
    fprintf(out, "%s: [%zu]\n", name, n);
    for (size_t i = 0; i < n; ++i) {
        ast_dump(children[i], out, indent + 2);
    }
}

/* ============== dump ============== */

void ast_dump(const AstNode *node, FILE *out, int indent) {
    if (!node) {
        put_indent(out, indent);
        fputs("<null>\n", out);
        return;
    }

    put_indent(out, indent);
    fprintf(out, "%s @ %zu:%zu\n",
            ast_kind_name(node->kind),
            node->span.start_line, node->span.start_col);

    int sub = indent + 2;

    switch (node->kind) {
    case AST_CHAIN_DECL: {
        const ChainDecl *c = &node->as.chain;
        put_field_strref(out, sub, "name", c->name);
        put_field_children(out, sub, "assignments", c->assignments, c->n_assignments);
        return;
    }
    case AST_MODULE_DECL: {
        const ModuleDecl *m = &node->as.module;
        put_field_strref(out, sub, "name", m->name);
        put_field_children(out, sub, "items", m->items, m->n_items);
        return;
    }
    case AST_PROTOCOL_DECL: {
        const ProtocolDecl *p = &node->as.protocol;
        put_field_strref(out, sub, "name", p->name);
        if (p->implements) put_field_child(out, sub, "implements", p->implements);
        put_field_children(out, sub, "items", p->items, p->n_items);
        return;
    }
    case AST_SUBSYSTEM_ASSIGN: {
        const SubsystemAssign *s = &node->as.subsystem;
        put_field_strref(out, sub, "key", s->key);
        put_field_child(out, sub, "value", s->value);
        return;
    }
    case AST_STATE_DECL: {
        const StateDecl *s = &node->as.state;
        put_field_strref(out, sub, "name", s->name);
        put_field_child(out, sub, "type", s->type);
        return;
    }
    case AST_GAS_DECL: {
        const GasDecl *g = &node->as.gas;
        put_field_strref(out, sub, "name", g->name);
        put_field_child(out, sub, "value", g->value);
        return;
    }
    case AST_FN_DECL: {
        const FnDecl *f = &node->as.fn;
        put_field_strref(out, sub, "name", f->name);
        put_field_children(out, sub, "params", f->params, f->n_params);
        if (f->return_type) put_field_child(out, sub, "return_type", f->return_type);
        put_field_child(out, sub, "body", f->body);
        return;
    }
    case AST_FN_PARAM: {
        const FnParam *p = &node->as.fn_param;
        put_field_strref(out, sub, "name", p->name);
        put_field_child(out, sub, "type", p->type);
        return;
    }
    case AST_EFFECT_DECL: {
        const EffectDecl *e = &node->as.effect;
        put_field_strref(out, sub, "name", e->name);
        put_field_children(out, sub, "params", e->params, e->n_params);
        if (e->return_type) put_field_child(out, sub, "return_type", e->return_type);
        return;
    }
    case AST_SLASH_ON_DECL: {
        const SlashOnDecl *s = &node->as.slash_on;
        put_field_strref(out, sub, "evidence_name", s->evidence_name);
        put_field_children(out, sub, "bindings", s->bindings, s->n_bindings);
        if (s->when_clause) put_field_child(out, sub, "when", s->when_clause);
        if (s->penalty_clause) put_field_child(out, sub, "penalty", s->penalty_clause);
        return;
    }
    case AST_TYPE_NAMED: {
        put_field_strref(out, sub, "name", node->as.type_named.name);
        return;
    }
    case AST_TYPE_GENERIC: {
        const TypeGeneric *g = &node->as.type_generic;
        put_field_strref(out, sub, "name", g->name);
        put_field_children(out, sub, "args", g->args, g->n_args);
        return;
    }
    case AST_TYPE_PARAM: {
        const TypeParam *p = &node->as.type_param;
        if (p->key.length > 0) put_field_strref(out, sub, "key", p->key);
        put_field_child(out, sub, "value", p->value);
        return;
    }
    case AST_EXPR_IDENT: {
        put_field_strref(out, sub, "name", node->as.ident.name);
        return;
    }
    case AST_EXPR_NUMBER: {
        put_field_strref(out, sub, "text", node->as.number.text);
        return;
    }
    case AST_EXPR_STRING: {
        put_field_strref(out, sub, "text", node->as.string.text);
        return;
    }
    case AST_EXPR_CHAR: {
        put_field_strref(out, sub, "text", node->as.character.text);
        return;
    }
    case AST_EXPR_BOOL: {
        put_indent(out, sub);
        fprintf(out, "value: %s\n", node->as.boolean.value ? "true" : "false");
        return;
    }
    case AST_EXPR_NULL: {
        return;
    }
    case AST_EXPR_BINARY: {
        const ExprBinary *b = &node->as.binary;
        put_field_strref(out, sub, "op", b->op);
        put_field_child(out, sub, "lhs", b->lhs);
        put_field_child(out, sub, "rhs", b->rhs);
        return;
    }
    case AST_EXPR_UNARY: {
        const ExprUnary *u = &node->as.unary;
        put_field_strref(out, sub, "op", u->op);
        put_field_child(out, sub, "operand", u->operand);
        return;
    }
    case AST_EXPR_CALL: {
        const ExprCall *c = &node->as.call;
        put_field_child(out, sub, "callee", c->callee);
        put_field_children(out, sub, "args", c->args, c->n_args);
        return;
    }
    case AST_EXPR_INDEX: {
        const ExprIndex *i = &node->as.index;
        put_field_child(out, sub, "array", i->array);
        put_field_child(out, sub, "index", i->index);
        return;
    }
    case AST_EXPR_BLOCK: {
        const ExprBlock *b = &node->as.block;
        put_field_children(out, sub, "stmts", b->stmts, b->n_stmts);
        if (b->result) put_field_child(out, sub, "result", b->result);
        return;
    }
    case AST_STMT_LET: {
        const StmtLet *s = &node->as.let_stmt;
        put_field_strref(out, sub, "name", s->name);
        if (s->type) put_field_child(out, sub, "type", s->type);
        put_field_child(out, sub, "value", s->value);
        return;
    }
    case AST_STMT_RETURN: {
        const StmtReturn *r = &node->as.ret_stmt;
        if (r->value) put_field_child(out, sub, "value", r->value);
        return;
    }
    case AST_STMT_IF: {
        const StmtIf *i = &node->as.if_stmt;
        put_field_child(out, sub, "cond", i->cond);
        put_field_child(out, sub, "then", i->then_branch);
        if (i->else_branch) put_field_child(out, sub, "else", i->else_branch);
        return;
    }
    case AST_STMT_EXPR: {
        put_field_child(out, sub, "expr", node->as.expr_stmt.expr);
        return;
    }
    }
}
