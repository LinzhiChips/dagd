/*
 * csum.c - Generate DAG file checksums
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include <gcrypt.h>

#include "linzhi/alloc.h"
#include "linzhi/dag.h"
#include "linzhi/dagalgo.h"

#include "csum.h"


static gcry_md_hd_t h;


static uint16_t lines_to_chunks(unsigned lines)
{
	return (lines + LINES_PER_CHUNK - 1) / LINES_PER_CHUNK;
}


static unsigned lines_in_chunk(uint16_t chunk, unsigned lines)
{
	uint16_t chunks = lines_to_chunks(lines);

	return chunk == chunks - 1 ? lines % LINES_PER_CHUNK : LINES_PER_CHUNK;
}


static void init_crypto(void)
{
	gcry_error_t err;

	gcry_check_version(NULL);
	gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
	err = gcry_md_open(&h, GCRY_MD_SHA3_256, 0);
	if (err) {
		fprintf(stderr, "gcry_md_open: %s\n", gcry_strerror(err));
		exit(1);
	}
}


void csum_generate(enum dag_algo algo, uint16_t epoch)
{
	uint8_t seed[SEED_BYTES];
	unsigned cache_bytes = get_cache_size(epoch);
	unsigned full_lines = get_full_lines(epoch);
	unsigned chunks = lines_to_chunks(full_lines);
	uint8_t *cache = alloc_size(cache_bytes);
	uint8_t *chunk = alloc_size(CHUNK_BYTES);
	unsigned i;

	init_crypto();
	dag_algo = algo;
	get_seedhash(seed, epoch);
	if (dag_algo != 2) {
		mkcache(cache, cache_bytes, seed);
	} else {
		mkcache_ubqhash(cache, cache_bytes, seed);
	}

	for (i = 0; i != chunks; i++) {
		unsigned lines = lines_in_chunk(i, full_lines);
		unsigned char *res;
		ssize_t wrote;

		calc_dataset_range(chunk, i * LINES_PER_CHUNK, lines,
		    cache, cache_bytes);
		gcry_md_reset(h);
		gcry_md_write(h, chunk, lines * DAG_LINE_BYTES);
		res = gcry_md_read(h, GCRY_MD_SHA3_256);
		wrote = write(1, res, CSUM_BYTES);
		if (wrote < 0) {
			perror("write");
			exit(1);
		}
		if (wrote != CSUM_BYTES) {
			fprintf(stderr, "short write: %u < %u\n",
			    (unsigned) wrote, CSUM_BYTES);
			exit(1);
		}
	}
	free(cache);
	free(chunk);
}
