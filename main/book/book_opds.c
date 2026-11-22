/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 书库的服务器配置、feed 导航、EPUB 下载与入库，以及可取消的后台任务。
 *       Atom 解析与纯工具在 main/book/opds_feed.c，界面在 main/apps/app_opds.c。
 * English: OPDS library server config, feed navigation, EPUB download/import and one
 *       cancellable worker. Atom parsing and pure helpers live in main/book/opds_feed.c,
 *       the page in main/apps/app_opds.c.
 *
 * 冻结：只支持 OPDS 1.2 Atom 与 HTTP Basic；只收 application/epub+zip；正式文件在校验
 *       通过后才出现；凭据只落 TF 卡。
 * Frozen: OPDS 1.2 Atom and HTTP Basic only; application/epub+zip only; the final file
 *       appears only after validation; credentials stay on the TF card.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "book_opds.h"

#include "book_store.h"
#include "opds_feed.h"
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

#define OPDS_DIR "/.readpico/net"
#define OPDS_FILE "/opds.json"
#define OPDS_TMP ".tmp"
#define OPDS_PREV ".previous"
#define OPDS_NAV_MAX 12
#define OPDS_HTTP_TIMEOUT_MS 20000
#define OPDS_FETCH_TIMEOUT_MS 30000
#define OPDS_DOWNLOAD_TIMEOUT_MS 45000
#define OPDS_PSRAM_RESERVE (2u * 1024u * 1024u)
#define OPDS_MIN_BOOK_BYTES 1024u
#define OPDS_ACCEPT                                                                                    \
    "application/atom+xml;profile=opds-catalog;kind=navigation, "                                     \
    "application/atom+xml;profile=opds-catalog;kind=acquisition;q=0.9, application/atom+xml;q=0.8"

static opds_status_t s_status;
static opds_config_t s_config;
static bool s_config_loaded;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;
static volatile bool s_running;
static volatile bool s_cancel;
static bool s_owns_network;
static bool s_store_changed;

static opds_feed_parser_t* s_parser;
static char s_base_url[OPDS_URL_MAX];
static char s_feed_url[OPDS_URL_MAX];

static uint8_t s_server_index;
static char* s_nav_stack; // PSRAM：OPDS_NAV_MAX 个绝对 URL / absolute URLs in PSRAM
static uint16_t s_nav_depth;

typedef enum { OPDS_ACT_NONE = 0, OPDS_ACT_FETCH, OPDS_ACT_DOWNLOAD } opds_action_t;
static opds_action_t s_action;
static char s_work_url[OPDS_URL_MAX];
static uint16_t s_work_index;

