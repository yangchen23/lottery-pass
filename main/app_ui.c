// main/app_ui.c —— 彩票模拟器：菜单、设置、列表、编辑、开奖、对奖。
//
// 纯本地模拟：开奖号码由片内 RNG 生成，不联网、不记录、不构成任何购彩建议。
//
// 三键语义：
//   菜单   上/下移动，OK 进入，长按 OK 进设置
//   设置   上/下选择，OK 改值/进入，长按 OK 返回
//   列表   上/下选注，OK 编辑该注 / 开奖
//          双击 OK 全部重新机选，长按 OK 返回
//   编辑   上/下改号码，OK 下一格（最后一格回列表）
//          双击 OK 本注重选，长按 OK 回列表
//   开奖   OK 跳过动画，长按 OK 返回菜单
//   结果   OK 再来一注，长按 OK 返回菜单
#include "app_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_display.h"
#include "lottery.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

LV_FONT_DECLARE(app_font_12);
LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_20);

static const char *TAG = "lottery";

#define MAX_TICKETS 5

// ---------------------------------------------------------------- 配色
#define C_BG      0x0E1218
#define C_PANEL   0x1A212B
#define C_PANEL2  0x232C39
#define C_INK     0x07090C
#define C_TEXT    0xF2F5F9
#define C_MUTED   0x8C97A6
#define C_GOLD    0xFFC845
#define C_RED     0xE0453C
#define C_BLUE    0x3D7EE8
#define C_GREEN   0x3EC98A
#define C_DIM     0x2A3341

#define SCREEN_W 240
#define SCREEN_H 320

typedef enum {
    SCR_MENU = 0,
    SCR_SETTINGS,
    SCR_COUNTS,
    SCR_LIST,
    SCR_EDIT,
    SCR_DRAW,
    SCR_RESULT,
    SCR_ABOUT,
} screen_id_t;

// ---------------------------------------------------------------- 设置
typedef struct {
    uint8_t count[LOT_COUNT];   // 每个彩种的注数 1..5
    uint8_t timeout;            // 0=15s 1=30s 2=1min 3=5min 4=常亮
    uint8_t sound;              // 0/1
    uint8_t speed;              // 0=快 1=标准 2=慢
    uint8_t bright;             // 0=低 1=中 2=高
} settings_t;

static settings_t s_cfg;
static nvs_handle_t s_nvs;
static bool s_nvs_ok;

static const char *TIMEOUT_TEXT[5] = { "15 秒", "30 秒", "1 分钟", "5 分钟", "常亮" };
static const uint32_t TIMEOUT_SEC[5] = { 15, 30, 60, 300, 0 };
static const char *SPEED_TEXT[3] = { "快", "标准", "慢" };
static const uint16_t SPEED_MS[3] = { 50, 80, 140 };
static const char *BRIGHT_TEXT[3] = { "低", "中", "高" };
static const uint8_t BRIGHT_PCT[3] = { 25, 60, 100 };

static void settings_defaults(void)
{
    for (int i = 0; i < LOT_COUNT; i++) s_cfg.count[i] = 1;
    s_cfg.timeout = 1;
    s_cfg.sound = 1;
    s_cfg.speed = 1;
    s_cfg.bright = 1;
}

static void settings_load(void)
{
    settings_defaults();
    if (!s_nvs_ok) return;
    settings_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(s_nvs, "cfg", &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
        s_cfg = tmp;
        for (int i = 0; i < LOT_COUNT; i++) {
            if (s_cfg.count[i] < 1) s_cfg.count[i] = 1;
            if (s_cfg.count[i] > MAX_TICKETS) s_cfg.count[i] = MAX_TICKETS;
        }
        if (s_cfg.timeout > 4) s_cfg.timeout = 1;
        if (s_cfg.speed > 2) s_cfg.speed = 1;
        if (s_cfg.bright > 2) s_cfg.bright = 1;
        if (s_cfg.sound > 1) s_cfg.sound = 1;
    }
}

static void settings_save(void)
{
    if (!s_nvs_ok) return;
    (void)nvs_set_blob(s_nvs, "cfg", &s_cfg, sizeof(s_cfg));
    (void)nvs_commit(s_nvs);
}

// ---------------------------------------------------------------- 息屏
static esp_timer_handle_t s_idle_timer;
static volatile uint32_t s_idle_s;
static volatile bool s_dimmed;

static void apply_brightness(void)
{
    bsp_display_backlight(BRIGHT_PCT[s_cfg.bright]);
}

static void idle_tick(void *arg)
{
    (void)arg;
    uint32_t limit = TIMEOUT_SEC[s_cfg.timeout];
    if (limit == 0 || s_dimmed) return;
    s_idle_s++;
    if (s_idle_s >= limit) {
        s_dimmed = true;
        bsp_display_backlight(0);
    }
}

static void idle_reset(void)
{
    s_idle_s = 0;
}

// 返回 true 表示这次按键只是唤醒，不应继续处理。
static bool idle_wake_guard(void)
{
    idle_reset();
    if (!s_dimmed) return false;
    s_dimmed = false;
    apply_brightness();
    return true;
}

// ---------------------------------------------------------------- 状态
static lv_obj_t *s_scr;
static screen_id_t s_screen;

