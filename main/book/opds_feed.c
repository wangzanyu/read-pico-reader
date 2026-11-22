/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 1.2（Atom）流式解析器。只做浏览/下载所需的最小集合：feed 标题、条目
 *       title/author/id、link(rel/href/type/length) 分类、feed 级分页链接。纯 C，
 *       内存由调用方注入；未知元素安全跳过，单个坏条目/坏链接不影响整份 feed。
 * English: Minimal streaming OPDS 1.2 (Atom) parser for browsing/download: feed title,
 *       entry title/author/id, link(rel/href/type/length) classification and feed-level
 *       pagination. Plain C with injected memory; unknown elements are skipped safely and
 *       one bad entry/link never fails the whole feed.
 *
 * 冻结：仅 Atom；OPDS 2.0 JSON/OAuth/LCP/间接获取/搜索不在范围。
 * Frozen: Atom only; OPDS 2.0 JSON, OAuth, LCP, indirect acquisition and search are out.
 */
#include "opds_feed.h"

#include <stdlib.h>
#include <string.h>

#define OPDS_TAG_MAX 2048u
#define OPDS_STACK_MAX 32u
#define OPDS_NAME_MAX 48u
#define OPDS_TEXT_MAX 1024u
#define OPDS_ENTITY_MAX 16u
#define OPDS_COPY_MAX (OPDS_URL_MAX * 2u + 32u)
#define OPDS_NO_ENTRY_RANK 1000

/* 词法状态机 / Lexer states. */
typedef enum {
    XS_TEXT = 0,
    XS_LT,
    XS_TAG,
    XS_COMMENT,
    XS_CDATA,
    XS_PI,
    XS_DOCTYPE,
    XS_BOGUS,
    XS_ENTITY,
} opds_xml_state_t;

/* 文本落点 / Current text target. */
typedef enum {
    TT_NONE = 0,
    TT_FEED_TITLE,
    TT_ENTRY_TITLE,
    TT_ENTRY_AUTHOR,
    TT_ENTRY_ID,
} opds_text_target_t;

struct opds_feed_parser {
    opds_feed_memory_t memory;
    char base_url[OPDS_URL_MAX];

    opds_entry_t* entries; /* 惰性分配 / lazily allocated */
    opds_feed_info_t info;

    bool failed;   /* 内存不足等内部错误 / internal failure */
    bool finished; /* finish() 已调用 / finish() called */
    bool ok;       /* finish() 结论 / finish() result */
    bool malformed;
    bool seen_root;
    bool in_feed;
    bool in_entry;
    bool in_author;

    uint8_t depth;
    uint8_t entry_level;
    uint8_t author_level;

    bool feed_title_set;
    bool entry_title_set;
    bool entry_author_set;
    bool entry_id_set;

    opds_entry_t current;
    bool have_acq;
    bool have_nav;
    int acq_rank;
    int nav_rank;
    char acq_href[OPDS_URL_MAX];
    char acq_type[OPDS_TYPE_MAX];
    char acq_rel[OPDS_REL_MAX];
    uint32_t acq_length;
    char nav_href[OPDS_URL_MAX];
    char nav_type[OPDS_TYPE_MAX];
    char nav_rel[OPDS_REL_MAX];

    /* 词法器 / lexer */
    opds_xml_state_t state;
    char lt[16];
    size_t lt_len;
    char tag[OPDS_TAG_MAX];
    size_t tag_len;
    bool tag_overflow;
    char tag_quote;
    uint8_t term;
    char cdata_tail[3];
    uint8_t cdata_len;
    char entity[OPDS_ENTITY_MAX];
    size_t entity_len;
    char stack[OPDS_STACK_MAX][OPDS_NAME_MAX];
    char dt_quote;
    uint8_t dt_brackets;
    char bogus_quote;
    char bom[3];
    uint8_t bom_len;
    bool bom_done;

    /* 属性暂存 / current attributes */
    char attr_val[OPDS_URL_MAX];
    size_t attr_val_len;
    char attr_rel[OPDS_REL_MAX];
    char attr_href[OPDS_URL_MAX];
    char attr_type[OPDS_TYPE_MAX];
    uint32_t attr_length;
    bool attr_has_rel;
    bool attr_has_href;
    bool attr_has_type;
    bool attr_bad;

    /* 文本暂存 / current text */
    opds_text_target_t text_target;
    uint8_t text_level;
    char text[OPDS_TEXT_MAX];
    size_t text_len;
    bool text_pending_space;
    bool text_overflow;

    char resolve_buf[OPDS_URL_MAX];
};

static bool xml_byte(struct opds_feed_parser* parser, unsigned char ch);
static bool handle_start_tag(struct opds_feed_parser* parser, const char* name, const char* attrs, size_t attrs_len);
static bool handle_end_tag(struct opds_feed_parser* parser, const char* name);
static bool close_element(struct opds_feed_parser* parser, uint8_t level);
static bool entry_finalize(struct opds_feed_parser* parser);

/* ---------- 通用小工具 / Generic helpers ---------- */

static bool is_space_char(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char ascii_lower_char(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + ('a' - 'A'));
    return c;
}

static bool ci_equal_n(const char* a, const char* b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (ascii_lower_char(a[i]) != ascii_lower_char(b[i])) return false;
    }
    return true;
}

static bool ci_equal(const char* a, const char* b) {
    size_t n = strlen(b);
    return strlen(a) == n && ci_equal_n(a, b, n);
}

static bool ci_starts_with(const char* text, const char* prefix) {
    size_t n = strlen(prefix);
    return strlen(text) >= n && ci_equal_n(text, prefix, n);
}

static bool ci_contains(const char* text, const char* needle) {
    size_t needle_len = strlen(needle);
    size_t text_len = strlen(text);
    size_t i;
    if (needle_len == 0 || text_len < needle_len) return false;
    for (i = 0; i + needle_len <= text_len; i++) {
        if (ci_equal_n(text + i, needle, needle_len)) return true;
    }
    return false;
}

/* 剥离命名空间前缀并转小写；返回长度。 / Strip namespace prefix and lowercase. */
static size_t local_name_lower(const char* src, size_t len, char* out, size_t cap) {
    size_t start = 0;
    size_t i;
    size_t used = 0;
    for (i = 0; i < len; i++) {
        if (src[i] == ':') {
            start = i + 1;
            break;
        }
    }
    for (i = start; i < len; i++) {
        if (used + 1 >= cap) break;
        out[used++] = ascii_lower_char(src[i]);
    }
    out[used] = '\0';
    return used;
}

