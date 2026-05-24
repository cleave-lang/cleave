/*
 * Cleave type checker (issue #34).
 *
 * Implementation choices
 * ----------------------
 *
 * Types are heap-allocated like AST nodes (see compiler/src/ast.c for the
 * "malloc and leak" rationale). Primitives, unit, and unknown are
 * interned: there is a single canonical Type* per primitive kind, and
 * equality between two primitive types is pointer equality.
 *
 * Generic types like Result<u64> are NOT interned today; a fresh Type is
 * built per occurrence. That keeps the lookup logic small and is fine
 * for the v0 compile-time budget. Codegen can hash-cons later if it
 * matters.
 *
 * Scopes are a flat stack of (name, type, depth) triples. Module-level
 * declarations are seeded at depth 0; entering a block bumps the depth
 * and leaving pops everything above that depth. Linear scan for lookup
 * is fine for v0 source files.
 *
 * Error policy mirrors the parser: emit a diagnostic with a 1-based
 * line:column and the offending lexeme, then carry on so the next
 * mistake is also visible. The checker returns a non-zero exit when
 * any error was reported.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "typecheck.h"

/* ============== interned singletons ============== */

#define N_PRIMS 13

static Type *PRIM_CACHE[N_PRIMS];
static Type *UNIT_CACHE;
static Type *UNKNOWN_CACHE;

static Type *alloc_type(TypeKind k) {
    Type *t = calloc(1, sizeof(Type));
    if (!t) { fputs("fatal: out of memory in typecheck\n", stderr); exit(1); }
    t->kind = k;
    return t;
}

const Type *type_prim(PrimKind k) {
    if (PRIM_CACHE[k]) return PRIM_CACHE[k];
    Type *t = alloc_type(TY_PRIM);
    t->as.prim = k;
    PRIM_CACHE[k] = t;
    return t;
}

const Type *type_unit(void) {
    if (UNIT_CACHE) return UNIT_CACHE;
    UNIT_CACHE = alloc_type(TY_UNIT);
    return UNIT_CACHE;
}

const Type *type_unknown(void) {
    if (UNKNOWN_CACHE) return UNKNOWN_CACHE;
    UNKNOWN_CACHE = alloc_type(TY_UNKNOWN);
    return UNKNOWN_CACHE;
}

/* ============== primitive name lookup ============== */

static const struct { const char *name; PrimKind kind; } PRIM_NAMES[] = {
    { "u8",   PRIM_U8   },
    { "u16",  PRIM_U16  },
    { "u32",  PRIM_U32  },
    { "u64",  PRIM_U64  },
    { "u128", PRIM_U128 },
    { "u256", PRIM_U256 },
    { "i8",   PRIM_I8   },
    { "i16",  PRIM_I16  },
    { "i32",  PRIM_I32  },
    { "i64",  PRIM_I64  },
    { "bool", PRIM_BOOL },
    { "str",  PRIM_STR  },
    { "char", PRIM_CHAR },
};
static const size_t N_PRIM_NAMES = sizeof(PRIM_NAMES) / sizeof(PRIM_NAMES[0]);

/* If the given identifier names a primitive, return its canonical Type*;
 * otherwise return NULL. */
static const Type *prim_from_name(const char *name, size_t length) {
    for (size_t i = 0; i < N_PRIM_NAMES; ++i) {
        if (strlen(PRIM_NAMES[i].name) == length &&
            memcmp(PRIM_NAMES[i].name, name, length) == 0) {
            return type_prim(PRIM_NAMES[i].kind);
        }
    }
    return NULL;
}

/* ============== type-expression resolution ============== */

static const Type *resolve_type_expr(TypeChecker *tc, AstNode *node);

static const Type *resolve_named(AstNode *node) {
    StrRef n = node->as.type_named.name;
    const Type *prim = prim_from_name(n.start, n.length);
    if (prim) return prim;
    /* Unknown user-defined type: model as an opaque generic with zero args.
     * Codegen needs concrete types eventually, but for now treat it as a
     * named opaque so equality at least is by name. */
    Type *t = alloc_type(TY_GENERIC);
    t->as.generic.head = n.start;
    t->as.generic.head_length = n.length;
    t->as.generic.args = NULL;
    t->as.generic.n_args = 0;
    return t;
}

