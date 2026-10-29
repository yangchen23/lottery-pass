// main/app_ui.h —— 彩票模拟器界面与状态机
#pragma once

#include "bsp_button.h"

void app_ui_start(void);
void app_audio_start(void);
void app_ui_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev);
