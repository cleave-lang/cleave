/* Microbenchmark for the WASM code generator (issue #17).
 *
 * Each iteration: lex + parse + typecheck + codegen the full source.
 * Reports throughput so trend regressions on the compile-time budget
 * surface in CI logs. */

#include <stdio.h>

#include "ast.h"
#include "bench.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "typecheck.h"
#include "wasm.h"

static const char *COUNTER_MVP =
    "module Counter {\n"
    "  state count: u64\n"
    "  fn increment() -> u64 { count = count + 1; count }\n"
    "  fn read() -> u64 { count }\n"
    "}";

static const char *ARITHMETIC_HEAVY =
    "module M {\n"
    "  state x: u64\n"
    "  fn f() -> u64 {\n"
    "    x = x + 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8\n"
    "    x = x * 2 - x / 2 + x\n"
    "    x\n"
    "  }\n"
    "}";

/* Exercises the if/else codegen path (issue #49): an else-if chain
 * that drives the cond coercion + nested-if logic. */
static const char *IF_HEAVY =
    "module M {\n"
    "  state x: u64\n"
    "  fn classify(n: u64) -> u64 {\n"
    "    if n == 0 { x = 1 }\n"
    "    else if n == 1 { x = 2 }\n"
    "    else if n == 2 { x = 3 }\n"
    "    else if n == 3 { x = 4 }\n"
    "    else if n == 4 { x = 5 }\n"
    "    else { x = 6 }\n"
    "    x\n"
    "  }\n"
    "}";

/* Exercises the let-binding codegen path (issue #44): eight local
 * variables, the trailing expression reads several of them. The
 * pre-scan walks 8 stmts; the locals declaration is one i64 group
 * of 8; emission produces 8 local.set + several local.get sequences. */
static const char *LET_HEAVY =
    "module M {\n"
    "  fn f(a: u64) -> u64 {\n"
    "    let b = a + 1\n"
    "    let c = b + 2\n"
    "    let d = c + 3\n"
    "    let e = d + 4\n"
    "    let g = e + 5\n"
    "    let h = g + 6\n"
    "    let i = h + 7\n"
    "    let j = i + 8\n"
    "    j\n"
    "  }\n"
    "}";

static FILE *DEV_NULL;

int main(void) {
    DEV_NULL = fopen("/dev/null", "w");
    if (!DEV_NULL) { fputs("bench: cannot open /dev/null\n", stderr); return 1; }

    BENCH("codegen_counter_mvp", {
        Lexer lex; lexer_init(&lex, COUNTER_MVP);
        Parser p;  parser_init(&p, &lex);
        AstNode *prog = parser_parse_program(&p);
        TypeChecker tc; typecheck_init(&tc, DEV_NULL);
        (void)typecheck_program(&tc, prog);
        Codegen cg; cg_init(&cg, DEV_NULL);
        WasmBuf bin;
        (void)cg_compile_program(&cg, prog, &bin);
        wasm_free(&bin);
    });

    BENCH("codegen_arithmetic_heavy", {
        Lexer lex; lexer_init(&lex, ARITHMETIC_HEAVY);
        Parser p;  parser_init(&p, &lex);
        AstNode *prog = parser_parse_program(&p);
        TypeChecker tc; typecheck_init(&tc, DEV_NULL);
        (void)typecheck_program(&tc, prog);
        Codegen cg; cg_init(&cg, DEV_NULL);
        WasmBuf bin;
        (void)cg_compile_program(&cg, prog, &bin);
        wasm_free(&bin);
    });

    BENCH("codegen_let_heavy", {
        Lexer lex; lexer_init(&lex, LET_HEAVY);
        Parser p;  parser_init(&p, &lex);
        AstNode *prog = parser_parse_program(&p);
        TypeChecker tc; typecheck_init(&tc, DEV_NULL);
        (void)typecheck_program(&tc, prog);
        Codegen cg; cg_init(&cg, DEV_NULL);
        WasmBuf bin;
        (void)cg_compile_program(&cg, prog, &bin);
        wasm_free(&bin);
    });

    BENCH("codegen_if_heavy", {
        Lexer lex; lexer_init(&lex, IF_HEAVY);
        Parser p;  parser_init(&p, &lex);
        AstNode *prog = parser_parse_program(&p);
        TypeChecker tc; typecheck_init(&tc, DEV_NULL);
        (void)typecheck_program(&tc, prog);
        Codegen cg; cg_init(&cg, DEV_NULL);
        WasmBuf bin;
        (void)cg_compile_program(&cg, prog, &bin);
        wasm_free(&bin);
    });

    fclose(DEV_NULL);
    return 0;
}