static lot_kind_t s_kind;
static lot_ticket_t s_tickets[MAX_TICKETS];
static lot_ticket_t s_draw;
static lot_result_t s_results[MAX_TICKETS];
static int s_sel;
static int s_cursor;

static lv_obj_t *s_menu_cards[LOT_COUNT + 1];
static int s_menu_sel;

static lv_obj_t *s_rows[MAX_TICKETS + 1];
static lv_obj_t *s_row_balls[MAX_TICKETS][LOT_MAX_MAIN + LOT_MAX_SPECIAL];
static lv_obj_t *s_row_lbls[MAX_TICKETS][LOT_MAX_MAIN + LOT_MAX_SPECIAL];
static lv_obj_t *s_list_sub;
static lv_obj_t *s_list_panel;

static lv_obj_t *s_slot_obj[LOT_MAX_MAIN + LOT_MAX_SPECIAL];
static lv_obj_t *s_slot_lbl[LOT_MAX_MAIN + LOT_MAX_SPECIAL];

static lv_obj_t *s_draw_obj[LOT_MAX_MAIN + LOT_MAX_SPECIAL];
static lv_obj_t *s_draw_lbl[LOT_MAX_MAIN + LOT_MAX_SPECIAL];
static int s_draw_slots;
static int s_draw_tick;
static lv_timer_t *s_draw_timer;

// ---------------------------------------------------------------- 音效
typedef struct {
    int freq;
    int ms;
} beep_cmd_t;

static QueueHandle_t s_beep_q;

static void beep_task(void *arg)
{
    (void)arg;
    static int16_t buf[4096];
    beep_cmd_t cmd;
    bool logged = false;
    for (;;) {
        if (xQueueReceive(s_beep_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        int n = (cmd.ms * 16000) / 1000;
        if (n > (int)(sizeof(buf) / sizeof(buf[0]))) n = sizeof(buf) / sizeof(buf[0]);
        if (n < 16) continue;
        uint32_t phase = 0;
        uint32_t inc = (uint32_t)((uint64_t)cmd.freq * 65536u / 16000u);
        for (int i = 0; i < n; i++) {
            phase += inc;
            uint16_t p = (uint16_t)(phase >> 16);
            int32_t s = (p < 32768u) ? 16000 : -16000;
            // 起音 8ms 淡入，其余线性衰减，避免爆音
            int32_t env;
            if (i < 128) env = i * 1000 / 128;
            else env = 1000 - ((i - 128) * 700 / (n - 128 > 0 ? n - 128 : 1));
            buf[i] = (int16_t)(s * env / 1000);
        }
        esp_err_t e = bsp_audio_write(buf, (size_t)n * sizeof(int16_t));
        if (!logged) {
            ESP_LOGI(TAG, "音效播放 %d Hz %d ms -> %s", cmd.freq, cmd.ms,
                     esp_err_to_name(e));
            logged = true;
        }
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "音效写入失败: %s", esp_err_to_name(e));
        }
    }
}

static void beep_ex(int freq, int ms)
{
    if (!s_beep_q || !s_cfg.sound) return;
    const beep_cmd_t cmd = { .freq = freq, .ms = ms };
    (void)xQueueSend(s_beep_q, &cmd, 0);
}

// 按键音已关闭：只在开奖时出声。
static void beep(int freq)
{
    (void)freq;
}

// 开奖专用音效：摇球时的短促咔哒、落定的提示音、结果的和弦。
static void sound_spin(void) { beep_ex(900, 16); }
static void sound_reveal(int slot) { beep_ex(1300 + slot * 40, 55); }
static void sound_win(void) { beep_ex(880, 130); beep_ex(1320, 220); }
static void sound_lose(void) { beep_ex(300, 220); }

// ---------------------------------------------------------------- 小工具
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    return scr;
}

static void show_screen(lv_obj_t *scr, screen_id_t id)
{
    lv_obj_t *old = s_scr;
    s_scr = scr;
    s_screen = id;
    lv_screen_load(scr);
    if (old && old != scr) lv_obj_delete(old);
}

static void build_header(lv_obj_t *scr, const char *title, const char *sub)
{
    lv_obj_t *t = label(scr, title, &app_font_20, C_TEXT);
    lv_obj_set_pos(t, 14, 10);
    if (sub && sub[0]) {
        lv_obj_t *s = label(scr, sub, &app_font_12, C_MUTED);
        lv_obj_set_pos(s, 14, 36);
    }
    box(scr, 14, 58, SCREEN_W - 28, 1, C_DIM);
}

static void build_footer(lv_obj_t *scr, const char *hint)
{
    box(scr, 0, SCREEN_H - 30, SCREEN_W, 30, C_PANEL);
    lv_obj_t *l = label(scr, hint, &app_font_12, C_MUTED);
    lv_obj_set_pos(l, 14, SCREEN_H - 22);
}

static void slot_color(lot_kind_t kind, int slot, uint32_t *fill, uint32_t *fg)
{
    const lot_def_t *d = lot_def(kind);
    if (d->style == LOT_STYLE_DIGIT) {
        *fill = C_PANEL2; *fg = C_TEXT; return;
    }
    bool special = slot >= d->n_main;
    *fill = special ? C_BLUE : C_RED;
    *fg = 0xFFFFFF;
}

