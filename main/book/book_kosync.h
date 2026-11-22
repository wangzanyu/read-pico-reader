/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：KOReader 进度同步（kosync 协议，仅 EPUB）：配置存 TF 卡、partial MD5 文档 ID、
 *       HTTP 拉取/上传、进度与 XPath 的双向映射，以及一个可取消的后台同步任务。
 * English: KOReader progress sync (kosync, EPUB only): TF-card configuration, partial-MD5
 *       document IDs, HTTP fetch/upload, progress <-> XPath mapping and one cancellable
 *       background sync task.
 *
 * 冻结：只访问用户配置的服务器；凭据只落 TF 卡（不进 NVS/备份）；不静默覆盖本地进度。
 * Frozen: Only the user-configured server is contacted; credentials stay on the TF card
 *       (never NVS/backup); local progress is never overwritten silently.
 *
 * 冻结：同步在后台任务里跑，界面只轮询状态；离页/锁屏/卸卡必须 cancel_join 后再返回。
 * Frozen: Sync runs in a background task and the page only polls status; pages must
 *       cancel_join before leaving, locking or returning after media loss.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "kosync_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 服务器 URL 缓冲上限（含 http:// 前缀）。/ Server URL bound including a scheme prefix.
#define KOSYNC_SERVER_MAX 192
#define KOSYNC_USER_MAX 64
#define KOSYNC_PASS_MAX 96
#define KOSYNC_MESSAGE_MAX 160
#define KOSYNC_PATH_MAX 288
#define KOSYNC_TITLE_MAX 128
#define KOSYNC_DEVICE_MAX 64

/// 默认公共同步服务器；用户可在设置页改成自建服务器。
/// Default public sync server; the settings page accepts a self-hosted one.
/// 实现与常量都在 kosync_proto.h / kosync_proto.c。/ Implemented in kosync_proto.{h,c}.
#define KOSYNC_DEFAULT_SERVER KOSYNC_PROTO_DEFAULT_SERVER

/// 文档匹配方式。/ How a local file is matched to the server document.
typedef enum {
    KOSYNC_MATCH_FILENAME = 0, ///< 文件名 MD5（跨来源同名书也能对上）/ MD5 of the file name
    KOSYNC_MATCH_BINARY = 1,   ///< 文件内容 partial MD5（与 KOReader 一致）/ KOReader partial MD5
} kosync_match_t;

typedef struct {
    char server[KOSYNC_SERVER_MAX]; ///< 完整基础 URL / full base URL
    char username[KOSYNC_USER_MAX];
    char password[KOSYNC_PASS_MAX]; ///< 明文只在内存；卡上做 XOR+hex 混淆 / plaintext in RAM only
    uint8_t match;                  ///< kosync_match_t
} kosync_config_t;

/// 配置读写：文件为 TF 卡 `/.readpico/net/kosync.json`，缺失时返回默认值。
/// Config file `/.readpico/net/kosync.json` on the TF card; missing fields fall back to defaults.
esp_err_t kosync_config_load(kosync_config_t* config);
esp_err_t kosync_config_save(const kosync_config_t* config);
/// 服务器、用户名与密码都可用时返回 true。/ True when server, user and password are usable.
bool kosync_config_ready(const kosync_config_t* config);
/// URL 规范化：无 scheme 补 http://，去掉尾部 '/'；空串用默认服务器。
/// Normalize a base URL: add http:// when no scheme, strip trailing slashes; empty means default.
/// 实现在 kosync_proto.c。/ Implemented in kosync_proto.c.
size_t kosync_normalize_url(const char* url, char* out, size_t cap);
/// 认证密钥：md5(password) 小写十六进制；失败返回 false。
/// Auth key: lowercase hex md5(password); false on failure.
/// 实现在 kosync_proto.c。/ Implemented in kosync_proto.c.
bool kosync_auth_key(const char* password, char out[33]);

typedef enum {
    KOSYNC_IDLE = 0,
    KOSYNC_CONNECTING, ///< 拉起 STA 网络 / bringing up the station link
    KOSYNC_PREPARING,  ///< 计算文档 ID 与本地 XPath / hashing and mapping locally
    KOSYNC_FETCHING,   ///< 拉取远端进度 / fetching remote progress
    KOSYNC_COMPARING,  ///< 比较两侧进度 / comparing both sides
    KOSYNC_UPLOADING,  ///< 上传本地进度 / uploading local progress
    KOSYNC_APPLYING,   ///< 写入远端进度 / applying remote progress
    KOSYNC_DONE,
    KOSYNC_FAILED,
    KOSYNC_CANCELLED,
} kosync_state_t;

typedef enum {
    KOSYNC_RESULT_NONE = 0,
    KOSYNC_REMOTE_AHEAD,   ///< 远端更新 / remote is ahead
    KOSYNC_LOCAL_AHEAD,    ///< 本地更新 / local is ahead
    KOSYNC_SYNCED,         ///< 两侧一致 / already synchronized
    KOSYNC_REMOTE_MISSING, ///< 服务器没有这本书的记录 / no remote record
} kosync_result_t;

typedef struct {
    kosync_state_t state;
    kosync_result_t result;
    char message[KOSYNC_MESSAGE_MAX]; ///< 给界面的一行说明 / one-line UI message
    char book_path[KOSYNC_PATH_MAX];
    char book_title[KOSYNC_TITLE_MAX];
    char remote_device[KOSYNC_DEVICE_MAX];
    float local_percent;  ///< 0..1，字符串进度缺失时的比较依据 / 0..1 comparison fallback
    float remote_percent; ///< 0..1
    unsigned revision;    ///< 每次变化自增 / incremented on every change
    bool busy;            ///< 后台任务仍在跑 / worker running
    bool has_remote;      ///< 最近一次拉取拿到了远端记录 / last fetch produced a record
    bool remote_applied;  ///< 已把远端进度写入本地进度库 / remote progress was written locally
} kosync_status_t;

/// 拉取并比较一本书的远端进度（不写本地进度）。book_path 为 NULL 时用最近阅读的书。
/// Fetch and compare one book's remote progress without touching local progress; NULL uses
/// the most recently opened book.
esp_err_t kosync_start(const char* book_path);
/// 上传一本书的本地进度（book_path 为 NULL 时用最近阅读的书）。
/// Upload one book's local progress; NULL uses the most recently opened book.
esp_err_t kosync_upload(const char* book_path);
/// 把最近一次拉取到的远端进度写入本地进度库（下次开书生效）。
/// Write the last fetched remote progress into the local progress store (applies on next open).
esp_err_t kosync_apply_remote(void);
/// 验证账号：GET /users/auth，异步。/ Validate credentials: GET /users/auth, asynchronous.
esp_err_t kosync_verify(void);
/// 请求取消并等待任务退出、释放它持有的网络；可重复调用。
/// Cancel, join, and release any network the task owned; repeatable.
void kosync_cancel_join(void);
/// 离页/卸卡后释放列表与配置缓存；TF 卡上的配置保留。
/// Release cached lists/memory after exit or media loss; the TF-card config stays.
void kosync_close(void);
/// 后台任务是否在跑 / Whether the worker is running.
bool kosync_busy(void);
/// 复制一致的状态快照 / Copy a consistent status snapshot.
void kosync_get_status(kosync_status_t* status);

#ifdef __cplusplus
}
#endif
