/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：KOReader 进度同步（kosync，仅 EPUB）。负责 TF 卡配置、partial MD5 文档 ID、
 *       认证与进度 HTTP、进度与 KOReader XPath 的双向映射，以及可取消的后台任务。
 *       界面在 main/apps/app_kosync.c；纯 XPath 映射在 main/book/kosync_xhtml.c。
 * English: KOReader progress sync (kosync, EPUB only): TF-card config, partial-MD5 document
 *       IDs, auth/progress HTTP, progress/XPath mapping and one cancellable worker. The page
 *       lives in main/apps/app_kosync.c and the pure XPath model in main/book/kosync_xhtml.c.
 *
 * 冻结：只访问用户配置的服务器；凭据只落 TF 卡；拉取成功也不自动覆盖本地进度。
 * Frozen: only the configured server is contacted; credentials stay on the TF card; a
 *       successful fetch never overwrites local progress on its own.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "book_kosync.h"

#include "book_epub.h"
#include "book_policy.h"
#include "book_progress.h"
#include "book_store.h"
#include "book_title.h"
#include "kosync_proto.h"
#include "kosync_xhtml.h"
#include "read_pico_transfer.h"
#include "settings.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define KOSYNC_DIR "/.readpico/net"
#define KOSYNC_FILE "/kosync.json"
#define KOSYNC_TMP ".tmp"
#define KOSYNC_PREV ".previous"
#define KOSYNC_HTTP_TIMEOUT_MS 20000
#define KOSYNC_HTTP_RESPONSE_MAX (64u * 1024u)
#define KOSYNC_DEVICE_NAME "kiikoread"
#define KOSYNC_DEVICE_ID "kiikoread"
#define KOSYNC_PSRAM_RESERVE (3u * 1024u * 1024u)

// ---- 状态 / state ------------------------------------------------------------
static kosync_status_t s_status;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;
static volatile bool s_running;
static volatile bool s_cancel;
static bool s_owns_network;
static bool s_time_synced;

typedef enum { KOSYNC_ACT_NONE = 0, KOSYNC_ACT_FETCH, KOSYNC_ACT_UPLOAD, KOSYNC_ACT_VERIFY } kosync_action_t;
static kosync_action_t s_action;

typedef struct {
    bool valid;
    char path[KOSYNC_PATH_MAX];
    char xpath[KOSYNC_XPATH_MAX];
    uint32_t chapter;
    uint32_t byte_off;
    float percent;
    char device[KOSYNC_DEVICE_MAX];
} kosync_remote_t;
static kosync_remote_t s_remote;

static void publish(kosync_state_t state, const char* message) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    if (message) snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
}

// ---- 小工具 / small helpers --------------------------------------------------

static size_t xor_hex_encode(const uint8_t* key, size_t key_len, const char* in, char* out, size_t cap) {
    if (!out || !cap) return 0;
    size_t n = strlen(in);
    if (n * 2 + 1 > cap) return 0;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        const uint8_t v = (uint8_t)in[i] ^ (key_len ? key[i % key_len] : 0xA5);
        out[i * 2] = hex[v >> 4];
        out[i * 2 + 1] = hex[v & 15];
    }
    out[n * 2] = 0;
    return n * 2;
}

static bool hex_xor_decode(const uint8_t* key, size_t key_len, const char* in, char* out, size_t cap) {
    if (!in || !out || !cap) return false;
    const size_t n = strlen(in) / 2;
    if (n * 2 != strlen(in) || n + 1 > cap) return false;
    for (size_t i = 0; i < n; i++) {
        int hi = in[i * 2], lo = in[i * 2 + 1];
        hi = hi >= '0' && hi <= '9' ? hi - '0' : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 : hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : -1;
        lo = lo >= '0' && lo <= '9' ? lo - '0' : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 : lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return false;
        out[i] = (char)((uint8_t)((hi << 4) | lo) ^ (key_len ? key[i % key_len] : 0xA5));
    }
    out[n] = 0;
    return true;
}

static void device_key(uint8_t key[6]) {
    if (esp_read_mac(key, ESP_MAC_WIFI_STA) != ESP_OK) {
        memset(key, 0, 6);
        key[0] = 0x5A;
    }
}

static bool config_dir(char* dir, size_t cap, char* file, size_t file_cap) {
    book_store_root_t root;
    if (book_store_upload_root(&root) != ESP_OK || !root.path[0]) return false;
    int n = snprintf(dir, cap, "%s" KOSYNC_DIR, root.path);
    if (n <= 0 || (size_t)n >= cap) return false;
    n = snprintf(file, file_cap, "%s%s", dir, KOSYNC_FILE);
    return n > 0 && (size_t)n < file_cap;
}

// 有界字符串复制：路径拼接用，避免把超长路径静默截断成另一个文件。
// Bounded copy for path assembly so an overlong path never silently becomes another file.
static void copy_bounded(char* dst, size_t cap, const char* src) {
    if (!dst || !cap) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n && src) memcpy(dst, src, n);
    dst[n] = 0;
}

static bool write_file_atomic(const char* path, const char* text) {
    char tmp[KOSYNC_PATH_MAX + 96], prev[KOSYNC_PATH_MAX + 96];
    const int tmp_len = snprintf(tmp, sizeof(tmp), "%s%s", path, KOSYNC_TMP);
    const int prev_len = snprintf(prev, sizeof(prev), "%s%s", path, KOSYNC_PREV);
    if (tmp_len <= 0 || (size_t)tmp_len >= sizeof(tmp) || prev_len <= 0 ||
        (size_t)prev_len >= sizeof(prev))
        return false;
    FILE* file = fopen(tmp, "wb");
    if (!file) return false;
    bool ok = fwrite(text, 1, strlen(text), file) == strlen(text);
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    struct stat st;
    const bool previous = stat(path, &st) == 0;
    if (previous) {
        unlink(prev);
        if (rename(path, prev) != 0) {
            unlink(tmp);
            return false;
        }
    }
    ok = rename(tmp, path) == 0;
    if (!ok) {
        if (previous) (void)rename(prev, path);
        unlink(tmp);
    }
    return ok;
}

