// SPDX-License-Identifier: GPL-2.0-only
/*
 * CQHCI crypto engine (inline encryption) support
 *
 * Copyright 2020 Google LLC
 *
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
 *
 * drivers/mmc/host/cqhci-crypto.c - Qualcomm Technologies, Inc.
 *
 * Original source is taken from:
 * https://android.googlesource.com/kernel/common/+/4bac1109a10c55d49c0aa4f7ebdc4bc53cc368e8
 * The driver caters to crypto engine support for UFS controllers.
 * The crypto engine programming sequence, HW functionality and register
 * offset is almost same in UFS and eMMC controllers.
 */

#include <crypto/algapi.h>
#include <linux/blk-crypto.h>
#include <linux/keyslot-manager.h>
#include <linux/mmc/host.h>

#include "cqhci-crypto.h"
#include "../core/queue.h"

/* Map from blk-crypto modes to CQHCI crypto algorithm IDs and key sizes */
static const struct cqhci_crypto_alg_entry {
	enum cqhci_crypto_alg alg;
	enum cqhci_crypto_key_size key_size;
} cqhci_crypto_algs[BLK_ENCRYPTION_MODE_MAX] = {
	[BLK_ENCRYPTION_MODE_AES_256_XTS] = {
		.alg = CQHCI_CRYPTO_ALG_AES_XTS,
		.key_size = CQHCI_CRYPTO_KEY_SIZE_256,
	},
};

static inline struct cqhci_host *
cqhci_host_from_ksm(struct blk_keyslot_manager *ksm)
{
	struct mmc_host *mmc = container_of(ksm, struct mmc_host, ksm);

	return mmc->cqe_private;
}

static u8 get_data_unit_size_mask(unsigned int data_unit_size)
{
	if (data_unit_size < 512 || data_unit_size > 65536 ||
	    !is_power_of_2(data_unit_size))
		return 0;

	return data_unit_size / 512;
}

int cqhci_crypto_cap_find(void *host_p, enum blk_crypto_mode_num crypto_mode,
			  unsigned int data_unit_size)
{
	struct cqhci_host *host = host_p;
	enum cqhci_crypto_alg cqhci_alg;
	u8 data_unit_mask;
	int cap_idx;
	enum cqhci_crypto_key_size cqhci_key_size;
	union cqhci_crypto_cap_entry *ccap_array = host->crypto_cap_array;

	if (!cqhci_host_is_crypto_supported(host))
		return -EINVAL;

	switch (crypto_mode) {
	case BLK_ENCRYPTION_MODE_AES_256_XTS:
		cqhci_alg = CQHCI_CRYPTO_ALG_AES_XTS;
		cqhci_key_size = CQHCI_CRYPTO_KEY_SIZE_256;
		break;
	default:
		return -EINVAL;
	}

	data_unit_mask = get_data_unit_size_mask(data_unit_size);

	for (cap_idx = 0; cap_idx < host->crypto_capabilities.num_crypto_cap;
	     cap_idx++) {
		if (ccap_array[cap_idx].algorithm_id == cqhci_alg &&
		    (ccap_array[cap_idx].sdus_mask & data_unit_mask) &&
		    ccap_array[cap_idx].key_size == cqhci_key_size)
			return cap_idx;
	}

	return -EINVAL;
}
EXPORT_SYMBOL(cqhci_crypto_cap_find);

