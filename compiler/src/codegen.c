/*
 * Cleave -> WebAssembly code generator (issue #17).
 *
 * Module shape produced
 * ---------------------
 *
 * Every Cleave module compiles into a WASM module with this fixed import
 * shape (per spec/abi/wasm.md):
 *
 *     (import "env" "state_get" (func (param i32) (result i64)))   ; index 0
 *     (import "env" "state_set" (func (param i32 i64)))             ; index 1
 *
 * Local function indices start at 2. Each Cleave `fn` becomes a local
 * function exported under its source name; each `state` declaration is
 * assigned a slot index in source order (0..N).
 *
 * What we lower today
 * -------------------
 *   - integer literals       -> i64.const
 *   - state identifier read  -> i32.const <slot>; call state_get
 *   - param identifier read  -> local.get <idx>
 *   - let binding            -> <expr>; local.set <idx>  (issue #44)
 *   - let identifier read    -> local.get <idx>
 *   - binary + - * /         -> i64.add / sub / mul / div_u
 *   - binary == != < > <= >= -> i64.eq / ne / lt_u / gt_u / le_u / ge_u
 *   - state = expr           -> i32.const <slot>; <expr>; call state_set
 *   - local = expr           -> <expr>; local.set <idx>
 *   - module-fn call         -> args...; call <fn index>
 *   - if / else / else-if    -> <cond>; if; <then>; else; <else>; end (issue #49)
 *   - match literal arms     -> save scrutinee; chained i64.eq+if/else (issue #43)
 *   - block trailing expr    -> becomes the function's return value
 *
 * What we deliberately reject
 * ---------------------------
 *   - Loops (no language construct yet, deliberate)
 *   - match binding patterns (`x => use(x)`): identifiers in pattern
 *     position currently act as wildcards; binding patterns need
 *     locals + sum types and are gated on #44, #48
 *   - if-as-expression (`let x = if c { 1 } else { 2 }`): grammar does
 *     not parse if at expression position today; lift when the parser
 *     does
 *   - sum types (`Result`, `Option`) -- see issue #48
 *   - identifiers we cannot resolve
 *
 * Anything in the reject list is reported with a codegen-not-supported
 * diagnostic so the user sees what to wait for, rather than a confusing
 * "validation error" downstream from the runtime.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "codegen.h"
#include "wasm.h"

/* ============== compile context ============== */

typedef struct {
    StrRef   name;
    uint32_t slot;
} StateEntry;

typedef struct {
    StrRef   name;
    uint32_t fn_index;   /* WASM function index (after the 2 imports) */
    uint32_t type_index; /* WASM type-section index for this fn's sig */
    size_t   n_params;
} FnEntry;

typedef struct {
    StrRef   name;
    uint32_t local_index;
} LocalEntry;

typedef struct {
    Codegen   *cg;
    AstNode   *module;        /* the user's module node */
    StateEntry *states;
    size_t     n_states;
    FnEntry   *fns;
    size_t     n_fns;
    /* per-function local table (params + let bindings + match scratch) */
    LocalEntry *locals;
    size_t     n_locals;
    /* Index of a shared scratch local used by `emit_match` to hold the
     * scrutinee value while a chain of equality comparisons runs.
     * Allocated only when the fn body contains at least one match.
     * Matches do not nest in a way that needs separate slots (each
     * match completes its arm-selection chain before any arm body
     * executes), so one scratch per fn is sufficient. */
    int        has_match_scratch;
    uint32_t   match_scratch_local;
} CodegenCtx;

/* ============== diagnostics ============== */

