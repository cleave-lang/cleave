#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ast.h"

/* ============== test helpers ============== */

static StrRef sr(const char *s) {
    StrRef r;
    r.start = s;
    r.length = strlen(s);
    return r;
}

static AstNode *new_node(AstKind kind) {
    AstNode *n = calloc(1, sizeof(AstNode));
    n->kind = kind;
    n->span.start_line = 1;
    n->span.start_col = 1;
    return n;
}

static AstNode *node_type_named(const char *name) {
    AstNode *n = new_node(AST_TYPE_NAMED);
    n->as.type_named.name = sr(name);
    return n;
}

static AstNode *node_ident(const char *name) {
    AstNode *n = new_node(AST_EXPR_IDENT);
    n->as.ident.name = sr(name);
    return n;
}

static AstNode *node_number(const char *text) {
    AstNode *n = new_node(AST_EXPR_NUMBER);
    n->as.number.text = sr(text);
    return n;
}

/* Render an AST to a heap-allocated string. Caller frees. */
static char *dump_to_string(const AstNode *node) {
    char *buf = NULL;
    size_t size = 0;
    FILE *out = open_memstream(&buf, &size);
    if (!out) return NULL;
    ast_dump(node, out, 0);
    fclose(out);
    return buf;
}

/* ============== tests ============== */

TEST(test_kind_name_covers_all_kinds) {
    /* Iterate the full enum range and assert no kind returns "Unknown". */
    AstKind kinds[] = {
        AST_CHAIN_DECL, AST_MODULE_DECL, AST_PROTOCOL_DECL,
        AST_SUBSYSTEM_ASSIGN, AST_STATE_DECL, AST_GAS_DECL,
        AST_FN_DECL, AST_FN_PARAM, AST_EFFECT_DECL, AST_SLASH_ON_DECL,
        AST_TYPE_NAMED, AST_TYPE_GENERIC, AST_TYPE_PARAM,
        AST_EXPR_IDENT, AST_EXPR_NUMBER, AST_EXPR_STRING, AST_EXPR_CHAR,
        AST_EXPR_BOOL, AST_EXPR_NULL,
        AST_EXPR_BINARY, AST_EXPR_UNARY, AST_EXPR_CALL,
        AST_EXPR_INDEX, AST_EXPR_BLOCK,
        AST_STMT_LET, AST_STMT_RETURN, AST_STMT_IF, AST_STMT_EXPR,
    };
    size_t n = sizeof(kinds) / sizeof(kinds[0]);
    for (size_t i = 0; i < n; ++i) {
        tests_run++;
        const char *name = ast_kind_name(kinds[i]);
        if (strcmp(name, "Unknown") == 0) {
            FAIL_AT("kind %d returned \"Unknown\"", (int)kinds[i]);
        }
    }
}

TEST(test_dump_null_does_not_crash) {
    char *out = dump_to_string(NULL);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "<null>") != NULL);
    free(out);
}

TEST(test_dump_chain_with_one_subsystem) {
    /* chain MyChain { consensus: Tendermint } */
    AstNode *type_tend = node_type_named("Tendermint");

    AstNode *cons = new_node(AST_SUBSYSTEM_ASSIGN);
    cons->as.subsystem.key = sr("consensus");
    cons->as.subsystem.value = type_tend;

    AstNode *chain = new_node(AST_CHAIN_DECL);
    chain->as.chain.name = sr("MyChain");
    chain->as.chain.assignments = malloc(sizeof(AstNode *));
    chain->as.chain.assignments[0] = cons;
    chain->as.chain.n_assignments = 1;

    char *out = dump_to_string(chain);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "ChainDecl") != NULL);
    ASSERT(strstr(out, "MyChain") != NULL);
    ASSERT(strstr(out, "SubsystemAssign") != NULL);
    ASSERT(strstr(out, "consensus") != NULL);
    ASSERT(strstr(out, "TypeNamed") != NULL);
    ASSERT(strstr(out, "Tendermint") != NULL);
    free(out);
}