// ---- 配置 / configuration ---------------------------------------------------
// URL 规范化与认证密钥在 kosync_proto.c（纯 C、可宿主测试）；配置读写留在本文件。
// URL normalisation and the auth key live in kosync_proto.c (plain C, host-testable); the
// config file handling stays here.
esp_err_t kosync_config_load(kosync_config_t* config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    snprintf(config->server, sizeof(config->server), "%s", KOSYNC_DEFAULT_SERVER);
    config->match = KOSYNC_MATCH_BINARY;
    char dir[BOOK_STORE_PATH_MAX + 32], path[KOSYNC_PATH_MAX + 64];
    char prev_path[KOSYNC_PATH_MAX + 96];
    if (!config_dir(dir, sizeof(dir), path, sizeof(path))) return ESP_ERR_NOT_FOUND;
    FILE* file = fopen(path, "rb");
    if (!file) {
        const int prev_len = snprintf(prev_path, sizeof(prev_path), "%s%s", path, KOSYNC_PREV);
        if (prev_len <= 0 || (size_t)prev_len >= sizeof(prev_path)) return ESP_ERR_NOT_FOUND;
        file = fopen(prev_path, "rb");
        if (!file) return ESP_ERR_NOT_FOUND;
    }
    char* text = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t length = fread(text, 1, 4095, file);
    bool complete = !ferror(file) && fgetc(file) == EOF;
    fclose(file);
    text[length] = 0;
    if (!complete) {
        heap_caps_free(text);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON* object = cJSON_Parse(text);
    heap_caps_free(text);
    if (!object) return ESP_ERR_INVALID_RESPONSE;
    cJSON* item = cJSON_GetObjectItem(object, "server");
    if (cJSON_IsString(item) && item->valuestring[0]) {
        char normalized[KOSYNC_SERVER_MAX];
        if (kosync_normalize_url(item->valuestring, normalized, sizeof(normalized)))
            snprintf(config->server, sizeof(config->server), "%s", normalized);
    }
    item = cJSON_GetObjectItem(object, "username");
    if (cJSON_IsString(item)) snprintf(config->username, sizeof(config->username), "%s", item->valuestring);
    item = cJSON_GetObjectItem(object, "password");
    uint8_t key[6];
    device_key(key);
    if (cJSON_IsString(item)) (void)hex_xor_decode(key, sizeof(key), item->valuestring, config->password, sizeof(config->password));
    item = cJSON_GetObjectItem(object, "match");
    if (cJSON_IsNumber(item) && item->valueint == KOSYNC_MATCH_FILENAME) config->match = KOSYNC_MATCH_FILENAME;
    cJSON_Delete(object);
    return ESP_OK;
}

esp_err_t kosync_config_save(const kosync_config_t* config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    char dir[BOOK_STORE_PATH_MAX + 32], path[KOSYNC_PATH_MAX + 64];
    if (!config_dir(dir, sizeof(dir), path, sizeof(path))) return ESP_ERR_NOT_FOUND;
    mkdir(dir, 0777);
    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    char normalized[KOSYNC_SERVER_MAX];
    if (kosync_normalize_url(config->server, normalized, sizeof(normalized)))
        cJSON_AddStringToObject(root, "server", normalized);
    cJSON_AddStringToObject(root, "username", config->username);
    uint8_t key[6];
    device_key(key);
    char encoded[KOSYNC_PASS_MAX * 2 + 1];
    if (!xor_hex_encode(key, sizeof(key), config->password, encoded, sizeof(encoded))) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON_AddStringToObject(root, "password", encoded);
    cJSON_AddNumberToObject(root, "match", config->match ? KOSYNC_MATCH_BINARY : KOSYNC_MATCH_FILENAME);
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    const bool ok = write_file_atomic(path, text);
    cJSON_free(text);
    return ok ? ESP_OK : ESP_FAIL;
}

bool kosync_config_ready(const kosync_config_t* config) {
    return config && config->username[0] && config->password[0] && config->server[0];
}

// ---- 文档 ID / document identifiers -----------------------------------------
// 文档 ID 计算在 kosync_proto.c（纯 C、可宿主测试）；此处不再保留重复实现。
// Document ID computation lives in kosync_proto.c (plain C, host-testable); no duplicate
// implementation is kept here.

// ---- HTTP -------------------------------------------------------------------
typedef struct {
    char* buffer;
    size_t cap;
    size_t length;
    bool overflow;
} http_sink_t;

static esp_err_t http_event(esp_http_client_event_t* event) {
    if (s_cancel) return ESP_FAIL;
    http_sink_t* sink = (http_sink_t*)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && sink) {
        if (sink->length + (size_t)event->data_len >= sink->cap) {
            sink->overflow = true;
            return ESP_FAIL;
        }
        memcpy(sink->buffer + sink->length, event->data, (size_t)event->data_len);
        sink->length += (size_t)event->data_len;
        sink->buffer[sink->length] = 0;
    }
    return ESP_OK;
}

