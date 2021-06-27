/*
 * debug.h - Debugging output
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#ifndef DAGD_DEBUG_H
#define	DAGD_DEBUG_H

#include <stdarg.h>


extern unsigned debug_level;


void vdebug(unsigned level, const char *fmt, va_list ap);


static inline void debug(unsigned level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static inline void debug(unsigned level, const char *fmt, ...)
{
	va_list ap;

	if (debug_level) {
		va_start(ap, fmt);
		vdebug(level, fmt, ap);
		va_end(ap);
	}
}

#endif /* !DAGD_DEBUG_H */
