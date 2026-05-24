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
 *   - binary + - * /         -> i64.add / sub / mul / div_u
 *   - binary == != < > <= >= -> i64.eq / ne / lt_u / gt_u / le_u / ge_u
 *   - state = expr           -> i32.const <slot>; <expr>; call state_set
 *   - module-fn call         -> args...; call <fn index>
 *   - block trailing expr    -> becomes the function's return value
 *
 * What we deliberately reject
 * ---------------------------
 *   - let statements (no local-slot allocation yet)
 *   - control flow (if/match/loops)
 *   - sum types (`Result`, `Option`)
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
    /* per-function local table (params today; lets later) */
    LocalEntry *locals;
    size_t     n_locals;
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
    for (size_t i = 0; i < ctx->n_locals; ++i) {
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
            cg_report(ctx, &s->span,
                      "let bindings not yet supported in v0 codegen");
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

static void emit_fn_body(CodegenCtx *ctx, AstNode *fn_decl,
                         WasmBuf *body_out) {
    FnDecl *f = &fn_decl->as.fn;

    /* Build the per-fn local table. v0 only registers params; let-bindings
     * are rejected by emit_block above. */
    ctx->n_locals = 0;
    free(ctx->locals);
    ctx->locals = f->n_params ? calloc(f->n_params, sizeof(LocalEntry)) : NULL;
    if (f->n_params && !ctx->locals) {
        fputs("fatal: out of memory in codegen\n", stderr); exit(1);
    }
    for (size_t i = 0; i < f->n_params; ++i) {
        ctx->locals[i].name = f->params[i]->as.fn_param.name;
        ctx->locals[i].local_index = (uint32_t)i;
        ctx->n_locals++;
    }

    /* Local declaration count = 0 (no let bindings declared as locals). */
    wasm_write_leb_u32(body_out, 0);

    if (!f->body) {
        /* synthesize a 0 return so the WASM body still validates */
        wasm_write_byte(body_out, WASM_OP_I64_CONST);
        wasm_write_leb_i64(body_out, 0);
    } else {
        emit_block(ctx, body_out, f->body);
        /* If the block has no trailing expression and no return, push a
         * default 0 so the function's i64 return type is satisfied. */
        ExprBlock *bb = &f->body->as.block;
        int needs_default = 1;
        if (bb->result) needs_default = 0;
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
