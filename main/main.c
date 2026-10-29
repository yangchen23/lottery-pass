// main/main.c —— Lottery Pass：初始化外设，把按键事件送进应用。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "app_ui.h"
#include "lottery.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;

// lottery.c 的随机源：设备上是硬件 RNG。
uint32_t lot_platform_random(void)
{
    return esp_random();
}

// 按键回调运行在 esp_timer 任务上：只入队，不做别的。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_queue) return;
    const input_event_t e = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &e, 0);
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t e;
    for (;;) {
        if (xQueueReceive(s_input_queue, &e, portMAX_DELAY) == pdTRUE) {
            if (!bsp_lvgl_lock(200)) continue;
            app_ui_handle_key(e.btn, e.event);
            bsp_lvgl_unlock();
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Lottery Pass 启动");
    (void)lot_selftest();

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        (void)nvs_flash_erase();
        nvs = nvs_flash_init();
    }

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }
    bsp_display_backlight(100);

    s_input_queue = xQueueCreate(8, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "输入队列创建失败");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
        return;
    }
    if (bsp_audio_init() == ESP_OK) {
        bsp_audio_set_format(16000, 16, 1);
        bsp_audio_set_volume(100);
        app_audio_start();
    } else {
        ESP_LOGW(TAG, "音频不可用，静音运行");
    }
    (void)bsp_battery_init();

    if (xTaskCreate(input_task, "input", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "输入任务创建失败");
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        app_ui_start();
        bsp_lvgl_unlock();
    }

    int mv = bsp_battery_mv();
    ESP_LOGI(TAG, "就绪 · 电池 %d mV", mv);
}