static bool base64_encode(const char* in, char* out, size_t cap) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t n = strlen(in);
    const size_t need = ((n + 2) / 3) * 4 + 1;
    if (need > cap) return false;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const unsigned a = (unsigned char)in[i];
        const unsigned b = i + 1 < n ? (unsigned char)in[i + 1] : 0;
        const unsigned c = i + 2 < n ? (unsigned char)in[i + 2] : 0;
        out[o++] = table[a >> 2];
        out[o++] = table[((a & 3) << 4) | (b >> 4)];
        out[o++] = i + 1 < n ? table[((b & 15) << 2) | (c >> 6)] : '=';
        out[o++] = i + 2 < n ? table[c & 63] : '=';
    }
    out[o] = 0;
    return true;
}

static const char* s_http_error;

static bool http_request(const char* url, const char* method, const char* body, const char* username,
                         const char* auth_key, char* response, size_t response_cap, int* status) {
    s_http_error = NULL;
    if (s_cancel || !url) return false;
    memset(response, 0, response_cap);
    http_sink_t sink = {.buffer = response, .cap = response_cap, .length = 0, .overflow = false};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = KOSYNC_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        s_http_error = "网络内存不足";
        return false;
    }
    esp_http_client_set_user_data(client, &sink);
    esp_http_client_set_header(client, "Accept", "application/vnd.koreader.v1+json");
    esp_http_client_set_header(client, "User-Agent", "kiikoread/" KOSYNC_DEVICE_ID);
    if (username && auth_key) {
        esp_http_client_set_header(client, "x-auth-user", username);
        esp_http_client_set_header(client, "x-auth-key", auth_key);
        char plain[KOSYNC_USER_MAX + 40], encoded[192], basic[200];
        snprintf(plain, sizeof(plain), "%s:%s", username, auth_key);
        if (base64_encode(plain, encoded, sizeof(encoded))) {
            snprintf(basic, sizeof(basic), "Basic %s", encoded);
            esp_http_client_set_header(client, "Authorization", basic);
        }
        memset(plain, 0, sizeof(plain));
        memset(encoded, 0, sizeof(encoded));
        memset(basic, 0, sizeof(basic));
    }
    if (body && (!strcmp(method, "PUT") || !strcmp(method, "POST"))) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_method(client, !strcmp(method, "PUT") ? HTTP_METHOD_PUT : HTTP_METHOD_POST);
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
    }
    esp_err_t err = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    if (status) *status = http_status;
    if (err != ESP_OK) {
        int tls_code = 0, verify_flags = 0;
        esp_err_t tls_error = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &verify_flags);
        if (verify_flags) s_http_error = "HTTPS 证书校验失败";
        else if (tls_error != ESP_OK || tls_code) s_http_error = "HTTPS 连接失败";
        else s_http_error = "网络连接失败";
        esp_http_client_cleanup(client);
        return false;
    }
    if (sink.overflow) {
        s_http_error = "服务器响应过长";
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_cleanup(client);
    return true;
}

// ---- 网络与时间 / network and clock ----------------------------------------
static bool ensure_network(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.network_ready && status.state == READ_PICO_TRANSFER_READY) return true;
    char ssid[33];
    bool configured = false;
    read_pico_transfer_get_saved_wifi(ssid, &configured);
    if (!configured) {
        publish(KOSYNC_FAILED, "请先连接 WiFi（设置 → WiFi）");
        return false;
    }
    if (status.state != READ_PICO_TRANSFER_STOPPED) read_pico_transfer_stop();
    read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
    if (read_pico_transfer_start(&cfg) != ESP_OK) {
        publish(KOSYNC_FAILED, "WiFi 连接启动失败");
        return false;
    }
    s_owns_network = true;
    for (int i = 0; i < 300 && !s_cancel; ++i) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        if (status.network_ready) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    publish(s_cancel ? KOSYNC_CANCELLED : KOSYNC_FAILED, s_cancel ? "已取消" : "WiFi 连接超时");
    return false;
}

// HTTPS 证书校验需要有效系统时间；对时失败仍尝试请求（证书可能仍命中）。
// TLS certificate validation needs a sane clock; a failed sync still tries the request.
static void ensure_clock(void) {
    const time_t now = time(NULL);
    if (now > 1700000000) {
        s_time_synced = true;
        return;
    }
    uint32_t utc = 0;
    if (read_pico_transfer_sync_time_online(&utc) == ESP_OK && utc > 1700000000u) s_time_synced = true;
}

// ---- 进度映射 / progress mapping -------------------------------------------
typedef struct {
    book_epub_t* book;
    html_text_t text;
    kosync_xhtml_t* model;
    char* html;
    size_t html_len;
    uint32_t chapter;
    uint32_t chapter_offset;
    uint32_t chapter_size;
    uint32_t total;
} map_chapter_t;

static void map_chapter_close(map_chapter_t* map) {
    if (!map) return;
    kosync_xhtml_free(map->model);
    map->model = NULL;
    html_text_free(&map->text);
    free(map->html);
    map->html = NULL;
}

static uint32_t chapter_raw_offset(const book_epub_t* book, size_t chapter) {
    return book_epub_chapter_byte_offset(book, chapter);
}

static uint32_t chapter_raw_size(const book_epub_t* book, size_t chapter) {
    const size_t count = book_epub_chapter_count(book);
    const uint32_t start = chapter_raw_offset(book, chapter);
    if (chapter + 1 < count) {
        const uint32_t end = chapter_raw_offset(book, chapter + 1);
        return end > start ? end - start : 0;
    }
    const uint32_t total = book_epub_total_bytes(book);
    return total > start ? total - start : 0;
}

static bool model_matches_text(const kosync_xhtml_t* model, const html_text_t* text) {
    if (!model || !text) return false;
    if (kosync_xhtml_block_count(model) != text->count) return false;
    for (size_t i = 0; i < text->count; i++) {
        const blk_t* block = &text->blocks[i];
        if (kosync_xhtml_block_length(model, (uint32_t)i) != block->len) return false;
        if (kosync_xhtml_block_hash(model, (uint32_t)i) != kosync_xhtml_hash(text->utf8 + block->offset, block->len))
            return false;
    }
    return true;
}

