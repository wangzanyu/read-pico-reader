/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：KOReader 进度 XPath 映射的宿主测试：与 main/book/html_text.c 的块表逐段对拍，
 *       并覆盖 CrossMux 关键夹具（嵌套元素、注释/PI/CDATA 边界、Calibre div/span、
 *       body 直接文本）、round-trip、图片占位与失败分支。
 * English: Host tests for the KOReader progress XPath mapper: verify every paragraph against
 *       main/book/html_text.c, cover the key CrossMux fixtures (nested elements, comment/PI/
 *       CDATA boundaries, Calibre div/span text, body-direct text), round-trips, image
 *       placeholders and failure paths.
 *
 * 冻结：仅宿主测试，不链接 ESP-IDF。/ Host tests only; never links ESP-IDF.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "html_text.h"
#include "kosync_xhtml.h"

static html_text_t html_parse(const char* html) {
    html_text_t text = {0};
    esp_err_t err = html_to_blocks(html, strlen(html), &text);
    if (err != ESP_OK) {
        fprintf(stderr, "html_to_blocks failed: %d len=%u html=%s\n", (int)err, (unsigned)strlen(html), html);
        abort();
    }
    return text;
}

// 对拍：段数、每段字节数、每段指纹必须与 html_text 完全一致。
// Cross-check: paragraph count, byte length and fingerprint must match html_text exactly.
static kosync_xhtml_t* parse_matching(const char* html) {
    html_text_t text = html_parse(html);
    kosync_xhtml_t* model = NULL;
    if (kosync_xhtml_parse(html, strlen(html), &model) != 0 || model == NULL) {
        fprintf(stderr, "kosync_xhtml_parse failed: %s\n", html);
        abort();
    }
    if (kosync_xhtml_block_count(model) != text.count) {
        fprintf(stderr, "block count %u != html_text %u: %s\n", (unsigned)kosync_xhtml_block_count(model),
                (unsigned)text.count, html);
        abort();
    }
    for (size_t i = 0; i < text.count; i++) {
        const blk_t* b = &text.blocks[i];
        if (kosync_xhtml_block_length(model, (uint32_t)i) != b->len ||
            kosync_xhtml_block_hash(model, (uint32_t)i) != kosync_xhtml_hash(text.utf8 + b->offset, b->len)) {
            fprintf(
                stderr, "block %u mismatch: model(len=%u) html(len=%u) block '%s'\n", (unsigned)i,
                (unsigned)kosync_xhtml_block_length(model, (uint32_t)i), (unsigned)b->len, text.utf8 + b->offset);
            abort();
        }
    }
    html_text_free(&text);
    return model;
}

static void check_xpath(kosync_xhtml_t* model, uint32_t block, uint32_t off, unsigned spine, const char* want) {
    char got[KOSYNC_XPATH_MAX];
    int rc = kosync_xhtml_xpath_for(model, block, off, spine, got, sizeof(got));
    if (rc != 0) {
        fprintf(stderr, "xpath_for(block=%u off=%u) failed, want %s\n", (unsigned)block, (unsigned)off, want);
        abort();
    }
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "xpath_for(block=%u off=%u) = %s, want %s\n", (unsigned)block, (unsigned)off, got, want);
        abort();
    }
}

static void check_xpath_fail(kosync_xhtml_t* model, uint32_t block, uint32_t off) {
    char got[KOSYNC_XPATH_MAX];
    if (kosync_xhtml_xpath_for(model, block, off, 0, got, sizeof(got)) == 0) {
        fprintf(stderr, "xpath_for(block=%u off=%u) should fail, got %s\n", (unsigned)block, (unsigned)off, got);
        abort();
    }
}

static void check_resolve(kosync_xhtml_t* model, const char* xpath, unsigned spine, uint32_t want_block,
                          uint32_t want_off) {
    uint32_t block = 0, off = 0;
    if (kosync_xhtml_resolve(model, xpath, spine, &block, &off) != 0) {
        fprintf(stderr, "resolve(%s) failed\n", xpath);
        abort();
    }
    if (block != want_block || off != want_off) {
        fprintf(stderr, "resolve(%s) = (%u,%u), want (%u,%u)\n", xpath, (unsigned)block, (unsigned)off,
                (unsigned)want_block, (unsigned)want_off);
        abort();
    }
}

