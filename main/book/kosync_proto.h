/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：kosync 协议的纯工具：服务器 URL 规范化、认证密钥、KOReader 文档 ID
 *       （内容 partial MD5 与文件名 MD5）。只依赖 kosync_md5.h 与 C 标准库，
 *       不依赖 ESP-IDF，可在宿主直接测试；配置读写等设备逻辑仍在 book_kosync.c。
 * English: Pure kosync protocol helpers: server URL normalisation, auth key and KOReader
 *       document IDs (content partial MD5 and file-name MD5). Depends only on kosync_md5.h
 *       and the C standard library, never on ESP-IDF, so host tests compile it directly;
 *       device-side config handling stays in book_kosync.c.
 *
 * 冻结：本文件不做网络、不读写配置；输入路径按 TF 卡绝对路径处理。
 * Frozen: no networking and no config I/O here; paths are TF-card absolute paths.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 默认公共同步服务器；用户可在设置页改成自建服务器。
/// Default public sync server; the settings page accepts a self-hosted one.
#define KOSYNC_PROTO_DEFAULT_SERVER "https://sync.koreader.rocks"

/// URL 规范化：空串用默认服务器，无 "://" 补 "http://"，去掉尾部所有 '/'。
/// 返回写入长度（不含 NUL）；缓冲不够返回 0。
/// Normalise a base URL: empty means the default server, a missing scheme gets "http://",
/// and every trailing '/' is stripped. Returns the written length without NUL, or 0 when
/// the buffer is too small.
size_t kosync_normalize_url(const char* url, char* out, size_t cap);

/// 认证密钥：md5(明文密码) 的小写十六进制 32 字符；失败返回 false。
/// Auth key: lowercase hex md5 of the plain password (32 chars); false on failure.
bool kosync_auth_key(const char* password, char out[33]);

/// 内容文档 ID：KOReader partial MD5。依次在偏移 0 与 1024<<(2*i)（i=0..10，共 12 个）
/// 各读最多 1024 字节并连续做 MD5；偏移 >= 文件长度时跳过，末段按剩余长度截断。
/// 成功写 32 字符小写十六进制 + NUL。
/// Content document ID: the KOReader partial MD5. It reads at most 1024 bytes at offset 0
/// and at 1024<<(2*i) for i=0..10 (12 offsets in total) into one digest; offsets at or past
/// the file size are skipped and the last chunk is truncated. Writes 32 lowercase hex chars
/// plus NUL on success.
bool kosync_document_id_binary(const char* path, char out[33]);

/// 文件名文档 ID：路径最后一段（含扩展名、原样字节）的 MD5，小写十六进制。
/// File-name document ID: md5 of the last path segment including its extension, verbatim
/// bytes, lowercase hex.
bool kosync_document_id_filename(const char* path, char out[33]);

/// 按匹配方式取文档 ID；binary 为 true 用内容 partial MD5，否则用文件名 MD5。
/// Document ID by match mode; true selects the content partial MD5, false the file name.
bool kosync_document_id(const char* path, bool binary, char out[33]);

#ifdef __cplusplus
}
#endif
