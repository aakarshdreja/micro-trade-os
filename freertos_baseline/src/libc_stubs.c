/* ================================================================
 * libc_stubs.c. Freestanding implementations of the few libc
 * routines the compiler may emit implicitly (array/struct init,
 * aggregate copies). We build with -nostdlib, so we must provide
 * these ourselves.
 *
 * IMPORTANT: at -O2, GCC's loop-distribute-patterns pass rewrites a
 * plain byte-copy/zero loop into a call to memcpy/memset. If that
 * pass is allowed to touch THESE functions, memset() ends up calling
 * memset(). Unbounded recursion that overflows the stack. The
 * per-function 'optimize' attribute disables that pass here so the
 * loops compile to straight-line code.
 * ================================================================ */

#include <stddef.h>

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int c, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)dst;
    while (n-- > 0) {
        *p++ = (unsigned char)c;
    }
    return dst;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, size_t n)
{
    volatile unsigned char       *d = (volatile unsigned char *)dst;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    while (n-- > 0) {
        *d++ = *s++;
    }
    return dst;
}