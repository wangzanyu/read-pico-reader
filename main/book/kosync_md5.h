/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：RFC 1321 MD5 的有界实现；kosync 文档 ID 与认证密钥用，纯 C 可宿主测试。
 * English: Bounded RFC 1321 MD5 used for kosync document IDs and auth keys; plain C and
 *       host-testable.
 *
 * 冻结：只做摘要，不用于加密；输入长度用 size_t，输出固定 16 字节。
 * Frozen: digest only, never for encryption; size_t input length and a fixed 16-byte output.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state[4];
    uint64_t bytes;
    uint8_t buffer[64];
    size_t used;
} kosync_md5_t;

void kosync_md5_init(kosync_md5_t* ctx);
void kosync_md5_update(kosync_md5_t* ctx, const void* data, size_t len);
void kosync_md5_final(kosync_md5_t* ctx, uint8_t digest[16]);
/// 一次性摘要 / One-shot digest.
void kosync_md5(const void* data, size_t len, uint8_t digest[16]);
/// 小写十六进制 32 字符 + NUL；返回 true。/ Lowercase 32-char hex plus NUL; true on success.
bool kosync_md5_hex(const void* data, size_t len, char out[33]);

#ifdef __cplusplus
}
#endif