static void check_resolve_fail(kosync_xhtml_t* model, const char* xpath, unsigned spine) {
    uint32_t block = 0, off = 0;
    if (kosync_xhtml_resolve(model, xpath, spine, &block, &off) == 0) {
        fprintf(stderr, "resolve(%s) should fail, got (%u,%u)\n", xpath, (unsigned)block, (unsigned)off);
        abort();
    }
}

static bool is_image_only_block(const kosync_xhtml_t* model, uint32_t block) {
    static const char placeholder[] = "\xef\xbf\xbc";
    return kosync_xhtml_block_length(model, block) == 3 &&
           kosync_xhtml_block_hash(model, block) == kosync_xhtml_hash(placeholder, 3);
}

// 往返：XPath 反解必须回到同一段，且偏移回退不超过一个码点（空白折叠/占位符）。
// Round-trip: the XPath must resolve back to the same paragraph, within one codepoint.
static void check_roundtrip(kosync_xhtml_t* model, unsigned spine) {
    const uint32_t count = kosync_xhtml_block_count(model);
    for (uint32_t block = 0; block < count; block++) {
        const uint32_t len = kosync_xhtml_block_length(model, block);
        if (!len || is_image_only_block(model, block)) continue;
        for (uint32_t off = 0; off < len; off++) {
            char xpath[KOSYNC_XPATH_MAX];
            if (kosync_xhtml_xpath_for(model, block, off, spine, xpath, sizeof(xpath)) != 0) {
                fprintf(stderr, "roundtrip: xpath_for(%u,%u) failed\n", (unsigned)block, (unsigned)off);
                abort();
            }
            uint32_t back_block = 0, back_off = 0;
            if (kosync_xhtml_resolve(model, xpath, spine, &back_block, &back_off) != 0) {
                fprintf(stderr, "roundtrip: resolve(%s) failed\n", xpath);
                abort();
            }
            if (back_block != block || (back_off > off ? back_off - off : off - back_off) > 4) {
                fprintf(stderr, "roundtrip: (%u,%u) -> %s -> (%u,%u)\n", (unsigned)block, (unsigned)off, xpath,
                        (unsigned)back_block, (unsigned)back_off);
                abort();
            }
        }
    }
}

// CrossMux kNestedFixture：嵌套元素、inline 文本节点与 text()[2]。
// CrossMux kNestedFixture: nested elements, inline text nodes and text()[2].
static void test_nested(void) {
    const char* html =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><div><section><p>Alpha bravo</p>"
        "<p>Second <em>nested</em> tail</p></section></div></body></html>";
    kosync_xhtml_t* m = parse_matching(html);
    assert(kosync_xhtml_block_count(m) == 2);
    check_xpath(m, 0, 0, 0, "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].0");
    check_xpath(m, 0, 6, 0, "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].6");
    check_xpath(m, 1, 0, 3, "/body/DocFragment[4]/body/div[1]/section[1]/p[2]/text()[1].0");
    check_xpath(m, 1, 7, 0, "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/em[1]/text()[1].0");
    check_xpath(m, 1, 13, 0, "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[2].1");
    check_resolve(m, "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/em[1]/text()[1].2", 0, 1, 9);
    check_resolve(m, "/body/DocFragment[1]/body/div[1]/section[1]/p[1]/text()[1].6", 0, 0, 6);
    check_resolve(m, "/body/DocFragment[2]/body/div[1]/section[1]/p[2]/text()[2].0", 1, 1, 13);
    check_resolve(m, "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/text()[2].1", 0, 1, 14);
    // 仅元素路径落在元素的段首（块级元素）或当前位置（行内元素）。
    // Element-only paths anchor the element start (block tag) or the current offset (inline).
    check_resolve(m, "/body/DocFragment[1]/body/div[1]/section[1]/p[2]", 0, 1, 0);
    check_resolve(m, "/body/DocFragment[1]/body/div[1]/section[1]/p[2]/em[1]", 0, 1, 7);
    // 整章起点路径（CrossMux isChapterStartXPath 的两种形态）。
    // Chapter-start forms accepted by CrossMux isChapterStartXPath.
    check_resolve(m, "/body/DocFragment[1]/body", 0, 0, 0);
    check_resolve(m, "/body/DocFragment[1]/body.0", 0, 0, 0);
    check_resolve_fail(m, "/body/DocFragment[1]/body.5", 0);
    check_roundtrip(m, 0);
    kosync_xhtml_free(m);
}

