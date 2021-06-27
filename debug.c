/*
 * debug.c - Debugging output
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#include <stdarg.h>
#include <stdio.h>

#include "debug.h"


unsigned debug_level;


void vdebug(unsigned level, const char *fmt, va_list ap)
{
	unsigned i;

	if (debug_level <= level)
		return;
	for (i = level; i; i--)
		fprintf(stderr, "    ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
}
