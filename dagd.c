/*
 * dagd.c - DAG generation and cache management demon
 *
 * Copyright (C) 2021, 2022 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <sys/statvfs.h>

#include "linzhi/alloc.h"
#include "linzhi/dagalgo.h"

#include "debug.h"
#include "mqtt.h"
#include "csum.h"
#include "epoch.h"


static void send_status(mqtt_handle mqtt, bool idle)
{
	char *status;

	status = epoch_report();
	mqtt_status(mqtt, status, idle);
	free(status);
	if (!idle)
		mqtt_poll(mqtt, 0);
}


static void loop(const char *broker)
{
	mqtt_handle mqtt = mqtt_init(broker, 0);
	bool holding = 0;

	while (1) {
		bool idle;

		if (shutdown_pending) {
			mqtt_poll(mqtt, 1);
			continue;
		}
		epoch_init();
		idle = 0;
		while (!shutdown_pending) {
			if (idle || hold) {
				enum dag_algo last_algo;
				uint16_t last_epoch;

				if (!holding && hold)
					debug(1, "holding");
				holding = hold;
				last_algo = curr_algo;
				last_epoch = curr_epoch;
				mqtt_poll(mqtt, 1);
				if (idle)
					idle = curr_algo == (int) last_algo &&
					    curr_epoch == last_epoch;
			} else {
				holding = 0;
				idle = !epoch_work(0);
				send_status(mqtt, idle);
			}
		}
		/*
		 * @@@ If we often have shutdowns that subsequently get
		 * cancelled, we may try to be a bit more efficient and just
		 * close the files but remember what we've already verified.
		 */
		epoch_shutdown();
	}
}


static void once(bool use_mqtt, const char *broker, bool just_one)
{
	mqtt_handle mqtt = use_mqtt ? mqtt_init(broker, 1) : NULL;

	epoch_init();
	while (!shutdown_pending && epoch_work(just_one))
		if (mqtt)
			send_status(mqtt, 0);
	send_status(mqtt, 1);
	mqtt_poll(mqtt, 1);
	epoch_shutdown();
}


static off_t get_space(const char *s)
{
	char *end;
	off_t n = strtoull(s, &end, 0);

	if (!*end && !n) {
		fprintf(stderr, "size limit must be non-zero\n");
		exit(1);
	}
	switch (*end) {
	case 0:
		break;
	case 'k':
		if (end[1])
			goto fail;
		n <<= 10;
		break;
	case 'M':
		if (end[1])
			goto fail;
		n <<= 20;
		break;
	case 'G':
		if (end[1])
			goto fail;
		n <<= 30;
		break;
	default:
	fail:
		fprintf(stderr, "invalid size \"%s\"\n", s);
		exit(1);
	}
	return n;
}


static off_t dag_cache_size(const char *s)
{
	const char *minus = strchr(s, '-');
	off_t reserve, size;
	struct statvfs vfs;
	char *tmp;

	if (!minus) {
		size = get_space(s);
		debug(1, "max cache size %llu bytes\n",
		    (unsigned long long) size);
		return size;
	}
	tmp = stralloc(s);
	tmp[minus - s] = 0;
	reserve = get_space(minus + 1);
	if (statvfs(tmp, &vfs) < 0) {
		perror(tmp);
		exit(1);
	}
	free(tmp);
	size = (off_t) vfs.f_frsize * vfs.f_blocks;
	if (size < reserve) {
		fprintf(stderr, "cannot reserve %llu bytes from %llu bytes\n",
		    (unsigned long long) reserve, (unsigned long long) size);
		exit(1);
	}
	debug(1, "max cache size %llu - %llu = %llu bytes\n",
	    (unsigned long long) size, (unsigned long long) reserve,
	    (unsigned long long) (size - reserve));
	return size - reserve;
}


