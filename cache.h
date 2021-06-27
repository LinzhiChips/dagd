/*
 * cache.h - DAG cache stage (not to be confused with the cache of DAG files)
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#ifndef DAGD_CACHE_H
#define	DAGD_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "linzhi/dagalgo.h"


struct cache {
	enum dag_algo	algo;
	uint16_t	epoch;
	unsigned	cache_bytes;
	uint8_t		*seed_hash;
	uint8_t		*cache;
	uint8_t		next_round;	/* CACHE_ROUNDS if done */
};


void cache_init(struct cache *c, enum dag_algo algo, uint16_t epoch);

/*
 * cache_build returns 1 if it had to do any work (and more work many need
 * to be done, 0 if the cache was already complete.
 */

bool cache_build(struct cache *c);
void cache_free(struct cache *c);

#endif /* !DAGD_CACHE_H */
