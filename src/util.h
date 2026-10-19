#ifndef UTIL_H
#define UTIL_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((noreturn, format(printf, 1, 2))) static inline void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", program_invocation_short_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#endif
