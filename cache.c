/*
 * cache.c - DAG cache stage (not to be confused with the cache of DAG files)
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "linzhi/alloc.h"
#include "linzhi/common.h"
#include "linzhi/dag.h"
#include "linzhi/dagalgo.h"

#include "debug.h"
#include "cache.h"


void cache_init(struct cache *c, enum dag_algo algo, uint16_t epoch)
{
	c->algo = algo;
	c->epoch = epoch;
	c->cache_bytes = get_cache_size(epoch);
	c->seed_hash = NULL;
	c->cache = NULL;
	c->next_round = 0;
	debug(1, "cache: %u bytes", c->cache_bytes);
}


bool cache_build(struct cache *c)
{
	debug(2, "cache_build: %p %p %u",
	    c->seed_hash, c->cache, c->next_round);
	dag_algo = c->algo;
	if (!c->seed_hash) {
		c->seed_hash = alloc_size(SEED_BYTES);
		get_seedhash(c->seed_hash, c->epoch);
		return 1;
	}
	if (dag_algo != 2) {
		if (!c->cache) {
			c->cache = alloc_size(c->cache_bytes);
			mkcache_init(c->cache, c->cache_bytes, c->seed_hash);
			return 1;
		}
		if (c->next_round != CACHE_ROUNDS) {
			mkcache_round(c->cache, c->cache_bytes);
			c->next_round++;
			return 1;
		}
	} else {
		if (!c->cache) {
			c->cache = alloc_size(c->cache_bytes);
			mkcache_init_ubqhash(c->cache, c->cache_bytes, c->seed_hash);
			return 1;
		}
		if (c->next_round != CACHE_ROUNDS) {
			mkcache_round_ubqhash(c->cache, c->cache_bytes);
			c->next_round++;
			return 1;
		}
	}
	return 0;
}


void cache_free(struct cache *c)
{
	if (c->seed_hash)
		free(c->seed_hash);
	if (c->cache)
		free(c->cache);
	c->seed_hash = NULL;
	c->cache = NULL;
}