static bool map_open_chapter(map_chapter_t* map, book_epub_t* book, size_t chapter) {
    memset(map, 0, sizeof(*map));
    map->book = book;
    map->chapter = (uint32_t)chapter;
    map->total = book_epub_total_bytes(book);
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < KOSYNC_PSRAM_RESERVE) {
        publish(KOSYNC_FAILED, "内存不足，请关闭阅读界面后重试");
        return false;
    }
    map->chapter_offset = chapter_raw_offset(book, chapter);
    map->chapter_size = chapter_raw_size(book, chapter);
    if (book_epub_chapter_source(book, chapter, &map->html, &map->html_len) != ESP_OK) {
        publish(KOSYNC_FAILED, "读取章节失败");
        map_chapter_close(map);
        return false;
    }
    if (book_epub_load(book, chapter, &map->text) != ESP_OK) {
        publish(KOSYNC_FAILED, "解析章节失败");
        map_chapter_close(map);
        return false;
    }
    if (kosync_xhtml_parse(map->html, map->html_len, &map->model) != 0 || !model_matches_text(map->model, &map->text)) {
        kosync_xhtml_free(map->model);
        map->model = NULL;
    }
    return true;
}

// 块表按 offset 升序；返回包含该偏移的块下标。
// Block offsets ascend; return the paragraph containing the byte offset.
static size_t block_for_offset(const html_text_t* text, uint32_t byte_off) {
    if (!text->count) return 0;
    if (byte_off <= text->blocks[0].offset) return 0;
    size_t low = 0, high = text->count - 1, best = 0;
    for (;;) {
        const size_t mid = (low + high) / 2;
        if (text->blocks[mid].offset <= byte_off) {
            best = mid;
            if (mid == high) break;
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }
    return best;
}

static float map_percent_at(const map_chapter_t* map, uint32_t text_off) {
    if (!map->total) return 0.0f;
    const size_t text_len = map->text.len;
    const double intra = text_len ? (double)text_off / (double)text_len : 0.0;
    const double bytes = (double)map->chapter_offset + intra * (double)map->chapter_size;
    double pct = bytes / (double)map->total;
    if (pct < 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    return (float)pct;
}

// 本机进度 -> KOReader XPath；模型对拍失败时退化为整章 XPath。
// Local progress to KOReader XPath; a failed model check degrades to the chapter-only XPath.
static bool local_position_to_xpath(book_epub_t* book, uint32_t chapter, uint32_t text_off, char* xpath, size_t cap,
                                    float* percent) {
    map_chapter_t map;
    if (!map_open_chapter(&map, book, chapter)) return false;
    if (text_off >= map.text.len) text_off = map.text.len ? map.text.len - 1 : 0;
    if (map.model) {
        const size_t block = block_for_offset(&map.text, text_off);
        uint32_t norm_off = text_off > map.text.blocks[block].offset ? text_off - map.text.blocks[block].offset : 0;
        if (norm_off >= map.text.blocks[block].len) norm_off = map.text.blocks[block].len ? map.text.blocks[block].len - 1 : 0;
        if (kosync_xhtml_xpath_for(map.model, (uint32_t)block, norm_off, chapter, xpath, cap) != 0) {
            if (kosync_xpath_chapter_only(chapter, xpath, cap) != 0) {
                map_chapter_close(&map);
                return false;
            }
        }
    } else if (kosync_xpath_chapter_only(chapter, xpath, cap) != 0) {
        map_chapter_close(&map);
        return false;
    }
    if (percent) *percent = map_percent_at(&map, text_off);
    map_chapter_close(&map);
    return true;
}

// KOReader XPath -> 本机 (chapter, 章内文本字节偏移)。
// KOReader XPath to local (chapter, in-chapter text byte offset).
static bool remote_xpath_to_position(book_epub_t* book, const char* xpath, uint32_t* chapter_out, uint32_t* text_off,
                                     float* percent) {
    if (!xpath || strncmp(xpath, "/body/DocFragment[", 18) != 0) return false;
    const char* open = xpath + 18;
    char* end = NULL;
    const unsigned long fragment = strtoul(open, &end, 10);
    if (!end || *end != ']' || fragment == 0) return false;
    const size_t count = book_epub_chapter_count(book);
    if (fragment - 1 >= count) return false;
    const uint32_t chapter = (uint32_t)(fragment - 1);
    map_chapter_t map;
    if (!map_open_chapter(&map, book, chapter)) return false;
    bool ok = false;
    uint32_t local_off = 0;
    if (map.model) {
        uint32_t block = 0, norm_off = 0;
        if (kosync_xhtml_resolve(map.model, xpath, chapter, &block, &norm_off) == 0 && block < map.text.count) {
            local_off = (uint32_t)map.text.blocks[block].offset + norm_off;
            ok = true;
        }
    }
    if (ok) {
        if (map.text.len && local_off >= map.text.len) local_off = (uint32_t)map.text.len - 1;
        *chapter_out = chapter;
        *text_off = local_off;
        if (percent) *percent = map_percent_at(&map, local_off);
    }
    map_chapter_close(&map);
    return ok;
}

// 百分比兜底：按 spine 原始字节权重找到章与章内比例，再按章文本长度换算偏移。
// Percentage fallback: pick the chapter by raw spine byte weight, then scale by chapter text length.
static bool remote_percent_to_position(book_epub_t* book, float pct, uint32_t* chapter_out, uint32_t* text_off,
                                       float* percent) {
    const uint32_t total = book_epub_total_bytes(book);
    const size_t count = book_epub_chapter_count(book);
    if (!total || !count) return false;
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    uint32_t target = (uint32_t)((double)pct * total);
    size_t chapter = 0;
    for (size_t i = 0; i < count; i++) {
        const uint32_t start = chapter_raw_offset(book, i);
        const uint32_t size = chapter_raw_size(book, i);
        if (i + 1 == count || target < start + size) {
            chapter = i;
            break;
        }
    }
    const uint32_t start = chapter_raw_offset(book, chapter);
    const uint32_t size = chapter_raw_size(book, chapter);
    const double intra = size && target > start ? (double)(target - start) / (double)size : 0.0;
    map_chapter_t map;
    if (!map_open_chapter(&map, book, chapter)) return false;
    uint32_t local_off = (uint32_t)(intra * (double)map.text.len);
    if (map.text.len && local_off >= map.text.len) local_off = (uint32_t)map.text.len - 1;
    *chapter_out = (uint32_t)chapter;
    *text_off = local_off;
    if (percent) *percent = map_percent_at(&map, local_off);
    map_chapter_close(&map);
    return true;
}

// ---- 服务端请求 / server requests -------------------------------------------
static bool api_url(const kosync_config_t* config, const char* suffix, char* out, size_t cap) {
    char base[KOSYNC_SERVER_MAX];
    if (!kosync_normalize_url(config->server, base, sizeof(base))) return false;
    int n = snprintf(out, cap, "%s%s", base, suffix);
    return n > 0 && (size_t)n < cap;
}

static void publish_auth_failed(int status) {
    if (status == 401) publish(KOSYNC_FAILED, "账号或密码不正确");
    else publish(KOSYNC_FAILED, "服务器返回错误，请稍后重试");
}

static bool api_verify(const kosync_config_t* config) {
    char url[KOSYNC_SERVER_MAX + 64], key[33];
    if (!api_url(config, "/users/auth", url, sizeof(url))) return false;
    if (!kosync_auth_key(config->password, key)) return false;
    char* response = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) {
        publish(KOSYNC_FAILED, "内存不足");
        return false;
    }
    int status = 0;
    const bool ok = http_request(url, "GET", NULL, config->username, key, response, 4096, &status);
    heap_caps_free(response);
    if (!ok) {
        publish(KOSYNC_FAILED, s_http_error ? s_http_error : "网络连接失败");
        return false;
    }
    if (status >= 200 && status < 300) return true;
    publish_auth_failed(status);
    return false;
}

typedef struct {
    char xpath[KOSYNC_XPATH_MAX];
    float percentage;
    char device[KOSYNC_DEVICE_MAX];
} remote_progress_t;

// 0 成功；1 无记录；-1 失败。无记录时 message 已发布为等待上传的说明。
// 0 success, 1 no record, -1 failure; on "no record" the status message already explains it.
static int api_get_progress(const kosync_config_t* config, const char* document, remote_progress_t* out) {
    char url[KOSYNC_SERVER_MAX + 96], key[33];
    char suffix[64];
    const int suffix_len = snprintf(suffix, sizeof(suffix), "/syncs/progress/%s", document);
    if (suffix_len <= 0 || (size_t)suffix_len >= sizeof(suffix)) return -1;
    if (!api_url(config, suffix, url, sizeof(url))) return -1;
    if (!kosync_auth_key(config->password, key)) return -1;
    char* response = heap_caps_calloc(1, KOSYNC_HTTP_RESPONSE_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) {
        publish(KOSYNC_FAILED, "内存不足");
        return -1;
    }
    int status = 0;
    const bool ok = http_request(url, "GET", NULL, config->username, key, response, KOSYNC_HTTP_RESPONSE_MAX, &status);
    if (!ok) {
        publish(KOSYNC_FAILED, s_http_error ? s_http_error : "网络连接失败");
        heap_caps_free(response);
        return -1;
    }
    if (status == 204 || status == 404) {
        heap_caps_free(response);
        return 1;
    }
    if (status < 200 || status >= 300) {
        publish_auth_failed(status);
        heap_caps_free(response);
        return -1;
    }
    cJSON* root = cJSON_Parse(response);
    heap_caps_free(response);
    if (!root) {
        publish(KOSYNC_FAILED, "服务器响应无法解析");
        return -1;
    }
    cJSON* item = cJSON_GetObjectItem(root, "progress");
    const bool has_progress = cJSON_IsString(item) && item->valuestring[0];
    if (has_progress) snprintf(out->xpath, sizeof(out->xpath), "%s", item->valuestring);
    item = cJSON_GetObjectItem(root, "percentage");
    out->percentage = cJSON_IsNumber(item) ? (float)item->valuedouble : 0.0f;
    item = cJSON_GetObjectItem(root, "device");
    if (cJSON_IsString(item)) snprintf(out->device, sizeof(out->device), "%s", item->valuestring);
    cJSON_Delete(root);
    if (!has_progress && out->percentage <= 0.0f) return 1;
    return 0;
}

static bool api_put_progress(const kosync_config_t* config, const char* document, const char* xpath, float percentage) {
    char url[KOSYNC_SERVER_MAX + 64], key[33];
    if (!api_url(config, "/syncs/progress", url, sizeof(url))) return false;
    if (!kosync_auth_key(config->password, key)) return false;
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        publish(KOSYNC_FAILED, "内存不足");
        return false;
    }
    cJSON_AddStringToObject(root, "document", document);
    cJSON_AddStringToObject(root, "progress", xpath ? xpath : "");
    cJSON_AddNumberToObject(root, "percentage", (double)percentage);
    cJSON_AddStringToObject(root, "device", KOSYNC_DEVICE_NAME);
    cJSON_AddStringToObject(root, "device_id", KOSYNC_DEVICE_ID);
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        publish(KOSYNC_FAILED, "内存不足");
        return false;
    }
    char* response = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!response) {
        cJSON_free(body);
        publish(KOSYNC_FAILED, "内存不足");
        return false;
    }
    int status = 0;
    const bool ok = http_request(url, "PUT", body, config->username, key, response, 4096, &status);
    cJSON_free(body);
    heap_caps_free(response);
    if (!ok) {
        publish(KOSYNC_FAILED, s_http_error ? s_http_error : "网络连接失败");
        return false;
    }
    if (status >= 200 && status < 300) return true;
    publish_auth_failed(status);
    return false;
}

