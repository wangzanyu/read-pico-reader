/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：KOReader 进度同步页面。页内编辑服务器/账号/密码/匹配方式，列出最近有进度的 EPUB，
 *       拉取比较或上传本机进度；同步任务在 book_kosync 里跑，页面只轮询状态快照。
 * English: KOReader progress-sync page. Edits server/account/password/match in place, lists
 *       recently-read EPUBs, fetches/compares or uploads progress. The worker lives in
 *       book_kosync; this page only polls the status snapshot.
 *
 * 冻结：只经 kosync_* 接口联网，不在此页直接发 HTTP；render 只画快照；
 *       离页/锁屏/卸卡先 cancel_join，再 close；不静默覆盖本地阅读进度。
 * Frozen: network IO only through kosync_*; render paints snapshots only; leave/lock/media-loss
 *       cancel and join before close; local progress is never overwritten silently.
 */

#ifdef ESP_PLATFORM
#include "ui_locale_format.h"
#endif
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "app.h"
#include "book_kosync.h"
#include "book_progress.h"
#include "book_store.h"
#include "book_title.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "ui_gesture.h"
#include "ui_keyboard.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_text_input.h"

#define KOSYNC_BOOKS_MAX 20
#define KOSYNC_BOOK_ROWS 4
#define KOSYNC_BOOK_TOP 452
#define KOSYNC_BOOK_STEP 110
#define KOSYNC_BOOK_H 100
#define KOSYNC_PAGER_TOP 890

#define KOSYNC_FIELD_SERVER 0
#define KOSYNC_FIELD_USER 1
#define KOSYNC_FIELD_PASS 2
#define KOSYNC_FIELD_MATCH 3
#define KOSYNC_FIELD_COUNT 4

typedef enum {
    KOSYNC_VIEW_LIST = 0,
    KOSYNC_VIEW_CONFIG,
    KOSYNC_VIEW_EDIT,
} kosync_view_t;

typedef struct {
    char path[KOSYNC_PATH_MAX];
    char title[KOSYNC_TITLE_MAX];
    uint8_t pct;
    uint32_t last_open;
} kosync_book_item_t;

static kosync_view_t s_view = KOSYNC_VIEW_LIST;
static kosync_config_t s_cfg;
static kosync_status_t s_status;
static unsigned s_seen_revision;
static uint32_t s_seen_progress;
/* 20 本书目约 9 KiB，放 PSRAM，保住内部 RAM 给 TLS。/ ~9 KiB of book metadata lives in PSRAM to keep internal RAM for TLS. */
static EXT_RAM_BSS_ATTR kosync_book_item_t s_books[KOSYNC_BOOKS_MAX];
static int s_book_count, s_book_sel, s_book_page;
static char s_notice[96];

static int s_edit_field;
static char s_edit_buf[KOSYNC_SERVER_MAX];
static ui_text_edit_t s_edit_input;
static bool s_pass_visible;
static EpdRect s_input_area;
static bool s_input_layout, s_input_settle;

/* ---- 布局 / layout ----------------------------------------------------------- */
static EpdRect book_rect(int row) {
    return (EpdRect){36, KOSYNC_BOOK_TOP + row * KOSYNC_BOOK_STEP, 612, KOSYNC_BOOK_H};
}
static EpdRect sync_button(void) { return (EpdRect){36, 960, 196, 64}; }
static EpdRect upload_button(void) { return (EpdRect){242, 960, 196, 64}; }
static EpdRect apply_button(void) { return (EpdRect){448, 960, 200, 64}; }
static EpdRect edit_button(void) { return (EpdRect){36, 1028, 196, 64}; }
static EpdRect verify_button(void) { return (EpdRect){242, 1028, 196, 64}; }
static EpdRect config_row_rect(int field) { return (EpdRect){36, 236 + field * 96, 612, 84}; }
static EpdRect config_save_rect(void) { return (EpdRect){36, 640, 296, 72}; }
static EpdRect config_cancel_rect(void) { return (EpdRect){352, 640, 296, 72}; }
static EpdRect password_toggle_rect(void) { return (EpdRect){36, 430, 200, 64}; }

