#include "utils.h"

// Duplicates a string (adds '\0' at len + 1)
char *string_duplicate(const char *s, size_t len) {
    if (!s) return NULL;
    char *dup = my_malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}
void string_copy(char *dest, const char *src, size_t size) {
    if (size == 0)
        return;
    memcpy(dest, src, size - 1);
    dest[size - 1] = '\0';
}
char *string_trim(char *str) {
    while (isspace(*str))
        str++;
    if (*str == 0)
        return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace(*end))
        end--;
    *(end + 1) = '\0';
    return str;
}
char *string_tokenize(char *str, const char *delim, char **saveptr) {
    if (str == NULL) {
        str = *saveptr;
    }

    // Skip any initial delimitors
    str += strspn(str, delim);

    // If the string is empty return NULL
    if (*str == '\0') {
        *saveptr = str;
        return NULL;
    }

    char *token = str; // Token start
    str = strpbrk(token, delim); // Find the next delimiter
    if (str == NULL) {
        // No delimiter found, next start = string end
        *saveptr = token + strlen(token);
    } else {
        // Terminate the token and update the pointer
        *str = '\0';
        *saveptr = str + 1;
    }

    return token;
}
char *string_allocf(const char *fmt, ...) {
    va_list args;
    va_list args_copy;

    va_start(args, fmt);
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (len < 0) {
        va_end(args_copy);
        return NULL;
    }

    // Allocation (+1 for '\0')
    char *buffer = my_malloc((size_t)len + 1);
    if (!buffer) {
        va_end(args_copy);
        return NULL;
    }

    // Write the str
    vsnprintf(buffer, (size_t)len + 1, fmt, args_copy);
    va_end(args_copy);
    return buffer;
}

// Vectors
bool vector_init(vector *vec) {
    vec->count = 0;
    vec->capacity = INITIAL_VEC_CAPACITY;
    vec->data = my_malloc(sizeof(void *) * vec->capacity);
    return vec->data != NULL;
}

void vector_init_null(vector *vec) {
    vec->count = 0;
    vec->capacity = 0;
    vec->data = NULL;
}

static void vector_resize(vector *vec, size_t capacity) {
    void **data = my_realloc(vec->data, sizeof(void *) * capacity);
    if (data) {
        vec->data = data;
        vec->capacity = capacity;
    }
}

void vector_add(vector *vec, void *value) {
    if (vec->count == vec->capacity)
        vector_resize(vec, vec->capacity * 2);

    if (vec->data)
        vec->data[vec->count++] = value;
}

void vector_insert(vector *vec, size_t index, void *value) {
    if (!vec->data || index > vec->count) return;
    if (vec->count == vec->capacity)
        vector_resize(vec, vec->capacity * 2);

    memmove(
        &vec->data[index + 1],
        &vec->data[index],
        (vec->count - index) * sizeof(void *)
    );

    vec->data[index] = value;
    vec->count++;
}

void vector_set(vector *vec, size_t index, void *value) {
    if (index < vec->count && vec->data)
        vec->data[index] = value;
}

void *vector_get(vector *vec, size_t index) {
    if (index < vec->count && vec->data)
        return vec->data[index];
    return NULL;
}

void vector_delete(vector *vec, size_t index) {
    if (index >= vec->count || !vec->data) return;

    // Move all the elements after Index to the left
    memmove(&vec->data[index], &vec->data[index + 1], (vec->count - index - 1) * sizeof(void *));

    vec->count--;
    if (vec->count > 0 && vec->count == vec->capacity / 4)
        vector_resize(vec, vec->capacity / 2);
}
void vector_free(vector *vec) {
    my_free(vec->data);
    vec->count = 0;
    vec->capacity = 0;
    vec->data = NULL;
}

static size_t allocations = 0;
void *my_malloc(size_t size) {
    void *alloc = malloc(size);

    if (alloc) allocations++;
    else fprintf(stderr, "Failed to allocate %zu bytes\n", size);
    return alloc;
}
void *my_calloc(size_t count, size_t size) {
    void *alloc = calloc(count, size);

    if (alloc) allocations++;
    else fprintf(stderr, "Failed to allocate %zu bytes\n", count * size);
    return alloc;
}
void *my_realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        void *alloc = realloc(NULL, size);

        if (alloc) allocations++;
        else fprintf(stderr, "Failed to allocate %zu bytes\n", size);
        return alloc;
    }

    void *alloc = realloc(ptr, size);
    if (!alloc) fprintf(stderr, "Failed to reallocate %zu bytes\n", size);
    return alloc;
}
void my_free_impl(void **ptr) {
    if (ptr && *ptr) {
        free(*ptr);
        *ptr = NULL;
        assert(allocations > 0);
        allocations--;
    }
#ifdef DEBUG
    else fprintf(stderr, "Failed to free: NULL pointer\n");
#endif
}
size_t get_allocations() {
    return allocations;
}

void timer_start(timer *t) {
    clock_gettime(CLOCK_MONOTONIC, &t->start);
}
void timer_update(timer *t) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    uint64_t secs = end.tv_sec - t->start.tv_sec;
    uint64_t nsecs = end.tv_nsec - t->start.tv_nsec;
    t->delta = (struct timespec){secs, nsecs};
}
double timer_elapsed(timer timer) {
    return timer.delta.tv_sec + (double)timer.delta.tv_nsec / NS_PER_SEC;
}

// All hash code is created using Claude Sonnet 4.6
static size_t hash(const char *key, const size_t key_len, size_t capacity) {
    size_t h = 2166136261u;
    for (size_t i = 0; i < key_len; ++i) {
        h ^= (unsigned char)key[i];
        h *= 16777619u;
    }
    return h & (capacity - 1); // Capacity is always a pow of 2
}
static size_t next_pow2(size_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}

