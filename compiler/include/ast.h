#ifndef CLEAVE_AST_H
#define CLEAVE_AST_H

#include <stddef.h>
#include <stdio.h>

/* A source location range. Lines and columns are 1-based, end is exclusive. */
typedef struct {
    size_t start_line;
    size_t start_col;
    size_t end_line;
    size_t end_col;
} Span;

/* A reference to a slice of source text. The pointer is not owned and must
 * outlive the AST (the source buffer is kept alive by the compiler driver). */
typedef struct {
    const char *start;
    size_t length;
} StrRef;

typedef enum {
    /* declarations */
    AST_CHAIN_DECL,        /* chain Name { ... }                              */
    AST_MODULE_DECL,       /* module Name { ... }                             */
    AST_PROTOCOL_DECL,     /* protocol Name implements Trait { ... }          */
    AST_SUBSYSTEM_ASSIGN,  /* consensus: Tendermint<...>  (inside a chain {}) */
    AST_STATE_DECL,        /* state name: Type                                */
    AST_GAS_DECL,          /* gas name = { ... }                              */
    AST_FN_DECL,           /* fn name(params) -> Return { body }              */
    AST_FN_PARAM,          /* a single `name: Type` parameter                 */
    AST_EFFECT_DECL,       /* effect name(params) -> Return                   */
    AST_SLASH_ON_DECL,     /* slash_on Evidence { ... } when ... penalty ...  */

    /* types */
    AST_TYPE_NAMED,        /* SimpleName                                       */
    AST_TYPE_GENERIC,      /* Name<arg, key=value, ...>                        */
    AST_TYPE_PARAM,        /* an entry inside <...>: either `value` or `k=v`   */

    /* expressions */
    AST_EXPR_IDENT,        /* foo                                              */
    AST_EXPR_NUMBER,       /* 42, 0x1A, 0b101                                  */
    AST_EXPR_STRING,       /* "text"                                           */
    AST_EXPR_CHAR,         /* 'a'                                              */
    AST_EXPR_BOOL,         /* true, false                                      */
    AST_EXPR_NULL,         /* null                                             */
    AST_EXPR_BINARY,       /* a + b, a == b, a.b, a::b                         */
    AST_EXPR_UNARY,        /* !a, -a                                           */
    AST_EXPR_CALL,         /* f(args)                                          */
    AST_EXPR_INDEX,        /* a[b]                                             */
    AST_EXPR_BLOCK,        /* { stmts; ... }                                   */

    /* statements */
    AST_STMT_LET,          /* let name = expr                                  */
    AST_STMT_RETURN,       /* return expr                                      */
    AST_STMT_IF,           /* if cond { ... } else { ... }                     */
    AST_STMT_EXPR          /* an expression used as a statement                */
} AstKind;

typedef struct AstNode AstNode;

/* ============== per-kind payloads ============== */

typedef struct {
    StrRef name;
    AstNode **assignments; /* each is AST_SUBSYSTEM_ASSIGN */
    size_t n_assignments;
} ChainDecl;

typedef struct {
    StrRef name;
    AstNode **items; /* mixed: state, gas, fn, etc. */
    size_t n_items;
} ModuleDecl;

typedef struct {
    StrRef name;
    AstNode *implements; /* optional TypeExpr; NULL if absent */
    AstNode **items;
    size_t n_items;
} ProtocolDecl;

typedef struct {
    StrRef key;     /* "consensus", "gas", "state", "exec", "da" */
    AstNode *value; /* a TypeExpr */
} SubsystemAssign;

typedef struct {
    StrRef name;
    AstNode *type; /* TypeExpr */
} StateDecl;

typedef struct {
    StrRef name;
    AstNode *value; /* expression, typically a record literal */
} GasDecl;

typedef struct {
    StrRef name;
    AstNode *type; /* TypeExpr */
} FnParam;

typedef struct {
    StrRef name;
    AstNode **params; /* each is AST_FN_PARAM */
    size_t n_params;
    AstNode *return_type; /* TypeExpr; NULL if absent */
    AstNode *body;        /* AST_EXPR_BLOCK */
} FnDecl;