static void* psram_alloc(void* ctx, size_t size) {
    (void)ctx;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void psram_free(void* ctx, void* memory) {
    (void)ctx;
    heap_caps_free(memory);
}

static void publish(opds_state_t state, const char* message) {
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    if (message) snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
}

// ---- 配置 / configuration ---------------------------------------------------
size_t opds_normalize_url(const char* url, char* out, size_t cap) {
    if (!out || !cap || !url || !url[0]) return 0;
    char buffer[OPDS_URL_MAX * 2];
    if (!strstr(url, "://")) snprintf(buffer, sizeof(buffer), "http://%s", url);
    else snprintf(buffer, sizeof(buffer), "%s", url);
    size_t n = strlen(buffer);
    while (n > 0 && buffer[n - 1] == '/') buffer[--n] = 0;
    if (n + 1 > cap) return 0;
    memcpy(out, buffer, n + 1);
    return n;
}

bool opds_server_ready(const opds_server_t* server) {
    return server && server->name[0] && server->url[0];
}

static void device_key(uint8_t key[6]) {
    if (esp_read_mac(key, ESP_MAC_WIFI_STA) != ESP_OK) {
        memset(key, 0, 6);
        key[0] = 0x5A;
    }
}

static void secret_encode(const char* in, char* out, size_t cap) {
    uint8_t key[6];
    device_key(key);
    static const char hex[] = "0123456789abcdef";
    const size_t n = strlen(in);
    if (n * 2 + 1 > cap) {
        out[0] = 0;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const uint8_t v = (uint8_t)in[i] ^ key[i % 6];
        out[i * 2] = hex[v >> 4];
        out[i * 2 + 1] = hex[v & 15];
    }
    out[n * 2] = 0;
}

static void secret_decode(const char* in, char* out, size_t cap) {
    uint8_t key[6];
    device_key(key);
    const size_t n = strlen(in) / 2;
    if (n * 2 != strlen(in) || n + 1 > cap) {
        out[0] = 0;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = in[i * 2], lo = in[i * 2 + 1];
        hi = hi >= '0' && hi <= '9' ? hi - '0' : hi >= 'a' && hi <= 'f' ? hi - 'a' + 10 : hi >= 'A' && hi <= 'F' ? hi - 'A' + 10 : 0;
        lo = lo >= '0' && lo <= '9' ? lo - '0' : lo >= 'a' && lo <= 'f' ? lo - 'a' + 10 : lo >= 'A' && lo <= 'F' ? lo - 'A' + 10 : 0;
        out[i] = (char)((uint8_t)((hi << 4) | lo) ^ key[i % 6]);
    }
    out[n] = 0;
}

static bool config_path(char* dir, size_t cap, char* file, size_t file_cap) {
    book_store_root_t root;
    if (book_store_upload_root(&root) != ESP_OK || !root.path[0]) return false;
    int n = snprintf(dir, cap, "%s" OPDS_DIR, root.path);
    if (n <= 0 || (size_t)n >= cap) return false;
    n = snprintf(file, file_cap, "%s%s", dir, OPDS_FILE);
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
    char tmp[BOOK_STORE_PATH_MAX * 2 + 64], prev[BOOK_STORE_PATH_MAX * 2 + 64];
    const int tmp_len = snprintf(tmp, sizeof(tmp), "%s%s", path, OPDS_TMP);
    const int prev_len = snprintf(prev, sizeof(prev), "%s%s", path, OPDS_PREV);
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

esp_err_t opds_config_load(opds_config_t* config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    char dir[BOOK_STORE_PATH_MAX + 32], path[BOOK_STORE_PATH_MAX * 2];
    if (!config_path(dir, sizeof(dir), path, sizeof(path))) return ESP_ERR_NOT_FOUND;
    FILE* file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    char* text = heap_caps_malloc(OPDS_MAX_SERVERS * (OPDS_URL_MAX + OPDS_NAME_MAX) + 1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    size_t length = fread(text, 1, OPDS_MAX_SERVERS * (OPDS_URL_MAX + OPDS_NAME_MAX) + 1023, file);
    bool complete = !ferror(file) && fgetc(file) == EOF;
    fclose(file);
    text[length] = 0;
    if (!complete) {
        heap_caps_free(text);
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON* root = cJSON_Parse(text);
    heap_caps_free(text);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON* servers = cJSON_GetObjectItem(root, "servers");
    if (cJSON_IsArray(servers)) {
        const int count = cJSON_GetArraySize(servers);
        for (int i = 0; i < count && config->count < OPDS_MAX_SERVERS; i++) {
            cJSON* item = cJSON_GetArrayItem(servers, i);
            if (!cJSON_IsObject(item)) continue;
            opds_server_t* server = &config->servers[config->count];
            cJSON* field = cJSON_GetObjectItem(item, "name");
            if (cJSON_IsString(field)) snprintf(server->name, sizeof(server->name), "%s", field->valuestring);
            field = cJSON_GetObjectItem(item, "url");
            if (cJSON_IsString(field)) {
                char normalized[OPDS_URL_MAX];
                if (opds_normalize_url(field->valuestring, normalized, sizeof(normalized)))
                    snprintf(server->url, sizeof(server->url), "%s", normalized);
            }
            field = cJSON_GetObjectItem(item, "user");
            if (cJSON_IsString(field)) snprintf(server->username, sizeof(server->username), "%s", field->valuestring);
            field = cJSON_GetObjectItem(item, "pass");
            if (cJSON_IsString(field)) secret_decode(field->valuestring, server->password, sizeof(server->password));
            if (opds_server_ready(server)) config->count++;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t opds_config_save(const opds_config_t* config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    char dir[BOOK_STORE_PATH_MAX + 32], path[BOOK_STORE_PATH_MAX * 2];
    if (!config_path(dir, sizeof(dir), path, sizeof(path))) return ESP_ERR_NOT_FOUND;
    mkdir(dir, 0777);
    cJSON* root = cJSON_CreateObject();
    // 用 CreateArray+AddItem 而不是 cJSON_AddArrayToObject，兼容 IDF 内置的较老 cJSON。
    // Use CreateArray+AddItem instead of cJSON_AddArrayToObject for older bundled cJSON.
    cJSON* servers = cJSON_CreateArray();
    if (!root || !servers) {
        if (servers) cJSON_Delete(servers);
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "servers", servers);
    for (uint8_t i = 0; i < config->count && i < OPDS_MAX_SERVERS; i++) {
        const opds_server_t* server = &config->servers[i];
        if (!opds_server_ready(server)) continue;
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", server->name);
        cJSON_AddStringToObject(item, "url", server->url);
        cJSON_AddStringToObject(item, "user", server->username);
        char encoded[OPDS_AUTH_MAX * 2 + 1];
        secret_encode(server->password, encoded, sizeof(encoded));
        cJSON_AddStringToObject(item, "pass", encoded);
        cJSON_AddItemToArray(servers, item);
    }
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    const bool ok = write_file_atomic(path, text);
    cJSON_free(text);
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t opds_config_reload(void) {
    opds_config_t next;
    esp_err_t err = opds_config_load(&next);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
    if (err == ESP_ERR_NOT_FOUND) memset(&next, 0, sizeof(next));
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config = next;
    s_config_loaded = true;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

const opds_config_t* opds_config_snapshot(void) {
    if (!s_config_loaded) (void)opds_config_reload();
    return &s_config;
}

// ---- 网络 / network ---------------------------------------------------------
static bool ensure_network(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.network_ready && status.state == READ_PICO_TRANSFER_READY) return true;
    char ssid[33];
    bool configured = false;
    read_pico_transfer_get_saved_wifi(ssid, &configured);
    if (!configured) {
        publish(OPDS_FAILED, "请先连接 WiFi（设置 → WiFi）");
        return false;
    }
    if (status.state != READ_PICO_TRANSFER_STOPPED) read_pico_transfer_stop();
    read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
    if (read_pico_transfer_start(&cfg) != ESP_OK) {
        publish(OPDS_FAILED, "WiFi 连接启动失败");
        return false;
    }
    s_owns_network = true;
    for (int i = 0; i < 300 && !s_cancel; ++i) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        if (status.network_ready) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    publish(s_cancel ? OPDS_CANCELLED : OPDS_FAILED, s_cancel ? "已取消" : "WiFi 连接超时");
    return false;
}

static void ensure_clock(void) {
    if (time(NULL) > 1700000000) return;
    uint32_t utc = 0;
    (void)read_pico_transfer_sync_time_online(&utc);
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

// HTTP Basic 的 Authorization 头必须是 "Basic <base64>"，少了前缀服务器会直接 401。
// The Authorization header must be "Basic <base64>"; the bare base64 value gets a 401.
#define OPDS_B64_MAX (((OPDS_AUTH_MAX * 2 + 2 + 2) / 3) * 4 + 1)
static void apply_basic_auth(esp_http_client_handle_t client, const opds_server_t* server) {
    if (!server || !server->username[0] || !server->password[0]) return;
    char plain[OPDS_AUTH_MAX * 2 + 2], encoded[OPDS_B64_MAX], basic[OPDS_B64_MAX + 8];
    snprintf(plain, sizeof(plain), "%s:%s", server->username, server->password);
    if (base64_encode(plain, encoded, sizeof(encoded))) {
        snprintf(basic, sizeof(basic), "Basic %s", encoded);
        esp_http_client_set_header(client, "Authorization", basic);
    }
    memset(plain, 0, sizeof(plain));
    memset(encoded, 0, sizeof(encoded));
    memset(basic, 0, sizeof(basic));
}

static void classify_http_failure(int status) {
    char message[OPDS_MESSAGE_MAX];
    if (status == 401 || status == 403) snprintf(message, sizeof(message), "服务器需要登录，请检查用户名和密码");
    else if (status >= 500) snprintf(message, sizeof(message), "服务器错误（HTTP %d），请稍后重试", status);
    else if (status > 0) snprintf(message, sizeof(message), "服务器返回 HTTP %d，请刷新重试", status);
    else snprintf(message, sizeof(message), "无法连接服务器，请检查地址与网络");
    publish(OPDS_FAILED, message);
}

// ---- feed 抓取 / feed fetching ---------------------------------------------
typedef struct {
    opds_feed_parser_t* parser;
    bool failed;
} feed_sink_t;

static esp_err_t feed_event(esp_http_client_event_t* event) {
    if (s_cancel) return ESP_FAIL;
    feed_sink_t* sink = (feed_sink_t*)event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && sink && !sink->failed) {
        if (!opds_feed_parser_feed(sink->parser, (const char*)event->data, (size_t)event->data_len)) {
            sink->failed = true;
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}


static bool push_navigation(const char* url) {
    if (!url || !url[0]) return true;
    if (s_nav_depth >= OPDS_NAV_MAX) {
        publish(OPDS_FAILED, "目录层数过多，请返回后再进入");
        return false;
    }
    if (!s_nav_stack) {
        s_nav_stack = heap_caps_calloc(OPDS_NAV_MAX, OPDS_URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_nav_stack) {
            publish(OPDS_FAILED, "内存不足");
            return false;
        }
    }
    snprintf(s_nav_stack + s_nav_depth * OPDS_URL_MAX, OPDS_URL_MAX, "%s", url);
    ++s_nav_depth;
    return true;
}

static bool worker_fetch(const uint8_t server_index, const char* url) {
    opds_server_t server;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (server_index >= s_config.count) {
        if (s_lock) xSemaphoreGive(s_lock);
        publish(OPDS_FAILED, "服务器不存在，请先在设置里添加");
        return false;
    }
    server = s_config.servers[server_index];
    s_server_index = server_index;
    if (s_lock) xSemaphoreGive(s_lock);
    char target[OPDS_URL_MAX];
    if (url && url[0]) snprintf(target, sizeof(target), "%s", url);
    else snprintf(target, sizeof(target), "%s", server.url);
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < OPDS_PSRAM_RESERVE) {
        publish(OPDS_FAILED, "内存不足，请关闭阅读界面后重试");
        return false;
    }
    publish(OPDS_CONNECTING, "正在连接服务器…");
    if (!ensure_network()) return false;
    ensure_clock();
    publish(OPDS_FETCHING, "正在读取目录…");
    opds_feed_memory_t memory = {.alloc = psram_alloc, .free = psram_free, .ctx = NULL};
    opds_feed_parser_t* parser = opds_feed_parser_create(target, &memory);
    if (!parser) {
        publish(OPDS_FAILED, "解析器内存不足");
        return false;
    }
    feed_sink_t sink = {.parser = parser, .failed = false};
    esp_http_client_config_t config = {
        .url = target,
        .timeout_ms = OPDS_FETCH_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = feed_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        opds_feed_parser_destroy(parser);
        publish(OPDS_FAILED, "网络内存不足");
        return false;
    }
    esp_http_client_set_user_data(client, &sink);
    esp_http_client_set_header(client, "Accept", OPDS_ACCEPT);
    esp_http_client_set_header(client, "User-Agent", "kiikoread/opds");
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    apply_basic_auth(client, &server);
    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || sink.failed || s_cancel) {
        opds_feed_parser_destroy(parser);
        if (s_cancel) publish(OPDS_CANCELLED, "已取消");
        else classify_http_failure(status);
        return false;
    }
    if (status < 200 || status >= 300) {
        opds_feed_parser_destroy(parser);
        classify_http_failure(status);
        return false;
    }
    if (!opds_feed_parser_finish(parser)) {
        opds_feed_parser_destroy(parser);
        publish(OPDS_FAILED, "目录内容无法解析，可能不是 OPDS 1.2 目录");
        return false;
    }
    opds_feed_info_t info;
    opds_feed_parser_info(parser, &info);
    if (info.total_entries == 0 && !info.feed_title[0]) {
        opds_feed_parser_destroy(parser);
        publish(OPDS_FAILED, "目录为空");
        return false;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_parser) opds_feed_parser_destroy(s_parser);
    s_parser = parser;
    snprintf(s_feed_url, sizeof(s_feed_url), "%s", target);
    snprintf(s_base_url, sizeof(s_base_url), "%s", target);
    s_status.feed_title[0] = 0;
    snprintf(s_status.feed_title, sizeof(s_status.feed_title), "%s", info.feed_title);
    s_status.entry_count = info.total_entries;
    s_status.book_count = 0;
    for (uint16_t i = 0; i < info.entry_count; i++) {
        opds_entry_t entry;
        if (opds_feed_parser_entry(parser, i, &entry) && entry.kind == OPDS_ENTRY_BOOK) ++s_status.book_count;
    }
    s_status.can_back = s_nav_depth > 0;
    s_status.downloading = false;
    s_status.downloaded = s_status.total = 0;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    publish(OPDS_DONE, info.total_entries ? "目录已加载" : "目录为空");
    return true;
}

// ---- 下载 / download -------------------------------------------------------
typedef struct {
    FILE* file;
    size_t written;
    bool failed;
    int64_t last_activity_us;
    uint32_t last_bucket;
    int64_t last_publish_us;
} download_sink_t;

static void publish_progress(download_sink_t* sink) {
    const int64_t now = esp_timer_get_time();
    // 300 ms 发布一次，页面再按 5% 档位决定是否刷新，避免频繁残影。
    // Publish every 300 ms; the page still only refreshes at 5% buckets to limit ghosting.
    if (now - sink->last_publish_us < 300000) return;
    sink->last_publish_us = now;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.downloaded = sink->written;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
}

static esp_err_t download_event(esp_http_client_event_t* event) {
    download_sink_t* sink = (download_sink_t*)event->user_data;
    if (!sink) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_CONNECTED || event->event_id == HTTP_EVENT_ON_HEADER ||
        event->event_id == HTTP_EVENT_ON_DATA)
        sink->last_activity_us = esp_timer_get_time();
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (s_cancel) {
        sink->failed = true;
        return ESP_FAIL;
    }
    if (esp_http_client_get_status_code(event->client) != 200) return ESP_OK;
    if (event->data_len > 0 && fwrite(event->data, 1, (size_t)event->data_len, sink->file) != (size_t)event->data_len) {
        sink->failed = true;
        return ESP_FAIL;
    }
    sink->written += (size_t)event->data_len;
    const uint32_t total = s_status.total;
    const uint32_t percent = total ? (uint32_t)((uint64_t)sink->written * 100u / total) : 0;
    if (percent / 5 != sink->last_bucket / 5) sink->last_bucket = percent;
    publish_progress(sink);
    return ESP_OK;
}

static esp_err_t download_perform(esp_http_client_handle_t client, download_sink_t* sink) {
    for (;;) {
        if (s_cancel) return ESP_ERR_INVALID_STATE;
        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN) return err;
        if (esp_timer_get_time() - sink->last_activity_us >= (int64_t)OPDS_DOWNLOAD_TIMEOUT_MS * 1000)
            return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool books_dir(char* out, size_t cap) {
    const char* configured = app_settings_books_dir();
    if (configured && configured[0]) {
        snprintf(out, cap, "%s", configured);
        return true;
    }
    book_store_root_t root;
    if (book_store_upload_root(&root) != ESP_OK || !root.path[0]) return false;
    snprintf(out, cap, "%s/books", root.path);
    return true;
}

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// 目标同名时追加 " (2)"…" (99)"，不覆盖用户已有文件。
// Append " (2)".." (99)" to avoid overwriting an existing file with the same name.
#define OPDS_PATH_BUF (BOOK_STORE_PATH_MAX + OPDS_TITLE_MAX + OPDS_AUTHOR_MAX + 64)
static bool unique_path(const char* dir, const char* name, char* out, size_t cap) {
    char stem[OPDS_TITLE_MAX + OPDS_AUTHOR_MAX + 16];
    char ext[16];
    char candidate[OPDS_PATH_BUF];
    const char* dot = strrchr(name, '.');
    if (dot && dot != name) {
        const size_t stem_len = (size_t)(dot - name);
        if (stem_len >= sizeof(stem) || strlen(dot) >= sizeof(ext)) return false;
        memcpy(stem, name, stem_len);
        stem[stem_len] = 0;
        snprintf(ext, sizeof(ext), "%s", dot);
    } else {
        if (strlen(name) >= sizeof(stem)) return false;
        memcpy(stem, name, strlen(name) + 1);
        ext[0] = 0;
    }
    for (int i = 1; i < 100; i++) {
        const int n = i == 1 ? snprintf(candidate, sizeof(candidate), "%s/%s%s", dir, stem, ext)
                             : snprintf(candidate, sizeof(candidate), "%s/%s (%d)%s", dir, stem, i, ext);
        if (n <= 0 || (size_t)n >= sizeof(candidate) || strlen(candidate) + 1 > cap) return false;
        copy_bounded(out, cap, candidate);
        if (!file_exists(out)) return true;
    }
    return false;
}

static bool worker_download(uint16_t index) {
    opds_entry_t entry;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool have = s_parser && opds_feed_parser_entry(s_parser, index, &entry) && entry.kind == OPDS_ENTRY_BOOK;
    const opds_server_t* server = &s_config.servers[s_server_index];
    opds_server_t server_copy;
    if (s_server_index < s_config.count) server_copy = *server;
    else memset(&server_copy, 0, sizeof(server_copy));
    if (s_lock) xSemaphoreGive(s_lock);
    if (!have || !entry.href[0]) {
        publish(OPDS_FAILED, "该条目没有可下载的 EPUB");
        return false;
    }
    publish(OPDS_CONNECTING, "正在连接服务器…");
    if (!ensure_network()) return false;
    ensure_clock();
    char dir[BOOK_STORE_PATH_MAX];
    if (!books_dir(dir, sizeof(dir))) {
        publish(OPDS_FAILED, "没有可用的书籍目录");
        return false;
    }
    mkdir(dir, 0777);
    char name[OPDS_TITLE_MAX + OPDS_AUTHOR_MAX + 16];
    if (!opds_sanitize_filename(entry.author, entry.title, name, sizeof(name))) {
        snprintf(name, sizeof(name), "book.epub");
    }
    char final_path[OPDS_PATH_BUF];
    if (!unique_path(dir, name, final_path, sizeof(final_path))) {
        publish(OPDS_FAILED, "文件名过长或目录不可用");
        return false;
    }
    char part_path[OPDS_PATH_BUF + 8];
    const int part_len = snprintf(part_path, sizeof(part_path), "%s.part", final_path);
    if (part_len <= 0 || (size_t)part_len >= sizeof(part_path)) {
        publish(OPDS_FAILED, "文件名过长或目录不可用");
        return false;
    }
    publish(OPDS_DOWNLOADING, "正在下载…");
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.downloading = true;
    s_status.downloaded = 0;
    s_status.total = entry.length;
    s_status.downloaded_path[0] = 0;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    FILE* file = fopen(part_path, "wb");
    if (!file) {
        publish(OPDS_FAILED, "无法写入书籍目录，请检查存储卡");
        if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.downloading = false;
        if (s_lock) xSemaphoreGive(s_lock);
        return false;
    }
    download_sink_t sink = {.file = file, .written = 0, .failed = false, .last_activity_us = esp_timer_get_time(),
                            .last_bucket = 0, .last_publish_us = 0};
    esp_http_client_config_t config = {
        .url = entry.href,
        .timeout_ms = OPDS_DOWNLOAD_TIMEOUT_MS,
        .is_async = true,
        .addr_type = HTTP_ADDR_TYPE_INET,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .event_handler = download_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    bool ok = false;
    if (client) {
        esp_http_client_set_user_data(client, &sink);
        esp_http_client_set_header(client, "Accept", "application/epub+zip, application/octet-stream;q=0.5");
        esp_http_client_set_header(client, "User-Agent", "kiikoread/opds");
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        apply_basic_auth(client, &server_copy);
        esp_err_t err = download_perform(client, &sink);
        const int status = esp_http_client_get_status_code(client);
        int64_t declared = esp_http_client_get_content_length(client);
        esp_http_client_cleanup(client);
        if (declared > 0) {
            if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.total = (size_t)declared;
            ++s_status.revision;
            if (s_lock) xSemaphoreGive(s_lock);
        }
        if (err != ESP_OK && !s_cancel) {
            publish(OPDS_FAILED, sink.written ? "下载中断，请重试" : "下载连接失败，请检查网络");
        } else if (status != 200) {
            classify_http_failure(status);
        } else if (sink.failed) {
            publish(OPDS_FAILED, "存储卡写入失败，请检查剩余空间");
        } else if (sink.written < OPDS_MIN_BOOK_BYTES) {
            publish(OPDS_FAILED, "下载内容不是有效的 EPUB");
        } else if (declared > 0 && (uint64_t)declared != sink.written) {
            publish(OPDS_FAILED, "下载不完整，请重试");
        } else {
            ok = true;
        }
    } else {
        publish(OPDS_FAILED, "网络内存不足");
    }
    const bool closed = fclose(file) == 0;
    if (ok && !closed) ok = false;
    if (ok) {
        // EPUB 是 ZIP：检查本地文件头，避免把错误页当成书入库。
        // EPUB is a ZIP: check the local file header so an error page never reaches the shelf.
        uint8_t magic[4] = {0};
        FILE* verify = fopen(part_path, "rb");
        const size_t got = verify ? fread(magic, 1, sizeof(magic), verify) : 0;
        if (verify) fclose(verify);
        ok = got == 4 && magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
        if (!ok) publish(OPDS_FAILED, "下载内容不是有效的 EPUB");
    }
    if (ok) {
        ok = rename(part_path, final_path) == 0;
        if (!ok) publish(OPDS_FAILED, "保存书籍失败，请检查存储卡");
    }
    if (!ok) {
        unlink(part_path);
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.downloading = false;
    s_status.downloaded = sink.written;
    ++s_status.revision;
    if (ok) {
        copy_bounded(s_status.downloaded_path, sizeof(s_status.downloaded_path), final_path);
        s_store_changed = true;
        s_status.store_changed = true;
    }
    if (s_lock) xSemaphoreGive(s_lock);
    if (ok) publish(OPDS_DONE, "已下载到书籍目录");
    else if (s_cancel) publish(OPDS_CANCELLED, "已取消");
    return ok;
}

// ---- 后台任务 / worker ------------------------------------------------------
static void worker(void* argument) {
    (void)argument;
    const opds_action_t action = s_action;
    const uint8_t server = s_server_index;
    char url[OPDS_URL_MAX];
    snprintf(url, sizeof(url), "%s", s_work_url);
    const uint16_t index = s_work_index;
    if (action == OPDS_ACT_FETCH) (void)worker_fetch(server, url[0] ? url : NULL);
    else if (action == OPDS_ACT_DOWNLOAD) (void)worker_download(index);
    if (s_cancel) publish(OPDS_CANCELLED, "已取消");
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

static esp_err_t start_work(opds_action_t action) {
    init_once();
    if (!s_lock || !s_done) return ESP_ERR_NO_MEM;
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (s_task) {
        xSemaphoreTake(s_done, portMAX_DELAY);
        s_task = NULL;
    }
    s_action = action;
    s_cancel = false;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = OPDS_CONNECTING;
    s_status.message[0] = 0;
    s_status.downloading = false;
    s_status.downloaded = s_status.total = 0;
    s_status.busy = true;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    while (xSemaphoreTake(s_done, 0) == pdTRUE) {
    }
    s_running = true;
    if (xTaskCreate(worker, "opds", 24576, NULL, 5, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        publish(OPDS_FAILED, "后台任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t opds_fetch(uint8_t server_index, const char* url) {
    init_once();
    if (!s_config_loaded) (void)opds_config_reload();
    if (server_index >= s_config.count) {
        publish(OPDS_FAILED, "请先在设置里添加 OPDS 服务器");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_running) return ESP_ERR_INVALID_STATE;
    // 进入子目录/翻页时把当前 feed 压栈，Back 才能原路返回。
    // Push the current feed when descending or paging so Back can return.
    if (url && url[0] && strcmp(url, s_feed_url) != 0 && s_feed_url[0]) {
        if (!push_navigation(s_feed_url)) return ESP_ERR_INVALID_STATE;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_server_index = server_index;
    s_work_url[0] = 0;
    if (url && url[0]) snprintf(s_work_url, sizeof(s_work_url), "%s", url);
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    return start_work(OPDS_ACT_FETCH);
}

esp_err_t opds_back(void) {
    init_once();
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (s_nav_depth == 0 || !s_nav_stack) return ESP_ERR_INVALID_STATE;
    --s_nav_depth;
    char url[OPDS_URL_MAX];
    snprintf(url, sizeof(url), "%s", s_nav_stack + s_nav_depth * OPDS_URL_MAX);
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_work_url, sizeof(s_work_url), "%s", url);
    s_status.can_back = s_nav_depth > 0;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    return start_work(OPDS_ACT_FETCH);
}

esp_err_t opds_download(uint16_t index) {
    init_once();
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_work_index = index;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
    return start_work(OPDS_ACT_DOWNLOAD);
}

bool opds_busy(void) { return s_running; }

void opds_get_status(opds_status_t* status) {
    if (!status) return;
    init_once();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *status = s_status;
    if (s_lock) xSemaphoreGive(s_lock);
}

uint16_t opds_entry_count(void) {
    init_once();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t count = s_parser ? opds_feed_parser_entry_count(s_parser) : 0;
    if (s_lock) xSemaphoreGive(s_lock);
    return count;
}

bool opds_entry_get(uint16_t index, opds_entry_t* out) {
    if (!out) return false;
    init_once();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = s_parser && opds_feed_parser_entry(s_parser, index, out);
    if (s_lock) xSemaphoreGive(s_lock);
    return ok;
}

bool opds_take_store_changed(void) {
    init_once();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = s_store_changed;
    s_store_changed = false;
    s_status.store_changed = false;
    if (s_lock) xSemaphoreGive(s_lock);
    return changed;
}

void opds_cancel_join(void) {
    if (s_running) {
        s_cancel = true;
        if (s_done) xSemaphoreTake(s_done, pdMS_TO_TICKS(45000));
        s_task = NULL;
    }
    if (s_owns_network) {
        read_pico_transfer_stop();
        s_owns_network = false;
    }
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.busy = false;
    s_status.downloading = false;
    ++s_status.revision;
    if (s_lock) xSemaphoreGive(s_lock);
}

void opds_close(void) {
    opds_cancel_join();
    // 超时仍没退出时宁可泄漏也不释放任务正在使用的解析器。
    // On a join timeout leak rather than free a parser the worker may still use.
    if (s_running) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_parser) {
        opds_feed_parser_destroy(s_parser);
        s_parser = NULL;
    }
    if (s_nav_stack) {
        heap_caps_free(s_nav_stack);
        s_nav_stack = NULL;
    }
    s_nav_depth = 0;
    s_feed_url[0] = 0;
    s_status.entry_count = 0;
    s_status.book_count = 0;
    if (s_lock) xSemaphoreGive(s_lock);
}
