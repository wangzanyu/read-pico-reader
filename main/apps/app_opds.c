/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 书库页面。维护最多 6 台 OPDS 1.2 服务器，浏览 Atom feed（导航/书籍/分页），
 *       下载 EPUB 到书籍目录；抓取与下载任务在 book_opds 里跑，页面只轮询状态快照。
 * English: OPDS library page. Manages up to six OPDS 1.2 servers, browses Atom feeds
 *       (navigation/books/pagination) and downloads EPUBs into the book root. Fetching and
 *       downloading live in book_opds; this page only polls the status snapshot.
 *
 * 冻结：只支持 OPDS 1.2 Atom 与 Basic 认证，不实现 OAuth/LCP/搜索；凭据只存 TF 卡；
 *       render 只画快照；离页/锁屏/卸卡先 cancel_join 再 close。
 * Frozen: OPDS 1.2 Atom with HTTP Basic only (no OAuth/LCP/search); credentials stay on the
 *       TF card; render paints snapshots only; leave/lock/media-loss cancel and join first.
 */

#ifdef ESP_PLATFORM
#include "ui_locale_format.h"
#endif
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "book_opds.h"
#include "book_store.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "opds_feed.h"
#include "ui_gesture.h"
#include "ui_keyboard.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_text_input.h"

#define OPDS_ROW_TOP 210
#define OPDS_ROW_STEP 96
#define OPDS_ROW_H 88
#define OPDS_ROWS 6

#define OPDS_BOOK_TOP 240
#define OPDS_BOOK_STEP 108
#define OPDS_BOOK_H 100
#define OPDS_BOOK_ROWS 5
#define OPDS_PAGER_TOP 800

#define OPDS_FIELD_NAME 0
#define OPDS_FIELD_URL 1
#define OPDS_FIELD_USER 2
#define OPDS_FIELD_PASS 3
#define OPDS_FIELD_COUNT 4

typedef enum {
    OPDS_VIEW_SERVERS = 0,
    OPDS_VIEW_BROWSE,
    OPDS_VIEW_EDIT,
    OPDS_VIEW_FIELD,
} opds_view_t;

static opds_view_t s_view = OPDS_VIEW_SERVERS;
/* 六台服务器的配置镜像约 6 KiB，放 PSRAM 与后台模块的卡上配置分开。 */
/* ~6 KiB server mirror in PSRAM, separate from the worker's on-card configuration. */
static EXT_RAM_BSS_ATTR opds_config_t s_cfg;
static opds_status_t s_status;
static unsigned s_seen_revision;
// 下载与读取目录期间只在本轮开始/结束时整页刷新，中间进度不碰屏幕。
// Refresh only when a download or catalog load starts/finishes; keep the panel static in between.
static bool s_busy_shown;
// 下载进度每 5% 一档自动刷新，只推底部进度行区域，不整页刷。
// Progress refreshes automatically in 5% buckets, pushing only the bottom line area.
static EpdRect s_progress_area;
static unsigned s_download_bucket = 0xFFFFu;
static int s_sel;
static int s_entry_page;
static char s_notice[OPDS_MESSAGE_MAX];
static char s_current_url[OPDS_URL_MAX];
static char s_last_download[OPDS_PATH_MAX];
static int s_pending_entry = -1;
static bool s_delete_confirm;

static int s_edit_index = -1;
static char s_edit_name[OPDS_NAME_MAX];
static char s_edit_url[OPDS_URL_MAX];
static char s_edit_user[OPDS_AUTH_MAX];
static char s_edit_pass[OPDS_AUTH_MAX];
static int s_edit_field;
static char s_field_buf[OPDS_URL_MAX];
static ui_text_edit_t s_field_input;
static bool s_pass_visible;
static EpdRect s_input_area;
static bool s_input_layout, s_input_settle;

/* ---- 布局 / layout ----------------------------------------------------------- */
static EpdRect server_row_rect(int row) {
    return (EpdRect){36, OPDS_ROW_TOP + row * OPDS_ROW_STEP, 612, OPDS_ROW_H};
}
static EpdRect server_add_rect(void) { return (EpdRect){36, 900, 196, 64}; }
static EpdRect server_edit_rect(void) { return (EpdRect){242, 900, 196, 64}; }
static EpdRect server_delete_rect(void) { return (EpdRect){448, 900, 200, 64}; }
static EpdRect browse_row_rect(int row) {
    return (EpdRect){36, OPDS_BOOK_TOP + row * OPDS_BOOK_STEP, 612, OPDS_BOOK_H};
}
static EpdRect browse_back_rect(void) { return (EpdRect){36, 920, 196, 64}; }
static EpdRect browse_refresh_rect(void) { return (EpdRect){242, 920, 196, 64}; }
static EpdRect browse_servers_rect(void) { return (EpdRect){448, 920, 200, 64}; }
static EpdRect confirm_download_rect(void) { return (EpdRect){80, 640, 240, 68}; }
static EpdRect confirm_cancel_rect(void) { return (EpdRect){364, 640, 240, 68}; }
static EpdRect confirm_delete_rect(void) { return (EpdRect){80, 640, 240, 68}; }
static EpdRect confirm_delete_cancel_rect(void) { return (EpdRect){364, 640, 240, 68}; }
static EpdRect edit_row_rect(int field) { return (EpdRect){36, 226 + field * 96, 612, 84}; }
static EpdRect edit_save_rect(void) { return (EpdRect){36, 640, 296, 72}; }
static EpdRect edit_cancel_rect(void) { return (EpdRect){352, 640, 296, 72}; }
static EpdRect field_toggle_rect(void) { return (EpdRect){36, 430, 200, 64}; }

