/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：RFC 1321 MD5 的有界实现，供 kosync 文档 ID 与认证密钥使用。算法按规范实现，
 *       不依赖 mbedtls 低层 API，宿主测试可直接编译。
 * English: Bounded RFC 1321 MD5 for kosync document IDs and auth keys. Implemented from the
 *       specification without mbedtls low-level APIs so host tests compile it directly.
 *
 * 冻结：仅摘要用途；所有输入长度有界，失败路径清零上下文。
 * Frozen: digest use only; every input length is bounded and failure clears the context.
 */
#include "kosync_md5.h"

#include <string.h>

static const uint32_t k_shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9,
    14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static const uint32_t k_table[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static uint32_t rol(uint32_t x, uint32_t n) { return (x << n) | (x >> (32u - n)); }

static void transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t m[16];
    for (unsigned i = 0; i < 16; i++)
        m[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) | ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t f;
        unsigned g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5u * i + 1u) & 15u;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3u * i + 5u) & 15u;
        } else {
            f = c ^ (b | ~d);
            g = (7u * i) & 15u;
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rol(a + f + k_table[i] + m[g], k_shift[i]);
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void kosync_md5_init(kosync_md5_t* ctx) {
    if (!ctx) return;
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu;
    ctx->state[3] = 0x10325476u;
    ctx->bytes = 0;
    ctx->used = 0;
}

void kosync_md5_update(kosync_md5_t* ctx, const void* data, size_t len) {
    if (!ctx || (!data && len)) return;
    const uint8_t* p = (const uint8_t*)data;
    ctx->bytes += len;
    while (len) {
        size_t take = 64 - ctx->used;
        if (take > len) take = len;
        memcpy(ctx->buffer + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (ctx->used == 64) {
            transform(ctx->state, ctx->buffer);
            ctx->used = 0;
        }
    }
}

void kosync_md5_final(kosync_md5_t* ctx, uint8_t digest[16]) {
    if (!ctx || !digest) return;
    const uint64_t bits = ctx->bytes << 3;
    const uint8_t pad = 0x80;
    kosync_md5_update(ctx, &pad, 1);
    static const uint8_t zero = 0;
    while (ctx->used != 56) kosync_md5_update(ctx, &zero, 1);
    uint8_t length[8];
    for (unsigned i = 0; i < 8; i++) length[i] = (uint8_t)(bits >> (8u * i));
    kosync_md5_update(ctx, length, 8);
    for (unsigned i = 0; i < 4; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i]);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] >> 24);
    }
    memset(ctx, 0, sizeof(*ctx));
}

void kosync_md5(const void* data, size_t len, uint8_t digest[16]) {
    kosync_md5_t ctx;
    kosync_md5_init(&ctx);
    kosync_md5_update(&ctx, data, len);
    kosync_md5_final(&ctx, digest);
}

bool kosync_md5_hex(const void* data, size_t len, char out[33]) {
    if (!out) return false;
    uint8_t digest[16];
    kosync_md5(data, len, digest);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[32] = 0;
    return true;
}
