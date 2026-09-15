/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2019 Google LLC
 */
#ifndef __LINUX_BIO_CRYPT_CTX_H
#define __LINUX_BIO_CRYPT_CTX_H

/*
 * This tree's block layer (block/blk-crypto.c, block/blk-crypto-internal.h)
 * and its consumers (fs/crypto/inline_crypt.c, ufshcd-crypto.c,
 * cqhci-crypto.c) all still speak the older, self-contained
 * struct blk_crypto_key / struct bio_crypt_ctx layout defined in
 * <linux/blk-crypto.h> (the one with the nested "crypto_cfg" config
 * struct), not the newer keyslot-manager-based layout ("bc_ksm" /
 * "bc_keyslot", packed "hash" field) this file originally carried in from
 * upstream. That newer layout was only ever consumed by
 * drivers/block/virtio_blk.c and drivers/scsi/ufs/ufshcd-crypto-qti.c,
 * both of which are gated off (CONFIG_VIRTIO_BLK, CONFIG_SCSI_UFS_CRYPTO_QTI)
 * for this device, and defining both layouts here caused
 * redefinition/conflicting-type errors wherever <linux/bio.h> (which always
 * pulls this header in) and <linux/blk-crypto.h> were both included.
 *
 * Pull in only <linux/blk-crypto.h>'s type definitions
 * (__LINUX_BLK_CRYPTO_TYPES_DEFINED), not the whole header: this file is
 * itself included from deep inside <linux/bio.h>, before bio.h has finished
 * defining bio_page()/bio_offset()/bio_cur_bytes()/struct bio_set, so
 * pulling in blk-crypto.h's own <linux/blkdev.h> include here would create
 * a circular-include ordering bug (blkdev.h using bio.h symbols that don't
 * exist yet). blk-crypto.h's <linux/blkdev.h>-dependent declarations get
 * included normally by whichever .c file #includes <linux/blk-crypto.h>
 * directly.
 */
#include <linux/types.h>

#ifndef __LINUX_BLK_CRYPTO_TYPES_DEFINED
#define __LINUX_BLK_CRYPTO_TYPES_DEFINED

enum blk_crypto_mode_num {
	BLK_ENCRYPTION_MODE_INVALID,
	BLK_ENCRYPTION_MODE_AES_256_XTS,
	BLK_ENCRYPTION_MODE_AES_128_CBC_ESSIV,
	BLK_ENCRYPTION_MODE_ADIANTUM,
	BLK_ENCRYPTION_MODE_MAX,
};

#define BLK_CRYPTO_MAX_KEY_SIZE		64
#define BLK_CRYPTO_MAX_WRAPPED_KEY_SIZE                128

struct blk_crypto_config {
	enum blk_crypto_mode_num crypto_mode;
	unsigned int data_unit_size;
	unsigned int dun_bytes;
	bool is_hw_wrapped;
};

struct blk_crypto_key {
	struct blk_crypto_config crypto_cfg;
	unsigned int data_unit_size_bits;
	unsigned int size;
	u8 raw[BLK_CRYPTO_MAX_WRAPPED_KEY_SIZE];
};

#define BLK_CRYPTO_MAX_IV_SIZE		32
#define BLK_CRYPTO_DUN_ARRAY_SIZE	(BLK_CRYPTO_MAX_IV_SIZE / sizeof(u64))

struct bio_crypt_ctx {
	const struct blk_crypto_key	*bc_key;
	u64				bc_dun[BLK_CRYPTO_DUN_ARRAY_SIZE];
};

#endif /* __LINUX_BLK_CRYPTO_TYPES_DEFINED */

#endif /* __LINUX_BIO_CRYPT_CTX_H */
