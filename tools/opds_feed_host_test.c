/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS feed 解析器/URL/文件名主机回归测试。纯 C，无 ESP-IDF 依赖；MSVC 与
 *       gcc(-fsanitize) 均可编译。
 * English: Host regressions for the OPDS feed parser, URL resolution and filename
 *       sanitizing. Plain C, no ESP-IDF dependency; builds with MSVC and gcc sanitizers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "opds_feed.h"

static int g_failures;

static void check_at(const char* file, int line, const char* what, int ok) {
    if (!ok) {
        fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
        g_failures++;
    }
}

static void check_str_at(const char* file, int line, const char* what, const char* got, const char* want) {
    if (got == NULL || want == NULL || strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s:%d: %s\n  got : \"%s\"\n  want: \"%s\"\n", file, line, what,
                got ? got : "(null)", want ? want : "(null)");
        g_failures++;
    }
}

static void check_u32_at(const char* file, int line, const char* what, unsigned long got, unsigned long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s:%d: %s got=%lu want=%lu\n", file, line, what, got, want);
        g_failures++;
    }
}

#define CHECK(cond) check_at(__FILE__, __LINE__, #cond, (cond) != 0)
#define CHECK_STR(what, got, want) check_str_at(__FILE__, __LINE__, what, got, want)
#define CHECK_U32(what, got, want) check_u32_at(__FILE__, __LINE__, what, (unsigned long)(got), (unsigned long)(want))

/* 解析结果快照 / Parse outcome snapshot. */
typedef struct {
    opds_feed_info_t info;
    opds_entry_t entries[8];
    uint16_t count;
    bool fed;
    bool finished;
} parse_result_t;

static void parse_feed(const char* base, const char* xml, size_t chunk, parse_result_t* out) {
    opds_feed_parser_t* parser;
    size_t len;
    size_t pos = 0;
    uint16_t i;

    memset(out, 0, sizeof(*out));
    parser = opds_feed_parser_create(base, NULL);
    CHECK(parser != NULL);
    if (parser == NULL) return;
    len = strlen(xml);
    out->fed = true;
    while (pos < len) {
        size_t n = chunk != 0 ? chunk : (len - pos);
        if (n > len - pos) n = len - pos;
        if (!opds_feed_parser_feed(parser, xml + pos, n)) {
            out->fed = false;
            break;
        }
        pos += n;
    }
    out->finished = opds_feed_parser_finish(parser);
    out->count = opds_feed_parser_entry_count(parser);
    for (i = 0; i < out->count && i < 8; i++) {
        if (!opds_feed_parser_entry(parser, i, &out->entries[i])) CHECK(0);
    }
    opds_feed_parser_info(parser, &out->info);
    opds_feed_parser_destroy(parser);
}

/* 逐字节/分块一致性 / Compare a chunked parse against the whole-buffer parse. */
static void check_chunk_equivalence(const char* xml, size_t chunk) {
    parse_result_t whole;
    parse_result_t chunked;
    parse_feed("https://example.com/opds/", xml, 0, &whole);
    parse_feed("https://example.com/opds/", xml, chunk, &chunked);
    CHECK(whole.fed && whole.finished);
    CHECK(chunked.fed && chunked.finished);
    CHECK_U32("chunked count", chunked.count, whole.count);
    CHECK_U32("chunked total", chunked.info.total_entries, whole.info.total_entries);
    CHECK_STR("chunked feed title", chunked.info.feed_title, whole.info.feed_title);
    CHECK_STR("chunked next", chunked.info.next, whole.info.next);
    CHECK_STR("chunked previous", chunked.info.previous, whole.info.previous);
    if (whole.count > 0) {
        CHECK_STR("chunked entry0 title", chunked.entries[0].title, whole.entries[0].title);
        CHECK_STR("chunked entry0 href", chunked.entries[0].href, whole.entries[0].href);
        CHECK_U32("chunked entry0 kind", chunked.entries[0].kind, whole.entries[0].kind);
    }
    if (whole.count > 2) {
        CHECK_STR("chunked entry2 author", chunked.entries[2].author, whole.entries[2].author);
        CHECK_STR("chunked entry2 type", chunked.entries[2].type, whole.entries[2].type);
    }
}