static int cqhci_crypto_program_key(struct cqhci_host *cq_host,
				    const union cqhci_crypto_cfg_entry *cfg,
				    int slot)
{
	u32 slot_offset = cq_host->crypto_cfg_register + slot * sizeof(*cfg);
	int i;

	if (cq_host->ops->program_key)
		return cq_host->ops->program_key(cq_host, cfg, slot);

	/* Clear CFGE */
	cqhci_writel(cq_host, 0, slot_offset + 16 * sizeof(cfg->reg_val[0]));

	/* Write the key */
	for (i = 0; i < 16; i++) {
		cqhci_writel(cq_host, le32_to_cpu(cfg->reg_val[i]),
			     slot_offset + i * sizeof(cfg->reg_val[0]));
	}
	/* Write dword 17 */
	cqhci_writel(cq_host, le32_to_cpu(cfg->reg_val[17]),
		     slot_offset + 17 * sizeof(cfg->reg_val[0]));
	/* Write dword 16, which includes the new value of CFGE */
	cqhci_writel(cq_host, le32_to_cpu(cfg->reg_val[16]),
		     slot_offset + 16 * sizeof(cfg->reg_val[0]));
	return 0;
}

static int cqhci_crypto_keyslot_program(struct blk_keyslot_manager *ksm,
					const struct blk_crypto_key *key,
					unsigned int slot)

{
	struct cqhci_host *cq_host = cqhci_host_from_ksm(ksm);
	const union cqhci_crypto_cap_entry *ccap_array =
		cq_host->crypto_cap_array;
	const struct cqhci_crypto_alg_entry *alg =
			&cqhci_crypto_algs[key->crypto_cfg.crypto_mode];
	u8 data_unit_mask = key->crypto_cfg.data_unit_size / 512;
	int i;
	int cap_idx = -1;
	union cqhci_crypto_cfg_entry cfg = {};
	int err;

	BUILD_BUG_ON(CQHCI_CRYPTO_KEY_SIZE_INVALID != 0);
	for (i = 0; i < cq_host->crypto_capabilities.num_crypto_cap; i++) {
		if (ccap_array[i].algorithm_id == alg->alg &&
		    ccap_array[i].key_size == alg->key_size &&
		    (ccap_array[i].sdus_mask & data_unit_mask)) {
			cap_idx = i;
			break;
		}
	}
	if (WARN_ON(cap_idx < 0))
		return -EOPNOTSUPP;

	cfg.data_unit_size = data_unit_mask;
	cfg.crypto_cap_idx = cap_idx;
	cfg.config_enable = CQHCI_CRYPTO_CONFIGURATION_ENABLE;

	if (ccap_array[cap_idx].algorithm_id == CQHCI_CRYPTO_ALG_AES_XTS) {
		/* In XTS mode, the blk_crypto_key's size is already doubled */
		memcpy(cfg.crypto_key, key->raw, key->size/2);
		memcpy(cfg.crypto_key + CQHCI_CRYPTO_KEY_MAX_SIZE/2,
		       key->raw + key->size/2, key->size/2);
	} else {
		memcpy(cfg.crypto_key, key->raw, key->size);
	}

	err = cqhci_crypto_program_key(cq_host, &cfg, slot);

	memzero_explicit(&cfg, sizeof(cfg));
	return err;
}

static int cqhci_crypto_clear_keyslot(struct cqhci_host *cq_host, int slot)
{
	union cqhci_crypto_cfg_entry cfg = {};

	return cqhci_crypto_program_key(cq_host, &cfg, slot);
}

static int cqhci_crypto_keyslot_evict(struct blk_keyslot_manager *ksm,
				      const struct blk_crypto_key *key,
				      unsigned int slot)
{
	struct cqhci_host *cq_host = cqhci_host_from_ksm(ksm);

	return cqhci_crypto_clear_keyslot(cq_host, slot);
}

/*
 * The keyslot management operations for CQHCI crypto.
 *
 * Note that the block layer ensures that these are never called while the host
 * controller is runtime-suspended.  However, the CQE won't necessarily be
 * "enabled" when these are called, i.e. CQHCI_ENABLE might not be set in the
 * CQHCI_CFG register.  But the hardware allows that.
 */
static const struct blk_ksm_ll_ops cqhci_ksm_ops = {
	.keyslot_program	= cqhci_crypto_keyslot_program,
	.keyslot_evict		= cqhci_crypto_keyslot_evict,
};

