/*
 * csum.h - Generate DAG file checksums
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#ifndef DAGD_CSUM_H
#define	DAGD_CSUM_H

#include <stdint.h>

#include "linzhi/dag.h"
#include "linzhi/dagalgo.h"


#define CHUNK_BYTES	(1024 * 1024)
#define LINES_PER_CHUNK	(CHUNK_BYTES / DAG_LINE_BYTES)
#define	CSUM_BYTES	8


void csum_generate(enum dag_algo algo, uint16_t epoch);

#endif /* !DAGD_CSUM_H */