typedef struct {
    StrRef name;
    AstNode **params; /* each is AST_FN_PARAM */
    size_t n_params;
    AstNode *return_type; /* TypeExpr; NULL if absent */
} EffectDecl;

typedef struct {
    StrRef evidence_name;
    AstNode **bindings;       /* each is AST_FN_PARAM (re-used: name + type) */
    size_t n_bindings;
    AstNode *when_clause;     /* expression; NULL if absent */
    AstNode *penalty_clause;  /* expression; NULL if absent */
} SlashOnDecl;

typedef struct {
    StrRef name;
} TypeNamed;

typedef struct {
    StrRef name;
    AstNode **args; /* each is AST_TYPE_PARAM */
    size_t n_args;
} TypeGeneric;

/* A single entry inside <...>: either a positional value (key empty) or a
 * keyword `key=value` pair. `value` is an expression because type parameters
 * can be types, idents, or literals depending on context. */
typedef struct {
    StrRef key;       /* empty for positional */
    AstNode *value;   /* expression or type */
} TypeParam;

typedef struct {
    StrRef name;
} ExprIdent;

typedef struct {
    StrRef text; /* raw lexeme including base prefix, e.g. "0x42" */
} ExprNumber;

typedef struct {
    StrRef text; /* raw, includes the surrounding quotes and escapes */
} ExprString;

typedef struct {
    StrRef text; /* raw, includes the surrounding single quotes */
} ExprChar;

typedef struct {
    int value; /* 0 or 1 */
} ExprBool;

typedef struct {
    AstNode *lhs;
    StrRef op;   /* operator lexeme: "+", "==", ".", "::", etc. */
    AstNode *rhs;
} ExprBinary;

typedef struct {
    StrRef op;
    AstNode *operand;
} ExprUnary;

typedef struct {
    AstNode *callee;
    AstNode **args; /* each is an expression */
    size_t n_args;
} ExprCall;

typedef struct {
    AstNode *array;
    AstNode *index;
} ExprIndex;

typedef struct {
    AstNode **stmts; /* statements before the trailing expression */
    size_t n_stmts;
    AstNode *result; /* optional trailing expression; NULL if absent */
} ExprBlock;

typedef struct {
    StrRef name;
    AstNode *type;  /* optional TypeExpr; NULL if inferred */
    AstNode *value; /* expression */
} StmtLet;

typedef struct {
    AstNode *value; /* expression; NULL for bare `return` */
} StmtReturn;

typedef struct {
    AstNode *cond;
    AstNode *then_branch; /* AST_EXPR_BLOCK */
    AstNode *else_branch; /* AST_EXPR_BLOCK or AST_STMT_IF, may be NULL */
} StmtIf;

typedef struct {
    AstNode *expr;
} StmtExpr;

/* ============== the tagged union ============== */

struct AstNode {
    AstKind kind;
    Span span;
    union {
        ChainDecl       chain;
        ModuleDecl      module;
        ProtocolDecl    protocol;
        SubsystemAssign subsystem;
        StateDecl       state;
        GasDecl         gas;
        FnDecl          fn;
        FnParam         fn_param;
        EffectDecl      effect;
        SlashOnDecl     slash_on;
        TypeNamed       type_named;
        TypeGeneric     type_generic;
        TypeParam       type_param;
        ExprIdent       ident;
        ExprNumber      number;
        ExprString      string;
        ExprChar        character;
        ExprBool        boolean;
        ExprBinary      binary;
        ExprUnary       unary;
        ExprCall        call;
        ExprIndex       index;
        ExprBlock       block;
        StmtLet         let_stmt;
        StmtReturn      ret_stmt;
        StmtIf          if_stmt;
        StmtExpr        expr_stmt;
    } as;
};

/* ============== public API ============== */

const char *ast_kind_name(AstKind kind);
void ast_dump(const AstNode *node, FILE *out, int indent);

#endif /* CLEAVE_AST_H */