/* ---- 文本与格式化 / text helpers -------------------------------------------- */
static void copy_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static void fit_text(char *text, int px, int width) {
    while (text[0] && ui_text_fixed_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

static void format_size(char *out, size_t cap, uint32_t bytes) {
    if (!bytes) { out[0] = 0; return; }
    if (bytes >= 1024u * 1024u) snprintf(out, cap, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    else snprintf(out, cap, "%u KB", (unsigned)((bytes + 1023u) / 1024u));
}

static void format_progress(char *out, size_t cap, size_t done, size_t total) {
    if (!total) { snprintf(out, cap, "已下载 %u KB", (unsigned)(done / 1024)); return; }
    unsigned percent = (unsigned)((uint64_t)done * 100u / total);
    snprintf(out, cap, "下载中 %u%% · %u/%u KB", percent,
             (unsigned)(done / 1024), (unsigned)(total / 1024));
}

static void config_load_from_card(void) {
    opds_config_reload();
    esp_err_t err = opds_config_load(&s_cfg);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND && !s_notice[0])
        snprintf(s_notice, sizeof(s_notice), "读取服务器配置失败：%s", esp_err_to_name(err));
    if (s_sel >= s_cfg.count) s_sel = s_cfg.count ? s_cfg.count - 1 : 0;
}

static const char *edit_field_label(int field) {
    switch (field) {
        case OPDS_FIELD_NAME: return "名称";
        case OPDS_FIELD_URL: return "地址";
        case OPDS_FIELD_USER: return "用户名";
        default: return "密码";
    }
}

static char *edit_field_buffer(int field) {
    switch (field) {
        case OPDS_FIELD_NAME: return s_edit_name;
        case OPDS_FIELD_URL: return s_edit_url;
        case OPDS_FIELD_USER: return s_edit_user;
        default: return s_edit_pass;
    }
}

static size_t edit_field_cap(int field) {
    switch (field) {
        case OPDS_FIELD_NAME: return sizeof(s_edit_name);
        case OPDS_FIELD_URL: return sizeof(s_edit_url);
        case OPDS_FIELD_USER: return sizeof(s_edit_user);
        default: return sizeof(s_edit_pass);
    }
}

static void edit_form_load(int index) {
    memset(s_edit_name, 0, sizeof(s_edit_name));
    memset(s_edit_url, 0, sizeof(s_edit_url));
    memset(s_edit_user, 0, sizeof(s_edit_user));
    memset(s_edit_pass, 0, sizeof(s_edit_pass));
    s_edit_index = index;
    if (index >= 0 && index < s_cfg.count) {
        copy_text(s_edit_name, sizeof(s_edit_name), s_cfg.servers[index].name);
        copy_text(s_edit_url, sizeof(s_edit_url), s_cfg.servers[index].url);
        copy_text(s_edit_user, sizeof(s_edit_user), s_cfg.servers[index].username);
        copy_text(s_edit_pass, sizeof(s_edit_pass), s_cfg.servers[index].password);
    }
    s_view = OPDS_VIEW_EDIT;
    s_notice[0] = 0;
}

static void field_open(int field) {
    s_edit_field = field;
    copy_text(s_field_buf, sizeof(s_field_buf), edit_field_buffer(field));
    ui_text_edit_init(&s_field_input, s_field_buf, sizeof(s_field_buf));
    s_pass_visible = false;
    ui_keyboard_begin(&s_field_input, true);
    s_view = OPDS_VIEW_FIELD;
    s_notice[0] = 0;
}

static void field_commit(void) {
    ui_keyboard_end();
    if (s_edit_field == OPDS_FIELD_PASS) {
        copy_text(s_edit_pass, sizeof(s_edit_pass), s_field_buf);
        memset(s_field_buf, 0, sizeof(s_field_buf));
    } else {
        copy_text(edit_field_buffer(s_edit_field), edit_field_cap(s_edit_field), s_field_buf);
        memset(s_field_buf, 0, sizeof(s_field_buf));
    }
    s_view = OPDS_VIEW_EDIT;
}

/* ---- 服务器表操作 / server actions ------------------------------------------ */
static void server_save_form(void) {
    if (!s_edit_name[0] || !s_edit_url[0]) {
        snprintf(s_notice, sizeof(s_notice), "名称与地址不能为空");
        return;
    }
    char normalized[OPDS_URL_MAX];
    if (!opds_normalize_url(s_edit_url, normalized, sizeof(normalized))) {
        snprintf(s_notice, sizeof(s_notice), "地址格式不正确");
        return;
    }
    if (s_edit_index < 0 && s_cfg.count >= OPDS_MAX_SERVERS) {
        snprintf(s_notice, sizeof(s_notice), "最多 %d 台服务器", OPDS_MAX_SERVERS);
        return;
    }
    opds_server_t server = {0};
    copy_text(server.name, sizeof(server.name), s_edit_name);
    copy_text(server.url, sizeof(server.url), normalized);
    copy_text(server.username, sizeof(server.username), s_edit_user);
    copy_text(server.password, sizeof(server.password), s_edit_pass);
    if (!opds_server_ready(&server)) {
        snprintf(s_notice, sizeof(s_notice), "服务器名称或地址无效");
        return;
    }
    if (s_edit_index >= 0 && s_edit_index < s_cfg.count) s_cfg.servers[s_edit_index] = server;
    else s_cfg.servers[s_cfg.count++] = server;
    const esp_err_t err = opds_config_save(&s_cfg);
    if (err != ESP_OK) {
        config_load_from_card();  /* 回滚未落盘的修改。/ Roll back the unpersisted edit. */
        snprintf(s_notice, sizeof(s_notice), "保存失败：%s", esp_err_to_name(err));
        return;
    }
    opds_config_reload();
    memset(s_edit_pass, 0, sizeof(s_edit_pass));
    s_view = OPDS_VIEW_SERVERS;
    snprintf(s_notice, sizeof(s_notice), "服务器已保存");
}

static void server_delete_selected(void) {
    if (s_sel < 0 || s_sel >= s_cfg.count) { s_delete_confirm = false; return; }
    for (int i = s_sel + 1; i < s_cfg.count; ++i) s_cfg.servers[i - 1] = s_cfg.servers[i];
    memset(&s_cfg.servers[s_cfg.count - 1], 0, sizeof(s_cfg.servers[0]));
    --s_cfg.count;
    const esp_err_t err = opds_config_save(&s_cfg);
    if (err != ESP_OK) {
        config_load_from_card();  /* 回滚未落盘的删除。/ Roll back the unpersisted delete. */
        snprintf(s_notice, sizeof(s_notice), "删除失败：%s", esp_err_to_name(err));
    } else snprintf(s_notice, sizeof(s_notice), "已删除服务器");
    opds_config_reload();
    if (s_sel >= s_cfg.count) s_sel = s_cfg.count ? s_cfg.count - 1 : 0;
    s_delete_confirm = false;
}

static void browse_open_selected(void) {
    if (s_sel < 0 || s_sel >= s_cfg.count) return;
    s_current_url[0] = 0;
    s_entry_page = 0;
    const esp_err_t err = opds_fetch((uint8_t)s_sel, NULL);
    if (err != ESP_OK) snprintf(s_notice, sizeof(s_notice), "打开服务器失败：%s", esp_err_to_name(err));
    else snprintf(s_notice, sizeof(s_notice), "正在读取目录…");
    s_view = OPDS_VIEW_BROWSE;
    s_seen_revision = 0;
}

static void browse_refresh(void) {
    if (s_sel < 0 || s_sel >= s_cfg.count) return;
    const esp_err_t err = opds_fetch((uint8_t)s_sel, s_current_url[0] ? s_current_url : NULL);
    if (err != ESP_OK) snprintf(s_notice, sizeof(s_notice), "刷新失败：%s", esp_err_to_name(err));
    else snprintf(s_notice, sizeof(s_notice), "正在刷新…");
    s_seen_revision = 0;
}

static void browse_back(void) {
    if (!s_status.can_back) return;
    const esp_err_t err = opds_back();
    if (err != ESP_OK) snprintf(s_notice, sizeof(s_notice), "返回失败：%s", esp_err_to_name(err));
    s_current_url[0] = 0;  /* 上级 URL 未知，刷新回根目录 */
    s_entry_page = 0;
    s_seen_revision = 0;
}

static void browse_entry_tap(int index) {
    opds_entry_t entry;
    if (!opds_entry_get((uint16_t)index, &entry)) return;
    if (entry.kind == OPDS_ENTRY_NAVIGATION) {
        copy_text(s_current_url, sizeof(s_current_url), entry.href);
        const esp_err_t err = opds_fetch((uint8_t)s_sel, entry.href);
        if (err != ESP_OK) snprintf(s_notice, sizeof(s_notice), "打开目录失败：%s", esp_err_to_name(err));
        else snprintf(s_notice, sizeof(s_notice), "正在读取目录…");
        s_entry_page = 0;
        s_seen_revision = 0;
    } else if (entry.kind == OPDS_ENTRY_BOOK) {
        s_pending_entry = index;
    } else {
        snprintf(s_notice, sizeof(s_notice), "此条目不提供 EPUB 下载");
    }
}

/* ---- 绘制 / rendering -------------------------------------------------------- */
static void render_header(uint8_t *fb, const char *title) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
    ui_hairline(fb, 174, 36, 612, UI_GRAY_LIGHT);
}

static void render_servers(uint8_t *fb) {
    render_header(fb, "OPDS 书库");
    char line[160];
    if (!s_cfg.count) {
        ui_text_vc(fb, 342, 420, 24, "还没有 OPDS 服务器", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 470, 20, "点下方「新增」填入名称与地址", EPD_DRAW_ALIGN_CENTER, false);
    }
    for (int row = 0; row < OPDS_ROWS && row < s_cfg.count; ++row) {
        const EpdRect rect = server_row_rect(row);
        const bool selected = row == s_sel;
        ui_fill_round_rect(fb, rect, 18, selected ? 0xee : UI_GRAY_WHITE);
        ui_draw_control_frame(fb, rect, 18, selected ? 0x40 : 0x80);
        char name[OPDS_NAME_MAX];
        copy_text(name, sizeof(name), s_cfg.servers[row].name);
        fit_text(name, 21, rect.width - 60);
        ui_text_vc(fb, rect.x + 22, rect.y + 30, 21, name, EPD_DRAW_ALIGN_LEFT, false);
        char url[OPDS_URL_MAX];
        copy_text(url, sizeof(url), s_cfg.servers[row].url);
        fit_text(url, 17, rect.width - 60);
        ui_text_vc(fb, rect.x + 22, rect.y + 64, 17, url, EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_icon(fb, rect.x + rect.width - 26, rect.y + rect.height / 2, 24,
                     UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
    }
    snprintf(line, sizeof(line), "%d/%d 台 · %s", s_cfg.count ? s_sel + 1 : 0, OPDS_MAX_SERVERS,
             s_cfg.count ? "点选进入浏览，长按编辑" : "先新增服务器");
    ui_text_vc(fb, 342, 856, 19, line, EPD_DRAW_ALIGN_CENTER, false);
    if (s_notice[0]) ui_text_box(fb, (EpdRect){36, 878, 612, 22}, 17, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, server_add_rect(), "新增", false);
    ui_draw_button(fb, server_edit_rect(), "编辑", false);
    ui_draw_button(fb, server_delete_rect(), "删除", false);

    if (s_delete_confirm) {
        ui_fill_round_rect(fb, (EpdRect){60, 430, 564, 300}, 24, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, (EpdRect){60, 430, 564, 300}, 24, 0x50);
        ui_text_vc(fb, 342, 500, 25, "删除这台服务器？", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, 342, 556, 19, "只删除配置，不删除已下载的书籍", EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, confirm_delete_rect(), "删除", true);
        ui_draw_button(fb, confirm_delete_cancel_rect(), "取消", false);
    }
}

static void render_browse(uint8_t *fb) {
    render_header(fb, "OPDS 浏览");
    char line[192];
    copy_text(line, sizeof(line), s_status.feed_title[0] ? s_status.feed_title : "目录");
    fit_text(line, 21, 580);
    ui_text_vc(fb, 342, 196, 21, line, EPD_DRAW_ALIGN_CENTER, false);

    const uint16_t count = opds_entry_count();
    if (!count) {
        ui_text_vc(fb, 342, 460, 22, s_status.busy ? "正在读取目录…" : "此目录没有可用条目",
                   EPD_DRAW_ALIGN_CENTER, false);
    }
    for (int row = 0; row < OPDS_BOOK_ROWS; ++row) {
        const int index = s_entry_page * OPDS_BOOK_ROWS + row;
        if (index >= (int)count) break;
        opds_entry_t entry;
        if (!opds_entry_get((uint16_t)index, &entry)) break;
        const EpdRect rect = browse_row_rect(row);
        ui_fill_round_rect(fb, rect, 18, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, rect, 18, 0x80);
        char title[OPDS_TITLE_MAX];
        copy_text(title, sizeof(title), entry.title[0] ? entry.title : "(无标题)");
        fit_text(title, entry.kind == OPDS_ENTRY_NAVIGATION ? 22 : 20, rect.width - 150);
        ui_text_vc(fb, rect.x + 22, rect.y + 32, entry.kind == OPDS_ENTRY_NAVIGATION ? 22 : 20,
                   title, EPD_DRAW_ALIGN_LEFT, false);
        if (entry.kind == OPDS_ENTRY_NAVIGATION) {
            ui_draw_icon(fb, rect.x + rect.width - 24, rect.y + rect.height / 2, 24,
                         UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
        } else if (entry.kind == OPDS_ENTRY_BOOK) {
            char meta[96] = {0};
            char size[24];
            format_size(size, sizeof(size), entry.length);
            if (entry.author[0] && size[0]) snprintf(meta, sizeof(meta), "%s · %s", entry.author, size);
            else if (entry.author[0]) copy_text(meta, sizeof(meta), entry.author);
            else copy_text(meta, sizeof(meta), size);
            fit_text(meta, 17, rect.width - 90);
            ui_text_vc(fb, rect.x + 22, rect.y + 66, 17, meta, EPD_DRAW_ALIGN_LEFT, false);
            ui_draw_icon(fb, rect.x + rect.width - 24, rect.y + rect.height / 2, 22,
                         UI_ICON_DOWNLOAD, UI_GRAY_BLACK);
        } else {
            ui_text_vc(fb, rect.x + rect.width - 24, rect.y + rect.height / 2, 18, "—",
                       EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    if (count > OPDS_BOOK_ROWS) {
        const int pages = (count + OPDS_BOOK_ROWS - 1) / OPDS_BOOK_ROWS;
        char label[48];
        snprintf(label, sizeof(label), "%d/%d · 共%u项", s_entry_page + 1, pages, (unsigned)count);
        ui_pager_draw_at(fb, label, s_entry_page, pages, false, -1, OPDS_PAGER_TOP);
    }
    if (s_status.downloading) {
        // 进度按 5% 一档自动刷新，这里只画底部这一行。
        // Progress refreshes in 5% steps; only this bottom line is painted here.
        format_progress(line, sizeof(line), s_status.downloaded, s_status.total);
        ui_text_vc(fb, 342, 864, 19, line, EPD_DRAW_ALIGN_CENTER, false);
    } else if (s_status.message[0] || s_notice[0]) {
        copy_text(line, sizeof(line), s_notice[0] ? s_notice : s_status.message);
        fit_text(line, 18, 600);
        ui_text_vc(fb, 342, 864, 18, line, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_draw_button(fb, browse_back_rect(), "返回上一级", false);
    ui_draw_button(fb, browse_refresh_rect(), "刷新", false);
    ui_draw_button(fb, browse_servers_rect(), "服务器", false);

    if (s_pending_entry >= 0) {
        opds_entry_t entry;
        if (!opds_entry_get((uint16_t)s_pending_entry, &entry)) { s_pending_entry = -1; return; }
        ui_fill_round_rect(fb, (EpdRect){60, 430, 564, 300}, 24, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, (EpdRect){60, 430, 564, 300}, 24, 0x50);
        ui_text_vc(fb, 342, 490, 25, "下载这本书？", EPD_DRAW_ALIGN_CENTER, false);
        copy_text(line, sizeof(line), entry.title[0] ? entry.title : "(无标题)");
        fit_text(line, 20, 500);
        ui_text_vc(fb, 342, 540, 20, line, EPD_DRAW_ALIGN_CENTER, false);
        char size[24];
        format_size(size, sizeof(size), entry.length);
        if (size[0]) ui_text_vc(fb, 342, 580, 18, size, EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, confirm_download_rect(), "下载", true);
        ui_draw_button(fb, confirm_cancel_rect(), "取消", false);
    }
}

static void render_edit(uint8_t *fb) {
    render_header(fb, s_edit_index < 0 ? "新增服务器" : "编辑服务器");
    ui_text(fb, 36, 192, 19, "只支持 OPDS 1.2（Atom）；用户名和密码可留空", EPD_DRAW_ALIGN_LEFT, false);
    for (int field = 0; field < OPDS_FIELD_COUNT; ++field) {
        const EpdRect rect = edit_row_rect(field);
        ui_fill_round_rect(fb, rect, 18, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, rect, 18, 0x70);
        ui_text_vc(fb, rect.x + 22, rect.y + rect.height / 2, 21, edit_field_label(field),
                   EPD_DRAW_ALIGN_LEFT, false);
        char value[128];
        if (field == OPDS_FIELD_PASS) {
            copy_text(value, sizeof(value), s_edit_pass[0] ? "********" : "可留空");
        } else {
            copy_text(value, sizeof(value), edit_field_buffer(field));
            if (!value[0]) copy_text(value, sizeof(value), "未填写");
        }
        fit_text(value, 18, rect.width - 180);
        ui_text_vc(fb, rect.x + rect.width - 22, rect.y + rect.height / 2, 18, value,
                   EPD_DRAW_ALIGN_RIGHT, false);
        ui_draw_icon(fb, rect.x + rect.width - 12, rect.y + rect.height / 2, 22,
                     UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
    }
    if (s_notice[0]) ui_text_box(fb, (EpdRect){36, 596, 612, 36}, 18, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, edit_save_rect(), "保存", false);
    ui_draw_button(fb, edit_cancel_rect(), "取消", false);
}

static void render_field(uint8_t *fb) {
    char title[64];
    snprintf(title, sizeof(title), "编辑%s", edit_field_label(s_edit_field));
    render_header(fb, title);
    ui_text(fb, 642, 95, 24, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    ui_text(fb, 36, 201, 20, s_edit_field == OPDS_FIELD_PASS
                                ? "密码只保存在内存卡；编辑时不显示明文"
                                : "输入后点右上角「完成」返回",
            EPD_DRAW_ALIGN_LEFT, false);
    const bool masked = s_edit_field == OPDS_FIELD_PASS && !s_pass_visible;
    ui_text_input_draw(fb, &s_field_input, (EpdRect){36, 243, 612, 82}, 28, masked, NULL);
    if (s_edit_field == OPDS_FIELD_PASS) {
        const EpdRect toggle = field_toggle_rect();
        ui_draw_button(fb, toggle, s_pass_visible ? "隐藏密码" : "显示密码", false);
    }
    if (s_notice[0]) ui_text_fixed_vc(fb, 36, 516, 20, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    ui_keyboard_draw(fb, UI_SYSTEM_KEYBOARD_TOP);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    switch (s_view) {
        case OPDS_VIEW_BROWSE: render_browse(fb); break;
        case OPDS_VIEW_EDIT: render_edit(fb); break;
        case OPDS_VIEW_FIELD: render_field(fb); break;
        default: render_servers(fb); break;
    }
}

/* ---- 键盘局部刷新 / keyboard refresh ---------------------------------------- */
static app_redraw_t field_paint(app_ctx_t *ctx, bool field) {
    const ui_keyboard_update_t update = ui_keyboard_update(
        ctx->fb, UI_SYSTEM_KEYBOARD_TOP, (EpdRect){36, 243, 612, 82}, 28,
        s_edit_field == OPDS_FIELD_PASS && !s_pass_visible, NULL, field);
    s_input_area = update.area;
    s_input_layout = update.layout;
    s_input_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}

static bool present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_AREA && s_view == OPDS_VIEW_BROWSE && s_status.downloading) {
        // 跟随波形 + DU 只更新进度行，屏幕其余部分保持不动。
        // Follow waveform + DU updates only the progress line; the rest of the panel stays still.
        guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU,
                                                                s_progress_area));
        return true;
    }
    if (redraw != APP_REDRAW_AREA || s_view != OPDS_VIEW_FIELD) return false;
    if (s_input_settle) {
        guard_draw_result(ctx->hl, update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, s_input_area));
        s_input_settle = false;
        return true;
    }
    guard_draw_result(ctx->hl, update_display_area_diff_with(ctx->hl,
        s_input_layout ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
        s_input_layout ? MODE_GL16 : MODE_DU, s_input_area));
    return true;
}

/* ---- 触摸 / gestures --------------------------------------------------------- */
static app_redraw_t field_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    if (event->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(event->x0, event->y0, UI_SYSTEM_KEYBOARD_TOP, ctx->now_ms)
                   ? field_paint(ctx, false)
                   : APP_REDRAW_NONE;
    const bool feedback = event->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (event->type == UI_GESTURE_SWIPE_L || event->type == UI_GESTURE_SWIPE_R) {
        const bool page = ui_keyboard_page(event->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? field_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (event->type != UI_GESTURE_TAP)
        return feedback ? field_paint(ctx, false) : APP_REDRAW_NONE;
    const int x = event->x0, y = event->y0;
    if (y < 160) {
        if (x < 160) {
            ui_keyboard_end();
            memset(s_field_buf, 0, sizeof(s_field_buf));
            s_view = OPDS_VIEW_EDIT;
            return APP_REDRAW_PAGE;
        }
        if (x > 510) { field_commit(); return APP_REDRAW_PAGE; }
    }
    if (s_edit_field == OPDS_FIELD_PASS && ui_rect_hit(field_toggle_rect(), x, y)) {
        s_pass_visible = !s_pass_visible;
        return APP_REDRAW_PAGE;
    }
    const bool masked = s_edit_field == OPDS_FIELD_PASS && !s_pass_visible;
    if (ui_text_input_tap(&s_field_input, (EpdRect){36, 243, 612, 82}, 28, masked, NULL, x, y))
        return field_paint(ctx, true);
    const ui_keyboard_result_t result = ui_keyboard_tap(x, y, UI_SYSTEM_KEYBOARD_TOP, esp_timer_get_time() / 1000);
    if (result == UI_KEYBOARD_DONE) { field_commit(); return APP_REDRAW_PAGE; }
    return result == UI_KEYBOARD_CHANGED ? field_paint(ctx, false) : APP_REDRAW_NONE;
}

static app_redraw_t servers_tap(int x, int y) {
    if (s_delete_confirm) {
        if (ui_rect_hit(confirm_delete_rect(), x, y)) { server_delete_selected(); return APP_REDRAW_PAGE; }
        if (ui_rect_hit(confirm_delete_cancel_rect(), x, y)) { s_delete_confirm = false; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(server_add_rect(), x, y)) { edit_form_load(-1); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(server_edit_rect(), x, y)) {
        if (s_sel < s_cfg.count) edit_form_load(s_sel);
        else snprintf(s_notice, sizeof(s_notice), "还没有可编辑的服务器");
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(server_delete_rect(), x, y)) {
        if (s_sel < s_cfg.count) s_delete_confirm = true;
        else snprintf(s_notice, sizeof(s_notice), "还没有可删除的服务器");
        return APP_REDRAW_PAGE;
    }
    for (int row = 0; row < OPDS_ROWS && row < s_cfg.count; ++row) {
        if (!ui_rect_hit(server_row_rect(row), x, y)) continue;
        s_sel = row;
        browse_open_selected();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t browse_tap(int x, int y) {
    if (s_pending_entry >= 0) {
        if (ui_rect_hit(confirm_download_rect(), x, y)) {
            const esp_err_t err = opds_download((uint16_t)s_pending_entry);
            if (err != ESP_OK) snprintf(s_notice, sizeof(s_notice), "下载启动失败：%s", esp_err_to_name(err));
            else snprintf(s_notice, sizeof(s_notice), "正在下载…");
            s_pending_entry = -1;
            s_seen_revision = 0;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(confirm_cancel_rect(), x, y)) { s_pending_entry = -1; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (ui_rect_hit(browse_back_rect(), x, y)) {
        if (s_status.can_back) browse_back();
        else { opds_cancel_join(); s_view = OPDS_VIEW_SERVERS; s_notice[0] = 0; }
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(browse_refresh_rect(), x, y)) { browse_refresh(); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(browse_servers_rect(), x, y)) {
        opds_cancel_join();
        s_view = OPDS_VIEW_SERVERS;
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    }
    const uint16_t count = opds_entry_count();
    if (count > OPDS_BOOK_ROWS) {
        const int pages = (count + OPDS_BOOK_ROWS - 1) / OPDS_BOOK_ROWS;
        const int target = ui_pager_target(ui_pager_hit_at(x, y, OPDS_PAGER_TOP), s_entry_page, pages);
        if (target != s_entry_page) { s_entry_page = target; return APP_REDRAW_PAGE; }
    }
    for (int row = 0; row < OPDS_BOOK_ROWS; ++row) {
        const int index = s_entry_page * OPDS_BOOK_ROWS + row;
        if (index >= (int)count) break;
        if (!ui_rect_hit(browse_row_rect(row), x, y)) continue;
        browse_entry_tap(index);
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t edit_tap(int x, int y) {
    if (ui_rect_hit(edit_save_rect(), x, y)) { server_save_form(); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(edit_cancel_rect(), x, y)) {
        memset(s_edit_pass, 0, sizeof(s_edit_pass));
        s_view = OPDS_VIEW_SERVERS;
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    }
    for (int field = 0; field < OPDS_FIELD_COUNT; ++field) {
        if (!ui_rect_hit(edit_row_rect(field), x, y)) continue;
        field_open(field);
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t go_back(app_ctx_t *ctx) {
    switch (s_view) {
        case OPDS_VIEW_FIELD:
            ui_keyboard_end();
            memset(s_field_buf, 0, sizeof(s_field_buf));
            s_view = OPDS_VIEW_EDIT;
            return APP_REDRAW_PAGE;
        case OPDS_VIEW_EDIT:
            memset(s_edit_pass, 0, sizeof(s_edit_pass));
            s_view = OPDS_VIEW_SERVERS;
            return APP_REDRAW_PAGE;
        case OPDS_VIEW_BROWSE:
            if (s_pending_entry >= 0) { s_pending_entry = -1; return APP_REDRAW_PAGE; }
            if (s_status.can_back) { browse_back(); return APP_REDRAW_PAGE; }
            ctx->request_return = true;
            return APP_REDRAW_NONE;
        default:
            ctx->request_return = true;
            return APP_REDRAW_NONE;
    }
}

static EpdRect page_area_hint(app_ctx_t *ctx) {
    (void)ctx;
    return s_progress_area;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    if (s_view == OPDS_VIEW_FIELD) return field_gesture(ctx, event);
    if (s_status.downloading) {
        // 下载进度自动局部刷新，其它手势一律忽略，避免误触离开页面。
        // Progress updates automatically; ignore gestures so the page is not left by accident.
        return APP_REDRAW_NONE;
    }
    if (event->type == UI_GESTURE_SWIPE_R) return go_back(ctx);
    if (event->type == UI_GESTURE_LONG_PRESS) {
        if (s_view == OPDS_VIEW_SERVERS) {
            for (int row = 0; row < OPDS_ROWS && row < s_cfg.count; ++row) {
                if (!ui_rect_hit(server_row_rect(row), event->x0, event->y0)) continue;
                s_sel = row;
                edit_form_load(row);
                return APP_REDRAW_PAGE;
            }
        }
        return APP_REDRAW_NONE;
    }
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (event->y0 < 160 && event->x0 < 160) return go_back(ctx);
    switch (s_view) {
        case OPDS_VIEW_BROWSE: return browse_tap(event->x0, event->y0);
        case OPDS_VIEW_EDIT: return edit_tap(event->x0, event->y0);
        default: return servers_tap(event->x0, event->y0);
    }
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    return go_back(ctx);
}

/* ---- 生命周期 / lifecycle ---------------------------------------------------- */
static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_view = OPDS_VIEW_SERVERS;
    s_notice[0] = 0;
    s_sel = 0;
    s_entry_page = 0;
    s_pending_entry = -1;
    s_delete_confirm = false;
    s_current_url[0] = 0;
    s_last_download[0] = 0;
    memset(&s_status, 0, sizeof(s_status));
    memset(s_edit_pass, 0, sizeof(s_edit_pass));
    config_load_from_card();
    opds_get_status(&s_status);
    s_seen_revision = s_status.revision;
}

static void page_exit(app_ctx_t *ctx) {
    (void)ctx;
    ui_keyboard_end();
    memset(s_field_buf, 0, sizeof(s_field_buf));
    memset(s_edit_pass, 0, sizeof(s_edit_pass));
    opds_cancel_join();
    opds_close();
}

static void on_before_lock(app_ctx_t *ctx) {
    (void)ctx;
    ui_keyboard_end();
    memset(s_field_buf, 0, sizeof(s_field_buf));
    memset(s_edit_pass, 0, sizeof(s_edit_pass));
    opds_cancel_join();
    opds_close();
}

static void on_media_lost(app_ctx_t *ctx) {
    (void)ctx;
    ui_keyboard_end();
    memset(s_field_buf, 0, sizeof(s_field_buf));
    opds_cancel_join();
    opds_close();
    s_pending_entry = -1;
    s_delete_confirm = false;
    snprintf(s_notice, sizeof(s_notice), "内存卡已失效，请插卡后重试");
}

static void on_media_ready(app_ctx_t *ctx) {
    (void)ctx;
    config_load_from_card();
    s_notice[0] = 0;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (s_view == OPDS_VIEW_FIELD) {
        const bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
                                  ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED)
            return field_paint(ctx, false);
        if (ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED)
            return field_paint(ctx, false);
        return APP_REDRAW_NONE;
    }
    if (opds_take_store_changed()) book_store_notify_changed();
    opds_status_t status;
    opds_get_status(&status);
    bool changed = false;
    if (status.revision != s_seen_revision) {
        s_seen_revision = status.revision;
        s_status = status;
        changed = true;
        if (status.downloaded_path[0] && strcmp(status.downloaded_path, s_last_download)) {
            copy_text(s_last_download, sizeof(s_last_download), status.downloaded_path);
            const char *base = strrchr(status.downloaded_path, '/');
            snprintf(s_notice, sizeof(s_notice), "已保存：%s",
                     base ? base + 1 : status.downloaded_path);
        }
    }
    if (!changed) return APP_REDRAW_NONE;
    if (s_status.downloading) {
        // 每跨过 5% 才推一次底部进度行，其余时间不碰屏幕。
        // Push the bottom progress line only when a new 5% bucket is reached.
        const unsigned percent = s_status.total
            ? (unsigned)((uint64_t)s_status.downloaded * 100u / s_status.total) : 0u;
        const unsigned bucket = percent / 5u;
        if (bucket != s_download_bucket) {
            s_download_bucket = bucket;
            s_progress_area = (EpdRect){140, 842, 404, 40};
            render_browse(ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }
    s_download_bucket = 0xFFFFu;
    // 读取目录时只刷新“开始/结束”，中间的连接、读取文案变化不再重复整页刷。
    // During a catalog load only the start and finish refresh; interim messages stay static.
    if (s_status.busy && s_busy_shown) return APP_REDRAW_NONE;
    s_busy_shown = s_status.busy;
    return APP_REDRAW_PAGE;
}

static bool page_navigation_visible(app_ctx_t *ctx) {
    (void)ctx;
    return false;  /* 自定义操作栏，内容覆盖到页面底部。*/
}

static bool page_edge_back(app_ctx_t *ctx) {
    (void)ctx;
    return !opds_busy();
}

static bool page_back_swipe_area(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    (void)ctx;
    return s_view != OPDS_VIEW_FIELD || event->y0 < UI_SYSTEM_KEYBOARD_TOP;
}

const app_desc_t app_opds = {
    .title = "OPDS 书库",
    .detail = "浏览 OPDS 目录并下载 EPUB",
    .enter_full = true,
    .binary_ui = false,
    .persistent_navigation = true,
    .navigation_visible = page_navigation_visible,
    .edge_back_enabled = page_edge_back,
    .back_swipe_area = page_back_swipe_area,
    .on_enter = on_enter,
    .on_exit = page_exit,
    .on_media_lost = on_media_lost,
    .on_media_ready = on_media_ready,
    .on_before_lock = on_before_lock,
    .render = render,
    .present = present,
    .on_tick = on_tick,
    .area_hint = page_area_hint,
    .on_gesture = on_gesture,
    .on_key = on_key,
};