// ---------------------------------------------------------------- 数据
static int ticket_count(void)
{
    int c = s_cfg.count[s_kind];
    if (c < 1) c = 1;
    if (c > MAX_TICKETS) c = MAX_TICKETS;
    return c;
}

static void tickets_randomize(void)
{
    int c = ticket_count();
    for (int i = 0; i < c; i++) {
        lot_random_ticket(s_kind, &s_tickets[i]);
    }
    s_cursor = 0;
}

// ---------------------------------------------------------------- 设置页
#define SET_ROWS 6
#define CNT_ROWS LOT_COUNT
static lv_obj_t *s_set_rows[SET_ROWS];
static lv_obj_t *s_set_vals[SET_ROWS];
static lv_obj_t *s_set_panel;
static int s_set_sel;
static lv_obj_t *s_cnt_rows[CNT_ROWS];
static lv_obj_t *s_cnt_vals[CNT_ROWS];
static lv_obj_t *s_cnt_panel;
static int s_cnt_sel;

static void set_value_text(int row, char *buf, size_t len)
{
    switch (row) {
    case 0: snprintf(buf, len, "%s", TIMEOUT_TEXT[s_cfg.timeout]); break;
    case 1: snprintf(buf, len, "%s", s_cfg.sound ? "开" : "关"); break;
    case 2: snprintf(buf, len, "%s", SPEED_TEXT[s_cfg.speed]); break;
    case 3: snprintf(buf, len, "%s", BRIGHT_TEXT[s_cfg.bright]); break;
    case 4: snprintf(buf, len, "查看"); break;
    default: snprintf(buf, len, "查看"); break;
    }
}

static const char *SET_NAMES[SET_ROWS] = {
    "息屏时间", "开奖音效", "开奖速度", "屏幕亮度", "修改机选注数", "关于",
};

static void settings_refresh(void)
{
    char buf[16];
    for (int i = 0; i < SET_ROWS; i++) {
        bool sel = (i == s_set_sel);
        lv_obj_set_style_bg_color(s_set_rows[i],
            lv_color_hex(sel ? C_GOLD : C_PANEL), 0);
        lv_obj_t *name = lv_obj_get_child(s_set_rows[i], 0);
        if (name) lv_obj_set_style_text_color(name,
            lv_color_hex(sel ? C_INK : C_TEXT), 0);
        set_value_text(i, buf, sizeof(buf));
        lv_label_set_text(s_set_vals[i], buf);
        lv_obj_set_style_text_color(s_set_vals[i],
            lv_color_hex(sel ? C_INK : C_MUTED), 0);
    }
    if (s_set_panel && s_set_rows[s_set_sel]) {
        lv_obj_scroll_to_view(s_set_rows[s_set_sel], LV_ANIM_ON);
    }
}

static void settings_show(void)
{
    lv_obj_t *scr = new_screen();
    build_header(scr, "设置", "改动立即保存");

    s_set_panel = lv_obj_create(scr);
    lv_obj_remove_flag(s_set_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_set_panel, 0, 66);
    lv_obj_set_size(s_set_panel, SCREEN_W, 206);
    lv_obj_set_style_bg_opa(s_set_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_set_panel, 0, 0);
    lv_obj_set_style_pad_all(s_set_panel, 0, 0);
    lv_obj_add_flag(s_set_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_set_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_set_panel, LV_SCROLLBAR_MODE_OFF);

    const int row_h = 30, row_gap = 4;
    for (int i = 0; i < SET_ROWS; i++) {
        lv_obj_t *row = box(s_set_panel, 14, i * (row_h + row_gap),
                            SCREEN_W - 28, row_h, C_PANEL);
        lv_obj_set_style_radius(row, 8, 0);
        s_set_rows[i] = row;

        lv_obj_t *n = label(row, SET_NAMES[i], &app_font_16, C_TEXT);
        lv_obj_set_pos(n, 10, 5);

        lv_obj_t *v = label(row, "", &app_font_12, C_MUTED);
        lv_obj_set_width(v, 96);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(v, SCREEN_W - 28 - 96 - 10, 9);
        s_set_vals[i] = v;
    }

    build_footer(scr, "上下选择 · OK 修改 · 长按 OK 返回");
    settings_refresh();
    show_screen(scr, SCR_SETTINGS);
}

// ---------------------------------------------------------------- 注数页
static void counts_refresh(void)
{
    char buf[16];
    for (int i = 0; i < CNT_ROWS; i++) {
        bool sel = (i == s_cnt_sel);
        lv_obj_set_style_bg_color(s_cnt_rows[i],
            lv_color_hex(sel ? C_GOLD : C_PANEL), 0);
        lv_obj_t *name = lv_obj_get_child(s_cnt_rows[i], 0);
        if (name) lv_obj_set_style_text_color(name,
            lv_color_hex(sel ? C_INK : C_TEXT), 0);
        snprintf(buf, sizeof(buf), "%d 注", s_cfg.count[i]);
        lv_label_set_text(s_cnt_vals[i], buf);
        lv_obj_set_style_text_color(s_cnt_vals[i],
            lv_color_hex(sel ? C_INK : C_MUTED), 0);
    }
    if (s_cnt_panel && s_cnt_rows[s_cnt_sel]) {
        lv_obj_scroll_to_view(s_cnt_rows[s_cnt_sel], LV_ANIM_ON);
    }
}

