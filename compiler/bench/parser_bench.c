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

/* A representative module body: state, gas, fn with arithmetic and a
 * trailing call expression. Mirrors examples/counter.cv and exercises the
 * expression / statement / block productions added in issue #35. */
static const char *COUNTER_MODULE =
    "module Counter {\n"
    "    state count: u64\n"
    "    gas increment = { cpu: 100, storage: 8 }\n"
    "    fn increment() -> Result<u64> {\n"
    "        count = count + 1\n"
    "        Ok(count)\n"
    "    }\n"
    "    fn read() -> u64 { count }\n"
    "}";

/* A protocol declaration with effect signatures and slash_on rules; broader
 * coverage of the productions that landed with #35. */
static const char *PROTOCOL_BODY =
    "protocol Consensus implements Base {\n"
    "    effect propose() -> Block\n"
    "    effect vote(b: Block) -> Signature\n"
    "    effect finalize(b: Block) deferred\n"
    "    fn select_leader(epoch: u64) -> Address { Address::zero() }\n"
    "    slash_on Bad { who: Address, b: Block }\n"
    "        when valid(b)\n"
    "        penalty 100\n"
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

    BENCH("parse_counter_module", {
        Lexer lex;
        lexer_init(&lex, COUNTER_MODULE);
        Parser p;
        parser_init(&p, &lex);
        (void)parser_parse_program(&p);
    });

    BENCH("parse_protocol_body", {
        Lexer lex;
        lexer_init(&lex, PROTOCOL_BODY);
        Parser p;
        parser_init(&p, &lex);
        (void)parser_parse_program(&p);
    });

    return 0;
}
