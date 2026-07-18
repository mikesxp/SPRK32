#include "preproc.h"
#include "lexer.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    int start;
    int end;
} token_range;
typedef struct {
    vector param_names; // Vector of token
    token_range body;
    bool expanding;
    bool variadic;
    token *tok;
} macro;
typedef struct {
    token_stream *stream;
    token *currtok;
    vector *files;
    hashmap macros;
} preprocessor;

static inline void macro_free(macro *m) {
    vector_free(&m->param_names);
    my_free(m);
}
static bool preprocess_define(preprocessor *pp) {
    token *macro_name = consume_token(TOK_IDENT, "expected macro name", pp->stream);
    if (!macro_name) return true;

    macro *defined_macro = hashmap_get(&pp->macros, macro_name->start, macro_name->len);
    if (defined_macro) {
        ERROR_AT(macro_name, pp->stream,
            "macro '%.*s' is already defined", macro_name->len, macro_name->start);
        report(defined_macro->tok, DIAGNOSTIC_NOTE, false,
            pp->stream, "previous definition is here");
        return true;
    }

    macro *m = my_malloc(sizeof *m);
    vector_init_null(&m->param_names);
    m->tok = macro_name;
    m->variadic = false;
    if (match_token(TOK_LPAREN, pp->stream)) {
        vector_init(&m->param_names);
        while (!pp->stream->error_occured) {
            token *currtok = take_token(pp->stream);
            if (currtok->type != TOK_IDENT) {
                if (currtok->type != TOK_DOTS3) {
                    EXPECT_AT(currtok, pp->stream, "expected a macro parameter name");
                    goto error;
                }
                m->variadic = true;
                break;
            }

            vector_add(&m->param_names, currtok);
            if (!match_token(TOK_COMMA, pp->stream)) break;
        }
        consume_token(TOK_RPAREN, "expected ')'", pp->stream);
    }

    consume_token(TOK_LBRACE, "expected macro body, use '{'", pp->stream);
    m->body.start = pp->stream->pos;
    while (!pp->stream->error_occured) {
        token *tok = current_token(pp->stream);
        if (tok->type == TOK_EOF) {
            ERROR_AT(prev_token(pp->stream), pp->stream, "unclosed macro body (expected '}')");
            report(macro_name, DIAGNOSTIC_NOTE, false, pp->stream, "macro definition is here");
            goto error;
        }
        if (match_token(TOK_RBRACE, pp->stream)) {
            m->body.end = pp->stream->pos - 1;
            break;
        }
        token *next = next_token(pp->stream);
        if (match_token(TOK_HASHTAG, pp->stream) && token_is_str(next, "forvarg")) {
            while (!match_token(TOK_RBRACE, pp->stream)) {
                if (match_token(TOK_EOF, pp->stream)) {
                    ERROR(pp->stream, "expected '}'");
                    break;
                }
                advance_token(pp->stream);
            }
            continue;
        }
        advance_token(pp->stream);
    }

    hashmap_put(&pp->macros, macro_name->start, macro_name->len, m);
    return true;

error:
    macro_free(m);
    return true;
}

