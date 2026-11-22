/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：EPUB 章节 XHTML 的段落/文本节点模型，用于 KOReader 进度 XPath 的生成与解析。
 *       段落划分、空白折叠、实体、注释/声明、跳过容器与图片占位规则逐条对齐
 *       main/book/html_text.c；调用方先用 kosync_xhtml_block_hash() 与 html_text 的块表对拍，
 *       全部一致才使用本模型。XPath 语义（元素兄弟序号、text() 非空节点计数、注释/PI/CDATA
 *       切分文本节点、UTF-8 码点偏移、仅元素路径、整章起点路径）对齐 CrossMux
 *       lib/KOReaderSync/ChapterXPathResolver 与 ProgressMapper。
 * English: Paragraph/text-node model of one EPUB spine XHTML chapter for KOReader progress
 *       XPath build/resolve. Paragraph splitting, whitespace collapsing, entities,
 *       comments/declarations, skipped containers and image placeholders mirror
 *       main/book/html_text.c; callers verify every paragraph hash against html_text before
 *       using the model. XPath semantics (same-name sibling indexes, non-empty text() node
 *       numbering, comment/PI/CDATA text-node boundaries, UTF-8 codepoint offsets,
 *       element-only paths, chapter-start path) follow CrossMux's
 *       lib/KOReaderSync/ChapterXPathResolver and ProgressMapper.
 *
 * 冻结：纯 C，不依赖 ESP-IDF/PSRAM；输入 XHTML 借用，必须活到模型释放之后。
 * Frozen: plain C, no ESP-IDF/PSRAM dependency; the input XHTML is borrowed and must outlive
 *       the model.
 */
#include "kosync_xhtml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define KS_MAX_DEPTH 64
#define KS_NAME_CAP 16
#define KS_CHILD_CAP 24
#define KS_NAME_POOL_MAX 128
#define KS_IMAGE_MAX 256u
#define KS_IMAGE_PATH_MAX 511u
#define KS_FNV_BASIS 14695981039346656037ull
#define KS_FNV_PRIME 1099511628211ull

typedef struct {
    uint32_t hash;  // 子元素本地名 hash / local-name hash
    uint16_t count; // 已出现的同名子元素数 / same-name children seen
} ks_child_t;

typedef struct {
    uint16_t name_id;    // 名称池下标 / index into the name pool
    uint16_t sibling;    // 同名兄弟序号，从 1 起 / 1-based same-name sibling index
    uint32_t node_cp;    // 当前文本节点内已消耗码点数 / codepoints consumed in the current node
    uint16_t text_nodes; // 已开始的文本节点数 / text nodes started
    bool pending_text;   // 下一个字符开始新文本节点 / next character starts a new node
    uint16_t child_count;
    ks_child_t children[KS_CHILD_CAP];
} ks_elem_t;

typedef struct {
    uint64_t hash;
    uint32_t len;
} ks_block_t;

typedef enum {
    KS_PASS_COUNT = 0, // 只统计段落，用于对拍 / count paragraphs and fingerprints
    KS_PASS_XPATH,     // 目标段落偏移 -> XPath / paragraph offset to XPath
    KS_PASS_RESOLVE,   // XPath -> 段落偏移 / XPath to paragraph offset
} ks_pass_t;

struct kosync_xhtml {
    const char* html;
    size_t len;
    unsigned spine_index;
    uint16_t name_pool_count;
    char names[KS_NAME_POOL_MAX][KS_NAME_CAP];
    ks_block_t* blocks;
    uint32_t block_count;
    uint32_t block_cap;

    // 行走状态 / walk state
    ks_elem_t depth[KS_MAX_DEPTH];
    unsigned depth_count; // 已入栈元素数 / pushed elements
    bool inside_body;
    bool saw_body;
    bool active;           // 当前段正在收字 / current paragraph collecting text
    bool space;            // 待折叠空白 / pending collapsed whitespace
    uint32_t active_block; // 当前段下标，仅 active 时有效 / active paragraph index
    uint32_t block_bytes;  // 当前段已收规范化字节 / normalized bytes in the active paragraph
    uint64_t block_hash;   // 当前段 FNV-1a / active paragraph FNV-1a
    uint32_t image_count;  // 已记入的图片数（html_text 的 HTML_IMAGE_MAX 口径）
    bool note_open;        // 处于同章脚注 <a href="#..."> 内 / inside a footnote anchor
    bool note_image;       // 该注号已收过图片占位 / marker image already emitted

    // 目标与结果 / query targets and results
    ks_pass_t pass;
    uint32_t target_block;
    uint32_t target_off;
    int status; // 0 成功，-1 失败 / 0 success, -1 failure
    bool found;
    char result[KOSYNC_XPATH_MAX];

    // 解析后的 XPath / parsed XPath
    struct {
        uint16_t count; // 元素步数 / element steps
        struct {
            uint32_t hash;
            uint16_t index;
        } steps[KS_MAX_DEPTH];
        uint16_t text_index; // text()[N]，0 表示无文本尾部 / 0 means element-only path
        uint32_t text_off;
        bool has_text;
    } path;
};