// CrossMux kKindleDivSpanFixture：Calibre 转换书没有 <p>，正文在 div/span 里。
// CrossMux kKindleDivSpanFixture: Calibre output with body text in div/span and no <p>.
static void test_kindle_div_span(void) {
    const char* html =
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body class=\"calibre\">\n"
        "<p id=\"filepos1\" class=\"calibre1\"></p>\n"
        "<div class=\"calibre8\"><span class=\"calibre9\">3</span></div><div class=\"calibre10\"> </div>\n"
        "<div class=\"calibre12\"><span class=\"calibre6\"><span class=\"bold\">She drove</span> back to "
        "D.C.</span></div>\n"
        "<div class=\"calibre19\"><span class=\"calibre6\">\xE2\x80\x9CWho?\xE2\x80\x9D</span></div>\n"
        "</body></html>";
    kosync_xhtml_t* m = parse_matching(html);
    // 空 <p>、只有空白的 div 不产段；正文 3 段。
    // The empty <p> and the whitespace-only div produce no paragraph; three real paragraphs.
    assert(kosync_xhtml_block_count(m) == 3);
    assert(kosync_xhtml_block_length(m, 0) == 1);
    check_xpath(m, 0, 0, 0, "/body/DocFragment[1]/body/div[1]/span[1]/text()[1].0");
    check_xpath(m, 1, 0, 0, "/body/DocFragment[1]/body/div[3]/span[1]/span[1]/text()[1].0");
    check_xpath(m, 1, 10, 0, "/body/DocFragment[1]/body/div[3]/span[1]/text()[1].1");
    check_xpath(m, 2, 0, 0, "/body/DocFragment[1]/body/div[4]/span[1]/text()[1].0");
    check_resolve(m, "/body/DocFragment[1]/body/div[3]/span[1]/text()[1].1", 0, 1, 10);
    check_resolve(m, "/body/DocFragment[1]/body/div[1]/span[1]", 0, 0, 0);
    check_roundtrip(m, 0);
    kosync_xhtml_free(m);
}

// CrossMux 注释夹具：块文本合并（html_text 不加分隔），但 text() 序号 +1。
// CrossMux comment fixture: block text merges (html_text adds no separator) while the
// text() index advances by one.
static void test_comment_boundary(void) {
    const char* html = "<html><body><p>before<!--comment-->after</p></body></html>";
    kosync_xhtml_t* m = parse_matching(html);
    assert(kosync_xhtml_block_count(m) == 1);
    assert(kosync_xhtml_block_length(m, 0) == 11);
    check_xpath(m, 0, 6, 0, "/body/DocFragment[1]/body/p[1]/text()[2].0");
    check_resolve(m, "/body/DocFragment[1]/body/p[1]/text()[2].1", 0, 0, 7);
    check_resolve_fail(m, "/body/DocFragment[1]/body/p[1]/text()[1].6", 0);
    check_roundtrip(m, 0);
    kosync_xhtml_free(m);

    // 首个边界之前不凭空产生空文本节点。
    // No empty text node appears before the first boundary.
    kosync_xhtml_t* lead = parse_matching("<html><body><p><!--comment-->text</p></body></html>");
    check_xpath(lead, 0, 0, 0, "/body/DocFragment[1]/body/p[1]/text()[1].0");
    kosync_xhtml_free(lead);
}

