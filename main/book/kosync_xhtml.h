/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：EPUB 章节 XHTML 的段落/文本节点模型，用于 KOReader 进度 XPath 的生成与解析。
 *       与 main/book/html_text.c 的规范化规则对齐：同一份 XHTML 解析出的段落序号、
 *       段内规范化字节偏移必须与 html_text 的块表一致（调用方用 hash 对拍后才会使用）。
 *       XPath 语义对齐 CrossMux：元素同名兄弟序号、text()[k] 只数非空文本节点、
 *       注释/PI/CDATA 切分文本节点、偏移按 UTF-8 码点、仅元素路径与整章起点路径。
 *       图片占位符 U+FFFC 计入段落字节（html_text 如此），但不计入 XPath 文字码点。
 * English: Paragraph/text-node model of one EPUB spine XHTML chapter, used to build and
 *       resolve KOReader progress XPaths. Its paragraph ordinals and normalized byte
 *       offsets mirror main/book/html_text.c; callers verify the two agree before use.
 *       XPath semantics follow CrossMux: same-name element sibling indexes, text()[k]
 *       counting only non-empty text nodes, comment/PI/CDATA text-node boundaries,
 *       UTF-8 codepoint offsets, element-only paths and the chapter-start path.
 *       The U+FFFC image placeholder counts in paragraph bytes (as html_text does) but
 *       not in XPath text codepoints.
 *
 * 冻结：纯 C，不依赖 ESP-IDF/PSRAM，可在宿主测试直接编译；输入的 XHTML 由调用方持有。
 * Frozen: Plain C with no ESP-IDF or PSRAM dependency so host tests compile it directly;
 *       the input XHTML buffer stays owned by the caller.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// XPath 缓冲上限；KOReader 远端 XPath 也可能很长，解析时超出返回错误。
/// XPath buffer bound; overlong remote XPaths fail instead of truncating silently.
#define KOSYNC_XPATH_MAX 320

/// 一次解析最多认的段落数，与 html_text 的 HTML_TEXT_MAX_BLOCKS 同量级。
/// Maximum paragraphs recognized in one chapter, matching html_text's block cap.
#define KOSYNC_XHTML_MAX_BLOCKS 16384u

typedef struct kosync_xhtml kosync_xhtml_t;

/// 解析一章未压缩 XHTML（借用，须存活到 free）；成功交所有权。
/// Parse one uncompressed chapter XHTML (borrowed, must outlive the model); success transfers ownership.
/// 返回 0 成功，-1 失败（内存不足、XML 畸形或段落超限）。
/// Returns 0 on success and -1 on failure (out of memory, malformed XML or too many paragraphs).
int kosync_xhtml_parse(const char* html, size_t len, kosync_xhtml_t** out);

/// 释放模型，可传 NULL。/ Free the model; NULL is allowed.
void kosync_xhtml_free(kosync_xhtml_t* model);

/// 段落数；超限解析失败时为 0。/ Paragraph count; zero when parsing failed.
uint32_t kosync_xhtml_block_count(const kosync_xhtml_t* model);

/// 第 block 段规范化后的 UTF-8 字节数；越界返回 0。
/// Normalized UTF-8 byte length of paragraph `block`; out-of-range returns zero.
uint32_t kosync_xhtml_block_length(const kosync_xhtml_t* model, uint32_t block);

/// 第 block 段规范化正文的 FNV-1a 64 位指纹，用于和 html_text 块对拍。
/// FNV-1a 64-bit fingerprint of the paragraph's normalized text, compared with html_text.
uint64_t kosync_xhtml_block_hash(const kosync_xhtml_t* model, uint32_t block);

/// 同一算法，供调用方计算 html_text 块的指纹。
/// The same hash applied by callers to html_text block text.
uint64_t kosync_xhtml_hash(const char* data, size_t len);

/// 正向：第 block 段内规范化字节偏移 -> KOReader XPath。
///   `/body/DocFragment[spine+1]/body/<元素[同名序号]>.../text()[k].<码点偏移>`
/// Forward: normalized byte offset inside paragraph `block` to a KOReader XPath.
/// 返回 0 成功；-1 表示偏移越界（`norm_off >= 段长`）或该段没有可锚定的文字。
/// 整段只有图片占位时退到下一段的段首（图片不占 XPath 文字码点）。
/// Returns 0 on success; -1 for an out-of-range offset or a paragraph with no anchorable
/// text. A paragraph that only holds image placeholders falls back to the next paragraph's
/// start, because images never occupy XPath text codepoints.
int kosync_xhtml_xpath_for(const kosync_xhtml_t* model, uint32_t block, uint32_t norm_off,
                           unsigned spine_index, char* out, size_t cap);

/// 反向：KOReader XPath -> (block, 段内规范化字节偏移)。
/// 只接受本模型能对上的 XPath；DocFragment 编号必须等于 spine_index+1。
/// 整章起点路径（`…]/body` 或 `…]/body.0`）固定解析为第 0 段偏移 0。
/// Reverse: KOReader XPath to (block, normalized byte offset within that paragraph).
/// Only paths matching this model are accepted; DocFragment must equal spine_index+1.
/// A chapter-start path (`…]/body` or `…]/body.0`) resolves to block 0, offset 0.
/// 返回 0 成功；-1 表示 XPath 不合法、编号不匹配或目标节点不存在。
/// Returns 0 on success; -1 for an invalid path, a mismatched fragment number or a missing node.
int kosync_xhtml_resolve(const kosync_xhtml_t* model, const char* xpath, unsigned spine_index,
                         uint32_t* block, uint32_t* norm_off);

/// 退化 XPath：整章起点 `/body/DocFragment[spine+1]/body`。远端解析失败时由调用方兜底。
/// Degenerate chapter-start XPath used as a fallback when full resolution fails.
int kosync_xpath_chapter_only(unsigned spine_index, char* out, size_t cap);

#ifdef __cplusplus
}
#endif
