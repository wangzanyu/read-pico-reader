/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 1.2（Atom）feed 的流式解析器与纯工具函数：条目/导航/分页链接、实体解码、
 *       CDATA、RFC 3986 相对 URL 解析与下载文件名清洗。纯 C，可宿主测试。
 * English: Streaming OPDS 1.2 (Atom) feed parser plus pure helpers: entries/navigation and
 *       pagination links, entity decoding, CDATA, RFC 3986 URL resolution and download
 *       filename sanitizing. Plain C and host-testable.
 *
 * 冻结：只识别 Atom；OPDS 2.0 JSON、OAuth、LCP、间接获取与搜索不在范围内；未知元素/属性
 *       一律安全跳过，单个坏条目不影响整份 feed。
 * Frozen: Atom only; OPDS 2.0 JSON, OAuth, LCP, indirect acquisition and search are out of
 *       scope; unknown elements/attributes are skipped safely and one bad entry never fails
 *       the whole feed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 单份 feed 最多保留的条目数；超出后继续解析但只计数。
/// Maximum retained entries per feed; extra entries are only counted.
#define OPDS_MAX_ENTRIES 64
#define OPDS_TITLE_MAX 192
#define OPDS_AUTHOR_MAX 128
#define OPDS_ID_MAX 96
#define OPDS_URL_MAX 768
#define OPDS_TYPE_MAX 96
#define OPDS_REL_MAX 96
#define OPDS_FEED_TITLE_MAX 128

typedef enum {
    OPDS_ENTRY_NAVIGATION = 0, ///< 目录子页（rel=subsection 等）/ navigation subsection
    OPDS_ENTRY_BOOK,           ///< 可下载的 EPUB（acquisition 链接）/ downloadable EPUB
    OPDS_ENTRY_OTHER,          ///< 没有可用链接，界面跳过 / no usable link, skipped by the UI
} opds_entry_kind_t;

typedef struct {
    char title[OPDS_TITLE_MAX];
    char author[OPDS_AUTHOR_MAX];
    char id[OPDS_ID_MAX];
    char href[OPDS_URL_MAX];   ///< 已解析的绝对地址 / resolved absolute target
    char type[OPDS_TYPE_MAX];  ///< 命中链接的 MIME / matched link MIME
    char rel[OPDS_REL_MAX];    ///< 命中链接的 rel / matched link rel
    uint8_t kind;              ///< opds_entry_kind_t
    uint32_t length;           ///< 声明的字节数，0 表示未知 / declared length, 0 unknown
} opds_entry_t;

typedef struct {
    char feed_title[OPDS_FEED_TITLE_MAX];
    char self[OPDS_URL_MAX];
    char next[OPDS_URL_MAX];
    char previous[OPDS_URL_MAX];
    char first[OPDS_URL_MAX];
    char last[OPDS_URL_MAX];
    uint16_t entry_count;   ///< 已保存条目数 / retained entries
    uint16_t total_entries; ///< 解析到的条目总数 / parsed entries including truncated
    bool truncated;         ///< 是否因上限截断 / entries were truncated
} opds_feed_info_t;

typedef void* (*opds_alloc_fn)(void* ctx, size_t size);
typedef void (*opds_free_fn)(void* ctx, void* memory);

typedef struct {
    opds_alloc_fn alloc; ///< NULL 时用 malloc / malloc when NULL
    opds_free_fn free;   ///< NULL 时用 free / free when NULL
    void* ctx;           ///< 回调上下文 / callback context
} opds_feed_memory_t;

typedef struct opds_feed_parser opds_feed_parser_t;

/// 创建解析器；base_url 必须是绝对 URL，用于解析相对 href。
/// Create a parser; base_url must be an absolute URL used to resolve relative hrefs.
opds_feed_parser_t* opds_feed_parser_create(const char* base_url, const opds_feed_memory_t* memory);
/// 流式喂数据；返回 false 表示解析器内部错误（内存/非法状态），可提前 destroy。
/// Feed bytes incrementally; false means an internal (memory/state) error.
bool opds_feed_parser_feed(opds_feed_parser_t* parser, const char* data, size_t len);
/// 结束输入；返回是否完整收尾。/ Finish the input; reports a clean end.
bool opds_feed_parser_finish(opds_feed_parser_t* parser);
void opds_feed_parser_destroy(opds_feed_parser_t* parser);
/// 当前已保存条目数（可在 feed 过程中查询做渐进显示）。
/// Retained entry count (queryable during streaming for progressive display).
uint16_t opds_feed_parser_entry_count(const opds_feed_parser_t* parser);
bool opds_feed_parser_entry(const opds_feed_parser_t* parser, uint16_t index, opds_entry_t* out);
void opds_feed_parser_info(const opds_feed_parser_t* parser, opds_feed_info_t* out);

/// RFC 3986 §5.2 相对解析；out 为绝对 URL。base/ref 均须非空。
/// RFC 3986 §5.2 relative resolution; out becomes an absolute URL.
bool opds_resolve_url(const char* base, const char* ref, char* out, size_t cap);
/// 下载文件名：`作者 - 书名.epub` 清洗为 FAT 安全名（非法字符替换、100 字节上限）。
/// Download filename: `author - title.epub` sanitized for FAT.
bool opds_sanitize_filename(const char* author, const char* title, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
