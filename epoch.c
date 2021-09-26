/*
 * epoch.c - Epoch data and operations
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#define _GNU_SOURCE	/* for asprintf */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>

#include "linzhi/alloc.h"
#include "linzhi/common.h"
#include "linzhi/format.h"
#include "linzhi/dag.h"
#include "linzhi/dagalgo.h"
#include "linzhi/dagio.h"

#include "debug.h"
#include "mqtt.h"
#include "cache.h"
#include "dag.h"
#include "epoch.h"


struct epoch *epochs = NULL;
const char *dag_path_template;
const char *csum_path_template;
off_t max_cache;

static off_t block_size;


/* ----- Helper functions -------------------------------------------------- */


static off_t round_to_block(off_t size, blksize_t blksize)
{
	size += blksize - 1;
	size -= size % blksize;
	return size;
}


static off_t get_block_size(void)
{
	char *path = ".";
	char *free_this = NULL;
	const char *var;
	char *slash;
	struct statfs fs;

	var = strchr(dag_path_template, '%');
	if (var) {
		path = free_this = stralloc(dag_path_template);
		path[var - dag_path_template] = 0;
		slash = strrchr(path, '/');
		if (slash)
			*slash = 0;
		else
			path = ".";
	}
	if (statfs(path, &fs) < 0) {
		perror(path);
		exit(1);
	}

	debug(2, "%s: block size %lu\b", path, (unsigned long) fs.f_bsize);

	free(free_this);
	return fs.f_bsize;
}


/* ----- File name templates ----------------------------------------------- */


static char *template_epoch(const char *fmt, enum dag_algo algo, uint16_t n)
{
	char *path;

	if (!format_compatible(fmt, "su"))
		return NULL;
	if (asprintf(&path, fmt, dagalgo_name(algo), n) < 0) {
		perror("asprintf");
		exit(1);
	}
	return path;
}


bool template_valid(const char *s)
{
	char *tmp;

	tmp = template_epoch(s, 0, 0);
	free(tmp);
	return !!tmp;
}


/* ----- Epoch meta-data --------------------------------------------------- */


static struct epoch *epoch_new(enum dag_algo algo, uint16_t n)
{
	struct epoch *e = alloc_type(struct epoch);

	e->path = template_epoch(dag_path_template, algo, n);

	debug(1, "new %u: %s", n, e->path);

	e->algo = algo;
	e->num = n;
	e->dag_handle = NULL;
	e->csum_fd = -1;

	e->pos = 0;
	e->nominal = 0;
	e->lines = get_full_lines(n);
	e->size = 0;
	e->final = round_to_block((off_t) e->lines * DAG_LINE_BYTES,
	    block_size);

	debug(1, "new: %u lines, %llu bytes, %llu disk bytes",
	    e->lines, (unsigned long long) e->lines * DAG_LINE_BYTES,
	    (unsigned long long) e->final * DAG_LINE_BYTES);

	cache_init(&e->cache, e->algo, e->num);
	e->chunk = NULL;

	e->next = NULL;

	return e;
}


static void open_csum(struct epoch *e)
{
	char *path;

	if (!csum_path_template) {
		e->csum_fd = -1;
		return;
	}

	path = template_epoch(csum_path_template, e->algo, e->num);
	debug(1, "%s", path);
	e->csum_fd = open(path, O_RDONLY);
	if (e->csum_fd < 0)
		perror(path);
	free(path);
}


static struct epoch *epoch_open(enum dag_algo algo, uint16_t n)
{
	struct epoch *e = epoch_new(algo, n);
	uint64_t bytes;

	debug(1, "open %s", e->path);
	e->dag_handle = dagio_try_open(e->path, O_RDWR, e->lines);
	if (!e->dag_handle)
		goto fail;

	bytes = dagio_bytes(e->dag_handle);
	e->nominal = bytes / DAG_LINE_BYTES;
	e->size = round_to_block(bytes, block_size);

	debug(1, "%llu bytes = %u lines",
	    (unsigned long long) bytes, e->nominal);

	open_csum(e);

	return e;

fail:
	free(e->path);
	free(e);
	return NULL;
}


/* ----- Epoch addition/removal/reset -------------------------------------- */


