// LCD pages for Bandwatch (172 x 320 portrait, LVGL v9). Only the visible page exists as widgets — see
// showPage(): the C5 has no PSRAM and the Wi-Fi/BLE stacks need most of the 320 KB, so ~300 permanent
// widgets are not affordable. Everything here runs on the loop task (uiTimerCb at kUiIntervalMs cadence,
// pollButton from Bandwatch_Loop); no other task touches LVGL.
#include "bandwatch.h"        // pulls in lvgl
#include "bandwatch_core.h"
#include "surv_ouis.h"   // kSurvName for the Devices page

// Shared with host_proto.cpp (the "band" command + the dwell heartbeat); declared in bandwatch_core.h.
int currentPage = 0;
uint16_t lastApSeen = 0;

namespace {

constexpr int kDevRows = 12;   // rows on the Devices page (and the devRows[] scratch below)

// ---------------------------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------------------------
inline lv_color_t c565(uint16_t v) {
    const uint8_t r5 = (v >> 11) & 0x1F;
    const uint8_t g6 = (v >> 5) & 0x3F;
    const uint8_t b5 = v & 0x1F;
    const uint8_t r8 = (uint16_t(r5) * 255) / 31;
    const uint8_t g8 = (uint16_t(g6) * 255) / 63;
    const uint8_t b8 = (uint16_t(b5) * 255) / 31;
    return lv_color_make(r8, g8, b8);
}

constexpr uint16_t BG_565      = 0x0122; // deep blue/black
constexpr uint16_t PANEL_565   = 0x0843; // muted navy
constexpr uint16_t WHITE_565   = 0xFFFF;
constexpr uint16_t BLACK_565   = 0x0000;
constexpr uint16_t GREEN_565   = 0x07E0;
constexpr uint16_t RED_565     = 0xF800;
constexpr uint16_t CYAN_565    = 0x07FF;
constexpr uint16_t GREY_565    = 0x8410;
constexpr uint16_t DIM_565     = 0x2124;
constexpr uint16_t YELLOW_565  = 0xFFE0;
constexpr uint16_t ORANGE_565  = 0xFD20;

constexpr RgbColor LED_GREEN  = {0, 180, 40};
constexpr RgbColor LED_YELLOW = {255, 200, 0};
constexpr RgbColor LED_ORANGE = {255, 120, 0};
constexpr RgbColor LED_RED    = {255, 24, 0};
constexpr RgbColor LED_BLUE   = {0, 60, 255};
constexpr RgbColor LED_CYAN_L = {0, 190, 210};   // recording pcap to the host (Mac)
constexpr RgbColor LED_MAGENTA= {220, 0, 180};   // recording pcap to the microSD card
constexpr RgbColor LED_WHITE  = {210, 210, 210}; // both sinks at once

// UI objects
lv_obj_t* pages[PAGE_COUNT] = {nullptr};
// overview
lv_obj_t* chanLabel = nullptr;
lv_obj_t* globalBar = nullptr;
lv_obj_t* globalLabel = nullptr;
lv_obj_t* sweepLabel = nullptr;
lv_obj_t* topRows[3] = {nullptr};
lv_obj_t* topRates[3] = {nullptr};
lv_obj_t* topBars[3] = {nullptr};
lv_obj_t* specBars[kChannelCount] = {nullptr};
lv_obj_t* specLabel = nullptr;
lv_obj_t* statsLine1 = nullptr;
lv_obj_t* statsLine2 = nullptr;
lv_obj_t* footLabel = nullptr;
// channels
lv_obj_t* listChanLabel = nullptr;
constexpr int kListSlots = 39;   // 3 columns x 13 rows
lv_obj_t* listRow[kListSlots] = {nullptr};
lv_obj_t* listName[kListSlots] = {nullptr};
lv_obj_t* listBar[kListSlots] = {nullptr};
lv_obj_t* listVal[kListSlots] = {nullptr};
lv_obj_t* listFoot = nullptr;
// devices
lv_obj_t* devHdrRight = nullptr;
lv_obj_t* devRow[kDevRows] = {nullptr};
lv_obj_t* devName[kDevRows] = {nullptr};
lv_obj_t* devRssi[kDevRows] = {nullptr};
lv_obj_t* devBar[kDevRows] = {nullptr};
lv_obj_t* devFoot = nullptr;
// hunt
lv_obj_t* huntHdrRight = nullptr;
lv_obj_t* huntBig = nullptr;
lv_obj_t* huntBar = nullptr;
lv_obj_t* huntMacLbl = nullptr;
lv_obj_t* huntNameLbl = nullptr;
lv_obj_t* huntInfo1 = nullptr;
lv_obj_t* huntInfo2 = nullptr;
lv_obj_t* huntHint = nullptr;
// system
constexpr int kSysLines = 14;
lv_obj_t* sysLines[kSysLines] = {nullptr};
// spectrum (energy-detect): one bar per 15.4 channel; non-15.4 entries stay nullptr
lv_obj_t* spChanLabel = nullptr;   // header right: current bin
lv_obj_t* spPeakLabel = nullptr;
lv_obj_t* spStatsLabel = nullptr;
constexpr int kLcdSpecBars = 42;   // fixed LCD bar slots; the fine bins (up to 84) map onto these (max-per-group)
lv_obj_t* spBars[kLcdSpecBars] = {nullptr};
lv_obj_t* spFoot = nullptr;
// mode splash: a full-screen name card flashed over whatever page is up when the band changes; while BOOT is held
// it steps through the modes' splashes and release commits the one on screen (pollButton + showBandSplash)
lv_obj_t* splashBg = nullptr;
lv_obj_t* splashTitle = nullptr;
lv_obj_t* splashSub = nullptr;
uint32_t splashStartMs = 0, splashDurMs = 0;

struct SplashText { const char* title; const char* sub; };
constexpr SplashText kSplash[] = {   // indexed by BandMode (kBandName is too terse for a full-screen card)
    {"5 GHz",         "Wi-Fi ch 36-165"},
    {"2.4 GHz",       "Wi-Fi ch 1-13"},
    {"Both bands",    "ch 1-13 + 36-165"},
    {"BLE",           "Bluetooth Low Energy"},
    {"Zigbee",        "802.15.4 ch 11-26"},   // "Zigbee / 15.4" is ~182 px at _28, just too wide for the card
    {"Spectrum",      "raw 2.4 GHz energy"},
};

uint16_t apMaxWindow = 0;
uint32_t apWindowStartedMs = 0;

lv_color_t scoreColor(float s) {
    if (s > 70.0f) return c565(RED_565);
    if (s > 40.0f) return c565(YELLOW_565);
    return c565(GREEN_565);
}

lv_color_t rssiColor(int rssi) {
    if (rssi >= -50) return c565(RED_565);
    if (rssi >= -65) return c565(ORANGE_565);
    if (rssi >= -80) return c565(YELLOW_565);
    return c565(CYAN_565);
}

void fmtRate(char* out, size_t n, float perSec, const char* unit) {
    if (perSec >= 10000.0f) snprintf(out, n, "%.0fk%s", perSec / 1000.0f, unit);
    else if (perSec >= 1000.0f) snprintf(out, n, "%.1fk%s", perSec / 1000.0f, unit);
    else snprintf(out, n, "%.0f%s", perSec, unit);
}

// ---------------------------------------------------------------------------------------------
// UI (172 x 320 portrait) – pages, BOOT button cycles the ones that apply
// ---------------------------------------------------------------------------------------------
lv_obj_t* make_label(lv_obj_t* parent, const char* txt, lv_color_t color, const lv_font_t* font = nullptr) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_color(lbl, color, 0);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    return lbl;
}