// CrossMux 处理指令夹具：与注释同样切分文本节点。
// CrossMux processing-instruction fixture: splits text nodes like a comment.
static void test_pi_boundary(void) {
    const char* html = "<html><body><p>before<?marker?>after</p></body></html>";
    kosync_xhtml_t* m = parse_matching(html);
    assert(kosync_xhtml_block_count(m) == 1);
    check_xpath(m, 0, 6, 0, "/body/DocFragment[1]/body/p[1]/text()[2].0");
    check_roundtrip(m, 0);
    kosync_xhtml_free(m);

    kosync_xhtml_t* lead = parse_matching("<html><body><p><?marker?>text</p></body></html>");
    check_xpath(lead, 0, 0, 0, "/body/DocFragment[1]/body/p[1]/text()[1].0");
    kosync_xhtml_free(lead);
}

// CDATA：html_text 把 CDATA 当声明整体跳过（内容不进块），本模型必须一致；
// 边界仍切分文本节点。CrossMux 把 CDATA 内容算作可见文本，这里以 html_text 为准。
// CDATA: html_text skips the whole section as a declaration (its text never reaches a block),
// so the model must agree; the boundary still splits text nodes. CrossMux counts CDATA text
// as visible content, but html_text is the authority here.
static void test_cdata_boundary(void) {
    const char* html = "<html><body><p>before<![CDATA[middle]]>after</p></body></html>";
    kosync_xhtml_t* m = parse_matching(html);
    assert(kosync_xhtml_block_count(m) == 1);
    assert(kosync_xhtml_block_length(m, 0) == 11); // "beforeafter" / the CDATA text is skipped
    check_xpath(m, 0, 6, 0, "/body/DocFragment[1]/body/p[1]/text()[2].0");
    check_roundtrip(m, 0);
    kosync_xhtml_free(m);

    // 纯 CDATA 内容：html_text 不产生任何文本，因此没有段落可锚定（CrossMux 会数 CDATA 文本，
    // 这里以 html_text 为准，整章无法映射）。
    // Pure CDATA content yields no text in html_text, so there is no paragraph to anchor
    // (CrossMux counts CDATA text; html_text is the authority here, so the chapter is unmappable).
    {
        const char* pure = "<html><body><p><![CDATA[text]]></p></body></html>";
        kosync_xhtml_t* lead = NULL;
        assert(kosync_xhtml_parse(pure, strlen(pure), &lead) != 0 && lead == NULL);
    }
}

// CrossMux ResolvesTextDirectlyInsideBody：body 直接文本（无 <p>）。
// CrossMux ResolvesTextDirectlyInsideBody: text directly inside <body> without <p>.
static void test_body_direct_text(void) {
    kosync_xhtml_t* bare = parse_matching("<html><body>Hello world</body></html>");
    assert(kosync_xhtml_block_count(bare) == 1);
    check_xpath(bare, 0, 6, 0, "/body/DocFragment[1]/body/text()[1].6");
    check_resolve(bare, "/body/DocFragment[1]/body/text()[1].6", 0, 0, 6);
    check_roundtrip(bare, 0);
    kosync_xhtml_free(bare);

    kosync_xhtml_t* after = parse_matching("<html><body><p>a</p>\nHello</body></html>");
    assert(kosync_xhtml_block_count(after) == 2);
    check_xpath(after, 1, 0, 0, "/body/DocFragment[1]/body/text()[1].1");
    check_resolve(after, "/body/DocFragment[1]/body/text()[1].1", 0, 1, 0);
    check_roundtrip(after, 0);
    kosync_xhtml_free(after);
}

// 空白、实体（&nbsp; 折叠成空格）与 UTF-8 码点偏移。
// Whitespace, entities (&nbsp; collapses to a space) and UTF-8 codepoint offsets.
static void test_whitespace_entities(void) {
    const char* html = "<html><body>  <p> a  b&nbsp;c &amp; d </p>\n <div>x</div> </body></html>";
    kosync_xhtml_t* m = parse_matching(html);
    assert(kosync_xhtml_block_count(m) == 2);
    check_xpath(m, 0, 0, 0, "/body/DocFragment[1]/body/p[1]/text()[1].1");
    check_xpath(m, 1, 0, 0, "/body/DocFragment[1]/body/div[1]/text()[1].0");
    check_resolve(m, "/body/DocFragment[1]/body/p[1]/text()[1].5", 0, 0, 3);
    check_roundtrip(m, 0);
    kosync_xhtml_free(m);
}

