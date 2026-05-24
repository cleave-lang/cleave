/* Microbenchmark for the type checker (issue #34).
 *
 * Each iteration: lex + parse + typecheck the full source. The bench
 * harness in bench.h reports `BENCH name=<x> iters=<n> ops_per_sec=<n>`
 * lines that CI parses for trend tracking. */

#include <stdio.h>

#include "bench.h"
#include "lexer.h"
#include "parser.h"
#include "typecheck.h"

static const char *COUNTER_MODULE =
    "module Counter {\n"
    "    state count: u64\n"
    "    fn increment() -> u64 {\n"
    "        count = count + 1\n"
    "        count\n"
    "    }\n"
    "    fn read() -> u64 { count }\n"
    "}";

static const char *ARITHMETIC_HEAVY =
    "module M {\n"
    "    fn f(a: u64, b: u64, c: u64) -> u64 {\n"
    "        let x = a + b * c - a / b\n"
    "        let y = x + x + x + x + x + x + x + x\n"
    "        let z = y * y - y / 2\n"
    "        z\n"
    "    }\n"
    "}";

static const char *PROTOCOL_WITH_FNS =
    "protocol P implements Base {\n"
    "    fn select(epoch: u64) -> u64 { epoch + 1 }\n"
    "    fn weight(stake: u64, age: u64) -> u64 { stake * age }\n"
    "    fn threshold() -> u64 { 100 }\n"
    "}";

/* Suppress all diagnostics during benchmarking so the BENCH lines are
 * the only output. */
static FILE *DEV_NULL;

int main(void) {
    DEV_NULL = fopen("/dev/null", "w");
    if (!DEV_NULL) {
        fputs("bench: cannot open /dev/null\n", stderr);
        return 1;
    }

    BENCH("typecheck_counter_module", {
        Lexer lex; lexer_init(&lex, COUNTER_MODULE);
        Parser p; parser_init(&p, &lex);
        AstNode *prog = parser_parse_program(&p);
        TypeChecker tc; typecheck_init(&tc, DEV_NULL);
        (void)typecheck_program(&tc, prog);
    });

    BENCH("typecheck_arithmetic_heavy", {
        Lexer lex; lexer_init(&lex, ARITHMETIC_HEAVY);
        Parser p; parser_init(&p, &lex);
        AstNode *prog = parser_parse_program(&p);
        TypeChecker tc; typecheck_init(&tc, DEV_NULL);
        (void)typecheck_program(&tc, prog);
    });

    BENCH("typecheck_protocol_with_fns", {
        Lexer lex; lexer_init(&lex, PROTOCOL_WITH_FNS);
        Parser p; parser_init(&p, &lex);
        AstNode *prog = parser_parse_program(&p);
        TypeChecker tc; typecheck_init(&tc, DEV_NULL);
        (void)typecheck_program(&tc, prog);
    });

    fclose(DEV_NULL);
    return 0;
}
