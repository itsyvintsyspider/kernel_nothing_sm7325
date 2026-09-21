/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal stand-in for the Gunyah Resource Manager driver API.
 *
 * Not present anywhere in our CLO merges at this tag. qts_core.c's
 * "Trusted Touch" (secure VM handoff) feature calls into this
 * unconditionally on the PVM/host side, but we don't have the real
 * Gunyah RM subsystem -- every entry point here fails cleanly so
 * trusted-touch-enable reports a real error at runtime instead of
 * silently doing nothing. Base touch functionality doesn't depend
 * on any of this.
 */
#ifndef _GH_RM_DRV_H
#define _GH_RM_DRV_H

#include <linux/types.h>
#include <linux/err.h>

typedef u16 gh_vmid_t;
typedef u32 gh_memparcel_handle_t;

enum gh_vm_names {
	GH_SELF_VM,
	GH_PRIMARY_VM,
	GH_TRUSTED_VM,
	GH_VM_MAX,
};

#define GH_RM_ACL_R				0x1
#define GH_RM_ACL_W				0x2
#define GH_RM_ACL_X				0x4

#define GH_RM_TRANS_TYPE_LEND			0x1
#define GH_RM_TRANS_TYPE_SHARE			0x2

#define GH_RM_MEM_TYPE_IO			0x1
#define GH_RM_MEM_TYPE_NORMAL			0x2

#define GH_RM_MEM_ACCEPT_VALIDATE_ACL_ATTRS	BIT(1)
#define GH_RM_MEM_ACCEPT_VALIDATE_LABEL	BIT(2)
#define GH_RM_MEM_ACCEPT_DONE			BIT(7)

#define GH_RM_MEM_NOTIFY_OWNER_RELEASED	BIT(1)
#define GH_RM_MEM_NOTIFY_RECIPIENT_SHARED	BIT(3)

#define GH_RM_NOTIF_MEM_SHARED			0x1
#define GH_RM_NOTIF_MEM_RELEASED		0x2
#define GH_RM_NOTIF_VM_IRQ_RELEASED		0x3

struct gh_acl_entry {
	gh_vmid_t vmid;
	u32 perms;
} __packed;

struct gh_acl_desc {
	u32 n_acl_entries;
	struct gh_acl_entry acl_entries[];
} __packed;

struct gh_sgl_entry {
	u64 ipa_base;
	u64 size;
} __packed;

struct gh_sgl_desc {
	u16 n_sgl_entries;
	struct gh_sgl_entry sgl_entries[];
} __packed;

struct gh_notify_vmid_entry {
	gh_vmid_t vmid;
} __packed;

struct gh_notify_vmid_desc {
	u32 n_vmid_entries;
	struct gh_notify_vmid_entry vmid_entries[];
} __packed;

struct gh_rm_notif_mem_shared_payload {
	u8 trans_type;
	u32 label;
	u32 mem_type;
	gh_memparcel_handle_t mem_handle;
	/* fields beyond this point unused by qts_core */
};

struct gh_rm_notif_mem_released_payload {
	u32 mem_type;
	gh_memparcel_handle_t mem_handle;
};

static inline void gh_rm_get_vmid(enum gh_vm_names vm_name, gh_vmid_t *vmid)
{
	*vmid = 0;
}

static inline struct gh_sgl_desc *gh_rm_mem_accept(gh_memparcel_handle_t handle,
		u8 mem_type, u8 trans_type, u32 flags, u32 label,
		struct gh_acl_desc *acl_desc, struct gh_sgl_desc *sgl_desc,
		void *mem_attr, u16 map_vmid)
{
	return ERR_PTR(-ENODEV);
}

static inline int gh_rm_mem_release(gh_memparcel_handle_t handle, u32 flags)
{
	return -ENODEV;
}

static inline int gh_rm_mem_notify(gh_memparcel_handle_t handle, u32 flags,
		int mem_tag, void *vmid_desc)
{
	return -ENODEV;
}

static inline int gh_rm_mem_lend(u8 mem_type, u32 flags, u32 label,
		struct gh_acl_desc *acl_desc, struct gh_sgl_desc *sgl_desc,
		void *mem_attr, gh_memparcel_handle_t *handle)
{
	return -ENODEV;
}

static inline int gh_rm_mem_reclaim(gh_memparcel_handle_t handle, u32 flags)
{
	return -ENODEV;
}

#endif /* _GH_RM_DRV_H */