/* UTF-8 码点边界拷贝 / Copy on UTF-8 code point boundaries. */
static void copy_utf8_truncated(char* dst, size_t cap, const char* src, size_t len) {
    size_t used = 0;
    size_t i = 0;
    if (cap == 0) return;
    while (i < len && used + 1 < cap) {
        unsigned char c = (unsigned char)src[i];
        size_t seq = 1;
        if (c >= 0xC2 && c <= 0xDF) {
            seq = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            seq = 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            seq = 4;
        }
        if (seq > len - i) break; /* 截断在码点中间：停在码点边界 / stop on a code point boundary */
        if (used + seq >= cap) break;
        memcpy(dst + used, src + i, seq);
        used += seq;
        i += seq;
    }
    dst[used] = '\0';
}

/* 实体解码：返回是否已知；未知实体由调用方宽松保留。/ Entity decode. */
static bool entity_decode(const char* name, size_t len, uint32_t* cp) {
    static const struct {
        const char* name;
        uint32_t value;
    } named[] = {
        {"amp", '&'},
        {"lt", '<'},
        {"gt", '>'},
        {"quot", '"'},
        {"apos", '\''},
    };
    size_t i;
    if (len == 0) return false;
    if (name[0] == '#') {
        uint32_t value = 0;
        unsigned base = 10;
        i = 1;
        if (i < len && (name[i] == 'x' || name[i] == 'X')) {
            base = 16;
            i++;
        }
        if (i == len) return false;
        for (; i < len; i++) {
            unsigned digit;
            char c = name[i];
            if (c >= '0' && c <= '9') {
                digit = (unsigned)(c - '0');
            } else if (base == 16 && c >= 'a' && c <= 'f') {
                digit = (unsigned)(c - 'a' + 10);
            } else if (base == 16 && c >= 'A' && c <= 'F') {
                digit = (unsigned)(c - 'A' + 10);
            } else {
                return false;
            }
            if (digit >= base) return false;
            if (value > (0x10FFFFu - digit) / base) return false;
            value = value * base + digit;
        }
        if (value == 0 || value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) return false;
        *cp = value;
        return true;
    }
    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (strlen(named[i].name) == len && ci_equal_n(name, named[i].name, len)) {
            *cp = named[i].value;
            return true;
        }
    }
    return false;
}

static size_t utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* ---------- 文本暂存 / Text accumulation ---------- */

static void text_reset(struct opds_feed_parser* parser) {
    parser->text_len = 0;
    parser->text_pending_space = false;
    parser->text_overflow = false;
}

static void text_begin(struct opds_feed_parser* parser, opds_text_target_t target, uint8_t level) {
    if (parser->text_target != TT_NONE) return;
    parser->text_target = target;
    parser->text_level = level;
    text_reset(parser);
}

static void text_emit_byte(struct opds_feed_parser* parser, unsigned char ch) {
    if (parser->text_target == TT_NONE) return;
    if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        if (parser->text_len > 0) parser->text_pending_space = true;
        return;
    }
    if (ch < 0x20 || ch == 0x7F) return; /* 控制字符丢弃 / drop controls */
    if (parser->text_pending_space) {
        if (parser->text_len + 1 < sizeof(parser->text)) {
            parser->text[parser->text_len++] = ' ';
        } else {
            parser->text_overflow = true;
        }
        parser->text_pending_space = false;
    }
    if (parser->text_len + 1 < sizeof(parser->text)) {
        parser->text[parser->text_len++] = (char)ch;
    } else {
        parser->text_overflow = true;
    }
}

static void text_emit_cp(struct opds_feed_parser* parser, uint32_t cp) {
    char bytes[4];
    size_t i;
    size_t n = utf8_encode(cp, bytes);
    for (i = 0; i < n; i++) text_emit_byte(parser, (unsigned char)bytes[i]);
}

static void text_append_literal(struct opds_feed_parser* parser, const char* text, size_t len) {
    size_t i;
    for (i = 0; i < len; i++) text_emit_byte(parser, (unsigned char)text[i]);
}

static void text_commit(struct opds_feed_parser* parser) {
    if (parser->text_target == TT_NONE) return;
    parser->text[parser->text_len] = '\0';
    parser->text_pending_space = false;
    switch (parser->text_target) {
        case TT_FEED_TITLE:
            if (!parser->feed_title_set) {
                copy_utf8_truncated(parser->info.feed_title, OPDS_FEED_TITLE_MAX, parser->text, parser->text_len);
                parser->feed_title_set = true;
            }
            break;
        case TT_ENTRY_TITLE:
            if (!parser->entry_title_set) {
                copy_utf8_truncated(parser->current.title, OPDS_TITLE_MAX, parser->text, parser->text_len);
                parser->entry_title_set = true;
            }
            break;
        case TT_ENTRY_AUTHOR:
            if (!parser->entry_author_set) {
                copy_utf8_truncated(parser->current.author, OPDS_AUTHOR_MAX, parser->text, parser->text_len);
                parser->entry_author_set = true;
            }
            break;
        case TT_ENTRY_ID:
            if (!parser->entry_id_set) {
                copy_utf8_truncated(parser->current.id, OPDS_ID_MAX, parser->text, parser->text_len);
                parser->entry_id_set = true;
            }
            break;
        default:
            break;
    }
    parser->text_target = TT_NONE;
    text_reset(parser);
}

/* ---------- 属性解析 / Attribute parsing ---------- */

static void attr_reset(struct opds_feed_parser* parser) {
    parser->attr_has_rel = false;
    parser->attr_has_href = false;
    parser->attr_has_type = false;
    parser->attr_length = 0;
    parser->attr_bad = false;
    parser->attr_rel[0] = '\0';
    parser->attr_href[0] = '\0';
    parser->attr_type[0] = '\0';
    parser->attr_val_len = 0;
}

static void attr_add_byte(struct opds_feed_parser* parser, unsigned char ch) {
    if (parser->attr_val_len + 1 < sizeof(parser->attr_val)) {
        parser->attr_val[parser->attr_val_len++] = (char)ch;
    } else {
        parser->attr_bad = true;
    }
}

/* 属性值：实体解码 + 宽度保护 / Decode an attribute value, keeping it bounded. */
static void attr_decode_value(struct opds_feed_parser* parser, const char* text, size_t len) {
    size_t i = 0;
    parser->attr_val_len = 0;
    while (i < len) {
        char c = text[i];
        if (c == '&') {
            size_t j = i + 1;
            size_t limit = j + OPDS_ENTITY_MAX;
            while (j < len && j < limit && text[j] != ';') j++;
            if (j < len && text[j] == ';') {
                uint32_t cp = 0;
                if (entity_decode(text + i + 1, j - i - 1, &cp)) {
                    char bytes[4];
                    size_t k;
                    size_t n = utf8_encode(cp, bytes);
                    for (k = 0; k < n; k++) attr_add_byte(parser, (unsigned char)bytes[k]);
                } else {
                    size_t k;
                    attr_add_byte(parser, (unsigned char)'&');
                    for (k = i + 1; k <= j; k++) attr_add_byte(parser, (unsigned char)text[k]);
                }
                i = j + 1;
                continue;
            }
            attr_add_byte(parser, (unsigned char)'&');
            i++;
            continue;
        }
        attr_add_byte(parser, (unsigned char)c);
        i++;
    }
}