// 参与折叠/计数的空白：ASCII 空白与 U+00A0（与 html_text 的 emit() 一致）。
// Whitespace that collapses or counts: ASCII space plus U+00A0, exactly like html_text emit().
static bool ks_space_cp(uint32_t cp) {
    return cp == 0xa0 || (cp < 128 && (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f'));
}

static bool ks_ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static char ks_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c; }

static bool ks_name_char(char c) {
    c = ks_lower(c);
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '-' || c == '_';
}

uint64_t kosync_xhtml_hash(const char* data, size_t len) {
    uint64_t h = KS_FNV_BASIS;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)data[i];
        h *= KS_FNV_PRIME;
    }
    return h;
}

static void ks_hash_update(uint64_t* h, const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        *h ^= (unsigned char)data[i];
        *h *= KS_FNV_PRIME;
    }
}

static uint32_t ks_hash_name(const char* name) {
    uint64_t h = 2166136261u;
    for (const char* p = name; *p; p++) {
        h ^= (unsigned char)ks_lower(*p);
        h *= 16777619u;
    }
    return (uint32_t)h;
}

static bool ks_name_equal(const char* a, const char* b) { return strcmp(a, b) == 0; }

// 命名空间前缀只保留本地名：`svg:image` -> `image`，并写入名称池。
// Keep only the local name for XPath (`svg:image` -> `image`) and intern it.
static int ks_name_intern(kosync_xhtml_t* m, const char* raw) {
    const char* colon = strrchr(raw, ':');
    const char* start = colon ? colon + 1 : raw;
    size_t n = strlen(start);
    if (n >= KS_NAME_CAP) return -1;
    char local[KS_NAME_CAP];
    for (size_t i = 0; i < n; i++) local[i] = ks_lower(start[i]);
    local[n] = 0;
    for (uint16_t i = 0; i < m->name_pool_count; i++) {
        if (strcmp(m->names[i], local) == 0) return (int)i;
    }
    if (m->name_pool_count >= KS_NAME_POOL_MAX) return -1;
    memcpy(m->names[m->name_pool_count], local, n + 1);
    return (int)m->name_pool_count++;
}

static bool ks_block_reserve(kosync_xhtml_t* m) {
    if (m->block_count < m->block_cap) return true;
    if (m->block_count >= KOSYNC_XHTML_MAX_BLOCKS) {
        m->status = -1;
        return false;
    }
    uint32_t cap = m->block_cap ? m->block_cap * 2 : 64;
    if (cap > KOSYNC_XHTML_MAX_BLOCKS) cap = KOSYNC_XHTML_MAX_BLOCKS;
    ks_block_t* next = (ks_block_t*)realloc(m->blocks, cap * sizeof(*next));
    if (!next) {
        m->status = -1;
        return false;
    }
    m->blocks = next;
    m->block_cap = cap;
    return true;
}

static void ks_begin_block(kosync_xhtml_t* m) {
    m->active = true;
    m->space = false;
    m->block_bytes = 0;
    m->block_hash = KS_FNV_BASIS;
    m->active_block = m->block_count;
}

static void ks_finish_block(kosync_xhtml_t* m) {
    if (!m->active) {
        m->space = false;
        return;
    }
    m->active = false;
    m->space = false;
    if (m->status != 0 || m->block_bytes == 0) return;
    if (!ks_block_reserve(m)) return;
    m->blocks[m->block_count].hash = m->block_hash;
    m->blocks[m->block_count].len = m->block_bytes;
    m->block_count++;
}

