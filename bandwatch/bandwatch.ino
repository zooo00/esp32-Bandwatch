// Bandwatch - Wi-Fi / BLE / Zigbee activity meter for the Waveshare ESP32-C5-LCD-1.47.
// Observes 802.11 (2.4 + 5 GHz), BLE and 802.15.4 in promiscuous mode, plus a raw-energy spectrum sweep,
// and shows a busy score per channel.
// Streams JSON stats (and optionally raw frames) over USB serial for host/bandwatch_host.py.
#include "Display_ST7789.h"
#include "LVGL_Driver.h"
#include "bandwatch.h"

void setup()
{
  Serial.setTxBufferSize(8192);   // = kSerialTxBuf: room for a burst of captured frames (each line checks for space first)
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);       // never block the UI if nobody is reading the USB serial port
  LCD_Init();
  Set_Backlight(90);
  Lvgl_Init();
}

void loop()
{
  Timer_Loop();
  Bandwatch_Loop();
  delay(2);
}