/* 属性值首尾去空白 / Trim spaces at both ends of a value. */
static void attr_trim(struct opds_feed_parser* parser, size_t* start, size_t* end) {
    size_t s = 0;
    size_t e = parser->attr_val_len;
    while (s < e && is_space_char(parser->attr_val[s])) s++;
    while (e > s && is_space_char(parser->attr_val[e - 1])) e--;
    *start = s;
    *end = e;
}

static bool attr_copy_bounded(char* dst, size_t cap, const char* src, size_t len, bool* too_long) {
    if (len >= cap) {
        *too_long = true;
        return false;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return true;
}

static uint32_t attr_parse_length(const char* text, size_t len) {
    uint32_t value = 0;
    bool any = false;
    size_t i;
    for (i = 0; i < len; i++) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            any = true;
            if (value > (0xFFFFFFFFu - (uint32_t)(c - '0')) / 10u) return 0xFFFFFFFFu;
            value = value * 10u + (uint32_t)(c - '0');
        } else if (is_space_char(c)) {
            continue;
        } else {
            return 0;
        }
    }
    return any ? value : 0;
}

static void parse_attrs(struct opds_feed_parser* parser, const char* attrs, size_t len) {
    size_t i = 0;
    attr_reset(parser);
    while (i < len) {
        size_t name_start;
        size_t name_len;
        size_t value_start;
        size_t value_end;
        char name[OPDS_NAME_MAX];
        char quote;
        while (i < len && is_space_char(attrs[i])) i++;
        if (i >= len) break;
        name_start = i;
        while (i < len && !is_space_char(attrs[i]) && attrs[i] != '=') i++;
        name_len = i - name_start;
        while (i < len && is_space_char(attrs[i])) i++;
        if (i >= len || attrs[i] != '=') continue; /* 裸属性忽略 / bare attribute skipped */
        i++;
        while (i < len && is_space_char(attrs[i])) i++;
        if (i >= len) break;
        quote = attrs[i];
        if (quote != '"' && quote != '\'') continue; /* 属性必须加引号 / must be quoted */
        i++;
        value_start = i;
        while (i < len && attrs[i] != quote) i++;
        value_end = i;
        if (i < len) i++; /* 跳过收尾引号 / skip closing quote */
        if (name_len == 0 || name_len >= sizeof(name)) continue;
        local_name_lower(attrs + name_start, name_len, name, sizeof(name));
        attr_decode_value(parser, attrs + value_start, value_end - value_start);
        {
            size_t s = 0;
            size_t e = 0;
            attr_trim(parser, &s, &e);
            if (strcmp(name, "href") == 0) {
                parser->attr_has_href =
                    attr_copy_bounded(parser->attr_href, sizeof(parser->attr_href), parser->attr_val + s, e - s,
                                      &parser->attr_bad);
            } else if (strcmp(name, "rel") == 0) {
                parser->attr_has_rel =
                    attr_copy_bounded(parser->attr_rel, sizeof(parser->attr_rel), parser->attr_val + s, e - s,
                                      &parser->attr_bad);
            } else if (strcmp(name, "type") == 0) {
                parser->attr_has_type =
                    attr_copy_bounded(parser->attr_type, sizeof(parser->attr_type), parser->attr_val + s, e - s,
                                      &parser->attr_bad);
            } else if (strcmp(name, "length") == 0) {
                parser->attr_length = attr_parse_length(parser->attr_val + s, e - s);
            }
        }
    }
}

/* ---------- 链接分类 / Link classification ---------- */

static bool is_acquisition_rel(const char* rel) {
    static const char base[] = "http://opds-spec.org/acquisition";
    if (ci_equal(rel, base)) return true;
    return ci_starts_with(rel, "http://opds-spec.org/acquisition/");
}

static bool is_epub_type(const char* type) {
    const char* semicolon = strchr(type, ';');
    size_t len = semicolon ? (size_t)(semicolon - type) : strlen(type);
    while (len > 0 && is_space_char(type[len - 1])) len--;
    return len == strlen("application/epub+zip") && ci_equal_n(type, "application/epub+zip", len);
}

static int acquisition_rank(const char* rel) {
    static const char base[] = "http://opds-spec.org/acquisition";
    const char* rest;
    size_t base_len = strlen(base);
    if (!ci_equal(rel, base)) {
        if (!ci_starts_with(rel, "http://opds-spec.org/acquisition/")) return -1;
        rest = rel + base_len + 1;
        if (ci_equal(rest, "open-access")) return 0;
        if (ci_equal(rest, "borrow")) return 2;
        if (ci_equal(rest, "buy")) return 3;
        if (ci_equal(rest, "subscribe")) return 4;
        return -1; /* sample/unknown: 不下载 / not downloaded */
    }
    return 1;
}

static int navigation_rank(const char* rel, const char* type) {
    if (is_acquisition_rel(rel)) return -1;
    if (ci_equal(rel, "subsection")) return 0;
    if (ci_starts_with(rel, "http://opds-spec.org/sort/")) return 1;
    if (ci_contains(type, "profile=opds-catalog")) return 2;
    if (ci_starts_with(type, "application/atom+xml")) return 3;
    return -1;
}

static bool resolve_href(struct opds_feed_parser* parser, const char* href, char* out, size_t cap) {
    return opds_resolve_url(parser->base_url, href, out, cap);
}

