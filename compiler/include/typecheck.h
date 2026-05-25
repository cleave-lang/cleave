#ifndef CLEAVE_TYPECHECK_H
#define CLEAVE_TYPECHECK_H

#include <stddef.h>
#include <stdio.h>

#include "ast.h"

/*
 * Cleave type checker (issue #34).
 *
 * Goals of this initial pass:
 *
 *   - Resolve every TypeExpr in the AST to a canonical Type*.
 *   - Annotate every expression node with the Type* it produces.
 *   - Validate let bindings, state declarations, assignments, binary ops,
 *     function call arity + arg-type match, and trailing-expression /
 *     return-type compatibility.
 *   - Emit diagnostics for type errors to a configurable FILE*; carry on
 *     so the user sees more than one error per run.
 *
 * Explicit non-goals for v0:
 *
 *   - Trait / protocol satisfaction checking.
 *   - Effect typing (RFC #9 covers that surface).
 *   - Borrow / linearity tracking.
 *   - Generic constraints. Generic types are opaque heads with arg slots.
 *   - Sophisticated inference. Integer literals default to u64; identifiers
 *     of unknown type get TY_UNKNOWN and propagate permissively, which is
 *     deliberately weak so we do not spam errors on names the parser has
 *     no business knowing (constructor names, stdlib symbols, etc.).
 */

typedef enum {
    TY_UNKNOWN, /* placeholder for anything the checker cannot resolve */
    TY_UNIT,    /* the () type (no value); the result of statements    */
    TY_PRIM,    /* a built-in primitive (u64, bool, etc.)              */
    TY_FN,      /* a function type with param + return types           */
    TY_GENERIC  /* an opaque head + type args, e.g. Result<u64>        */
} TypeKind;

typedef enum {
    PRIM_U8, PRIM_U16, PRIM_U32, PRIM_U64, PRIM_U128, PRIM_U256,
    PRIM_I8, PRIM_I16, PRIM_I32, PRIM_I64,
    PRIM_BOOL,
    PRIM_STR,
    PRIM_CHAR
} PrimKind;

typedef struct Type Type;

typedef struct {
    Type **params;
    size_t n_params;
    Type *result;
} FnType;

typedef struct {
    const char *head;     /* generic head name (interned source pointer) */
    size_t head_length;
    Type **args;          /* concrete type args; may be NULL for opaque */
    size_t n_args;
} GenericType;

struct Type {
    TypeKind kind;
    union {
        PrimKind    prim;
        FnType      fn;
        GenericType generic;
    } as;
};

/* Diagnostics + context shared across the type-checking walk. */
typedef struct {
    FILE *err;       /* where diagnostics go; default stderr */
    int  n_errors;   /* count of reported errors */
} TypeChecker;

/* Initialize a type checker that writes diagnostics to the given FILE*
 * (pass stderr to mirror parser error output). Safe to reuse across
 * multiple programs. */
void typecheck_init(TypeChecker *tc, FILE *err);

/* Run the type checker over a parsed program (the synthetic module
 * returned by parser_parse_program). Returns 0 if no errors, non-zero
 * if at least one type error was reported. */
int typecheck_program(TypeChecker *tc, AstNode *program);

/* ---------- type construction / introspection ---------- */

/* Get the canonical interned Type* for a primitive. The returned pointer
 * is stable for the lifetime of the process; equality of two primitives
 * is pointer equality. */
const Type *type_prim(PrimKind k);

/* The canonical Unit and Unknown singletons. */
const Type *type_unit(void);
const Type *type_unknown(void);

/* Render a type into a static buffer for diagnostics. The buffer is
 * reused across calls; copy out if you need to keep the string. */
const char *type_describe(const Type *t);

/* Is this type `Copy`? Copy types can be used freely without losing the
 * source binding (no move semantics). Per RFC 0001:
 *   - All primitives (u8..u256, i8..i64, bool, char, str) are Copy.
 *   - Unit `()` is Copy.
 *   - Function types are Copy (the reference, not the body).
 *   - Generic-head aggregates (`Result<u64>`, `Vec<T>`, etc.) are NOT Copy
 *     by default; they move on use.
 *   - Unknown is permissively treated as Copy so the checker does not
 *     spam errors on values it cannot resolve.
 *
 * Used by the move-tracker to decide whether a binding's use marks the
 * binding as moved. Once `&T` references land, this also informs how
 * borrows compose with moves. */
int type_is_copy(const Type *t);

#endif /* CLEAVE_TYPECHECK_H */
