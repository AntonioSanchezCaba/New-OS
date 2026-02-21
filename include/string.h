/*
 * string.h - String and memory manipulation functions
 */
#ifndef STRING_H
#define STRING_H

#include <types.h>

/* Memory functions */
void* memset(void* dest, int c, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
int   memcmp(const void* a, const void* b, size_t n);

/* String functions */
size_t strlen(const char* str);
char*  strcpy(char* dest, const char* src);
char*  strncpy(char* dest, const char* src, size_t n);
char*  strcat(char* dest, const char* src);
char*  strncat(char* dest, const char* src, size_t n);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
char*  strchr(const char* str, int c);
char*  strrchr(const char* str, int c);
char*  strstr(const char* haystack, const char* needle);
char*  strtok(char* str, const char* delim);
char*  strtok_r(char* str, const char* delim, char** saveptr);

/* Number conversion */
long   strtol(const char* str, char** endptr, int base);
unsigned long strtoul(const char* str, char** endptr, int base);
int    atoi(const char* str);
long   atol(const char* str);

/* Span functions */
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);

/* Kernel sprintf functions */
int    sprintf(char* buf, const char* fmt, ...);
int    snprintf(char* buf, size_t n, const char* fmt, ...);

#endif /* STRING_H */