static void copy_url(char* dst, size_t cap, const char* src) {
    size_t len = strlen(src);
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void feed_link(struct opds_feed_parser* parser) {
    if (!parser->attr_has_href || parser->attr_bad) return;
    if (!resolve_href(parser, parser->attr_href, parser->resolve_buf, sizeof(parser->resolve_buf))) return;
    if (!parser->attr_has_rel) return;
    if (ci_equal(parser->attr_rel, "next")) {
        copy_url(parser->info.next, sizeof(parser->info.next), parser->resolve_buf);
    } else if (ci_equal(parser->attr_rel, "previous") || ci_equal(parser->attr_rel, "prev")) {
        copy_url(parser->info.previous, sizeof(parser->info.previous), parser->resolve_buf);
    } else if (ci_equal(parser->attr_rel, "first")) {
        copy_url(parser->info.first, sizeof(parser->info.first), parser->resolve_buf);
    } else if (ci_equal(parser->attr_rel, "last")) {
        copy_url(parser->info.last, sizeof(parser->info.last), parser->resolve_buf);
    } else if (ci_equal(parser->attr_rel, "self")) {
        copy_url(parser->info.self, sizeof(parser->info.self), parser->resolve_buf);
    }
}

static void entry_link(struct opds_feed_parser* parser) {
    if (!parser->attr_has_href || parser->attr_bad) return;
    if (!resolve_href(parser, parser->attr_href, parser->resolve_buf, sizeof(parser->resolve_buf))) return;
    if (is_acquisition_rel(parser->attr_rel)) {
        int rank;
        if (!is_epub_type(parser->attr_type)) return; /* 只收直接 EPUB / only direct EPUB */
        rank = acquisition_rank(parser->attr_rel);
        if (rank < 0) return;
        if (!parser->have_acq || rank < parser->acq_rank) {
            copy_url(parser->acq_href, sizeof(parser->acq_href), parser->resolve_buf);
            copy_url(parser->acq_type, sizeof(parser->acq_type), parser->attr_type);
            copy_url(parser->acq_rel, sizeof(parser->acq_rel), parser->attr_rel);
            parser->acq_length = parser->attr_length;
            parser->acq_rank = rank;
            parser->have_acq = true;
        }
        return;
    }
    {
        int rank = navigation_rank(parser->attr_rel, parser->attr_type);
        if (rank < 0) return;
        if (!parser->have_nav || rank < parser->nav_rank) {
            copy_url(parser->nav_href, sizeof(parser->nav_href), parser->resolve_buf);
            copy_url(parser->nav_type, sizeof(parser->nav_type), parser->attr_type);
            copy_url(parser->nav_rel, sizeof(parser->nav_rel), parser->attr_rel);
            parser->nav_rank = rank;
            parser->have_nav = true;
        }
    }
}

/* ---------- 条目生命周期 / Entry lifecycle ---------- */

static void entry_begin(struct opds_feed_parser* parser, uint8_t level) {
    memset(&parser->current, 0, sizeof(parser->current));
    parser->entry_level = level;
    parser->in_entry = true;
    parser->in_author = false;
    parser->entry_title_set = false;
    parser->entry_author_set = false;
    parser->entry_id_set = false;
    parser->have_acq = false;
    parser->have_nav = false;
    parser->acq_rank = OPDS_NO_ENTRY_RANK;
    parser->nav_rank = OPDS_NO_ENTRY_RANK;
    parser->text_target = TT_NONE;
}

static bool entry_finalize(struct opds_feed_parser* parser) {
    opds_entry_kind_t kind;
    if (parser->info.total_entries < (uint16_t)0xFFFFu) parser->info.total_entries++;
    if (parser->info.total_entries > OPDS_MAX_ENTRIES) parser->info.truncated = true;

    if (parser->have_acq) {
        kind = OPDS_ENTRY_BOOK;
    } else if (parser->have_nav) {
        kind = OPDS_ENTRY_NAVIGATION;
    } else {
        return true; /* OTHER 不保留 / not retained */
    }

    if (parser->info.entry_count >= OPDS_MAX_ENTRIES) {
        parser->info.truncated = true;
        return true;
    }
    if (!parser->entries) {
        parser->entries = (opds_entry_t*)(
            parser->memory.alloc ? parser->memory.alloc(parser->memory.ctx, sizeof(opds_entry_t) * OPDS_MAX_ENTRIES)
                                 : malloc(sizeof(opds_entry_t) * OPDS_MAX_ENTRIES));
        if (!parser->entries) {
            parser->failed = true;
            return false;
        }
        memset(parser->entries, 0, sizeof(opds_entry_t) * OPDS_MAX_ENTRIES);
    }
    {
        opds_entry_t* entry = &parser->entries[parser->info.entry_count];
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->title, parser->current.title, sizeof(entry->title));
        memcpy(entry->author, parser->current.author, sizeof(entry->author));
        memcpy(entry->id, parser->current.id, sizeof(entry->id));
        entry->kind = (uint8_t)kind;
        if (kind == OPDS_ENTRY_BOOK) {
            memcpy(entry->href, parser->acq_href, sizeof(entry->href));
            memcpy(entry->type, parser->acq_type, sizeof(entry->type));
            memcpy(entry->rel, parser->acq_rel, sizeof(entry->rel));
            entry->length = parser->acq_length;
        } else {
            memcpy(entry->href, parser->nav_href, sizeof(entry->href));
            memcpy(entry->type, parser->nav_type, sizeof(entry->type));
            memcpy(entry->rel, parser->nav_rel, sizeof(entry->rel));
            entry->length = 0;
        }
        parser->info.entry_count++;
    }
    return true;
}

/* ---------- 元素处理 / Element handling ---------- */

static bool close_element(struct opds_feed_parser* parser, uint8_t level) {
    if (parser->text_target != TT_NONE && parser->text_level == level) text_commit(parser);
    if (parser->in_author && parser->author_level == level) parser->in_author = false;
    if (parser->in_entry && parser->entry_level == level) {
        if (!entry_finalize(parser)) return false;
        parser->in_entry = false;
    }
    if (parser->in_feed && level == 1) parser->in_feed = false;
    return true;
}

static bool handle_start_tag(struct opds_feed_parser* parser, const char* name, const char* attrs, size_t attrs_len) {
    uint8_t level = (uint8_t)(parser->depth + 1);

    if (parser->depth == 0) {
        if (parser->seen_root) {
            parser->malformed = true;
            return true;
        }
        parser->seen_root = true;
        if (strcmp(name, "feed") == 0) {
            parser->in_feed = true;
        } else {
            parser->malformed = true;
        }
        return true;
    }
    if (parser->in_feed && !parser->in_entry && parser->depth == 1) {
        if (strcmp(name, "title") == 0) {
            if (!parser->feed_title_set) text_begin(parser, TT_FEED_TITLE, level);
        } else if (strcmp(name, "link") == 0) {
            parse_attrs(parser, attrs, attrs_len);
            feed_link(parser);
        } else if (strcmp(name, "entry") == 0) {
            entry_begin(parser, level);
        }
        return true;
    }
    if (parser->in_entry) {
        if (parser->depth == parser->entry_level) {
            if (strcmp(name, "title") == 0) {
                if (!parser->entry_title_set) text_begin(parser, TT_ENTRY_TITLE, level);
            } else if (strcmp(name, "id") == 0) {
                if (!parser->entry_id_set) text_begin(parser, TT_ENTRY_ID, level);
            } else if (strcmp(name, "author") == 0) {
                parser->in_author = true;
                parser->author_level = level;
            } else if (strcmp(name, "link") == 0) {
                parse_attrs(parser, attrs, attrs_len);
                entry_link(parser);
            }
        } else if (parser->in_author && parser->depth == parser->author_level && strcmp(name, "name") == 0) {
            if (!parser->entry_author_set) text_begin(parser, TT_ENTRY_AUTHOR, level);
        }
    }
    return true;
}