// 把规范化的一个码点写入当前段；空白折叠规则同 html_text。
// Write one normalized codepoint into the current paragraph, collapsing whitespace as html_text does.
static void ks_emit(kosync_xhtml_t* m, uint32_t cp) {
    if (m->status != 0) return;
    if (ks_space_cp(cp)) {
        if (m->active) m->space = true;
        return;
    }
    char bytes[4];
    size_t n;
    if (cp < 0x80) {
        bytes[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        bytes[0] = (char)(0xc0 | (cp >> 6));
        bytes[1] = (char)(0x80 | (cp & 63));
        n = 2;
    } else if (cp < 0x10000) {
        bytes[0] = (char)(0xe0 | (cp >> 12));
        bytes[1] = (char)(0x80 | ((cp >> 6) & 63));
        bytes[2] = (char)(0x80 | (cp & 63));
        n = 3;
    } else {
        bytes[0] = (char)(0xf0 | (cp >> 18));
        bytes[1] = (char)(0x80 | ((cp >> 12) & 63));
        bytes[2] = (char)(0x80 | ((cp >> 6) & 63));
        bytes[3] = (char)(0x80 | (cp & 63));
        n = 4;
    }
    if (!m->active) ks_begin_block(m);
    if (m->status != 0) return;
    if (m->space) {
        m->space = false;
        ks_hash_update(&m->block_hash, " ", 1);
        m->block_bytes++;
    }
    ks_hash_update(&m->block_hash, bytes, n);
    m->block_bytes += (uint32_t)n;
}

// 图片占位符 U+FFFC：计入规范化段字节（html_text 如此），但不计入 XPath 文本码点，
// 因为 KOReader/CrossMux 的可见文本计数里图片不占文字码点。
// The U+FFFC image placeholder counts toward normalized paragraph bytes (as html_text does)
// but not toward XPath text codepoints: KOReader/CrossMux never count images as text.
static void ks_emit_image(kosync_xhtml_t* m) {
    static const unsigned char bytes[3] = {0xef, 0xbf, 0xbc};
    if (m->status != 0) return;
    if (!m->active) ks_begin_block(m);
    if (m->status != 0) return;
    if (m->space) {
        m->space = false;
        ks_hash_update(&m->block_hash, " ", 1);
        m->block_bytes++;
    }
    ks_hash_update(&m->block_hash, (const char*)bytes, sizeof(bytes));
    m->block_bytes += (uint32_t)sizeof(bytes);
}

static bool ks_append(char* out, size_t cap, size_t* used, const char* text) {
    size_t n = strlen(text);
    if (*used + n + 1 > cap) return false;
    memcpy(out + *used, text, n + 1);
    *used += n;
    return true;
}

// 由当前栈构造 XPath / Build the XPath from the open element stack.
static bool ks_build_xpath(kosync_xhtml_t* m, uint16_t text_index, uint32_t text_off, char* out, size_t cap) {
    size_t used = 0;
    out[0] = 0;
    char head[64];
    snprintf(head, sizeof(head), "/body/DocFragment[%u]/body", m->spine_index + 1u);
    if (!ks_append(out, cap, &used, head)) return false;
    for (unsigned i = 1; i < m->depth_count; i++) {
        const char* local = m->names[m->depth[i].name_id];
        char step[KS_NAME_CAP + 16];
        snprintf(step, sizeof(step), "/%s[%u]", local, (unsigned)m->depth[i].sibling);
        if (!ks_append(out, cap, &used, step)) return false;
    }
    char tail[40];
    if (text_index)
        snprintf(tail, sizeof(tail), "/text()[%u].%u", (unsigned)text_index, (unsigned)text_off);
    else
        snprintf(tail, sizeof(tail), "/text().%u", (unsigned)text_off);
    return ks_append(out, cap, &used, tail);
}

// 字符数据到达：必要时开新文本节点，返回该码点在节点内的偏移。
// Character data arrived: start a text node when needed and return this codepoint's offset.
static uint32_t ks_text_cp(kosync_xhtml_t* m) {
    if (m->depth_count == 0) return 0;
    ks_elem_t* top = &m->depth[m->depth_count - 1];
    if (top->pending_text) {
        top->pending_text = false;
        top->text_nodes++;
        top->node_cp = 0;
    }
    return top->node_cp++;
}

// XPath 模式：当前位置命中目标规范化字节时记录 XPath。
// XPath mode: record the XPath once the requested normalized byte is reached.
static void ks_maybe_xpath(kosync_xhtml_t* m, uint32_t node_cp) {
    if (m->pass != KS_PASS_XPATH || m->found || m->status != 0 || m->depth_count == 0) return;
    if (!m->active || m->active_block != m->target_block) return;
    if (m->block_bytes <= m->target_off) return;
    ks_elem_t* top = &m->depth[m->depth_count - 1];
    if (!top->text_nodes) return;
    if (!ks_build_xpath(m, top->text_nodes, node_cp, m->result, sizeof(m->result))) return;
    m->found = true;
}

// resolve 模式：目标码点出现时记录段落/段内偏移。
// Resolve mode: record paragraph and in-paragraph offset when the target codepoint appears.
static void ks_maybe_resolve(kosync_xhtml_t* m, uint32_t cp, uint32_t node_cp) {
    if (m->pass != KS_PASS_RESOLVE || m->found || m->status != 0) return;
    if (m->depth_count < 1 || !m->path.has_text) return;
    if (m->path.count != m->depth_count - 1) return;
    for (unsigned i = 0; i < m->path.count; i++) {
        ks_elem_t* e = &m->depth[i + 1];
        if (ks_hash_name(m->names[e->name_id]) != m->path.steps[i].hash || e->sibling != m->path.steps[i].index)
            return;
    }
    ks_elem_t* top = &m->depth[m->depth_count - 1];
    if (top->text_nodes != m->path.text_index || node_cp != m->path.text_off) return;
    m->target_block = m->active ? m->active_block : m->block_count;
    uint32_t off = m->active ? m->block_bytes : 0;
    // 待折叠空白会在下一个可见字符前补一个空格，目标位置要把它算进去。
    // The pending collapsed whitespace becomes one space before the next visible character.
    if (m->active && m->space && !ks_space_cp(cp)) off++;
    m->target_off = off;
    m->found = true;
}

static void ks_text_cp_emit(kosync_xhtml_t* m, uint32_t cp) {
    uint32_t node_cp = ks_text_cp(m);
    ks_maybe_resolve(m, cp, node_cp);
    ks_emit(m, cp);
    ks_maybe_xpath(m, node_cp);
}

// 块级标签集合与 html_text 的 block_tag() 完全一致。
// Block tags match html_text's block_tag() exactly.
static bool ks_block_tag(const char* name) {
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) return true;
    static const char* const tags[] = {"p", "div", "li", "tr", "br", "hr", "blockquote", "center"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++)
        if (ks_name_equal(name, tags[i])) return true;
    return false;
}

static bool ks_void_tag(const char* name) {
    static const char* const tags[] = {"br", "hr", "img", "image", "svg:image", "meta", "link",
                                       "input", "area", "base", "col", "embed", "param", "source",
                                       "track", "wbr"};
    for (size_t i = 0; i < sizeof(tags) / sizeof(*tags); i++)
        if (ks_name_equal(name, tags[i])) return true;
    return false;
}

