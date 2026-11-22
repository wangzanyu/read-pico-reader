/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：kosync 协议纯工具的宿主测试：RFC 1321 MD5 已知向量、小写十六进制、URL 规范化、
 *       认证密钥、partial MD5（空文件/短文件/多偏移/偏移跳过/末段截断）与文件名 MD5。
 * English: Host tests for the pure kosync protocol helpers: RFC 1321 MD5 known vectors,
 *       lowercase hex, URL normalisation, the auth key, the partial MD5 (empty/short files,
 *       several offsets, skipped offsets, truncated last chunk) and the file-name MD5.
 *
 * 冻结：仅宿主测试；临时文件写在系统临时目录，结束即删。
 * Frozen: host tests only; temporary files live in the system temp directory and are removed.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kosync_md5.h"
#include "kosync_proto.h"

static void expect_hex(const char* what, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got %s, want %s\n", what, got, want);
        abort();
    }
}

static void expect_lower_hex(const char* what, const char* hex) {
    for (size_t i = 0; i < 32; i++) {
        const char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            fprintf(stderr, "%s: non-lowercase-hex char '%c' at %u (%s)\n", what, c, (unsigned)i, hex);
            abort();
        }
    }
    if (hex[32] != 0) {
        fprintf(stderr, "%s: missing NUL terminator\n", what);
        abort();
    }
}

// RFC 1321 附录 A.5 的已知向量 + 100 万字符长串。
// RFC 1321 appendix A.5 known vectors plus the million-character string.
static void test_md5_vectors(void) {
    char hex[33];
    assert(kosync_md5_hex("", 0, hex));
    expect_hex("md5(\"\")", hex, "d41d8cd98f00b204e9800998ecf8427e");
    expect_lower_hex("md5(\"\")", hex);

    assert(kosync_md5_hex("abc", 3, hex));
    expect_hex("md5(\"abc\")", hex, "900150983cd24fb0d6963f7d28e17f72");

    assert(kosync_md5_hex("message digest", 14, hex));
    expect_hex("md5(\"message digest\")", hex, "f96b697d7cb7938d525a2f31aaf161d0");

    assert(kosync_md5_hex("abcdefghijklmnopqrstuvwxyz", 26, hex));
    expect_hex("md5(alphabet)", hex, "c3fcd3d76192e4007dfb496cca67e13b");

    assert(kosync_md5_hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 62, hex));
    expect_hex("md5(alnum)", hex, "d174ab98d277d9f5a5611c2c9f419d9f");

    assert(kosync_md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890", 80,
                          hex));
    expect_hex("md5(digits)", hex, "57edf4a22be3c955ac49da2e2107b67a");

    // 1,000,000 个 'a'：分块更新必须与一次性摘要一致。
    // One million 'a' bytes: chunked updates must match the one-shot digest.
    {
        const size_t size = 1000000;
        char* million = (char*)malloc(size);
        assert(million != NULL);
        memset(million, 'a', size);
        assert(kosync_md5_hex(million, size, hex));
        expect_hex("md5(1e6 a)", hex, "7707d6ae4e027c70eea2a935c2296f21");
        kosync_md5_t ctx;
        kosync_md5_init(&ctx);
        for (size_t at = 0; at < size; at += 7777) {
            const size_t n = size - at < 7777 ? size - at : 7777;
            kosync_md5_update(&ctx, million + at, n);
        }
        uint8_t digest[16];
        kosync_md5_final(&ctx, digest);
        static const char hexchars[] = "0123456789abcdef";
        for (unsigned i = 0; i < 16; i++) {
            hex[i * 2] = hexchars[digest[i] >> 4];
            hex[i * 2 + 1] = hexchars[digest[i] & 15];
        }
        hex[32] = 0;
        expect_hex("md5(1e6 a) chunked", hex, "7707d6ae4e027c70eea2a935c2296f21");
        free(million);
    }
}

static void test_normalize_url(void) {
    char out[256];
    size_t n = kosync_normalize_url(NULL, out, sizeof(out));
    assert(n == strlen(KOSYNC_PROTO_DEFAULT_SERVER));
    assert(strcmp(out, KOSYNC_PROTO_DEFAULT_SERVER) == 0);
    assert(kosync_normalize_url("", out, sizeof(out)) == n && strcmp(out, KOSYNC_PROTO_DEFAULT_SERVER) == 0);

    // 无 scheme：补 http://。 / Missing scheme: http:// is prefixed.
    assert(kosync_normalize_url("example.com:7200", out, sizeof(out)) == strlen("http://example.com:7200"));
    assert(strcmp(out, "http://example.com:7200") == 0);

    // 已有 scheme：原样保留。 / An existing scheme is preserved.
    assert(kosync_normalize_url("https://sync.koreader.rocks", out, sizeof(out)) == 27);
    assert(strcmp(out, "https://sync.koreader.rocks") == 0);

    // 去掉尾部所有 '/'。 / Every trailing slash is stripped.
    assert(kosync_normalize_url("http://example.com///", out, sizeof(out)) == strlen("http://example.com"));
    assert(strcmp(out, "http://example.com") == 0);

    // 缓冲刚好 / 不够。 / Buffer exactly big enough / too small.
    assert(kosync_normalize_url("http://example.com", out, strlen("http://example.com") + 1) == 18);
    assert(kosync_normalize_url("http://example.com", out, 8) == 0);
    assert(kosync_normalize_url("x", NULL, 8) == 0);
    assert(kosync_normalize_url("x", out, 0) == 0);
}