static void usage(const char *name)
{
	fprintf(stderr,
"usage: %s [-1 [-1]] [-a algo] [-d ...] [-e epoch] [-M] [-m host[:port]]\n"
"       %*s[-s space|path-space] dag-fmt [csum-fmt]\n"
"       %s -g epoch\n"
"\n"
"  dag-fmt\n"
"    Printf-style format string that expands to the paths to DAG files.\n"
"    The algorithm name (string) is and the epoch number (unsigned) are given\n"
"    as parameters (for %%s and %%u, respectively).\n"
"  csum-fmt\n"
"    Printf-style format string that expands to the paths to checksum files.\n"
"    The algorithm name and the epoch number (unsigned) are given as\n"
"    parameters.\n"
"\n"
"  -1  One-shot operation: don't use MQTT and stop after updating the cache.\n"
"  -1 -1\n"
"      Verify or generate the DAG indicated with -a and -e, without checking\n"
"      available space, then exit.\n"
"  -a algorithm\n"
"      PoW algorithm, \"ethash\", \"etchash\" or \"ubqhash\". Default: ethash.\n"
"  -d  Increase debug level (default: no debug output)\n"
"  -e epoch\n"
"      Begin preparing DAGs starting at the indicated epoch (default: start\n"
"      with the first epoch in the cache or, if there is none, wait until an\n"
"      epoch is announced over MQTT)\n"
"  -g epoch\n"
"      generate the checksums for the specified epoch (on standard output)\n"
"  -M  if using one-shot mode (options -1 or -1 -1), still announce progress\n"
"      on MQTT.\n"
"  -m host[:port]\n"
"      Connect to the specified MQTT broker. Default is localhost:1883\n"
"  -s space\n"
"      Available space for DAGs (including the ones already present).\n"
"      The number is in bytes and the suffices k, M, and G can be used\n"
"      to multiply by 2^10, 2^20, and 2^30, respectively. (By default, the\n"
"      DAG cache is unlimited.)\n"
"  -s path-space\n"
"      The available DAG space is the size of the file system at \"path\",\n"
"      minus the specified space.\n"
"  --etchash=activation_epoch\n"
"      Set up ETChash (ECIP-1099) activation epoch. By default, the epoch\n"
"      390 is used for the algorithm change.\n"
    , name, (int) strlen(name) + 1, "", name);
	exit(1);
}


int main(int argc, char **argv)
{
	bool one_shot = 0;
	bool just_one = 0;
	const char *broker = NULL;
	bool generate = 0;
	bool status_on_mqtt = 0;
	char *end;
	int c;

	int longopt = 0;
	const struct option longopts[] = {
		{ "alt-epoch",	1,	&longopt,	'E' },
		{ "etchash",	1,	&longopt,	'e' },
		{ NULL,		0,	NULL,		0 }
	};

	while ((c = getopt_long(argc, argv, "1a:de:g:Mm:s:", longopts, NULL))
	    != EOF)
		switch (c) {
		case '1':
			just_one = one_shot;
			one_shot = 1;
			break;
		case 'a':
			curr_algo = dagalgo_code(optarg);
			if (curr_algo == -1) {
				fprintf(stderr, "unknown algorithm \"%s\"\n",
				    optarg);
				exit(1);
			}
			break;
		case 'd':
			debug_level++;
			break;
		case 'g':
			generate = 1;
			/* fall through */
		case 'e':
			curr_epoch = strtoul(optarg, &end, 0);
			if (*end)
				usage(*argv);
			if (curr_epoch > EPOCH_MAX) {
				fprintf(stderr,
				    "maximum epoch supported is %u\n",
				    EPOCH_MAX);
				exit(1);
			}
			break;
		case 'M':
			status_on_mqtt = 1;
			break;
		case 'm':
			broker = optarg;
			break;
		case 's':
			max_cache = dag_cache_size(optarg);
			break;
		case 0:
			switch (longopt) {
			case 'E':
				alt_epoch = strtoul(optarg, &end, 0);
				if (*end)
					usage(*argv);
				break;
			case 'e':
				etchash_epoch = strtoul(optarg, &end, 0);
				if (*end)
					usage(*argv);
				break;
			default:
				abort();
			}
			break;
		default:
			usage(*argv);
		}

	if (generate) {
		switch (argc - optind) {
		case 0:
			csum_generate(curr_algo, curr_epoch);
			return 0;
		default:
			usage(*argv);
		}
	}

	switch (argc - optind) {
	case 2:
		csum_path_template = argv[optind + 1];
		if (!template_valid(csum_path_template))
			usage(*argv);
		/* fall through */
	case 1:
		dag_path_template = argv[optind];
		if (!template_valid(dag_path_template))
			usage(*argv);
		break;
	default:
		usage(*argv);
	}

	if (one_shot)
		once(status_on_mqtt, broker, just_one);
	else
		loop(broker);

	return 0;
}
