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

    fclose(DEV_NULL);
    return 0;
}