static enum blk_crypto_mode_num
cqhci_find_blk_crypto_mode(union cqhci_crypto_cap_entry cap)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cqhci_crypto_algs); i++) {
		BUILD_BUG_ON(CQHCI_CRYPTO_KEY_SIZE_INVALID != 0);
		if (cqhci_crypto_algs[i].alg == cap.algorithm_id &&
		    cqhci_crypto_algs[i].key_size == cap.key_size)
			return i;
	}
	return BLK_ENCRYPTION_MODE_INVALID;
}

/**
 * cqhci_crypto_init - initialize CQHCI crypto support
 * @cq_host: a cqhci host
 *
 * If the driver previously set MMC_CAP2_CRYPTO and the CQE declares
 * CQHCI_CAP_CS, initialize the crypto support.  This involves reading the
 * crypto capability registers, initializing the keyslot manager, clearing all
 * keyslots, and enabling 128-bit task descriptors.
 *
 * Return: 0 if crypto was initialized or isn't supported; whether
 *	   MMC_CAP2_CRYPTO remains set indicates which one of those cases it is.
 *	   Also can return a negative errno value on unexpected error.
 */
int cqhci_crypto_init(struct cqhci_host *cq_host)
{
	struct mmc_host *mmc = cq_host->mmc;
	struct device *dev = mmc_dev(mmc);
	struct blk_keyslot_manager *ksm = &mmc->ksm;
	unsigned int num_keyslots;
	unsigned int cap_idx;
	enum blk_crypto_mode_num blk_mode_num;
	unsigned int slot;
	int err = 0;

	if (!(mmc->caps2 & MMC_CAP2_CRYPTO) ||
	    !(cqhci_readl(cq_host, CQHCI_CAP) & CQHCI_CAP_CS))
		goto out;

	cq_host->crypto_capabilities.reg_val =
			cpu_to_le32(cqhci_readl(cq_host, CQHCI_CCAP));

	cq_host->crypto_cfg_register =
		(u32)cq_host->crypto_capabilities.config_array_ptr * 0x100;

	cq_host->crypto_cap_array =
		devm_kcalloc(dev, cq_host->crypto_capabilities.num_crypto_cap,
			     sizeof(cq_host->crypto_cap_array[0]), GFP_KERNEL);
	if (!cq_host->crypto_cap_array) {
		err = -ENOMEM;
		goto out;
	}

	/*
	 * CCAP.CFGC is off by one, so the actual number of crypto
	 * configurations (a.k.a. keyslots) is CCAP.CFGC + 1.
	 */
	num_keyslots = cq_host->crypto_capabilities.config_count + 1;

	err = devm_blk_ksm_init(dev, ksm, num_keyslots);
	if (err)
		goto out;

	ksm->ksm_ll_ops = cqhci_ksm_ops;
	ksm->dev = dev;

	/* Unfortunately, CQHCI crypto only supports 32 DUN bits. */
	ksm->max_dun_bytes_supported = 4;

	ksm->features = BLK_CRYPTO_FEATURE_STANDARD_KEYS |
			 BLK_CRYPTO_FEATURE_WRAPPED_KEYS;

	/*
	 * Cache all the crypto capabilities and advertise the supported crypto
	 * modes and data unit sizes to the block layer.
	 */
	for (cap_idx = 0; cap_idx < cq_host->crypto_capabilities.num_crypto_cap;
	     cap_idx++) {
		cq_host->crypto_cap_array[cap_idx].reg_val =
			cpu_to_le32(cqhci_readl(cq_host,
						CQHCI_CRYPTOCAP +
						cap_idx * sizeof(__le32)));
		blk_mode_num = cqhci_find_blk_crypto_mode(
					cq_host->crypto_cap_array[cap_idx]);
		if (blk_mode_num == BLK_ENCRYPTION_MODE_INVALID)
			continue;
		ksm->crypto_modes_supported[blk_mode_num] |=
			cq_host->crypto_cap_array[cap_idx].sdus_mask * 512;
	}

	/* Clear all the keyslots so that we start in a known state. */
	for (slot = 0; slot < num_keyslots; slot++)
		cqhci_crypto_clear_keyslot(cq_host, slot);

	/* CQHCI crypto requires the use of 128-bit task descriptors. */
	cq_host->caps |= CQHCI_TASK_DESC_SZ_128;

	return 0;

out:
	mmc->caps2 &= ~MMC_CAP2_CRYPTO;
	return err;
}