static void test_auth_key(void) {
    char key[33];
    assert(kosync_auth_key("pass", key));
    expect_hex("auth_key(\"pass\")", key, "1a1dc91c907325c69271ddf0c944bc72");
    expect_lower_hex("auth_key(\"pass\")", key);
    assert(kosync_auth_key("", key));
    expect_hex("auth_key(\"\")", key, "d41d8cd98f00b204e9800998ecf8427e");
    assert(!kosync_auth_key(NULL, key));
    assert(!kosync_auth_key("pass", NULL));
}

// 临时目录：TEMP / TMP / TMPDIR / /tmp / 当前目录。
// Temp directory: TEMP / TMP / TMPDIR / /tmp / current directory.
static const char* temp_dir(void) {
    static char dir[512];
    if (dir[0]) return dir;
    const char* env = getenv("TEMP");
    if (!env || !env[0]) env = getenv("TMP");
    if (!env || !env[0]) env = getenv("TMPDIR");
    if (!env || !env[0]) env = "/tmp";
    const size_t len = strlen(env);
    const size_t copy = len < sizeof(dir) ? len : sizeof(dir) - 1;
    memcpy(dir, env, copy);
    dir[copy] = 0;
    return dir;
}

// 文件内容按 (i*37+11)&0xFF 生成，便于与外部工具算出的摘要逐字节对拍。
// File content is (i*37+11)&0xFF so the externally computed digests can be compared bytewise.
static void write_pattern_file(const char* path, size_t size) {
    FILE* file = fopen(path, "wb");
    assert(file != NULL);
    for (size_t at = 0; at < size; at++) {
        const unsigned value = (unsigned)((at * 37u + 11u) & 0xffu);
        const int written = fputc((int)value, file);
        assert(written != EOF);
    }
    assert(fclose(file) == 0);
}

static void cleanup(const char* path) { (void)remove(path); }

// 拼接临时文件路径；容量不足直接失败，避免 snprintf 截断（也避免 -Wformat-truncation）。
// Join a temp path; abort when it cannot fit so nothing is ever silently truncated.
static void join_path(char* out, size_t cap, const char* dir, const char* name) {
    const size_t d = strlen(dir);
    const size_t n = strlen(name);
    if (d + 1 + n + 1 > cap) {
        fprintf(stderr, "temp path too long: %s/%s\n", dir, name);
        abort();
    }
    memcpy(out, dir, d);
    out[d] = '/';
    memcpy(out + d + 1, name, n + 1);
}