static bool handle_end_tag(struct opds_feed_parser* parser, const char* name) {
    uint8_t level;
    bool ok;
    if (parser->depth == 0) {
        parser->malformed = true;
        return true;
    }
    if (strcmp(parser->stack[parser->depth - 1], name) != 0) {
        parser->malformed = true; /* 结构错乱：只标记，不 poison 单条 / mark only */
        return true;
    }
    level = parser->depth;
    ok = close_element(parser, level);
    parser->depth--;
    return ok;
}

static void stack_push(struct opds_feed_parser* parser, const char* name) {
    size_t len;
    if ((unsigned)parser->depth >= OPDS_STACK_MAX) {
        parser->malformed = true;
        return;
    }
    len = strlen(name);
    if (len >= OPDS_NAME_MAX) len = OPDS_NAME_MAX - 1;
    memcpy(parser->stack[parser->depth], name, len);
    parser->stack[parser->depth][len] = '\0';
    parser->depth++;
}

/* ---------- 词法器 / Lexer ---------- */

static void tag_add(struct opds_feed_parser* parser, unsigned char ch) {
    if (!parser->tag_overflow) {
        if (parser->tag_len + 1 < sizeof(parser->tag)) {
            parser->tag[parser->tag_len++] = (char)ch;
        } else {
            parser->tag_overflow = true;
        }
    }
}

static void start_tag_from_lt(struct opds_feed_parser* parser) {
    size_t i;
    parser->tag_len = 0;
    parser->tag_overflow = false;
    parser->tag_quote = 0;
    for (i = 0; i < parser->lt_len; i++) tag_add(parser, (unsigned char)parser->lt[i]);
    parser->state = XS_TAG;
}

static bool is_lt_prefix(const char* lt, size_t len, const char* literal) {
    size_t i;
    size_t n = strlen(literal);
    if (len > n) return false;
    for (i = 0; i < len; i++) {
        if (ascii_lower_char(lt[i]) != ascii_lower_char(literal[i])) return false;
    }
    return true;
}

static bool tag_complete(struct opds_feed_parser* parser) {
    size_t len = parser->tag_len;
    bool empty = false;
    bool closing;
    size_t i;
    size_t name_start;
    size_t name_len;
    char name[OPDS_NAME_MAX];

    while (len > 1 && is_space_char(parser->tag[len - 1])) len--;
    if (len > 1 && parser->tag[len - 1] == '/') {
        empty = true;
        len--;
        while (len > 1 && is_space_char(parser->tag[len - 1])) len--;
    }
    if (len < 2) {
        parser->malformed = true;
        return true;
    }
    closing = parser->tag[1] == '/';
    i = closing ? 2u : 1u;
    name_start = i;
    while (i < len && !is_space_char(parser->tag[i]) && parser->tag[i] != '/') i++;
    name_len = i - name_start;
    if (name_len == 0) {
        parser->malformed = true;
        return true;
    }
    local_name_lower(parser->tag + name_start, name_len, name, sizeof(name));
    if (name[0] == '\0') {
        parser->malformed = true;
        return true;
    }
    if (closing) return handle_end_tag(parser, name);
    if (!handle_start_tag(parser, name, parser->tag + i, len - i)) return false;
    if (empty) return close_element(parser, (uint8_t)(parser->depth + 1));
    stack_push(parser, name);
    return true;
}

static bool tag_byte(struct opds_feed_parser* parser, unsigned char ch) {
    if (parser->tag_quote != 0) {
        if ((char)ch == parser->tag_quote) parser->tag_quote = 0;
        tag_add(parser, ch);
        return true;
    }
    if (ch == '"' || ch == '\'') {
        parser->tag_quote = (char)ch;
        tag_add(parser, ch);
        return true;
    }
    if (ch == '>') {
        parser->state = XS_TEXT;
        if (parser->tag_overflow) {
            parser->tag_overflow = false;
            return true;
        }
        return tag_complete(parser);
    }
    tag_add(parser, ch);
    return true;
}

static bool lt_byte(struct opds_feed_parser* parser, unsigned char ch) {
    if (parser->lt_len < sizeof(parser->lt)) parser->lt[parser->lt_len] = (char)ch;
    parser->lt_len++;
    if (parser->lt_len > sizeof(parser->lt)) {
        parser->state = XS_BOGUS;
        parser->bogus_quote = 0;
        return true;
    }
    if (parser->lt_len == 1) return true; /* 只有 '<' 时不会走到这里 / '<' only */
    if (parser->lt[1] == '!') {
        if (parser->lt_len == 2) return true;
        if (parser->lt[2] == '-') {
            if (parser->lt_len == 3) return true;
            if (parser->lt[3] == '-') {
                parser->state = XS_COMMENT;
                parser->term = 0;
                return true;
            }
            parser->state = XS_BOGUS;
            parser->bogus_quote = 0;
            return true;
        }
        if (parser->lt[2] == '[' || parser->lt[2] == 'D' || parser->lt[2] == 'd' ||
            parser->lt[2] == 'C' || parser->lt[2] == 'c') {
            if (parser->lt_len < 9) {
                if (is_lt_prefix(parser->lt, parser->lt_len, "<![CDATA[") ||
                    is_lt_prefix(parser->lt, parser->lt_len, "<!DOCTYPE")) {
                    return true;
                }
                parser->state = XS_BOGUS;
                parser->bogus_quote = 0;
                return true;
            }
            if (ci_equal_n(parser->lt, "<![CDATA[", 9)) {
                parser->state = XS_CDATA;
                parser->cdata_len = 0;
                return true;
            }
            if (ci_equal_n(parser->lt, "<!DOCTYPE", 9)) {
                parser->state = XS_DOCTYPE;
                parser->dt_quote = 0;
                parser->dt_brackets = 0;
                return true;
            }
            parser->state = XS_BOGUS;
            parser->bogus_quote = 0;
            return true;
        }
        parser->state = XS_BOGUS;
        parser->bogus_quote = 0;
        return true;
    }
    if (parser->lt[1] == '?') {
        parser->state = XS_PI;
        parser->term = 0;
        return true;
    }
    start_tag_from_lt(parser);
    return true;
}