static bool ks_display_image_tag(const char* name) {
    return ks_name_equal(name, "img") || ks_name_equal(name, "image") || ks_name_equal(name, "svg:image") ||
           ks_name_equal(name, "object");
}

// html_text 跳过内容的三类容器；title/rp 等其余容器保留文本以保持段落一致。
// Only these three containers skip their content in html_text; every other tag keeps its text.
static bool ks_skip_content_tag(const char* name) {
    return ks_name_equal(name, "head") || ks_name_equal(name, "style") || ks_name_equal(name, "script");
}

// 与 html_text 的 attr_value() 同规则：属性名大小写不敏感，值可加引号；返回值的区间。
// Same rules as html_text attr_value(): case-insensitive names, quoted values; returns the span.
static bool ks_attr_value(const char* html, size_t at, size_t end, const char* wanted, size_t* value,
                          size_t* value_len) {
    const size_t wanted_len = strlen(wanted);
    while (at < end) {
        while (at < end && (ks_ascii_space((unsigned char)html[at]) || html[at] == '/')) at++;
        const size_t name = at;
        while (at < end && ks_name_char(html[at])) at++;
        const size_t n = at - name;
        while (at < end && ks_ascii_space((unsigned char)html[at])) at++;
        if (at >= end || html[at++] != '=') continue;
        while (at < end && ks_ascii_space((unsigned char)html[at])) at++;
        if (at >= end) break;
        const char quote = (html[at] == '\'' || html[at] == '"') ? html[at++] : 0;
        const size_t start = at;
        while (at < end && (quote ? html[at] != quote : !ks_ascii_space((unsigned char)html[at]) && html[at] != '>'))
            at++;
        const size_t length = at - start;
        if (quote && at < end) at++;
        if (n == wanted_len && !strncasecmp(html + name, wanted, n)) {
            *value = start;
            *value_len = length;
            return true;
        }
    }
    return false;
}

// 是否处于同章脚注锚点（`<a href="#...">`，锚点后至少 1 个字符），与 html_text 一致。
// Whether this is a same-chapter footnote anchor (`<a href="#...">` with a non-empty target).
static bool ks_note_href(const char* html, size_t at, size_t end) {
    size_t value = 0, length = 0;
    if (!ks_attr_value(html, at, end, "href", &value, &length)) return false;
    return length > 1 && html[value] == '#';
}

static size_t ks_entity_lookup(const char* s, size_t len, uint32_t* cp);
static size_t ks_utf8(const char* s, size_t len, uint32_t* cp);

// 与 html_text 的 image_attr() 同判定：图片属性可识别、解码后不超长。
// 注意 html_text 的 `data:` 过滤在命中后并不 break，扫描结束后仍沿用它的 value/length，
// 因此 data: URI 实际仍会生成占位（历史行为）；本模型逐字照抄以保持块表对拍。
// Same decision as html_text image_attr(): a recognizable, not-too-long path. Note that
// html_text's `data:` rejection does not break its candidate loop and the rejected
// value/length survive to the end, so data: URIs still produce a placeholder in practice;
// this model copies that behaviour verbatim so the paragraph tables keep matching.
static bool ks_image_attr_ok(const char* html, size_t at, size_t end, bool object) {
    static const char* const names[] = {"data-src", "data-original", "data-lazy-src", "src", "xlink:href",
                                        "href", "srcset", "data-srcset"};
    size_t value = 0, length = 0;
    bool have = false;
    if (object) {
        have = ks_attr_value(html, at, end, "data", &value, &length);
    } else {
        for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
            size_t v = 0, n = 0;
            if (!ks_attr_value(html, at, end, names[i], &v, &n)) continue;
            have = true;
            value = v;
            length = n;
            const bool data_uri = n >= 5 && !strncasecmp(html + v, "data:", 5);
            if (!n || data_uri) continue; // 被拒但仍留下 value/length / rejected yet retained
            if (strstr(names[i], "srcset") != NULL) {
                size_t j = 0;
                while (j < n && ks_ascii_space((unsigned char)html[value + j])) j++;
                size_t k = 0;
                while (j + k < n && !ks_ascii_space((unsigned char)html[value + j + k]) && html[value + j + k] != ',')
                    k++;
                value += j;
                length = k;
            }
            break;
        }
    }
    if (!have || !length || length > KS_IMAGE_PATH_MAX) return false;
    // 解码实体后的长度上限（html_text 在拼路径时逐字节限长）。
    // Bound the decoded length of entity references exactly like html_text's path copy.
    size_t used = 0;
    for (size_t i = 0; i < length;) {
        if (html[value + i] == '&') {
            uint32_t cp = 0;
            size_t consumed = ks_entity_lookup(html + value + i, length - i, &cp);
            if (consumed) {
                size_t count = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
                if (used + count > KS_IMAGE_PATH_MAX) return false;
                used += count;
                i += consumed;
                continue;
            }
        }
        if (used == KS_IMAGE_PATH_MAX) return false;
        used++;
        i++;
    }
    return used != 0;
}