/* ---- 文本 / 配置字段 / text and config fields -------------------------------- */
static void copy_text(char *dst, size_t cap, const char *src) {
    if (!dst || !cap) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

static const char *field_label(int field) {
    switch (field) {
        case KOSYNC_FIELD_SERVER: return "服务器";
        case KOSYNC_FIELD_USER: return "用户名";
        case KOSYNC_FIELD_PASS: return "密码";
        default: return "文档匹配";
    }
}

static const char *field_value(int field) {
    switch (field) {
        case KOSYNC_FIELD_SERVER: return s_cfg.server;
        case KOSYNC_FIELD_USER: return s_cfg.username;
        case KOSYNC_FIELD_PASS: return s_cfg.password;
        default: return s_cfg.match == KOSYNC_MATCH_FILENAME ? "文件名 MD5" : "内容 partial MD5";
    }
}

static void field_store(int field, const char *value) {
    switch (field) {
        case KOSYNC_FIELD_SERVER: copy_text(s_cfg.server, sizeof(s_cfg.server), value); break;
        case KOSYNC_FIELD_USER: copy_text(s_cfg.username, sizeof(s_cfg.username), value); break;
        case KOSYNC_FIELD_PASS: copy_text(s_cfg.password, sizeof(s_cfg.password), value); break;
        default: break;
    }
}

static void config_load_from_card(void) {
    esp_err_t err = kosync_config_load(&s_cfg);
    if (err == ESP_ERR_NOT_FOUND) return;  /* 缺配置时 load 已填默认服务器/匹配方式 */
    if (err != ESP_OK) snprintf(s_notice, sizeof(s_notice), "读取配置失败：%s", esp_err_to_name(err));
}

static bool config_ready(void) { return kosync_config_ready(&s_cfg); }

static void config_host(char *out, size_t cap) {
    const char *server = s_cfg.server;
    const char *scheme = strstr(server, "://");
    if (scheme) server = scheme + 3;
    size_t length = strcspn(server, "/?#");
    if (length >= cap) length = cap - 1;
    memcpy(out, server, length);
    out[length] = 0;
    if (!out[0]) copy_text(out, cap, "未配置");
}

/* ---- 书籍列表 / book list ---------------------------------------------------- */
static bool book_visit(const char *path, const book_progress_t *progress, void *ctx) {
    (void)ctx;
    if (!path || !path[0] || !progress) return true;
    const char *ext = strrchr(path, '.');
    if (!ext || strcasecmp(ext, ".epub")) return true;  /* kosync 只同步 EPUB */
    if (s_book_count >= KOSYNC_BOOKS_MAX) return false;
    kosync_book_item_t *item = &s_books[s_book_count];
    memset(item, 0, sizeof(*item));
    copy_text(item->path, sizeof(item->path), path);
    if (!book_display_name_from_path(path, item->title, sizeof(item->title), NULL, 0)) {
        const char *base = strrchr(path, '/');
        copy_text(item->title, sizeof(item->title), base ? base + 1 : path);
    }
    item->pct = progress->pct;
    item->last_open = progress->last_open_s;
    ++s_book_count;
    return true;
}

static void books_reload(void) {
    s_book_count = 0;
    if (book_progress_list(book_visit, NULL) != ESP_OK && !s_notice[0])
        snprintf(s_notice, sizeof(s_notice), "读取阅读记录失败");
    for (int i = 1; i < s_book_count; ++i)
        for (int j = i; j > 0 && s_books[j].last_open > s_books[j - 1].last_open; --j) {
            kosync_book_item_t item = s_books[j];
            s_books[j] = s_books[j - 1];
            s_books[j - 1] = item;
        }
    char latest[KOSYNC_PATH_MAX];
    if (book_progress_last_path(latest, sizeof(latest)))
        for (int i = 0; i < s_book_count; ++i)
            if (!strcmp(s_books[i].path, latest)) { s_book_sel = i; break; }
    if (s_book_sel >= s_book_count) s_book_sel = s_book_count ? s_book_count - 1 : 0;
    if (s_book_sel < 0) s_book_sel = 0;
    const int pages = s_book_count ? (s_book_count + KOSYNC_BOOK_ROWS - 1) / KOSYNC_BOOK_ROWS : 1;
    s_book_page = s_book_sel / KOSYNC_BOOK_ROWS;
    if (s_book_page >= pages) s_book_page = pages - 1;
}

/* ---- 操作 / actions ---------------------------------------------------------- */
static const char *selected_book_path(void) {
    return s_book_count ? s_books[s_book_sel].path : NULL;
}

static void notice_error(esp_err_t err) {
    if (err == ESP_OK) return;
    snprintf(s_notice, sizeof(s_notice), "%s", esp_err_to_name(err));
}

static void action_sync(void) {
    if (kosync_busy()) { snprintf(s_notice, sizeof(s_notice), "同步任务进行中，请稍候"); return; }
    if (!config_ready()) { snprintf(s_notice, sizeof(s_notice), "请先编辑并保存服务器、用户名与密码"); s_view = KOSYNC_VIEW_CONFIG; return; }
    notice_error(kosync_start(selected_book_path()));
    s_seen_revision = 0;
    snprintf(s_notice, sizeof(s_notice), "正在同步…");
}

static void action_upload(void) {
    if (kosync_busy()) { snprintf(s_notice, sizeof(s_notice), "同步任务进行中，请稍候"); return; }
    if (!config_ready()) { snprintf(s_notice, sizeof(s_notice), "请先编辑并保存服务器、用户名与密码"); s_view = KOSYNC_VIEW_CONFIG; return; }
    notice_error(kosync_upload(selected_book_path()));
    s_seen_revision = 0;
    snprintf(s_notice, sizeof(s_notice), "正在上传本机进度…");
}

static void action_apply(void) {
    if (kosync_busy()) return;
    if (s_status.result != KOSYNC_REMOTE_AHEAD || !s_status.has_remote) {
        snprintf(s_notice, sizeof(s_notice), "云端没有可应用的进度");
        return;
    }
    esp_err_t err = kosync_apply_remote();
    if (err == ESP_OK) snprintf(s_notice, sizeof(s_notice), "已应用云端进度，下次开书生效");
    else notice_error(err);
    s_seen_revision = 0;
}

static void action_verify(void) {
    if (kosync_busy()) { snprintf(s_notice, sizeof(s_notice), "同步任务进行中，请稍候"); return; }
    if (!config_ready()) { snprintf(s_notice, sizeof(s_notice), "请先编辑并保存服务器、用户名与密码"); s_view = KOSYNC_VIEW_CONFIG; return; }
    notice_error(kosync_verify());
    s_seen_revision = 0;
    snprintf(s_notice, sizeof(s_notice), "正在验证账号…");
}

/* ---- 视图切换 / view transitions -------------------------------------------- */
static void leave_keyboard(void) {
    ui_keyboard_end();
    memset(s_edit_buf, 0, sizeof(s_edit_buf));
    s_pass_visible = false;
}

static void edit_open(int field) {
    s_edit_field = field;
    copy_text(s_edit_buf, sizeof(s_edit_buf), field_value(field));
    ui_text_edit_init(&s_edit_input, s_edit_buf, sizeof(s_edit_buf));
    s_pass_visible = false;
    ui_keyboard_begin(&s_edit_input, true);  /* 服务器/账号/密码都是 ASCII */
    s_view = KOSYNC_VIEW_EDIT;
}

static void edit_commit(void) {
    if (ui_keyboard_pending()) { snprintf(s_notice, sizeof(s_notice), "请先选择候选字"); return; }
    if (s_edit_field != KOSYNC_FIELD_PASS && !s_edit_buf[0]) {
        snprintf(s_notice, sizeof(s_notice), "%s不能为空", field_label(s_edit_field));
        return;
    }
    ui_keyboard_end();
    if (s_edit_field == KOSYNC_FIELD_PASS) {
        /* 密码只进配置结构；编辑缓冲立即清零。/ Store the password in config only, then wipe the edit buffer. */
        copy_text(s_cfg.password, sizeof(s_cfg.password), s_edit_buf);
        memset(s_edit_buf, 0, sizeof(s_edit_buf));
    } else {
        field_store(s_edit_field, s_edit_buf);
        memset(s_edit_buf, 0, sizeof(s_edit_buf));
    }
    s_view = KOSYNC_VIEW_CONFIG;
    snprintf(s_notice, sizeof(s_notice), "已修改，点「保存」写入内存卡");
}

static app_redraw_t go_back(app_ctx_t *ctx) {
    if (s_view == KOSYNC_VIEW_EDIT) {
        leave_keyboard();
        s_view = KOSYNC_VIEW_CONFIG;
        return APP_REDRAW_PAGE;
    }
    if (s_view == KOSYNC_VIEW_CONFIG) {
        leave_keyboard();
        config_load_from_card();  /* 放弃未保存改动 */
        s_view = KOSYNC_VIEW_LIST;
        return APP_REDRAW_PAGE;
    }
    ctx->request_return = true;
    return APP_REDRAW_NONE;
}

/* ---- 绘制 / rendering -------------------------------------------------------- */
static void render_header(uint8_t *fb, const char *title) {
    ui_clear_page(fb);
    ui_nav_status(fb);
    ui_nav_back(fb, 36, 79);
    ui_text_vc(fb, 342, 107, 34, title, EPD_DRAW_ALIGN_CENTER, false);
    ui_hairline(fb, 174, 36, 612, UI_GRAY_LIGHT);
}

static void render_list(uint8_t *fb) {
    render_header(fb, "KOReader 同步");

    char host[KOSYNC_SERVER_MAX], line[160];
    config_host(host, sizeof(host));
    ui_fill_round_rect(fb, (EpdRect){36, 190, 612, 96}, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, (EpdRect){36, 190, 612, 96}, 20, 0x70);
    snprintf(line, sizeof(line), "服务器：%s", host);
    ui_text_vc(fb, 60, 216, 22, line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "账号：%s · 匹配：%s",
             s_cfg.username[0] ? s_cfg.username : "未配置",
             s_cfg.match == KOSYNC_MATCH_FILENAME ? "文件名" : "内容");
    ui_text_vc(fb, 60, 254, 20, line, EPD_DRAW_ALIGN_LEFT, false);

    /* 状态卡片 / status card */
    ui_fill_round_rect(fb, (EpdRect){36, 300, 612, 120}, 20, UI_GRAY_WHITE);
    ui_draw_control_frame(fb, (EpdRect){36, 300, 612, 120}, 20, 0x70);
    const char *message;
    if (s_notice[0]) message = s_notice;
    else if (!config_ready()) message = "未配置：点「编辑配置」填写服务器、用户名与密码";
    else if (s_status.message[0]) message = s_status.message;
    else message = "点选书籍后同步或上传";
    ui_text_box(fb, (EpdRect){58, 306, 568, 42}, 21, message, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "本机 %u%% · 云端 %u%%%s%s",
             (unsigned)(s_status.local_percent * 100.0f + 0.5f),
             (unsigned)(s_status.remote_percent * 100.0f + 0.5f),
             s_status.remote_device[0] ? " · 设备：" : "",
             s_status.remote_device);
    ui_text_vc(fb, 58, 372, 20, line, EPD_DRAW_ALIGN_LEFT, false);
    if (s_status.result == KOSYNC_REMOTE_AHEAD && s_status.has_remote)
        ui_text_vc(fb, 58, 404, 19, "云端更新：可点「应用云端进度」", EPD_DRAW_ALIGN_LEFT, false);
    else if (s_status.result == KOSYNC_LOCAL_AHEAD)
        ui_text_vc(fb, 58, 404, 19, "本机更新：可点「上传本机」", EPD_DRAW_ALIGN_LEFT, false);
    else if (s_status.result == KOSYNC_SYNCED)
        ui_text_vc(fb, 58, 404, 19, "两侧进度已一致", EPD_DRAW_ALIGN_LEFT, false);

    snprintf(line, sizeof(line), "有进度的书籍（%d）", s_book_count);
    ui_text(fb, 42, 426, 20, line, EPD_DRAW_ALIGN_LEFT, false);
    if (!s_book_count) {
        ui_text_vc(fb, 342, 620, 22, "还没有 EPUB 阅读进度", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        for (int row = 0; row < KOSYNC_BOOK_ROWS; ++row) {
            const int index = s_book_page * KOSYNC_BOOK_ROWS + row;
            if (index >= s_book_count) break;
            const kosync_book_item_t *item = &s_books[index];
            const EpdRect rect = book_rect(row);
            const bool selected = index == s_book_sel;
            ui_fill_round_rect(fb, rect, 18, selected ? 0xee : UI_GRAY_WHITE);
            ui_draw_control_frame(fb, rect, 18, selected ? 0x40 : 0x80);
            char title[KOSYNC_TITLE_MAX];
            copy_text(title, sizeof(title), item->title);
            int width = rect.width - 150;
            while (title[0] && ui_text_fixed_width_px(20, title) > width) {
                size_t n = strlen(title) - 1;
                while (n && ((unsigned char)title[n] & 0xc0) == 0x80) --n;
                title[n] = 0;
            }
            ui_text_vc(fb, rect.x + 22, rect.y + rect.height / 2, 20, title, EPD_DRAW_ALIGN_LEFT, false);
            char meta[24];
            snprintf(meta, sizeof(meta), "%u%%", (unsigned)item->pct);
            ui_text_vc(fb, rect.x + rect.width - 26, rect.y + rect.height / 2, 20, meta,
                       EPD_DRAW_ALIGN_RIGHT, false);
        }
        const int pages = (s_book_count + KOSYNC_BOOK_ROWS - 1) / KOSYNC_BOOK_ROWS;
        char label[48];
        snprintf(label, sizeof(label), "%d/%d · 共%d本", s_book_page + 1, pages, s_book_count);
        ui_pager_draw_at(fb, label, s_book_page, pages, false, -1, KOSYNC_PAGER_TOP);
    }

    const bool busy = kosync_busy();
    ui_draw_button(fb, sync_button(), busy ? "同步中…" : "同步", false);
    ui_draw_button(fb, upload_button(), "上传本机", false);
    ui_draw_button(fb, apply_button(), "应用云端", false);
    ui_draw_button(fb, edit_button(), "编辑配置", false);
    ui_draw_button(fb, verify_button(), busy ? "请稍候…" : "验证账号", false);
}

static void render_config(uint8_t *fb) {
    render_header(fb, "同步设置");
    ui_text(fb, 36, 196, 20, "服务器、账号与密码保存在内存卡，不写入系统设置备份",
            EPD_DRAW_ALIGN_LEFT, false);
    for (int field = 0; field < KOSYNC_FIELD_COUNT; ++field) {
        const EpdRect rect = config_row_rect(field);
        ui_fill_round_rect(fb, rect, 18, UI_GRAY_WHITE);
        ui_draw_control_frame(fb, rect, 18, 0x70);
        ui_text_vc(fb, rect.x + 22, rect.y + rect.height / 2, 22, field_label(field),
                   EPD_DRAW_ALIGN_LEFT, false);
        char value[96];
        if (field == KOSYNC_FIELD_PASS) {
            copy_text(value, sizeof(value), s_cfg.password[0] ? "********" : "未设置");
        } else {
            copy_text(value, sizeof(value), field_value(field));
        }
        int width = rect.width - 180;
        while (value[0] && ui_text_fixed_width_px(18, value) > width) {
            size_t n = strlen(value) - 1;
            while (n && ((unsigned char)value[n] & 0xc0) == 0x80) --n;
            value[n] = 0;
        }
        ui_text_vc(fb, rect.x + rect.width - 22, rect.y + rect.height / 2, 18, value,
                   EPD_DRAW_ALIGN_RIGHT, false);
        if (field < KOSYNC_FIELD_MATCH)
            ui_draw_icon(fb, rect.x + rect.width - 12, rect.y + rect.height / 2, 22,
                         UI_ICON_CHEVRON_RIGHT, UI_GRAY_BLACK);
    }
    if (s_notice[0]) ui_text_box(fb, (EpdRect){36, 596, 612, 36}, 18, s_notice, EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, config_save_rect(), "保存", false);
    ui_draw_button(fb, config_cancel_rect(), "取消", false);
}

static void render_edit(uint8_t *fb) {
    char title[48];
    snprintf(title, sizeof(title), "编辑%s", field_label(s_edit_field));
    render_header(fb, title);
    ui_text(fb, 642, 95, 24, "完成", EPD_DRAW_ALIGN_RIGHT, false);
    ui_text(fb, 36, 201, 20, s_edit_field == KOSYNC_FIELD_PASS
                                ? "密码只保存在内存卡；编辑时不显示明文"
                                : "输入后点右上角「完成」返回",
            EPD_DRAW_ALIGN_LEFT, false);
    const bool masked = s_edit_field == KOSYNC_FIELD_PASS && !s_pass_visible;
    ui_text_input_draw(fb, &s_edit_input, (EpdRect){36, 243, 612, 82}, 28, masked, NULL);
    if (s_edit_field == KOSYNC_FIELD_PASS) {
        const EpdRect toggle = password_toggle_rect();
        ui_draw_button(fb, toggle, s_pass_visible ? "隐藏密码" : "显示密码", false);
    }
    if (s_notice[0]) ui_text_fixed_vc(fb, 36, 516, 20, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    ui_keyboard_draw(fb, UI_SYSTEM_KEYBOARD_TOP);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    switch (s_view) {
        case KOSYNC_VIEW_CONFIG: render_config(fb); break;
        case KOSYNC_VIEW_EDIT: render_edit(fb); break;
        default: render_list(fb); break;
    }
}

/* ---- 键盘局部刷新 / keyboard refresh ---------------------------------------- */
static app_redraw_t edit_paint(app_ctx_t *ctx, bool field) {
    const ui_keyboard_update_t update = ui_keyboard_update(
        ctx->fb, UI_SYSTEM_KEYBOARD_TOP, (EpdRect){36, 243, 612, 82}, 28,
        s_edit_field == KOSYNC_FIELD_PASS && !s_pass_visible, NULL, field);
    s_input_area = update.area;
    s_input_layout = update.layout;
    s_input_settle = update.settle;
    return update.area.width ? APP_REDRAW_AREA : APP_REDRAW_NONE;
}

static bool present(app_ctx_t *ctx, app_redraw_t redraw) {
    if (redraw != APP_REDRAW_AREA || s_view != KOSYNC_VIEW_EDIT) return false;
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
static app_redraw_t edit_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    if (event->type == UI_GESTURE_PRESS)
        return ui_keyboard_press(event->x0, event->y0, UI_SYSTEM_KEYBOARD_TOP, ctx->now_ms)
                   ? edit_paint(ctx, false)
                   : APP_REDRAW_NONE;
    const bool feedback = event->type != UI_GESTURE_LONG_PRESS && ui_keyboard_release();
    if (event->type == UI_GESTURE_SWIPE_L || event->type == UI_GESTURE_SWIPE_R) {
        const bool page = ui_keyboard_page(event->type == UI_GESTURE_SWIPE_L ? 1 : -1);
        return (page || feedback) ? edit_paint(ctx, false) : APP_REDRAW_NONE;
    }
    if (event->type != UI_GESTURE_TAP)
        return feedback ? edit_paint(ctx, false) : APP_REDRAW_NONE;

    const int x = event->x0, y = event->y0;
    if (y < 160) {
        if (x < 160) {
            leave_keyboard();
            s_view = KOSYNC_VIEW_CONFIG;
            return APP_REDRAW_PAGE;
        }
        if (x > 510) { edit_commit(); return APP_REDRAW_PAGE; }
    }
    if (s_edit_field == KOSYNC_FIELD_PASS && ui_rect_hit(password_toggle_rect(), x, y)) {
        s_pass_visible = !s_pass_visible;
        return APP_REDRAW_PAGE;
    }
    const bool masked = s_edit_field == KOSYNC_FIELD_PASS && !s_pass_visible;
    if (ui_text_input_tap(&s_edit_input, (EpdRect){36, 243, 612, 82}, 28, masked, NULL, x, y))
        return edit_paint(ctx, true);
    const ui_keyboard_result_t result = ui_keyboard_tap(x, y, UI_SYSTEM_KEYBOARD_TOP, esp_timer_get_time() / 1000);
    if (result == UI_KEYBOARD_DONE) { edit_commit(); return APP_REDRAW_PAGE; }
    return result == UI_KEYBOARD_CHANGED ? edit_paint(ctx, false) : APP_REDRAW_NONE;
}

static app_redraw_t list_tap(app_ctx_t *ctx, int x, int y) {
    (void)ctx;
    if (ui_rect_hit(sync_button(), x, y)) { action_sync(); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(upload_button(), x, y)) { action_upload(); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(apply_button(), x, y)) { action_apply(); return APP_REDRAW_PAGE; }
    if (ui_rect_hit(edit_button(), x, y)) {
        config_load_from_card();
        s_notice[0] = 0;
        s_view = KOSYNC_VIEW_CONFIG;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(verify_button(), x, y)) { action_verify(); return APP_REDRAW_PAGE; }

    if (s_book_count) {
        const int pages = (s_book_count + KOSYNC_BOOK_ROWS - 1) / KOSYNC_BOOK_ROWS;
        const int target = ui_pager_target(ui_pager_hit_at(x, y, KOSYNC_PAGER_TOP), s_book_page, pages);
        if (target != s_book_page) { s_book_page = target; return APP_REDRAW_PAGE; }
    }
    for (int row = 0; row < KOSYNC_BOOK_ROWS; ++row) {
        const int index = s_book_page * KOSYNC_BOOK_ROWS + row;
        if (index >= s_book_count) break;
        if (!ui_rect_hit(book_rect(row), x, y)) continue;
        s_book_sel = index;
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t config_tap(int x, int y) {
    if (ui_rect_hit(config_save_rect(), x, y)) {
        const esp_err_t err = kosync_config_save(&s_cfg);
        if (err == ESP_OK) {
            snprintf(s_notice, sizeof(s_notice), "配置已保存");
            s_view = KOSYNC_VIEW_LIST;
        } else {
            snprintf(s_notice, sizeof(s_notice), "保存失败：%s", esp_err_to_name(err));
        }
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(config_cancel_rect(), x, y)) {
        config_load_from_card();
        s_notice[0] = 0;
        s_view = KOSYNC_VIEW_LIST;
        return APP_REDRAW_PAGE;
    }
    for (int field = 0; field < KOSYNC_FIELD_COUNT; ++field) {
        if (!ui_rect_hit(config_row_rect(field), x, y)) continue;
        if (field == KOSYNC_FIELD_MATCH) {
            s_cfg.match = s_cfg.match == KOSYNC_MATCH_FILENAME ? KOSYNC_MATCH_BINARY : KOSYNC_MATCH_FILENAME;
            snprintf(s_notice, sizeof(s_notice), "匹配方式已切换，点「保存」写入内存卡");
            return APP_REDRAW_PAGE;
        }
        edit_open(field);
        s_notice[0] = 0;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    if (s_view == KOSYNC_VIEW_EDIT) return edit_gesture(ctx, event);
    if (event->type == UI_GESTURE_SWIPE_R) return go_back(ctx);
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    if (event->y0 < 160 && event->x0 < 160) return go_back(ctx);
    if (s_view == KOSYNC_VIEW_CONFIG) return config_tap(event->x0, event->y0);
    return list_tap(ctx, event->x0, event->y0);
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    return go_back(ctx);
}

/* ---- 生命周期 / lifecycle ---------------------------------------------------- */
static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_view = KOSYNC_VIEW_LIST;
    s_notice[0] = 0;
    s_book_sel = 0;
    s_book_page = 0;
    s_pass_visible = false;
    s_edit_buf[0] = 0;
    memset(&s_status, 0, sizeof(s_status));
    config_load_from_card();
    books_reload();
    kosync_get_status(&s_status);
    s_seen_revision = s_status.revision;
    s_seen_progress = book_progress_revision();
}

static void page_exit(app_ctx_t *ctx) {
    (void)ctx;
    leave_keyboard();
    kosync_cancel_join();
    kosync_close();
}

static void on_before_lock(app_ctx_t *ctx) {
    (void)ctx;
    leave_keyboard();
    kosync_cancel_join();
    kosync_close();
}

static void on_media_lost(app_ctx_t *ctx) {
    (void)ctx;
    leave_keyboard();
    kosync_cancel_join();
    kosync_close();
    s_notice[0] = 0;
    books_reload();
}

static void on_media_ready(app_ctx_t *ctx) {
    (void)ctx;
    books_reload();
    config_load_from_card();
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (s_view == KOSYNC_VIEW_EDIT) {
        const bool held = !ctx->released && ctx->touch && ctx->touch->touched && ctx->touch->count == 1;
        if (ui_keyboard_hold_tick(held, ctx->touch ? ctx->touch->x : 0,
                                  ctx->touch ? ctx->touch->y : 0, ctx->now_ms) == UI_KEYBOARD_CHANGED)
            return edit_paint(ctx, false);
        if (ui_keyboard_idle_tick(ctx->touch && ctx->touch->touched, ctx->now_ms) == UI_KEYBOARD_CHANGED)
            return edit_paint(ctx, false);
        return APP_REDRAW_NONE;
    }
    bool changed = false;
    kosync_status_t status;
    kosync_get_status(&status);
    if (status.revision != s_seen_revision) {
        s_seen_revision = status.revision;
        s_status = status;
        changed = true;
    }
    const uint32_t progress = book_progress_revision();
    if (progress != s_seen_progress) {
        s_seen_progress = progress;
        books_reload();
        changed = true;
    }
    return changed ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
}

static bool page_navigation_visible(app_ctx_t *ctx) {
    (void)ctx;
    return false;  /* 本页不画底栏，内容/刷新覆盖到页面底部。*/
}

static bool page_edge_back(app_ctx_t *ctx) {
    (void)ctx;
    return !kosync_busy();
}

static bool page_back_swipe_area(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    (void)ctx;
    return s_view != KOSYNC_VIEW_EDIT || event->y0 < UI_SYSTEM_KEYBOARD_TOP;
}

const app_desc_t app_kosync = {
    .title = "KOReader 同步",
    .detail = "kosync 进度同步（EPUB）",
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
    .on_gesture = on_gesture,
    .on_key = on_key,
};