static bool cdata_byte(struct opds_feed_parser* parser, unsigned char ch) {
    static const char tail[] = "]]>";
    for (;;) {
        if ((unsigned)parser->cdata_len < 3u && (char)ch == tail[parser->cdata_len]) {
            parser->cdata_tail[parser->cdata_len++] = (char)ch;
            if (parser->cdata_len == 3u) {
                parser->cdata_len = 0;
                parser->state = XS_TEXT;
            }
            return true;
        }
        if (parser->cdata_len == 0u) {
            text_emit_byte(parser, ch);
            return true;
        }
        text_emit_byte(parser, (unsigned char)parser->cdata_tail[0]);
        memmove(parser->cdata_tail, parser->cdata_tail + 1, (size_t)(parser->cdata_len - 1u));
        parser->cdata_len--;
    }
}

static bool entity_byte(struct opds_feed_parser* parser, unsigned char ch) {
    if (ch == ';') {
        uint32_t cp = 0;
        if (entity_decode(parser->entity, parser->entity_len, &cp)) {
            text_emit_cp(parser, cp);
        } else {
            text_emit_byte(parser, (unsigned char)'&');
            text_append_literal(parser, parser->entity, parser->entity_len);
            text_emit_byte(parser, (unsigned char)';');
        }
        parser->state = XS_TEXT;
        parser->entity_len = 0;
        return true;
    }
    if (parser->entity_len + 1 < sizeof(parser->entity) && ch != '<' && ch != '&' && !is_space_char((char)ch) &&
        ch >= 0x20 && ch != 0x7F) {
        parser->entity[parser->entity_len++] = (char)ch;
        return true;
    }
    text_emit_byte(parser, (unsigned char)'&');
    text_append_literal(parser, parser->entity, parser->entity_len);
    parser->entity_len = 0;
    parser->state = XS_TEXT;
    return xml_byte(parser, ch);
}

static bool xml_byte(struct opds_feed_parser* parser, unsigned char ch) {
    switch (parser->state) {
        case XS_TEXT:
            if (ch == '<') {
                parser->state = XS_LT;
                parser->lt[0] = '<';
                parser->lt_len = 1;
            } else if (ch == '&') {
                parser->state = XS_ENTITY;
                parser->entity_len = 0;
            } else {
                text_emit_byte(parser, ch);
            }
            return true;
        case XS_LT:
            return lt_byte(parser, ch);
        case XS_TAG:
            return tag_byte(parser, ch);
        case XS_COMMENT:
            if (ch == (unsigned char)"-->"[parser->term]) {
                parser->term++;
                if (parser->term == 3u) {
                    parser->term = 0;
                    parser->state = XS_TEXT;
                }
            } else {
                parser->term = (ch == '-') ? 1u : 0u;
            }
            return true;
        case XS_PI:
            if (parser->term == 0u) {
                if (ch == '?') parser->term = 1u;
            } else if (ch == '>') {
                parser->term = 0;
                parser->state = XS_TEXT;
            } else {
                parser->term = (ch == '?') ? 1u : 0u;
            }
            return true;
        case XS_CDATA:
            return cdata_byte(parser, ch);
        case XS_DOCTYPE:
            if (parser->dt_quote != 0) {
                if ((char)ch == parser->dt_quote) parser->dt_quote = 0;
            } else if (ch == '"' || ch == '\'') {
                parser->dt_quote = (char)ch;
            } else if (ch == '[') {
                parser->dt_brackets++;
            } else if (ch == ']') {
                if (parser->dt_brackets > 0) parser->dt_brackets--;
            } else if (ch == '>' && parser->dt_brackets == 0) {
                parser->state = XS_TEXT;
            }
            return true;
        case XS_BOGUS:
            if (parser->bogus_quote != 0) {
                if ((char)ch == parser->bogus_quote) parser->bogus_quote = 0;
            } else if (ch == '"' || ch == '\'') {
                parser->bogus_quote = (char)ch;
            } else if (ch == '>') {
                parser->state = XS_TEXT;
            }
            return true;
        case XS_ENTITY:
            return entity_byte(parser, ch);
        default:
            parser->failed = true;
            return false;
    }
}

/* ---------- 公共 API / Public API ---------- */

static void* parser_alloc(const opds_feed_memory_t* memory, size_t size) {
    if (memory && memory->alloc) return memory->alloc(memory->ctx, size);
    return malloc(size);
}

static void parser_free(const opds_feed_memory_t* memory, void* pointer) {
    if (!pointer) return;
    if (memory && memory->free) {
        memory->free(memory->ctx, pointer);
        return;
    }
    free(pointer);
}

opds_feed_parser_t* opds_feed_parser_create(const char* base_url, const opds_feed_memory_t* memory) {
    struct opds_feed_parser* parser;
    size_t base_len;
    if (!base_url) return NULL;
    base_len = strlen(base_url);
    if (base_len == 0 || base_len >= OPDS_URL_MAX) return NULL;
    parser = (struct opds_feed_parser*)parser_alloc(memory, sizeof(*parser));
    if (!parser) return NULL;
    memset(parser, 0, sizeof(*parser));
    if (memory) {
        parser->memory = *memory;
    } else {
        parser->memory.alloc = NULL;
        parser->memory.free = NULL;
        parser->memory.ctx = NULL;
    }
    memcpy(parser->base_url, base_url, base_len + 1);
    parser->state = XS_TEXT;
    parser->acq_rank = OPDS_NO_ENTRY_RANK;
    parser->nav_rank = OPDS_NO_ENTRY_RANK;
    parser->text_target = TT_NONE;
    return parser;
}

bool opds_feed_parser_feed(opds_feed_parser_t* parser, const char* data, size_t len) {
    size_t i;
    if (!parser) return false;
    if (!data && len != 0) return false;
    if (parser->failed || parser->finished) return false;
    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)data[i];
        if (!parser->bom_done) {
            bool prefix = true;
            size_t k;
            if ((size_t)parser->bom_len < sizeof(parser->bom)) parser->bom[parser->bom_len] = (char)ch;
            parser->bom_len++;
            for (k = 0; k < (size_t)parser->bom_len && k < 3u; k++) {
                static const unsigned char bom_bytes[3] = {0xEFu, 0xBBu, 0xBFu};
                if ((unsigned char)parser->bom[k] != bom_bytes[k]) {
                    prefix = false;
                    break;
                }
            }
            if (prefix) {
                if (parser->bom_len == 3u) {
                    parser->bom_done = true;
                    parser->bom_len = 0;
                }
                continue;
            }
            parser->bom_done = true;
            for (k = 0; k < (size_t)parser->bom_len && k < sizeof(parser->bom); k++) {
                if (!xml_byte(parser, (unsigned char)parser->bom[k])) return false;
            }
            parser->bom_len = 0;
            continue;
        }
        if (!xml_byte(parser, ch)) return false;
    }
    return !parser->failed;
}