/*
 * Functions implementing eMMC v5.2 specification behaviour, used as the
 * fallback when no vendor crypto_vops are registered for this host (i.e.
 * plain CQHCI_CAP_CS-based crypto, already fully brought up by
 * cqhci_crypto_init() above).
 */
void cqhci_crypto_enable_spec(struct cqhci_host *host)
{
	if (!cqhci_host_is_crypto_supported(host))
		return;

	host->caps |= CQHCI_CAP_CRYPTO_SUPPORT;
}
EXPORT_SYMBOL(cqhci_crypto_enable_spec);

void cqhci_crypto_disable_spec(struct cqhci_host *host)
{
	host->caps &= ~CQHCI_CAP_CRYPTO_SUPPORT;
}
EXPORT_SYMBOL(cqhci_crypto_disable_spec);

/*
 * cqhci_host_init_crypto_spec - vendor-vops-absent fallback for
 * cqhci_host_init_crypto().
 *
 * The generic keyslot manager for this host is already fully initialized by
 * cqhci_crypto_init(), which cqhci_core.c calls unconditionally before
 * cqhci_host_init_crypto(). So when there is no vendor crypto_vops
 * implementation to hand this off to, there is nothing further to do here.
 */
int cqhci_host_init_crypto_spec(struct cqhci_host *host,
				const struct blk_ksm_ll_ops *ksm_ops)
{
	return 0;
}
EXPORT_SYMBOL(cqhci_host_init_crypto_spec);

void cqhci_crypto_setup_rq_keyslot_manager_spec(struct cqhci_host *host,
				struct request_queue *q)
{
	if (!cqhci_host_is_crypto_supported(host) || !q)
		return;

	blk_ksm_register(&host->mmc->ksm, q);
}
EXPORT_SYMBOL(cqhci_crypto_setup_rq_keyslot_manager_spec);

void cqhci_crypto_destroy_rq_keyslot_manager_spec(struct cqhci_host *host,
					      struct request_queue *q)
{
	blk_ksm_unregister(q);
}
EXPORT_SYMBOL(cqhci_crypto_destroy_rq_keyslot_manager_spec);

int cqhci_prepare_crypto_desc_spec(struct cqhci_host *host,
				struct mmc_request *mrq,
				u64 *ice_ctx)
{
	if (WARN_ON(mrq->crypto_ctx && !cqhci_is_crypto_enabled(host))) {
		/*
		 * Upper layer asked us to do inline encryption
		 * but that isn't enabled, so we fail this request.
		 */
		return -EINVAL;
	}

	if (ice_ctx)
		*ice_ctx = cqhci_crypto_prep_task_desc(mrq);

	return 0;
}
EXPORT_SYMBOL(cqhci_prepare_crypto_desc_spec);

/* Crypto Variant Ops Support */

void cqhci_crypto_enable(struct cqhci_host *host)
{
	if (host->crypto_vops && host->crypto_vops->enable)
		return host->crypto_vops->enable(host);

	return cqhci_crypto_enable_spec(host);
}
EXPORT_SYMBOL(cqhci_crypto_enable);

void cqhci_crypto_disable(struct cqhci_host *host)
{
	if (host->crypto_vops && host->crypto_vops->disable)
		return host->crypto_vops->disable(host);

	return cqhci_crypto_disable_spec(host);
}
EXPORT_SYMBOL(cqhci_crypto_disable);

