/*
 * epoch.h - Epoch data and operations
 *
 * Copyright (C) 2021, 2022 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#ifndef DAGD_EPOCH_H
#define	DAGD_EPOCH_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "linzhi/dagio.h"

#include "cache.h"


/*
 * Note: if mining an Ethash-based coin with a really tiny DAG, you may need to
 * change EPOCH_MIN. (ZIL, currently at with epoch 0, is treated as a special
 * case.)
 */

#define	EPOCH_MIN	8	/* first epoch we may possibly see (POM) */
#define	EPOCH_MAX	1000	/* highest epoch our hardware supports is 439
				   (* 2 for ETC), but we can zombie-mine beyond
				   this. 120 more epochs should be more than
				   enough. */

struct epoch {
	char		*path;	/* path to DAG file */
	enum dag_algo	algo;	/* algorithm */
	uint16_t	num;	/* epoch number */
	struct dag_handle *dag_handle; /* NULL if none yet */
	int		csum_fd;/* checksum file for epoch; < 0 if missing */
	uint32_t	pos;	/* current line being verified/calculated */
	uint32_t	nominal;/* number of lines nominally present in file */
	uint32_t	lines;	/* total number of lines */
	off_t		size;	/* size in bytes (rounded to disk blocks) */
	off_t		final;	/* final size in bytes (rounded) */
	struct cache	cache;	/* Ethash cache */
	uint8_t		*chunk;	/* buffer */
	struct epoch	*next;	/* next epoch */
};


extern struct epoch *epochs;
extern const char *dag_path_template;
extern const char *csum_path_template;	/* may be NULL */

/*
 * Maximum DAG cache size, in bytes. dagd will never try to exceed this size,
 * but if it finds a cache that is larger, it will only remove items exceeding
 * the cache size if we need to make room for new or incomplete DAGs.
 */

extern off_t max_cache;


bool template_valid(const char *s);

char *epoch_report(void);

/*
 * epoch_work returns 1 if there is more work to do and we should call it again
 * soon, 0 if there won't be any work left before the next epoch change.
 */
bool epoch_work(bool just_one);
void epoch_init(void);
void epoch_shutdown(void);

#endif /* !DAGD_EPOCH_H */
