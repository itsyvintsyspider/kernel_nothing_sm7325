/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MINMAX_H
#define _LINUX_MINMAX_H

/*
 * Compat shim for the upstream split of min()/max()/clamp() out of
 * linux/kernel.h into their own header. This tree predates that split
 * and still defines them (via __careful_cmp) directly in kernel.h, so
 * just pull that in for callers backported from newer kernels that
 * #include <linux/minmax.h> directly.
 */
#include <linux/kernel.h>

#endif /* _LINUX_MINMAX_H */