bool hashmap_init(hashmap *map) {
    map->capacity = HASHMAP_INIT_CAPACITY;
    map->count = 0;
    map->buckets = my_calloc(map->capacity, sizeof(hashmap_node *));
    return map->buckets != NULL;
}
void hashmap_free(hashmap *map) {
    for (size_t i = 0; i < map->capacity; i++) {
        hashmap_node *n = map->buckets[i];
        while (n) {
            hashmap_node *next = n->next;
            my_free(n->key);
            my_free(n);
            n = next;
        }
    }
    my_free(map->buckets);
    map->buckets  = NULL;
    map->capacity = 0;
    map->count    = 0;
}

static bool rehash(hashmap *map) {
    size_t new_capacity = next_pow2(map->capacity * 2);
    hashmap_node **new_buckets = my_calloc(new_capacity, sizeof(hashmap_node *));
    if (!new_buckets) return false;

    for (size_t i = 0; i < map->capacity; i++) {
        hashmap_node *n = map->buckets[i];
        while (n) {
            hashmap_node *next = n->next;

            size_t idx = hash(n->key, strlen(n->key), new_capacity);
            n->next = new_buckets[idx];
            new_buckets[idx] = n;

            n = next;
        }
    }

    my_free(map->buckets);
    map->buckets  = new_buckets;
    map->capacity = new_capacity;
    return true;
}

// Puts or updates a new value in the hashmap. (Only the key is copied)
bool hashmap_put(hashmap *map, const char *key, const size_t key_len, void *value) {
    if (!map) return NULL;
    if ((double)map->count / map->capacity >= HASHMAP_LOAD_FACTOR) {
        if (!rehash(map)) return false;
    }

    size_t idx = hash(key, key_len, map->capacity);
    for (hashmap_node *n = map->buckets[idx]; n; n = n->next) {
        if (memcmp(n->key, key, strlen(n->key)) == 0) {
            n->value = value;
            return true;
        }
    }

    hashmap_node *node = my_malloc(sizeof(*node));
    if (!node) return false;

    node->key = string_duplicate(key, key_len);
    node->value = value;
    node->next = map->buckets[idx];
    map->buckets[idx] = node;
    map->count++;
    return true;
}

void *hashmap_get(const hashmap *map, const char *key, const size_t key_len) {
    if (!map) return NULL;

    for (hashmap_node *n = map->buckets[hash(key, key_len, map->capacity)]; n; n = n->next) {
        int len = strlen(n->key);
        if (len == key_len && !memcmp(n->key, key, len))
            return n->value;
    }
    return NULL;
}

bool hashmap_remove(hashmap *map, const char *key, const size_t key_len) {
    hashmap_node **pp = &map->buckets[hash(key, key_len, map->capacity)];
    while (*pp) {
        if (strncmp((*pp)->key, key, strlen((*pp)->key)) == 0) {
            hashmap_node *dead = *pp;
            *pp = dead->next;
            my_free(dead->key);
            my_free(dead);
            map->count--;
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}
hashmap_iter hashmap_iter_init(const hashmap *map) {
    return (hashmap_iter){
        .map = map,
        .bucket_idx = 0,
        .node = NULL
    };
}

bool hashmap_next(hashmap_iter *iter, const char **key_out, void **val_out) {
    if (iter->node) iter->node = iter->node->next;

    while (!iter->node && iter->bucket_idx < iter->map->capacity) {
        iter->node = iter->map->buckets[iter->bucket_idx++];
    }

    if (!iter->node) return false;

    if (key_out) *key_out = iter->node->key;
    if (val_out) *val_out = iter->node->value;
    return true;
}

static void *read_raw_file(const char *filename, size_t *out_size, size_t extra_bytes) {
    FILE *f = file_open(filename, "rb");
    if (!file_seek(f, filename, SEEK_END)) return NULL;

    long size = ftell(f);
    rewind(f);

    if (size < 0) {
        fprintf(stderr, "Error reading file size '%s': %s\n", filename, strerror(errno));
        fclose(f);
        return NULL;
    }

    void *buffer = my_malloc(size + extra_bytes);
    if (!buffer) {
        fclose(f);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, size, f);
    fclose(f);

    *out_size = read_size;
    return buffer;
}
uint8_t *file_to_buffer(const char *filename, size_t *out_size) {
    return (uint8_t *)read_raw_file(filename, out_size, 0);
}
char *file_to_string(const char *filename) {
    size_t read_size = 0;
    char *str = (char *)read_raw_file(filename, &read_size, 1);
    if (str) str[read_size] = '\0';
    return str;
}
char *file_read_line(FILE *f) {
    int c;
    size_t len = 0;
    char *line = my_malloc(len + 1);
    for (;;) {
        c = fgetc(f);
        if (c == EOF || c == '\n') break;

        line = my_realloc(line, len + 2); // +1 char + '\0'
        if (!line) return NULL;
        line[len++] = c;
    }

    if (c == EOF && len == 0) {
        my_free(line);
        return NULL;
    }

    line[len] = '\0';
    return line;
}

char *read_line() {
    size_t size = 16;
    char *buffer = my_malloc(size);
    if (!buffer) return NULL;

    size_t len = 0;
    int c;

    for (;;) {
        c = getchar();
        if (c == EOF || c == '\n') {
            buffer[len] = '\0';
            return buffer;
        }
        buffer[len++] = c;

        if (len >= size) {
            size *= 2;
            buffer = my_realloc(buffer, size);
            if (!buffer) {
                my_free(buffer);
                return NULL;
            }
        }
    }
}