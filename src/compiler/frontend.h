#ifndef FRONTEND_H
#define FRONTEND_H

#include "compiler.h"
#include "lexer.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    INSTR_LIMIT,
    INSTR_EMIT,
    INSTR_RESERVE,

    INSTR_NOP,
    INSTR_HALT,
    INSTR_LABEL,
    INSTR_INT,
    INSTR_CALL,
    INSTR_JMP,
    INSTR_RET,
    INSTR_RETI,

    INSTR_LOAD,
    INSTR_ADD,
    INSTR_SUB,
    INSTR_MUL,
    INSTR_DIV,
    INSTR_REM,
    INSTR_AND,
    INSTR_OR,
    INSTR_XOR,

    INSTR_SHL,
    INSTR_SHR,
    INSTR_ROL,
    INSTR_ROR,

    INSTR_CMP,
    INSTR_COUNT,
} ir_instr_kind;

typedef struct label label;
typedef struct instr ir_instr;
struct label {
    enum {
        LABEL_NAMED,
        LABEL_INDEXED,
    } type;
    union {
        char *name;
        int index;
    };

    bool is_interrupt;
    bool is_word;
    label *owner;
    token *tok;
    ir_instr *instr;
};
typedef enum {
    SIGN_NONE,
    SIGN_SIGNED,
    SIGN_UNSIGNED,
} register_sign;
typedef enum {
    EXPR_NUMBER,
    EXPR_LABEL,
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_LAYOUT,
} expr_kind;

typedef enum {
    OPERATOR_NONE,
    OPERATOR_ADD,
    OPERATOR_SUB,
    OPERATOR_MUL,
    OPERATOR_DIV,
    OPERATOR_POW,
    OPERATOR_MOD,
    OPERATOR_LSHIFT,
    OPERATOR_RSHIFT,
    OPERATOR_LT,
    OPERATOR_LE,
    OPERATOR_GT,
    OPERATOR_GE,
    OPERATOR_EQ,
    OPERATOR_NEQ,
    OPERATOR_BIT_AND,
    OPERATOR_BIT_XOR,
    OPERATOR_BIT_OR,
    OPERATOR_LOGIC_AND,
    OPERATOR_LOGIC_OR,
    OPERATOR_MIN,
    OPERATOR_MAX,
    OPERATOR_UNARY_MINUS,
    OPERATOR_BIT_NOT,
    OPERATOR_LOGIC_NOT,
    OPERATOR_SIN,
    OPERATOR_COS,
    OPERATOR_TAN,
    OPERATOR_EXP,
    OPERATOR_LOG,
    OPERATOR_ABS,
    OPERATOR_SQRT,
    OPERATOR_SIGN,
    OPERATOR_CEIL,
    OPERATOR_FLOOR,
} operator_type;

typedef enum {
    VAL_NONE,
    VAL_REG,
    VAL_EXPR,
    VAL_SP,
    VAL_FLAGS,
} value_kind;
typedef struct expr_node expr_node;
typedef struct value {
    value_kind kind;
    prim_size size;
    bool is_addr;
    union {
        struct {
            register_sign sign;
            uint8_t index;
        } reg;
        expr_node *expr;
    };
    operator_type op_type;
    struct value *operand;
} value;
typedef struct {
    token *name;
    expr_node *elements_count;
    expr_node *element_size;
} layout_field;
typedef struct {
    vector fields; // Vector of layout_field_t
    bool aligned;
} layout;

struct expr_node {
    expr_kind kind;
    token *tok;

    union {
        int64_t number;
        label *label;

        struct {
            operator_type op;
            struct expr_node *expr;
        } unary;
        struct {
            operator_type op;
            struct expr_node *lhs;
            struct expr_node *rhs;
            token *op_tok;
        } binary;
        struct {
            layout *value;
            layout_field *field;
            struct expr_node *index;
            bool is_sizeof;
        } layout;
    };
    int64_t result;
};

typedef enum {
    BRANCH_NONE,
    BRANCH_EQ,
    BRANCH_NEQ,
    BRANCH_GT_U,
    BRANCH_GT_S,
    BRANCH_GE_U,
    BRANCH_GE_S,
    BRANCH_LT_U,
    BRANCH_LT_S,
    BRANCH_LE_U,
    BRANCH_LE_S,
} branch_type;

typedef struct {
    value lhs;
    value rhs;
    token *tok; // Operator tok
} binary_instr;
struct instr {
    ir_instr_kind kind;
    uint64_t addr;
    uint32_t current_size;
    union {
        label *label;
        binary_instr bin;
        struct {
            branch_type type;
            value addr;
            token *tok;
        } branch;
        struct {
            prim_size size;
            vector values; // Vector of expr_node*
        } emit;
        struct {
            expr_node *start;
            expr_node *end;
        } limit;
        struct {
            expr_node *expr;
        } reserve;
        struct {
            token *index;
            bool value;
        } set_flag;
    };
};

typedef struct {
    token *tok;
    label *parent;
    label **dest;
} unresolved_label;

void value_free(value *value);
void layout_free(layout *layout);
void expr_free(expr_node *expr);

bool parse(arch_backend *backend, token_stream *stream, vector *shared_labels,
            hashmap *layouts, vector *deleted_layouts, vector *instrs);
void eval_value(codegen_ctx *c, value *value);
int64_t eval_expr(codegen_ctx *c, expr_node *expr);

static inline bool value_is_simple(value *value, value_kind kind) {
    return value->kind == kind && !value->is_addr && value->op_type == OPERATOR_NONE;
}
static inline bool value_is_addr(value *value, value_kind kind) {
    return value->kind == kind && value->is_addr && value->op_type == OPERATOR_NONE;
}
static inline bool value_is_number(value *value, int number) {
    return value_is_simple(value, VAL_EXPR) && value->expr->result == number;
}
static inline bool value_is_interrupt(value *value) {
    return value_is_simple(value, VAL_EXPR) &&
            value->expr->kind == EXPR_LABEL && value->expr->label &&
            value->expr->label->is_interrupt;
}

bool value_matches(const value *actual, const value *expected);
void print_value(FILE *out, const value *value, bool show_imm);
void print_values(FILE *out, const value *value1, const value *value2, bool show_imm);

#endif