// SPDX-License-Identifier: GPL-2.0
/*
 * Nothing OS manual GC for SK hynix UFS ("nt" sysfs group).
 * Ported from Pong's ufs-qcom.c to the in-core ufshcd; state lives in
 * struct ufsf_feature instead of struct ufs_qcom_host.
 *
 * /sys/devices/.../1d84000.ufshc/nt/manual_gc       (rw)
 * /sys/devices/.../1d84000.ufshc/nt/manual_gc_hold  (rw, ms)
 */

#include <linux/pm_runtime.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include "ufshcd.h"
#include "ufsfeature.h"

#define UFSF_MGC_HOLD_HIBERN8		10000	/* 10 seconds */
#define UFSF_MGC_HOLD_HIBERN8_MAX	10000
#define UFSF_MGC_HOLD_HIBERN8_MIN	2000

#define UFSF_ATTR_IDN_MANUAL_GC_CONT	0x12
#define UFSF_ATTR_IDN_MANUAL_GC_STATUS	0x13

enum {
	MANUAL_GC_OFF = 0,
	MANUAL_GC_ON,
	MANUAL_GC_DISABLE,
	MANUAL_GC_ENABLE,
	MANUAL_GC_MAX,
};

enum {
	MANUAL_GC_STATUS_CLEAN = 0,
	MANUAL_GC_STATUS_PAUSE,
	MANUAL_GC_STATUS_DIRTY,
	MANUAL_GC_STATUS_MAX,
};

static inline struct ufsf_manual_gc *mgc_of(struct ufs_hba *hba)
{
	return &hba->ufsf.manual_gc;
}

static ssize_t manual_gc_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct ufsf_manual_gc *mgc = mgc_of(hba);
	u32 status = MANUAL_GC_OFF;

	if (mgc->state == MANUAL_GC_DISABLE)
		return scnprintf(buf, PAGE_SIZE, "%s", "disabled\n");

	if (mgc->hagc_support) {
		int err;

		if (!(hba->eh_flags & UFSF_EH_IN_PROGRESS)) {
			pm_runtime_get_sync(hba->dev);
			err = ufshcd_query_attr_retry(hba,
				UPIU_QUERY_OPCODE_READ_ATTR,
				UFSF_ATTR_IDN_MANUAL_GC_STATUS, 0, 0, &status);
			pm_runtime_put_sync(hba->dev);
			mgc->hagc_support = err ? false : true;
		}
	}

	if (!mgc->hagc_support)
		return scnprintf(buf, PAGE_SIZE, "%s", "bkops\n");

	dev_dbg(dev, "%s status=%d\n", __func__, status);
	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 status == MANUAL_GC_STATUS_CLEAN ? "GREEN" :
			 status == MANUAL_GC_STATUS_PAUSE ? "YELLOW" :
			 status == MANUAL_GC_STATUS_DIRTY ? "RED" : "UNKNOWN");
}

static int manual_gc_enable(struct ufs_hba *hba, u32 *value)
{
	int ret;

	if (hba->eh_flags & UFSF_EH_IN_PROGRESS)
		return -EBUSY;

	dev_dbg(hba->dev, "%s value=%d\n", __func__, *value);
	pm_runtime_get_sync(hba->dev);
	ret = ufshcd_query_attr_retry(hba, UPIU_QUERY_OPCODE_WRITE_ATTR,
				      UFSF_ATTR_IDN_MANUAL_GC_CONT, 0, 0,
				      value);
	pm_runtime_put_sync(hba->dev);
	dev_dbg(hba->dev, "%s ret=%d\n", __func__, ret);
	return ret;
}

static ssize_t manual_gc_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	struct ufsf_manual_gc *mgc = mgc_of(hba);
	u32 value;
	int err = 0;
	u8 index = 0;

	if (kstrtou32(buf, 0, &value))
		return -EINVAL;

	if (value >= MANUAL_GC_MAX)
		return -EINVAL;

	if (hba->eh_flags & UFSF_EH_IN_PROGRESS)
		return -EBUSY;

	if (value == MANUAL_GC_DISABLE || value == MANUAL_GC_ENABLE) {
		mgc->state = value;
		return count;
	}
	if (mgc->state == MANUAL_GC_DISABLE)
		return count;

	if (mgc->hagc_support)
		mgc->hagc_support = manual_gc_enable(hba, &value) ? false : true;

	pm_runtime_get_sync(hba->dev);

	if (!mgc->hagc_support) {
		/* fallback: bkops + WB flush */
		enum query_opcode opcode = (value == MANUAL_GC_ON) ?
						UPIU_QUERY_OPCODE_SET_FLAG :
						UPIU_QUERY_OPCODE_CLEAR_FLAG;

		err = ufshcd_bkops_ctrl(hba, (value == MANUAL_GC_ON) ?
					BKOPS_STATUS_NON_CRITICAL :
					BKOPS_STATUS_CRITICAL);
		if (!hba->auto_bkops_enabled)
			err = -EAGAIN;
		index = ufshcd_wb_get_query_index(hba);

		ufshcd_query_flag_retry(hba, opcode,
				QUERY_FLAG_IDN_WB_BUFF_FLUSH_DURING_HIBERN8,
				index, NULL);
		ufshcd_query_flag_retry(hba, opcode,
				QUERY_FLAG_IDN_WB_BUFF_FLUSH_EN, index, NULL);
	}

	if (err || hrtimer_active(&mgc->hrtimer)) {
		pm_runtime_put_sync(hba->dev);
		return count;
	}

	/* pm_runtime_put_sync() happens in mgc_hibern8_work after delay_ms */
	hrtimer_start(&mgc->hrtimer, ms_to_ktime(mgc->delay_ms),
		      HRTIMER_MODE_REL);
	return count;
}
static DEVICE_ATTR_RW(manual_gc);

