// main/lottery.c —— 彩票规则、随机选号、开奖与对奖。
//
// 说明：奖级与奖金按公开规则的常见档位实现，头奖等浮动奖使用固定模拟值。
// 本程序只做本地模拟，不涉及任何真实购彩行为。
#include "lottery.h"

#include <string.h>

// 由平台层提供（设备上是 esp_random，主机测试可替换）。
uint32_t lot_platform_random(void);

static const lot_def_t DEFS[LOT_COUNT] = {
    [LOT_SSQ] = { .name = "双色球", .subtitle = "6+1 选号",
                  .style = LOT_STYLE_BALL, .n_main = 6, .max_main = 33,
                  .n_special = 1, .max_special = 16, .special_disjoint = false,
                  .price = 2 },
    [LOT_DLT] = { .name = "大乐透", .subtitle = "5+2 选号",
                  .style = LOT_STYLE_BALL, .n_main = 5, .max_main = 35,
                  .n_special = 2, .max_special = 12, .special_disjoint = false,
                  .price = 2 },
    [LOT_3D]  = { .name = "福彩3D", .subtitle = "三位数字",
                  .style = LOT_STYLE_DIGIT, .n_digits = 3, .price = 2 },
    [LOT_PL3] = { .name = "排列三", .subtitle = "三位数字",
                  .style = LOT_STYLE_DIGIT, .n_digits = 3, .price = 2 },
    [LOT_PL5] = { .name = "排列五", .subtitle = "五位数字",
                  .style = LOT_STYLE_DIGIT, .n_digits = 5, .price = 2 },
    [LOT_QXC] = { .name = "七星彩", .subtitle = "七位数字",
                  .style = LOT_STYLE_DIGIT, .n_digits = 7, .price = 2 },
    [LOT_QLC] = { .name = "七乐彩", .subtitle = "7+1 选号",
                  .style = LOT_STYLE_BALL, .n_main = 7, .max_main = 30,
                  .n_special = 1, .max_special = 30, .special_disjoint = true,
                  .price = 2 },
};

const lot_def_t *lot_def(lot_kind_t kind)
{
    if (kind < 0 || kind >= LOT_COUNT) return &DEFS[0];
    return &DEFS[kind];
}

int lot_slots(lot_kind_t kind)
{
    const lot_def_t *d = lot_def(kind);
    if (d->style == LOT_STYLE_DIGIT) return d->n_digits;
    return (int)d->n_main + (int)d->n_special;
}

static uint32_t rng_below(uint32_t bound)
{
    return bound ? (lot_platform_random() % bound) : 0;
}

// 在 1..max 里挑 n 个互不重复的数，exclude 里的值跳过。
static void pick_unique(uint8_t *out, int n, int max, const uint8_t *exclude, int n_ex)
{
    int found = 0;
    int guard = 0;
    while (found < n && guard < 4000) {
        guard++;
        uint8_t v = (uint8_t)(rng_below((uint32_t)max) + 1);
        bool used = false;
        for (int i = 0; i < found; i++) {
            if (out[i] == v) { used = true; break; }
        }
        if (!used) {
            for (int i = 0; i < n_ex; i++) {
                if (exclude[i] == v) { used = true; break; }
            }
        }
        if (!used) out[found++] = v;
    }
    // 极端情况下补全（正常随机不会走到这里）。
    for (int v = 1; v <= max && found < n; v++) {
        bool used = false;
        for (int i = 0; i < found; i++) if (out[i] == v) used = true;
        for (int i = 0; i < n_ex; i++) if (exclude[i] == v) used = true;
        if (!used) out[found++] = v;
    }
}