static void test_document_id_binary(void) {
    char path[640];
    char got[33];

    join_path(path, sizeof(path), temp_dir(), "kosync_proto_missing.bin");
    cleanup(path);
    assert(!kosync_document_id_binary(path, got));
    assert(!kosync_document_id_binary(NULL, got));

    // 0 字节：没有任何块被读入，摘要等于空串。
    // Zero bytes: no chunk is read at all, so the digest is the empty-string one.
    join_path(path, sizeof(path), temp_dir(), "kosync_proto_0.bin");
    write_pattern_file(path, 0);
    assert(kosync_document_id_binary(path, got));
    expect_hex("binary(0)", got, "d41d8cd98f00b204e9800998ecf8427e");
    expect_lower_hex("binary(0)", got);
    cleanup(path);

    // 100 字节：只有偏移 0 的截断块。 / 100 bytes: only the truncated chunk at offset 0.
    join_path(path, sizeof(path), temp_dir(), "kosync_proto_100.bin");
    write_pattern_file(path, 100);
    assert(kosync_document_id_binary(path, got));
    expect_hex("binary(100)", got, "5b9c57d4cba3367b9c65b22a80e88bca");
    cleanup(path);

    // 1024 字节：偏移 1024 不满足 < 文件长度，只读偏移 0。
    // 1024 bytes: offset 1024 is not < the file size, so only offset 0 is read.
    join_path(path, sizeof(path), temp_dir(), "kosync_proto_1024.bin");
    write_pattern_file(path, 1024);
    assert(kosync_document_id_binary(path, got));
    expect_hex("binary(1024)", got, "d4883a545d61557ce12ff4e95350e997");
    cleanup(path);

    // 2048 字节：偏移 0 与 1024。 / 2048 bytes: offsets 0 and 1024.
    join_path(path, sizeof(path), temp_dir(), "kosync_proto_2048.bin");
    write_pattern_file(path, 2048);
    assert(kosync_document_id_binary(path, got));
    expect_hex("binary(2048)", got, "e62a67c5b476dacf6149e487ccc3b42f");
    cleanup(path);

    // 3000 字节：4096 及以后的偏移全部跳过。 / 3000 bytes: every offset from 4096 on is skipped.
    join_path(path, sizeof(path), temp_dir(), "kosync_proto_3000.bin");
    write_pattern_file(path, 3000);
    assert(kosync_document_id_binary(path, got));
    expect_hex("binary(3000)", got, "e62a67c5b476dacf6149e487ccc3b42f");
    cleanup(path);

    // 5000 字节：0 / 1024 / 4096（末段 904 字节截断）。
    // 5000 bytes: 0 / 1024 / 4096 with a truncated 904-byte last chunk.
    join_path(path, sizeof(path), temp_dir(), "kosync_proto_5000.bin");
    write_pattern_file(path, 5000);
    assert(kosync_document_id_binary(path, got));
    expect_hex("binary(5000)", got, "bc0a36e392584120c95013be66f97b0b");
    cleanup(path);

    // 20000 字节：0 / 1024 / 4096 / 16384（末段截断）。
    // 20000 bytes: 0 / 1024 / 4096 / 16384 with a truncated last chunk.
    join_path(path, sizeof(path), temp_dir(), "kosync_proto_20000.bin");
    write_pattern_file(path, 20000);
    assert(kosync_document_id_binary(path, got));
    expect_hex("binary(20000)", got, "efc404fa609a799c7801273de6d65e84");
    cleanup(path);
}

static void test_document_id_filename(void) {
    char got[33];
    char long_path[640];

    assert(kosync_document_id_filename("/sdcard/books/My Book.epub", got));
    expect_hex("filename(My Book.epub)", got, "9a671b9d07b5404d84ce60af27058b04");
    expect_lower_hex("filename(My Book.epub)", got);

    // 没有 '/' 时整串都是文件名；只做 MD5，不做大小写/Unicode 归一化。
    // Without '/' the whole string is the name; only MD5, never case or Unicode folding.
    assert(kosync_document_id_filename("book.epub", got));
    {
        char expected[33];
        assert(kosync_md5_hex("book.epub", strlen("book.epub"), expected));
        expect_hex("filename(book.epub)", got, expected);
    }

    assert(!kosync_document_id_filename("/sdcard/books/", got));
    assert(!kosync_document_id_filename("", got));
    assert(!kosync_document_id_filename(NULL, got));
    assert(!kosync_document_id_filename("/sdcard/books/My Book.epub", NULL));

    // 400 字符的文件名：旧实现用 288 字节缓冲会截断，新实现必须哈希完整文件名。
    // A 400-character name: the old 288-byte buffer truncated it; the new code hashes all of it.
    {
        char name[420];
        memset(name, 'x', 400);
        memcpy(name + 400, ".epub", 5);
        name[405] = 0;
        static const char prefix[] = "/sdcard/books/";
        memcpy(long_path, prefix, sizeof(prefix) - 1);
        memcpy(long_path + sizeof(prefix) - 1, name, strlen(name) + 1);
        assert(kosync_document_id_filename(long_path, got));
        expect_hex("filename(long)", got, "3eea033be8d57e049321d51ac4f6b099");
    }
}

static void test_document_id_dispatch(void) {
    char path[640];
    char content[33];
    char name[33];
    join_path(path, sizeof(path), temp_dir(), "kosync_proto_dispatch.bin");
    write_pattern_file(path, 100);
    assert(kosync_document_id(path, true, content));
    assert(kosync_document_id(path, false, name));
    char expect_name[33];
    const char* base = strrchr(path, '/');
    assert(base && kosync_md5_hex(base + 1, strlen(base + 1), expect_name));
    expect_hex("dispatch(binary)", content, "5b9c57d4cba3367b9c65b22a80e88bca");
    expect_hex("dispatch(filename)", name, expect_name);
    assert(!kosync_document_id(NULL, true, content));
    cleanup(path);
}

int main(void) {
    fprintf(stderr, "md5 vectors\n");
    test_md5_vectors();
    fprintf(stderr, "url normalisation\n");
    test_normalize_url();
    fprintf(stderr, "auth key\n");
    test_auth_key();
    fprintf(stderr, "document id binary\n");
    test_document_id_binary();
    fprintf(stderr, "document id filename\n");
    test_document_id_filename();
    fprintf(stderr, "document id dispatch\n");
    test_document_id_dispatch();
    printf("kosync_proto: all tests passed\n");
    return 0;
}
