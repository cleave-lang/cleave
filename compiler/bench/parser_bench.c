#include "bench.h"
#include "lexer.h"
#include "parser.h"

static const char *MINIMAL_CHAIN =
    "chain MyChain {\n"
    "    consensus: Tendermint<sig=BLS12_381, finality=Deterministic>\n"
    "    gas:       Multidim<{cpu, storage, witness}>\n"
    "    state:     SparseMerkle<key=u256, hash=Blake3>\n"
    "    exec:      WasmVM<deterministic>\n"
    "}";

static const char *EMPTY_CHAIN = "chain Empty { }";

static const char *SUBSYSTEM_HEAVY =
    "chain Big {\n"
    "    consensus: Tendermint<sig=BLS12_381, finality=Deterministic, slashing=Conservative>\n"
    "    gas:       Multidim<{cpu, storage, witness, bandwidth, latency, proof}>\n"
    "    state:     SparseMerkle<key=u256, hash=Blake3, prove=KZG>\n"
    "    exec:      WasmVM<deterministic, fuel_metered, sandboxed>\n"
    "    da:        Celestia<sampling=128>\n"
    "}";

/* Each parser iteration allocates AST nodes; we leak them on purpose for
 * benchmarking. Memory ownership is documented in compiler/src/ast.c. */

int main(void) {
    BENCH("parse_empty_chain", {
        Lexer lex;
        lexer_init(&lex, EMPTY_CHAIN);
        Parser p;
        parser_init(&p, &lex);
        (void)parser_parse_chain(&p);
    });

    BENCH("parse_minimal_chain", {
        Lexer lex;
        lexer_init(&lex, MINIMAL_CHAIN);
        Parser p;
        parser_init(&p, &lex);
        (void)parser_parse_chain(&p);
    });

    BENCH("parse_subsystem_heavy", {
        Lexer lex;
        lexer_init(&lex, SUBSYSTEM_HEAVY);
        Parser p;
        parser_init(&p, &lex);
        (void)parser_parse_chain(&p);
    });

    return 0;
}