static token_range *check_macro_param(token *tok, vector *param_ranges, macro *m) {
    for (size_t p = 0; p < m->param_names.count; p++) {
        token *param_name = vector_get(&m->param_names, p);
        if (tokens_equal(param_name, tok)) return vector_get(param_ranges, p);
    }
    return NULL;
}
static bool preprocess_macro_call(preprocessor *pp, token *tok);
static inline void insert_token_copy(int token_index, preprocessor *pp) {
    token *tok = my_malloc(sizeof *tok);
    *tok = *(token*)vector_get(&pp->stream->tokens, token_index);
    tok->src = pp->currtok;
    vector_insert(&pp->stream->tokens, pp->stream->pos, tok);
}
static bool expand_forvargs(preprocessor *pp, vector *param_ranges, macro *m, int *token_index) {
    token *currtok = vector_get(&pp->stream->tokens, *token_index);
    if (currtok->type != TOK_RBRACE) return false ;
    int body_end = *token_index;
    int body_start = -1, depth = 1;
    for (int i = *token_index - 1; i >= m->body.start; i--) {
        token *t = vector_get(&pp->stream->tokens, i);
        if (t->type == TOK_RBRACE)      depth++;
        else if (t->type == TOK_LBRACE) depth--;
        if (depth == 0) {
            body_start = i;
            break;
        }
    }

    if (body_start < 2) return false;
    token *hash = vector_get(&pp->stream->tokens, body_start - 2);
    token *keyword = vector_get(&pp->stream->tokens, body_start - 1);
    if (hash->type != TOK_HASHTAG || !token_is_str(keyword, "forvarg")) return false;

    if (!m->variadic) {
        ERROR_AT(keyword, pp->stream,
            "variadic arguments are not defined for this macro");
        return false;
    }

    for (int arg = param_ranges->count - 1; arg >= (int)m->param_names.count; arg--) {
        token_range *param_range = vector_get(param_ranges, arg);
        for (int i = body_end - 1; i > body_start; i--) {
            token *tok = vector_get(&pp->stream->tokens, i);
            if (token_is_str(tok, "varg")) {
                for (int j = param_range->end - 1; j >= param_range->start; j--)
                    insert_token_copy(j, pp);
                continue;
            }
            insert_token_copy(i, pp);
        }
    }
    *token_index = body_start - 2;
    return true;
}
static void expand_macro(preprocessor *pp, macro *m, vector *param_ranges) {
    m->expanding = true;
    for (int mtoken_index = m->body.end - 1;
        mtoken_index >= m->body.start && !pp->stream->error_occured;
        mtoken_index--)
    {
        token *currtok = vector_get(&pp->stream->tokens, mtoken_index);
        if (expand_forvargs(pp, param_ranges, m, &mtoken_index)) continue;

        token_range *param_range = check_macro_param(currtok, param_ranges, m);
        if (param_range) {
            // Expand parameter
            for (int ptoken_index = param_range->end - 1; ptoken_index >= param_range->start; ptoken_index--)
                insert_token_copy(ptoken_index, pp);
            continue;
        }

        insert_token_copy(mtoken_index, pp);
        preprocess_macro_call(pp, currtok);
    }
    m->expanding = false;
}
static token_range *read_param_range(preprocessor *pp, bool last) {
    token_range *range = my_malloc(sizeof *range);
    range->start = pp->stream->pos;

    for (;;) {
        token *tok = take_token(pp->stream);
        if (last) {
            if (tok->type == TOK_RPAREN) break;
            if (tok->type == TOK_EOF || tok->type == TOK_COMMA) {
                ERROR(pp->stream, "expected ')'");
                break;
            }
            continue;
        }

        if (tok->type == TOK_COMMA) break;
        if (tok->type == TOK_EOF || tok->type == TOK_RPAREN) {
            ERROR_AT(tok, pp->stream, "too few parameters in macro call");
            break;
        }
    }
    range->end = pp->stream->pos - 1;
    return range;
}
static bool preprocess_macro_call(preprocessor *pp, token *tok) {
    if (tok->type != TOK_IDENT) return false;

    macro *m = hashmap_get(&pp->macros, tok->start, tok->len);
    if (!m) return false;

    advance_token(pp->stream); // Skip macro name
    if (m->expanding) return true;

    // Read params
    vector param_ranges = {};
    if (m->param_names.count > 0 || m->variadic) {
        consume_token(TOK_LPAREN, "expected '('", pp->stream);
        vector_init(&param_ranges);

        size_t normal_count = m->param_names.count;
        for (size_t p = 0; p < normal_count; p++) {
            bool last = (p == normal_count - 1) && !m->variadic;
            vector_add(&param_ranges, read_param_range(pp, last));
        }

        if (m->variadic) {
            bool read = true;
            while (read) {
                token_range *range = my_malloc(sizeof *range);
                range->start = pp->stream->pos;
                for (;;) {
                    token *tok = take_token(pp->stream);
                    bool is_eof = tok->type == TOK_EOF;
                    if (is_eof || tok->type == TOK_RPAREN) {
                        if (is_eof) ERROR_AT(tok, pp->stream, "expected ')'");
                        read = false;
                        break;
                    }
                    if (tok->type == TOK_COMMA) break;
                }
                range->end = pp->stream->pos - 1;
                vector_add(&param_ranges, range);
            }
        }
    }

    expand_macro(pp, m, &param_ranges);

    for (int r = 0; r < param_ranges.count; r++) {
        token_range *range = vector_get(&param_ranges, r);
        my_free(range);
    }
    vector_free(&param_ranges);
    return true;
}