// ---- 本地书与进度 / local book and progress -------------------------------
typedef struct {
    book_epub_t* book;
    char title[KOSYNC_TITLE_MAX];
    uint32_t size;
    bool has_progress;
    book_progress_t progress;
} local_book_t;

static void local_book_close(local_book_t* local) {
    if (!local) return;
    book_epub_close(local->book);
    local->book = NULL;
}

static bool local_book_open(local_book_t* local, const char* path) {
    memset(local, 0, sizeof(*local));
    if (!path || !path[0]) return false;
    const char* ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".epub") != 0) {
        publish(KOSYNC_FAILED, "只有 EPUB 支持 KOReader 同步");
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        publish(KOSYNC_FAILED, "书籍文件不可用");
        return false;
    }
    local->size = (uint32_t)st.st_size;
    if (book_epub_open(path, &local->book) != ESP_OK || !local->book) {
        publish(KOSYNC_FAILED, "打开书籍失败");
        return false;
    }
    char author[96];
    if (!book_display_name_from_path(path, local->title, sizeof(local->title), author, sizeof(author))) {
        const char* base = strrchr(path, '/');
        copy_bounded(local->title, sizeof(local->title), base ? base + 1 : path);
    }
    esp_err_t err = book_progress_load_checked(path, local->size, &local->progress);
    local->has_progress = err == ESP_OK;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        publish(KOSYNC_FAILED, "读取阅读进度失败");
        local_book_close(local);
        return false;
    }
    return true;
}

