/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2019 Google LLC
 *
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
 *
 */

#ifndef _CQHCI_CRYPTO_H
#define _CQHCI_CRYPTO_H

#include <linux/mmc/host.h>
#include <linux/module.h>
#include "cqhci.h"

#ifdef CONFIG_MMC_CRYPTO

int cqhci_crypto_init(struct cqhci_host *host);

/*
 * Returns the crypto bits that should be set in bits 64-127 of the
 * task descriptor.
 */
static inline u64 cqhci_crypto_prep_task_desc(struct mmc_request *mrq)
{
	if (!mrq->crypto_ctx)
		return 0;

	/* We set max_dun_bytes_supported=4, so all DUNs should be 32-bit. */
	WARN_ON_ONCE(mrq->crypto_ctx->bc_dun[0] > U32_MAX);

	return CQHCI_CRYPTO_ENABLE_BIT |
	       CQHCI_CRYPTO_KEYSLOT(mrq->crypto_key_slot) |
	       mrq->crypto_ctx->bc_dun[0];
}

#else /* CONFIG_MMC_CRYPTO */

static inline int cqhci_crypto_init(struct cqhci_host *host)
{
	return 0;
}

static inline u64 cqhci_crypto_prep_task_desc(struct mmc_request *mrq)
{
	return 0;
}

#endif /* !CONFIG_MMC_CRYPTO */

static inline int cqhci_num_keyslots(struct cqhci_host *host)
{
	return host->crypto_capabilities.config_count + 1;
}

static inline bool cqhci_keyslot_valid(struct cqhci_host *host,
				       unsigned int slot)
{
	/*
	 * The actual number of configurations supported is (CFGC+1), so slot
	 * numbers range from 0 to config_count inclusive.
	 */
	return slot < cqhci_num_keyslots(host);
}

static inline bool cqhci_host_is_crypto_supported(struct cqhci_host *host)
{
	return host->crypto_capabilities.reg_val != 0;
}

static inline bool cqhci_is_crypto_enabled(struct cqhci_host *host)
{
	return host->caps & CQHCI_CAP_CRYPTO_SUPPORT;
}

/* Functions implementing eMMC v5.2 specification behaviour */
int cqhci_prepare_crypto_desc_spec(struct cqhci_host *host,
				   struct mmc_request *mrq,
				   u64 *ice_ctx);

void cqhci_crypto_enable_spec(struct cqhci_host *host);

void cqhci_crypto_disable_spec(struct cqhci_host *host);

int cqhci_host_init_crypto_spec(struct cqhci_host *host,
				const struct blk_ksm_ll_ops *ksm_ops);

void cqhci_crypto_setup_rq_keyslot_manager_spec(struct cqhci_host *host,
						struct request_queue *q);

void cqhci_crypto_destroy_rq_keyslot_manager_spec(struct cqhci_host *host,
						  struct request_queue *q);

#if IS_ENABLED(CONFIG_MMC_CRYPTO) || IS_ENABLED(CONFIG_MMC_CQHCI_CRYPTO)
void cqhci_crypto_set_vops(struct cqhci_host *host,
			   struct cqhci_host_crypto_variant_ops *crypto_vops);
#else
static inline void cqhci_crypto_set_vops(struct cqhci_host *host,
			   struct cqhci_host_crypto_variant_ops *crypto_vops)
{}
#endif /* CONFIG_MMC_CRYPTO || CONFIG_MMC_CQHCI_CRYPTO */

/* Crypto Variant Ops Support */
#if IS_ENABLED(CONFIG_MMC_CRYPTO) || IS_ENABLED(CONFIG_MMC_CQHCI_CRYPTO)
void cqhci_crypto_enable(struct cqhci_host *host);

void cqhci_crypto_disable(struct cqhci_host *host);

int cqhci_host_init_crypto(struct cqhci_host *host);

int cqhci_crypto_get_ctx(struct cqhci_host *host,
			 struct mmc_request *mrq,
			 u64 *ice_ctx);

int cqhci_complete_crypto_desc(struct cqhci_host *host,
			       struct mmc_request *mrq,
			       u64 *ice_ctx);

void cqhci_crypto_debug(struct cqhci_host *host);

int cqhci_crypto_suspend(struct cqhci_host *host);

int cqhci_crypto_reset(struct cqhci_host *host);

int cqhci_crypto_recovery_finish(struct cqhci_host *host);


void cqhci_crypto_setup_rq_keyslot_manager(struct cqhci_host *host,
					   struct request_queue *q);
#else
static inline void cqhci_crypto_enable(struct cqhci_host *host)
{}

static inline void cqhci_crypto_disable(struct cqhci_host *host)
{}

static inline int cqhci_host_init_crypto(struct cqhci_host *host)
{
	return 0;
}

static inline int cqhci_crypto_get_ctx(struct cqhci_host *host,
			 struct mmc_request *mrq,
			 u64 *ice_ctx)
{
	return 0;
}

static inline int cqhci_complete_crypto_desc(struct cqhci_host *host,
			       struct mmc_request *mrq,
			       u64 *ice_ctx)
{
	return 0;
}

static inline void cqhci_crypto_debug(struct cqhci_host *host)
{}

static inline int cqhci_crypto_suspend(struct cqhci_host *host)
{
	return 0;
}

static inline int cqhci_crypto_reset(struct cqhci_host *host)
{
	return 0;
}

static inline int cqhci_crypto_recovery_finish(struct cqhci_host *host)
{
	return 0;
}

static inline void cqhci_crypto_setup_rq_keyslot_manager(struct cqhci_host *host,
					   struct request_queue *q)
{}
#endif /* CONFIG_MMC_CRYPTO || CONFIG_MMC_CQHCI_CRYPTO */

void cqhci_crypto_destroy_rq_keyslot_manager(struct cqhci_host *host,
					     struct request_queue *q);


int cqhci_crypto_resume(struct cqhci_host *host);

int cqhci_crypto_cap_find(void *host_p,  enum blk_crypto_mode_num crypto_mode,
			  unsigned int data_unit_size);

#endif /* _CQHCI_CRYPTO_H */
