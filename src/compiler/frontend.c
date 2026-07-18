#include "lexer.h"
#include "compiler.h"
#include "frontend.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

typedef struct {
    prim_size size;
    register_sign sign;
} register_type;
typedef struct {
    register_type type;
    bool used;
} register_state;
typedef struct {
    arch_backend *backend;
    token_stream *stream;
    register_state *register_states;
    label *current_label;
    hashmap *layouts;
    vector *shared_labels; // Vector of label*
    vector *deleted_layouts;
    vector *instrs;

    uint64_t label_count;
    enum { PARSE_NONE, PARSE_ROUTINE, PARSE_LABEL } parse_mode;
    vector unresolved_labels; // Vector of unresolved_label*
} parser;

static inline ir_instr *instr_new(ir_instr_kind kind, parser *p) {
    ir_instr *instr = my_malloc(sizeof *instr);
    *instr = (ir_instr){kind};
    return instr;
}
static expr_node *expr_new(expr_kind kind) {
    expr_node *n = my_malloc(sizeof(*n));
    n->kind = kind;
    return n;
}
static ir_instr *label_jump_new(label *label, branch_type type, parser *p) {
    ir_instr *instr = instr_new(INSTR_JMP, p);
    instr->branch.type = type;
    instr->branch.addr = (value){.kind = VAL_EXPR, .expr = expr_new(EXPR_LABEL)};
    instr->branch.addr.expr->label = label;
    return instr;
}
static ir_instr *indexed_label_new(parser *p) {
    ir_instr *instr = instr_new(INSTR_LABEL, p);
    instr->label = my_malloc(sizeof *instr->label);
    instr->label->index = p->label_count++;
    instr->label->type = LABEL_INDEXED;
    instr->label->instr = instr;
    return instr;
}

static inline void unexpected_token(parser *p) {
    token *tok = current_token(p->stream);
    const char *kind = "tok";
    if (tok->type == TOK_IDENT)    kind = "identifier";
    else if (tok->type == TOK_NUM) kind = "number";
    else if (tok->type >= TOK_KEYWORD_START && tok->type < TOK_KEYWORD_END)
        kind = "keyword";
    else if (tok->type >= TOK_OPERATOR_START && tok->type < TOK_OPERATOR_END)
        kind = "operator";
    else if (tok->type >= TOK_SEPARATOR_START && tok->type < TOK_SEPARATOR_END)
        kind = "separator";
    else if (tok->type >= TOK_SYMBOL_START && tok->type < TOK_SYMBOL_END)
        kind = "symbol";
    else if (tok->type >= TOK_SPECIAL_START && tok->type < TOK_SPECIAL_END)
        kind = "special";
    token *prev = prev_token(p->stream);
    ERROR(p->stream, "unexpected %s after '%.*s'", kind, prev->len, prev->start);
}

