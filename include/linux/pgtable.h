/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PGTABLE_H
#define _LINUX_PGTABLE_H

/*
 * Compat shim for the upstream v5.9 include/linux/pgtable.h rename
 * (asm/pgtable.h -> linux/pgtable.h). This tree predates that reorg;
 * callers backported from newer kernels that #include <linux/pgtable.h>
 * only need the arch pgtable definitions, so just redirect.
 */
#include <asm/pgtable.h>

#endif /* _LINUX_PGTABLE_H */