static const Type *resolve_generic(TypeChecker *tc, AstNode *node) {
    const TypeGeneric *g = &node->as.type_generic;
    Type *t = alloc_type(TY_GENERIC);
    t->as.generic.head = g->name.start;
    t->as.generic.head_length = g->name.length;
    t->as.generic.args = NULL;
    t->as.generic.n_args = 0;
    if (g->n_args == 0) return t;

    t->as.generic.args = calloc(g->n_args, sizeof(Type *));
    if (!t->as.generic.args) {
        fputs("fatal: out of memory in typecheck\n", stderr);
        exit(1);
    }
    t->as.generic.n_args = g->n_args;

    for (size_t i = 0; i < g->n_args; ++i) {
        AstNode *param = g->args[i];
        /* A TypeParam wraps either a type-like value or a literal. We try
         * to resolve only the type-like cases; literals stay opaque. */
        AstNode *value = param->as.type_param.value;
        const Type *resolved = NULL;
        if (value && value->kind == AST_EXPR_IDENT) {
            const Type *p = prim_from_name(value->as.ident.name.start,
                                           value->as.ident.name.length);
            resolved = p ? p : type_unknown();
        } else {
            resolved = type_unknown();
        }
        t->as.generic.args[i] = (Type *)resolved;
    }
    (void)tc;
    return t;
}

static const Type *resolve_type_expr(TypeChecker *tc, AstNode *node) {
    if (!node) return type_unknown();
    switch (node->kind) {
    case AST_TYPE_NAMED:   return resolve_named(node);
    case AST_TYPE_GENERIC: return resolve_generic(tc, node);
    default:               return type_unknown();
    }
}

/* ============== type compatibility + description ============== */

static int type_equal(const Type *a, const Type *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind == TY_UNKNOWN || b->kind == TY_UNKNOWN) return 1;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case TY_PRIM:
        return a->as.prim == b->as.prim;
    case TY_UNIT:
        return 1;
    case TY_GENERIC:
        if (a->as.generic.head_length != b->as.generic.head_length) return 0;
        if (memcmp(a->as.generic.head, b->as.generic.head,
                   a->as.generic.head_length) != 0) return 0;
        if (a->as.generic.n_args != b->as.generic.n_args) return 0;
        for (size_t i = 0; i < a->as.generic.n_args; ++i) {
            if (!type_equal(a->as.generic.args[i], b->as.generic.args[i])) {
                return 0;
            }
        }
        return 1;
    case TY_FN:
        if (a->as.fn.n_params != b->as.fn.n_params) return 0;
        for (size_t i = 0; i < a->as.fn.n_params; ++i) {
            if (!type_equal(a->as.fn.params[i], b->as.fn.params[i])) return 0;
        }
        return type_equal(a->as.fn.result, b->as.fn.result);
    case TY_UNKNOWN:
        return 1;
    }
    return 0;
}

/* Render a type into the provided output buffer, returning the number of
 * characters written (excluding the trailing nul). Recursive; safe because
 * each call gets its own buffer slice. */