lv_obj_t* make_panel(lv_obj_t* parent, int height, uint16_t bg, int pad) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_PCT(100), height);
    lv_obj_set_style_bg_color(p, c565(bg), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 4, 0);
    lv_obj_set_style_pad_all(p, pad, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t* make_page(lv_obj_t* parent) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, c565(BG_565), 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    lv_obj_set_style_pad_row(p, 4, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t* make_header(lv_obj_t* page, const char* title, lv_obj_t** rightLabel) {
    lv_obj_t* header = make_panel(page, 26, PANEL_565, 4);
    lv_obj_t* t = make_label(header, title, c565(WHITE_565), &lv_font_montserrat_14);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 2, 0);
    // _12 (not _14): the right label carries live strings ("park 165 USB") that would collide with long titles.
    *rightLabel = make_label(header, "", c565(CYAN_565), &lv_font_montserrat_12);
    lv_obj_align(*rightLabel, LV_ALIGN_RIGHT_MID, -2, 0);
    return header;
}

lv_obj_t* make_row(lv_obj_t* parent, int height, int colGap) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, colGap, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t* make_bar(lv_obj_t* parent, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, c565(BLACK_565), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    return bar;
}

void buildOverviewPage(lv_obj_t* page) {
    make_header(page, "Activity", &chanLabel);   // was "Bandwatch" (the product name) - ambiguous next to Channels/Devices/...

    lv_obj_t* globalWrap = make_panel(page, 56, PANEL_565, 6);
    globalLabel = make_label(globalWrap, "0", c565(WHITE_565), &lv_font_montserrat_20);
    lv_obj_align(globalLabel, LV_ALIGN_TOP_LEFT, 0, -2);
    lv_obj_t* gl = make_label(globalWrap, "max busy", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(gl, LV_ALIGN_TOP_LEFT, 40, 3);
    sweepLabel = make_label(globalWrap, "sweep 0", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(sweepLabel, LV_ALIGN_TOP_RIGHT, 0, 3);
    globalBar = lv_bar_create(globalWrap);
    lv_bar_set_range(globalBar, 0, 100);
    lv_obj_set_size(globalBar, LV_PCT(100), 14);
    lv_obj_align(globalBar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(globalBar, c565(BLACK_565), 0);
    lv_obj_set_style_bg_opa(globalBar, LV_OPA_40, 0);

    lv_obj_t* topBox = make_panel(page, 92, BG_565, 2);
    lv_obj_set_style_pad_row(topBox, 3, 0);
    lv_obj_set_flex_flow(topBox, LV_FLEX_FLOW_COLUMN);
    make_label(topBox, "Top 3", c565(YELLOW_565), &lv_font_montserrat_12);
    for (int i = 0; i < 3; i++) {
        // "ch165 100" is ~72 px at _14: the label box must hold it on one line or it wraps into the row below.
        lv_obj_t* row = make_row(topBox, 20, 4);
        topRows[i] = make_label(row, "--", c565(WHITE_565), &lv_font_montserrat_14);
        lv_obj_set_width(topRows[i], 72);
        topBars[i] = make_bar(row, 42, 10);
        topRates[i] = make_label(row, "", c565(GREY_565), &lv_font_montserrat_12);
    }

    lv_obj_t* spec = make_panel(page, 58, PANEL_565, 4);
    lv_obj_t* barsRow = lv_obj_create(spec);
    lv_obj_set_size(barsRow, LV_PCT(100), 36);
    lv_obj_align(barsRow, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(barsRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(barsRow, 0, 0);
    lv_obj_set_style_pad_all(barsRow, 0, 0);
    lv_obj_set_style_pad_column(barsRow, 1, 0);
    lv_obj_set_flex_flow(barsRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barsRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(barsRow, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < kChannelCount; i++) {
        lv_obj_t* b = lv_obj_create(barsRow);
        lv_obj_set_size(b, 5, 2);
        lv_obj_set_style_bg_color(b, c565(DIM_565), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        for (int g = 1; g < 5; g++) if (i == kGroupStart[g]) lv_obj_set_style_margin_left(b, 3, 0);
        specBars[i] = b;
    }
    specLabel = make_label(spec, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(specLabel, LV_PCT(100));
    lv_obj_set_style_text_align(specLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(specLabel, LV_ALIGN_BOTTOM_MID, 0, 2);

    lv_obj_t* stats = make_panel(page, 40, BG_565, 2);
    statsLine1 = make_label(stats, "", c565(WHITE_565), &lv_font_montserrat_12);
    lv_obj_align(statsLine1, LV_ALIGN_TOP_LEFT, 0, 0);
    statsLine2 = make_label(stats, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(statsLine2, LV_ALIGN_TOP_LEFT, 0, 18);

    footLabel = make_label(page, "APs --", c565(YELLOW_565), &lv_font_montserrat_14);
    lv_obj_set_width(footLabel, LV_PCT(100));
    lv_obj_set_style_text_align(footLabel, LV_TEXT_ALIGN_CENTER, 0);
}

void buildChannelsPage(lv_obj_t* page) {
    make_header(page, "Channels", &listChanLabel);
    lv_obj_t* grid = make_panel(page, 262, BG_565, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, 2, 0);
    lv_obj_t* cols[3];
    for (int c = 0; c < 3; c++) {
        cols[c] = lv_obj_create(grid);
        lv_obj_set_size(cols[c], 53, LV_PCT(100));
        lv_obj_set_style_bg_opa(cols[c], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(cols[c], 0, 0);
        lv_obj_set_style_pad_all(cols[c], 0, 0);
        lv_obj_set_style_pad_row(cols[c], 0, 0);
        lv_obj_set_flex_flow(cols[c], LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(cols[c], LV_OBJ_FLAG_SCROLLABLE);
    }
    for (int i = 0; i < kListSlots; i++) {
        lv_obj_t* row = make_row(cols[i / 13], 20, 2);
        listRow[i] = row;
        listName[i] = make_label(row, "", c565(WHITE_565), &lv_font_montserrat_12);
        lv_obj_set_width(listName[i], 20);
        lv_obj_set_style_text_align(listName[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(listName[i], LV_LABEL_LONG_CLIP);   // "165" just fits; no wrapping into the next row
        listBar[i] = make_bar(row, 11, 8);
        listVal[i] = make_label(row, "-", c565(GREY_565), &lv_font_montserrat_12);
        lv_obj_set_width(listVal[i], 18);   // a maxed "100" is ~22 px: CLIP keeps it inside its column
        lv_label_set_long_mode(listVal[i], LV_LABEL_LONG_CLIP);
    }
    listFoot = make_label(page, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(listFoot, LV_PCT(100));
    lv_obj_set_style_text_align(listFoot, LV_TEXT_ALIGN_CENTER, 0);
}

void buildDevicesPage(lv_obj_t* page) {
    make_header(page, "Devices", &devHdrRight);
    lv_obj_t* box = make_panel(page, 246, BG_565, 0);
    lv_obj_set_style_pad_row(box, 0, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < kDevRows; i++) {
        lv_obj_t* row = make_row(box, 20, 3);
        devRow[i] = row;
        devName[i] = make_label(row, "", c565(WHITE_565), &lv_font_montserrat_12);
        lv_obj_set_width(devName[i], 84);
        lv_label_set_long_mode(devName[i], LV_LABEL_LONG_CLIP);
        devRssi[i] = make_label(row, "", c565(GREY_565), &lv_font_montserrat_12);
        lv_obj_set_width(devRssi[i], 26);
        lv_obj_set_style_text_align(devRssi[i], LV_TEXT_ALIGN_RIGHT, 0);
        devBar[i] = make_bar(row, 44, 8);
    }
    devFoot = make_label(page, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(devFoot, LV_PCT(100));
    lv_obj_set_style_text_align(devFoot, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(devFoot, LV_LABEL_LONG_CLIP);
}

void buildHuntPage(lv_obj_t* page) {
    make_header(page, "Hunt", &huntHdrRight);
    lv_obj_t* box = make_panel(page, 120, PANEL_565, 6);
    huntBig = make_label(box, "--", c565(WHITE_565), &lv_font_montserrat_28);
    lv_obj_align(huntBig, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_t* unit = make_label(box, "dBm", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 40);
    huntBar = lv_bar_create(box);
    lv_bar_set_range(huntBar, 0, 100);
    lv_obj_set_size(huntBar, LV_PCT(100), 22);
    lv_obj_align(huntBar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(huntBar, c565(BLACK_565), 0);
    lv_obj_set_style_bg_opa(huntBar, LV_OPA_40, 0);
    lv_obj_t* lab = make_label(box, "far", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(lab, LV_ALIGN_BOTTOM_LEFT, 0, -24);
    lab = make_label(box, "near", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(lab, LV_ALIGN_BOTTOM_RIGHT, 0, -24);

    lv_obj_t* info = make_panel(page, 120, BG_565, 2);
    lv_obj_set_style_pad_row(info, 4, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    huntMacLbl = make_label(info, "", c565(CYAN_565), &lv_font_montserrat_14);
    huntNameLbl = make_label(info, "", c565(WHITE_565), &lv_font_montserrat_14);
    lv_obj_set_width(huntNameLbl, LV_PCT(100));
    lv_label_set_long_mode(huntNameLbl, LV_LABEL_LONG_CLIP);
    huntInfo1 = make_label(info, "", c565(GREY_565), &lv_font_montserrat_12);
    huntInfo2 = make_label(info, "", c565(GREY_565), &lv_font_montserrat_12);
    huntHint = make_label(page, "hold to stop the hunt", c565(GREY_565), &lv_font_montserrat_12);   // one line: ~140 px in a 164 box
    lv_obj_set_width(huntHint, LV_PCT(100));
    lv_obj_set_style_text_align(huntHint, LV_TEXT_ALIGN_CENTER, 0);
}

void buildSystemPage(lv_obj_t* page) {
    lv_obj_t* hdrRight;
    make_header(page, "System", &hdrRight);
    char v[16];   // "v1.10.0" already overflowed the old char[8]
    snprintf(v, sizeof(v), "v%s", kVersion);
    lv_label_set_text(hdrRight, v);
    lv_obj_t* box = make_panel(page, 280, BG_565, 2);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < kSysLines; i++) {
        sysLines[i] = make_label(box, "", c565(WHITE_565), &lv_font_montserrat_12);
        lv_obj_set_width(sysLines[i], LV_PCT(100));
        lv_label_set_long_mode(sysLines[i], LV_LABEL_LONG_CLIP);
    }
}

void buildSpectrumPage(lv_obj_t* page) {
    make_header(page, "Spectrum", &spChanLabel);

    lv_obj_t* info = make_panel(page, 46, PANEL_565, 6);
    spPeakLabel = make_label(info, "-- dBm", c565(WHITE_565), &lv_font_montserrat_20);
    lv_obj_align(spPeakLabel, LV_ALIGN_TOP_LEFT, 0, -2);
    lv_obj_t* il = make_label(info, "peak", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(il, LV_ALIGN_TOP_RIGHT, 0, 3);
    spStatsLabel = make_label(info, "scanning...", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(spStatsLabel, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t* spec = make_panel(page, 150, PANEL_565, 4);
    lv_obj_t* barsRow = lv_obj_create(spec);
    lv_obj_set_size(barsRow, LV_PCT(100), 124);
    lv_obj_align(barsRow, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(barsRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(barsRow, 0, 0);
    lv_obj_set_style_pad_all(barsRow, 0, 0);
    lv_obj_set_style_pad_column(barsRow, 2, 0);
    lv_obj_set_flex_flow(barsRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barsRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(barsRow, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < kLcdSpecBars; i++) {
        lv_obj_t* b = lv_obj_create(barsRow);
        lv_obj_set_size(b, 3, 2);
        lv_obj_set_style_bg_color(b, c565(DIM_565), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 1, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        spBars[i] = b;
    }
    spFoot = make_label(spec, "2400    2440    2483 MHz", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(spFoot, LV_PCT(100));
    lv_obj_set_style_text_align(spFoot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(spFoot, LV_ALIGN_BOTTOM_MID, 0, 2);
}

bool pageAvailable(int n) {
    if (n == PAGE_SPECTRUM) return modeSpec();
    if (n == PAGE_OVERVIEW || n == PAGE_CHANNELS) return hopMode() && !modeSpec();
    if (n == PAGE_DEVICES) return !modeSpec();   // no decoded devices in energy-detect mode
    if (n == PAGE_HUNT) return hunt.active;
    return true;   // system page
}

} // namespace

// Only the visible page exists as LVGL objects (the others are rebuilt on demand): the C5 has no PSRAM and
// the Wi-Fi/BLE stacks need most of the 320 KB, so ~300 permanent widgets are not affordable.
void showPage(int n) {
    n = ((n % PAGE_COUNT) + PAGE_COUNT) % PAGE_COUNT;
    for (int tries = 0; tries < PAGE_COUNT && !pageAvailable(n); tries++) n = (n + 1) % PAGE_COUNT;
    if (pages[currentPage] && n == currentPage) return;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (pages[i]) { lv_obj_delete(pages[i]); pages[i] = nullptr; }
    }
    currentPage = n;
    lv_obj_t* pg = make_page(lv_scr_act());
    pages[n] = pg;
    switch (n) {
        case PAGE_OVERVIEW: buildOverviewPage(pg); break;
        case PAGE_CHANNELS: buildChannelsPage(pg); break;
        case PAGE_SPECTRUM: buildSpectrumPage(pg); break;
        case PAGE_DEVICES:  buildDevicesPage(pg); break;
        case PAGE_HUNT:     buildHuntPage(pg); break;
        default:            buildSystemPage(pg); break;
    }
}

namespace {
inline bool splashActive() { return splashDurMs > 0 && (millis() - splashStartMs) < splashDurMs; }
} // namespace

void showBandSplash(BandMode m, uint32_t durMs) {
    splashStartMs = millis();
    splashDurMs = durMs;
    lv_label_set_text(splashTitle, kSplash[m].title);
    lv_label_set_text(splashSub, kSplash[m].sub);
    lv_obj_remove_flag(splashBg, LV_OBJ_FLAG_HIDDEN);
    const int n = lv_obj_get_child_count(lv_scr_act());      // keep it above any page just (re)built below it
    if (n > 0) lv_obj_move_to_index(splashBg, n - 1);
}

void buildUi() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, c565(BG_565), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    showPage(PAGE_OVERVIEW);

    // Splash overlay: created after the first page so it starts above it; hidden until a band change.
    splashBg = lv_obj_create(scr);
    lv_obj_set_size(splashBg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(splashBg, 0, 0);
    lv_obj_set_style_bg_color(splashBg, c565(BG_565), 0);
    lv_obj_set_style_border_width(splashBg, 0, 0);
    lv_obj_remove_flag(splashBg, LV_OBJ_FLAG_SCROLLABLE);
    splashTitle = make_label(splashBg, "", c565(WHITE_565), &lv_font_montserrat_28);
    lv_obj_align(splashTitle, LV_ALIGN_CENTER, 0, -14);
    splashSub = make_label(splashBg, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_align(splashSub, LV_ALIGN_CENTER, 0, 18);
    lv_obj_add_flag(splashBg, LV_OBJ_FLAG_HIDDEN);
    showBandSplash(bandMode, kSplashShowMs);   // name the boot mode while Wi-Fi is still coming up
}

namespace {

bool recording() { return sd.capEnabled || captureEnabled; }

// Paint a header label red while recording so the LCD has an unmistakable record light, not just a word.
void applyRecColor(lv_obj_t* lbl) {
    if (!lbl) return;
    lv_obj_set_style_text_color(lbl, recording() ? c565(RED_565) : c565(GREY_565), 0);
}

// "SD", "USB" or "REC" (both) while a capture is running; empty otherwise.
const char* recTag() {
    if (sd.capEnabled && captureEnabled) return " REC";
    if (sd.capEnabled) return " SD";
    if (captureEnabled) return " USB";
    return "";
}

void chanHeaderText(char* buf, size_t n) {
    if (!hopMode()) { snprintf(buf, n, "BLE"); return; }
    const char* band = (currentIdx >= 0 && is154(currentIdx)) ? "15.4" : (currentIdx >= 0 && is5g(currentIdx)) ? "5G" : "2.4G";
    if (!monitorReady)       snprintf(buf, n, "no ch%s", recTag());
    else if (parkedIdx >= 0) snprintf(buf, n, "park %u%s", kChannels[currentIdx], recTag());
    // In BOTH mode the per-band prefix would just flip ("2.4G ch6" / "5G ch36") and read like single-band mode;
    // anchor the set instead - the number says which side (36+ = 5 GHz), and the strip below highlights it too.
    else if (bandMode == BAND_BOTH) snprintf(buf, n, "BOTH ch%u%s", kChannels[currentIdx], recTag());
    else                            snprintf(buf, n, "%s ch%u%s", band, kChannels[currentIdx], recTag());
}

void refreshOverview(float global) {
    char buf[64];
    char r1[16], r2[16];
    lv_bar_set_value(globalBar, static_cast<int>(global + 0.5f), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(globalBar, scoreColor(global), LV_PART_INDICATOR);
    snprintf(buf, sizeof(buf), "%.0f", global);
    lv_label_set_text(globalLabel, buf);
    snprintf(buf, sizeof(buf), "sweep %lu", static_cast<unsigned long>(sweepCount));
    lv_label_set_text(sweepLabel, buf);
    chanHeaderText(buf, sizeof(buf));
    lv_label_set_text(chanLabel, buf);
    applyRecColor(chanLabel);

    int top[3];
    sortTop3(top);
    for (int i = 0; i < 3; i++) {
        if (top[i] < 0) {
            lv_label_set_text(topRows[i], "--");
            lv_label_set_text(topRates[i], "");
            lv_bar_set_value(topBars[i], 0, LV_ANIM_OFF);
            continue;
        }
        const ChannelState& ch = channels[top[i]];
        snprintf(buf, sizeof(buf), "ch%u %.0f", kChannels[top[i]], ch.busyEma);
        lv_label_set_text(topRows[i], buf);
        lv_bar_set_value(topBars[i], static_cast<int>(ch.busyEma + 0.5f), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(topBars[i], scoreColor(ch.busyEma), LV_PART_INDICATOR);
        fmtRate(r1, sizeof(r1), ch.metrics.frames * 1000.0f / kDwellMs, "/s");
        lv_label_set_text(topRates[i], r1);
    }

    const int nEnabled = enabledCount();
    int barW = (150 / (nEnabled > 0 ? nEnabled : 1)) - 1;
    if (barW < 2) barW = 2;
    if (barW > 10) barW = 10;
    for (int i = 0; i < kChannelCount; i++) {
        const ChannelState& ch = channels[i];
        if (!chanEnabled(i)) { lv_obj_add_flag(specBars[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(specBars[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(specBars[i], barW);
        int h = 2;
        lv_color_t col = c565(DIM_565);
        if (ch.unavailable) {
            col = c565(BLACK_565);
        } else if (ch.hasData) {
            h = 2 + static_cast<int>(ch.busyEma * 34.0f / 100.0f);
            col = (ch.busyEma < 3.0f) ? c565(GREY_565) : scoreColor(ch.busyEma);
        }
        if (i == currentIdx && monitorReady) col = c565(CYAN_565);
        lv_obj_set_height(specBars[i], h);
        lv_obj_set_style_bg_color(specBars[i], col, 0);
    }
    lv_label_set_text(specLabel, bandMode == BAND_5G ? "36-64   100-144   149-165"
                                 : bandMode == BAND_24G ? "2.4 GHz channels 1-13"
                                 : bandMode == BAND_154 ? "802.15.4 channels 11-26"
                                 : "1-13 + 36-165");   // the full list is ~190 px and would wrap in this panel

    const int curIdx = currentIdx < 0 ? 0 : currentIdx;
    const ChannelState& cur = channels[curIdx];
    if (cur.hasData) {
        fmtRate(r1, sizeof(r1), cur.metrics.frames * 1000.0f / kDwellMs, " pkt/s");
        fmtRate(r2, sizeof(r2), cur.metrics.bytes * 1000.0f / kDwellMs, " B/s");
        snprintf(buf, sizeof(buf), "ch%u: %s  %s", kChannels[curIdx], r1, r2);
        lv_label_set_text(statsLine1, buf);
        snprintf(buf, sizeof(buf), "talkers %u  strong %u/%lu  raw %.0f", cur.metrics.unique, cur.metrics.strong,
                 static_cast<unsigned long>(cur.metrics.frames), cur.busyCurrent);
        lv_label_set_text(statsLine2, buf);
    } else {
        lv_label_set_text(statsLine1, "listening...");
        lv_label_set_text(statsLine2, "");
    }

    if (captureEnabled) {
        snprintf(buf, sizeof(buf), "APs %u  " LV_SYMBOL_DOWNLOAD " %lu  drop %lu", lastApSeen,
                 static_cast<unsigned long>(capSent), static_cast<unsigned long>(capDropped));
    } else {
        const int q = quietestChannel();   // C8: name the quietest channel when the line has room (capture off)
        if (q >= 0)
            snprintf(buf, sizeof(buf), "APs %u   quiet ch%u %.0f", lastApSeen, kChannels[q], channels[q].busyEma);
        else
            snprintf(buf, sizeof(buf), "APs %u", lastApSeen);
    }
    lv_label_set_text(footLabel, buf);
}

void refreshChannels(float global) {
    char buf[48];
    chanHeaderText(buf, sizeof(buf));
    lv_label_set_text(listChanLabel, buf);
    applyRecColor(listChanLabel);
    int slot = 0;
    for (int i = 0; i < kChannelCount && slot < kListSlots; i++) {
        if (!chanEnabled(i)) continue;
        const ChannelState& ch = channels[i];
        lv_obj_remove_flag(listRow[slot], LV_OBJ_FLAG_HIDDEN);
        snprintf(buf, sizeof(buf), "%u", kChannels[i]);
        lv_label_set_text(listName[slot], buf);
        lv_obj_set_style_text_color(listName[slot], (i == currentIdx && monitorReady) ? c565(CYAN_565)
                                                    : ch.unavailable ? c565(GREY_565) : c565(WHITE_565), 0);
        if (ch.unavailable) {
            lv_label_set_text(listVal[slot], "x");
            lv_bar_set_value(listBar[slot], 0, LV_ANIM_OFF);
        } else if (!ch.hasData) {
            lv_label_set_text(listVal[slot], "-");
            lv_bar_set_value(listBar[slot], 0, LV_ANIM_OFF);
        } else {
            snprintf(buf, sizeof(buf), "%.0f", ch.busyEma);
            lv_label_set_text(listVal[slot], buf);
            lv_bar_set_value(listBar[slot], static_cast<int>(ch.busyEma + 0.5f), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(listBar[slot], scoreColor(ch.busyEma), LV_PART_INDICATOR);
        }
        slot++;
    }
    for (; slot < kListSlots; slot++) lv_obj_add_flag(listRow[slot], LV_OBJ_FLAG_HIDDEN);
    snprintf(buf, sizeof(buf), "max %.0f  sweep %lu  APs %u", global, static_cast<unsigned long>(sweepCount), lastApSeen);
    lv_label_set_text(listFoot, buf);
}

inline int rssiPct(int rssi) {   // -100 dBm -> 0, -30 dBm -> 100
    int p = (rssi + 100) * 100 / 70;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

struct DevRowInfo { uint8_t mac[6]; int8_t rssi; bool ap; uint8_t surv; bool destOnly; char label[33]; };
DevRowInfo devRows[kDevRows];
int devRowCount = 0;

void refreshDevices() {
    static uint32_t lastSortMs = 0;
    const uint32_t now = millis();
    if (now - lastSortMs >= 500) {
        lastSortMs = now;
        // Collect RSSI-sorted refs, then fetch the top kDevRows full records one at a time (the page shows only
        // the loudest 12). A ref whose slot changed since collect fails fetch and is skipped, so `r` indexes refs
        // while `out` counts rows actually filled.
        if (mode154()) {
            static const char* const kProto[] = {"15.4", "ZigB", "ZGP", "Thrd", "sec"};
            const int zn = collect154Refs(g_devRefs, kDev154Slots, kDevLcdFreshMs);
            int out = 0;
            for (int r = 0; r < zn && out < kDevRows; r++) {
                Dev154 d;
                if (!fetch154Dev(g_devRefs[r], d, kDevLcdFreshMs)) continue;
                memcpy(devRows[out].mac, d.key, 6);
                devRows[out].rssi = d.rssi;
                devRows[out].ap = d.flags & 2;
                devRows[out].surv = 0;
                devRows[out].destOnly = false;
                if (d.flags & 1) snprintf(devRows[out].label, 33, "%s %02x%02x%02x", kProto[d.proto < 5 ? d.proto : 0], d.key[5], d.key[6], d.key[7]);
                else snprintf(devRows[out].label, 33, "%s %04x", kProto[d.proto < 5 ? d.proto : 0], d.shortAddr);
                if (d.flags & 4) strncat(devRows[out].label, " join", 32 - strlen(devRows[out].label));
                out++;
            }
            devRowCount = out;
        } else if (wifiMode()) {
            const int wn = collectWifiRefs(g_devRefs, kWifiDevSlots, kDevLcdFreshMs);
            int out = 0;
            for (int r = 0; r < wn && out < kDevRows; r++) {
                WifiDev d;
                if (!fetchWifiDev(g_devRefs[r], d, kDevLcdFreshMs)) continue;
                memcpy(devRows[out].mac, d.mac, 6);
                devRows[out].rssi = d.rssi;
                devRows[out].ap = d.flags & 1;
                devRows[out].surv = d.surv;
                devRows[out].destOnly = d.flags & 4;
                strncpy(devRows[out].label, d.ssid, 32); devRows[out].label[32] = 0;
                out++;
            }
            devRowCount = out;
        } else {
            const int bn = collectBleRefs(g_devRefs, kBleDevSlots, kDevLcdFreshMs);
            int out = 0;
            for (int r = 0; r < bn && out < kDevRows; r++) {
                BleDev d;
                if (!fetchBleDev(g_devRefs[r], d, kDevLcdFreshMs)) continue;
                memcpy(devRows[out].mac, d.mac, 6);
                devRows[out].rssi = d.rssi;
                devRows[out].ap = false;
                devRows[out].surv = d.surv;
                devRows[out].destOnly = false;
                strncpy(devRows[out].label, d.name, 32); devRows[out].label[32] = 0;
                out++;
            }
            devRowCount = out;
        }
    }
    char buf[48];
    const int n = devRowCount;
    if (bandMode == BAND_BLE)
        snprintf(buf, sizeof(buf), "BLE %d %s%s", n, bleScan.active ? "act" : "psv", recTag());
    else
        snprintf(buf, sizeof(buf), "%s %d%s", mode154() ? "15.4" : "WiFi", n, recTag());
    lv_label_set_text(devHdrRight, buf);
    applyRecColor(devHdrRight);
    for (int i = 0; i < kDevRows; i++) {
        if (i >= n) { lv_obj_add_flag(devRow[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(devRow[i], LV_OBJ_FLAG_HIDDEN);
        const uint8_t* mac = devRows[i].mac;
        const int rssi = devRows[i].rssi;
        const char* label = devRows[i].label;
        const bool ap = devRows[i].ap;
        // "!" marks known surveillance hardware, "~" a device only ever seen as a destination (tier 1).
        const char* pfx = devRows[i].surv ? "! " : devRows[i].destOnly ? "~ " : ap ? "* " : "";
        if (devRows[i].surv)      snprintf(buf, sizeof(buf), "%s%s", pfx, kSurvName[devRows[i].surv]);
        else if (label[0])        snprintf(buf, sizeof(buf), "%s%s", pfx, label);
        else                      snprintf(buf, sizeof(buf), "%s%02x:%02x:%02x", pfx, mac[3], mac[4], mac[5]);
        lv_label_set_text(devName[i], buf);
        const bool hunted = hunt.active && (hunt.kind == 1 ? memcmp(mac, hunt.key, 6) == 0 : macEq(mac, hunt.mac));
        lv_obj_set_style_text_color(devName[i], devRows[i].surv ? c565(ORANGE_565)
                                               : hunted ? c565(CYAN_565)
                                               : devRows[i].destOnly ? c565(GREY_565) : c565(WHITE_565), 0);
        snprintf(buf, sizeof(buf), "%d", rssi);
        lv_label_set_text(devRssi[i], buf);
        lv_bar_set_value(devBar[i], rssiPct(rssi), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(devBar[i], rssiColor(rssi), LV_PART_INDICATOR);
    }
    if (mode154()) snprintf(buf, sizeof(buf), "* = beacons (router)  seen < 20 s");
    else if (wifiMode()) snprintf(buf, sizeof(buf), "* = AP (beacons)  seen < 20 s");
    else snprintf(buf, sizeof(buf), "BLE scan cycle %lu  seen < 20 s", static_cast<unsigned long>(bleScan.cycles));
    lv_label_set_text(devFoot, buf);
}

void refreshHunt() {
    char buf[64];
    const uint32_t last = hunt.lastMs;
    const int rssi = hunt.rssi;
    const bool seen = last != 0;
    const uint32_t age = seen ? millis() - last : 0;
    lv_label_set_text(huntHdrRight, recording() ? recTag() + 1 : hopMode() ? (parkedIdx >= 0 ? "parked" : "hopping") : "BLE");
    applyRecColor(huntHdrRight);
    if (seen && age < 5000) snprintf(buf, sizeof(buf), "%d", rssi);
    else snprintf(buf, sizeof(buf), "--");
    lv_label_set_text(huntBig, buf);
    lv_bar_set_value(huntBar, (seen && age < 5000) ? rssiPct(rssi) : 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(huntBar, rssiColor(rssi), LV_PART_INDICATOR);
    huntIdText(buf, sizeof(buf));
    lv_label_set_text(huntMacLbl, buf);
    if (!hunt.label[0]) lookupHuntLabel();
    lv_label_set_text(huntNameLbl, hunt.label[0] ? hunt.label : "(no name seen)");
    if (seen) snprintf(buf, sizeof(buf), "seen %.1f s ago  hits %lu", age / 1000.0f, static_cast<unsigned long>(hunt.count));
    else snprintf(buf, sizeof(buf), "not seen yet");
    lv_label_set_text(huntInfo1, buf);
    if (hopMode()) snprintf(buf, sizeof(buf), "listening ch %u%s", currentChannelNum, parkedIdx >= 0 ? " (parked)" : " (hopping)");
    else snprintf(buf, sizeof(buf), "BLE %s scan", bleScan.active ? "active" : "passive");
    lv_label_set_text(huntInfo2, buf);
}

void refreshSystem(float global) {
    char buf[64];
    char r1[16], r2[16];
    int n = 0;
    if (hopMode()) {
        int top[3];
        sortTop3(top);
        if (top[0] >= 0) {
            const ChannelState& ch = channels[top[0]];
            fmtRate(r1, sizeof(r1), ch.metrics.frames * 1000.0f / kDwellMs, " pkt/s");
            fmtRate(r2, sizeof(r2), ch.metrics.bytes * 1000.0f / kDwellMs, " B/s");
            snprintf(buf, sizeof(buf), "busiest ch%u  score %.0f", kChannels[top[0]], ch.busyEma);
            lv_label_set_text(sysLines[n++], buf);
            snprintf(buf, sizeof(buf), "  %s  %s", r1, r2);
            lv_label_set_text(sysLines[n++], buf);
            snprintf(buf, sizeof(buf), "  talkers %u  strong %u/%lu", ch.metrics.unique, ch.metrics.strong,
                     static_cast<unsigned long>(ch.metrics.frames));
            lv_label_set_text(sysLines[n++], buf);
        } else {
            lv_label_set_text(sysLines[n++], "busiest: listening...");
            lv_label_set_text(sysLines[n++], "");
            lv_label_set_text(sysLines[n++], "");
        }
    } else {
        int devs = 0;
        const uint32_t now = millis();
        portENTER_CRITICAL(&g_devMux);
        for (int i = 0; i < kBleDevSlots; i++) if (bleDevs[i].lastMs && now - bleDevs[i].lastMs <= kDevFreshMs) devs++;
        portEXIT_CRITICAL(&g_devMux);
        snprintf(buf, sizeof(buf), "BLE mode: %d devices (60 s)", devs);
        lv_label_set_text(sysLines[n++], buf);
        snprintf(buf, sizeof(buf), "  scan %s -> %s  %lu adv", bleScanModeName(),
                 bleScan.active ? "active" : "passive", static_cast<unsigned long>(bleScan.advSeen));
        lv_label_set_text(sysLines[n++], buf);
        lv_label_set_text(sysLines[n++], "  Wi-Fi sniffing paused");
    }
    const uint32_t up = millis() / 1000;
    snprintf(buf, sizeof(buf), "uptime %02lu:%02lu:%02lu   max %.0f", static_cast<unsigned long>(up / 3600),
             static_cast<unsigned long>((up / 60) % 60), static_cast<unsigned long>(up % 60), global);
    lv_label_set_text(sysLines[n++], buf);
    int skipped = 0;
    for (int i = 0; i < kChannelCount; i++) if (chanEnabled(i) && channels[i].unavailable) skipped++;
    snprintf(buf, sizeof(buf), "sweeps %lu  dwell %u ms", static_cast<unsigned long>(sweepCount), static_cast<unsigned>(dwellMs()));
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "band %s  channels %d  skip %d", kBandName[bandMode], enabledCount(), skipped);
    lv_label_set_text(sysLines[n++], buf);
    if (parkedIdx >= 0) snprintf(buf, sizeof(buf), "park: ch%u", kChannels[parkedIdx]);
    else snprintf(buf, sizeof(buf), "park: off (hopping)");
    lv_label_set_text(sysLines[n++], buf);
    if (captureEnabled) snprintf(buf, sizeof(buf), "usb rec: %lu sent  %lu drop", static_cast<unsigned long>(capSent),
                                 static_cast<unsigned long>(capDropped));
    else snprintf(buf, sizeof(buf), "usb rec: off (host: cap 1)");
    lv_label_set_text(sysLines[n++], buf);
    if (sd.capEnabled) snprintf(buf, sizeof(buf), "sd: rec %lu fr  %lu kB", static_cast<unsigned long>(sd.frames),
                               static_cast<unsigned long>(sd.bytes / 1024));
    else if (sd.mounted) snprintf(buf, sizeof(buf), "sd: card ready (host: sdcap 1)");
    else snprintf(buf, sizeof(buf), "sd: no card");
    lv_label_set_text(sysLines[n++], buf);
    if (hunt.active) { char m[26]; huntIdText(m, sizeof(m)); snprintf(buf, sizeof(buf), "hunt %s", m); }
    else snprintf(buf, sizeof(buf), "hunt off");
    if (deauth.active) {
        char d[26];
        fmtMac(d, sizeof(d), deauth.targeted ? deauth.targetMac : deauth.bssid);   // the MAC actually being kicked
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " · kick %s", d);   // buf is 64: both MACs fit
    }
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "radio %s/%s/%s", errBand == ESP_OK ? "band ok" : "band ERR",
             errCountry == ESP_OK ? "cc ok" : "cc ERR", errPromisc == ESP_OK ? "promisc ok" : "promisc ERR");
    lv_label_set_text(sysLines[n++], buf);
    snprintf(buf, sizeof(buf), "heap %u kB free", static_cast<unsigned>(ESP.getFreeHeap() / 1024));
    lv_label_set_text(sysLines[n++], buf);
    lv_label_set_text(sysLines[n++], "BOOT: tap = next page");
    lv_label_set_text(sysLines[n++], "hold=walk, release picks");   // both lines are ~160 px max (CLIP past the box)
    for (; n < kSysLines; n++) lv_label_set_text(sysLines[n], "");
}

// Spectrum page: per-bin energy bars (dBm) for the 2.4 GHz 15.4 channels, plus the current peak.
void refreshSpectrum() {
    char buf[64];
    const int nb = specBinCount();

    int peakDbm = -128, peakMhz = 0;
    for (int i = 0; i < nb; i++) {
        if (specFine[i].edSamples == 0) continue;
        if (specFine[i].edMax > peakDbm) { peakDbm = specFine[i].edMax; peakMhz = specBinMhz(i); }
    }
    if (peakMhz) {
        snprintf(buf, sizeof(buf), "%d dBm", peakDbm);
        lv_label_set_text(spPeakLabel, buf);
        snprintf(buf, sizeof(buf), "peak %d MHz · %d MHz step", peakMhz, specStepMhz);
        lv_label_set_text(spStatsLabel, buf);
    } else {
        lv_label_set_text(spPeakLabel, "-- dBm");
        lv_label_set_text(spStatsLabel, "scanning...");
    }
    if (monitorReady && currentSpecMhz) snprintf(buf, sizeof(buf), "%d MHz%s", currentSpecMhz, recTag());
    else snprintf(buf, sizeof(buf), "scan%s", recTag());
    lv_label_set_text(spChanLabel, buf);
    applyRecColor(spChanLabel);

    // Map the nb fine bins onto the fixed LCD bar slots (max-per-group so 1-bin peaks survive downsampling).
    const int curBar = (nb > 0 && currentSpecMhz) ? (currentIdx * kLcdSpecBars / nb) : -1;
    for (int j = 0; j < kLcdSpecBars; j++) {
        if (!spBars[j]) continue;
        int lo = j * nb / kLcdSpecBars, hi = (j + 1) * nb / kLcdSpecBars;
        if (hi <= lo) hi = lo + 1;
        if (lo >= nb) lo = nb - 1, hi = nb;
        int mx = -128; bool any = false;
        for (int i = lo; i < hi && i < nb; i++) if (specFine[i].edSamples > 0) { any = true; if (specFine[i].edMax > mx) mx = specFine[i].edMax; }
        int h = 2;
        lv_color_t col = c565(DIM_565);
        if (any) {
            const float sc = edDbmToScore(mx);
            h = 2 + static_cast<int>(sc * 120.0f / 100.0f);
            col = (sc < 3.0f) ? c565(GREY_565) : scoreColor(sc);
        }
        if (j == curBar && monitorReady) col = c565(CYAN_565);
        lv_obj_set_height(spBars[j], h);
        lv_obj_set_style_bg_color(spBars[j], col, 0);
    }
}

void driveLed(float global) {
    if (deauth.active) { setLedColor(LED_RED, static_cast<uint8_t>((millis() / kUiIntervalMs & 1u) ? 12 : 70)); return; }   // blink while kicking
    if (hunt.active) {
        const uint32_t last = hunt.lastMs;
        if (!last || millis() - last > 5000) { setLedColor(LED_BLUE, 15); return; }
        const int r = hunt.rssi;
        if (r >= -50)      setLedColor(LED_RED, 100);
        else if (r >= -62) setLedColor(LED_ORANGE, 80);
        else if (r >= -75) setLedColor(LED_YELLOW, 60);
        else if (r >= -88) setLedColor(LED_GREEN, 40);
        else               setLedColor(LED_BLUE, 30);
        return;
    }
    if (sd.capEnabled || captureEnabled) {
        // ~1.2 s breathing pulse so "recording" reads at a glance; colour says which sink.
        const uint32_t phase = millis() % 1200;
        const uint32_t tri = phase < 600 ? phase : (1200 - phase);          // 0..600
        const uint8_t bright = static_cast<uint8_t>(10 + (tri * 60) / 600); // 10..70
        setLedColor((sd.capEnabled && captureEnabled) ? LED_WHITE
                    : sd.capEnabled ? LED_MAGENTA : LED_CYAN_L, bright);
        return;
    }
    if (!hopMode()) { setLedColor(LED_BLUE, 25); return; }
    if (global > 75.0f)      setLedColor(LED_RED);
    else if (global > 50.0f) setLedColor(LED_ORANGE);
    else if (global > 25.0f) setLedColor(LED_YELLOW);
    else                     setLedColor(LED_GREEN);
}

} // namespace

void refreshUi() {
    const float global = hopMode() ? globalActivityMax() : 0.0f;
    driveLed(global);

    if (hopMode()) {
        uint16_t liveUnique;
        portENTER_CRITICAL(&g_accumMux);
        liveUnique = g_accum.unique;
        portEXIT_CRITICAL(&g_accumMux);
        const uint32_t nowMs = millis();
        if (apWindowStartedMs == 0) apWindowStartedMs = nowMs;
        if (liveUnique > apMaxWindow) apMaxWindow = liveUnique;
        if ((nowMs - apWindowStartedMs) >= kApUpdateMs) {
            lastApSeen = apMaxWindow;
            apWindowStartedMs = nowMs;
            apMaxWindow = 0;
        }
    }

    if (splashActive()) return;                    // the splash owns the screen until it fades out
    lv_obj_add_flag(splashBg, LV_OBJ_FLAG_HIDDEN);
    if (!pageAvailable(currentPage) || !pages[currentPage]) showPage(currentPage + 1);
    switch (currentPage) {
        case PAGE_OVERVIEW: refreshOverview(global); break;
        case PAGE_CHANNELS: refreshChannels(global); break;
        case PAGE_SPECTRUM: refreshSpectrum(); break;
        case PAGE_DEVICES:  refreshDevices(); break;
        case PAGE_HUNT:     refreshHunt(); break;
        default:            refreshSystem(global); break;
    }
}

void uiTimerCb(lv_timer_t* t) {
    (void)t;
    hopIfNeeded();
    serviceDeauth();
    refreshUi();
}

// ---------------------------------------------------------------------------------------------
// BOOT button: tap = next page. Hold = walk the mode splashes one by one, release commits the one on
// screen (holding from the hunt page stops the hunt first, then walks). See showBandSplash + kSplash*Ms.
// ---------------------------------------------------------------------------------------------
void pollButton() {
    static bool wasDown = false;
    static uint32_t downSince = 0;
    static bool holding = false;      // long-press engaged: mode walk (or the hunt-stop that opens one)
    static bool stepped = false;      // at least one band step during this hold (release re-splashes it)
    static uint32_t lastStepMs = 0;
    static uint32_t lastEdgeMs = 0;
    const uint32_t now = millis();
    const bool down = digitalRead(kBootButtonPin) == LOW;

    if (down != wasDown) {                        // edge, debounced to 30 ms
        if (now - lastEdgeMs < 30) return;
        lastEdgeMs = now;
        wasDown = down;
        if (down) {
            downSince = now;
            holding = false;
            stepped = false;
        } else if (!holding) {
            showPage(currentPage + 1);            // tap: next page
            splashDurMs = 0;                      // ... without a lingering mode splash shadowing it
            refreshUi();
        } else if (stepped) {                     // release commits the mode on screen: linger on its splash
            showBandSplash(bandMode, kSplashTailMs);
            saveSettings();                        // persist the committed mode once, not per walk-step (C3)
        }
        return;
    }

    if (!down) return;
    auto step = []() {                            // one walk-step: next band + its splash for one interval
        setBandMode(static_cast<BandMode>((bandMode + 1) % kBandModes));
        showBandSplash(bandMode, kSplashStepMs);
        if (serialRoom(120)) Serial.printf("{\"t\":\"log\",\"msg\":\"button: band %s\"}\n", kBandName[bandMode]);
        sendHello();
        stepped = true;
    };
    if (!holding && now - downSince >= kLongPressMs) {   // hold engaged
        holding = true;
        lastStepMs = now;
        if (currentPage == PAGE_HUNT && hunt.active) {
            stopHunt();                           // keep holding to walk modes from here too
            if (serialRoom(80)) Serial.print("{\"t\":\"ack\",\"cmd\":\"hunt\",\"hunt\":null,\"park\":0}\n");
        } else step();
        refreshUi();
    } else if (holding && now - lastStepMs >= kSplashStepMs) {   // keep holding: next splash
        lastStepMs = now;
        step();
        refreshUi();
    }
}