void lot_random_ticket(lot_kind_t kind, lot_ticket_t *out)
{
    const lot_def_t *d = lot_def(kind);
    memset(out, 0, sizeof(*out));

    if (d->style == LOT_STYLE_DIGIT) {
        for (int i = 0; i < d->n_digits; i++) {
            out->digits[i] = (uint8_t)rng_below(10);
        }
        return;
    }

    pick_unique(out->main, d->n_main, d->max_main, NULL, 0);
    if (d->n_special > 0) {
        if (d->special_disjoint) {
            pick_unique(out->special, d->n_special, d->max_special,
                        out->main, d->n_main);
        } else {
            pick_unique(out->special, d->n_special, d->max_special, NULL, 0);
        }
    }
}

void lot_draw(lot_kind_t kind, lot_ticket_t *out)
{
    lot_random_ticket(kind, out);
}

uint8_t lot_slot_value(lot_kind_t kind, const lot_ticket_t *t, int slot)
{
    const lot_def_t *d = lot_def(kind);
    if (d->style == LOT_STYLE_DIGIT) {
        if (slot < 0 || slot >= d->n_digits) return 0;
        return t->digits[slot];
    }
    if (slot < 0) return 0;
    if (slot < d->n_main) return t->main[slot];
    int s = slot - d->n_main;
    if (s < 0 || s >= d->n_special) return 0;
    return t->special[s];
}

static bool region_used(const uint8_t *arr, int n, int skip, int v)
{
    for (int i = 0; i < n; i++) {
        if (i != skip && arr[i] == v) return true;
    }
    return false;
}

void lot_slot_adjust(lot_kind_t kind, lot_ticket_t *t, int slot, int delta)
{
    const lot_def_t *d = lot_def(kind);
    if (delta == 0) return;

    if (d->style == LOT_STYLE_DIGIT) {
        if (slot < 0 || slot >= d->n_digits) return;
        int v = (int)t->digits[slot] + delta;
        v %= 10;
        if (v < 0) v += 10;
        t->digits[slot] = (uint8_t)v;
        return;
    }

    bool is_special = slot >= d->n_main;
    int idx = is_special ? slot - d->n_main : slot;
    int n = is_special ? d->n_special : d->n_main;
    int max = is_special ? d->max_special : d->max_main;
    if (idx < 0 || idx >= n) return;

    uint8_t *arr = is_special ? t->special : t->main;
    // 七乐彩的特别号与基本号互斥：改基本号时要避开特别号，改特别号时要避开全部基本号。
    const uint8_t *ex = NULL;
    int n_ex = 0;
    if (d->special_disjoint) {
        ex = is_special ? t->main : t->special;
        n_ex = is_special ? d->n_main : d->n_special;
    }

    int v = arr[idx];
    int step = delta > 0 ? 1 : -1;
    int tries = delta > 0 ? delta : -delta;
    for (int k = 0; k < tries; k++) {
        do {
            v += step;
            if (v > max) v = 1;
            if (v < 1) v = max;
        } while (region_used(arr, n, idx, v) ||
                 (ex && region_used(ex, n_ex, -1, v)));
        arr[idx] = (uint8_t)v;
    }
}

static int count_matches(const uint8_t *a, int na, const uint8_t *b, int nb)
{
    int hit = 0;
    for (int i = 0; i < na; i++) {
        for (int j = 0; j < nb; j++) {
            if (a[i] == b[j]) { hit++; break; }
        }
    }
    return hit;
}

static void sort3(const uint8_t *in, uint8_t *out)
{
    out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2 - i; j++) {
            if (out[j] > out[j + 1]) {
                uint8_t tmp = out[j]; out[j] = out[j + 1]; out[j + 1] = tmp;
            }
        }
    }
}

static lot_result_t result(int level, uint32_t amount, const char *name)
{
    lot_result_t r = { .level = level, .amount = amount, .level_name = name };
    return r;
}

static const char *LEVEL_NAME[10] = {
    "", "一等奖", "二等奖", "三等奖", "四等奖", "五等奖",
    "六等奖", "七等奖", "八等奖", "九等奖",
};

