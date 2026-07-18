#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define KB 0x400
#define INITIAL_VEC_CAPACITY 4
#define ARRAYLEN(a) (sizeof(a) / sizeof(a[0]))
#define NS_PER_SEC 1000000000
#define NANOSLEEP_PRECISION 50000 // 50 nanoseconds
#define FIXED_SCALE (1 << 16) // Q16.16
//#define DEBUG

#define UNREACHABLE() do { assert(!"Unreachable code reached"); abort(); } while (0)

static inline int32_t float_to_fixed(float x) { return (int32_t)lroundf(x * FIXED_SCALE); }
static inline float fixed_to_float(int32_t fx) { return (float)fx / FIXED_SCALE; }

typedef enum primitive_size {
    SIZE_NONE,
    SIZE_BYTE  = 1,
    SIZE_HWORD = 2,
    SIZE_WORD  = 4,
    SIZE_DWORD = 8,
    SIZES_COUNT = 4,
} prim_size;
static inline prim_size get_primitive_size(int64_t value) {
    if (value >= INT8_MIN && value <= UINT8_MAX)        return SIZE_BYTE;
    else if (value >= INT16_MIN && value <= UINT16_MAX) return SIZE_HWORD;
    else if (value >= INT32_MIN && value <= UINT32_MAX) return SIZE_WORD;
    return SIZE_DWORD;
}

char *string_duplicate(const char *s, size_t len);
void string_copy(char *dest, const char *src, size_t size);
char *string_trim(char *str);
char *string_tokenize(char *str, const char *delim, char **saveptr);
char *string_allocf(const char *fmt, ...);

// Based on https://eddmann.com/posts/implementing-a-dynamic-vector-array-in-c/
typedef struct {
    size_t count, capacity;
    void **data;
} vector;

// Return false if the allocated data is NULL
bool vector_init(vector *vec);
void vector_init_null(vector *vec);

void vector_add(vector *vec, void *value);
void vector_insert(vector *vec, size_t index, void *value);
void vector_set(vector *vec, size_t index, void *value);
void *vector_get(vector *vec, size_t index);

void vector_delete(vector *vec, size_t index);
void vector_free(vector *vec);

void *my_malloc(size_t size);
void *my_realloc(void *ptr, size_t size);
void *my_calloc(size_t count, size_t size);
void my_free_impl(void **ptr);
size_t get_allocations();
#define my_free(ptr) my_free_impl((void **)&(ptr))

static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}
static inline void wait_ns(uint64_t ns_to_wait) {
    uint64_t start = now_ns();
    while ((now_ns() - start) < ns_to_wait) {
#if defined(__x86_64__) || defined(_M_X64)
        __asm__ __volatile__("pause");
#endif
    }
}

typedef struct { struct timespec start, delta; } timer;
void timer_start(timer *t);
void timer_update(timer *t);
double timer_elapsed(timer timer);

#define HASHMAP_INIT_CAPACITY 16
#define HASHMAP_LOAD_FACTOR 0.75

typedef struct hashmap_node {
    char *key;
    void *value;
    struct hashmap_node *next;
} hashmap_node;
typedef struct {
    hashmap_node **buckets;
    size_t capacity, count;
} hashmap;
bool hashmap_init(hashmap *map);
bool hashmap_put(hashmap *map, const char *key, const size_t key_len, void *value);
void *hashmap_get(const hashmap *map, const char *key, const size_t key_len);
bool hashmap_remove(hashmap *map, const char *key, const size_t key_len);
void hashmap_free(hashmap *map);
typedef struct {
    const hashmap *map;
    size_t bucket_idx;
    hashmap_node *node;
} hashmap_iter;
hashmap_iter hashmap_iter_init(const hashmap *map);
bool hashmap_next(hashmap_iter *iter, const char **key_out, void **val_out);

static inline void write16(uint8_t *out, const uint32_t addr, const uint16_t value) {
    uint8_t *ptr = out + addr;
    ptr[0] = (uint8_t)(value >> 8);
    ptr[1] = (uint8_t)(value);
}
static inline void write32(uint8_t *out, const uint32_t addr, const uint32_t value) {
    uint8_t *ptr = out + addr;
    ptr[0] = (value >> 24) & 0xFF;
    ptr[1] = (value >> 16) & 0xFF;
    ptr[2] = (value >> 8)  & 0xFF;
    ptr[3] = value & 0xFF;
}
static inline void write64(uint8_t *out, const uint32_t addr, const uint64_t value) {
    uint8_t *ptr = out + addr;
    ptr[0] = (uint8_t)(value >> 56);
    ptr[1] = (uint8_t)(value >> 48);
    ptr[2] = (uint8_t)(value >> 40);
    ptr[3] = (uint8_t)(value >> 32);
    ptr[4] = (uint8_t)(value >> 24);
    ptr[5] = (uint8_t)(value >> 16);
    ptr[6] = (uint8_t)(value >> 8);
    ptr[7] = (uint8_t)(value);
}
static inline uint16_t read16(const uint8_t *buffer, const uint32_t offset) {
    return ((uint16_t)buffer[offset] << 8) |
           ((uint16_t)buffer[offset + 1]);
}
static inline uint32_t read32(const uint8_t *buffer, const uint32_t offset) {
    return ((uint32_t)buffer[offset] << 24) |
           ((uint32_t)buffer[offset + 1] << 16) |
           ((uint32_t)buffer[offset + 2] << 8) |
           ((uint32_t)buffer[offset + 3]);
}
static inline uint64_t read64(const uint8_t *buffer, const uint32_t offset) {
    return ((uint64_t)buffer[offset]     << 56) |
           ((uint64_t)buffer[offset + 1] << 48) |
           ((uint64_t)buffer[offset + 2] << 40) |
           ((uint64_t)buffer[offset + 3] << 32) |
           ((uint64_t)buffer[offset + 4] << 24) |
           ((uint64_t)buffer[offset + 5] << 16) |
           ((uint64_t)buffer[offset + 6] << 8)  |
           ((uint64_t)buffer[offset + 7]);
}
static inline FILE *file_open(const char *filename, const char *mode) {
    FILE *f = fopen(filename, mode);
    if (!f) {
        fprintf(stderr, "Error opening file '%s': %s\n", filename, strerror(errno));
        return NULL;
    }
    return f;
}
static inline bool file_seek(FILE *f, const char *filename, int start) {
    if (!f) return false;
    if (fseek(f, 0, start) != 0) {
        fprintf(stderr, "Error seeking file '%s': %s\n", filename, strerror(errno));
        fclose(f);
        return false;
    }
    return true;
}
uint8_t *file_to_buffer(const char *filename, size_t *out_size);
char *file_to_string(const char *filename);
char *file_read_line(FILE *f);
char *read_line();

#endif