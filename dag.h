/*
 * dag.h - DAG file generation
 *
 * Copyright (C) 2021 Linzhi Ltd.
 *
 * This work is licensed under the terms of the MIT License.
 * A copy of the license can be found in the file COPYING.txt
 */

#ifndef DAGD_DAG_H
#define	DAGD_DAG_H

#include <stdbool.h>

#include "epoch.h"


/*
 * work_on returns 1 if the work done (if any) was successful, 0 if work was
 * attempted but failed.
 */

bool work_on(struct epoch *e);
void dag_init(void);

#endif /* !DAGD_DAG_H */