static void test_cjk(void) {
    const char* html = "<html><body><p>A\u00e9\u4e2dB</p></body></html>";
    kosync_xhtml_t* m = parse_matching(html);
    assert(kosync_xhtml_block_count(m) == 1);
    // 段内字节偏移 1 / 3 / 6 都是字符起点；码点偏移依次为 1 / 2 / 3。
    // Block byte offsets 1/3/6 are character starts; codepoint offsets are 1/2/3.
    check_xpath(m, 0, 1, 0, "/body/DocFragment[1]/body/p[1]/text()[1].1");
    check_xpath(m, 0, 3, 0, "/body/DocFragment[1]/body/p[1]/text()[1].2");
    check_resolve(m, "/body/DocFragment[1]/body/p[1]/text()[1].3", 0, 0, 6);
    check_roundtrip(m, 0);
    kosync_xhtml_free(m);
}

// 块级标签与 <br>：br 切段，同时推进文本节点。
// Block tags and <br>: br splits paragraphs and advances the text node.
static void test_blocks_and_breaks(void) {
    const char* html =
        "<html><body><h1>T</h1><p>a<br/>b</p><ul><li>one</li><li>two</li></ul>"
        "<blockquote>q</blockquote></body></html>";
    kosync_xhtml_t* m = parse_matching(html);
    assert(kosync_xhtml_block_count(m) == 6);
    check_xpath(m, 0, 0, 0, "/body/DocFragment[1]/body/h1[1]/text()[1].0");
    check_xpath(m, 2, 0, 0, "/body/DocFragment[1]/body/p[1]/text()[2].0");
    check_xpath(m, 4, 0, 0, "/body/DocFragment[1]/body/ul[1]/li[2]/text()[1].0");
    check_resolve(m, "/body/DocFragment[1]/body/ul[1]/li[1]/text()[1].0", 0, 3, 0);
    check_resolve(m, "/body/DocFragment[1]/body/p[1]/text()[2].0", 0, 2, 0);
    check_roundtrip(m, 0);
    kosync_xhtml_free(m);
}

// 图片：块级图片占一段（U+FFFC），同章脚注注号里的图片是行内占位符。
// Images: a block image owns a paragraph (U+FFFC); an image in a same-chapter footnote
// anchor is an inline placeholder inside the running paragraph.
static void test_images(void) {
    kosync_xhtml_t* block = parse_matching("<html><body><p>a<img src=\"x.png\"/>b</p></body></html>");
    assert(kosync_xhtml_block_count(block) == 3);
    assert(is_image_only_block(block, 1));
    check_xpath(block, 0, 0, 0, "/body/DocFragment[1]/body/p[1]/text()[1].0");
    check_xpath(block, 2, 0, 0, "/body/DocFragment[1]/body/p[1]/text()[2].0");
    // 图片段没有文字可锚定：退到下一段段首。
    // An image-only paragraph has no anchorable text: fall back to the next paragraph start.
    check_xpath(block, 1, 0, 0, "/body/DocFragment[1]/body/p[1]/text()[2].0");
    check_roundtrip(block, 0);
    kosync_xhtml_free(block);

    // 没有可用图片属性的 <img> 在 html_text 里不占段；<img src=""> 同样不占段。
    // An <img> without a recognizable image attribute owns no paragraph in html_text, and
    // neither does <img src="">.
    kosync_xhtml_t* skipped =
        parse_matching("<html><body><p>a<img alt=\"x\"/><img src=\"\"/>b</p></body></html>");
    assert(kosync_xhtml_block_count(skipped) == 1);
    kosync_xhtml_free(skipped);

    // data: URI 的图片：html_text 的 data: 过滤不会 break，扫描结束后仍沿用其 value/length，
    // 因此实际仍会生成占位段（历史行为）。本模型必须保持一致。
    // A data: URI image: html_text's data: filter never breaks and its value/length survive
    // the loop, so a placeholder paragraph is still produced (historic behaviour). The model
    // must agree.
    kosync_xhtml_t* data_uri =
        parse_matching("<html><body><p>a<img src=\"data:image/png;base64,AA\"/>b</p></body></html>");
    assert(kosync_xhtml_block_count(data_uri) == 3 && is_image_only_block(data_uri, 1));
    kosync_xhtml_free(data_uri);

    // 同章脚注注号图片：行内占位，区块仍是 "word\uFFFCtail"。
    // Same-chapter note marker image: inline placeholder; the paragraph stays a single block.
    kosync_xhtml_t* note = parse_matching(
        "<html><body><p>word<a href=\"#n1\"><img src=\"m.png\"/></a>tail</p>"
        "<aside id=\"n1\">note</aside></body></html>");
    assert(kosync_xhtml_block_count(note) == 2);
    assert(kosync_xhtml_block_length(note, 0) == 4 + 3 + 4); // word + U+FFFC + tail
    // 占位符不占 XPath 文字码点，尾部文字落在 text()[2]。
    // Placeholders never occupy XPath text codepoints; the trailing text is text()[2].
    check_xpath(note, 0, 7, 0, "/body/DocFragment[1]/body/p[1]/text()[2].0");
    check_roundtrip(note, 0);
    kosync_xhtml_free(note);
}

