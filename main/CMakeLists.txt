idf_component_register(
    SRCS "main.c"
         "app_ui.c"
         "lottery.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash
)

# 中文字体子集（tools/extract_symbols.py + lv_font_conv 生成）
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_12.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_20.c"
)