// 最近阅读的书籍路径；没有时输出空串。
// Most recently opened book path; empty when none.
static bool last_book_path(char* out, size_t cap) {
    return book_progress_last_path(out, cap);
}

static void local_position(const local_book_t* local, uint32_t* chapter, uint32_t* text_off, float* percent) {
    if (local->has_progress && local->progress.chapter < book_epub_chapter_count(local->book)) {
        *chapter = local->progress.chapter;
        *text_off = local->progress.byte_off;
        *percent = (float)local->progress.pct / 100.0f;
    } else {
        *chapter = 0;
        *text_off = 0;
        *percent = 0.0f;
    }
}

static float local_bytes_percent(book_epub_t* book, uint32_t chapter, uint32_t text_off) {
    map_chapter_t map;
    if (!map_open_chapter(&map, book, chapter)) return 0.0f;
    if (text_off >= map.text.len) text_off = map.text.len ? map.text.len - 1 : 0;
    const float pct = map_percent_at(&map, text_off);
    map_chapter_close(&map);
    return pct;
}

// ---- 后台任务 / worker ------------------------------------------------------
static void publish_result(kosync_result_t result, const char* message, float local_pct, float remote_pct) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.result = result;
    s_status.local_percent = local_pct;
    s_status.remote_percent = remote_pct;
    ++s_status.revision;
    if (message) snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    if (s_lock) xSemaphoreGive(s_lock);
}

