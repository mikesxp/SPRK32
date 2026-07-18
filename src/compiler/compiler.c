#include "compiler.h"
#include "preproc.h"
#include "frontend.h"
#include "lexer.h"
#include <stdio.h>

void codegen(vector *instrs, arch_backend *backend, token_stream *stream) {
    codegen_ctx c = {stream, backend, instrs};
    bool changed = true;

    value *switch_value = NULL;
    while (changed && !c.stream->error_occured) {
        if (backend->reset) backend->reset(&c);
        c.current_ip = 0;

        uint64_t current_address = c.current_ip;
        for (int i = 0; i < instrs->count; i++) {
            ir_instr *instr = vector_get(instrs, i);
            if (instr->kind == INSTR_LIMIT) {
                uint64_t limit_addr = eval_expr(&c, instr->limit.start);
                current_address = limit_addr;
            }

            // Recalculate the addresses of all instructions
            instr->addr = current_address;
            current_address += instr->current_size;
        }

        changed = false;
        for (c.instr_index = 0; c.instr_index < instrs->count && !c.stream->error_occured; c.instr_index++) {
            c.instr = vector_get(instrs, c.instr_index);

            uint64_t start_ip = c.current_ip;
            c.backend->gen_instr(&c);

            if (c.instr->kind == INSTR_LIMIT) {
                expr_node *last_limit = c.instr->limit.start;
                uint64_t new_limit = eval_expr(&c, last_limit);

                if (new_limit != last_limit->result) {
                    last_limit->result = new_limit;
                    changed = true;
                }

                current_address = last_limit->result;
                continue;
            }

            uint64_t size = c.current_ip - start_ip;
            if (c.instr->current_size < size) {
                c.instr->current_size = size;
                changed = true;
            }
        }
    }
}

void labels_free(vector *labels) {
    for (int i = 0; i < labels->count; i++) {
        label *lb = vector_get(labels, i);
        if (lb->type == LABEL_NAMED) my_free(lb->name);
        my_free(lb->instr);
        my_free(lb);
    }
    vector_free(labels);
}

bool compile(vector *files, arch_backend *backend, vector *shared_labels) {
    if (files->count == 0) return false;
    source_file *file = vector_get(files, files->count - 1);
    if (!file->content) return false;

    token_stream raw_stream = {0};
    if (!lex(file, &raw_stream.tokens)) {
        stream_free(&raw_stream);
        return false;
    }

    token_stream token_stream = {0};
    if (!preprocess(files, &raw_stream, &token_stream.tokens)) {
        vector_free(&token_stream.tokens);
        stream_free(&raw_stream);
        return false;
    }

    vector instrs, deleted_layouts;
    vector_init(&instrs);
    vector_init(&deleted_layouts);
    hashmap layouts;
    hashmap_init(&layouts);

    if (parse(backend, &token_stream, shared_labels, &layouts, &deleted_layouts, &instrs))
        codegen(&instrs, backend, &token_stream);
cleanup:
    for (int i = 0; i < token_stream.tokens.count; i++) {
        token *t = vector_get(&token_stream.tokens, i);
        if (t->generated) { // The tok is created by the preprocessor
            my_free(t->start);
            my_free(t);
        }
    }
    vector_free(&token_stream.tokens);
    stream_free(&raw_stream);

    hashmap_iter layout_iter = hashmap_iter_init(&layouts);
    layout *layout;
    while (hashmap_next(&layout_iter, NULL, (void**)&layout)) {
        layout_free(layout);
    }
    hashmap_free(&layouts);
    for (int i = 0; i < deleted_layouts.count; i++) {
        layout = vector_get(&deleted_layouts, i);
        layout_free(layout);
    }
    vector_free(&deleted_layouts);

    for (int i = 0; i < instrs.count; i++) {
        ir_instr *instr = vector_get(&instrs, i);
        switch (instr->kind) {
        case INSTR_LIMIT:
            expr_free(instr->limit.start);
            expr_free(instr->limit.end);
            break;
        case INSTR_RESERVE:
            expr_free(instr->reserve.expr);
            break;
        case INSTR_LABEL:
            if (instr->label->type == LABEL_NAMED) continue;
            my_free(instr->label);
            break;
        case INSTR_EMIT:
            for (int i = 0; i < instr->emit.values.count; i++) {
                expr_node *value = vector_get(&instr->emit.values, i);
                expr_free(value);
            }
            vector_free(&instr->emit.values);
            break;
        case INSTR_CALL:
        case INSTR_INT:
        case INSTR_JMP:
            value_free(&instr->branch.addr);
            break;
        case INSTR_LOAD:
        case INSTR_ADD:
        case INSTR_SUB:
        case INSTR_MUL:
        case INSTR_DIV:
        case INSTR_REM:
        case INSTR_AND:
        case INSTR_OR:
        case INSTR_XOR:
        case INSTR_SHL:
        case INSTR_SHR:
        case INSTR_ROL:
        case INSTR_ROR:
        case INSTR_CMP:
            value_free(&instr->bin.lhs);
            value_free(&instr->bin.rhs);
            break;
        case INSTR_NOP:
        case INSTR_HALT:
        case INSTR_RET:
        case INSTR_RETI:
        case INSTR_COUNT:
            break;
        }
        my_free(instr);
    }
    vector_free(&instrs);
    return !token_stream.error_occured;
}