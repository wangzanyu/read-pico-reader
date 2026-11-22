/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：kosync 协议的纯工具实现：URL 规范化、认证密钥、KOReader 文档 ID。
 *       只依赖 kosync_md5.h 与 C 标准库，宿主测试可直接编译。
 * English: Pure kosync protocol helpers: URL normalisation, auth key and KOReader document
 *       IDs. Depends only on kosync_md5.h and the C standard library so host tests compile it
 *       directly.
 *
 * 冻结：不联网、不读写配置；所有路径长度由调用方负责，函数自身只做有界读取。
 * Frozen: no networking and no config I/O; callers own path storage and every read here is
 *       bounded.
 */
#include "kosync_proto.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kosync_md5.h"

// 每个 partial MD5 块的读取上限与固定偏移表（0 与 1024<<(2*i)，i=0..10）。
// Chunk bound and the fixed offset table of the partial MD5 (0 and 1024<<(2*i), i=0..10).
#define KOSYNC_PART_CHUNK 1024u

static const uint64_t k_partial_offsets[12] = {
    0ull,        1024ull,      4096ull,       16384ull,     65536ull,      262144ull,
    1048576ull,  4194304ull,   16777216ull,   67108864ull,  268435456ull,  1073741824ull,
};

size_t kosync_normalize_url(const char* url, char* out, size_t cap) {
    if (!out || !cap) return 0;
    const char* value = (url && url[0]) ? url : KOSYNC_PROTO_DEFAULT_SERVER;
    static const char scheme[] = "http://";
    const bool needs_scheme = strstr(value, "://") == NULL;
    const size_t prefix = needs_scheme ? sizeof(scheme) - 1 : 0;
    size_t n = strlen(value);
    // 去掉尾部所有 '/'，避免拼出 "//users/auth"。
    // Strip every trailing '/' so API paths never gain a double slash.
    while (n > 0 && value[n - 1] == '/') n--;
    if (prefix + n + 1 > cap) return 0;
    if (needs_scheme) memcpy(out, scheme, prefix);
    memcpy(out + prefix, value, n);
    out[prefix + n] = 0;
    return prefix + n;
}

bool kosync_auth_key(const char* password, char out[33]) {
    if (!password || !out) return false;
    return kosync_md5_hex(password, strlen(password), out);
}

// 路径最后一段（最后一个 '/' 之后）；没有 '/' 时整串即文件名。
// The last path segment (after the final '/'); without a '/' the whole string is the name.
static const char* file_basename(const char* path) {
    const char* base = strrchr(path, '/');
    return base ? base + 1 : path;
}

bool kosync_document_id_binary(const char* path, char out[33]) {
    if (!path || !out) return false;
    FILE* file = fopen(path, "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return false;
    }
    rewind(file);
    kosync_md5_t md5;
    kosync_md5_init(&md5);
    uint8_t buffer[KOSYNC_PART_CHUNK];
    bool ok = true;
    for (unsigned i = 0; i < sizeof(k_partial_offsets) / sizeof(k_partial_offsets[0]); i++) {
        const uint64_t offset = k_partial_offsets[i];
        if (offset >= (uint64_t)size) continue;      // 偏移在文件末尾或之后：跳过 / at or past EOF: skip
        if (offset > (uint64_t)LONG_MAX) continue;    // 32 位 long 无法定位 / cannot seek on 32-bit long
        if (fseek(file, (long)offset, SEEK_SET) != 0) {
            ok = false;
            break;
        }
        const uint64_t remain = (uint64_t)size - offset;
        const size_t want = remain < KOSYNC_PART_CHUNK ? (size_t)remain : KOSYNC_PART_CHUNK;
        const size_t got = fread(buffer, 1, want, file);
        if (got != want) {
            ok = false;
            break;
        }
        kosync_md5_update(&md5, buffer, got);
    }
    fclose(file);
    if (!ok) return false;
    uint8_t digest[16];
    kosync_md5_final(&md5, digest);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 16; i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    out[32] = 0;
    return true;
}

bool kosync_document_id_filename(const char* path, char out[33]) {
    if (!path || !out) return false;
    const char* base = file_basename(path);
    if (!base[0]) return false;
    return kosync_md5_hex(base, strlen(base), out);
}

bool kosync_document_id(const char* path, bool binary, char out[33]) {
    return binary ? kosync_document_id_binary(path, out) : kosync_document_id_filename(path, out);
}