static void counts_show(void)
{
    lv_obj_t *scr = new_screen();
    build_header(scr, "机选注数", "每个彩种独立 · OK 切换 1-5");

    s_cnt_panel = lv_obj_create(scr);
    lv_obj_remove_flag(s_cnt_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_cnt_panel, 0, 66);
    lv_obj_set_size(s_cnt_panel, SCREEN_W, 206);
    lv_obj_set_style_bg_opa(s_cnt_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cnt_panel, 0, 0);
    lv_obj_set_style_pad_all(s_cnt_panel, 0, 0);
    lv_obj_add_flag(s_cnt_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_cnt_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_cnt_panel, LV_SCROLLBAR_MODE_OFF);

    const int row_h = 26, row_gap = 2;
    for (int i = 0; i < CNT_ROWS; i++) {
        lv_obj_t *row = box(s_cnt_panel, 14, i * (row_h + row_gap),
                            SCREEN_W - 28, row_h, C_PANEL);
        lv_obj_set_style_radius(row, 7, 0);
        s_cnt_rows[i] = row;

        lv_obj_t *n = label(row, lot_def((lot_kind_t)i)->name, &app_font_16, C_TEXT);
        lv_obj_set_pos(n, 10, 3);

        lv_obj_t *v = label(row, "", &app_font_12, C_MUTED);
        lv_obj_set_width(v, 96);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(v, SCREEN_W - 28 - 96 - 10, 7);
        s_cnt_vals[i] = v;
    }

    build_footer(scr, "上下选择 · OK 切换 · 长按 OK 返回");
    counts_refresh();
    show_screen(scr, SCR_COUNTS);
}

// ---------------------------------------------------------------- 列表
static void list_refresh(void)
{
    int n = lot_slots(s_kind);
    int c = ticket_count();
    for (int i = 0; i < c; i++) {
        lv_obj_set_style_bg_color(s_rows[i],
            lv_color_hex(i == s_sel ? C_GOLD : C_PANEL), 0);
        lv_obj_t *idx = lv_obj_get_child(s_rows[i], 0);
        if (idx) lv_obj_set_style_text_color(idx,
            lv_color_hex(i == s_sel ? C_INK : C_MUTED), 0);
        for (int k = 0; k < n; k++) {
            uint32_t fill, fg;
            slot_color(s_kind, k, &fill, &fg);
            lv_obj_set_style_bg_color(s_row_balls[i][k], lv_color_hex(fill), 0);
            char t[8];
            snprintf(t, sizeof(t), "%02u",
                     lot_slot_value(s_kind, &s_tickets[i], k));
            lv_label_set_text(s_row_lbls[i][k], t);
            lv_obj_set_style_text_color(s_row_lbls[i][k], lv_color_hex(fg), 0);
        }
    }
    lv_obj_set_style_bg_color(s_rows[c],
        lv_color_hex(s_sel == c ? C_GOLD : C_PANEL2), 0);
    lv_obj_t *go = lv_obj_get_child(s_rows[c], 0);
    if (go) lv_obj_set_style_text_color(go,
        lv_color_hex(s_sel == c ? C_INK : C_TEXT), 0);

    const lot_def_t *d = lot_def(s_kind);
    char sub[64];
    snprintf(sub, sizeof(sub), "%d 注 · 合计 %u 元", c,
             (unsigned)(d->price * c));
    if (s_list_sub) lv_label_set_text(s_list_sub, sub);
    if (s_list_panel && s_rows[s_sel]) {
        lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_ON);
    }
}