static void ks_push(kosync_xhtml_t* m, int name_id) {
    if (m->depth_count >= KS_MAX_DEPTH) {
        m->status = -1;
        return;
    }
    if (name_id < 0) {
        m->status = -1;
        return;
    }
    ks_elem_t* e = &m->depth[m->depth_count];
    memset(e, 0, sizeof(*e));
    e->name_id = (uint16_t)name_id;
    e->sibling = 1;
    e->pending_text = true;
    if (m->depth_count) {
        ks_elem_t* parent = &m->depth[m->depth_count - 1];
        uint32_t hash = name_id > 0 ? ks_hash_name(m->names[name_id]) : 0;
        uint16_t idx = 1;
        bool known = false;
        for (uint16_t i = 0; i < parent->child_count; i++) {
            if (parent->children[i].hash == hash) {
                parent->children[i].count++;
                idx = parent->children[i].count;
                known = true;
                break;
            }
        }
        if (!known) {
            if (parent->child_count >= KS_CHILD_CAP) {
                m->status = -1;
                return;
            }
            parent->children[parent->child_count].hash = hash;
            parent->children[parent->child_count].count = 1;
            parent->child_count++;
        }
        e->sibling = idx;
    }
    m->depth_count++;
}

// 关闭标签：顶层同名才弹栈；否则向下查找同名祖先，跳过中间未闭合层（宽松 HTML）。
// A close tag pops the top when names match; otherwise it pops down to the nearest same-name
// ancestor so unclosed inline tags in lenient HTML do not break the ancestry.
static void ks_pop_for_close(kosync_xhtml_t* m, const char* name) {
    if (!m->inside_body || m->depth_count <= 1) return;
    const char* top = m->names[m->depth[m->depth_count - 1].name_id];
    if (strcmp(top, name) == 0) {
        m->depth_count--;
        return;
    }
    for (unsigned i = m->depth_count; i-- > 1;) {
        if (strcmp(m->names[m->depth[i].name_id], name) == 0) {
            m->depth_count = i;
            return;
        }
    }
}

// resolve 模式下元素打开时命中“只有元素路径”的 XPath。
// In resolve mode an element-only XPath anchors that element's start.
static void ks_maybe_element_start(kosync_xhtml_t* m) {
    if (m->pass != KS_PASS_RESOLVE || m->found || m->status != 0) return;
    if (m->path.has_text || m->path.count == 0 || m->path.count != m->depth_count - 1) return;
    for (unsigned i = 0; i < m->path.count; i++) {
        ks_elem_t* e = &m->depth[i + 1];
        if (ks_hash_name(m->names[e->name_id]) != m->path.steps[i].hash || e->sibling != m->path.steps[i].index)
            return;
    }
    m->target_block = m->active ? m->active_block : m->block_count;
    m->target_off = m->active ? m->block_bytes : 0;
    // 待折叠空白会在元素内容前落成一个空格，元素起点要算上它。
    // Pending collapsed whitespace lands as one space before the element content.
    if (m->active && m->space) m->target_off++;
    m->found = true;
}

// 找标签结束的 '>'（跳过引号内的 '>'），与 html_text 的扫描一致。
// Find the tag's '>' while skipping quoted values, matching html_text's scan.
static size_t ks_tag_end(const char* html, size_t len, size_t at) {
    char quote = 0;
    for (size_t i = at; i < len; i++) {
        char c = html[i];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '\'' || c == '"') {
            quote = c;
        } else if (c == '>') {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t ks_entity_lookup(const char* s, size_t len, uint32_t* cp) {
    size_t end = 1;
    while (end < len && end <= 32 && s[end] != ';' && !ks_ascii_space((unsigned char)s[end]) && s[end] != '&' &&
           s[end] != '<')
        end++;
    if (end >= len || end > 32 || s[end] != ';') return 0;
    static const struct {
        const char* name;
        uint32_t cp;
    } names[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
    };
    if (end > 2 && s[1] == '#') {
        size_t i = 2;
        unsigned base = 10;
        if (i < end && (s[i] == 'x' || s[i] == 'X')) {
            base = 16;
            i++;
        }
        if (i == end) return 0;
        uint32_t value = 0;
        bool overflow = false;
        for (; i < end; i++) {
            unsigned char c = (unsigned char)ks_lower(s[i]);
            unsigned digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 255;
            if (digit >= base) return 0;
            if (value > (0x10ffffu - digit) / base) overflow = true;
            else if (!overflow)
                value = value * base + digit;
        }
        *cp = overflow || !value || (value >= 0xd800 && value <= 0xdfff) ? 0xfffd : value;
        return end + 1;
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i].name) == end - 1 && memcmp(s + 1, names[i].name, end - 1) == 0) {
            *cp = names[i].cp;
            return end + 1;
        }
    }
    return 0;
}

static size_t ks_utf8(const char* s, size_t len, uint32_t* cp) {
    const unsigned char* p = (const unsigned char*)s;
    if (p[0] && p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    size_t n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 : p[0] >= 0xe0 && p[0] <= 0xef ? 3 : p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > len) return 0;
    uint32_t value = p[0] & (0x7f >> n);
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) return 0;
        value = (value << 6) | (p[i] & 63);
    }
    if ((n == 2 && value < 0x80) || (n == 3 && value < 0x800) || (n == 4 && value < 0x10000) || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff))
        return 0;
    *cp = value;
    return n;
}