static const char* const FEED_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE feed [ <!ENTITY x \"ignored > value\"> ]>\n"
    "<!-- feed comment with > and -- > -->\n"
    "<?processing instruction?>\n"
    "<feed xmlns=\"http://www.w3.org/2005/Atom\" xmlns:opds=\"http://opds-spec.org/2010/catalog\">\n"
    "  <title>My &amp; Catalog</title>\n"
    "  <id>urn:feed</id>\n"
    "  <link rel=\"self\" href=\"https://example.com/opds/\""
    " type=\"application/atom+xml;profile=opds-catalog;kind=navigation\"/>\n"
    "  <link rel='next' href='?page=2'/>\n"
    "  <link rel=\"previous\" href=\"/opds?page=0\"/>\n"
    "  <link rel=\"first\" href=\"https://example.com/opds/first\"/>\n"
    "  <link rel=\"last\" href=\"last\"/>\n"
    "  <entry>\n"
    "    <title>Navigation &amp; Section</title>\n"
    "    <id>urn:nav</id>\n"
    "    <link rel=\"subsection\""
    " type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\" href=\"books/\"/>\n"
    "  </entry>\n"
    "  <entry>\n"
    "    <title>Book &lt;One&gt;</title>\n"
    "    <author><name>  Jane   Doe </name></author>\n"
    "    <author><name>Second Author</name></author>\n"
    "    <id>urn:book1</id>\n"
    "    <link rel=\"http://opds-spec.org/image\" type=\"image/jpeg\" href=\"covers/1.jpg\"/>\n"
    "    <link rel='http://opds-spec.org/acquisition' type='application/epub+zip' length='12345'"
    " href='files/one.epub?x=1&amp;y=2'/>\n"
    "  </entry>\n"
    "  <entry>\n"
    "    <title>open access wins</title>\n"
    "    <link rel=\"http://opds-spec.org/acquisition/buy\" type=\"application/epub+zip\" href=\"buy.epub\"/>\n"
    "    <link rel=\"http://opds-spec.org/acquisition/borrow\" type=\"application/epub+zip\""
    " href=\"borrow.epub\"/>\n"
    "    <link rel=\"http://opds-spec.org/acquisition/open-access\" type=\"application/epub+zip\""
    " href=\"open.epub\"/>\n"
    "  </entry>\n"
    "  <entry>\n"
    "    <title>non epub acquisition</title>\n"
    "    <link rel=\"http://opds-spec.org/acquisition\" type=\"application/pdf\" href=\"x.pdf\"/>\n"
    "    <link rel=\"subsection\" type=\"application/atom+xml;profile=opds-catalog\" href=\"sub/\"/>\n"
    "  </entry>\n"
    "  <entry><title>no links</title><id>urn:other</id></entry>\n"
    "  <entry><title><![CDATA[CDATA <raw> & text]]></title>"
    "<link rel=\"subsection\" href=\"cdata/\"/></entry>\n"
    "  <entry><title>encoded &#233; &#x4E2D; &nbsp; end</title>"
    "<link rel=\"subsection\" href=\"ent/\"/></entry>\n"
    "</feed>\n";

static void test_full_feed(void) {
    parse_result_t result;
    const opds_entry_t* entry;
    parse_feed("https://example.com/opds/", FEED_XML, 0, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_U32("count", result.count, 6);
    CHECK_U32("total", result.info.total_entries, 7);
    CHECK(!result.info.truncated);
    CHECK_STR("feed title", result.info.feed_title, "My & Catalog");
    CHECK_STR("self", result.info.self, "https://example.com/opds/");
    CHECK_STR("next", result.info.next, "https://example.com/opds/?page=2");
    CHECK_STR("previous", result.info.previous, "https://example.com/opds?page=0");
    CHECK_STR("first", result.info.first, "https://example.com/opds/first");
    CHECK_STR("last", result.info.last, "https://example.com/opds/last");

    entry = &result.entries[0];
    CHECK_U32("nav kind", entry->kind, OPDS_ENTRY_NAVIGATION);
    CHECK_STR("nav title", entry->title, "Navigation & Section");
    CHECK_STR("nav rel", entry->rel, "subsection");
    CHECK_STR("nav href", entry->href, "https://example.com/opds/books/");

    entry = &result.entries[1];
    CHECK_U32("book kind", entry->kind, OPDS_ENTRY_BOOK);
    CHECK_STR("book title", entry->title, "Book <One>");
    CHECK_STR("book author first only", entry->author, "Jane Doe");
    CHECK_STR("book id", entry->id, "urn:book1");
    CHECK_STR("book href", entry->href, "https://example.com/opds/files/one.epub?x=1&y=2");
    CHECK_STR("book type", entry->type, "application/epub+zip");
    CHECK_STR("book rel", entry->rel, "http://opds-spec.org/acquisition");
    CHECK_U32("book length", entry->length, 12345u);

    entry = &result.entries[2];
    CHECK_U32("open access kind", entry->kind, OPDS_ENTRY_BOOK);
    CHECK_STR("missing author is empty", entry->author, "");
    CHECK_STR("open access href", entry->href, "https://example.com/opds/open.epub");
    CHECK_STR("open access rel", entry->rel, "http://opds-spec.org/acquisition/open-access");

    entry = &result.entries[3];
    CHECK_U32("pdf+nav kind", entry->kind, OPDS_ENTRY_NAVIGATION);
    CHECK_STR("pdf+nav href", entry->href, "https://example.com/opds/sub/");

    entry = &result.entries[4];
    CHECK_U32("cdata kind", entry->kind, OPDS_ENTRY_NAVIGATION);
    CHECK_STR("cdata title", entry->title, "CDATA <raw> & text");
    CHECK_STR("cdata href", entry->href, "https://example.com/opds/cdata/");

    entry = &result.entries[5];
    CHECK_U32("entity kind", entry->kind, OPDS_ENTRY_NAVIGATION);
    CHECK_STR("entity title", entry->title, "encoded \xC3\xA9 \xE4\xB8\xAD &nbsp; end");
}

static void test_chunk_boundaries(void) {
    static const size_t chunks[] = {1, 2, 3, 5, 7, 13, 64};
    size_t i;
    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        check_chunk_equivalence(FEED_XML, chunks[i]);
    }
    check_chunk_equivalence(
        "\xEF\xBB\xBF \n\t<feed><title>BOM</title><entry><title>A&amp;B</title>"
        "<link rel=\"subsection\" href=\"n\"/></entry></feed>",
        1);
}

static void test_entities_cdata_namespaces(void) {
    static const char* const xml =
        "<ATOM:feed xmlns:ATOM=\"http://www.w3.org/2005/Atom\">"
        "<ATOM:title>T&amp;T</ATOM:title>"
        "<ATOM:entry><ATOM:title><![CDATA[<b>raw</b> & x]]></ATOM:title>"
        "<ATOM:link rel=\"subsection\" href=\"n/\""
        " type=\"application/atom+xml;profile=opds-catalog;kind=navigation\"/>"
        "</ATOM:entry>"
        "<ATOM:entry><ATOM:title>A&nbsp;B</ATOM:title><ATOM:link rel='subsection' href='x'/></ATOM:entry>"
        "</ATOM:feed>";
    parse_result_t result;
    parse_feed("https://h/base/", xml, 3, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_U32("entities count", result.count, 2);
    CHECK_STR("entities feed title", result.info.feed_title, "T&T");
    CHECK_STR("cdata title", result.entries[0].title, "<b>raw</b> & x");
    CHECK_STR("cdata href", result.entries[0].href, "https://h/base/n/");
    CHECK_STR("unknown entity literal", result.entries[1].title, "A&nbsp;B");
    CHECK_STR("single quoted href", result.entries[1].href, "https://h/base/x");
}

static void test_link_priority(void) {
    static const char* const xml =
        "<feed><title>Priorities</title>"
        "<entry><title>E1</title>"
        "<link rel=\"http://opds-spec.org/acquisition/buy\" type=\"application/epub+zip\" href=\"buy\"/>"
        "<link rel=\"http://opds-spec.org/acquisition/borrow\" type=\"application/epub+zip\" href=\"borrow\"/>"
        "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" href=\"generic\"/>"
        "<link rel=\"http://opds-spec.org/acquisition/open-access\" type=\"application/epub+zip\""
        " href=\"open\"/></entry>"
        "<entry><title>E2</title>"
        "<link rel=\"http://opds-spec.org/acquisition/borrow\" type=\"application/epub+zip\" href=\"borrow\"/>"
        "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip\" href=\"generic\"/></entry>"
        "<entry><title>E3</title>"
        "<link rel=\"http://opds-spec.org/acquisition/buy\" type=\"application/epub+zip\" href=\"buy\"/>"
        "<link rel=\"http://opds-spec.org/acquisition/borrow\" type=\"application/epub+zip\""
        " href=\"borrow\"/></entry>"
        "<entry><title>E4</title>"
        "<link rel=\"http://opds-spec.org/acquisition/sample\" type=\"application/epub+zip\" href=\"sample\"/></entry>"
        "<entry><title>E5</title>"
        "<link rel=\"HTTP://OPDS-SPEC.ORG/ACQUISITION\" type=\"APPLICATION/EPUB+ZIP\" href=\"upper\"/></entry>"
        "<entry><title>E6</title>"
        "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/epub+zip; charset=binary\""
        " href=\"params\"/></entry>"
        "<entry><title>E7</title>"
        "<link rel=\"http://opds-spec.org/acquisition\" type=\"application/pdf\" href=\"doc.pdf\"/>"
        "<link rel=\"alternate\" type=\"application/atom+xml\" href=\"alt/\"/></entry>"
        "</feed>";
    parse_result_t result;
    parse_feed("https://lib.test/opds/", xml, 2, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_U32("priority count", result.count, 6);
    CHECK_U32("priority total", result.info.total_entries, 7);
    CHECK_STR("E1 open access wins", result.entries[0].href, "https://lib.test/opds/open");
    CHECK_STR("E1 rel", result.entries[0].rel, "http://opds-spec.org/acquisition/open-access");
    CHECK_STR("E2 generic beats borrow", result.entries[1].href, "https://lib.test/opds/generic");
    CHECK_STR("E3 borrow beats buy", result.entries[2].href, "https://lib.test/opds/borrow");
    CHECK_STR("E5 uppercase accepted", result.entries[3].href, "https://lib.test/opds/upper");
    CHECK_STR("E5 rel preserved", result.entries[3].rel, "HTTP://OPDS-SPEC.ORG/ACQUISITION");
    CHECK_STR("E6 parameters accepted", result.entries[4].href, "https://lib.test/opds/params");
    CHECK_U32("E7 navigation fallback", result.entries[5].kind, OPDS_ENTRY_NAVIGATION);
    CHECK_STR("E7 alternate href", result.entries[5].href, "https://lib.test/opds/alt/");
}

static void test_navigation_types(void) {
    static const char* const xml =
        "<feed><title>Nav</title>"
        "<entry><title>N1</title><link rel=\"subsection\" href=\"a\"/></entry>"
        "<entry><title>N2</title><link rel=\"http://opds-spec.org/sort/new\""
        " type=\"application/atom+xml;profile=opds-catalog;kind=acquisition\" href=\"new\"/></entry>"
        "<entry><title>N3</title><link type=\"application/atom+xml;profile=opds-catalog;kind=navigation\""
        " href=\"b\"/></entry>"
        "<entry><title>N4</title><link rel=\"alternate\" type=\"text/html\" href=\"page.html\"/></entry>"
        "<entry><title>N5</title><link rel=\"subsection\"/></entry>"
        "</feed>";
    parse_result_t result;
    parse_feed("https://h/", xml, 0, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_U32("nav count", result.count, 3);
    CHECK_STR("N1", result.entries[0].href, "https://h/a");
    CHECK_STR("N2", result.entries[1].href, "https://h/new");
    CHECK_STR("N3", result.entries[2].href, "https://h/b");
    CHECK_U32("N3 kind", result.entries[2].kind, OPDS_ENTRY_NAVIGATION);
}

static void test_attribute_quotes_and_gt(void) {
    static const char* const xml =
        "<feed>\n  <title>  A \n  B  </title>"
        "<entry><title>B</title>"
        "<link rel = 'subsection' href = 'x>y' type = \"application/atom+xml\" />"
        "</entry></feed>";
    parse_result_t result;
    parse_feed("https://h/", xml, 2, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_STR("whitespace folded", result.info.feed_title, "A B");
    CHECK_U32("gt attr count", result.count, 1);
    CHECK_STR("gt inside attribute", result.entries[0].href, "https://h/x>y");
    CHECK_STR("gt attr type", result.entries[0].type, "application/atom+xml");
}

static void test_pagination(void) {
    static const char* const xml =
        "<feed><title>Pages</title>"
        "<link rel=\"next\" href=\"?p=2\"/>"
        "<link rel=\"prev\" href=\"?p=0\"/>"
        "<link rel=\"first\" href=\"/f\"/>"
        "<link rel=\"last\" href=\"../l\"/>"
        "<link rel=\"up\" href=\"/u\"/>"
        "<link rel=\"start\" href=\"/s\"/>"
        "</feed>";
    parse_result_t result;
    parse_feed("https://h/a/feed", xml, 1, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_STR("next", result.info.next, "https://h/a/feed?p=2");
    CHECK_STR("prev", result.info.previous, "https://h/a/feed?p=0");
    CHECK_STR("first", result.info.first, "https://h/f");
    CHECK_STR("last", result.info.last, "https://h/l");
    CHECK_STR("up ignored", result.info.self, "");
}

static void test_entry_limit(void) {
    char xml[8192];
    size_t used = 0;
    unsigned i;
    opds_entry_t entries[OPDS_MAX_ENTRIES];
    opds_feed_info_t info;
    uint16_t count = 0;
    bool fed = false;
    bool finished = false;
    opds_feed_parser_t* parser;

    used += (size_t)snprintf(xml + used, sizeof(xml) - used, "<feed><title>Many</title>");
    for (i = 0; i < 70 && used < sizeof(xml); i++) {
        used += (size_t)snprintf(xml + used, sizeof(xml) - used,
                                 "<entry><title>B%u</title><link rel=\"subsection\" href=\"n\"/></entry>", i);
    }
    used += (size_t)snprintf(xml + used, sizeof(xml) - used, "</feed>");

    parser = opds_feed_parser_create("https://h/", NULL);
    CHECK(parser != NULL);
    if (parser == NULL) return;
    fed = opds_feed_parser_feed(parser, xml, used);
    finished = opds_feed_parser_finish(parser);
    count = opds_feed_parser_entry_count(parser);
    opds_feed_parser_info(parser, &info);
    CHECK(fed);
    CHECK(finished);
    CHECK_U32("retained cap", count, OPDS_MAX_ENTRIES);
    CHECK_U32("total parsed", info.total_entries, 70);
    CHECK(info.truncated);
    CHECK(!opds_feed_parser_entry(parser, OPDS_MAX_ENTRIES, &entries[0]));
    if (count > 0) {
        CHECK(opds_feed_parser_entry(parser, (uint16_t)(count - 1), &entries[0]));
        CHECK_STR("last retained title", entries[0].title, "B63");
    }
    opds_feed_parser_destroy(parser);
}

static void test_bad_entries_links_tolerated(void) {
    char xml[4096];
    size_t used = 0;
    size_t i;
    parse_result_t result;
    used += (size_t)snprintf(xml + used, sizeof(xml) - used, "<feed><title>Tolerant</title><entry><title>bad href</title>");
    used += (size_t)snprintf(xml + used, sizeof(xml) - used, "<link rel=\"subsection\" href=\"");
    for (i = 0; i < 900 && used + 2 < sizeof(xml); i++) xml[used++] = 'a';
    used += (size_t)snprintf(xml + used, sizeof(xml) - used, "\"/></entry>");
    used += (size_t)snprintf(xml + used, sizeof(xml) - used,
                             "<entry><title>nested link</title><summary><link rel=\"http://opds-spec.org/acquisition\""
                             " type=\"application/epub+zip\" href=\"nested.epub\"/></summary></entry>");
    used += (size_t)snprintf(xml + used, sizeof(xml) - used,
                             "<entry><title>good</title><link rel=\"subsection\" href=\"ok/\"/></entry></feed>");
    xml[used] = '\0';
    parse_feed("https://h/", xml, 0, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_U32("tolerant retained", result.count, 1);
    CHECK_STR("tolerant title", result.entries[0].title, "good");
    CHECK_STR("tolerant href", result.entries[0].href, "https://h/ok/");
}

static void test_bad_xml(void) {
    static const char* const bad[] = {
        "<notfeed></notfeed>",
        "<feed></feed><feed></feed>",
        "<feed><title>x</title>",
        "<feed><title>x</notfeed>",
        "<feed",
        "<feed><title>x</title></feed><!--",
        "",
    };
    size_t i;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        parse_result_t result;
        parse_feed("https://h/", bad[i], 0, &result);
        CHECK(!result.finished);
    }
    {
        parse_result_t result;
        parse_feed("https://h/", "<feed><title>ok</title></feed>", 0, &result);
        CHECK(result.fed);
        CHECK(result.finished);
    }
}

static void test_url_resolution(void) {
    static const char* const base = "https://example.com/a/b/feed.xml?x=1";
    static const struct {
        const char* ref;
        const char* want;
    } cases[] = {
        {"https://other.test/z.epub?q=1#frag", "https://other.test/z.epub?q=1"},
        {"//cdn.test/f.epub", "https://cdn.test/f.epub"},
        {"//cdn.test/x?y#z", "https://cdn.test/x?y"},
        {"/root.epub", "https://example.com/root.epub"},
        {"c.epub", "https://example.com/a/b/c.epub"},
        {"../up.epub", "https://example.com/a/up.epub"},
        {"./x/../y.epub", "https://example.com/a/b/y.epub"},
        {"?page=2", "https://example.com/a/b/feed.xml?page=2"},
        {"#frag", "https://example.com/a/b/feed.xml?x=1"},
        {"a%20b.epub", "https://example.com/a/b/a%20b.epub"},
        {"sub/", "https://example.com/a/b/sub/"},
        {"../../..", "https://example.com/"},
        {"HTTP://UP.TEST/A", "HTTP://UP.TEST/A"},
        {"https://h/a/../b?q#f", "https://h/b?q"},
        {"mailto:a@b", "mailto:a@b"},
    };
    size_t i;
    char out[OPDS_URL_MAX];
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(out, 0x55, sizeof(out));
        CHECK(opds_resolve_url(base, cases[i].ref, out, sizeof(out)));
        CHECK_STR(cases[i].ref, out, cases[i].want);
    }
    {
        CHECK(opds_resolve_url("https://example.com", "x.epub", out, sizeof(out)));
        CHECK_STR("no base path", out, "https://example.com/x.epub");
        CHECK(opds_resolve_url("https://example.com/a/b/feed.xml", "../../..", out, sizeof(out)));
        CHECK_STR("root collapse", out, "https://example.com/");
    }
    {
        char small[8];
        CHECK(!opds_resolve_url(base, "https://other.test/very/long/path.epub", small, sizeof(small)));
        CHECK(!opds_resolve_url("", "x", out, sizeof(out)));
        CHECK(!opds_resolve_url(base, "", out, sizeof(out)));
        CHECK(!opds_resolve_url("no-scheme/a", "b", out, sizeof(out)));
    }
}

static void test_filename(void) {
    char out[128];
    char out2[128];
    CHECK(opds_sanitize_filename("J. Doe", "My Book", out, sizeof(out)));
    CHECK_STR("author-title", out, "J. Doe - My Book.epub");
    CHECK(opds_sanitize_filename("", "My Book", out, sizeof(out)));
    CHECK_STR("title only", out, "My Book.epub");
    CHECK(opds_sanitize_filename("Author", "", out, sizeof(out)));
    CHECK_STR("author only", out, "Author.epub");
    CHECK(opds_sanitize_filename("A/B", "C:D*E?", out, sizeof(out)));
    CHECK_STR("illegal characters", out, "A_B - C_D_E_.epub");
    CHECK(opds_sanitize_filename("", " ..My Book. ", out, sizeof(out)));
    CHECK_STR("leading/trailing trim", out, "My Book.epub");
    CHECK(opds_sanitize_filename("", "A\tB\nC", out, sizeof(out)));
    CHECK_STR("control dropped", out, "ABC.epub");
    CHECK(opds_sanitize_filename("\xE9\xB2\x81\xE8\xBF\x85", "\xE5\x91\x90\xE5\x96\x8A", out, sizeof(out)));
    CHECK_STR("utf8 chinese", out, "\xE9\xB2\x81\xE8\xBF\x85 - \xE5\x91\x90\xE5\x96\x8A.epub");
    CHECK(opds_sanitize_filename("", "", out, sizeof(out)));
    CHECK_STR("fallback", out, "book.epub");
    CHECK(opds_sanitize_filename("J. Doe", "My Book", out, sizeof(out)));
    CHECK(opds_sanitize_filename("J. Doe", "My Book", out2, sizeof(out2)));
    CHECK_STR("deterministic", out2, out);
    {
        char tiny[105];
        CHECK(!opds_sanitize_filename("", "x", tiny, sizeof(tiny)));
    }
}

static void test_filename_limits(void) {
    char title[512];
    char out[256];
    size_t i;
    memset(title, 'a', 200);
    title[200] = '\0';
    CHECK(opds_sanitize_filename("", title, out, sizeof(out)));
    CHECK_U32("long base length", strlen(out), 105);
    CHECK(strncmp(out, title, 100) == 0);
    CHECK_STR("extension after cap", out + 100, ".epub");

    for (i = 0; i < 40; i++) {
        memcpy(title + i * 3, "\xE4\xB8\xAD", 3); /* 中 / CJK character */
    }
    title[120] = '\0';
    CHECK(opds_sanitize_filename("", title, out, sizeof(out)));
    CHECK_U32("chinese truncation", strlen(out), 104);
    CHECK_STR("chinese extension", out + 99, ".epub");

    for (i = 0; i < 51; i++) {
        memcpy(title + i * 2, "\xC3\xA9", 2); /* é / 2-byte UTF-8 */
    }
    title[102] = '\0';
    CHECK(opds_sanitize_filename("", title, out, sizeof(out)));
    CHECK_U32("2-byte truncation", strlen(out), 105);
}

typedef struct {
    unsigned allocs;
    unsigned frees;
    int fail_at;
} counting_memory_t;

static void* counting_alloc(void* ctx, size_t size) {
    counting_memory_t* memory = (counting_memory_t*)ctx;
    memory->allocs++;
    if (memory->fail_at > 0 && memory->allocs == (unsigned)memory->fail_at) return NULL;
    return malloc(size);
}

static void counting_free(void* ctx, void* pointer) {
    counting_memory_t* memory = (counting_memory_t*)ctx;
    if (pointer != NULL) {
        memory->frees++;
        free(pointer);
    }
}

static void test_memory_callbacks(void) {
    static const char* const xml =
        "<feed><title>M</title><entry><title>B</title>"
        "<link rel=\"subsection\" href=\"n\"/></entry></feed>";
    opds_feed_memory_t memory;
    counting_memory_t counter;
    opds_feed_parser_t* parser;

    memset(&counter, 0, sizeof(counter));
    counter.fail_at = 1;
    memory.alloc = counting_alloc;
    memory.free = counting_free;
    memory.ctx = &counter;
    parser = opds_feed_parser_create("https://h/", &memory);
    CHECK(parser == NULL);

    memset(&counter, 0, sizeof(counter));
    counter.fail_at = 2;
    parser = opds_feed_parser_create("https://h/", &memory);
    CHECK(parser != NULL);
    if (parser != NULL) {
        CHECK(!opds_feed_parser_feed(parser, xml, strlen(xml)));
        opds_feed_parser_destroy(parser);
    }
    CHECK_U32("failed entries allocs", counter.allocs, 2);
    CHECK_U32("failed entries frees", counter.frees, 1);

    memset(&counter, 0, sizeof(counter));
    parser = opds_feed_parser_create("https://h/", &memory);
    CHECK(parser != NULL);
    if (parser != NULL) {
        CHECK(opds_feed_parser_feed(parser, xml, strlen(xml)));
        CHECK(opds_feed_parser_finish(parser));
        CHECK_U32("entry count", opds_feed_parser_entry_count(parser), 1);
        opds_feed_parser_destroy(parser);
    }
    CHECK_U32("success allocs", counter.allocs, 2);
    CHECK_U32("success frees", counter.frees, 2);
}

static void test_progressive_and_lifecycle(void) {
    static const char* const first =
        "<feed><title>P</title><entry><title>A</title><link rel=\"subsection\" href=\"a\"/></entry>";
    static const char* const second = "<entry><title>B</title><link rel=\"subsection\" href=\"b\"/></entry></feed>";
    opds_feed_parser_t* parser;
    opds_entry_t entry;
    opds_feed_info_t info;

    CHECK(opds_feed_parser_create(NULL, NULL) == NULL);
    CHECK(opds_feed_parser_create("", NULL) == NULL);
    CHECK(opds_feed_parser_entry_count(NULL) == 0);
    CHECK(!opds_feed_parser_entry(NULL, 0, &entry));
    CHECK(!opds_feed_parser_feed(NULL, "x", 1));
    CHECK(!opds_feed_parser_finish(NULL));
    opds_feed_parser_destroy(NULL);
    opds_feed_parser_info(NULL, &info);
    CHECK_U32("null info count", info.entry_count, 0);

    parser = opds_feed_parser_create("https://h/", NULL);
    CHECK(parser != NULL);
    if (parser == NULL) return;
    CHECK(!opds_feed_parser_finish(parser)); /* 未喂数据就结束 / finish without data */
    opds_feed_parser_destroy(parser);

    parser = opds_feed_parser_create("https://h/", NULL);
    CHECK(parser != NULL);
    if (parser == NULL) return;
    CHECK(opds_feed_parser_feed(parser, first, strlen(first)));
    CHECK_U32("progressive count", opds_feed_parser_entry_count(parser), 1);
    CHECK(opds_feed_parser_feed(parser, second, strlen(second)));
    CHECK(opds_feed_parser_finish(parser));
    CHECK_U32("progressive final", opds_feed_parser_entry_count(parser), 2);
    CHECK(!opds_feed_parser_feed(parser, "", 0)); /* 已 finish / after finish */
    opds_feed_parser_destroy(parser);

    parser = opds_feed_parser_create("https://h/", NULL);
    CHECK(parser != NULL);
    if (parser != NULL) {
        CHECK(opds_feed_parser_feed(parser, NULL, 0));
        CHECK(!opds_feed_parser_finish(parser));
        opds_feed_parser_destroy(parser);
    }
}

static void test_field_truncation(void) {
    char big[301];
    char xml[2048];
    parse_result_t result;
    memset(big, 'a', 300);
    big[300] = '\0';
    snprintf(xml, sizeof(xml),
             "<feed><title>%s</title><entry><title>%s</title><author><name>%s</name></author>"
             "<id>%s</id><link rel=\"subsection\" href=\"n\"/></entry></feed>",
             big, big, big, big);
    parse_feed("https://h/", xml, 0, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_U32("feed title cap", strlen(result.info.feed_title), OPDS_FEED_TITLE_MAX - 1);
    CHECK_U32("entry title cap", strlen(result.entries[0].title), OPDS_TITLE_MAX - 1);
    CHECK_U32("author cap", strlen(result.entries[0].author), OPDS_AUTHOR_MAX - 1);
    CHECK_U32("id cap", strlen(result.entries[0].id), OPDS_ID_MAX - 1);
}

static void test_empty_feed(void) {
    parse_result_t result;
    parse_feed("https://h/", "<feed><title>Empty</title></feed>", 0, &result);
    CHECK(result.fed);
    CHECK(result.finished);
    CHECK_U32("empty count", result.count, 0);
    CHECK_U32("empty total", result.info.total_entries, 0);
    CHECK_STR("empty title", result.info.feed_title, "Empty");
}

static void test_truncation_sweep(void) {
    size_t len = strlen(FEED_XML);
    size_t n;
    for (n = 0; n <= len; n++) {
        opds_feed_parser_t* parser = opds_feed_parser_create("https://example.com/opds/", NULL);
        if (parser == NULL) {
            CHECK(0);
            return;
        }
        (void)opds_feed_parser_feed(parser, FEED_XML, n);
        (void)opds_feed_parser_finish(parser);
        opds_feed_parser_destroy(parser);
    }
    {
        char garbage[1024];
        size_t i;
        for (i = 0; i < sizeof(garbage); i++) garbage[i] = (char)((i * 37u + 11u) & 0xFFu);
        {
            opds_feed_parser_t* parser = opds_feed_parser_create("https://h/", NULL);
            if (parser == NULL) {
                CHECK(0);
                return;
            }
            (void)opds_feed_parser_feed(parser, garbage, sizeof(garbage));
            (void)opds_feed_parser_finish(parser);
            opds_feed_parser_destroy(parser);
        }
    }
}

int main(void) {
    test_full_feed();
    test_chunk_boundaries();
    test_entities_cdata_namespaces();
    test_link_priority();
    test_navigation_types();
    test_attribute_quotes_and_gt();
    test_pagination();
    test_entry_limit();
    test_bad_entries_links_tolerated();
    test_bad_xml();
    test_url_resolution();
    test_filename();
    test_filename_limits();
    test_memory_callbacks();
    test_progressive_and_lifecycle();
    test_field_truncation();
    test_empty_feed();
    test_truncation_sweep();
    if (g_failures != 0) {
        fprintf(stderr, "opds_feed host tests: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("opds_feed host tests: PASS\n");
    return 0;
}