static void list_show(void)
{
    const lot_def_t *d = lot_def(s_kind);
    const int row_h = 28, row_gap = 2;
    const int ball = 17, ball_gap = 2;
    int n = lot_slots(s_kind);
    int c = ticket_count();

    lv_obj_t *scr = new_screen();
    build_header(scr, d->name, "");
    s_list_sub = label(scr, "", &app_font_12, C_MUTED);
    lv_obj_set_pos(s_list_sub, 14, 36);

    s_list_panel = lv_obj_create(scr);
    lv_obj_remove_flag(s_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_list_panel, 0, 66);
    lv_obj_set_size(s_list_panel, SCREEN_W, 206);
    lv_obj_set_style_bg_opa(s_list_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_panel, 0, 0);
    lv_obj_set_style_pad_all(s_list_panel, 0, 0);
    lv_obj_add_flag(s_list_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list_panel, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < c; i++) {
        lv_obj_t *row = box(s_list_panel, 14, i * (row_h + row_gap),
                            SCREEN_W - 28, row_h, C_PANEL);
        lv_obj_set_style_radius(row, 8, 0);
        s_rows[i] = row;

        char ib[8];
        snprintf(ib, sizeof(ib), "%d", i + 1);
        lv_obj_t *idx = label(row, ib, &app_font_12, C_MUTED);
        lv_obj_set_pos(idx, 8, 6);

        int total_w = n * ball + (n - 1) * ball_gap;
        int x0 = (SCREEN_W - 28 - total_w) / 2;
        if (x0 < 28) x0 = 28;
        for (int k = 0; k < n; k++) {
            lv_obj_t *o = box(row, x0 + k * (ball + ball_gap),
                              (row_h - ball) / 2, ball, ball, C_PANEL2);
            lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
            s_row_balls[i][k] = o;
            lv_obj_t *l = label(o, "00", &app_font_12, C_TEXT);
            lv_obj_center(l);
            s_row_lbls[i][k] = l;
        }
    }

    int y = c * (row_h + row_gap);
    lv_obj_t *go = box(s_list_panel, 14, y, SCREEN_W - 28, row_h, C_PANEL2);
    lv_obj_set_style_radius(go, 8, 0);
    s_rows[c] = go;
    lv_obj_t *gl = label(go, "开奖", &app_font_16, C_TEXT);
    lv_obj_set_width(gl, SCREEN_W - 28);
    lv_obj_set_style_text_align(gl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(gl, 0, 4);

    build_footer(scr, "上下选 · OK 编辑/开奖 · 双击机选");
    list_refresh();
    show_screen(scr, SCR_LIST);
}

// ---------------------------------------------------------------- 编辑
static void edit_refresh(void)
{
    int n = lot_slots(s_kind);
    for (int i = 0; i < n; i++) {
        uint32_t fill, fg;
        slot_color(s_kind, i, &fill, &fg);
        lv_obj_set_style_bg_color(s_slot_obj[i], lv_color_hex(fill), 0);
        lv_obj_set_style_border_width(s_slot_obj[i], i == s_cursor ? 3 : 0, 0);
        lv_obj_set_style_border_color(s_slot_obj[i], lv_color_hex(C_GOLD), 0);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02u",
                 lot_slot_value(s_kind, &s_tickets[s_sel], i));
        lv_label_set_text(s_slot_lbl[i], buf);
        lv_obj_set_style_text_color(s_slot_lbl[i], lv_color_hex(fg), 0);
    }
}

static void edit_show(void)
{
    const lot_def_t *d = lot_def(s_kind);
    char sub[48];
    snprintf(sub, sizeof(sub), "第 %d 注 · 单注 %u 元", s_sel + 1,
             (unsigned)d->price);

    lv_obj_t *scr = new_screen();
    build_header(scr, d->name, sub);

    int n = lot_slots(s_kind);
    int digit = (d->style == LOT_STYLE_DIGIT);
    int main_n = digit ? 0 : d->n_main;

    int gap = (n >= 7) ? 3 : 4;
    int size_digit = (n <= 3) ? 44 : (n <= 5 ? 40 : 28);
    int size_ball = 30;
    if (main_n > 0) {
        int fit = (SCREEN_W - 24 - (main_n - 1) * gap) / main_n;
        if (fit < size_ball) size_ball = fit;
    }

    for (int i = 0; i < n; i++) {
        int size, row_y, in_row, row_start;
        if (digit) {
            size = size_digit; in_row = n; row_y = 96; row_start = 0;
        } else if (i < main_n) {
            size = size_ball; in_row = main_n; row_y = 92; row_start = 0;
        } else {
            size = size_ball; in_row = d->n_special; row_y = 144; row_start = main_n;
        }
        int idx = i - row_start;
        int total_w = in_row * size + (in_row - 1) * gap;
        int x = (SCREEN_W - total_w) / 2 + idx * (size + gap);

        lv_obj_t *o = box(scr, x, row_y, size, size, C_PANEL2);
        lv_obj_set_style_radius(o, digit ? 8 : LV_RADIUS_CIRCLE, 0);
        s_slot_obj[i] = o;
        lv_obj_t *l = label(o, "00", digit ? &app_font_20 : &app_font_16, C_TEXT);
        lv_obj_center(l);
        s_slot_lbl[i] = l;
    }

    lv_obj_t *hint = label(scr, "上/下改号 · OK 下一格 · 双击机选本注",
                           &app_font_12, C_MUTED);
    lv_obj_set_pos(hint, 14, 200);

    build_footer(scr, "长按 OK 回到列表");
    edit_refresh();
    show_screen(scr, SCR_EDIT);
}

// ---------------------------------------------------------------- 开奖
static void result_show(void);

static void draw_finish(void)
{
    if (s_draw_timer) {
        lv_timer_delete(s_draw_timer);
        s_draw_timer = NULL;
    }
    int c = ticket_count();
    for (int i = 0; i < c; i++) {
        s_results[i] = lot_evaluate(s_kind, &s_tickets[i], &s_draw);
    }
    bool any = false;
    for (int i = 0; i < c; i++) {
        if (s_results[i].level > 0) any = true;
    }
    if (any) sound_win(); else sound_lose();
}

