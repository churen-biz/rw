#ifndef SC_ARENA_H
#define SC_ARENA_H

#include <stddef.h>

typedef struct ScArena ScArena;

ScArena *sc_arena_create(void);
void sc_arena_destroy(ScArena *arena);
void *sc_arena_alloc(ScArena *arena, size_t size);
char *sc_arena_strndup(ScArena *arena, const char *s, size_t n);

#endif
