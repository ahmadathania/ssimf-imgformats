/* Minimal C runtime for the SSIMF WebAssembly build: a simple heap and the mem* functions. */
#include <stddef.h>
#include <stdint.h>
extern unsigned char __heap_base;
static uintptr_t top = 0;
typedef struct Blk { size_t size; struct Blk *next; } Blk;
static Blk *freel = 0;
void *malloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    Blk **pp = &freel;
    for (Blk *b = freel; b; pp = &b->next, b = b->next) if (b->size >= n) { *pp = b->next; return (char *)b + 16; }
    if (!top) top = ((uintptr_t)&__heap_base + 15) & ~(uintptr_t)15;
    uintptr_t need = top + 16 + n, have = __builtin_wasm_memory_size(0) * 65536;
    if (need > have) { if (__builtin_wasm_memory_grow(0, (need - have + 65535) / 65536) == (size_t)-1) return 0; }
    Blk *b = (Blk *)top; b->size = n; top = need; return (char *)b + 16;
}
void free(void *p) { if (!p) return; Blk *b = (Blk *)((char *)p - 16); b->next = freel; freel = b; }
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *calloc(size_t a, size_t b) { size_t n = a * b; void *p = malloc(n); if (p) memset(p, 0, n); return p; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *o = d; const unsigned char *i = s; while (n--) *o++ = *i++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *o = d; const unsigned char *i = s;
    if (o < i) while (n--) *o++ = *i++; else { o += n; i += n; while (n--) *--o = *--i; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) { const unsigned char *x = a, *y = b; for (; n; n--, x++, y++) if (*x != *y) return *x - *y; return 0; }
int abs(int v) { return v < 0 ? -v : v; }
/* exported helpers */
void *ssimf_alloc(size_t n) { return malloc(n); }
void ssimf_free(void *p) { free(p); }