static void draw_tick(lv_timer_t *t)
{
    (void)t;
    int n = s_draw_slots;
    int slot = s_draw_tick / 3;
    int phase = s_draw_tick % 3;
    if (slot >= n) {
        draw_finish();
        result_show();
        return;
    }
    char buf[8];
    if (phase < 2) {
        snprintf(buf, sizeof(buf), "%02u", (unsigned)(esp_random() % 100));
        sound_spin();
    } else {
        snprintf(buf, sizeof(buf), "%02u", lot_slot_value(s_kind, &s_draw, slot));
        sound_reveal(slot);
    }
    lv_label_set_text(s_draw_lbl[slot], buf);
    lv_obj_set_style_bg_color(s_draw_obj[slot], lv_color_hex(C_GOLD), 0);
    if (phase == 2) {
        uint32_t fill, fg;
        slot_color(s_kind, slot, &fill, &fg);
        lv_obj_set_style_bg_color(s_draw_obj[slot], lv_color_hex(fill), 0);
        lv_obj_set_style_text_color(s_draw_lbl[slot], lv_color_hex(fg), 0);
    }
    s_draw_tick++;
}

static void draw_show(void)
{
    const lot_def_t *d = lot_def(s_kind);
    int c = ticket_count();
    char sub[48];
    snprintf(sub, sizeof(sub), "%d 注 · 投入 %u 元", c,
             (unsigned)(d->price * c));
    lv_obj_t *scr = new_screen();
    build_header(scr, "开奖", sub);

    int n = lot_slots(s_kind);
    s_draw_slots = n;
    int digit = (d->style == LOT_STYLE_DIGIT);
    int main_n = digit ? 0 : d->n_main;

    int gap = (n >= 7) ? 3 : 4;
    int size_digit = (n <= 3) ? 44 : (n <= 5 ? 40 : 28);
    int size_ball = 30;
    if (main_n > 0) {
        int fit = (SCREEN_W - 24 - (main_n - 1) * gap) / main_n;
        if (fit < size_ball) size_ball = fit;
    }

    for (int i = 0; i < n; i++) {
        int size, row_y, in_row, row_start;
        if (digit) {
            size = size_digit; in_row = n; row_y = 100; row_start = 0;
        } else if (i < main_n) {
            size = size_ball; in_row = main_n; row_y = 96; row_start = 0;
        } else {
            size = size_ball; in_row = d->n_special; row_y = 148; row_start = main_n;
        }
        int idx = i - row_start;
        int total_w = in_row * size + (in_row - 1) * gap;
        int x = (SCREEN_W - total_w) / 2 + idx * (size + gap);

        lv_obj_t *o = box(scr, x, row_y, size, size, C_DIM);
        lv_obj_set_style_radius(o, digit ? 8 : LV_RADIUS_CIRCLE, 0);
        s_draw_obj[i] = o;
        lv_obj_t *l = label(o, "--", digit ? &app_font_20 : &app_font_16, C_MUTED);
        lv_obj_center(l);
        s_draw_lbl[i] = l;
    }

    build_footer(scr, "开奖中 · OK 跳过 · 长按返回");
    s_draw_tick = 0;
    show_screen(scr, SCR_DRAW);
    s_draw_timer = lv_timer_create(draw_tick, SPEED_MS[s_cfg.speed], NULL);
}

// ---------------------------------------------------------------- 结果
static void result_show(void)
{
    const lot_def_t *d = lot_def(s_kind);
    int c = ticket_count();

    uint32_t total = 0;
    bool any = false;
    for (int i = 0; i < c; i++) {
        if (s_results[i].level > 0) {
            any = true;
            total += s_results[i].amount;
        }
    }

    char sub[48];
    snprintf(sub, sizeof(sub), "%d 注 · 投入 %u 元", c,
             (unsigned)(d->price * c));
    lv_obj_t *scr = new_screen();
    build_header(scr, any ? "中奖" : "未中奖", sub);

    lv_obj_t *big = label(scr, any ? "本期有奖" : "本期未中", &app_font_20,
                          any ? C_GOLD : C_MUTED);
    lv_obj_set_pos(big, 14, 66);

    char buf[48];
    if (any) {
        snprintf(buf, sizeof(buf), "+%u 元", (unsigned)total);
        lv_obj_t *amt = label(scr, buf, &app_font_20, C_GREEN);
        lv_obj_set_pos(amt, 14, 94);
    } else {
        lv_obj_t *amt = label(scr, "0 元", &app_font_16, C_MUTED);
        lv_obj_set_pos(amt, 14, 96);
    }

    lv_obj_t *cap = label(scr, "开奖", &app_font_12, C_MUTED);
    lv_obj_set_pos(cap, 14, 126);
    int n = lot_slots(s_kind);
    int digit = (d->style == LOT_STYLE_DIGIT);
    int main_n = digit ? 0 : d->n_main;
    int size = 18, gap = 3;
    for (int row = 0; row < 2; row++) {
        if (digit && row == 1) break;
        int count = digit ? n : (row == 0 ? main_n : d->n_special);
        if (count <= 0) continue;
        int y = 144 + row * 22;
        int total_w = count * size + (count - 1) * gap;
        int x0 = (SCREEN_W - total_w) / 2;
        for (int i = 0; i < count; i++) {
            int slot = digit ? i : (row == 0 ? i : main_n + i);
            uint32_t fill, fg;
            slot_color(s_kind, slot, &fill, &fg);
            lv_obj_t *o = box(scr, x0 + i * (size + gap), y, size, size, fill);
            lv_obj_set_style_radius(o, digit ? 4 : LV_RADIUS_CIRCLE, 0);
            char t[8];
            snprintf(t, sizeof(t), "%02u", lot_slot_value(s_kind, &s_draw, slot));
            lv_obj_t *l = label(o, t, &app_font_12, fg);
            lv_obj_center(l);
        }
    }

    int y = digit ? 180 : 196;
    for (int i = 0; i < c && y < 286; i++) {
        char left[32];
        if (s_results[i].level > 0) {
            snprintf(left, sizeof(left), "%d  %s", i + 1, s_results[i].level_name);
        } else {
            snprintf(left, sizeof(left), "%d  未中奖", i + 1);
        }
        lv_obj_t *l = label(scr, left, &app_font_12,
                            s_results[i].level > 0 ? C_TEXT : C_MUTED);
        lv_obj_set_pos(l, 16, y);

        if (s_results[i].level > 0) {
            char right[24];
            snprintf(right, sizeof(right), "+%u", (unsigned)s_results[i].amount);
            lv_obj_t *r = label(scr, right, &app_font_12, C_GREEN);
            lv_obj_set_width(r, SCREEN_W - 32);
            lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(r, 16, y);
        }
        y += 18;
    }

    build_footer(scr, "OK 再来一注 · 长按返回菜单");
    show_screen(scr, SCR_RESULT);
}

