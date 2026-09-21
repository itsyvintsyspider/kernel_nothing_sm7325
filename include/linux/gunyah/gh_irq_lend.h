/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal stand-in for the Gunyah IRQ-lend API -- see gh_rm_drv.h for
 * why this exists. Every entry point fails cleanly.
 */
#ifndef _GH_IRQ_LEND_H
#define _GH_IRQ_LEND_H

#include <linux/types.h>
#include "gh_rm_drv.h"

enum gh_irq_label {
	GH_IRQ_LABEL_TRUSTED_TOUCH_PRIMARY,
	GH_IRQ_LABEL_TRUSTED_TOUCH_SECONDARY,
	GH_IRQ_LABEL_MAX,
};

typedef void (*gh_irq_lend_notify_cb)(void *data, unsigned long notif_type,
		enum gh_irq_label label);
typedef void (*gh_irq_lend_release_cb)(void *data, unsigned long notif_type,
		enum gh_irq_label label);

static inline int gh_irq_lend_v2(enum gh_irq_label label,
		enum gh_vm_names vm_name, int irq,
		gh_irq_lend_release_cb on_release, void *data)
{
	return -ENODEV;
}

static inline int gh_irq_lend_notify(enum gh_irq_label label)
{
	return -ENODEV;
}

static inline int gh_irq_wait_for_lend_v2(enum gh_irq_label label,
		enum gh_vm_names vm_name, gh_irq_lend_notify_cb on_lend,
		void *data)
{
	return -ENODEV;
}

static inline int gh_irq_accept(enum gh_irq_label label, int irq, int flags)
{
	return -ENODEV;
}

static inline int gh_irq_release(enum gh_irq_label label)
{
	return -ENODEV;
}

static inline int gh_irq_release_notify(enum gh_irq_label label)
{
	return -ENODEV;
}

static inline int gh_irq_reclaim(enum gh_irq_label label)
{
	return -ENODEV;
}

#endif /* _GH_IRQ_LEND_H */
