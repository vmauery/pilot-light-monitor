/* nanoprintf: a tiny embeddable printf replacement written in C.
   https://github.com/charlesnicholson/nanoprintf
   charles.nicholson+nanoprintf@gmail.com
   dual-licensed under 0bsd and unlicense, take your pick. see eof for details.
 */

#ifndef NANOPRINTF_H_INCLUDED
#define NANOPRINTF_H_INCLUDED

#include <stdarg.h>
#include <stddef.h>

#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS 1

// Define this to fully sandbox nanoprintf inside of a translation unit.
#ifdef NANOPRINTF_VISIBILITY_STATIC
#define NPF_VISIBILITY static
#else
#define NPF_VISIBILITY extern
#endif

#if defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
#define NPF_PRINTF_ATTR(FORMAT_INDEX, VARGS_INDEX)                             \
    __attribute__((format(printf, FORMAT_INDEX, VARGS_INDEX)))
#else
#define NPF_PRINTF_ATTR(FORMAT_INDEX, VARGS_INDEX)
#endif

// Public API

#ifdef __cplusplus
extern "C" {
#endif

NPF_VISIBILITY int npf_snprintf(char* buffer, size_t bufsz, const char* format,
                                ...) NPF_PRINTF_ATTR(3, 4);

NPF_VISIBILITY int npf_vsnprintf(char* buffer, size_t bufsz, char const* format,
                                 va_list vlist) NPF_PRINTF_ATTR(3, 0);

typedef void (*npf_putc)(int c, void* ctx);
NPF_VISIBILITY int npf_pprintf(npf_putc pc, void* pc_ctx, char const* format,
                               ...) NPF_PRINTF_ATTR(3, 4);

NPF_VISIBILITY int npf_vpprintf(npf_putc pc, void* pc_ctx, char const* format,
                                va_list vlist) NPF_PRINTF_ATTR(3, 0);

#define snprintf npf_snprintf

#ifdef __cplusplus
}
#endif

#endif // NANOPRINTF_H_INCLUDED
