/*
 * hostrun shim: newlib's funopen() for Checkpoint's scriptconsole.cpp, which
 * builds the script's stdout with it. glibc has fopencookie() instead; this
 * header is force-included (-include) into that one file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOSTRUN_FUNOPEN_H
#define HOSTRUN_FUNOPEN_H

#include <stdio.h>

#define _READ_WRITE_BUFSIZE_TYPE int

#ifdef __cplusplus
extern "C" {
#endif

/* Write-only: the console stream never reads, seeks or closes. */
FILE* hostrun_funopen(const void* cookie, void* readfn, int (*writefn)(void*, const char*, int), void* seekfn, void* closefn);

#ifdef __cplusplus
}
#endif

#define funopen hostrun_funopen

#endif
