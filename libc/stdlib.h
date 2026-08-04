#pragma once
/*
 * Minimal <stdlib.h> for TAPP builds.
 *
 * Prefer os_malloc/os_free from tapp_api.h — they are tracked against the app's budget.
 * Deliberately absent (not exported by firmware): calloc, exit, abort, atof, atol, bsearch,
 * getenv, system.
 */

#include <stddef.h>

void* malloc(size_t n);
void  free(void* p);
void* realloc(void* p, size_t n);

void  qsort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*));

int   rand(void);
long  random(void);

int   atoi(const char* s);
float atoff(const char* s);          /* float-returning atof; the double atof is not exported */
long  strtol(const char* s, char** end, int base);
unsigned long strtoul(const char* s, char** end, int base);
float strtof(const char* s, char** end);

/* No libcall on Cortex-M7; keep them inline rather than importing. */
static inline int  abs(int x)   { return __builtin_abs(x); }
static inline long labs(long x) { return __builtin_labs(x); }
