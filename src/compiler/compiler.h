#ifndef COMPILER_H
#define COMPILER_H

#include "lexer.h"
#include "../utils.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct codegen_ctx codegen_ctx;
typedef struct {
    uint8_t register_count;
    prim_size max_value_size;
    prim_size alignment[SIZES_COUNT];
    void *emitter;
    void (*reset)(struct codegen_ctx *c);
    void (*gen_instr)(struct codegen_ctx *c);
    void (*emit)(struct codegen_ctx *c, int64_t value, prim_size size);
} arch_backend;
struct instr;
struct codegen_ctx {
    token_stream *stream;
    arch_backend *backend;
    vector *instrs;
    struct instr *instr;
    size_t instr_index;
    uint64_t current_ip;
};

struct value;
void emit_value(codegen_ctx *c, const struct value *value, prim_size size);

bool compile(vector *files, arch_backend *backend, vector *shared_labels);
void labels_free(vector *labels);

#endif