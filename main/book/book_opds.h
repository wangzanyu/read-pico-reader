/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 书库：服务器列表存 TF 卡、feed 抓取与导航栈、EPUB 下载入库（临时文件 + 校验 +
 *       原子改名），后台任务与状态快照。界面在 main/apps/app_opds.c；Atom 解析在
 *       main/book/opds_feed.c。
 * English: OPDS library: TF-card server list, feed fetching with a navigation stack, EPUB
 *       downloads (staged file + validation + atomic rename), one background worker and a
 *       status snapshot. The page lives in main/apps/app_opds.c and Atom parsing in
 *       main/book/opds_feed.c.
 *
 * 冻结：只支持 OPDS 1.2 Atom 与 HTTP Basic；不实现 OAuth/LCP/OPDS 2.0 JSON/搜索；
 *       下载完成且校验通过前不出现正式文件。
 * Frozen: OPDS 1.2 Atom and HTTP Basic only; no OAuth/LCP/OPDS 2.0 JSON/search; a final file
 *       only appears after a completed download passed validation.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "opds_feed.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPDS_MAX_SERVERS 6
#define OPDS_NAME_MAX 64
#define OPDS_AUTH_MAX 64
#define OPDS_MESSAGE_MAX 160
#define OPDS_PATH_MAX 288

typedef struct {
    char name[OPDS_NAME_MAX];
    char url[OPDS_URL_MAX]; ///< 已规范化的绝对地址 / normalized absolute URL
    char username[OPDS_AUTH_MAX];
    char password[OPDS_AUTH_MAX]; ///< 明文只在内存；卡上 XOR+hex 混淆 / plaintext in RAM only
} opds_server_t;

typedef struct {
    opds_server_t servers[OPDS_MAX_SERVERS];
    uint8_t count;
} opds_config_t;

/// 配置读写：TF 卡 `/.readpico/net/opds.json`。/ TF-card config `/.readpico/net/opds.json`.
esp_err_t opds_config_load(opds_config_t* config);
esp_err_t opds_config_save(const opds_config_t* config);
/// 服务器条目可用（名称+地址）时返回 true。/ True when a server has a name and URL.
bool opds_server_ready(const opds_server_t* server);
/// 服务器 URL 规范化：无 scheme 补 http://，去尾部 '/'。/ Normalize a server URL.
size_t opds_normalize_url(const char* url, char* out, size_t cap);

typedef enum {
    OPDS_IDLE = 0,
    OPDS_CONNECTING,  ///< 拉起 STA / bringing up the station link
    OPDS_FETCHING,    ///< 抓取 feed / fetching a feed
    OPDS_DOWNLOADING, ///< 下载 EPUB / downloading an EPUB
    OPDS_DONE,
    OPDS_FAILED,
    OPDS_CANCELLED,
} opds_state_t;

typedef struct {
    opds_state_t state;
    char message[OPDS_MESSAGE_MAX]; ///< 一行说明 / one-line status
    char feed_title[OPDS_FEED_TITLE_MAX];
    uint16_t entry_count;
    uint16_t book_count;
    bool busy;
    bool downloading;
    bool can_back;                ///< 导航栈非空 / navigation stack is not empty
    size_t downloaded;            ///< 当前下载字节 / current download bytes
    size_t total;                 ///< 当前下载总字节（未知为 0）/ current download total (0 unknown)
    unsigned revision;            ///< 每次变化自增 / incremented on every change
    bool store_changed;           ///< 有新书落盘，界面线程应通知书架 / shelf rescan owed to the UI thread
    char downloaded_path[OPDS_PATH_MAX]; ///< 最近一次下载完成的书 / last completed download
} opds_status_t;

/// 抓取一个 feed：url 为空时取服务器根目录。异步。
/// Fetch a feed; an empty url means the server root. Asynchronous.
esp_err_t opds_fetch(uint8_t server_index, const char* url);
/// 返回上一级 feed（导航栈）。异步。/ Go back one navigation level. Asynchronous.
esp_err_t opds_back(void);
/// 下载当前 feed 的第 index 个条目（只接受书籍条目）。异步。
/// Download entry `index` of the current feed (book entries only). Asynchronous.
esp_err_t opds_download(uint16_t index);
/// 取消并等待任务退出、停止它持有的网络；可重复调用。
/// Cancel, join and release any network the worker owned; repeatable.
void opds_cancel_join(void);
/// 离页/卸卡后释放 feed 条目与缓存；TF 卡配置保留。
/// Release feed entries after exit or media loss; the TF-card config stays.
void opds_close(void);
bool opds_busy(void);
void opds_get_status(opds_status_t* status);
/// 当前 feed 的条目快照（仅界面线程读取；任务运行中也可读最后完成的 feed）。
/// Entry snapshot of the current feed (UI thread; valid for the last completed feed).
uint16_t opds_entry_count(void);
bool opds_entry_get(uint16_t index, opds_entry_t* out);
/// 配置缓存的只读快照，供界面列出服务器名。/ Cached config snapshot for server names.
const opds_config_t* opds_config_snapshot(void);
/// 取走“有新书”标记（界面线程调用后自行通知书架重扫）。
/// Take the "new book landed" flag; the UI thread then notifies the shelf rescan.
bool opds_take_store_changed(void);
/// 重新从 TF 卡读取服务器列表（界面进入前调用）。/ Reload the server list from the TF card.
esp_err_t opds_config_reload(void);

#ifdef __cplusplus
}
#endif