static void cg_report(CodegenCtx *ctx, const Span *span, const char *fmt, ...) {
    ctx->cg->n_errors++;
    FILE *out = ctx->cg->err ? ctx->cg->err : stderr;
    fprintf(out, "%zu:%zu: codegen error: ",
            span->start_line, span->start_col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

/* ============== lookups ============== */

static int strref_eq(StrRef a, StrRef b) {
    return a.length == b.length && memcmp(a.start, b.start, a.length) == 0;
}

static const StateEntry *find_state(const CodegenCtx *ctx, StrRef name) {
    for (size_t i = 0; i < ctx->n_states; ++i) {
        if (strref_eq(ctx->states[i].name, name)) return &ctx->states[i];
    }
    return NULL;
}

static const FnEntry *find_fn(const CodegenCtx *ctx, StrRef name) {
    for (size_t i = 0; i < ctx->n_fns; ++i) {
        if (strref_eq(ctx->fns[i].name, name)) return &ctx->fns[i];
    }
    return NULL;
}

static const LocalEntry *find_local(const CodegenCtx *ctx, StrRef name) {
    /* Scan from the back so shadowing works the way users expect: the
     * most recent binding with a given name wins. */
    for (size_t i = ctx->n_locals; i-- > 0; ) {
        if (strref_eq(ctx->locals[i].name, name)) return &ctx->locals[i];
    }
    return NULL;
}

/* ============== compile-time symbol table build ============== */

static void collect_module_symbols(CodegenCtx *ctx, AstNode *module) {
    ModuleDecl *m = &module->as.module;

    /* Pass 1: count, then allocate. Two passes keep the code simple and
     * the arrays exactly sized; for v0 module bodies this costs nothing. */
    size_t n_states = 0, n_fns = 0;
    for (size_t i = 0; i < m->n_items; ++i) {
        switch (m->items[i]->kind) {
        case AST_STATE_DECL: n_states++; break;
        case AST_FN_DECL:    n_fns++; break;
        default: break;
        }
    }

    ctx->states = n_states ? calloc(n_states, sizeof(StateEntry)) : NULL;
    ctx->fns    = n_fns    ? calloc(n_fns,    sizeof(FnEntry))    : NULL;
    if ((n_states && !ctx->states) || (n_fns && !ctx->fns)) {
        fputs("fatal: out of memory in codegen\n", stderr);
        exit(1);
    }

    ctx->n_states = 0;
    ctx->n_fns = 0;

    /* Hostcall imports occupy fn indices 0 and 1; locals start at 2. */
    uint32_t next_fn_index = 2;

    for (size_t i = 0; i < m->n_items; ++i) {
        AstNode *item = m->items[i];
        if (item->kind == AST_STATE_DECL) {
            ctx->states[ctx->n_states].name = item->as.state.name;
            ctx->states[ctx->n_states].slot = (uint32_t)ctx->n_states;
            ctx->n_states++;
        } else if (item->kind == AST_FN_DECL) {
            FnEntry *f = &ctx->fns[ctx->n_fns];
            f->name = item->as.fn.name;
            f->fn_index = next_fn_index++;
            f->n_params = item->as.fn.n_params;
            /* type_index is filled in by emit_type_section once we know
             * how many distinct arities exist. */
            f->type_index = 0;
            ctx->n_fns++;
        }
    }
}

/* ============== expression / statement lowering ============== */

static void emit_expr(CodegenCtx *ctx, WasmBuf *body, AstNode *node);

static int parse_int_literal(StrRef text, int64_t *out) {
    /* Match the lexer's accepted forms: 0x.., 0b.., decimal with `_`
     * separators. The lexer guarantees the lexeme is syntactically valid;
     * we just need to compute the numeric value. */
    int64_t v = 0;
    if (text.length >= 2 && text.start[0] == '0' &&
        (text.start[1] == 'x' || text.start[1] == 'X')) {
        for (size_t i = 2; i < text.length; ++i) {
            char c = text.start[i];
            if (c == '_') continue;
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? 10 + c - 'a'
                  : (c >= 'A' && c <= 'F') ? 10 + c - 'A'
                  : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
    } else if (text.length >= 2 && text.start[0] == '0' &&
               (text.start[1] == 'b' || text.start[1] == 'B')) {
        for (size_t i = 2; i < text.length; ++i) {
            char c = text.start[i];
            if (c == '_') continue;
            if (c != '0' && c != '1') return 0;
            v = v * 2 + (c - '0');
        }
    } else {
        for (size_t i = 0; i < text.length; ++i) {
            char c = text.start[i];
            if (c == '_') continue;
            if (c < '0' || c > '9') return 0;
            v = v * 10 + (c - '0');
        }
    }
    *out = v;
    return 1;
}

static void emit_load_ident(CodegenCtx *ctx, WasmBuf *body, AstNode *node) {
    StrRef name = node->as.ident.name;

    const LocalEntry *l = find_local(ctx, name);
    if (l) {
        wasm_write_byte(body, WASM_OP_LOCAL_GET);
        wasm_write_leb_u32(body, l->local_index);
        return;
    }

    const StateEntry *s = find_state(ctx, name);
    if (s) {
        /* push slot index, then call state_get */
        wasm_write_byte(body, WASM_OP_I32_CONST);
        wasm_write_leb_i32(body, (int32_t)s->slot);
        wasm_write_byte(body, WASM_OP_CALL);
        wasm_write_leb_u32(body, 0); /* state_get is import index 0 */
        return;
    }

    const FnEntry *f = find_fn(ctx, name);
    if (f) {
        /* Bare function reference as a value. Not legal in our v0
         * codegen since we have no function-pointer types. */
        cg_report(ctx, &node->span,
                  "function '%.*s' used as a value (not supported in v0)",
                  (int)name.length, name.start);
        wasm_write_byte(body, WASM_OP_I64_CONST);
        wasm_write_leb_i64(body, 0);
        return;
    }

    cg_report(ctx, &node->span,
              "unresolved identifier '%.*s'",
              (int)name.length, name.start);
    /* Emit a benign placeholder so the body still parses as WASM. */
    wasm_write_byte(body, WASM_OP_I64_CONST);
    wasm_write_leb_i64(body, 0);
}

static uint8_t binary_opcode(StrRef op) {
    if (op.length == 1) {
        switch (op.start[0]) {
        case '+': return WASM_OP_I64_ADD;
        case '-': return WASM_OP_I64_SUB;
        case '*': return WASM_OP_I64_MUL;
        case '/': return WASM_OP_I64_DIV_U;
        case '<': return WASM_OP_I64_LT_U;
        case '>': return WASM_OP_I64_GT_U;
        }
    } else if (op.length == 2) {
        if (memcmp(op.start, "==", 2) == 0) return WASM_OP_I64_EQ;
        if (memcmp(op.start, "!=", 2) == 0) return WASM_OP_I64_NE;
        if (memcmp(op.start, "<=", 2) == 0) return WASM_OP_I64_LE_U;
        if (memcmp(op.start, ">=", 2) == 0) return WASM_OP_I64_GE_U;
    }
    return 0;
}

static void emit_assignment(CodegenCtx *ctx, WasmBuf *body, AstNode *node) {
    ExprBinary *b = &node->as.binary;
    if (b->lhs->kind != AST_EXPR_IDENT) {
        cg_report(ctx, &node->span,
                  "assignment LHS must be an identifier (got %s)",
                  ast_kind_name(b->lhs->kind));
        return;
    }
    StrRef name = b->lhs->as.ident.name;

    const StateEntry *s = find_state(ctx, name);
    if (s) {
        /* state_set(slot, value); slot first per stack order */
        wasm_write_byte(body, WASM_OP_I32_CONST);
        wasm_write_leb_i32(body, (int32_t)s->slot);
        emit_expr(ctx, body, b->rhs);
        wasm_write_byte(body, WASM_OP_CALL);
        wasm_write_leb_u32(body, 1); /* state_set import index */
        return;
    }

    const LocalEntry *l = find_local(ctx, name);
    if (l) {
        emit_expr(ctx, body, b->rhs);
        wasm_write_byte(body, WASM_OP_LOCAL_SET);
        wasm_write_leb_u32(body, l->local_index);
        return;
    }

    cg_report(ctx, &node->span,
              "assignment target '%.*s' is not a known state or local",
              (int)name.length, name.start);
}

static void emit_call(CodegenCtx *ctx, WasmBuf *body, AstNode *node) {
    ExprCall *c = &node->as.call;
    if (c->callee->kind != AST_EXPR_IDENT) {
        cg_report(ctx, &c->callee->span,
                  "indirect calls not supported in v0 codegen");
        return;
    }
    StrRef name = c->callee->as.ident.name;
    const FnEntry *f = find_fn(ctx, name);
    if (!f) {
        cg_report(ctx, &c->callee->span,
                  "call to unknown function '%.*s'",
                  (int)name.length, name.start);
        return;
    }
    if (c->n_args != f->n_params) {
        cg_report(ctx, &node->span,
                  "call to '%.*s' has %zu arg(s), expected %zu",
                  (int)name.length, name.start, c->n_args, f->n_params);
        /* still emit args so the wasm body is syntactically valid */
    }
    for (size_t i = 0; i < c->n_args; ++i) {
        emit_expr(ctx, body, c->args[i]);
    }
    wasm_write_byte(body, WASM_OP_CALL);
    wasm_write_leb_u32(body, f->fn_index);
}

/* Returns 1 if `expr`, when emitted, pushes a value onto the operand
 * stack.  The only v0 expression that does NOT push is an assignment
 * (`x = y`), because both state_set (a void hostcall) and local.set
 * consume the value silently.  Callers use this to decide whether
 * trailing-expression slots and if-branch results need a `drop` or
 * fn-body default-zero to balance the stack.
 *
 * For blocks, we recurse into `block.result`: a block leaves a value
 * iff its trailing expression does.  Lets `emit_match` correctly
 * decide whether to drop after each arm body, since arm bodies are
 * full expressions that may themselves be blocks ending in an
 * assignment. */
static int leaves_value_on_stack(const AstNode *expr) {
    if (!expr) return 0;
    if (expr->kind == AST_EXPR_BLOCK) {
        return leaves_value_on_stack(expr->as.block.result);
    }
    if (expr->kind == AST_EXPR_BINARY
        && expr->as.binary.op.length == 1
        && expr->as.binary.op.start[0] == '=') {
        return 0;
    }
    return 1;
}

/* Returns 1 if `node`, when emitted, leaves an i32 on the WASM operand
 * stack rather than the usual i64. The v0 codegen produces i32 only for
 * comparison binary operators (i64.eq, i64.lt_u, etc. all return i32
 * per the WASM spec). Everything else (literals, identifiers, calls,
 * arithmetic) leaves i64. Used by emit_if to decide whether the cond
 * value needs an i32.wrap_i64 coercion before the `if` instruction
 * consumes it. */
static int leaves_i32_on_stack(const AstNode *node) {
    if (!node) return 0;
    if (node->kind != AST_EXPR_BINARY) return 0;
    StrRef op = node->as.binary.op;
    if (op.length == 1) {
        return op.start[0] == '<' || op.start[0] == '>';
    }
    if (op.length == 2) {
        return memcmp(op.start, "==", 2) == 0
            || memcmp(op.start, "!=", 2) == 0
            || memcmp(op.start, "<=", 2) == 0
            || memcmp(op.start, ">=", 2) == 0;
    }
    return 0;
}

static void emit_if(CodegenCtx *ctx, WasmBuf *body, AstNode *node);
static void emit_match(CodegenCtx *ctx, WasmBuf *body, AstNode *node);
static void emit_block(CodegenCtx *ctx, WasmBuf *body, AstNode *node);

/* Emit one branch of an if statement.  v0 if statements have an empty
 * block type, so any value the branch leaves on the stack must be
 * dropped to keep the WASM type checker happy.  When sum types and
 * if-as-expression land, this layer learns to pass the value through
 * instead of dropping it. */
static void emit_if_branch(CodegenCtx *ctx, WasmBuf *body, AstNode *branch) {
    if (!branch) return;
    if (branch->kind == AST_EXPR_BLOCK) {
        emit_block(ctx, body, branch);
        /* Drop the trailing result so the block produces no value, but
         * only when the result expression actually pushed something.
         * Assignments (state_set / local.set) leave the stack empty;
         * a `drop` there would underflow. */
        if (leaves_value_on_stack(branch->as.block.result)) {
            wasm_write_byte(body, WASM_OP_DROP);
        }
    } else if (branch->kind == AST_STMT_IF) {
        /* `else if` chain: nest another if/else/end. */
        emit_if(ctx, body, branch);
    }
}

/* v0 match codegen: lowers `match scrut { p1 => e1, ..., _ => en }`
 * into a chain of equality comparisons against the scrutinee + nested
 * if/else.  Supported patterns:
 *   - integer literal: compares `scrut == literal` via i64.eq
 *   - bool literal: same
 *   - identifier (including `_`): always matches, acts as a final
 *     else branch.  We treat any identifier as a wildcard for v0;
 *     binding patterns (`x => use(x)`) and constructor patterns need
 *     locals + sum types and are gated on issues #44, #48.
 *
 * Match is statement-shaped in v0: each arm body's value is dropped
 * after evaluation.  When sum types and match-as-expression land,
 * arms will share a result type and the chain block-types update.
 *
 * Wildcard arms after the first short-circuit further arms; we honor
 * them as the chain terminator and stop emitting subsequent arms.
 * An unmatched scrutinee with no wildcard is a silent no-op today;
 * exhaustiveness checking lives in the type checker (future work).
 */
static int pattern_is_wildcard(const AstNode *pat) {
    return pat && pat->kind == AST_EXPR_IDENT;
}

static void emit_arm_body(CodegenCtx *ctx, WasmBuf *body, AstNode *arm_body) {
    emit_expr(ctx, body, arm_body);
    if (leaves_value_on_stack(arm_body)) {
        wasm_write_byte(body, WASM_OP_DROP);
    }
}

static void emit_match_arms(CodegenCtx *ctx, WasmBuf *body,
                            AstNode **arms, size_t n_arms, size_t idx,
                            uint32_t scratch) {
    if (idx >= n_arms) return;
    AstNode *arm = arms[idx];
    AstNode *pattern = arm->as.match_arm.pattern;
    AstNode *arm_body = arm->as.match_arm.body;

    if (pattern_is_wildcard(pattern)) {
        /* Wildcard absorbs the rest of the chain; subsequent arms are
         * unreachable. v0 codegen silently drops them. */
        emit_arm_body(ctx, body, arm_body);
        return;
    }

    /* Comparison: local.get scratch; <pattern>; i64.eq -> i32 on stack. */
    wasm_write_byte(body, WASM_OP_LOCAL_GET);
    wasm_write_leb_u32(body, scratch);
    emit_expr(ctx, body, pattern);
    wasm_write_byte(body, WASM_OP_I64_EQ);

    wasm_write_byte(body, WASM_OP_IF);
    wasm_write_byte(body, WASM_BLOCKTYPE_EMPTY);

    emit_arm_body(ctx, body, arm_body);

    int has_more = (idx + 1) < n_arms;
    if (has_more) {
        wasm_write_byte(body, WASM_OP_ELSE);
        emit_match_arms(ctx, body, arms, n_arms, idx + 1, scratch);
    }

    wasm_write_byte(body, WASM_OP_END);
}

static void emit_match(CodegenCtx *ctx, WasmBuf *body, AstNode *node) {
    StmtMatch *m = &node->as.match_stmt;

    if (!ctx->has_match_scratch) {
        /* The pre-scan should have allocated the scratch local. If it
         * didn't, the locals declaration is wrong and the resulting
         * WASM will be invalid; surface it as a clear diagnostic. */
        cg_report(ctx, &node->span,
                  "internal: match statement encountered but scratch "
                  "local was not allocated during pre-scan");
        return;
    }

    /* Emit scrutinee onto the stack, then save into the scratch local
     * so subsequent comparisons can re-read it without re-evaluating. */
    emit_expr(ctx, body, m->scrutinee);
    wasm_write_byte(body, WASM_OP_LOCAL_SET);
    wasm_write_leb_u32(body, ctx->match_scratch_local);

    if (m->n_arms == 0) return;

    emit_match_arms(ctx, body, m->arms, m->n_arms, 0,
                    ctx->match_scratch_local);
}

static void emit_if(CodegenCtx *ctx, WasmBuf *body, AstNode *node) {
    StmtIf *i = &node->as.if_stmt;

    emit_expr(ctx, body, i->cond);
    if (!leaves_i32_on_stack(i->cond)) {
        /* The condition came out as i64 (bool literal, identifier read,
         * call result, etc.).  WASM `if` expects an i32, so narrow. */
        wasm_write_byte(body, WASM_OP_I32_WRAP_I64);
    }

    wasm_write_byte(body, WASM_OP_IF);
    wasm_write_byte(body, WASM_BLOCKTYPE_EMPTY);

    emit_if_branch(ctx, body, i->then_branch);

    if (i->else_branch) {
        wasm_write_byte(body, WASM_OP_ELSE);
        emit_if_branch(ctx, body, i->else_branch);
    }

    wasm_write_byte(body, WASM_OP_END);
}

static void emit_block(CodegenCtx *ctx, WasmBuf *body, AstNode *node) {
    ExprBlock *b = &node->as.block;
    for (size_t i = 0; i < b->n_stmts; ++i) {
        AstNode *s = b->stmts[i];
        if (s->kind == AST_STMT_EXPR) {
            AstNode *e = s->as.expr_stmt.expr;
            if (e->kind == AST_EXPR_BINARY && e->as.binary.op.length == 1 &&
                e->as.binary.op.start[0] == '=') {
                /* assignment statement: leaves nothing on stack */
                emit_assignment(ctx, body, e);
            } else {
                /* general expression statement: emit then drop */
                emit_expr(ctx, body, e);
                wasm_write_byte(body, WASM_OP_DROP);
            }
        } else if (s->kind == AST_STMT_RETURN) {
            if (s->as.ret_stmt.value) {
                emit_expr(ctx, body, s->as.ret_stmt.value);
            } else {
                wasm_write_byte(body, WASM_OP_I64_CONST);
                wasm_write_leb_i64(body, 0);
            }
            wasm_write_byte(body, WASM_OP_RETURN);
        } else if (s->kind == AST_STMT_LET) {
            /* Local index was assigned during pre-scan in emit_fn_body;
             * find it via the symbol-table lookup. The most-recent-wins
             * scan order in find_local handles shadowing correctly. */
            StmtLet *l = &s->as.let_stmt;
            const LocalEntry *bind = find_local(ctx, l->name);
            if (!bind) {
                /* Shouldn't happen: pre-scan adds every let. Guard
                 * defensively so a future refactor of the pre-scan
                 * surfaces the bug as a clear diagnostic. */
                cg_report(ctx, &s->span,
                          "internal: let binding '%.*s' was not pre-scanned",
                          (int)l->name.length, l->name.start);
                continue;
            }
            emit_expr(ctx, body, l->value);
            wasm_write_byte(body, WASM_OP_LOCAL_SET);
            wasm_write_leb_u32(body, bind->local_index);
        } else if (s->kind == AST_STMT_IF) {
            emit_if(ctx, body, s);
        } else if (s->kind == AST_STMT_MATCH) {
            emit_match(ctx, body, s);
        } else {
            cg_report(ctx, &s->span,
                      "statement kind %s not yet supported in v0 codegen",
                      ast_kind_name(s->kind));
        }
    }
    if (b->result) {
        emit_expr(ctx, body, b->result);
    }
}

static void emit_expr(CodegenCtx *ctx, WasmBuf *body, AstNode *node) {
    if (!node) return;
    switch (node->kind) {
    case AST_EXPR_NUMBER: {
        int64_t v = 0;
        if (!parse_int_literal(node->as.number.text, &v)) {
            cg_report(ctx, &node->span, "malformed integer literal");
        }
        wasm_write_byte(body, WASM_OP_I64_CONST);
        wasm_write_leb_i64(body, v);
        return;
    }
    case AST_EXPR_BOOL: {
        wasm_write_byte(body, WASM_OP_I64_CONST);
        wasm_write_leb_i64(body, node->as.boolean.value ? 1 : 0);
        return;
    }
    case AST_EXPR_IDENT:
        emit_load_ident(ctx, body, node);
        return;
    case AST_EXPR_BINARY: {
        ExprBinary *b = &node->as.binary;
        /* Assignment-as-expression bubbles up: treat as statement (leaves
         * nothing on stack, which makes its enclosing usage an error). */
        if (b->op.length == 1 && b->op.start[0] == '=') {
            emit_assignment(ctx, body, node);
            return;
        }
        uint8_t op = binary_opcode(b->op);
        if (op == 0) {
            cg_report(ctx, &node->span,
                      "binary operator '%.*s' not yet supported in v0 codegen",
                      (int)b->op.length, b->op.start);
            return;
        }
        emit_expr(ctx, body, b->lhs);
        emit_expr(ctx, body, b->rhs);
        wasm_write_byte(body, op);
        return;
    }
    case AST_EXPR_CALL:
        emit_call(ctx, body, node);
        return;
    case AST_EXPR_BLOCK:
        emit_block(ctx, body, node);
        return;
    default:
        cg_report(ctx, &node->span,
                  "expression kind %s not yet supported in v0 codegen",
                  ast_kind_name(node->kind));
        return;
    }
}

/* ============== fn body emission ============== */

/* Walk the body block's top-level statements once to count locals the
 * codegen will need:
 *   - one local per `let` binding (issue #44)
 *   - one shared scratch local if any `match` statement appears (#43)
 *
 * v0 only scans the immediate body block since the codegen still does
 * not recurse for nested-stmt local discovery; lets buried inside
 * if/match branches are missed. The grammar already accepts them and
 * the parser builds the AST, but pre-scan ignores them for now. A
 * future iteration will recurse; see the same TODO on issue #44.
 */
typedef struct {
    size_t n_lets;
    int    has_match;
} LocalsCount;

static LocalsCount count_local_slots(const AstNode *body) {
    LocalsCount c = { 0, 0 };
    if (!body || body->kind != AST_EXPR_BLOCK) return c;
    const ExprBlock *b = &body->as.block;
    for (size_t i = 0; i < b->n_stmts; ++i) {
        AstKind k = b->stmts[i]->kind;
        if (k == AST_STMT_LET)   c.n_lets++;
        if (k == AST_STMT_MATCH) c.has_match = 1;
    }
    return c;
}

static void emit_fn_body(CodegenCtx *ctx, AstNode *fn_decl,
                         WasmBuf *body_out) {
    FnDecl *f = &fn_decl->as.fn;

    /* Pre-scan the body to size the local table. Locals layout:
     *   [0, n_params)                                  -- fn params
     *   [n_params, n_params + n_lets)                  -- let bindings
     *   [n_params + n_lets]                            -- match scratch (if any)
     */
    LocalsCount lc = count_local_slots(f->body);
    size_t n_lets = lc.n_lets;
    size_t n_extra_anon = lc.has_match ? 1 : 0;

    size_t cap = f->n_params + n_lets + n_extra_anon;
    ctx->n_locals = 0;
    free(ctx->locals);
    ctx->locals = cap ? calloc(cap, sizeof(LocalEntry)) : NULL;
    if (cap && !ctx->locals) {
        fputs("fatal: out of memory in codegen\n", stderr); exit(1);
    }
    for (size_t i = 0; i < f->n_params; ++i) {
        ctx->locals[i].name = f->params[i]->as.fn_param.name;
        ctx->locals[i].local_index = (uint32_t)i;
        ctx->n_locals++;
    }
    if (f->body && f->body->kind == AST_EXPR_BLOCK) {
        const ExprBlock *b = &f->body->as.block;
        for (size_t i = 0; i < b->n_stmts; ++i) {
            AstNode *s = b->stmts[i];
            if (s->kind != AST_STMT_LET) continue;
            uint32_t idx = (uint32_t)ctx->n_locals;
            ctx->locals[ctx->n_locals].name = s->as.let_stmt.name;
            ctx->locals[ctx->n_locals].local_index = idx;
            ctx->n_locals++;
        }
    }
    ctx->has_match_scratch = lc.has_match;
    if (lc.has_match) {
        /* Anonymous local at the end of the table.  No name binding;
         * find_local won't surface it. emit_match references it via
         * ctx->match_scratch_local. */
        uint32_t idx = (uint32_t)ctx->n_locals;
        ctx->locals[ctx->n_locals].name.start = NULL;
        ctx->locals[ctx->n_locals].name.length = 0;
        ctx->locals[ctx->n_locals].local_index = idx;
        ctx->n_locals++;
        ctx->match_scratch_local = idx;
    }

    /* Local declaration: 0 groups if no extra locals, else 1 group of
     * (n_lets + n_extra_anon) i64 entries. All widen to i64 today,
     * matching the rest of the v0 ABI. */
    size_t total_extra = n_lets + n_extra_anon;
    if (total_extra == 0) {
        wasm_write_leb_u32(body_out, 0);
    } else {
        wasm_write_leb_u32(body_out, 1);
        wasm_write_leb_u32(body_out, (uint32_t)total_extra);
        wasm_write_byte(body_out, WASM_TYPE_I64);
    }

    if (!f->body) {
        /* synthesize a 0 return so the WASM body still validates */
        wasm_write_byte(body_out, WASM_OP_I64_CONST);
        wasm_write_leb_i64(body_out, 0);
    } else {
        emit_block(ctx, body_out, f->body);
        /* If the block has no trailing expression and no return, push a
         * default 0 so the function's i64 return type is satisfied.
         * `leaves_value_on_stack` treats an assignment-as-result as
         * not-a-value, which is the right call here: a fn that ends
         * with `x = expr` needs the synthetic 0 just like one with no
         * trailing expression at all. */
        ExprBlock *bb = &f->body->as.block;
        int needs_default = 1;
        if (leaves_value_on_stack(bb->result)) needs_default = 0;
        for (size_t i = 0; i < bb->n_stmts; ++i) {
            if (bb->stmts[i]->kind == AST_STMT_RETURN) {
                needs_default = 0;
                break;
            }
        }
        if (needs_default) {
            wasm_write_byte(body_out, WASM_OP_I64_CONST);
            wasm_write_leb_i64(body_out, 0);
        }
    }

    wasm_write_byte(body_out, WASM_OP_END);
}

/* ============== section emitters ============== */

/* Determine the largest fn arity in the module; types beyond that are
 * never needed. */
static size_t max_fn_arity(const CodegenCtx *ctx) {
    size_t mx = 0;
    for (size_t i = 0; i < ctx->n_fns; ++i) {
        if (ctx->fns[i].n_params > mx) mx = ctx->fns[i].n_params;
    }
    return mx;
}

/* Type section layout:
 *   index 0: (i32) -> i64        (state_get)
 *   index 1: (i32, i64) -> ()    (state_set)
 *   index 2 + k: (i64 x k) -> i64 for fn arities 0..max
 */
static void emit_type_section(CodegenCtx *ctx, WasmBuf *out) {
    WasmBuf body;
    wasm_init(&body);

    size_t max_arity = max_fn_arity(ctx);
    size_t n_types = 2 + (max_arity + 1);

    wasm_write_leb_u32(&body, (uint32_t)n_types);

    /* type 0: (i32) -> i64 */
    wasm_write_byte(&body, WASM_TYPE_FUNC);
    wasm_write_leb_u32(&body, 1);
    wasm_write_byte(&body, WASM_TYPE_I32);
    wasm_write_leb_u32(&body, 1);
    wasm_write_byte(&body, WASM_TYPE_I64);

    /* type 1: (i32, i64) -> () */
    wasm_write_byte(&body, WASM_TYPE_FUNC);
    wasm_write_leb_u32(&body, 2);
    wasm_write_byte(&body, WASM_TYPE_I32);
    wasm_write_byte(&body, WASM_TYPE_I64);
    wasm_write_leb_u32(&body, 0);

    /* types 2..2+max_arity: (i64 x k) -> i64 */
    for (size_t k = 0; k <= max_arity; ++k) {
        wasm_write_byte(&body, WASM_TYPE_FUNC);
        wasm_write_leb_u32(&body, (uint32_t)k);
        for (size_t p = 0; p < k; ++p) {
            wasm_write_byte(&body, WASM_TYPE_I64);
        }
        wasm_write_leb_u32(&body, 1);
        wasm_write_byte(&body, WASM_TYPE_I64);
    }

    /* Fill in each fn's type_index now that we know the layout. */
    for (size_t i = 0; i < ctx->n_fns; ++i) {
        ctx->fns[i].type_index = (uint32_t)(2 + ctx->fns[i].n_params);
    }

    wasm_emit_section(out, WASM_SECTION_TYPE, &body);
    wasm_free(&body);
}

static void emit_import_section(CodegenCtx *ctx, WasmBuf *out) {
    (void)ctx;
    WasmBuf body;
    wasm_init(&body);

    wasm_write_leb_u32(&body, 2);

    /* import 0: env.state_get : type 0 */
    wasm_write_name(&body, "env", 3);
    wasm_write_name(&body, "state_get", 9);
    wasm_write_byte(&body, 0x00); /* func descriptor */
    wasm_write_leb_u32(&body, 0); /* type 0 */

    /* import 1: env.state_set : type 1 */
    wasm_write_name(&body, "env", 3);
    wasm_write_name(&body, "state_set", 9);
    wasm_write_byte(&body, 0x00);
    wasm_write_leb_u32(&body, 1);

    wasm_emit_section(out, WASM_SECTION_IMPORT, &body);
    wasm_free(&body);
}

static void emit_function_section(CodegenCtx *ctx, WasmBuf *out) {
    WasmBuf body;
    wasm_init(&body);

    wasm_write_leb_u32(&body, (uint32_t)ctx->n_fns);
    for (size_t i = 0; i < ctx->n_fns; ++i) {
        wasm_write_leb_u32(&body, ctx->fns[i].type_index);
    }

    wasm_emit_section(out, WASM_SECTION_FUNCTION, &body);
    wasm_free(&body);
}

static void emit_export_section(CodegenCtx *ctx, WasmBuf *out) {
    WasmBuf body;
    wasm_init(&body);

    wasm_write_leb_u32(&body, (uint32_t)ctx->n_fns);
    for (size_t i = 0; i < ctx->n_fns; ++i) {
        FnEntry *f = &ctx->fns[i];
        wasm_write_name(&body, f->name.start, f->name.length);
        wasm_write_byte(&body, WASM_EXPORT_FUNC);
        wasm_write_leb_u32(&body, f->fn_index);
    }

    wasm_emit_section(out, WASM_SECTION_EXPORT, &body);
    wasm_free(&body);
}

static void emit_code_section(CodegenCtx *ctx, WasmBuf *out) {
    WasmBuf body;
    wasm_init(&body);

    wasm_write_leb_u32(&body, (uint32_t)ctx->n_fns);

    ModuleDecl *m = &ctx->module->as.module;
    size_t fn_seen = 0;
    for (size_t i = 0; i < m->n_items; ++i) {
        AstNode *item = m->items[i];
        if (item->kind != AST_FN_DECL) continue;

        WasmBuf fn_body;
        wasm_init(&fn_body);
        emit_fn_body(ctx, item, &fn_body);

        /* Each code entry is itself length-prefixed. */
        wasm_write_leb_u32(&body, (uint32_t)fn_body.len);
        wasm_write_bytes(&body, fn_body.data, fn_body.len);

        wasm_free(&fn_body);
        fn_seen++;
    }
    (void)fn_seen;

    wasm_emit_section(out, WASM_SECTION_CODE, &body);
    wasm_free(&body);
}

/* ============== public API ============== */

void cg_init(Codegen *cg, FILE *err) {
    cg->err = err;
    cg->n_errors = 0;
}

CodegenStatus cg_compile_program(Codegen *cg, AstNode *program, WasmBuf *out) {
    if (!program) return CG_ERR;

    /* Find the first AST_MODULE_DECL under the synthetic program root. */
    AstNode *user_module = NULL;
    if (program->kind == AST_MODULE_DECL) {
        ModuleDecl *root = &program->as.module;
        for (size_t i = 0; i < root->n_items; ++i) {
            if (root->items[i]->kind == AST_MODULE_DECL) {
                user_module = root->items[i];
                break;
            }
        }
    }
    if (!user_module) {
        FILE *err = cg->err ? cg->err : stderr;
        fputs("codegen error: program contains no module declaration\n", err);
        cg->n_errors++;
        return CG_ERR;
    }

    CodegenCtx ctx = {0};
    ctx.cg = cg;
    ctx.module = user_module;
    collect_module_symbols(&ctx, user_module);

    wasm_init(out);
    wasm_emit_header(out);
    emit_type_section(&ctx, out);
    emit_import_section(&ctx, out);
    emit_function_section(&ctx, out);
    emit_export_section(&ctx, out);
    emit_code_section(&ctx, out);

    free(ctx.states);
    free(ctx.fns);
    free(ctx.locals);

    if (cg->n_errors > 0) {
        wasm_free(out);
        return CG_ERR;
    }
    return CG_OK;
}