// ---------------------------------------------------------------- 关于
static void about_show(void)
{
    lv_obj_t *scr = new_screen();
    build_header(scr, "关于", "Lottery Pass v1.3");

    const char *lines =
        "这是一个本地模拟器。\n"
        "开奖号码由设备随机生成，\n"
        "与真实彩票开奖无关。\n\n"
        "奖级按公开规则计算，\n"
        "浮动奖以固定值模拟。\n\n"
        "仅供娱乐，不构成投资、\n"
        "购彩或任何形式的建议。\n\n"
        "请理性对待，量力而行。";
    lv_obj_t *l = label(scr, lines, &app_font_12, C_TEXT);
    lv_obj_set_pos(l, 16, 72);
    lv_obj_set_width(l, SCREEN_W - 32);
    lv_obj_set_style_text_line_space(l, 2, 0);

    build_footer(scr, "任意键返回");
    show_screen(scr, SCR_ABOUT);
}

// ---------------------------------------------------------------- 菜单
static void menu_refresh(void)
{
    for (int i = 0; i <= LOT_COUNT; i++) {
        bool sel = (i == s_menu_sel);
        uint32_t base = (i == LOT_COUNT) ? C_PANEL2 : C_PANEL;
        lv_obj_set_style_bg_color(s_menu_cards[i],
                                  lv_color_hex(sel ? C_GOLD : base), 0);
        lv_obj_t *lbl = lv_obj_get_child(s_menu_cards[i], 0);
        if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? C_INK : C_TEXT), 0);
    }
}

