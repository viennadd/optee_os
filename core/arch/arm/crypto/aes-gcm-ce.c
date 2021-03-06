/*
 * Copyright (c) 2017, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <crypto/internal_aes-gcm.h>
#include <crypto/ghash-ce-core.h>
#include <io.h>
#include <kernel/panic.h>
#include <kernel/thread.h>
#include <tomcrypt.h>
#include <types_ext.h>

static void get_be_block(void *dst, const void *src)
{
	uint64_t *d = dst;

	d[1] = get_be64(src);
	d[0] = get_be64((const uint8_t *)src + 8);
}

static void put_be_block(void *dst, const void *src)
{
	const uint64_t *s = src;

	put_be64(dst, s[1]);
	put_be64((uint8_t *)dst + 8, s[0]);
}

TEE_Result internal_aes_gcm_set_key(struct internal_aes_gcm_ctx *ctx,
				    const void *key, size_t key_len)
{
	uint64_t k[2];
	uint64_t a;
	uint64_t b;

	if (aes_setup(key, key_len, 0, &ctx->skey))
		return TEE_ERROR_BAD_PARAMETERS;

	internal_aes_gcm_encrypt_block(ctx, ctx->ctr, ctx->hash_subkey);

	/* Store hash key in little endian and multiply by 'x' */
	b = get_be64(ctx->hash_subkey);
	a = get_be64(ctx->hash_subkey + 8);
	k[0] = (a << 1) | (b >> 63);
	k[1] = (b << 1) | (a >> 63);
	if (b >> 63)
		k[1] ^= 0xc200000000000000UL;

	memcpy(ctx->hash_subkey, k, TEE_AES_BLOCK_SIZE);
	return TEE_SUCCESS;
}

void internal_aes_gcm_ghash_update(struct internal_aes_gcm_ctx *ctx,
				   const void *head, const void *data,
				 size_t num_blocks)
{
	uint32_t vfp_state;
	uint64_t dg[2];
	uint64_t *k;

	get_be_block(dg, ctx->hash_state);

	k = (void *)ctx->hash_subkey;

	vfp_state = thread_kernel_enable_vfp();

#ifdef CFG_HWSUPP_PMULL
	pmull_ghash_update_p64(num_blocks, dg, data, k, head);
#else
	pmull_ghash_update_p8(num_blocks, dg, data, k, head);
#endif
	thread_kernel_disable_vfp(vfp_state);

	put_be_block(ctx->hash_state, dg);
}

#ifdef ARM64
void internal_aes_gcm_encrypt_block(struct internal_aes_gcm_ctx *ctx,
				    const void *src, void *dst)
{
	uint32_t vfp_state;
	void *enc_key = ctx->skey.rijndael.eK;
	size_t rounds = ctx->skey.rijndael.Nr;

	vfp_state = thread_kernel_enable_vfp();

	pmull_gcm_load_round_keys(enc_key, rounds);
	pmull_gcm_encrypt_block(dst, src, rounds);

	thread_kernel_disable_vfp(vfp_state);
}

void
internal_aes_gcm_update_payload_block_aligned(struct internal_aes_gcm_ctx *ctx,
					      TEE_OperationMode m,
					      const void *src,
					      size_t num_blocks, void *dst)
{
	uint32_t vfp_state;
	uint64_t dg[2];
	uint64_t ctr[2];
	uint64_t *k;
	void *enc_key = ctx->skey.rijndael.eK;
	size_t rounds = ctx->skey.rijndael.Nr;

	get_be_block(dg, ctx->hash_state);
	get_be_block(ctr, ctx->ctr);

	k = (void *)ctx->hash_subkey;

	vfp_state = thread_kernel_enable_vfp();

	pmull_gcm_load_round_keys(enc_key, rounds);

	if (m == TEE_MODE_ENCRYPT)
		pmull_gcm_encrypt(num_blocks, dg, dst, src, k, ctr, rounds,
				  ctx->buf_cryp);
	else
		pmull_gcm_decrypt(num_blocks, dg, dst, src, k, ctr, rounds);

	thread_kernel_disable_vfp(vfp_state);

	put_be_block(ctx->ctr, ctr);
	put_be_block(ctx->hash_state, dg);
}
#endif /*ARM64*/