static lot_result_t eval_ssq(const lot_ticket_t *t, const lot_ticket_t *d)
{
    int rm = count_matches(t->main, 6, d->main, 6);
    bool bm = (t->special[0] == d->special[0]);
    if (rm == 6 && bm)  return result(1, 5000000u, LEVEL_NAME[1]);
    if (rm == 6)        return result(2, 150000u,  LEVEL_NAME[2]);
    if (rm == 5 && bm)  return result(3, 3000u,    LEVEL_NAME[3]);
    if (rm == 5 || (rm == 4 && bm)) return result(4, 200u, LEVEL_NAME[4]);
    if (rm == 4 || (rm == 3 && bm)) return result(5, 10u,  LEVEL_NAME[5]);
    if (bm)             return result(6, 5u,       LEVEL_NAME[6]);
    return result(0, 0, "");
}

static lot_result_t eval_dlt(const lot_ticket_t *t, const lot_ticket_t *d)
{
    int fm = count_matches(t->main, 5, d->main, 5);
    int bm = count_matches(t->special, 2, d->special, 2);
    if (fm == 5 && bm == 2) return result(1, 10000000u, LEVEL_NAME[1]);
    if (fm == 5 && bm == 1) return result(2, 300000u,   LEVEL_NAME[2]);
    if (fm == 5)            return result(3, 10000u,    LEVEL_NAME[3]);
    if (fm == 4 && bm == 2) return result(4, 3000u,     LEVEL_NAME[4]);
    if (fm == 4 && bm == 1) return result(5, 300u,      LEVEL_NAME[5]);
    if (fm == 3 && bm == 2) return result(6, 200u,      LEVEL_NAME[6]);
    if (fm == 4)            return result(7, 100u,      LEVEL_NAME[7]);
    if ((fm == 3 && bm == 1) || (fm == 2 && bm == 2)) return result(8, 15u, LEVEL_NAME[8]);
    if (fm == 3 || bm == 2 || (fm == 2 && bm == 1) || (fm == 1 && bm == 2))
        return result(9, 5u, LEVEL_NAME[9]);
    return result(0, 0, "");
}

static lot_result_t eval_digit3(const lot_ticket_t *t, const lot_ticket_t *d)
{
    bool direct = true;
    for (int i = 0; i < 3; i++) {
        if (t->digits[i] != d->digits[i]) { direct = false; break; }
    }
    if (direct) return result(1, 1040u, "直选");

    uint8_t a[3], b[3];
    sort3(t->digits, a);
    sort3(d->digits, b);
    if (memcmp(a, b, 3) != 0) return result(0, 0, "");

    bool pair = (b[0] == b[1] || b[1] == b[2]);
    if (pair) return result(2, 346u, "组选三");
    return result(3, 173u, "组选六");
}

static lot_result_t eval_pl5(const lot_ticket_t *t, const lot_ticket_t *d)
{
    for (int i = 0; i < 5; i++) {
        if (t->digits[i] != d->digits[i]) return result(0, 0, "");
    }
    return result(1, 100000u, "直选");
}

static lot_result_t eval_qxc(const lot_ticket_t *t, const lot_ticket_t *d)
{
    int hit = 0;
    for (int i = 0; i < 7; i++) {
        if (t->digits[i] == d->digits[i]) hit++;
    }
    switch (hit) {
    case 7: return result(1, 5000000u, LEVEL_NAME[1]);
    case 6: return result(2, 100000u,  LEVEL_NAME[2]);
    case 5: return result(3, 3000u,    LEVEL_NAME[3]);
    case 4: return result(4, 500u,     LEVEL_NAME[4]);
    case 3: return result(5, 20u,      LEVEL_NAME[5]);
    case 2: return result(6, 5u,       LEVEL_NAME[6]);
    default: return result(0, 0, "");
    }
}