// 单遍扫描；文本与段落规则对齐 html_text.c，XPath 的节点计数与祖先链只统计 body 内。
// One pass; text and paragraphs mirror html_text.c while XPath node/ancestry state only
// counts inside <body>.
static void ks_walk(kosync_xhtml_t* m) {
    const char* html = m->html;
    const size_t len = m->len;
    size_t pos = (len >= 3 && memcmp(html, "\xef\xbb\xbf", 3) == 0) ? 3 : 0;
    char skip[KS_NAME_CAP] = "";
    bool resume_head = false;
    while (pos < len && m->status == 0) {
        if ((!skip[0] || strcmp(skip, "head") == 0) && len - pos >= 4 && memcmp(html + pos, "<!--", 4) == 0) {
            pos += 4;
            while (len - pos >= 3 && memcmp(html + pos, "-->", 3)) pos++;
            pos = len - pos >= 3 ? pos + 3 : len;
            if (m->inside_body && m->depth_count) m->depth[m->depth_count - 1].pending_text = true;
            continue;
        }
        if (html[pos] == '<') {
            size_t at = pos + 1;
            const bool closing = at < len && html[at] == '/';
            if (closing) at++;
            const size_t name_start = at;
            while (at < len && ks_name_char(html[at])) at++;
            const size_t name_len = at - name_start;
            char name[KS_NAME_CAP] = "";
            if (name_len < sizeof(name)) {
                for (size_t i = 0; i < name_len; i++) name[i] = ks_lower(html[name_start + i]);
                name[name_len] = 0;
            }
            const bool declaration = name_start < len && (html[name_start] == '!' || html[name_start] == '?');
            const bool head_child = strcmp(skip, "head") == 0 && !closing &&
                                    (strcmp(name, "script") == 0 || strcmp(name, "style") == 0);
            // 跳过容器内部：非容器自身的关闭标签一律按字符跳过，与 html_text 相同。
            // Inside a skipped container every tag but its own close is skipped char by char.
            if (skip[0] && !head_child && (!closing || strcmp(name, skip) != 0)) {
                pos++;
                continue;
            }
            const unsigned char initial = name_start < len ? (unsigned char)ks_lower(html[name_start]) : 0;
            if (!declaration && !(name_len && initial >= 'a' && initial <= 'z')) {
                // 不能开启标签的 '<'：html_text 把它当普通文字收下，这里也照做。
                // A '<' that cannot start a tag is plain text in html_text, so emit it as such.
                if (m->inside_body && m->depth_count)
                    ks_text_cp_emit(m, '<');
                else
                    ks_emit(m, '<');
                pos++;
                continue;
            }
            const size_t end = ks_tag_end(html, len, at);
            if (end == SIZE_MAX) {
                m->status = -1;
                return;
            }
            const bool self_closing = end > at && html[end - 1] == '/';
            pos = end + 1;
            if (skip[0]) {
                if (head_child) {
                    if (!self_closing) {
                        memcpy(skip, name, sizeof(skip));
                        resume_head = true;
                    }
                } else if (resume_head) {
                    memcpy(skip, "head", 5);
                    resume_head = false;
                } else {
                    skip[0] = 0;
                    if (m->depth_count) m->depth[m->depth_count - 1].pending_text = true;
                }
                continue;
            }
            if (declaration) {
                // 注释/处理指令/CDATA/DOCTYPE 都是节点边界；其内容 html_text 不收。
                // Comments, PIs, CDATA and DOCTYPE are node boundaries; html_text keeps none of it.
                if (m->inside_body && m->depth_count) m->depth[m->depth_count - 1].pending_text = true;
                continue;
            }
            if (closing) {
                if (m->note_open && ks_name_equal(name, "a")) {
                    m->note_open = false;
                    m->note_image = false;
                }
                if (m->inside_body) ks_pop_for_close(m, name);
                if (m->inside_body && ks_block_tag(name)) ks_finish_block(m);
                if (ks_name_equal(name, "body")) m->inside_body = false;
                if (m->inside_body && m->depth_count) m->depth[m->depth_count - 1].pending_text = true;
                continue;
            }
            // 图片：先于块标签处理，与 html_text 的顺序一致；图片标签不压栈。
            // Images are handled before block tags (html_text order) and never push a frame.
            if (ks_display_image_tag(name)) {
                if (m->image_count < KS_IMAGE_MAX &&
                    ks_image_attr_ok(html, at, end, ks_name_equal(name, "object"))) {
                    if (m->note_open && !m->note_image) {
                        m->note_image = true;
                        ks_emit_image(m);
                    } else {
                        ks_finish_block(m);
                        ks_emit_image(m);
                        ks_finish_block(m);
                    }
                    m->image_count++;
                }
                if (m->inside_body && m->depth_count) m->depth[m->depth_count - 1].pending_text = true;
                continue;
            }
            if (ks_skip_content_tag(name) && !self_closing) {
                memcpy(skip, name, sizeof(skip));
                continue;
            }
            if (ks_name_equal(name, "body")) {
                if (!m->saw_body) {
                    m->saw_body = true;
                    m->inside_body = true;
                    if (m->depth_count == 0) ks_push(m, 0); // body 自身占一帧 / the body frame
                }
                if (m->inside_body && m->depth_count) m->depth[m->depth_count - 1].pending_text = true;
                continue;
            }
            if (m->inside_body && !self_closing && !ks_void_tag(name)) {
                // 名字超长无法写进 XPath 步骤（CrossMux 的步骤名也只有 12 字节）：
                // 整章拒绝，免得给出无法复现的路径。
                // A name too long for an XPath step (CrossMux allows 12 bytes) rejects the
                // chapter instead of producing a path nobody can resolve.
                if (name_len >= KS_NAME_CAP) {
                    m->status = -1;
                    return;
                }
                int name_id = ks_name_intern(m, name);
                if (name_id < 0) {
                    m->status = -1;
                    return;
                }
                ks_push(m, name_id);
                if (m->status != 0) return;
                if (ks_name_equal(name, "a")) {
                    m->note_open = ks_note_href(html, at, end);
                    m->note_image = false;
                }
                if (ks_block_tag(name)) ks_finish_block(m);
                ks_maybe_element_start(m);
                m->depth[m->depth_count - 1].pending_text = true;
                continue;
            }
            // 自闭合/空元素（含 br/hr）与 body 外标签：只做块切分与文本节点边界。
            // Void/self-closing tags (and tags outside body): only paragraph splits and boundaries.
            if (m->inside_body && m->depth_count) {
                if (ks_block_tag(name)) ks_finish_block(m);
                m->depth[m->depth_count - 1].pending_text = true;
            }
            continue;
        }
        if (skip[0]) {
            pos++;
            continue;
        }
        uint32_t cp = 0;
        size_t n = html[pos] == '&' ? ks_entity_lookup(html + pos, len - pos, &cp) : 0;
        if (!n) n = ks_utf8(html + pos, len - pos, &cp);
        if (!n) {
            m->status = -1;
            return;
        }
        if (m->inside_body && m->depth_count)
            ks_text_cp_emit(m, cp);
        else
            ks_emit(m, cp);
        pos += n;
    }
    if (m->status == 0) ks_finish_block(m);
}