static void append_epoch(struct epoch *e)
{
	struct epoch **anchor = &epochs;

	for (anchor = &epochs; *anchor; anchor = &(*anchor)->next)
		if ((*anchor)->num > e->num)
			break;
	e->next = *anchor;
	*anchor = e;
}


static void free_epoch(struct epoch *e)
{
	debug(1, "free_epoch %u (%p)", e->num, e);
	if (e->dag_handle)
		dagio_close(e->dag_handle);
	if (e->csum_fd >= 0 && close(e->csum_fd) < 0)
		perror("close checksum");
	free(e->path);
	cache_free(&e->cache);
	if (e->chunk)
		free(e->chunk);
	free(e);
}


static void wipe_epoch(struct epoch *e)
{
	dagio_close_and_delete(e->dag_handle);
	e->dag_handle = NULL;
}


static void remove_epoch(struct epoch *e, off_t *sum)
{
	struct epoch *prev = NULL;
	unsigned long long old_size = *sum;

	assert(epochs);

	*sum = 0;
	for (prev = epochs; prev && prev->next != e; prev = prev->next)
		*sum += prev->size;

	debug(1, "remove_epoch %s %u: %llu/%llu bytes",
	    dagalgo_name(e->algo), e->num, (unsigned long long) e->size,
	    old_size);

	if (prev)
		prev->next = e->next;
	else
		epochs = e->next;

	wipe_epoch(e);
	free_epoch(e);

	while (prev) {
		*sum += prev->size;
		prev = prev->next;
	}
}


/* ----- Report ------------------------------------------------------------ */


#define	REPORT_ENTRY_MAX_BYTES	(16 + (8 + 1) * 6 + 1)


char *epoch_report(void)
{
	unsigned n = 0;
	struct epoch *e;
	char *buf, *s;
	int len;

	for (e = epochs; e; e = e->next)
		n++;
	buf = s = alloc_size(n * REPORT_ENTRY_MAX_BYTES + 1);
	for (e = epochs; e; e = e->next) {
		if (s != buf)
			*s++ = ';';

		len = sprintf(s, "%s,%u,%u,%u,%u,%u,%u",
		    dagalgo_name(e->algo), e->num, e->pos, e->nominal,
		    e->lines, e->cache.next_round, CACHE_ROUNDS);
		s += len;
		assert(buf + n * REPORT_ENTRY_MAX_BYTES + 1 > s);
	}
	*s = 0;
	return buf;
}


/* ----- Scan cache for DAGs ----------------------------------------------- */


static void epoch_scan(void)
{
	struct epoch *e;
	enum dag_algo algo;
	uint16_t epoch;

	for (algo = 0; algo != dag_algos; algo++)
		for (epoch = EPOCH_MIN; epoch <= EPOCH_MAX; epoch++) {
			debug(1, "epoch_scan: %s (%u) %u",
			    dagalgo_name(algo), algo, epoch);
			e = epoch_open(algo, epoch);
			if (!e)
				continue;
			append_epoch(e);
		}
}


/* ----- Work on the DAG cache --------------------------------------------- */


static bool may_add(enum dag_algo algo, uint16_t n, off_t sum)
{
	struct epoch *victim;
	off_t size =
	    round_to_block((off_t) get_full_lines(n) * DAG_LINE_BYTES,
	    block_size);

	debug(1, "consider adding epoch %s %u (size %llu, cache %llu/%llu",
	    dagalgo_name(algo), n, (unsigned long long) size,
	    (unsigned long long) sum, (unsigned long long) max_cache);
	while (sum >= max_cache || sum + size >= max_cache) {
		if (!epochs)
			return 0;
		for (victim = epochs; victim->next; victim = victim->next)
			if (victim->algo != algo)
				break;
		if (victim->algo == algo && victim->num <= n)
			return 0;
		debug(1, "remove epoch %u (make room)", victim->num);
		remove_epoch(victim, &sum);
	}
	return 1;
}


static bool create_dag(struct epoch *e)
{
	assert(!e->dag_handle);
	e->dag_handle = dagio_try_open(e->path, O_CREAT | O_RDWR | O_TRUNC,
	    e->lines);
	if (!e->dag_handle)
		perror(e->path);
	return e->dag_handle;
}


static void new_epoch(enum dag_algo algo, uint16_t n)
{
	struct epoch *e = epoch_new(algo, n);

	if (!create_dag(e)) {
		free_epoch(e);
		return;
	}
	open_csum(e);
	append_epoch(e);
}