bool token_is_register(uint8_t *index, token *tok, parser *p) {
    if (tok->type != TOK_IDENT) return false;
    for (uint8_t i = 0; i < p->backend->register_count; i++) {
        register_state *reg = &p->register_states[i];
        char reg_name[5];
        snprintf(reg_name, ARRAYLEN(reg_name), "r%u", i);

        if (token_is_str(tok, reg_name)) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}
bool read_register(uint8_t *index, bool *is_sp, parser *p) {
    if (match_token(TOK_SP, p->stream)) {
        if (is_sp) {
            *is_sp = true;
            return true;
        }
        EXPECT_PREV(p->stream, "sp is a special register, expected rN format");
        return false;
    }

    if (token_is_register(index, current_token(p->stream), p)) {
        advance_token(p->stream);
        return true;
    }
    return false;
}

static prim_size size_from_string(char *str, size_t strlen) {
    if (strlen == 0) return SIZE_NONE;
    if (strlen == 1 && str[0] == '8') return SIZE_BYTE;
    if (!strncmp(str, "16", strlen)) return SIZE_HWORD;
    if (!strncmp(str, "32", strlen)) return SIZE_WORD;
    if (!strncmp(str, "64", strlen)) return SIZE_DWORD;
    return -1;
}

static token *parse_type(register_type *type, parser *p) {
    token *type_tok = current_token(p->stream);
    if (type_tok->type != TOK_IDENT && type_tok->type != TOK_NUM) return NULL;

    register_sign sign = (*type_tok->start == 's') ? SIGN_SIGNED :
            (*type_tok->start == 'u') ? SIGN_UNSIGNED : SIGN_NONE;
    bool has_prefix = sign != SIGN_NONE;
    prim_size size =
        size_from_string(type_tok->start + has_prefix, type_tok->len - has_prefix);
    if (size == -1) return NULL;

    *type = (register_type){size, sign};
    advance_token(p->stream);
    return type_tok;
}

static inline void reset_registers(parser *p) {
    for (uint8_t i = 0; i < p->backend->register_count; i++) {
        p->register_states[i].type = (register_type){.sign = SIGN_NONE, .size = SIZE_NONE};
        p->register_states[i].used = false;
    }
}
static bool parse_use(parser *p) {
    bool is_unuse = match_token(TOK_UNUSE, p->stream);
    if (!is_unuse && !match_token(TOK_USE, p->stream)) return false;
    if (p->parse_mode == PARSE_NONE) {
        ERROR_AT(prev_token(p->stream), p->stream, "'use'/'unuse' can only be used under a label");
        return true;
    }

    p->current_label->is_word = true;
    p->parse_mode = PARSE_ROUTINE;
    if (match_token(TOK_ALL, p->stream)) {
        if (is_unuse) {
            p->parse_mode = PARSE_NONE;
            reset_registers(p);
            return true;
        }
        register_type type = {SIZE_NONE, SIGN_NONE};
        parse_type(&type, p);
        for (int i = 0; i < p->backend->register_count; i++) {
            p->register_states[i].used = true;
            p->register_states[i].type = type;
        }
        return true;
    }

    for (;;) {
        uint8_t reg = 0;
        if (!read_register(&reg, NULL, p)) {
            token *tok = current_token(p->stream);
            ERROR_AT(tok, p->stream, "'%.*s' is not a register", tok->len, tok->start);
            return true;
        }
        p->register_states[reg].used = !is_unuse;
        if (!is_unuse) parse_type(&p->register_states[reg].type, p);
        if (!match_token(TOK_COMMA, p->stream)) break;
    }
    return true;
}

static inline bool consume_expression(expr_node **out, parser *p);
static inline layout_field *check_layout_index(int64_t index, token *field_tok, token *layoutok, layout *layout, parser *p) {
    if (index < layout->fields.count) return vector_get(&layout->fields, index);
    ERROR_AT(field_tok, p->stream,
        "field index '%.*s' out of bounds for layout '%.*s' (%d field available)",
            field_tok->len, field_tok->start, layoutok->len, layoutok->start,
            layout->fields.count);
    return NULL;
}
static layout_field *find_layout_field(parser *p, token *field_tok, token *layoutok, layout *layout) {
    if (!field_tok) return NULL;
    if (field_tok->type == TOK_NUM)
        return check_layout_index(field_tok->number, field_tok, layoutok, layout, p);
    for (int i = 0; i < layout->fields.count; i++) {
        layout_field *field = vector_get(&layout->fields, i);
        if (tokens_equal(field->name, field_tok)) return field;
    }
    ERROR_AT(field_tok, p->stream,
        "no field named '%.*s' in layout '%.*s'",
        field_tok->len, field_tok->start,
        layoutok->len, layoutok->start);
    return NULL;
}
static bool parse_expr(expr_node **out, int min_prec, parser *p);
static inline bool resolve_expression(expr_node **out, parser *p) {
    return parse_expr(out, 0, p);
}
static bool read_layout_value(expr_node **out, parser *p) {
    token *sizeof_tok = match_token(TOK_SIZEOF, p->stream);

    token *tok = current_token(p->stream);
    if (tok->type != TOK_IDENT) {
        if (sizeof_tok) EXPECT(p->stream, "expected layout name after 'sizeof'");
        return false;
    }
    layout *layout = hashmap_get(p->layouts, tok->start, tok->len);
    if (!layout) {
        if (sizeof_tok)
            ERROR_AT(tok, p->stream, "'%.*s' is not a valid layout name", tok->len, tok->start);
        return false;
    }
    advance_token(p->stream);

    token *field_tok = NULL;
    if (match_token(TOK_COMMERCIAL_AT, p->stream)) {
        field_tok = current_token(p->stream);
        if (field_tok->type != TOK_IDENT && field_tok->type != TOK_NUM) {
            EXPECT_PREV(p->stream, "expected field name or index");
            return true;
        }
        advance_token(p->stream);
    }
    expr_node *n = expr_new(EXPR_LAYOUT);
    n->layout.field = find_layout_field(p, field_tok, tok, layout);
    n->layout.is_sizeof = sizeof_tok;
    n->layout.value = layout;
    n->layout.index = NULL;
    n->tok = tok;
    *out = n;

    if (match_token(TOK_LSQUARE, p->stream)) {
        if (!resolve_expression(&n->layout.index, p)) {
            p->stream->pos--;
            return true;
        }
        if (field_tok) {
            consume_token(TOK_RSQUARE, "expected ']'", p->stream);
            return true;
        }

        ERROR_AT(prev_token(p->stream), p->stream, "indexing can only be used with a layout field");
    }
    return true;
}
static bool parse_factor(expr_node **out, parser *p) {
    if (match_token(TOK_LPAREN, p->stream))
        return parse_expr(out, 0, p) && consume_token(TOK_RPAREN, "expected ')'", p->stream);
    token *num_tok = match_token(TOK_NUM, p->stream);
    if (num_tok) {
        *out = expr_new(EXPR_NUMBER);
        (*out)->number = num_tok->number;
        (*out)->tok = num_tok;
        return true;
    }
    if (read_layout_value(out, p)) return true;
    if (next_token(p->stream)->type == TOK_COMMERCIAL_AT) {
        token *layout_name = current_token(p->stream);
        ERROR_AT(layout_name, p->stream, "layout '%.*s' is undefined", layout_name->len, layout_name->start);
        return false;
    }
    token *label_name = current_token(p->stream);
    if (label_name->type == TOK_IDENT) {
        if (token_is_register(NULL, label_name, p)) return false;
        advance_token(p->stream);

        *out = expr_new(EXPR_LABEL);
        (*out)->tok = label_name;
        for (int i = 0; i < p->shared_labels->count; i++) {
            label *label = vector_get(p->shared_labels, i);
            if (label->type == LABEL_NAMED && token_is_str(label_name, label->name)) {
                (*out)->label = label;
                return true;
            }
        }
        unresolved_label *unresolved_lb = my_malloc(sizeof *unresolved_lb);
        *unresolved_lb = (unresolved_label){
            .dest = &(*out)->label, .parent = p->current_label, .tok = label_name
        };
        vector_add(&p->unresolved_labels, unresolved_lb);
        return true;
    }
    return false;
}
static bool parse_unary(expr_node **out, parser *p) {
    *out = NULL;
    token *tok = current_token(p->stream);
    operator_type op = OPERATOR_NONE;
    switch (tok->type) {
        case TOK_MINUS: op = OPERATOR_UNARY_MINUS; break;
        case TOK_TILDE: op = OPERATOR_BIT_NOT; break;
        case TOK_BANG:  op = OPERATOR_LOGIC_NOT; break;
        default:
            if (token_is_str(tok, "sin")) op = OPERATOR_SIN;
            else if (token_is_str(tok, "cos")) op = OPERATOR_COS;
            else if (token_is_str(tok, "tan")) op = OPERATOR_TAN;
            else if (token_is_str(tok, "exp")) op = OPERATOR_EXP;
            else if (token_is_str(tok, "log")) op = OPERATOR_LOG;
            else if (token_is_str(tok, "abs")) op = OPERATOR_ABS;
            else if (token_is_str(tok, "sqrt")) op = OPERATOR_SQRT;
            else if (token_is_str(tok, "sign")) op = OPERATOR_SIGN;
            else if (token_is_str(tok, "ceil")) op = OPERATOR_CEIL;
            else if (token_is_str(tok, "floor")) op = OPERATOR_FLOOR;
            else return parse_factor(out, p);
    }
    advance_token(p->stream);
    expr_node *expr;
    if (!parse_unary(&expr, p)) return false;
    *out = expr_new(EXPR_UNARY);
    (*out)->unary.expr = expr;
    (*out)->unary.op = op;
    return true;
}
static int precedence(operator_type t) {
    switch (t) {
        case OPERATOR_POW: return 12;
        case OPERATOR_MUL:
        case OPERATOR_DIV:
        case OPERATOR_MOD: return 11;
        case OPERATOR_ADD:
        case OPERATOR_SUB: return 10;
        case OPERATOR_LSHIFT:
        case OPERATOR_RSHIFT: return 9;
        case OPERATOR_LT:
        case OPERATOR_LE:
        case OPERATOR_GT:
        case OPERATOR_GE: return 8;
        case OPERATOR_EQ:
        case OPERATOR_NEQ: return 7;
        case OPERATOR_BIT_AND: return 6;
        case OPERATOR_BIT_XOR: return 5;
        case OPERATOR_BIT_OR: return 4;
        case OPERATOR_LOGIC_AND: return 3;
        case OPERATOR_LOGIC_OR: return 2;
        case OPERATOR_MIN:
        case OPERATOR_MAX: return 1;
        default: return -1;
    }
}
static inline operator_type token_to_basic_binop(token *tok) {
    switch (tok->type) {
    case TOK_PLUS:  return OPERATOR_ADD;
    case TOK_MINUS: return OPERATOR_SUB;
    case TOK_STAR:  return OPERATOR_MUL;
    case TOK_SLASH: return OPERATOR_DIV;
    case TOK_AND:   return OPERATOR_BIT_AND;
    case TOK_OR:    return OPERATOR_BIT_OR;
    case TOK_XOR:   return OPERATOR_BIT_XOR;
    case TOK_SHL:   return OPERATOR_LSHIFT;
    case TOK_SHR:   return OPERATOR_RSHIFT;
    default:        return OPERATOR_NONE;
    }
}
static inline operator_type token_to_binop(token *tok) {
    operator_type op = token_to_basic_binop(tok);
    if (op != OPERATOR_NONE) return op;
    switch (tok->type) {
    case TOK_LESS:          return OPERATOR_LT;
    case TOK_LESS_EQUAL:    return OPERATOR_LE;
    case TOK_EQUAL_EQUAL:   return OPERATOR_EQ;
    case TOK_BANG_EQUAL:    return OPERATOR_NEQ;
    case TOK_GREATER:       return OPERATOR_GT;
    case TOK_GREATER_EQUAL: return OPERATOR_GE;
    case TOK_AND_AND:       return OPERATOR_LOGIC_AND;
    case TOK_OR_OR:         return OPERATOR_LOGIC_OR;
    default:
        if (token_is_str(tok, "pow")) return OPERATOR_POW;
        if (token_is_str(tok, "min")) return OPERATOR_MIN;
        if (token_is_str(tok, "max")) return OPERATOR_MAX;
        if (token_is_str(tok, "mod")) return OPERATOR_MOD;
        return OPERATOR_NONE;
    }
}
static bool parse_expr(expr_node **out, int min_prec, parser *p) {
    if (!parse_unary(out, p)) return false;
    for (;;) {
        token *op_tok = current_token(p->stream);
        operator_type op = token_to_binop(op_tok);

        int prec = precedence(op);
        if (prec < min_prec) return true;
        advance_token(p->stream);

        expr_node *rhs = NULL;
        if (!parse_expr(&rhs, prec + 1, p)) return false;

        expr_node *lhs = *out;
        *out = expr_new(EXPR_BINARY);
        (*out)->binary.lhs = lhs;
        (*out)->binary.rhs = rhs;
        (*out)->binary.op = op;
    }
}
static inline bool consume_expression(expr_node **out, parser *p) {
    if (resolve_expression(out, p)) return true;
    expr_free(*out);
    *out = NULL;
    token *currtok = current_token(p->stream);
    EXPECT_PREV(p->stream, "expected expression before '%.*s'", currtok->len, currtok->start);
    return false;
}

static inline bool consume_value(value *val, parser *p);
static inline ir_instr *instr_incdec(value *val, bool is_dec, token *tok, parser *p) {
    ir_instr *instr = instr_new(is_dec ? INSTR_SUB : INSTR_ADD, p);
    instr->bin.lhs = *val;
    instr->bin.lhs.is_addr = false;
    instr->bin.rhs = (value){
        .kind = VAL_EXPR, .is_addr = false, .size = SIZE_BYTE,
        .expr = expr_new(EXPR_NUMBER)
    };
    instr->bin.rhs.expr->number = 1;
    instr->bin.tok = tok;
    return instr;
}
static bool parse_raw_value(value *val, parser *p) {
    val->op_type = OPERATOR_NONE;
    val->operand = NULL;
    bool is_sp = false;
    if (match_token(TOK_FLAGS, p->stream)) {
        val->kind = VAL_FLAGS;
        return true;
    }
    if (read_register(&val->reg.index, &is_sp, p)) {
        if (!is_sp) {
            if (!p->register_states[val->reg.index].used) {
                ERROR_AT(prev_token(p->stream), p->stream,
                    "'r%d' is missing a declaration", val->reg.index);
                return true;
            }

            register_type t = p->register_states[val->reg.index].type;
            val->kind = VAL_REG;
            val->reg.sign = t.sign;
            val->size = t.size;
        } else val->kind = VAL_SP;
        return true;
    }
    if (resolve_expression(&val->expr, p)) {
        val->kind = VAL_EXPR;
        return true;
    }
    if (val->is_addr) EXPECT(p->stream, "expected an address expression after '['");
    return false;
}
static bool parse_value(value *val, parser *p) {
    val->is_addr = match_token(TOK_LSQUARE, p->stream);
    if (!parse_raw_value(val, p)) return false;

    if (match_token(TOK_AS, p->stream)) {
        if (val->kind != VAL_REG) {
            ERROR_AT(prev_token(p->stream), p->stream,
                "'as' can only be used with registers", p->stream);
            return true;
        }
        register_type type = {SIZE_NONE, SIGN_NONE};
        if (!parse_type(&type, p)) {
            EXPECT_PREV(p->stream, "expected type after 'as'", p->stream);
            return true;
        }
        val->reg.sign = type.sign;
        val->size = type.size;
    }
    token *tok = current_token(p->stream);
    operator_type op = token_to_basic_binop(tok);
    if (op != OPERATOR_NONE) {
        advance_token(p->stream);
        val->operand = my_malloc(sizeof *val->operand);
        val->op_type = op;
        consume_value(val->operand, p);
    }
    if (val->is_addr && !match_token(TOK_RSQUARE, p->stream))
        EXPECT(p->stream, "expected ']'", p->stream);
    return true;
}
static inline bool consume_value(value *val, parser *p) {
    if (parse_value(val, p)) return true;
    unexpected_token(p);
    return false;
}
static void parse_instr(parser *p);
static inline bool report_unclosed_block(parser *p, token *start_tok) {
    if (p->stream->error_occured) return true;
    bool result = at_end(p->stream);
    if (result) {
        ERROR(p->stream, "%s", "unclosed block");
        report(start_tok, DIAGNOSTIC_NOTE, false, p->stream, "block starts here");
    }
    return result;
}
static bool parse_if(parser *p) {
    token *if_tok = match_token(TOK_IF, p->stream);
    if (!if_tok) return false;
    if (p->parse_mode != PARSE_ROUTINE) {
        ERROR_AT(prev_token(p->stream), p->stream, "'if' can only be used under routine");
        return true;
    }

    ir_instr *cmp_instr = instr_new(INSTR_CMP, p);
    consume_value(&cmp_instr->bin.lhs, p);

    token *value_tok = prev_token(p->stream);
    token *operator_tok = take_token(p->stream);

    cmp_instr->bin.tok = operator_tok;
    vector_add(p->instrs, cmp_instr);

    branch_type btype = BRANCH_NONE;
    bool is_signed = cmp_instr->bin.lhs.reg.sign == SIGN_SIGNED;
    switch (operator_tok->type) {
    case TOK_EQUAL_EQUAL:   btype = BRANCH_EQ;  break;
    case TOK_BANG_EQUAL:    btype = BRANCH_NEQ; break;
    case TOK_GREATER:       btype = is_signed ? BRANCH_GT_S : BRANCH_GT_U; break;
    case TOK_GREATER_EQUAL: btype = is_signed ? BRANCH_GE_S : BRANCH_GE_U; break;
    case TOK_LESS:          btype = is_signed ? BRANCH_LT_S : BRANCH_LT_U; break;
    case TOK_LESS_EQUAL:    btype = is_signed ? BRANCH_LE_S : BRANCH_LE_U; break;
    default: EXPECT_AT(value_tok, p->stream,
                "expected relational operator ('<', '>', '<=', '>=', '==', '!=')");
    }

    consume_value(&cmp_instr->bin.rhs, p);
    if (value_is_simple(&cmp_instr->bin.lhs, VAL_REG) &&
        value_is_simple(&cmp_instr->bin.rhs, VAL_REG) &&
        cmp_instr->bin.lhs.reg.sign != cmp_instr->bin.rhs.reg.sign)
        ERROR_AT(operator_tok, p->stream, "comparison between different registers sign");

    consume_token(TOK_LBRACE, "expected '{'", p->stream);
    token *branch_tok = current_token(p->stream);
    bool is_jmp = branch_tok->type == TOK_JMP;
    if (is_jmp || branch_tok->type == TOK_CALL) {
        advance_token(p->stream);

        // if operand == operand jmp/call operand
        ir_instr *jmp_instr = instr_new(is_jmp ? INSTR_JMP : INSTR_CALL, p);
        parse_value(&jmp_instr->branch.addr, p);
        jmp_instr->branch.type = btype;
        jmp_instr->branch.tok = branch_tok;

        vector_add(p->instrs, jmp_instr);
        consume_token(TOK_RBRACE, "expected '}' in if-jmp statement", p->stream);
        return true;
    }

    const branch_type inverted_branches[] = {
        [BRANCH_NONE] = BRANCH_NONE, [BRANCH_EQ]   = BRANCH_NEQ,  [BRANCH_NEQ]  = BRANCH_EQ,
        [BRANCH_GE_U] = BRANCH_LT_U, [BRANCH_LE_U] = BRANCH_GT_U, [BRANCH_GT_U] = BRANCH_LE_U,
        [BRANCH_LT_U] = BRANCH_GE_U, [BRANCH_GE_S] = BRANCH_LT_S, [BRANCH_LE_S] = BRANCH_GT_S,
        [BRANCH_GT_S] = BRANCH_LE_S, [BRANCH_LT_S] = BRANCH_GE_S,
    };
    ir_instr *else_label = indexed_label_new(p);
    vector_add(p->instrs, label_jump_new(else_label->label, inverted_branches[btype], p));
    // Jcond else
    // A
    // jmp end
    // else:
    // B
    // end:
    while (!match_token(TOK_RBRACE, p->stream) && !report_unclosed_block(p, if_tok)) {
        parse_instr(p);
    }

    token *else_tok = match_token(TOK_ELSE, p->stream);
    if (else_tok) {
        consume_token(TOK_LBRACE, "expected '{'", p->stream);

        // JMP end
        ir_instr *end_label = indexed_label_new(p);
        vector_add(p->instrs, label_jump_new(end_label->label, BRANCH_NONE, p));

        // Else label
        vector_add(p->instrs, else_label);

        while (!match_token(TOK_RBRACE, p->stream) && !report_unclosed_block(p, else_tok)) {
            parse_instr(p);
        }

        // End label
        vector_add(p->instrs, end_label);
        return true;
    }

    vector_add(p->instrs, else_label);
    return true;
}
static bool parse_labeldef(parser *p) {
    bool is_interrupt = match_token(TOK_INT, p->stream) != NULL;
    if (!is_interrupt && !match_token(TOK_COLON, p->stream)) return false;

    token *lb_tok = consume_token(TOK_IDENT, "expected label name", p->stream);
    if (!lb_tok) return true;

    // Check if label already exists
    char *lb_name = string_duplicate(lb_tok->start, lb_tok->len);
    for (int i = 0; i < p->shared_labels->count; i++) {
        label *defined_label = vector_get(p->shared_labels, i);

        // The current label is undefined label owner
        if (defined_label->owner != NULL && defined_label->owner != p->current_label) continue;

        if (strcmp(lb_name, defined_label->name) == 0) {
            ERROR_AT(lb_tok, p->stream,
                "label '%.*s' is already defined", lb_tok->len, lb_tok->start);
            report(defined_label->tok, DIAGNOSTIC_NOTE, true,
                    p->stream, "previous definition is here");
            my_free(lb_name);
            return true;
        }
    }

    // Add instruction
    ir_instr *instr = instr_new(INSTR_LABEL, p);
    instr->label = my_malloc(sizeof *instr->label);
    *instr->label = (label) {
        .type = LABEL_NAMED, .name = lb_name,
        .tok = lb_tok, .is_word = false,
        .is_interrupt = is_interrupt, .instr = instr
    };
    switch (p->parse_mode) {
    case PARSE_NONE: p->parse_mode = PARSE_LABEL;
    case PARSE_LABEL:
        p->current_label = instr->label;
        instr->label->owner = NULL;
        break;
    case PARSE_ROUTINE: instr->label->owner = p->current_label; break;
    }
    vector_add(p->shared_labels, instr->label);
    vector_add(p->instrs, instr);
    return true;
}
static bool parse_return(parser *p) {
    bool is_semicolon = match_token(TOK_SEMICOLON, p->stream);
    if (!is_semicolon && !match_token(TOK_RETURN, p->stream)) return false;
    if (p->parse_mode == PARSE_NONE) {
        ERROR_AT(prev_token(p->stream), p->stream,
            "return can only be used under a routine or a label");
        return true;
    }

    ir_instr *instr = instr_new(p->current_label->is_interrupt ? INSTR_RETI : INSTR_RET, p);
    vector_add(p->instrs, instr);

    if (is_semicolon) {
        reset_registers(p);
        p->current_label = NULL;
        p->parse_mode = PARSE_NONE;
    }
    return true;
}

static bool parse_binary(parser *p) {
    value lhs = {0};
    if (!parse_value(&lhs, p)) return false;

    token *value_tok = prev_token(p->stream);
    token *operator_tok = take_token(p->stream);
    ir_instr_kind kind = INSTR_LOAD;
    switch (operator_tok->type) {
    case TOK_EQUAL:         break;
    case TOK_PLUS_EQUAL:    kind = INSTR_ADD; break;
    case TOK_MINUS_EQUAL:   kind = INSTR_SUB; break;
    case TOK_STAR_EQUAL:    kind = INSTR_MUL; break;
    case TOK_SLASH_EQUAL:   kind = INSTR_DIV; break;
    case TOK_AND_EQUAL:     kind = INSTR_AND; break;
    case TOK_OR_EQUAL:      kind = INSTR_OR;  break;
    case TOK_XOR_EQUAL:     kind = INSTR_XOR; break;
    case TOK_SHL_EQUAL:     kind = INSTR_SHL; break;
    case TOK_SHR_EQUAL:     kind = INSTR_SHR; break;
    case TOK_ROL_EQUAL:     kind = INSTR_ROL; break;
    case TOK_ROR_EQUAL:     kind = INSTR_ROR; break;
    case TOK_PERCENT_EQUAL: kind = INSTR_REM; break;
    default:
        EXPECT_AT(value_tok, p->stream,
            "unrecognized operator after '%.*s'", value_tok->len, value_tok->start);
        value_free(&lhs);
        return true;
    }
    if (p->parse_mode != PARSE_ROUTINE) {
        ERROR_AT(operator_tok, p->stream, "binary operations can only be used under a routine");
        value_free(&lhs);
        return true;
    }

    value rhs = {0};
    consume_value(&rhs, p);

    ir_instr *instr = instr_new(kind, p);
    instr->bin.lhs = lhs;
    instr->bin.rhs = rhs;
    instr->bin.tok = operator_tok;

    vector_add(p->instrs, instr);
    return true;
}

static bool parse_branch(parser *p) {
    token *branch_tok = current_token(p->stream);
    bool is_jmp = branch_tok->type == TOK_JMP;
    if (!is_jmp && branch_tok->type != TOK_CALL) return false;

    advance_token(p->stream);

    ir_instr *instr = instr_new(is_jmp ? INSTR_JMP : INSTR_CALL, p);
    instr->branch.type = BRANCH_NONE;
    instr->branch.tok = branch_tok;
    vector_add(p->instrs, instr);

    return consume_value(&instr->branch.addr, p);
}

static inline token *consume_label(char **name, parser *p) {
    token *lb_tok = take_token(p->stream);
    if (lb_tok->type != TOK_IDENT) {
        ERROR_AT(lb_tok,  p->stream, "'%.*s' is not a valid label name", lb_tok->len, lb_tok->start);
        return NULL;
    }
    *name = string_duplicate(lb_tok->start, lb_tok->len);
    return lb_tok;
}
void layout_free(layout *layout) {
    for (int i = 0; i < layout->fields.count; i++) {
        layout_field *field = vector_get(&layout->fields, i);
        expr_free(field->element_size);
        expr_free(field->elements_count);
        my_free(field);
    }
    vector_free(&layout->fields);
    my_free(layout);
}
static bool parse_data_directive(parser *p) {
    token *tok = current_token(p->stream);
    if (tok->start[0] != 'd') return false;

    prim_size size = size_from_string(tok->start + 1, tok->len - 1);
    if (size == SIZE_NONE || size == -1) return false;

    advance_token(p->stream);

    ir_instr *instr = instr_new(INSTR_EMIT, p);
    instr->emit.size = size;
    vector_init(&instr->emit.values);

    for (;;) {
        expr_node *expr = NULL;
        consume_expression(&expr, p);
        vector_add(&instr->emit.values, expr);
        if (!match_token(TOK_COMMA, p->stream)) break;
    }

    vector_add(p->instrs, instr);
    return true;
}

static void parse_instr(parser *p) {
    if (match_token(TOK_LIMIT, p->stream)) {
        ir_instr *instr = instr_new(INSTR_LIMIT, p);
        consume_expression(&instr->limit.start, p);
        consume_expression(&instr->limit.end, p);
        vector_add(p->instrs, instr);
        return;
    }
    if (match_token(TOK_RESERVE, p->stream)) {
        ir_instr *instr = instr_new(INSTR_RESERVE, p);
        consume_expression(&instr->reserve.expr, p);
        vector_add(p->instrs, instr);
        return;
    }
    if (match_token(TOK_LAYOUT, p->stream)) {
        token *layout_name = consume_token(TOK_IDENT, "expected layout name", p->stream);
        if (!layout_name) return;
        if (hashmap_get(p->layouts, layout_name->start, layout_name->len)) {
            ERROR_AT(layout_name, p->stream,
                "layout '%.*s' is already defined", layout_name->len, layout_name->start);
            return;
        }

        layout *layout = my_malloc(sizeof *layout);
        layout->aligned = match_token(TOK_ALIGN, p->stream);
        vector_init(&layout->fields);
        hashmap_put(p->layouts, layout_name->start, layout_name->len, layout);

        for (;;) {
            token *field_name = current_token(p->stream);
            if (field_name->type != TOK_IDENT) {
                ERROR_AT(prev_token(p->stream), p->stream,
                    "unexpected comma at end of a layout declaration");
                break;
            }
            advance_token(p->stream);

            layout_field *field = my_malloc(sizeof *field);
            field->name = field_name;
            if (match_token(TOK_LSQUARE, p->stream)) {
                consume_expression(&field->elements_count, p);
                consume_token(TOK_RSQUARE, "expected ']'", p->stream);
            } else {
                field->elements_count = expr_new(EXPR_NUMBER);
                field->elements_count->number = 1;
            }

            if (!resolve_expression(&field->element_size, p)) {
                field->element_size = expr_new(EXPR_NUMBER);
                field->element_size->number = 1;
            }

            vector_add(&layout->fields, field);
            if (!match_token(TOK_COMMA, p->stream)) break;
        }
        if (layout->fields.count == 0) {
            ERROR_AT(layout_name, p->stream,
                "layout '%.*s' has no fields", layout_name->len, layout_name->start);
        }
        return;
    }
    if (match_token(TOK_DELETE, p->stream)) {
        token *name = consume_token(TOK_IDENT, "expected layout name", p->stream);
        if (!name) return;

        layout *layout = hashmap_get(p->layouts, name->start, name->len);
        if (!layout) {
            ERROR_AT(name, p->stream, "layout '%.*s' is undefined", name->len, name->start);
            return;
        }

        vector_add(p->deleted_layouts, layout);
        hashmap_remove(p->layouts, name->start, name->len);
        return;
    }
    if (match_token(TOK_NOP, p->stream)) {
        ir_instr *instr = instr_new(INSTR_NOP, p);
        vector_add(p->instrs, instr);
        return;
    }
    if (match_token(TOK_HLT, p->stream)) {
        ir_instr *instr = instr_new(INSTR_HALT, p);
        vector_add(p->instrs, instr);
        return;
    }
    if (parse_data_directive(p) || parse_labeldef(p) || parse_return(p) ||
        parse_use(p) || parse_binary(p) || parse_if(p) || parse_branch(p))
        return;

    unexpected_token(p);
}

void codegen(vector *instrs, arch_backend *backend, token_stream *stream);
bool parse(arch_backend *backend, token_stream *stream, vector *shared_labels, hashmap *layouts, vector *deleted_layouts, vector *instrs) {
    if (stream->tokens.count == 0) return true;

    parser p = {
        .backend = backend,
        .register_states = my_malloc(p.backend->register_count * sizeof(register_state)),
        .parse_mode = PARSE_NONE,
        .current_label = NULL,
        .layouts = layouts,
        .instrs = instrs,
        .shared_labels = shared_labels,
        .deleted_layouts = deleted_layouts,
        .stream = stream,
    };

    vector_init(&p.unresolved_labels);
    reset_registers(&p);

    while (!p.stream->error_occured && !at_end(p.stream)) parse_instr(&p);

    // Resolve labels
    for (int i = 0; i < p.unresolved_labels.count; i++) {
        unresolved_label *unresolved_label = vector_get(&p.unresolved_labels, i);

        bool found = false;
        for (int j = 0; j < p.shared_labels->count; j++) {
            label *target_label = vector_get(p.shared_labels, j);

            if (target_label->owner && unresolved_label->parent != target_label->owner) continue;
            if (token_is_str(unresolved_label->tok, target_label->name)) {
                *unresolved_label->dest = target_label;
                found = true;
                break;
            }
        }

        if (!found)
            ERROR_AT(unresolved_label->tok, p.stream,
                    "label '%.*s' is undefined", unresolved_label->tok->len,
                    unresolved_label->tok->start);
        my_free(unresolved_label);
    }
    vector_free(&p.unresolved_labels);
    my_free(p.register_states);
    return !p.stream->error_occured;
}

static int calculate_field_offset(codegen_ctx *c, layout *layout, layout_field *field) {
    int offset = 0;
    int padding_size = 0;
    for (int i = 0; i < layout->fields.count; i++) {
        layout_field *current_field = vector_get(&layout->fields, i);

        int64_t element_size = eval_expr(c, current_field->element_size);
        int64_t alignment_size = element_size; // The size for the alignment

        int64_t total_size = element_size * eval_expr(c, current_field->elements_count);
        if (layout->aligned) {
            // Example:
            // layout MY_LAYOUT aligned
            //      A sizeof LAYOUT -> Align A from the layout type
            if (current_field->element_size->kind == EXPR_LAYOUT &&
                current_field->element_size->layout.is_sizeof &&
                !current_field->element_size->layout.field &&
                !current_field->element_size->layout.index)
            {
                layout_field *first_field = vector_get(&current_field->element_size->layout.value->fields, 0);
                alignment_size = eval_expr(c, first_field->element_size);
            }
            if (alignment_size <= SIZE_DWORD && alignment_size % 2 == 0) {
                uint64_t index = 64 - __builtin_clzll(alignment_size % SIZE_DWORD);
                uint64_t alignment = c->backend->alignment[index];
                offset = ((offset + alignment - 1) / alignment) * alignment;
            }
        }
        if (tokens_equal(current_field->name, field->name)) break;
        if (token_is_str(current_field->name, "padding")) padding_size = total_size;
        offset += total_size;
    }
    offset -= padding_size;
    return offset;
}

int64_t eval_expr(codegen_ctx *c, expr_node *expr) {
    if (!expr) return 0;
    switch (expr->kind) {
    case EXPR_NUMBER: expr->result = expr->number; break;
    case EXPR_LABEL:  expr->result = expr->label->instr->addr; break;
    case EXPR_LAYOUT: {
        layout *layout = expr->layout.value;
        layout_field *field = expr->layout.field;
        expr->result = 0;
        if (field) {
            int64_t count = eval_expr(c, field->elements_count);
            int64_t size = eval_expr(c, field->element_size);
            if (expr->layout.is_sizeof) {
                expr->result = size * count;
                break;
            }

            if (expr->layout.index) {
                if (count == 1) {
                    ERROR_AT(expr->tok, c->stream,
                        "layout field of '%.*s' does not support indexing (it only has one element)",
                            expr->tok->len, expr->tok->start);
                    return false;
                }

                int64_t index = eval_expr(c, expr->layout.index);
                if (index >= count) {
                    ERROR_AT(expr->tok, c->stream, "array index out of bounds");
                    return false;
                }
                expr->result += size * index;
            }
            expr->result += calculate_field_offset(c, layout, field);
            break;
        }

        if (expr->layout.is_sizeof) {
            field = vector_get(&layout->fields, layout->fields.count - 1);

            int64_t size = eval_expr(c, field->element_size);
            int64_t count = eval_expr(c, field->elements_count);
            expr->result = calculate_field_offset(c, layout, field) + size * count;
            break;
        }

        for (int i = 0; i < layout->fields.count; i++) {
            field = vector_get(&layout->fields, i);
            if (token_is_str(field->name, "padding")) {
                expr->result += eval_expr(c, field->element_size) *
                                eval_expr(c, field->elements_count);
                break;
            }
        }
        break;
    }
    case EXPR_UNARY: {
        int64_t val = eval_expr(c, expr->unary.expr);
        switch (expr->unary.op) {
        case OPERATOR_UNARY_MINUS: val = -val; break;
        case OPERATOR_BIT_NOT:     val = ~val; break;
        case OPERATOR_LOGIC_NOT:   val = !val; break;
        case OPERATOR_SIN:         val = (int64_t)sin((double)val); break;
        case OPERATOR_COS:         val = (int64_t)cos((double)val); break;
        case OPERATOR_TAN:         val = (int64_t)tan((double)val); break;
        case OPERATOR_EXP:         val = (int64_t)exp((double)val); break;
        case OPERATOR_LOG:         val = (int64_t)log((double)val); break;
        case OPERATOR_ABS:         val = labs(val); break;
        case OPERATOR_SQRT:        val = (int64_t)sqrt((double)val); break;
        case OPERATOR_SIGN:        val = (val > 0) - (val < 0); break;
        case OPERATOR_CEIL:        val = (int64_t)ceil((double)val); break;
        case OPERATOR_FLOOR:       val = (int64_t)floor((double)val); break;
        default: break;
        }
        expr->result = val;
        break;
    }
    case EXPR_BINARY: {
        int64_t lhs = eval_expr(c, expr->binary.lhs);
        int64_t rhs = eval_expr(c, expr->binary.rhs);
        switch (expr->binary.op) {
        case OPERATOR_ADD: lhs += rhs;  break;
        case OPERATOR_SUB: lhs -= rhs;  break;
        case OPERATOR_MUL: lhs *= rhs;  break;
        case OPERATOR_DIV:
            if (rhs == 0) {
                ERROR_AT(expr->binary.op_tok, c->stream, "division by zero");
                break;
            }
            lhs /= rhs;
            break;
        case OPERATOR_MOD:
            if (rhs == 0) {
                ERROR_AT(expr->binary.op_tok, c->stream, "modulo by zero");
                return false;
            }
            lhs %= rhs;
            break;
        case OPERATOR_BIT_AND: lhs &= rhs; break;
        case OPERATOR_BIT_OR:  lhs |= rhs; break;
        case OPERATOR_BIT_XOR: lhs ^= rhs; break;
        case OPERATOR_LSHIFT:  lhs <<= rhs; break;
        case OPERATOR_RSHIFT:  lhs >>= rhs; break;
        case OPERATOR_POW: lhs = (int64_t)pow((double)lhs, (double)rhs); break;
        case OPERATOR_MIN: lhs = (lhs < rhs) ? lhs : rhs; break;
        case OPERATOR_MAX: lhs = (lhs > rhs) ? lhs : rhs; break;
        case OPERATOR_LT:  lhs = (lhs < rhs); break;
        case OPERATOR_LE:  lhs = (lhs <= rhs); break;
        case OPERATOR_EQ:  lhs = (lhs == rhs); break;
        case OPERATOR_NEQ: lhs = (lhs != rhs); break;
        case OPERATOR_GE:  lhs = (lhs >= rhs); break;
        case OPERATOR_GT:  lhs = (lhs > rhs); break;
        case OPERATOR_LOGIC_AND: lhs = (lhs && rhs); break;
        case OPERATOR_LOGIC_OR:  lhs = (lhs || rhs); break;
        default: break;
        }

        expr->result = lhs;
        break;
    }
    }

    return expr->result;
}

void value_free(value *val) {
    if (val->kind == VAL_EXPR) expr_free(val->expr);

    if (val->operand) {
        value_free(val->operand);
        my_free(val->operand);
    }
}
void expr_free(expr_node *expr) {
    if (!expr) return;
    switch (expr->kind) {
    case EXPR_NUMBER: case EXPR_LABEL: break;
    case EXPR_LAYOUT:
        expr_free(expr->layout.index);
        break;
    case EXPR_BINARY:
        expr_free(expr->binary.lhs);
        expr_free(expr->binary.rhs);
        break;
    case EXPR_UNARY:
        expr_free(expr->unary.expr);
        break;
    }
    my_free(expr);
}

bool value_matches(const value *actual, const value *expected) {
    if (!actual && !expected) return true;
    if (!actual || !expected || actual->kind != expected->kind || actual->is_addr != expected->is_addr ||
        actual->op_type != expected->op_type || !value_matches(actual->operand, expected->operand))
        return false;

    switch (actual->kind) {
    case VAL_REG:
        return actual->reg.index == expected->reg.index &&
            (actual->size == SIZE_NONE || expected->size == SIZE_NONE ||
            actual->size == expected->size) &&
            (actual->reg.sign == SIGN_NONE || expected->reg.sign == SIGN_NONE ||
            actual->reg.sign == expected->reg.sign);
    case VAL_EXPR: return actual->size <= expected->size;
    case VAL_FLAGS:
    case VAL_SP:
    case VAL_NONE: return true;
    }
}

// Prints a value without '\n', 'show imm' shows the immediate value if the value kind is an expression.
void print_value(FILE *out, const value *val, bool show_imm) {
    if (!val) return;
    if (val->is_addr) fprintf(out, "[");
    switch (val->kind) {
    case VAL_REG: {
        bool size_none = val->size == SIZE_NONE || val->size == SIZE_WORD;
        if (val->reg.sign == SIGN_NONE && size_none) {
            fprintf(out, "r%d", val->reg.index);
            break;
        }

        const char *sign = val->reg.sign == SIGN_SIGNED ? "s" :
                    val->reg.sign == SIGN_UNSIGNED ? "u" : "";
        if (size_none) {
            fprintf(out, "r%d(%s)", val->reg.index, sign);
            break;
        }

        fprintf(out, "r%d(%s%d)", val->reg.index, sign, val->size * 8);
        break;
    }
    case VAL_EXPR: {
        uint8_t size = val->size * 8;
        if (show_imm) {
            fprintf(out, "%lld(imm%d)", val->expr->result, size);
            break;
        }
        fprintf(out, "imm%d", size);
        break;
    }
    case VAL_FLAGS: fprintf(out, "flags"); break;
    case VAL_SP:    fprintf(out, "sp");    break;
    case VAL_NONE:  break;
    }
    if (val->operand) {
        const char op_char[] = {
            [OPERATOR_ADD] = '+', [OPERATOR_SUB] = '-', [OPERATOR_MUL] = '*', [OPERATOR_DIV] = '/'
        };
        fprintf(out, " %c ", op_char[val->op_type]);
        print_value(out, val->operand, show_imm);
    }
    if (val->is_addr) fprintf(out, "]");
}

void print_values(FILE *out, const value *val1, const value *val2, bool show_imm) {
    print_value(out, val1, show_imm);
    if (val2->kind != VAL_NONE) {
        fprintf(out, ", ");
        print_value(out, val2, show_imm);
    }
    fputc('\n', out);
}

void emit_value(codegen_ctx *c, const value *val, prim_size size) {
    if (!val) return;
    if (val->kind == VAL_EXPR) c->backend->emit(c, val->expr->result, size);
    if (val->operand) emit_value(c, val->operand, val->operand->size);
}
void eval_value(codegen_ctx *c, value *val) {
    if (!val) return;
    if (val->kind == VAL_EXPR) {
        eval_expr(c, val->expr);
        val->size = get_primitive_size(val->expr->result);
    }
    while (val->operand) {
        val = val->operand;
        eval_value(c, val);
    }
}