// ---------------- XPath 解析 / XPath parsing ----------------

static bool ks_parse_uint(const char* s, size_t len, size_t* at, unsigned* out) {
    size_t i = *at;
    if (i >= len || s[i] < '0' || s[i] > '9') return false;
    unsigned value = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        value = value * 10u + (unsigned)(s[i] - '0');
        i++;
        if (value > 100000u) return false;
    }
    *at = i;
    *out = value;
    return true;
}

// 解析 `/body/DocFragment[n]/body(/name[idx])*(/text()[k].off|/text().off)?`；
// 也接受章节起点形态 `/body/DocFragment[n]/body` 与 `/body/DocFragment[n]/body.0`。
// Parse the KOReader progress path shape; chapter-start forms are accepted too.
static int ks_parse_xpath(kosync_xhtml_t* m, const char* xpath, size_t len) {
    static const char prefix[] = "/body/DocFragment[";
    const size_t plen = sizeof(prefix) - 1;
    if (len < plen || memcmp(xpath, prefix, plen) != 0) return -1;
    size_t at = plen;
    unsigned fragment = 0;
    if (!ks_parse_uint(xpath, len, &at, &fragment)) return -1;
    if (fragment != m->spine_index + 1u) return -1;
    static const char body[] = "]/body";
    const size_t blen = sizeof(body) - 1;
    if (len - at < blen || memcmp(xpath + at, body, blen) != 0) return -1;
    at += blen;
    m->path.count = 0;
    m->path.has_text = false;
    // 章节起点：`…]/body` 或 `…]/body.0`（CrossMux isChapterStartXPath 接受的全零偏移）。
    // Chapter start: `…]/body` or `…]/body.0` (all-zero offset as CrossMux accepts).
    if (at == len) return 0;
    if (xpath[at] == '.') {
        at++;
        unsigned off = 0;
        if (!ks_parse_uint(xpath, len, &at, &off)) return -1;
        if (at != len || off != 0) return -1;
        return 0;
    }
    while (at < len) {
        if (xpath[at] != '/') return -1;
        at++;
        if (len - at >= 6 && memcmp(xpath + at, "text()", 6) == 0) {
            at += 6;
            unsigned text_index = 1;
            if (at < len && xpath[at] == '[') {
                at++;
                if (!ks_parse_uint(xpath, len, &at, &text_index)) return -1;
                if (at >= len || xpath[at] != ']') return -1;
                at++;
            }
            unsigned off = 0;
            if (at < len && xpath[at] == '.') {
                at++;
                if (!ks_parse_uint(xpath, len, &at, &off)) return -1;
            }
            if (at != len) return -1;
            m->path.text_index = (uint16_t)text_index;
            m->path.text_off = off;
            m->path.has_text = true;
            return 0;
        }
        size_t name_start = at;
        while (at < len && xpath[at] != '[' && xpath[at] != '/' && xpath[at] != '.') at++;
        size_t name_len = at - name_start;
        if (!name_len || name_len >= KS_NAME_CAP) return -1;
        char local[KS_NAME_CAP];
        for (size_t i = 0; i < name_len; i++) local[i] = ks_lower(xpath[name_start + i]);
        local[name_len] = 0;
        unsigned index = 1;
        if (at < len && xpath[at] == '[') {
            at++;
            if (!ks_parse_uint(xpath, len, &at, &index)) return -1;
            if (at >= len || xpath[at] != ']') return -1;
            at++;
        }
        if (m->path.count >= KS_MAX_DEPTH) return -1;
        m->path.steps[m->path.count].hash = ks_hash_name(local);
        m->path.steps[m->path.count].index = (uint16_t)index;
        m->path.count++;
    }
    m->path.has_text = false;
    return 0;
}