static void worker_fetch(const char* path) {
    kosync_config_t config;
    if (kosync_config_load(&config) != ESP_OK || !kosync_config_ready(&config)) {
        publish(KOSYNC_FAILED, "请先配置 KOReader 同步账号");
        return;
    }
    if (!ensure_network()) return;
    ensure_clock();
    publish(KOSYNC_PREPARING, "正在准备进度…");
    local_book_t local;
    if (!local_book_open(&local, path)) return;
    snprintf(s_status.book_title, sizeof(s_status.book_title), "%s", local.title);
    uint32_t chapter = 0, text_off = 0;
    float local_percent = 0.0f;
    local_position(&local, &chapter, &text_off, &local_percent);
    char local_xpath[KOSYNC_XPATH_MAX] = "";
    if (!local_position_to_xpath(local.book, chapter, text_off, local_xpath, sizeof(local_xpath), &local_percent)) {
        // 章节太大或内存不足时退化为整章 XPath + 已存百分比，至少不阻断同步。
        // Degrade to a chapter-only XPath plus the saved percentage when the chapter is too
        // large or memory is short, so sync is not blocked.
        if (kosync_xpath_chapter_only(chapter, local_xpath, sizeof(local_xpath)) != 0) {
            publish(KOSYNC_FAILED, "本地进度映射失败");
            local_book_close(&local);
            return;
        }
        local_percent = local.has_progress ? (float)local.progress.pct / 100.0f : 0.0f;
    }
    char primary[33] = "", alternate[33] = "";
    if (!kosync_document_id(path, config.match == KOSYNC_MATCH_BINARY, primary)) {
        publish(KOSYNC_FAILED, "计算文档 ID 失败");
        local_book_close(&local);
        return;
    }
    (void)kosync_document_id(path, config.match != KOSYNC_MATCH_BINARY, alternate);
    local_book_close(&local);

    publish(KOSYNC_FETCHING, "正在读取云端进度…");
    remote_progress_t remote;
    memset(&remote, 0, sizeof(remote));
    int result = api_get_progress(&config, primary, &remote);
    if (result == 1 && alternate[0]) {
        result = api_get_progress(&config, alternate, &remote);
        if (result == 0) snprintf(primary, sizeof(primary), "%s", alternate);
    }
    if (result == 1) {
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.has_remote = false;
        s_remote.valid = false;
        if (s_lock) xSemaphoreGive(s_lock);
        publish_result(KOSYNC_REMOTE_MISSING, "云端没有这本书的进度，可上传本机进度", local_percent, 0.0f);
        publish(KOSYNC_DONE, "云端没有这本书的进度，可上传本机进度");
        return;
    }
    if (result != 0) return;

    publish(KOSYNC_COMPARING, "正在比较进度…");
    // 重新打开书籍以映射远端位置：远端 XPath 优先，失败退回百分比。
    // Reopen the book to map the remote position: XPath first, percentage as the fallback.
    book_epub_t* book = NULL;
    float remote_percent = remote.percentage;
    uint32_t remote_chapter = 0, remote_text_off = 0;
    bool mapped = false;
    if (book_epub_open(path, &book) == ESP_OK && book) {
        if (remote.xpath[0] && remote_xpath_to_position(book, remote.xpath, &remote_chapter, &remote_text_off, NULL)) {
            remote_percent = local_bytes_percent(book, remote_chapter, remote_text_off);
            mapped = true;
        } else if (remote_percent > 0.0f && remote_percent_to_position(book, remote_percent, &remote_chapter,
                                                                      &remote_text_off, NULL)) {
            remote_percent = local_bytes_percent(book, remote_chapter, remote_text_off);
            mapped = true;
        }
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.has_remote = true;
    s_status.remote_percent = remote_percent;
    s_status.local_percent = local_percent;
    snprintf(s_status.remote_device, sizeof(s_status.remote_device), "%s", remote.device);
    s_remote.valid = mapped || remote_percent > 0.0f;
    snprintf(s_remote.path, sizeof(s_remote.path), "%s", path);
    snprintf(s_remote.xpath, sizeof(s_remote.xpath), "%s", remote.xpath);
    snprintf(s_remote.device, sizeof(s_remote.device), "%s", remote.device);
    s_remote.chapter = remote_chapter;
    s_remote.byte_off = remote_text_off;
    s_remote.percent = remote_percent;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    if (book) book_epub_close(book);

    char compare[KOSYNC_MESSAGE_MAX];
    if (!mapped) {
        publish_result(KOSYNC_REMOTE_AHEAD, "云端进度存在但无法精确映射，将按百分比恢复", local_percent, remote_percent);
        return;
    }
    const float delta = remote_percent - local_percent;
    if (fabsf(delta) <= 0.001f) {
        snprintf(compare, sizeof(compare), "两端进度一致（%.1f%%）", (double)(local_percent * 100.0f));
        publish_result(KOSYNC_SYNCED, compare, local_percent, remote_percent);
    } else if (delta > 0.0f) {
        snprintf(compare, sizeof(compare), "云端领先：云端 %.1f%%，本机 %.1f%%",
                 (double)(remote_percent * 100.0f), (double)(local_percent * 100.0f));
        publish_result(KOSYNC_REMOTE_AHEAD, compare, local_percent, remote_percent);
    } else {
        snprintf(compare, sizeof(compare), "本机领先：本机 %.1f%%，云端 %.1f%%",
                 (double)(local_percent * 100.0f), (double)(remote_percent * 100.0f));
        publish_result(KOSYNC_LOCAL_AHEAD, compare, local_percent, remote_percent);
    }
    publish(KOSYNC_DONE, compare);
}

static void worker_upload(const char* path) {
    kosync_config_t config;
    if (kosync_config_load(&config) != ESP_OK || !kosync_config_ready(&config)) {
        publish(KOSYNC_FAILED, "请先配置 KOReader 同步账号");
        return;
    }
    if (!ensure_network()) return;
    ensure_clock();
    publish(KOSYNC_PREPARING, "正在准备进度…");
    local_book_t local;
    if (!local_book_open(&local, path)) return;
    snprintf(s_status.book_title, sizeof(s_status.book_title), "%s", local.title);
    uint32_t chapter = 0, text_off = 0;
    float percent = 0.0f;
    local_position(&local, &chapter, &text_off, &percent);
    if (!local.has_progress) {
        publish(KOSYNC_FAILED, "本机还没有这本书的阅读进度");
        local_book_close(&local);
        return;
    }
    char xpath[KOSYNC_XPATH_MAX] = "";
    if (!local_position_to_xpath(local.book, chapter, text_off, xpath, sizeof(xpath), &percent)) {
        if (kosync_xpath_chapter_only(chapter, xpath, sizeof(xpath)) != 0) {
            publish(KOSYNC_FAILED, "本地进度映射失败");
            local_book_close(&local);
            return;
        }
        percent = (float)local.progress.pct / 100.0f;
    }
    char document[33] = "";
    if (!kosync_document_id(path, config.match == KOSYNC_MATCH_BINARY, document)) {
        publish(KOSYNC_FAILED, "计算文档 ID 失败");
        return;
    }
    publish(KOSYNC_UPLOADING, "正在上传进度…");
    if (!api_put_progress(&config, document, xpath, percent)) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.result = KOSYNC_SYNCED;
    s_status.local_percent = percent;
    s_status.remote_percent = percent;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    publish(KOSYNC_DONE, "已上传本机进度");
}

static void worker_verify(void) {
    kosync_config_t config;
    if (kosync_config_load(&config) != ESP_OK || !kosync_config_ready(&config)) {
        publish(KOSYNC_FAILED, "请先配置 KOReader 同步账号");
        return;
    }
    if (!ensure_network()) return;
    ensure_clock();
    publish(KOSYNC_FETCHING, "正在验证账号…");
    if (!api_verify(&config)) return;
    publish(KOSYNC_DONE, "账号验证通过");
}

static void worker(void* argument) {
    (void)argument;
    char path[KOSYNC_PATH_MAX] = "";
    kosync_action_t action = s_action;
    if (action != KOSYNC_ACT_VERIFY) {
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(path, sizeof(path), "%s", s_status.book_path);
        if (s_lock) xSemaphoreGive(s_lock);
        if (!path[0] && (!last_book_path(path, sizeof(path)) || !path[0])) {
            publish(KOSYNC_FAILED, "还没有阅读过书籍");
            goto finish;
        }
    }
    switch (action) {
        case KOSYNC_ACT_FETCH: worker_fetch(path); break;
        case KOSYNC_ACT_UPLOAD: worker_upload(path); break;
        case KOSYNC_ACT_VERIFY: worker_verify(); break;
        default: break;
    }
finish:
    if (s_cancel) publish(KOSYNC_CANCELLED, "已取消");
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.busy = false;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    s_running = false;
    if (s_done) xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---- 公开 API / public API --------------------------------------------------
static void init_once(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
}

static esp_err_t start_work(kosync_action_t action, const char* book_path) {
    init_once();
    if (!s_lock || !s_done) return ESP_ERR_NO_MEM;
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (s_task) {
        xSemaphoreTake(s_done, portMAX_DELAY);
        s_task = NULL;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_action = action;
    s_cancel = false;
    s_status.state = KOSYNC_CONNECTING;
    s_status.result = KOSYNC_RESULT_NONE;
    s_status.has_remote = false;
    s_status.remote_applied = false;
    s_status.remote_percent = 0.0f;
    s_status.local_percent = 0.0f;
    s_status.remote_device[0] = 0;
    s_status.message[0] = 0;
    s_status.book_path[0] = 0;
    s_status.book_title[0] = 0;
    if (book_path) snprintf(s_status.book_path, sizeof(s_status.book_path), "%s", book_path);
    s_status.busy = true;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {
    }
    s_running = true;
    if (xTaskCreate(worker, "kosync", 24576, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        publish(KOSYNC_FAILED, "后台任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t kosync_start(const char* book_path) {
    if (book_path && book_path[0]) {
        // 明确指定的书籍优先，否则沿用最近阅读。
        // An explicit book wins; otherwise the worker falls back to the last opened one.
    }
    return start_work(KOSYNC_ACT_FETCH, book_path);
}

esp_err_t kosync_upload(const char* book_path) { return start_work(KOSYNC_ACT_UPLOAD, book_path); }

esp_err_t kosync_verify(void) { return start_work(KOSYNC_ACT_VERIFY, NULL); }

void kosync_get_status(kosync_status_t* status) {
    if (!status) return;
    init_once();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    if (s_lock) xSemaphoreGive(s_lock);
}

bool kosync_busy(void) { return s_running; }

esp_err_t kosync_apply_remote(void) {
    init_once();
    if (s_running) return ESP_ERR_INVALID_STATE;
    kosync_remote_t remote;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    remote = s_remote;
    if (s_lock) xSemaphoreGive(s_lock);
    if (!remote.valid || !remote.path[0]) {
        publish(KOSYNC_FAILED, "没有可应用的云端进度");
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(remote.path, &st) != 0 || st.st_size <= 0) {
        publish(KOSYNC_FAILED, "书籍文件不可用");
        return ESP_ERR_NOT_FOUND;
    }
    book_epub_t* book = NULL;
    if (book_epub_open(remote.path, &book) != ESP_OK || !book) {
        publish(KOSYNC_FAILED, "打开书籍失败");
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t chapter = remote.chapter;
    uint32_t text_off = remote.byte_off;
    bool mapped = remote.xpath[0] && remote_xpath_to_position(book, remote.xpath, &chapter, &text_off, NULL);
    if (!mapped && remote.percent > 0.0f) mapped = remote_percent_to_position(book, remote.percent, &chapter, &text_off, NULL);
    if (!mapped) {
        book_epub_close(book);
        publish(KOSYNC_FAILED, "云端进度无法映射到本书");
        return ESP_ERR_INVALID_RESPONSE;
    }
    book_progress_t progress = {0};
    const uint32_t size = (uint32_t)st.st_size;
    (void)book_progress_load_checked(remote.path, size, &progress);
    progress.file_size = size;
    progress.chapter = (uint16_t)(chapter > 0xFFFF ? 0xFFFF : chapter);
    progress.byte_off = text_off;
    progress.px = app_settings_book_px();
    float percent = local_bytes_percent(book, chapter, text_off);
    progress.pct = (uint8_t)(percent * 100.0f + 0.5f);
    book_epub_close(book);
    esp_err_t err = book_progress_save(remote.path, &progress);
    if (err != ESP_OK) {
        publish(KOSYNC_FAILED, "写入阅读进度失败");
        return err;
    }
    book_progress_notify_changed();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.result = KOSYNC_SYNCED;
    s_status.remote_applied = true;
    s_status.local_percent = percent;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    publish(KOSYNC_DONE, "已应用云端进度，下次开书生效");
    return ESP_OK;
}

void kosync_cancel_join(void) {
    if (s_running) {
        s_cancel = true;
        if (s_done) xSemaphoreTake(s_done, pdMS_TO_TICKS(60000));
        s_task = NULL;
    }
    if (s_owns_network) {
        read_pico_transfer_stop();
        s_owns_network = false;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.busy = false;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
}

void kosync_close(void) {
    kosync_cancel_join();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_remote.valid = false;
    s_remote.path[0] = 0;
    if (s_lock) xSemaphoreGive(s_lock);
}
