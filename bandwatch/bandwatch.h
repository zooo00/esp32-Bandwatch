#pragma once

#ifndef LV_CONF_INCLUDE_SIMPLE
#define LV_CONF_INCLUDE_SIMPLE 1
#endif

#include <lvgl.h>

void Bandwatch_Init(void);   // Build the UI and start the 5 GHz monitor (called from Lvgl_Init)
void Bandwatch_Loop(void);   // Service host serial commands and stream captured frames (call from loop)