// ---------------- 公共接口 / public API ----------------

static void ks_reset_state(kosync_xhtml_t* m) {
    m->depth_count = 0;
    m->inside_body = false;
    m->saw_body = false;
    m->active = false;
    m->space = false;
    m->block_bytes = 0;
    m->block_hash = KS_FNV_BASIS;
    m->image_count = 0;
    m->note_open = false;
    m->note_image = false;
    m->found = false;
    m->status = 0;
    m->result[0] = 0;
}

int kosync_xhtml_parse(const char* html, size_t len, kosync_xhtml_t** out) {
    if (!out) return -1;
    *out = NULL;
    if (!html && len) return -1;
    kosync_xhtml_t* m = (kosync_xhtml_t*)calloc(1, sizeof(*m));
    if (!m) return -1;
    m->html = html;
    m->len = len;
    // 0 号名为空，body 帧使用 / slot 0 is empty and used by the body frame
    m->names[0][0] = 0;
    m->name_pool_count = 1;
    m->pass = KS_PASS_COUNT;
    ks_reset_state(m);
    ks_walk(m);
    if (m->status != 0 || !m->saw_body || m->block_count == 0) {
        kosync_xhtml_free(m);
        return -1;
    }
    *out = m;
    return 0;
}

void kosync_xhtml_free(kosync_xhtml_t* model) {
    if (!model) return;
    free(model->blocks);
    free(model);
}

uint32_t kosync_xhtml_block_count(const kosync_xhtml_t* model) { return model ? model->block_count : 0; }

uint32_t kosync_xhtml_block_length(const kosync_xhtml_t* model, uint32_t block) {
    return model && block < model->block_count ? model->blocks[block].len : 0;
}

uint64_t kosync_xhtml_block_hash(const kosync_xhtml_t* model, uint32_t block) {
    return model && block < model->block_count ? model->blocks[block].hash : 0;
}

// 单次行走；target 为 (block, off)。返回找到的 XPath 或失败。
// One walk for a (block, offset) target; returns the XPath or failure.
static int ks_xpath_walk(kosync_xhtml_t* m, uint32_t block, uint32_t off) {
    m->pass = KS_PASS_XPATH;
    m->target_block = block;
    m->target_off = off;
    m->block_count = 0; // 重建段落表且结果确定 / rebuild the paragraph table deterministically
    ks_reset_state(m);
    ks_walk(m);
    return (!m->found || m->status != 0) ? -1 : 0;
}

int kosync_xhtml_xpath_for(const kosync_xhtml_t* model, uint32_t block, uint32_t norm_off, unsigned spine_index,
                           char* out, size_t cap) {
    if (!model || !out || cap < 2 || block >= model->block_count) return -1;
    const uint32_t block_len = model->blocks[block].len;
    if (!block_len || norm_off >= block_len) return -1;
    kosync_xhtml_t* m = (kosync_xhtml_t*)model; // 复用行走状态；调用方串行化 / reuse walk state; callers serialize
    m->spine_index = spine_index;
    if (ks_xpath_walk(m, block, norm_off) != 0) {
        // 该段没有可锚定的文字（例如整段只有图片占位）：退到下一段文字起点。
        // The paragraph has no anchorable text (e.g. image-only): fall back to the next one.
        if (block + 1 >= m->block_count || ks_xpath_walk(m, block + 1, 0) != 0) return -1;
    }
    if (strlen(m->result) + 1 > cap) return -1;
    memcpy(out, m->result, strlen(m->result) + 1);
    return 0;
}

int kosync_xhtml_resolve(const kosync_xhtml_t* model, const char* xpath, unsigned spine_index, uint32_t* block,
                         uint32_t* norm_off) {
    if (!model || !xpath || !block || !norm_off) return -1;
    kosync_xhtml_t* m = (kosync_xhtml_t*)model;
    m->spine_index = spine_index;
    if (ks_parse_xpath(m, xpath, strlen(xpath)) != 0) return -1;
    const bool chapter_start = m->path.count == 0 && !m->path.has_text;
    m->pass = KS_PASS_RESOLVE;
    m->block_count = 0;
    ks_reset_state(m);
    ks_walk(m);
    if (m->status != 0) return -1;
    if (chapter_start) {
        // 整章起点固定落在第 0 段起点（无文本时即整章起点）。
        // A chapter-start path anchors the first paragraph's start.
        *block = 0;
        *norm_off = 0;
        return 0;
    }
    if (!m->found) return -1;
    if (m->target_block >= m->block_count) return -1;
    *block = m->target_block;
    *norm_off = m->target_off;
    return 0;
}

int kosync_xpath_chapter_only(unsigned spine_index, char* out, size_t cap) {
    if (!out || !cap) return -1;
    int n = snprintf(out, cap, "/body/DocFragment[%u]/body", spine_index + 1u);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}