// 不合法/边界输入：解析失败、越界、坏 XPath。
// Invalid and boundary inputs: parse failures, out-of-range offsets and bad XPaths.
static void test_failures(void) {
    kosync_xhtml_t* model = NULL;
    assert(kosync_xhtml_parse("", 0, &model) != 0 && model == NULL);
    assert(kosync_xhtml_parse(NULL, 3, &model) != 0 && model == NULL);
    // 没有 <body>：无法给出 body 相对的 XPath。
    // No <body>: a body-relative XPath cannot be produced.
    {
        const char* no_body = "<html><p>x</p></html>";
        assert(kosync_xhtml_parse(no_body, strlen(no_body), &model) != 0 && model == NULL);
    }
    // 只有空白：没有段落。
    // Whitespace only: no paragraphs.
    {
        const char* blank = "<html><body> \n </body></html>";
        assert(kosync_xhtml_parse(blank, strlen(blank), &model) != 0 && model == NULL);
    }
    // 标签缺少 '>'：扫描到文件尾仍找不到结束符（未闭合的元素本身是允许的，与 html_text 一致）。
    // A tag without '>': no terminator before end of input (unclosed elements themselves are
    // tolerated, matching html_text).
    {
        const char* broken = "<html><body><p";
        assert(kosync_xhtml_parse(broken, strlen(broken), &model) != 0 && model == NULL);
    }
    // 超长标签名：无法地址化，整章拒绝而不是给出错误 XPath。
    // An over-long tag name cannot be addressed; reject the chapter instead of guessing.
    {
        const char* long_tag = "<html><body><p>x</p><verylongtagname1>y</verylongtagname1></body></html>";
        assert(kosync_xhtml_parse(long_tag, strlen(long_tag), &model) != 0 && model == NULL);
    }
    // 嵌套超过 64 层。
    // Nesting deeper than 64 levels.
    {
        char deep[1024];
        size_t used = 0;
        static const char head[] = "<html><body>";
        memcpy(deep, head, sizeof(head) - 1);
        used = sizeof(head) - 1;
        for (int i = 0; i < 70; i++) {
            if (used + 5 + 2 >= sizeof(deep)) break;
            memcpy(deep + used, "<div>", 5);
            used += 5;
        }
        deep[used++] = 'x';
        deep[used] = 0;
        assert(kosync_xhtml_parse(deep, used, &model) != 0 && model == NULL);
    }

    const char* html = "<html><body><p>ok</p><p>two</p></body></html>";
    assert(kosync_xhtml_parse(html, strlen(html), &model) == 0);
    char out[KOSYNC_XPATH_MAX];
    assert(kosync_xpath_chapter_only(2, out, sizeof(out)) == 0);
    assert(strcmp(out, "/body/DocFragment[3]/body") == 0);
    assert(kosync_xpath_chapter_only(2, out, 8) != 0);
    assert(kosync_xpath_chapter_only(0, NULL, 8) != 0);

    // xpath_for 失败分支：越界段、越界偏移、缓冲太小、空参数。
    // xpath_for failures: bad paragraph, bad offset, small buffer, null arguments.
    check_xpath_fail(model, 2, 0);
    check_xpath_fail(model, 0, kosync_xhtml_block_length(model, 0));
    {
        char tiny[8];
        assert(kosync_xhtml_xpath_for(model, 0, 0, 0, tiny, sizeof(tiny)) != 0);
        assert(kosync_xhtml_xpath_for(NULL, 0, 0, 0, tiny, sizeof(tiny)) != 0);
        assert(kosync_xhtml_xpath_for(model, 0, 0, 0, NULL, sizeof(tiny)) != 0);
    }

    // resolve 失败分支：前缀、编号、元素、文本序号、尾部垃圾、越界偏移。
    // resolve failures: prefix, fragment number, element, text index, trailing garbage, offset.
    uint32_t block = 0, off = 0;
    assert(kosync_xhtml_resolve(model, "/body/DocFragment[1]/body", 0, &block, &off) == 0 && block == 0 && off == 0);
    assert(kosync_xhtml_resolve(model, "/body/DocFragment[2]/body", 0, &block, &off) != 0);
    check_resolve_fail(model, "/body/DocFragment[1]/body/p[9]", 0);
    check_resolve_fail(model, "/body/DocFragment[1]/body/p[1]/text()[2]", 0);
    check_resolve_fail(model, "/body/DocFragment[1]/body/p[1]/text()[1].9", 0);
    check_resolve_fail(model, "/body/DocFragment[1]/body/p[1].2", 0);
    check_resolve_fail(model, "/body/DocFragment[1]/body/text()[1].0", 0);
    check_resolve_fail(model, "/body/DocFragment[1]/body/p[1]/text()[0]", 0);
    check_resolve_fail(model, "body/DocFragment[1]/body", 0);
    check_resolve_fail(model, "/body/DocFragment[1]/body/p[1]/text()[1].0x", 0);
    assert(kosync_xhtml_resolve(NULL, "/body/DocFragment[1]/body", 0, &block, &off) != 0);
    assert(kosync_xhtml_resolve(model, NULL, 0, &block, &off) != 0);
    assert(kosync_xhtml_resolve(model, "/body/DocFragment[1]/body", 0, NULL, &off) != 0);

    // 重复查询后块表仍然稳定（每次查询都会重建）。
    // The paragraph table stays stable across repeated queries (each query rebuilds it).
    const uint32_t count = kosync_xhtml_block_count(model);
    const uint64_t first = kosync_xhtml_block_hash(model, 0);
    check_xpath(model, 0, 0, 0, "/body/DocFragment[1]/body/p[1]/text()[1].0");
    assert(kosync_xhtml_block_count(model) == count && kosync_xhtml_block_hash(model, 0) == first);
    kosync_xhtml_free(model);
    kosync_xhtml_free(NULL);
}

int main(void) {
    fprintf(stderr, "nested\n");
    test_nested();
    fprintf(stderr, "kindle div/span\n");
    test_kindle_div_span();
    fprintf(stderr, "comment boundary\n");
    test_comment_boundary();
    fprintf(stderr, "pi boundary\n");
    test_pi_boundary();
    fprintf(stderr, "cdata boundary\n");
    test_cdata_boundary();
    fprintf(stderr, "body direct text\n");
    test_body_direct_text();
    fprintf(stderr, "whitespace/entities\n");
    test_whitespace_entities();
    fprintf(stderr, "cjk\n");
    test_cjk();
    fprintf(stderr, "blocks\n");
    test_blocks_and_breaks();
    fprintf(stderr, "images\n");
    test_images();
    fprintf(stderr, "failures\n");
    test_failures();
    printf("kosync_xhtml: all tests passed\n");
    return 0;
}

// tools/html_text_stubs/esp_heap_caps.h 的故障注入钩子：-1 表示不注入。
// Failure-injection hook of tools/html_text_stubs/esp_heap_caps.h: -1 disables injection.
int html_test_fail_after = -1;
