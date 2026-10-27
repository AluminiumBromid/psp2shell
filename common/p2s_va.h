#ifndef P2S_VA_H
#define P2S_VA_H

// libk's va_start takes the address of the last named parameter and assumes the variadic
// arguments follow it in memory. On ARM they arrive in registers and GCC spills them to a
// separate area, so libk's va_start yields garbage pointers (with "%s" this crashed the
// Vita). Use GCC's va_start instead: on ARM its va_list wraps a pointer to that contiguous
// argument area, which is what libk's v*printf functions walk.
// Only 32-bit sized arguments are safe: libk's va_arg ignores the 8-byte alignment of
// 64-bit types.
// libk's printf also has no precision ("%.*s", "%.3s"); copy substrings by hand.

#ifdef __PSP2__
#define P2S_VA_LIST __builtin_va_list
#define P2S_VA_START(ap, last) __builtin_va_start(ap, last)
#define P2S_VA_END(ap) __builtin_va_end(ap)
#define P2S_VA_ARGS(ap) (*(char **) &(ap))
#else
#include <stdarg.h>
#define P2S_VA_LIST va_list
#define P2S_VA_START(ap, last) va_start(ap, last)
#define P2S_VA_END(ap) va_end(ap)
#define P2S_VA_ARGS(ap) (ap)
#endif

#endif // P2S_VA_H