static size_t render_type(const Type *t, char *out, size_t cap) {
    if (cap == 0) return 0;
    if (!t) {
        int n = snprintf(out, cap, "<null>");
        return (n > 0) ? (size_t)n : 0;
    }
    switch (t->kind) {
    case TY_UNKNOWN: return (size_t)snprintf(out, cap, "<unknown>");
    case TY_UNIT:    return (size_t)snprintf(out, cap, "()");
    case TY_PRIM: {
        const char *name = "<prim?>";
        switch (t->as.prim) {
        case PRIM_U8:   name = "u8";   break;
        case PRIM_U16:  name = "u16";  break;
        case PRIM_U32:  name = "u32";  break;
        case PRIM_U64:  name = "u64";  break;
        case PRIM_U128: name = "u128"; break;
        case PRIM_U256: name = "u256"; break;
        case PRIM_I8:   name = "i8";   break;
        case PRIM_I16:  name = "i16";  break;
        case PRIM_I32:  name = "i32";  break;
        case PRIM_I64:  name = "i64";  break;
        case PRIM_BOOL: name = "bool"; break;
        case PRIM_STR:  name = "str";  break;
        case PRIM_CHAR: name = "char"; break;
        }
        return (size_t)snprintf(out, cap, "%s", name);
    }
    case TY_GENERIC: {
        const GenericType *g = &t->as.generic;
        size_t pos = 0;
        int n = snprintf(out + pos, cap - pos, "%.*s",
                         (int)g->head_length, g->head);
        if (n > 0) pos += (size_t)n;
        if (g->n_args > 0 && pos + 1 < cap) {
            if (pos + 1 < cap) out[pos++] = '<';
            for (size_t i = 0; i < g->n_args && pos < cap; ++i) {
                if (i > 0 && pos + 2 < cap) {
                    out[pos++] = ',';
                    out[pos++] = ' ';
                }
                pos += render_type(g->args[i], out + pos, cap - pos);
            }
            if (pos < cap - 1) out[pos++] = '>';
            if (pos < cap) out[pos] = '\0';
        }
        return pos;
    }
    case TY_FN: {
        size_t pos = 0;
        int n = snprintf(out + pos, cap - pos, "fn(");
        if (n > 0) pos += (size_t)n;
        for (size_t i = 0; i < t->as.fn.n_params && pos < cap; ++i) {
            if (i > 0 && pos + 2 < cap) {
                out[pos++] = ',';
                out[pos++] = ' ';
            }
            pos += render_type(t->as.fn.params[i], out + pos, cap - pos);
        }
        n = snprintf(out + pos, cap - pos, ") -> ");
        if (n > 0) pos += (size_t)n;
        pos += render_type(t->as.fn.result, out + pos, cap - pos);
        return pos;
    }
    }
    return (size_t)snprintf(out, cap, "<type?>");
}

const char *type_describe(const Type *t) {
    static char buf[256];
    buf[0] = '\0';
    render_type(t, buf, sizeof(buf));
    return buf;
}

/* ============== diagnostics ============== */