static bool maybe_prepend(void)
{
	struct epoch *e;

	for (e = epochs; e; e = e->next)
		if ((int) e->algo == curr_algo)
			break;
	if (!e || e->num <= curr_epoch)
		return 0;

	debug(1, "prepend epoch %s %u (first was %u)",
	    dagalgo_name(curr_algo), curr_epoch, e->num);
	e = epoch_new(curr_algo, curr_epoch);
	e->next = epochs;
	epochs = e;
	return 1;
}


static bool maybe_wipe(void)
{
	struct epoch **anchor = &epochs;
	struct epoch *e = NULL;

	for (anchor = &epochs; *anchor; anchor = &(*anchor)->next) {
		e = *anchor;
		if ((int) e->algo == curr_algo)
			break;
	}
	if (!e || e->num >= curr_epoch)
		return 0;
	
	debug(1, "purge epoch %u (current is %u)", e->num, curr_epoch);
	if (e->dag_handle)
		wipe_epoch(e);
	*anchor = e->next;
	free_epoch(e);
	return 1;
}


bool epoch_work(bool just_one)
{
	off_t sum = 0;	/* cache size in bytes, rounded to blocks */
	struct epoch *e = epochs;
	uint16_t next;

	debug(1, "epoch_work");
	if (curr_algo == -1) {
		debug(2, "no current algorithm");
		return 0;
	}
	if (!curr_epoch) {
		debug(2, "no current epoch");
		return 0;
	}
	if (maybe_prepend())
		return 1;
	if (!just_one)
		if (maybe_wipe())
			return 1;
	for (e = epochs; e; e = e->next)
		sum += e->size;
	debug(0, "total DAG cache size: %llu/%llu bytes",
	    (unsigned long long) sum, (unsigned long long) max_cache);
	next = curr_epoch;
	for (e = epochs; e; e = e->next) {
		uint64_t bytes;

		if (e->num < next)
			continue;
		if (e->num != next)
			break;
		next = e->num + 1;
		if (e->pos == e->lines) {
			if (e->chunk) {
				free(e->chunk);
				e->chunk = NULL;
			}
			cache_free(&e->cache);
			continue;
		}
		debug(1, "epoch %s %u: %u/%u/%u lines",
		    dagalgo_name(e->algo), e->num, e->pos, e->nominal,
		    e->lines);
		debug(1, "cache %lu/%lu, dag %lu/%lu",
		    (unsigned long) sum, (unsigned long) max_cache,
		    (unsigned long) e->size, (unsigned long) e->final);
		if (!just_one) {
			while (sum > max_cache ||
			    sum + e->final - e->size > max_cache) {
				/* We can't make room for more epochs. */
				if (!e->next)
					return 0;
				debug(1,
				    "remove epoch %u (try to make room for %u)",
				    e->next->num, e->num);
				remove_epoch(e->next, &sum);
				return 1;
			}
		}
		if (!e->dag_handle) {
			if (!create_dag(e))
				return 0;
		}
		if (!work_on(e))
			return 0;

		bytes = dagio_bytes(e->dag_handle);
		e->size = round_to_block(bytes, block_size);
		debug(2,
		    "update size to %llu/%llu "
		    "(%llu bytes, %llu block size)",
		    (unsigned long long) e->size,
		    (unsigned long long) e->final,
		    (unsigned long long) bytes,
		    (unsigned long long) block_size);
		assert(e->size <= e->final);
		return 1;
	}

	if (just_one) {
		if (next != curr_epoch)
			return 0;
	} else {
		if (!may_add(curr_algo, next, sum))
			return 0;
	}
	new_epoch(curr_algo, next);
	return 1;
}


/* ----- Initialize the DAG cache ------------------------------------------ */


void epoch_init(void)
{
	debug(1, "epoch_init");
	dag_init();
	block_size = get_block_size();
	epoch_scan();
	if (curr_algo == -1 && epochs)
		curr_algo = epochs->algo;
	if (!curr_epoch && epochs)
		curr_epoch = epochs->num;
}


/* ----- Shutdown ---------------------------------------------------------- */


void epoch_shutdown(void)
{
	struct epoch *e;

	debug(1, "epoch_shutdown");
	while (epochs) {
		e = epochs;
		epochs = e->next;
		free_epoch(e);
	}
}
