#pragma once
/*
 * Minimal <string.h> for TAPP builds.
 *
 * Rule: declare ONLY what the firmware exports in lib/tapp/api/tapp_api_table.c.
 * Anything omitted here is a compile error at the call site (-Werror=implicit-function-declaration)
 * instead of an "Unsupported/missing import" failure when the device loads the tapp.
 *
 * Deliberately absent (not exported by firmware): strcat, strncat, strdup, strtok, strcasecmp,
 * strspn, strcspn, strpbrk, memccpy.
 */

#include <stddef.h>

void*  memchr(const void* s, int c, size_t n);
int    memcmp(const void* a, const void* b, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset(void* s, int c, size_t n);

char*  strchr(const char* s, int c);
int    strcmp(const char* a, const char* b);
char*  strcpy(char* dst, const char* src);
size_t strlen(const char* s);
int    strncasecmp(const char* a, const char* b, size_t n);
int    strncmp(const char* a, const char* b, size_t n);
char*  strncpy(char* dst, const char* src, size_t n);
char*  strndup(const char* s, size_t n);
size_t strnlen(const char* s, size_t n);
char*  strrchr(const char* s, int c);
char*  strstr(const char* hay, const char* needle);