static ssize_t manual_gc_hold_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%lu\n", mgc_of(hba)->delay_ms);
}

static ssize_t manual_gc_hold_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct ufs_hba *hba = dev_get_drvdata(dev);
	unsigned long value;

	if (kstrtoul(buf, 0, &value))
		return -EINVAL;

	if (value < UFSF_MGC_HOLD_HIBERN8_MIN ||
	    value > UFSF_MGC_HOLD_HIBERN8_MAX) {
		dev_err(dev, "%s Invalid value: %lu\n", __func__, value);
		return -EINVAL;
	}
	mgc_of(hba)->delay_ms = value;
	return count;
}
static DEVICE_ATTR_RW(manual_gc_hold);

static enum hrtimer_restart mgc_hrtimer_handler(struct hrtimer *timer)
{
	struct ufsf_manual_gc *mgc = container_of(timer, struct ufsf_manual_gc,
						  hrtimer);

	queue_work(mgc->mgc_workq, &mgc->hibern8_work);
	return HRTIMER_NORESTART;
}

static void mgc_hibern8_work(struct work_struct *work)
{
	struct ufsf_manual_gc *mgc = container_of(work, struct ufsf_manual_gc,
						  hibern8_work);
	struct ufsf_feature *ufsf = container_of(mgc, struct ufsf_feature,
						 manual_gc);

	pm_runtime_put_sync(ufsf->hba->dev);
	/* bkops will be disabled when power down */
}

static struct attribute *ufs_nt_sysfs_attrs[] = {
	&dev_attr_manual_gc.attr,
	&dev_attr_manual_gc_hold.attr,
	NULL
};

static const struct attribute_group ufs_nt_sysfs_group = {
	.name = "nt",
	.attrs = ufs_nt_sysfs_attrs,
};

/* called once the device descriptor is known (vendor id valid) */
void ufs_manual_gc_sysfs_init(struct ufs_hba *hba)
{
	struct ufsf_manual_gc *mgc = mgc_of(hba);
	int ret;

	if (mgc->sysfs_created ||
	    hba->dev_info.wmanufacturerid != UFS_VENDOR_SKHYNIX)
		return;

	ret = sysfs_create_group(&hba->dev->kobj, &ufs_nt_sysfs_group);
	if (ret)
		dev_err(hba->dev,
			"%s: Failed to create manual_gc sysfs group (err = %d)\n",
			__func__, ret);
	else
		mgc->sysfs_created = true;
}

void ufs_manual_gc_init(struct ufs_hba *hba)
{
	struct ufsf_manual_gc *mgc = mgc_of(hba);
	char wq_name[32];

	mgc->state = MANUAL_GC_ENABLE;
	mgc->hagc_support = true;
	mgc->sysfs_created = false;
	mgc->delay_ms = UFSF_MGC_HOLD_HIBERN8;

	hrtimer_init(&mgc->hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	mgc->hrtimer.function = mgc_hrtimer_handler;

	INIT_WORK(&mgc->hibern8_work, mgc_hibern8_work);
	snprintf(wq_name, ARRAY_SIZE(wq_name), "ufs_mgc_hibern8_work_%d",
		 hba->host->host_no);
	mgc->mgc_workq = create_singlethread_workqueue(wq_name);
}

void ufs_manual_gc_remove(struct ufs_hba *hba)
{
	struct ufsf_manual_gc *mgc = mgc_of(hba);

	if (mgc->sysfs_created) {
		sysfs_remove_group(&hba->dev->kobj, &ufs_nt_sysfs_group);
		mgc->sysfs_created = false;
	}
	hrtimer_cancel(&mgc->hrtimer);
	if (mgc->mgc_workq) {
		destroy_workqueue(mgc->mgc_workq);
		mgc->mgc_workq = NULL;
	}
}