static void report_at(TypeChecker *tc, const Span *span, const char *fmt, ...) {
    tc->n_errors++;
    FILE *out = tc->err ? tc->err : stderr;
    va_list ap;
    fprintf(out, "%zu:%zu: type error: ",
            span->start_line, span->start_col);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

/* Convenience: report a type-mismatch diagnostic. */
static void report_mismatch(TypeChecker *tc, const Span *span,
                            const Type *expected, const Type *got,
                            const char *context) {
    char e_buf[128], g_buf[128];
    /* type_describe shares a static buffer; copy out before second call. */
    strncpy(e_buf, type_describe(expected), sizeof(e_buf) - 1);
    e_buf[sizeof(e_buf) - 1] = '\0';
    strncpy(g_buf, type_describe(got), sizeof(g_buf) - 1);
    g_buf[sizeof(g_buf) - 1] = '\0';
    report_at(tc, span, "%s: expected %s, got %s", context, e_buf, g_buf);
}

/* ============== scope ============== */

typedef struct {
    StrRef     name;
    const Type *type;
    int        depth;
} ScopeEntry;

typedef struct {
    ScopeEntry *entries;
    size_t      n;
    size_t      cap;
    int         depth;
} Scope;

static void scope_init(Scope *s) {
    s->entries = NULL;
    s->n = 0;
    s->cap = 0;
    s->depth = 0;
}

static void scope_enter(Scope *s) { s->depth++; }

static void scope_leave(Scope *s) {
    while (s->n > 0 && s->entries[s->n - 1].depth >= s->depth) {
        s->n--;
    }
    s->depth--;
}

static void scope_bind(Scope *s, StrRef name, const Type *type) {
    if (s->n >= s->cap) {
        s->cap = s->cap == 0 ? 8 : s->cap * 2;
        ScopeEntry *next = realloc(s->entries, s->cap * sizeof(ScopeEntry));
        if (!next) { fputs("fatal: out of memory in typecheck\n", stderr); exit(1); }
        s->entries = next;
    }
    s->entries[s->n].name = name;
    s->entries[s->n].type = type;
    s->entries[s->n].depth = s->depth;
    s->n++;
}

static const Type *scope_lookup(const Scope *s, StrRef name) {
    for (size_t i = s->n; i-- > 0; ) {
        const ScopeEntry *e = &s->entries[i];
        if (e->name.length == name.length &&
            memcmp(e->name.start, name.start, name.length) == 0) {
            return e->type;
        }
    }
    return NULL;
}

/* ============== expression checker ============== */

static const Type *check_expr(TypeChecker *tc, Scope *scope, AstNode *node);

static const Type *check_binary(TypeChecker *tc, Scope *scope, AstNode *node) {
    ExprBinary *b = &node->as.binary;
    StrRef op = b->op;

    /* Member access `.` and path access `::` are name lookups, not value-level
     * binary ops; the result type is unknown at this layer (no field metadata
     * yet). Just walk the LHS and return Unknown. */
    if (op.length == 1 && op.start[0] == '.') {
        check_expr(tc, scope, b->lhs);
        return type_unknown();
    }
    if (op.length == 2 && op.start[0] == ':' && op.start[1] == ':') {
        return type_unknown();
    }

    const Type *lhs_t = check_expr(tc, scope, b->lhs);
    const Type *rhs_t = check_expr(tc, scope, b->rhs);

    /* Assignment: rhs must be compatible with lhs type; result is lhs type. */
    if (op.length == 1 && op.start[0] == '=') {
        if (!type_equal(lhs_t, rhs_t)) {
            report_mismatch(tc, &node->span, lhs_t, rhs_t,
                            "assignment");
        }
        return lhs_t;
    }

    /* Comparisons and equality produce bool; operands must be compatible. */
    if ((op.length == 2 && (memcmp(op.start, "==", 2) == 0 ||
                            memcmp(op.start, "!=", 2) == 0 ||
                            memcmp(op.start, "<=", 2) == 0 ||
                            memcmp(op.start, ">=", 2) == 0)) ||
        (op.length == 1 && (op.start[0] == '<' || op.start[0] == '>'))) {
        if (!type_equal(lhs_t, rhs_t)) {
            report_mismatch(tc, &node->span, lhs_t, rhs_t,
                            "comparison operands");
        }
        return type_prim(PRIM_BOOL);
    }

    /* Logical: both sides must be bool; result is bool. */
    if (op.length == 2 && (memcmp(op.start, "&&", 2) == 0 ||
                           memcmp(op.start, "||", 2) == 0)) {
        const Type *boolt = type_prim(PRIM_BOOL);
        if (!type_equal(lhs_t, boolt)) {
            report_mismatch(tc, &node->span, boolt, lhs_t,
                            "logical operator lhs");
        }
        if (!type_equal(rhs_t, boolt)) {
            report_mismatch(tc, &node->span, boolt, rhs_t,
                            "logical operator rhs");
        }
        return boolt;
    }

    /* Arithmetic: both sides must match; result is that type. */
    if (op.length == 1 && (op.start[0] == '+' || op.start[0] == '-' ||
                           op.start[0] == '*' || op.start[0] == '/')) {
        if (!type_equal(lhs_t, rhs_t)) {
            report_mismatch(tc, &node->span, lhs_t, rhs_t,
                            "arithmetic operands");
        }
        return lhs_t;
    }

    return type_unknown();
}

static const Type *check_call(TypeChecker *tc, Scope *scope, AstNode *node) {
    ExprCall *c = &node->as.call;
    const Type *callee_t = check_expr(tc, scope, c->callee);
    for (size_t i = 0; i < c->n_args; ++i) {
        check_expr(tc, scope, c->args[i]);
    }
    if (callee_t && callee_t->kind == TY_FN) {
        if (c->n_args != callee_t->as.fn.n_params) {
            report_at(tc, &node->span,
                      "call has %zu arg(s), expected %zu",
                      c->n_args, callee_t->as.fn.n_params);
        } else {
            for (size_t i = 0; i < c->n_args; ++i) {
                const Type *arg_t = c->args[i]->resolved_type;
                if (!type_equal(arg_t, callee_t->as.fn.params[i])) {
                    report_mismatch(tc, &c->args[i]->span,
                                    callee_t->as.fn.params[i], arg_t,
                                    "call argument");
                }
            }
        }
        return callee_t->as.fn.result;
    }
    return type_unknown();
}

static const Type *check_block(TypeChecker *tc, Scope *scope, AstNode *node);

static const Type *check_expr(TypeChecker *tc, Scope *scope, AstNode *node) {
    if (!node) return type_unknown();
    const Type *t = type_unknown();
    switch (node->kind) {
    case AST_EXPR_NUMBER:
        /* Default integer literal type; richer inference is a later issue. */
        t = type_prim(PRIM_U64);
        break;
    case AST_EXPR_STRING:
        t = type_prim(PRIM_STR);
        break;
    case AST_EXPR_CHAR:
        t = type_prim(PRIM_CHAR);
        break;
    case AST_EXPR_BOOL:
        t = type_prim(PRIM_BOOL);
        break;
    case AST_EXPR_NULL:
        t = type_unknown();
        break;
    case AST_EXPR_IDENT: {
        const Type *bound = scope_lookup(scope, node->as.ident.name);
        t = bound ? bound : type_unknown();
        break;
    }
    case AST_EXPR_BINARY:
        t = check_binary(tc, scope, node);
        break;
    case AST_EXPR_UNARY: {
        const Type *operand_t = check_expr(tc, scope, node->as.unary.operand);
        StrRef op = node->as.unary.op;
        if (op.length == 1 && op.start[0] == '!') {
            const Type *boolt = type_prim(PRIM_BOOL);
            if (!type_equal(operand_t, boolt)) {
                report_mismatch(tc, &node->span, boolt, operand_t,
                                "logical negation operand");
            }
            t = boolt;
        } else {
            /* `-x` and `x%` keep the operand type. */
            t = operand_t;
        }
        break;
    }
    case AST_EXPR_CALL:
        t = check_call(tc, scope, node);
        break;
    case AST_EXPR_INDEX:
        check_expr(tc, scope, node->as.index.array);
        check_expr(tc, scope, node->as.index.index);
        t = type_unknown();
        break;
    case AST_EXPR_BLOCK:
        t = check_block(tc, scope, node);
        break;
    case AST_EXPR_RECORD: {
        ExprRecord *r = &node->as.record;
        for (size_t i = 0; i < r->n_fields; ++i) {
            AstNode *f = r->fields[i];
            check_expr(tc, scope, f->as.record_field.value);
        }
        /* Records are opaque until we have struct types. */
        t = type_unknown();
        break;
    }
    default:
        /* Statements bubbled up as expressions: walk children for side effects. */
        t = type_unknown();
        break;
    }
    node->resolved_type = t;
    return t;
}

/* ============== statement + block ============== */

static void check_let(TypeChecker *tc, Scope *scope, AstNode *node) {
    StmtLet *l = &node->as.let_stmt;
    const Type *value_t = check_expr(tc, scope, l->value);

    const Type *declared_t = NULL;
    if (l->type) {
        declared_t = resolve_type_expr(tc, l->type);
        if (!type_equal(declared_t, value_t)) {
            report_mismatch(tc, &node->span, declared_t, value_t,
                            "let binding initializer");
        }
        scope_bind(scope, l->name, declared_t);
    } else {
        scope_bind(scope, l->name, value_t);
    }
}

static void check_return(TypeChecker *tc, Scope *scope, AstNode *node,
                         const Type *expected_ret) {
    StmtReturn *r = &node->as.ret_stmt;
    if (!r->value) {
        if (expected_ret && expected_ret->kind != TY_UNIT &&
            expected_ret->kind != TY_UNKNOWN) {
            report_mismatch(tc, &node->span, expected_ret, type_unit(),
                            "bare return");
        }
        return;
    }
    const Type *value_t = check_expr(tc, scope, r->value);
    if (expected_ret && !type_equal(expected_ret, value_t)) {
        report_mismatch(tc, &node->span, expected_ret, value_t,
                        "return value");
    }
}

static void check_if(TypeChecker *tc, Scope *scope, AstNode *node,
                     const Type *expected_ret);

static void check_match(TypeChecker *tc, Scope *scope, AstNode *node,
                        const Type *expected_ret) {
    StmtMatch *m = &node->as.match_stmt;
    check_expr(tc, scope, m->scrutinee);
    for (size_t i = 0; i < m->n_arms; ++i) {
        AstNode *arm = m->arms[i];
        check_expr(tc, scope, arm->as.match_arm.pattern);
        check_expr(tc, scope, arm->as.match_arm.body);
    }
    (void)expected_ret;
}

static void check_statement(TypeChecker *tc, Scope *scope, AstNode *node,
                            const Type *expected_ret) {
    switch (node->kind) {
    case AST_STMT_LET:
        check_let(tc, scope, node);
        break;
    case AST_STMT_RETURN:
        check_return(tc, scope, node, expected_ret);
        break;
    case AST_STMT_IF:
        check_if(tc, scope, node, expected_ret);
        break;
    case AST_STMT_MATCH:
        check_match(tc, scope, node, expected_ret);
        break;
    case AST_STMT_EXPR:
        check_expr(tc, scope, node->as.expr_stmt.expr);
        break;
    default:
        /* declaration in statement position; ignore for now */
        break;
    }
}

static const Type *check_block(TypeChecker *tc, Scope *scope, AstNode *node) {
    scope_enter(scope);
    ExprBlock *b = &node->as.block;
    for (size_t i = 0; i < b->n_stmts; ++i) {
        /* expected_ret = NULL here; the block isn't a function body. */
        check_statement(tc, scope, b->stmts[i], NULL);
    }
    const Type *result_t = type_unit();
    if (b->result) {
        result_t = check_expr(tc, scope, b->result);
    }
    scope_leave(scope);
    return result_t;
}

static void check_if(TypeChecker *tc, Scope *scope, AstNode *node,
                     const Type *expected_ret) {
    StmtIf *i = &node->as.if_stmt;
    const Type *cond_t = check_expr(tc, scope, i->cond);
    const Type *boolt = type_prim(PRIM_BOOL);
    if (!type_equal(cond_t, boolt)) {
        report_mismatch(tc, &i->cond->span, boolt, cond_t,
                        "if condition");
    }
    check_block(tc, scope, i->then_branch);
    if (i->else_branch) {
        if (i->else_branch->kind == AST_EXPR_BLOCK) {
            check_block(tc, scope, i->else_branch);
        } else if (i->else_branch->kind == AST_STMT_IF) {
            check_if(tc, scope, i->else_branch, expected_ret);
        }
    }
}

/* ============== function-body checker ============== */

static void check_fn(TypeChecker *tc, Scope *scope, AstNode *node) {
    FnDecl *f = &node->as.fn;

    const Type *ret_t = type_unit();
    if (f->return_type) {
        ret_t = resolve_type_expr(tc, f->return_type);
    }

    /* Build the function's own type so callers can resolve it. */
    Type *fn_type = alloc_type(TY_FN);
    fn_type->as.fn.n_params = f->n_params;
    fn_type->as.fn.params = NULL;
    if (f->n_params > 0) {
        fn_type->as.fn.params = calloc(f->n_params, sizeof(Type *));
        if (!fn_type->as.fn.params) {
            fputs("fatal: out of memory in typecheck\n", stderr); exit(1);
        }
    }
    for (size_t i = 0; i < f->n_params; ++i) {
        const Type *pt = resolve_type_expr(tc, f->params[i]->as.fn_param.type);
        fn_type->as.fn.params[i] = (Type *)pt;
    }
    fn_type->as.fn.result = (Type *)ret_t;

    scope_bind(scope, f->name, fn_type);

    /* Enter a body scope, bind params, walk the body, then leave. */
    scope_enter(scope);
    for (size_t i = 0; i < f->n_params; ++i) {
        scope_bind(scope, f->params[i]->as.fn_param.name,
                   fn_type->as.fn.params[i]);
    }

    if (f->body && f->body->kind == AST_EXPR_BLOCK) {
        ExprBlock *bb = &f->body->as.block;
        scope_enter(scope);
        for (size_t i = 0; i < bb->n_stmts; ++i) {
            check_statement(tc, scope, bb->stmts[i], ret_t);
        }
        if (bb->result) {
            const Type *trailing_t = check_expr(tc, scope, bb->result);
            if (!type_equal(ret_t, trailing_t)) {
                report_mismatch(tc, &bb->result->span, ret_t, trailing_t,
                                "function return");
            }
        } else if (ret_t->kind != TY_UNIT && ret_t->kind != TY_UNKNOWN) {
            /* Body has no trailing expr, but a value is expected. */
            int has_return = 0;
            for (size_t i = 0; i < bb->n_stmts; ++i) {
                if (bb->stmts[i]->kind == AST_STMT_RETURN) {
                    has_return = 1;
                    break;
                }
            }
            if (!has_return) {
                report_at(tc, &node->span,
                          "function '%.*s' must return %s but body is empty or "
                          "ends without a value",
                          (int)f->name.length, f->name.start,
                          type_describe(ret_t));
            }
        }
        scope_leave(scope);
    }
    scope_leave(scope);
}

/* ============== module + program ============== */

static void check_module_items_pass1(TypeChecker *tc, Scope *scope,
                                     AstNode *module) {
    /* First pass: seed names so fn bodies can refer to siblings declared
     * later in the same module. */
    ModuleDecl *m = &module->as.module;
    for (size_t i = 0; i < m->n_items; ++i) {
        AstNode *item = m->items[i];
        switch (item->kind) {
        case AST_STATE_DECL: {
            const Type *t = resolve_type_expr(tc, item->as.state.type);
            scope_bind(scope, item->as.state.name, t);
            break;
        }
        case AST_GAS_DECL:
            /* Gas declarations bind a name to a (record) expression value.
             * Treat as opaque for now; codegen layer cares about the value. */
            scope_bind(scope, item->as.gas.name, type_unknown());
            break;
        case AST_EVENT_DECL:
            scope_bind(scope, item->as.event.name, type_unknown());
            break;
        case AST_EFFECT_DECL:
            scope_bind(scope, item->as.effect.name, type_unknown());
            break;
        case AST_FN_DECL: {
            FnDecl *f = &item->as.fn;
            Type *fn_type = alloc_type(TY_FN);
            fn_type->as.fn.n_params = f->n_params;
            fn_type->as.fn.params = NULL;
            if (f->n_params > 0) {
                fn_type->as.fn.params = calloc(f->n_params, sizeof(Type *));
                if (!fn_type->as.fn.params) {
                    fputs("fatal: out of memory in typecheck\n", stderr);
                    exit(1);
                }
            }
            for (size_t j = 0; j < f->n_params; ++j) {
                const Type *pt = resolve_type_expr(tc,
                    f->params[j]->as.fn_param.type);
                fn_type->as.fn.params[j] = (Type *)pt;
            }
            fn_type->as.fn.result = (Type *)(f->return_type
                ? resolve_type_expr(tc, f->return_type)
                : type_unit());
            scope_bind(scope, f->name, fn_type);
            break;
        }
        default:
            break;
        }
    }
}

static void check_module(TypeChecker *tc, Scope *scope, AstNode *module) {
    check_module_items_pass1(tc, scope, module);
    ModuleDecl *m = &module->as.module;
    for (size_t i = 0; i < m->n_items; ++i) {
        AstNode *item = m->items[i];
        if (item->kind == AST_FN_DECL) {
            /* Note: check_fn rebinds the fn name; harmless and lets the
             * inner pass build a richer function type if it diverges. */
            check_fn(tc, scope, item);
        }
        if (item->kind == AST_GAS_DECL) {
            check_expr(tc, scope, item->as.gas.value);
        }
    }
}

void typecheck_init(TypeChecker *tc, FILE *err) {
    tc->err = err;
    tc->n_errors = 0;
}

int typecheck_program(TypeChecker *tc, AstNode *program) {
    if (!program) return 1;

    Scope scope;
    scope_init(&scope);

    /* parser_parse_program returns a synthetic module wrapping the actual
     * top-level declarations. Walk one level down for the real items. */
    if (program->kind == AST_MODULE_DECL) {
        ModuleDecl *root = &program->as.module;
        for (size_t i = 0; i < root->n_items; ++i) {
            AstNode *top = root->items[i];
            if (top->kind == AST_MODULE_DECL) {
                check_module(tc, &scope, top);
            } else if (top->kind == AST_CHAIN_DECL) {
                /* Chain manifests have no expression-level bodies to check
                 * at this layer. Surface them as resolved type expressions
                 * for the codegen pass to consume. */
                ChainDecl *c = &top->as.chain;
                for (size_t k = 0; k < c->n_assignments; ++k) {
                    AstNode *assign = c->assignments[k];
                    (void)resolve_type_expr(tc, assign->as.subsystem.value);
                }
            } else if (top->kind == AST_PROTOCOL_DECL) {
                /* Protocols hold signatures; treat like a module for now. */
                ProtocolDecl *p = &top->as.protocol;
                for (size_t k = 0; k < p->n_items; ++k) {
                    AstNode *it = p->items[k];
                    if (it->kind == AST_FN_DECL) {
                        check_fn(tc, &scope, it);
                    }
                }
            }
        }
    }

    free(scope.entries);
    return tc->n_errors == 0 ? 0 : 1;
}
