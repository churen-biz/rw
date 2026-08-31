#include "sc_arena.h"

#include <stdlib.h>
#include <string.h>

enum { SC_ARENA_CHUNK = 4096 };

typedef struct ScArenaChunk {
    struct ScArenaChunk *next;
    size_t used;
    size_t capacity;
    unsigned char data[];
} ScArenaChunk;

struct ScArena {
    ScArenaChunk *head;
};

static size_t align8(size_t n) {
    return (n + 7u) & ~((size_t)7u);
}

ScArena *sc_arena_create(void) {
    ScArena *arena = calloc(1, sizeof(*arena));
    return arena;
}

void sc_arena_destroy(ScArena *arena) {
    if (arena == NULL) {
        return;
    }
    ScArenaChunk *chunk = arena->head;
    while (chunk != NULL) {
        ScArenaChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    free(arena);
}

void *sc_arena_alloc(ScArena *arena, size_t size) {
    if (arena == NULL || size == 0) {
        return NULL;
    }
    size_t need = align8(size);
    if (arena->head == NULL || arena->head->used + need > arena->head->capacity) {
        size_t capacity = need > SC_ARENA_CHUNK ? need : (size_t)SC_ARENA_CHUNK;
        ScArenaChunk *chunk = malloc(sizeof(*chunk) + capacity);
        if (chunk == NULL) {
            return NULL;
        }
        chunk->next = arena->head;
        chunk->used = 0;
        chunk->capacity = capacity;
        arena->head = chunk;
    }
    void *ptr = arena->head->data + arena->head->used;
    arena->head->used += need;
    return ptr;
}

char *sc_arena_strndup(ScArena *arena, const char *s, size_t n) {
    char *out = sc_arena_alloc(arena, n + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}