bool opds_feed_parser_finish(opds_feed_parser_t* parser) {
    if (!parser) return false;
    if (parser->finished) return parser->ok;
    if (!parser->failed && !parser->bom_done && parser->bom_len > 0) {
        size_t k;
        for (k = 0; k < (size_t)parser->bom_len && k < sizeof(parser->bom); k++) {
            if (!xml_byte(parser, (unsigned char)parser->bom[k])) return false;
        }
        parser->bom_len = 0;
    }
    parser->finished = true;
    parser->ok = !parser->failed && !parser->malformed && parser->seen_root && parser->depth == 0 &&
                 parser->state == XS_TEXT;
    return parser->ok;
}

void opds_feed_parser_destroy(opds_feed_parser_t* parser) {
    opds_feed_memory_t memory;
    if (!parser) return;
    memory = parser->memory;
    parser_free(&memory, parser->entries);
    parser_free(&memory, parser);
}

uint16_t opds_feed_parser_entry_count(const opds_feed_parser_t* parser) {
    if (!parser) return 0;
    return parser->info.entry_count;
}

bool opds_feed_parser_entry(const opds_feed_parser_t* parser, uint16_t index, opds_entry_t* out) {
    if (!parser || !out || !parser->entries) return false;
    if (index >= parser->info.entry_count) return false;
    *out = parser->entries[index];
    return true;
}

void opds_feed_parser_info(const opds_feed_parser_t* parser, opds_feed_info_t* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!parser) return;
    *out = parser->info;
}

/* ---------- RFC 3986 §5.2 相对 URL 解析 / Relative URL resolution ---------- */

static bool put_text(char* buf, size_t* used, size_t cap, const char* text, size_t len) {
    if (len > cap - *used - 1) return false;
    memcpy(buf + *used, text, len);
    *used += len;
    buf[*used] = '\0';
    return true;
}

static bool scheme_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' ||
           c == '.';
}

/* scheme 长度（":" 之前的字符数），0 表示不是 scheme。 */
static size_t scheme_prefix_len(const char* url) {
    size_t i;
    if (!((url[0] >= 'a' && url[0] <= 'z') || (url[0] >= 'A' && url[0] <= 'Z'))) return 0;
    for (i = 1; url[i]; i++) {
        if (url[i] == ':') return i;
        if (!scheme_char(url[i])) return 0;
    }
    return 0;
}

/* 查找 "scheme://" 的结束位置；0 表示不是带 authority 的绝对 URL。 */
static size_t absolute_prefix_len(const char* url) {
    size_t scheme_len = scheme_prefix_len(url);
    if (scheme_len == 0) return 0;
    if (url[scheme_len + 1] != '/' || url[scheme_len + 2] != '/') return 0;
    return scheme_len + 3;
}

/* 路径去点段（RFC 3986 §5.2.4）；输出可能为空串。 */
static bool remove_dot_segments(const char* path, size_t path_len, char* out, size_t cap) {
    size_t segments[64];
    size_t segment_lens[64];
    size_t count = 0;
    size_t i;
    size_t used = 0;
    bool absolute = path_len > 0 && path[0] == '/';
    bool trailing = false;

    i = absolute ? 1u : 0u;
    while (i < path_len) {
        size_t start = i;
        size_t len;
        while (i < path_len && path[i] != '/') i++;
        len = i - start;
        if (len == 1 && path[start] == '.') {
            if (i >= path_len) trailing = true;
        } else if (len == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (count > 0) count--;
            if (i >= path_len) trailing = true;
        } else if (len > 0) {
            if (count >= sizeof(segments) / sizeof(segments[0])) return false;
            segments[count] = start;
            segment_lens[count] = len;
            count++;
        } else if (i >= path_len) {
            trailing = true;
        }
        if (i < path_len && path[i] == '/') {
            i++;
            if (i >= path_len) trailing = true;
        }
    }
    if (absolute) {
        if (used + 1 >= cap) return false;
        out[used++] = '/';
    }
    {
        size_t k;
        for (k = 0; k < count; k++) {
            size_t len = segment_lens[k];
            if (k > 0) {
                if (used + 1 >= cap) return false;
                out[used++] = '/';
            }
            if (len >= cap - used) return false;
            memcpy(out + used, path + segments[k], len);
            used += len;
        }
    }
    if (trailing && used > 0 && out[used - 1] != '/') {
        if (used + 1 >= cap) return false;
        out[used++] = '/';
    }
    out[used] = '\0';
    return true;
}

/* 合并后的绝对 URL 去 fragment，并规范化路径点段。 */
static bool normalize_absolute(const char* url, char* out, size_t cap) {
    size_t scheme_end = absolute_prefix_len(url);
    size_t auth_end;
    size_t path_start;
    size_t path_end;
    size_t query_start = 0;
    size_t used = 0;
    char normalized[OPDS_COPY_MAX];

    if (scheme_end == 0) {
        size_t n = 0;
        while (url[n] && url[n] != '#') n++;
        if (n >= cap) return false;
        memcpy(out, url, n);
        out[n] = '\0';
        return true;
    }
    auth_end = scheme_end;
    while (url[auth_end] && url[auth_end] != '/' && url[auth_end] != '?' && url[auth_end] != '#') auth_end++;
    path_start = auth_end;
    path_end = auth_end;
    if (url[path_start] == '/') {
        while (url[path_end] && url[path_end] != '?' && url[path_end] != '#') path_end++;
    }
    if (url[path_end] == '?') query_start = path_end;
    if (!remove_dot_segments(url + path_start, path_end - path_start, normalized, sizeof(normalized))) return false;
    if (!put_text(out, &used, cap, url, auth_end)) return false;
    if (!put_text(out, &used, cap, normalized, strlen(normalized))) return false;
    if (query_start > 0) {
        size_t q = query_start;
        while (url[q] && url[q] != '#') q++;
        if (!put_text(out, &used, cap, url + query_start, q - query_start)) return false;
    }
    return true;
}