int cqhci_host_init_crypto(struct cqhci_host *host)
{
	if (host->crypto_vops && host->crypto_vops->host_init_crypto)
		return host->crypto_vops->host_init_crypto(host,
							   &cqhci_ksm_ops);

	return cqhci_host_init_crypto_spec(host, &cqhci_ksm_ops);
}
EXPORT_SYMBOL(cqhci_host_init_crypto);

void cqhci_crypto_setup_rq_keyslot_manager(struct cqhci_host *host,
					    struct request_queue *q)
{
	if (host->crypto_vops && host->crypto_vops->setup_rq_keyslot_manager)
		return host->crypto_vops->setup_rq_keyslot_manager(host, q);

	return cqhci_crypto_setup_rq_keyslot_manager_spec(host, q);
}

void cqhci_crypto_destroy_rq_keyslot_manager(struct cqhci_host *host,
					      struct request_queue *q)
{
	if (host->crypto_vops && host->crypto_vops->destroy_rq_keyslot_manager)
		return host->crypto_vops->destroy_rq_keyslot_manager(host, q);

	return cqhci_crypto_destroy_rq_keyslot_manager_spec(host, q);
}

int cqhci_crypto_get_ctx(struct cqhci_host *host,
			       struct mmc_request *mrq,
			       u64 *ice_ctx)
{
	if (host->crypto_vops && host->crypto_vops->prepare_crypto_desc)
		return host->crypto_vops->prepare_crypto_desc(host, mrq,
								ice_ctx);

	return cqhci_prepare_crypto_desc_spec(host, mrq, ice_ctx);
}
EXPORT_SYMBOL(cqhci_crypto_get_ctx);

int cqhci_complete_crypto_desc(struct cqhci_host *host,
				struct mmc_request *mrq,
				u64 *ice_ctx)
{
	if (host->crypto_vops && host->crypto_vops->complete_crypto_desc)
		return host->crypto_vops->complete_crypto_desc(host, mrq,
								ice_ctx);

	return 0;
}
EXPORT_SYMBOL(cqhci_complete_crypto_desc);

void cqhci_crypto_debug(struct cqhci_host *host)
{
	if (host->crypto_vops && host->crypto_vops->debug)
		host->crypto_vops->debug(host);
}
EXPORT_SYMBOL(cqhci_crypto_debug);

void cqhci_crypto_set_vops(struct cqhci_host *host,
			struct cqhci_host_crypto_variant_ops *crypto_vops)
{
	host->crypto_vops = crypto_vops;
}
EXPORT_SYMBOL(cqhci_crypto_set_vops);

int cqhci_crypto_suspend(struct cqhci_host *host)
{
	if (host->crypto_vops && host->crypto_vops->suspend)
		return host->crypto_vops->suspend(host);

	return 0;
}
EXPORT_SYMBOL(cqhci_crypto_suspend);

int cqhci_crypto_resume(struct cqhci_host *host)
{
	if (host->crypto_vops && host->crypto_vops->resume)
		return host->crypto_vops->resume(host);

	return 0;
}

int cqhci_crypto_reset(struct cqhci_host *host)
{
	if (host->crypto_vops && host->crypto_vops->reset)
		return host->crypto_vops->reset(host);

	return 0;
}
EXPORT_SYMBOL(cqhci_crypto_reset);

int cqhci_crypto_recovery_finish(struct cqhci_host *host)
{
	if (host->crypto_vops && host->crypto_vops->recovery_finish)
		return host->crypto_vops->recovery_finish(host);

	/* Reset/Recovery might clear all keys, so reprogram all the keys. */
	if (host->mmc->ksm.num_slots)
		blk_ksm_reprogram_all_keys(&host->mmc->ksm);

	return 0;
}
EXPORT_SYMBOL(cqhci_crypto_recovery_finish);

MODULE_DESCRIPTION("CQHCI Crypto Engine Support");
MODULE_LICENSE("GPL v2");
