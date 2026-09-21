/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal stand-in for the QCOM SDE panel event notifier API.
 *
 * The real implementation (drivers/soc/qcom/panel_event_notifier.c in a
 * newer CLO display-drivers release) isn't present at our merged tag.
 * Callers here only need registration to fail gracefully -- touch loses
 * panel blank/unblank awareness until the real subsystem is imported,
 * but still probes and functions otherwise.
 */
#ifndef _PANEL_EVENT_NOTIFIER_H_
#define _PANEL_EVENT_NOTIFIER_H_

#include <linux/types.h>
#include <drm/drm_panel.h>

enum panel_event_notifier_tag {
	PANEL_EVENT_NOTIFICATION_NONE,
	PANEL_EVENT_NOTIFICATION_PRIMARY,
	PANEL_EVENT_NOTIFICATION_SECONDARY,
	PANEL_EVENT_NOTIFICATION_MAX,
};

enum panel_event_notifier_client {
	PANEL_EVENT_NOTIFIER_CLIENT_PRIMARY_TOUCH,
	PANEL_EVENT_NOTIFIER_CLIENT_SECONDARY_TOUCH,
	PANEL_EVENT_NOTIFIER_CLIENT_PRIMARY_FP,
	PANEL_EVENT_NOTIFIER_CLIENT_MAX,
};

enum panel_event_notification_type {
	DRM_PANEL_EVENT_NONE,
	DRM_PANEL_EVENT_BLANK,
	DRM_PANEL_EVENT_UNBLANK,
	DRM_PANEL_EVENT_BLANK_LP,
	DRM_PANEL_EVENT_FPS_CHANGE,
	DRM_PANEL_EVENT_FOR_TOUCH,
};

struct panel_event_notification {
	enum panel_event_notification_type notif_type;
	union {
		bool early_trigger;
		struct {
			u32 old_fps;
			u32 new_fps;
		};
	} notif_data;
};

static inline void *panel_event_notifier_register(
		enum panel_event_notifier_tag tag,
		enum panel_event_notifier_client client,
		struct drm_panel *panel,
		void (*notifier_fn)(enum panel_event_notifier_tag tag,
				struct panel_event_notification *notification,
				void *client_data),
		void *client_data)
{
	return NULL;
}

static inline void panel_event_notifier_unregister(void *cookie)
{
}

#endif /* _PANEL_EVENT_NOTIFIER_H_ */