static void menu_show(void)
{
    lv_obj_t *scr = new_screen();
    build_header(scr, "彩票模拟器", "本地随机开奖 · 七款彩种");

    for (int i = 0; i < LOT_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;
        int x = 12 + col * 112;
        int y = 70 + row * 52;
        lv_obj_t *card = box(scr, x, y, 104, 44, C_PANEL);
        lv_obj_set_style_radius(card, 10, 0);
        s_menu_cards[i] = card;
        lv_obj_t *name = label(card, lot_def((lot_kind_t)i)->name, &app_font_16, C_TEXT);
        lv_obj_set_width(name, 104);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(name, 0, 3);
        lv_obj_t *sub = label(card, lot_def((lot_kind_t)i)->subtitle, &app_font_12, C_MUTED);
        lv_obj_set_width(sub, 104);
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(sub, 0, 26);
    }

    lv_obj_t *card = box(scr, 12 + 112, 70 + 3 * 52, 104, 44, C_PANEL2);
    lv_obj_set_style_radius(card, 10, 0);
    s_menu_cards[LOT_COUNT] = card;
    lv_obj_t *nm = label(card, "设置", &app_font_16, C_TEXT);
    lv_obj_set_width(nm, 104);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(nm, 0, 3);
    lv_obj_t *sb = label(card, "注数 · 息屏 · 音效", &app_font_12, C_MUTED);
    lv_obj_set_width(sb, 104);
    lv_obj_set_style_text_align(sb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(sb, 0, 26);

    build_footer(scr, "上下选择 · OK 进入 · 长按 OK 设置");
    menu_refresh();
    show_screen(scr, SCR_MENU);
}

// ---------------------------------------------------------------- 入口
void app_ui_start(void)
{
    s_nvs_ok = (nvs_open("cfg", NVS_READWRITE, &s_nvs) == ESP_OK);
    settings_load();
    apply_brightness();

    const esp_timer_create_args_t args = {
        .callback = idle_tick,
        .name = "idle",
    };
    esp_timer_create(&args, &s_idle_timer);
    if (s_idle_timer) esp_timer_start_periodic(s_idle_timer, 1000000);

    s_kind = LOT_SSQ;
    s_sel = 0;
    s_cursor = 0;
    s_menu_sel = 0;
    tickets_randomize();
    menu_show();
    ESP_LOGI(TAG, "彩票模拟器就绪：%d 注 / 息屏 %s / 音效 %s",
             s_cfg.count[LOT_SSQ], TIMEOUT_TEXT[s_cfg.timeout],
             s_cfg.sound ? "开" : "关");
}

static void begin_draw(void)
{
    lot_draw(s_kind, &s_draw);
    draw_show();
}

static void settings_activate(int row)
{
    switch (row) {
    case 0: s_cfg.timeout = (uint8_t)((s_cfg.timeout + 1) % 5); break;
    case 1: s_cfg.sound = s_cfg.sound ? 0 : 1; break;
    case 2: s_cfg.speed = (uint8_t)((s_cfg.speed + 1) % 3); break;
    case 3:
        s_cfg.bright = (uint8_t)((s_cfg.bright + 1) % 3);
        apply_brightness();
        break;
    case 4:
        s_cnt_sel = 0;
        counts_show();
        return;
    default:
        about_show();
        return;
    }
    settings_save();
    idle_reset();
    beep(760);
    settings_refresh();
}

void app_ui_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_PRESS || ev == BSP_BTN_RELEASE) return;
    if (idle_wake_guard()) return;

    switch (s_screen) {
    case SCR_MENU:
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            s_menu_sel = (s_menu_sel + LOT_COUNT) % (LOT_COUNT + 1);
            beep(520);
            menu_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            s_menu_sel = (s_menu_sel + 1) % (LOT_COUNT + 1);
            beep(520);
            menu_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            beep(760);
            if (s_menu_sel == LOT_COUNT) {
                s_set_sel = 0;
                settings_show();
            } else {
                s_kind = (lot_kind_t)s_menu_sel;
                s_sel = 0;
                tickets_randomize();
                list_show();
            }
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            beep(700);
            s_set_sel = 0;
            settings_show();
        }
        break;

    case SCR_SETTINGS:
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            s_set_sel = (s_set_sel + SET_ROWS - 1) % SET_ROWS;
            beep(520);
            settings_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            s_set_sel = (s_set_sel + 1) % SET_ROWS;
            beep(520);
            settings_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            settings_activate(s_set_sel);
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            beep(420);
            menu_show();
        }
        break;

    case SCR_COUNTS:
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            s_cnt_sel = (s_cnt_sel + CNT_ROWS - 1) % CNT_ROWS;
            beep(520);
            counts_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            s_cnt_sel = (s_cnt_sel + 1) % CNT_ROWS;
            beep(520);
            counts_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            s_cfg.count[s_cnt_sel] =
                (uint8_t)((s_cfg.count[s_cnt_sel] % MAX_TICKETS) + 1);
            settings_save();
            idle_reset();
            beep(760);
            counts_refresh();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            beep(420);
            s_set_sel = SET_ROWS - 1;
            settings_show();
        }
        break;

    case SCR_LIST:
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_UP) {
            int c = ticket_count();
            s_sel = (s_sel + c) % (c + 1);
            beep(520);
            list_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_DOWN) {
            int c = ticket_count();
            s_sel = (s_sel + 1) % (c + 1);
            beep(520);
            list_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            if (s_sel >= ticket_count()) {
                beep(900);
                begin_draw();
            } else {
                s_cursor = 0;
                beep(700);
                edit_show();
            }
        } else if (ev == BSP_BTN_DOUBLE && btn == BSP_BTN_OK) {
            tickets_randomize();
            beep(820);
            list_refresh();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            beep(420);
            menu_show();
        }
        break;

    case SCR_EDIT:
        if (ev == BSP_BTN_CLICK && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
            lot_slot_adjust(s_kind, &s_tickets[s_sel], s_cursor,
                            btn == BSP_BTN_UP ? 1 : -1);
            beep(600);
            edit_refresh();
        } else if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            int n = lot_slots(s_kind);
            if (s_cursor + 1 >= n) {
                beep(700);
                list_show();
            } else {
                s_cursor++;
                beep(700);
                edit_refresh();
            }
        } else if (ev == BSP_BTN_DOUBLE && btn == BSP_BTN_OK) {
            lot_random_ticket(s_kind, &s_tickets[s_sel]);
            s_cursor = 0;
            beep(820);
            edit_refresh();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            beep(420);
            list_show();
        }
        break;

    case SCR_DRAW:
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            if (s_draw_timer) {
                lv_timer_delete(s_draw_timer);
                s_draw_timer = NULL;
            }
            draw_finish();
            result_show();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            if (s_draw_timer) {
                lv_timer_delete(s_draw_timer);
                s_draw_timer = NULL;
            }
            menu_show();
        }
        break;

    case SCR_RESULT:
        if (ev == BSP_BTN_CLICK && btn == BSP_BTN_OK) {
            beep(760);
            tickets_randomize();
            s_sel = 0;
            list_show();
        } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            beep(420);
            menu_show();
        }
        break;

    case SCR_ABOUT:
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG) {
            beep(420);
            s_set_sel = 5;
            settings_show();
        }
        break;
    }
}

void app_audio_start(void)
{
    s_beep_q = xQueueCreate(6, sizeof(beep_cmd_t));
    if (!s_beep_q) return;
    if (xTaskCreate(beep_task, "beep", 4096, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_beep_q);
        s_beep_q = NULL;
    }
}