TEST(test_dump_generic_type_with_kwargs) {
    /* Tendermint<sig=BLS12_381, finality=Deterministic> */
    AstNode *sig_val = node_ident("BLS12_381");
    AstNode *sig = new_node(AST_TYPE_PARAM);
    sig->as.type_param.key = sr("sig");
    sig->as.type_param.value = sig_val;

    AstNode *fin_val = node_ident("Deterministic");
    AstNode *fin = new_node(AST_TYPE_PARAM);
    fin->as.type_param.key = sr("finality");
    fin->as.type_param.value = fin_val;

    AstNode *tg = new_node(AST_TYPE_GENERIC);
    tg->as.type_generic.name = sr("Tendermint");
    tg->as.type_generic.args = malloc(sizeof(AstNode *) * 2);
    tg->as.type_generic.args[0] = sig;
    tg->as.type_generic.args[1] = fin;
    tg->as.type_generic.n_args = 2;

    char *out = dump_to_string(tg);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "TypeGeneric") != NULL);
    ASSERT(strstr(out, "Tendermint") != NULL);
    ASSERT(strstr(out, "args: [2]") != NULL);
    ASSERT(strstr(out, "sig") != NULL);
    ASSERT(strstr(out, "BLS12_381") != NULL);
    ASSERT(strstr(out, "finality") != NULL);
    ASSERT(strstr(out, "Deterministic") != NULL);
    free(out);
}

TEST(test_dump_module_with_state_and_fn) {
    /*
     * module Counter {
     *   state count: u64
     *   fn increment() -> Result { count + 1 }
     * }
     */
    AstNode *u64_ty = node_type_named("u64");
    AstNode *state = new_node(AST_STATE_DECL);
    state->as.state.name = sr("count");
    state->as.state.type = u64_ty;

    AstNode *result_ty = node_type_named("Result");

    AstNode *count_ident = node_ident("count");
    AstNode *one = node_number("1");
    AstNode *add = new_node(AST_EXPR_BINARY);
    add->as.binary.op = sr("+");
    add->as.binary.lhs = count_ident;
    add->as.binary.rhs = one;

    AstNode *body = new_node(AST_EXPR_BLOCK);
    body->as.block.stmts = NULL;
    body->as.block.n_stmts = 0;
    body->as.block.result = add;

    AstNode *fn = new_node(AST_FN_DECL);
    fn->as.fn.name = sr("increment");
    fn->as.fn.params = NULL;
    fn->as.fn.n_params = 0;
    fn->as.fn.return_type = result_ty;
    fn->as.fn.body = body;

    AstNode *module = new_node(AST_MODULE_DECL);
    module->as.module.name = sr("Counter");
    module->as.module.items = malloc(sizeof(AstNode *) * 2);
    module->as.module.items[0] = state;
    module->as.module.items[1] = fn;
    module->as.module.n_items = 2;

    char *out = dump_to_string(module);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "ModuleDecl") != NULL);
    ASSERT(strstr(out, "Counter") != NULL);
    ASSERT(strstr(out, "StateDecl") != NULL);
    ASSERT(strstr(out, "count") != NULL);
    ASSERT(strstr(out, "u64") != NULL);
    ASSERT(strstr(out, "FnDecl") != NULL);
    ASSERT(strstr(out, "increment") != NULL);
    ASSERT(strstr(out, "Result") != NULL);
    ASSERT(strstr(out, "ExprBinary") != NULL);
    ASSERT(strstr(out, "ExprNumber") != NULL);
    free(out);
}

TEST(test_dump_records_source_span) {
    AstNode *n = node_ident("foo");
    n->span.start_line = 42;
    n->span.start_col = 13;

    char *out = dump_to_string(n);
    ASSERT(out != NULL);
    ASSERT(strstr(out, "@ 42:13") != NULL);
    free(out);
}

int main(void) {
    RUN(test_kind_name_covers_all_kinds);
    RUN(test_dump_null_does_not_crash);
    RUN(test_dump_chain_with_one_subsystem);
    RUN(test_dump_generic_type_with_kwargs);
    RUN(test_dump_module_with_state_and_fn);
    RUN(test_dump_records_source_span);
    REPORT();
}
