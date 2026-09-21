/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal stand-in for the Gunyah mem-notifier API -- see gh_rm_drv.h
 * for why this exists. Registration always fails cleanly (returns
 * NULL, which qts_core.c already checks and handles as an error).
 */
#ifndef _GH_MEM_NOTIFIER_H
#define _GH_MEM_NOTIFIER_H

enum gh_mem_notifier_tag {
	GH_MEM_NOTIFIER_TAG_TOUCH_PRIMARY,
	GH_MEM_NOTIFIER_TAG_TOUCH_SECONDARY,
	GH_MEM_NOTIFIER_TAG_MAX,
};

typedef void (*gh_mem_notifier_handler)(enum gh_mem_notifier_tag tag,
		unsigned long notif_type, void *entry_data, void *notif_msg);

static inline void *gh_mem_notifier_register(enum gh_mem_notifier_tag tag,
		gh_mem_notifier_handler handler, void *data)
{
	return NULL;
}

static inline void gh_mem_notifier_unregister(void *cookie)
{
}

#endif /* _GH_MEM_NOTIFIER_H */
