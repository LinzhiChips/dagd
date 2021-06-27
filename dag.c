/*
 * dag.c - DAG file generation
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include <gcrypt.h>

#include "linzhi/alloc.h"
#include "linzhi/dag.h"
#include "linzhi/dagio.h"

#include "debug.h"
#include "csum.h"
#include "dag.h"


/*
 * @@@ If we want to have finer granularity than a chunk at a time AND want
 * to be able to change between epochs (or check them in parallel), we'll need
 * one gcrypt context per epoch, not one shared among them all.
 *
 * If, on the other hand, we stay with the current approach, we could also have
 * a single shared chunk buffer.
 */

static gcry_md_hd_t h;


/*
 * @@@ We assume that the first checksum error we hit indicates that the rest
 * of the file needs to be calculated.
 */

static bool generate_chunk(struct epoch *e)
{
	uint32_t want_lines;

	/*
	 * @@@ Should adjust number of lines we calculate to CPU speed.
	 */
	debug(2, "generating chunk %u of epoch %u",
	    e->pos / LINES_PER_CHUNK, e->num);

	want_lines = e->pos + LINES_PER_CHUNK > e->lines ?
	    e->lines - e->pos : LINES_PER_CHUNK;

	debug(2, "%u lines, %lu bytes", want_lines,
	    (unsigned long) want_lines * DAG_LINE_BYTES);

	calc_dataset_range(e->chunk, e->pos, want_lines,
	    e->cache.cache, e->cache.cache_bytes);
	dagio_pwrite(e->dag_handle, e->chunk, want_lines, e->pos);
	e->pos += want_lines;
	return 1;
}


static bool check_chunk(struct epoch *e)
{
	uint8_t ref[CSUM_BYTES];
	unsigned char *res;
	unsigned chunk;
	uint32_t want_lines;
	ssize_t got;

	if (e->csum_fd < 0)
		return 0;
	chunk = e->pos / LINES_PER_CHUNK;
	debug(2, "checking chunk %u of epoch %u", chunk, e->num);
	got = pread(e->csum_fd, ref, CSUM_BYTES, (off_t) chunk * CSUM_BYTES);
	if (got < 0) {
		perror("checksum read");
		return 0;
	}
	if (got != CSUM_BYTES) {
		fprintf(stderr, "checksum: got %lu instead of %u bytes\n",
		    (unsigned long) got, CSUM_BYTES);
		return 0;
	}

	want_lines = e->pos + LINES_PER_CHUNK > e->lines ?
	    e->lines - e->pos : LINES_PER_CHUNK;
	debug(2, "%u lines, %lu bytes", want_lines,
	    (unsigned long) want_lines * DAG_LINE_BYTES);
	dagio_pread(e->dag_handle, e->chunk, want_lines, e->pos);

	gcry_md_reset(h);
	gcry_md_write(h, e->chunk, (size_t) want_lines * DAG_LINE_BYTES);
	res = gcry_md_read(h, GCRY_MD_SHA3_256);
	debug(2, "got %02x%02x%02x..., expected %02x%02x%02x...",
	    res[0], res[1], res[2], ref[0], ref[1], ref[2]);
	if (memcmp(res, ref, CSUM_BYTES))
		return 0;
	e->pos += want_lines;
	return 1;
}


bool work_on(struct epoch *e)
{
	if (!e->chunk)
		e->chunk = alloc_size(CHUNK_BYTES);
	assert(e->pos < e->lines);
	debug(0, "work_on epoch %u: lines %u/%u/%u",
	    e->num, e->pos, e->nominal, e->lines);
	if (e->pos + LINES_PER_CHUNK > e->nominal &&
	    e->nominal != e->lines) {
		if (cache_build(&e->cache))
			return 1;
		if (!generate_chunk(e))
			return 0;
	} else {
		if (!check_chunk(e)) {
			/*
			 * @@@ Should we ftruncate the DAG file at this point ?
			 * For now, we ignore any further content, but if we
			 * move to interval lists, we could map chunks that are
			 * completely backed by on-disk data.
			 */
			e->pos = e->pos - (e->pos % LINES_PER_CHUNK);
			e->nominal = e->pos;
			return 1;
		}
	}
	if (e->nominal < e->pos)
		e->nominal = e->pos;
	return 1;
}


void dag_init(void)
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