static inline bool is_endif(token *tok) {
    return token_is_str(tok, "endif");
}
static bool preprocess_if(preprocessor *pp, bool expand, vector *out) {
    bool invert = match_token(TOK_BANG, pp->stream);
    token *value = take_token(pp->stream);

    bool result = false;
    if (value->type == TOK_IDENT) result = hashmap_get(&pp->macros, value->start, value->len);
    else if (value->type == TOK_NUM) result = value->number;
    else {
        EXPECT_AT(value, pp->stream, "expected macro name or number in preprocessor if");
        return false;
    }
    if (invert) result = !result;

    token_range true_range = {pp->stream->pos};
    token_range false_range = {0, 0};
    for (;;) {
        token *current = current_token(pp->stream);
        if (match_token(TOK_EOF, pp->stream)) {
            EXPECT_AT(current, pp->stream, "unclosed '#if' (expected '#endif')");
            return false;
        }
        token *next = advance_token(pp->stream);

        // Preprocess '#endif/else'
        bool is_end = is_endif(next);
        if (current->type != TOK_HASHTAG || (next->type != TOK_ELSE && !is_end)) continue;

        true_range.end = pp->stream->pos - 1; // Stop before '#endif/else'
        advance_token(pp->stream); // Skip endif/else
        if (is_end) break;

        // Preprocess else statement
        false_range.start = pp->stream->pos;
        current = current_token(pp->stream);
        for (;;) {
            if (match_token(TOK_EOF, pp->stream)) {
                ERROR_AT(current, pp->stream, "unclosed '#else' (expected '#end')");
                return false;
            }
            if (is_endif(next_token(pp->stream)) &&
                match_token(TOK_HASHTAG, pp->stream))
            {
                false_range.end = pp->stream->pos - 1; // Range ends before '#'
                advance_token(pp->stream); // Skip 'endif'
                break;
            }
            current = advance_token(pp->stream);
        }
        break;
    }

    if (!expand) return true;
    if (result) {
        for (int i = true_range.end - 1; i >= true_range.start; i--)
            insert_token_copy(i, pp);
        return true;
    }

    for (int i = false_range.end - 1; i >= false_range.start; i--)
        insert_token_copy(i, pp);
    return true;
}
bool preprocess_include(preprocessor *pp) {
    token *include_tok = prev_token(pp->stream);
    if (!consume_token(TOK_LESS, "expected '<'", pp->stream))
        return true;

    token *currtok = current_token(pp->stream);
    char *path_start = currtok->start;
    while (!match_token(TOK_GREATER, pp->stream))
        currtok = advance_token(pp->stream);
    int path_len = currtok->start - path_start;

    for (int i = 0; i < pp->files->count; i++) {
        source_file *file = vector_get(pp->files, i);
        if (strncmp(file->path_name, path_start, strlen(file->path_name)) == 0) {
            report(include_tok, DIAGNOSTIC_WARNING, false, pp->stream, "file already inserted");
            return true;
        }
    }

    source_file *file = file_new(pp->files, string_duplicate(path_start, path_len));
    if (file) {
        vector out;
        lex(file, &out);

        for (int i = out.count - 2; i >= 0; i--) { // out.count - 1 == TOK_EOF
            token *t = vector_get(&out, i);
            vector_insert(&pp->stream->tokens, pp->stream->pos, t);
        }

        token *eof_tok = vector_get(&out, out.count - 1);
        my_free(eof_tok);
        vector_free(&out);
    }
    return true;
}

static bool preproc_undef(preprocessor *pp) {
    for (;;) {
        token *macro_name = consume_token(TOK_IDENT, "expected macro name", pp->stream);
        if (!macro_name) return false;

        macro *m = hashmap_get(&pp->macros, macro_name->start, macro_name->len);
        if (!m) {
            ERROR_AT(macro_name, pp->stream, "macro '%.*s' is undefined", macro_name->len, macro_name->start);
            break;
        }
        macro_free(m);
        hashmap_remove(&pp->macros, macro_name->start, macro_name->len);

        if (!match_token(TOK_COMMA, pp->stream)) break;
    }
    return true;
}

static bool preprocess_directive(preprocessor *pp, vector *out) {
    token *directive_tok = match_token(TOK_HASHTAG, pp->stream);
    if (!directive_tok) return false;

    token *directive_name = take_token(pp->stream);
    if (directive_name->type == TOK_IF) return preprocess_if(pp, true, out);
    if (token_is_str(directive_name, "define")) return preprocess_define(pp);
    if (token_is_str(directive_name, "undef"))  return preproc_undef(pp);
    if (token_is_str(directive_name, "include")) return preprocess_include(pp);

    EXPECT_AT(directive_tok, pp->stream, "invalid directive name");
    return false;
}

bool preprocess(vector *files, token_stream *in, vector *out) {
    preprocessor pp = {
        .stream = in,
        .files = files,
        .currtok = current_token(pp.stream),
    };

    hashmap_init(&pp.macros);
    vector_init(out);
    while (!pp.stream->error_occured) {
        token *next = next_token(pp.stream);
        if (next && next->type == TOK_HASHTAG_HASHTAG) {
            token *t1 = current_token(pp.stream);
            advance_token(pp.stream);
            token *t2 = advance_token(pp.stream);

            token *result = my_malloc(sizeof *result);
            result->len = t1->len + t2->len;
            result->start = (char *)my_malloc(result->len + 1);

            memcpy(result->start, t1->start, t1->len);
            memcpy(result->start + t1->len, t2->start, t2->len);
            result->start[result->len] = '\0';

            result->location = t1->location;
            result->type = t1->type;
            result->src = t1;
            result->generated = true;
            result->number = 0;

            if (preprocess_macro_call(&pp, result)) {
                my_free(result->start);
                my_free(result);
            } else vector_add(out, result);
            pp.currtok = advance_token(pp.stream); // Skip t2
            continue;
        }
        if (preprocess_directive(&pp, out) || preprocess_macro_call(&pp, pp.currtok)) {
            pp.currtok = current_token(pp.stream);
            continue;
        }

        // Add normal tok
        vector_add(out, pp.currtok);
        if (match_token(TOK_EOF, pp.stream)) break;
        pp.currtok = advance_token(pp.stream);
    }

    hashmap_iter miter = hashmap_iter_init(&pp.macros);
    macro *m;
    while (hashmap_next(&miter, NULL, (void**)&m)) macro_free(m);
    hashmap_free(&pp.macros);

    return !pp.stream->error_occured;
}