static lot_result_t eval_qlc(const lot_ticket_t *t, const lot_ticket_t *d)
{
    int bm = count_matches(t->main, 7, d->main, 7);
    bool sm = (t->special[0] == d->special[0]);
    if (bm == 7)              return result(1, 5000000u, LEVEL_NAME[1]);
    if (bm == 6 && sm)        return result(2, 200000u,  LEVEL_NAME[2]);
    if (bm == 6)              return result(3, 3000u,    LEVEL_NAME[3]);
    if (bm == 5 && sm)        return result(4, 200u,     LEVEL_NAME[4]);
    if (bm == 5)              return result(5, 50u,      LEVEL_NAME[5]);
    if (bm == 4 && sm)        return result(6, 10u,      LEVEL_NAME[6]);
    if (bm == 4)              return result(7, 5u,       LEVEL_NAME[7]);
    return result(0, 0, "");
}

lot_result_t lot_evaluate(lot_kind_t kind, const lot_ticket_t *ticket,
                          const lot_ticket_t *draw)
{
    switch (kind) {
    case LOT_SSQ: return eval_ssq(ticket, draw);
    case LOT_DLT: return eval_dlt(ticket, draw);
    case LOT_3D:
    case LOT_PL3: return eval_digit3(ticket, draw);
    case LOT_PL5: return eval_pl5(ticket, draw);
    case LOT_QXC: return eval_qxc(ticket, draw);
    case LOT_QLC: return eval_qlc(ticket, draw);
    default: return result(0, 0, "");
    }
}

// ---------------------------------------------------------------- 自检
#include <stdio.h>

static int s_test_fail;

static void expect_level(const char *name, lot_kind_t kind,
                         const lot_ticket_t *t, const lot_ticket_t *d,
                         int level)
{
    lot_result_t r = lot_evaluate(kind, t, d);
    if (r.level != level) {
        s_test_fail++;
        printf("SELFTEST FAIL %s: got level %d want %d\n", name, r.level, level);
    }
}

static void check_unique(const char *name, const uint8_t *arr, int n, int max)
{
    for (int i = 0; i < n; i++) {
        if (arr[i] < 1 || arr[i] > max) {
            s_test_fail++;
            printf("SELFTEST FAIL %s: value %u out of 1..%d\n", name, arr[i], max);
        }
        for (int j = i + 1; j < n; j++) {
            if (arr[i] == arr[j]) {
                s_test_fail++;
                printf("SELFTEST FAIL %s: duplicate %u\n", name, arr[i]);
            }
        }
    }
}

