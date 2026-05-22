#include <string.h>

#include "bench.h"
#include "lexer.h"

/* Drain the token stream all the way to EOF. */
static void lex_to_eof(const char *src) {
    Lexer lex;
    lexer_init(&lex, src);
    Token t;
    do {
        t = lexer_next(&lex);
    } while (t.kind != TK_EOF);
}

static const char *MINIMAL_CHAIN =
    "chain MyChain {\n"
    "    consensus: Tendermint<sig=BLS12_381, finality=Deterministic>\n"
    "    gas:       Multidim<{cpu, storage, witness}>\n"
    "    state:     SparseMerkle<key=u256, hash=Blake3>\n"
    "    exec:      WasmVM<deterministic>\n"
    "}";

static const char *KEYWORD_SOUP =
    "chain module protocol consensus gas state exec da "
    "fn effect slash_on implements when penalty "
    "let if else match return true false null "
    "->  =>  ==  !=  <=  >=  &&  ||  ::  ..";

/* Build a large synthetic input by repeating a chain template. Returns a
 * malloc'd null-terminated string; caller frees. */
static char *synth_large_module(size_t n_chains) {
    static const char block[] =
        "chain Synth {\n"
        "    consensus: Tendermint<sig=BLS12_381>\n"
        "    gas:       Multidim<{cpu, storage}>\n"
        "    state:     SparseMerkle<key=u256, hash=Blake3>\n"
        "    exec:      WasmVM<deterministic>\n"
        "}\n";
    size_t block_len = sizeof(block) - 1;
    size_t total = block_len * n_chains + 1;
    char *buf = malloc(total);
    if (!buf) return NULL;
    char *cur = buf;
    for (size_t i = 0; i < n_chains; ++i) {
        memcpy(cur, block, block_len);
        cur += block_len;
    }
    *cur = '\0';
    return buf;
}

int main(void) {
    /* baseline: tokenize the canonical minimal chain manifest */
    BENCH("lex_minimal_chain", {
        lex_to_eof(MINIMAL_CHAIN);
    });

    /* keyword + punctuation lookups, no IDENTs */
    BENCH("lex_keyword_soup", {
        lex_to_eof(KEYWORD_SOUP);
    });

    /* large synthetic input: 1000 chain blocks (~ 200KB of source) */
    char *big = synth_large_module(1000);
    if (big) {
        BENCH("lex_synthetic_1k_chains", {
            lex_to_eof(big);
        });
        free(big);
    }

    return 0;
}
