// main/lottery.h —— 彩票规则与开奖逻辑（纯逻辑，可在主机上测试）
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LOT_MAX_MAIN    7
#define LOT_MAX_SPECIAL 2
#define LOT_MAX_DIGITS  7

typedef enum {
    LOT_SSQ = 0,   // 双色球：红 6/33 + 蓝 1/16
    LOT_DLT,       // 大乐透：前 5/35 + 后 2/12
    LOT_3D,        // 福彩 3D：三位数字
    LOT_PL3,       // 排列三：三位数字
    LOT_PL5,       // 排列五：五位数字
    LOT_QXC,       // 七星彩：七位数字
    LOT_QLC,       // 七乐彩：基本 7/30 + 特别 1/30（不与基本号重复）
    LOT_COUNT
} lot_kind_t;

typedef enum {
    LOT_STYLE_BALL = 0,   // 球号
    LOT_STYLE_DIGIT,      // 数字位
} lot_style_t;

typedef struct {
    const char *name;
    const char *subtitle;
    lot_style_t style;
    uint8_t n_main;
    uint8_t max_main;
    uint8_t n_special;
    uint8_t max_special;
    bool special_disjoint;   // 特别号不得与基本号重复
    uint8_t n_digits;
    uint16_t price;          // 单注金额（元）
} lot_def_t;

typedef struct {
    uint8_t main[LOT_MAX_MAIN];
    uint8_t special[LOT_MAX_SPECIAL];
    uint8_t digits[LOT_MAX_DIGITS];
} lot_ticket_t;

typedef struct {
    int level;               // 0 = 未中奖
    uint32_t amount;         // 奖金（元）
    const char *level_name;
} lot_result_t;

const lot_def_t *lot_def(lot_kind_t kind);

// 可编辑格数：球号 = n_main + n_special，数字彩 = n_digits。
int lot_slots(lot_kind_t kind);

// 取/改某一格的值。delta 为 +1/-1，越界循环，并保证同区不重复。
uint8_t lot_slot_value(lot_kind_t kind, const lot_ticket_t *t, int slot);
void lot_slot_adjust(lot_kind_t kind, lot_ticket_t *t, int slot, int delta);

// 生成一注随机号码。
void lot_random_ticket(lot_kind_t kind, lot_ticket_t *out);

// 开奖：号码生成规则与选号一致。
void lot_draw(lot_kind_t kind, lot_ticket_t *out);

// 对奖。draw 与 ticket 使用同一结构。
lot_result_t lot_evaluate(lot_kind_t kind, const lot_ticket_t *ticket,
                          const lot_ticket_t *draw);

// 规则自检：开奖号码生成与奖级判定。返回失败项数，0 表示全部通过。
int lot_selftest(void);