int lot_selftest(void)
{
    s_test_fail = 0;
    lot_ticket_t a, b;

    // 双色球
    memset(&a, 0, sizeof(a));
    a.main[0] = 1; a.main[1] = 2; a.main[2] = 3;
    a.main[3] = 4; a.main[4] = 5; a.main[5] = 6; a.special[0] = 7;
    b = a;
    expect_level("ssq-1st", LOT_SSQ, &a, &b, 1);
    b.main[5] = 7;            // 5+1
    expect_level("ssq-3rd", LOT_SSQ, &a, &b, 3);
    b.main[5] = 8; b.special[0] = 1;   // 5+0
    expect_level("ssq-4th", LOT_SSQ, &a, &b, 4);
    memset(&b, 0, sizeof(b));
    b.main[0] = 1; b.main[1] = 2; b.main[2] = 3;
    b.main[3] = 4; b.main[4] = 5; b.special[0] = 9;  // 5+0
    expect_level("ssq-4th2", LOT_SSQ, &a, &b, 4);

    // 大乐透
    memset(&a, 0, sizeof(a));
    a.main[0] = 1; a.main[1] = 2; a.main[2] = 3; a.main[3] = 4; a.main[4] = 5;
    a.special[0] = 1; a.special[1] = 2;
    b = a;
    expect_level("dlt-1st", LOT_DLT, &a, &b, 1);
    b.special[1] = 3;         // 5+1
    expect_level("dlt-2nd", LOT_DLT, &a, &b, 2);
    b.special[0] = 4; b.special[1] = 5;  // 5+0
    expect_level("dlt-3rd", LOT_DLT, &a, &b, 3);

    // 3D / 排列三
    memset(&a, 0, sizeof(a));
    a.digits[0] = 1; a.digits[1] = 2; a.digits[2] = 3;
    b = a;
    expect_level("3d-direct", LOT_3D, &a, &b, 1);
    memset(&b, 0, sizeof(b));
    b.digits[0] = 3; b.digits[1] = 2; b.digits[2] = 1;
    expect_level("3d-group6", LOT_3D, &a, &b, 3);
    memset(&a, 0, sizeof(a));
    a.digits[0] = 1; a.digits[1] = 1; a.digits[2] = 2;
    memset(&b, 0, sizeof(b));
    b.digits[0] = 1; b.digits[1] = 2; b.digits[2] = 1;
    expect_level("3d-group3", LOT_3D, &a, &b, 2);
    expect_level("pl3-group3", LOT_PL3, &a, &b, 2);

    // 排列五
    memset(&a, 0, sizeof(a));
    for (int i = 0; i < 5; i++) a.digits[i] = (uint8_t)(i + 1);
    b = a;
    expect_level("pl5-direct", LOT_PL5, &a, &b, 1);
    b.digits[4] = 0;
    expect_level("pl5-miss", LOT_PL5, &a, &b, 0);

    // 七星彩
    memset(&a, 0, sizeof(a));
    for (int i = 0; i < 7; i++) a.digits[i] = (uint8_t)(i + 1);
    b = a;
    expect_level("qxc-7", LOT_QXC, &a, &b, 1);
    b.digits[6] = 0;
    expect_level("qxc-6", LOT_QXC, &a, &b, 2);

    // 七乐彩
    memset(&a, 0, sizeof(a));
    for (int i = 0; i < 7; i++) a.main[i] = (uint8_t)(i + 1);
    a.special[0] = 20;
    b = a;
    expect_level("qlc-1st", LOT_QLC, &a, &b, 1);
    b.main[6] = 30; b.special[0] = 20;   // 6+1
    expect_level("qlc-2nd", LOT_QLC, &a, &b, 2);

    // 随机选号：范围与去重
    for (int k = 0; k < LOT_COUNT; k++) {
        const lot_def_t *d = lot_def((lot_kind_t)k);
        for (int n = 0; n < 300; n++) {
            lot_ticket_t t;
            lot_random_ticket((lot_kind_t)k, &t);
            if (d->style == LOT_STYLE_BALL) {
                check_unique(d->name, t.main, d->n_main, d->max_main);
                check_unique(d->name, t.special, d->n_special, d->max_special);
                if (d->special_disjoint) {
                    for (int i = 0; i < d->n_special; i++) {
                        for (int j = 0; j < d->n_main; j++) {
                            if (t.special[i] == t.main[j]) {
                                s_test_fail++;
                                printf("SELFTEST FAIL %s: special hits main\n", d->name);
                            }
                        }
                    }
                }
            }
        }
    }

    // 手动改号：不应产生重复
    for (int k = 0; k < LOT_COUNT; k++) {
        const lot_def_t *d = lot_def((lot_kind_t)k);
        if (d->style != LOT_STYLE_BALL) continue;
        lot_ticket_t t;
        lot_random_ticket((lot_kind_t)k, &t);
        for (int step = 0; step < 200; step++) {
            lot_slot_adjust((lot_kind_t)k, &t, step % lot_slots((lot_kind_t)k), 1);
        }
        check_unique(d->name, t.main, d->n_main, d->max_main);
        check_unique(d->name, t.special, d->n_special, d->max_special);
    }

    printf("SELFTEST %s (%d failures)\n", s_test_fail ? "FAIL" : "PASS", s_test_fail);
    return s_test_fail;
}