static bool base_parts(const char* base, size_t* scheme_end, size_t* auth_end, size_t* path_start, size_t* path_end) {
    size_t se = absolute_prefix_len(base);
    size_t ae;
    size_t ps;
    size_t pe;
    if (se == 0) return false;
    ae = se;
    while (base[ae] && base[ae] != '/' && base[ae] != '?' && base[ae] != '#') ae++;
    ps = ae;
    pe = ae;
    if (base[ps] == '/') {
        while (base[pe] && base[pe] != '?' && base[pe] != '#') pe++;
    }
    *scheme_end = se;
    *auth_end = ae;
    *path_start = ps;
    *path_end = pe;
    return true;
}

bool opds_resolve_url(const char* base, const char* ref, char* out, size_t cap) {
    char candidate[OPDS_COPY_MAX];
    size_t scheme_end;
    size_t auth_end;
    size_t path_start;
    size_t path_end;
    size_t used = 0;

    if (!base || !ref || !out || cap < 2) return false;
    if (base[0] == '\0' || ref[0] == '\0') return false;

    /* 绝对引用：与 base 无关 / absolute reference needs no base. */
    if (scheme_prefix_len(ref) != 0) return normalize_absolute(ref, out, cap);

    if (!base_parts(base, &scheme_end, &auth_end, &path_start, &path_end)) return false;

    if (ref[0] == '/' && ref[1] == '/') {
        /* scheme + ":" + "//host..." / protocol-relative. */
        if (!put_text(candidate, &used, sizeof(candidate), base, scheme_end - 2u)) return false;
        if (!put_text(candidate, &used, sizeof(candidate), ref, strlen(ref))) return false;
    } else if (ref[0] == '/') {
        if (!put_text(candidate, &used, sizeof(candidate), base, auth_end)) return false;
        if (!put_text(candidate, &used, sizeof(candidate), ref, strlen(ref))) return false;
    } else if (ref[0] == '?') {
        if (!put_text(candidate, &used, sizeof(candidate), base, path_end)) return false;
        if (!put_text(candidate, &used, sizeof(candidate), ref, strlen(ref))) return false;
    } else if (ref[0] == '#') {
        size_t base_len = strlen(base);
        size_t cut = base_len;
        size_t i;
        for (i = 0; i < base_len; i++) {
            if (base[i] == '#') {
                cut = i;
                break;
            }
        }
        if (cut >= cap) return false;
        memcpy(out, base, cut);
        out[cut] = '\0';
        return true;
    } else {
        size_t dir_end = path_start;
        size_t i;
        for (i = path_start; i < path_end; i++) {
            if (base[i] == '/') dir_end = i + 1;
        }
        if (dir_end == path_start) {
            if (!put_text(candidate, &used, sizeof(candidate), base, path_start)) return false;
            if (!put_text(candidate, &used, sizeof(candidate), "/", 1)) return false;
        } else {
            if (!put_text(candidate, &used, sizeof(candidate), base, dir_end)) return false;
        }
        if (!put_text(candidate, &used, sizeof(candidate), ref, strlen(ref))) return false;
    }
    return normalize_absolute(candidate, out, cap);
}

/* ---------- 下载文件名清洗 / Download filename sanitizer ---------- */

typedef struct {
    char* out;
    size_t cap;
    size_t len;
    bool started;
} filename_builder_t;

static void filename_add_cp(filename_builder_t* builder, uint32_t cp) {
    char bytes[4];
    size_t n;
    if (!builder->started) {
        if (cp == ' ' || cp == '.') return; /* 去开头空白/点 / skip leading spaces & dots */
        builder->started = true;
    }
    if (cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
        cp == '|') {
        cp = '_';
    } else if (cp < 32u || cp == 0x7Fu) {
        return; /* 控制字符丢弃 / drop control characters */
    }
    n = utf8_encode(cp, bytes);
    if (builder->len + n > 100u) return; /* 100 字节基名上限 / 100-byte base cap */
    if (builder->len + n >= builder->cap) return; /* 输出容量不足则丢弃 / keep room for NUL */
    memcpy(builder->out + builder->len, bytes, n);
    builder->len += n;
}

static void filename_add_text(filename_builder_t* builder, const char* text) {
    const unsigned char* s = (const unsigned char*)text;
    size_t i = 0;
    while (s[i]) {
        uint32_t cp;
        size_t seq;
        if (s[i] < 0x80) {
            cp = s[i];
            seq = 1;
        } else if ((s[i] & 0xE0u) == 0xC0u && s[i + 1]) {
            cp = ((uint32_t)(s[i] & 0x1Fu) << 6) | (uint32_t)(s[i + 1] & 0x3Fu);
            seq = 2;
        } else if ((s[i] & 0xF0u) == 0xE0u && s[i + 1] && s[i + 2]) {
            cp = ((uint32_t)(s[i] & 0x0Fu) << 12) | ((uint32_t)(s[i + 1] & 0x3Fu) << 6) |
                 (uint32_t)(s[i + 2] & 0x3Fu);
            seq = 3;
        } else if ((s[i] & 0xF8u) == 0xF0u && s[i + 1] && s[i + 2] && s[i + 3]) {
            cp = ((uint32_t)(s[i] & 0x07u) << 18) | ((uint32_t)(s[i + 1] & 0x3Fu) << 12) |
                 ((uint32_t)(s[i + 2] & 0x3Fu) << 6) | (uint32_t)(s[i + 3] & 0x3Fu);
            seq = 4;
        } else {
            i++;
            continue; /* 非法 UTF-8：跳过 / invalid UTF-8 byte skipped */
        }
        filename_add_cp(builder, cp);
        i += seq;
    }
}

bool opds_sanitize_filename(const char* author, const char* title, char* out, size_t cap) {
    filename_builder_t builder;
    bool have_author;
    bool have_title;
    if (!author || !title || !out || cap < 106u) return false; /* 105 字节结果 + NUL / 105 bytes + NUL */
    have_author = author[0] != '\0';
    have_title = title[0] != '\0';

    builder.out = out;
    builder.cap = cap;
    builder.len = 0;
    builder.started = false;
    out[0] = '\0';

    if (have_author && have_title) {
        filename_add_text(&builder, author);
        filename_add_text(&builder, " - ");
        filename_add_text(&builder, title);
    } else if (have_title) {
        filename_add_text(&builder, title);
    } else if (have_author) {
        filename_add_text(&builder, author);
    }

    while (builder.len > 0 && (out[builder.len - 1] == ' ' || out[builder.len - 1] == '.')) builder.len--;
    out[builder.len] = '\0';
    if (builder.len == 0) {
        static const char fallback[] = "book";
        size_t n = strlen(fallback);
        if (cap <= n) return false;
        memcpy(out, fallback, n + 1);
        builder.len = n;
    }
    if (builder.len + 5u >= cap) return false; /* 需要给 ".epub" 留位置 / room for .epub */
    memcpy(out + builder.len, ".epub", 6);
    return true;
}
