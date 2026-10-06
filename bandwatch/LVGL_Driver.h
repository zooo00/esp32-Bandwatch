#pragma once

#ifndef LV_CONF_INCLUDE_SIMPLE
#define LV_CONF_INCLUDE_SIMPLE 1
#endif

#include <lvgl.h>
#include "lv_conf.h"
#include <esp_heap_caps.h>
#include "Display_ST7789.h"

#define LVGL_WIDTH    (LCD_WIDTH )
#define LVGL_HEIGHT   LCD_HEIGHT
/* Partial-render draw buffer. /21 (~5.2 KB) instead of /14 (~7.9 KB) reclaims ~2.6 KB of static RAM; in
   PARTIAL mode LVGL just paints each dirty region in more chunks over the synchronous SPI flush, which is
   invisible for this bars+labels UI at ~1-2 Hz. ~2620 px = ~15 full rows per chunk (well above the 1-row
   minimum). If full-page redraws (the mode splash) ever flicker, raise the divisor back toward /14. */
#define LVGL_BUF_LEN  (LVGL_WIDTH * LVGL_HEIGHT / 21)

#define EXAMPLE_LVGL_TICK_PERIOD_MS  5


void Lvgl_print(const char * buf);

// Targeting LVGL v9 (Arduino LVGL library). Only the v9 driver APIs are declared to avoid
// compilation failures due to removed v8 types (lv_disp_drv_t, lv_indev_drv_t, etc.).
void Lvgl_Display_LCD( lv_display_t *disp, const lv_area_t *area, uint8_t *px_map );          // Displays LVGL content on the LCD
void Lvgl_Touchpad_Read( lv_indev_t * indev, lv_indev_data_t * data );                        // Read the touchpad
void example_increase_lvgl_tick(void *arg);

void Lvgl_Init(void);
void Timer_Loop(void);
