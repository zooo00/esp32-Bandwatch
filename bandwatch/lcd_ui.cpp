// LCD pages for Bandwatch (172 x 320 portrait, LVGL v9). Only the visible page exists as widgets — see
// showPage(): the C5 has no PSRAM and the Wi-Fi/BLE stacks need most of the 320 KB, so ~300 permanent
// widgets are not affordable. Everything here runs on the loop task (uiTimerCb at kUiIntervalMs cadence,
// pollButton from Bandwatch_Loop); no other task touches LVGL.
#include "bandwatch.h"        // pulls in lvgl
#include "bandwatch_core.h"
#include "Display_ST7789.h"   // LCD_WIDTH/LCD_HEIGHT for the boot logo
#include "surv_ouis.h"   // kSurvName for the Devices page
#include "boot_logos.h"   // kBootLogoData*: boot pictures (RGB565, live in flash)
#include <esp_random.h>    // esp_fill_random: entropy-backed pick, no seeding to worry about

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
uint32_t pageCostB[PAGE_COUNT] = {0};   // heap each page took to build, largest seen (lcdPageHeadroomB)
// overview
lv_obj_t* chanLabel = nullptr;
lv_obj_t* globalBar = nullptr;
lv_obj_t* globalLabel = nullptr;
lv_obj_t* sweepLabel = nullptr;
lv_obj_t* topRows[3] = {nullptr};
lv_obj_t* topRates[3] = {nullptr};
lv_obj_t* topBars[3] = {nullptr};
// Bar strips (Overview per-channel, Spectrum per-bin): one custom-drawn object each (barStripDraw), not a
// widget per bar. The refresh lays the bars out (x, width, height, colour) and barStripSet() invalidates only
// the columns that changed; a layout change repaints the whole strip.
struct StripBar { uint8_t x; uint8_t w; uint8_t h; uint16_t col565; };
struct BarStrip { lv_obj_t* obj; StripBar* bars; int count; };
StripBar ovBarsBuf[kChannelCount];
BarStrip ovStrip{nullptr, ovBarsBuf, 0};
lv_obj_t* specLabel = nullptr;
lv_obj_t* statsLine1 = nullptr;
lv_obj_t* statsLine2 = nullptr;
lv_obj_t* footLabel = nullptr;
// channels
lv_obj_t* listChanLabel = nullptr;
// Channels grid: one custom-drawn object, not 39 rows x (row + 2 labels + bar). As widgets the page cost
// ~156 LVGL objects - by far the heaviest page (docs/DEVELOPER.md §16). refreshChannels() fills this snapshot
// and invalidates only the cells that changed; chanGridDraw() paints them from it.
constexpr int kChanCols = 3, kChanRowsPerCol = 13, kChanColW = 53, kChanColGap = 2, kChanRowH = 20;
constexpr int kListSlots = kChanCols * kChanRowsPerCol;   // 39
enum : uint8_t { CELL_DATA = 0, CELL_NODATA = 1, CELL_UNAVAIL = 2, CELL_CURRENT = 0x80 };
struct ChanCell { uint8_t ch; uint8_t val; uint8_t state; };   // state: CELL_* | CELL_CURRENT
lv_obj_t* chanGrid = nullptr;
ChanCell chanCells[kListSlots];
int chanCellCount = 0;
lv_obj_t* listFoot = nullptr;
// devices
lv_obj_t* devHdrRight = nullptr;
// Device list: one custom-drawn object (devListDraw) painting devRows[] directly; devRowHash[] fingerprints what
// each row showed last time so refreshDevices() invalidates only rows whose text, colour or RSSI changed.
lv_obj_t* devList = nullptr;
uint32_t devRowHash[kDevRows] = {0};
int devListShown = 0;
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
StripBar spBarsBuf[kLcdSpecBars];
BarStrip spStrip{nullptr, spBarsBuf, 0};
lv_obj_t* spFoot = nullptr;
// mode splash: a full-screen name card flashed over whatever page is up when the band changes; while BOOT is held
// it steps through the modes' splashes and release commits the one on screen (pollButton + showBandSplash)
lv_obj_t* splashBg = nullptr;
lv_obj_t* splashTitle = nullptr;
lv_obj_t* splashSub = nullptr;
uint32_t splashStartMs = 0, splashDurMs = 0;

// Boot logos: full-screen pictures (bootlogo/) shown at boot and when the page selector wraps around.
// The pixels are const in flash; LVGL draws them from there, so RAM cost is only the existing draw buffer.
lv_obj_t* logoSplash = nullptr;
uint32_t logoSplashUntilMs = 0;
int logoIdx = -1;               // picture on screen right now (for stepping through them)
bool logoThenModeCard = false;   // boot: hand off to the mode card when the picture fades

#define BW_LOGO(i)   {.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, \
                                .flags = 0, .w = LCD_WIDTH, .h = LCD_HEIGHT, .stride = LCD_WIDTH * 2}, \
                      .data_size = sizeof(kBootLogoData##i), .data = kBootLogoData##i}
const lv_image_dsc_t kBootLogos[K_BOOT_LOGO_COUNT] = { K_BOOT_LOGO_LIST };   // list stays in sync (boot_logos.h)
#undef BW_LOGO

// Random pick; skip an immediate repeat while there is a choice.
int logoPick(int prevIdx) {
    uint32_t r;
    esp_fill_random(&r, sizeof(r));
    int i = static_cast<int>(r % K_BOOT_LOGO_COUNT);
    if (K_BOOT_LOGO_COUNT > 1 && i == prevIdx) i = (i + 1) % K_BOOT_LOGO_COUNT;
    return i;
}

// Show picture idx full-screen for durMs. Boot passes thenModeCard so the mode card still names the boot
// band afterwards; a page-wrap tap just flashes one. Taps while it is up step to the next (see pollButton).
void showLogoPic(int idx, uint32_t durMs, bool thenModeCard) {
    if (!logoSplash) {
        logoSplash = lv_image_create(lv_scr_act());
        lv_obj_add_flag(logoSplash, LV_OBJ_FLAG_HIDDEN);
    }
    logoIdx = idx;
    lv_image_set_src(logoSplash, &kBootLogos[idx]);
    const int n = lv_obj_get_child_count(lv_scr_act());   // above any page or mode card just (re)built below it
    if (n > 0) lv_obj_move_to_index(logoSplash, n - 1);
    logoThenModeCard = thenModeCard;
    lv_obj_remove_flag(logoSplash, LV_OBJ_FLAG_HIDDEN);
    logoSplashUntilMs = millis() + durMs;
}

void showLogoSplash(uint32_t durMs, bool thenModeCard) {
    showLogoPic(logoPick(logoIdx), durMs, thenModeCard);   // random, no immediate repeat
}

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

uint16_t score565(float s) {
    if (s > 70.0f) return RED_565;
    if (s > 40.0f) return YELLOW_565;
    return GREEN_565;
}
lv_color_t scoreColor(float s) { return c565(score565(s)); }

uint16_t rssi565(int rssi);
lv_color_t rssiColor(int rssi) { return c565(rssi565(rssi)); }

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

inline int rssiPct(int rssi) {   // -100 dBm -> 0, -30 dBm -> 100
    int p = (rssi + 100) * 100 / 70;
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

struct DevRowInfo { uint8_t mac[6]; int8_t rssi; bool ap; uint8_t surv; bool destOnly; char label[33]; };
DevRowInfo devRows[kDevRows];
int devRowCount = 0;

// lv_area_intersect() is private API in LVGL 9.3 (lv_area_private.h); this is the same few lines.
bool areaIntersect(lv_area_t& out, const lv_area_t& a, const lv_area_t& b) {
    out.x1 = LV_MAX(a.x1, b.x1); out.y1 = LV_MAX(a.y1, b.y1);
    out.x2 = LV_MIN(a.x2, b.x2); out.y2 = LV_MIN(a.y2, b.y2);
    return out.x1 <= out.x2 && out.y1 <= out.y2;
}

void chanCellArea(int slot, lv_area_t& a) {
    lv_obj_get_coords(chanGrid, &a);
    const int32_t x = a.x1 + (slot / kChanRowsPerCol) * (kChanColW + kChanColGap);
    const int32_t y = a.y1 + (slot % kChanRowsPerCol) * kChanRowH;
    a.x1 = x; a.y1 = y; a.x2 = x + kChanColW - 1; a.y2 = y + kChanRowH - 1;
}

// One text run clipped to its box, the way LV_LABEL_LONG_CLIP kept "165"/"100" inside their columns:
// the draw task records the layer's clip area when it is created, so narrow it just for this call.
void drawClippedText(lv_layer_t* layer, lv_draw_label_dsc_t& l, const lv_area_t& box) {
    const lv_area_t saved = layer->_clip_area;
    lv_area_t clip;
    if (areaIntersect(clip, saved, box)) {
        layer->_clip_area = clip;
        lv_draw_label(layer, &l, &box);
    }
    layer->_clip_area = saved;
}

// Bars bottom-aligned in the strip's box, positions precomputed by the page's refresh.
void barStripDraw(lv_event_t* e) {
    const BarStrip* st = static_cast<const BarStrip*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(st->obj, &a);
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = 1;
    for (int i = 0; i < st->count; i++) {
        const StripBar& b = st->bars[i];
        const lv_area_t bar{a.x1 + b.x, a.y2 - b.h + 1, a.x1 + b.x + b.w - 1, a.y2};
        lv_area_t tmp;
        if (!areaIntersect(tmp, bar, layer->_clip_area)) continue;
        r.bg_color = c565(b.col565);
        lv_draw_rect(layer, &r, &bar);
    }
}

lv_obj_t* makeBarStrip(lv_obj_t* parent, BarStrip& st, int height) {
    st.obj = lv_obj_create(parent);
    lv_obj_set_size(st.obj, LV_PCT(100), height);
    lv_obj_align(st.obj, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(st.obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st.obj, 0, 0);
    lv_obj_set_style_pad_all(st.obj, 0, 0);
    lv_obj_remove_flag(st.obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(st.obj, barStripDraw, LV_EVENT_DRAW_MAIN_END, &st);
    st.count = 0;   // a fresh page is wholly dirty
    return st.obj;
}

// Store bar k; when the layout is unchanged, repaint just the columns it occupied before and after.
void barStripSet(BarStrip& st, int k, const StripBar& b, bool relayout) {
    StripBar& o = st.bars[k];
    if (!relayout && (o.x != b.x || o.w != b.w || o.h != b.h || o.col565 != b.col565)) {
        lv_area_t sa;
        lv_obj_get_coords(st.obj, &sa);
        const int x1 = LV_MIN(o.x, b.x), x2 = LV_MAX(o.x + o.w, b.x + b.w) - 1;
        const lv_area_t col{sa.x1 + x1, sa.y1, sa.x1 + x2, sa.y2};
        lv_obj_invalidate_area(st.obj, &col);
    }
    o = b;
}

// What row i of the Devices list shows: its text (prefix + surveillance name / label / MAC tail) and name colour.
// Shared by refreshDevices() (to fingerprint the row) and devListDraw() (to paint it).
uint16_t devRowText(int i, char* buf, size_t n) {
    const DevRowInfo& d = devRows[i];
    // "!" marks known surveillance hardware, "~" a device only ever seen as a destination (tier 1).
    const char* pfx = d.surv ? "! " : d.destOnly ? "~ " : d.ap ? "* " : "";
    if (d.surv)          snprintf(buf, n, "%s%s", pfx, kSurvName[d.surv]);
    else if (d.label[0]) snprintf(buf, n, "%s%s", pfx, d.label);
    else                 snprintf(buf, n, "%s%02x:%02x:%02x", pfx, d.mac[3], d.mac[4], d.mac[5]);
    const bool hunted = hunt.active && (hunt.kind == 1 ? memcmp(d.mac, hunt.key, 6) == 0 : macEq(d.mac, hunt.mac));
    return d.surv ? ORANGE_565 : hunted ? CYAN_565 : d.destOnly ? GREY_565 : WHITE_565;
}

uint16_t rssi565(int rssi) {
    if (rssi >= -50) return RED_565;
    if (rssi >= -65) return ORANGE_565;
    if (rssi >= -80) return YELLOW_565;
    return CYAN_565;
}

// Same geometry as the old widget rows (flex row, 3 px gap): name 84 px clipped, RSSI 26 px right-aligned, bar 44x8.
constexpr int kDevRowH = 20;
void devRowArea(int i, lv_area_t& a) {
    lv_obj_get_coords(devList, &a);
    a.y1 += i * kDevRowH;
    a.y2 = a.y1 + kDevRowH - 1;
}

void devListDraw(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_font_t* font = &lv_font_montserrat_12;
    const int32_t ty = (kDevRowH - lv_font_get_line_height(font)) / 2;
    for (int i = 0; i < devListShown; i++) {
        lv_area_t row, tmp;
        devRowArea(i, row);
        if (!areaIntersect(tmp, row, layer->_clip_area)) continue;
        char text[48], num[8];
        lv_draw_label_dsc_t l;
        lv_draw_label_dsc_init(&l);
        l.font = font;
        l.text_local = 1;
        l.text = text;
        l.color = c565(devRowText(i, text, sizeof(text)));
        l.flag = LV_TEXT_FLAG_EXPAND;   // one line, clipped - never wrap at a space like an 84 px box would
        drawClippedText(layer, l, lv_area_t{row.x1, row.y1 + ty, row.x1 + 83, row.y2 - ty});
        l.flag = LV_TEXT_FLAG_NONE;

        // A tier-1 ("~") device has only been seen as a destination: there is no RSSI to show.
        const bool heard = !devRows[i].destOnly;
        const int rssi = devRows[i].rssi;
        if (heard) snprintf(num, sizeof(num), "%d", rssi);
        else snprintf(num, sizeof(num), "--");
        l.text = num;
        l.align = LV_TEXT_ALIGN_RIGHT;
        l.color = c565(GREY_565);
        drawClippedText(layer, l, lv_area_t{row.x1 + 87, row.y1 + ty, row.x1 + 112, row.y2 - ty});

        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = 2;
        r.bg_color = c565(BLACK_565);
        r.bg_opa = LV_OPA_30;
        lv_area_t bar{row.x1 + 116, row.y1 + 6, row.x1 + 159, row.y1 + 13};
        lv_draw_rect(layer, &r, &bar);
        const int fill = heard ? (44 * rssiPct(rssi) + 50) / 100 : 0;
        if (fill > 0) {
            r.radius = LV_RADIUS_CIRCLE;   // lv_bar's indicator is pill-shaped in the default theme
            r.bg_color = c565(rssi565(rssi));
            r.bg_opa = LV_OPA_COVER;
            bar.x2 = bar.x1 + fill - 1;
            lv_draw_rect(layer, &r, &bar);
        }
    }
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
    makeBarStrip(spec, ovStrip, 36);
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

// Same geometry as the old widget rows: name 20 px right-aligned, 2 px, bar 11x8, 2 px, value 18 px.
void chanGridDraw(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_font_t* font = &lv_font_montserrat_12;
    const int32_t ty = (kChanRowH - lv_font_get_line_height(font)) / 2;
    for (int i = 0; i < chanCellCount; i++) {
        lv_area_t cell, tmp;
        chanCellArea(i, cell);
        if (!areaIntersect(tmp, cell, layer->_clip_area)) continue;   // only repaint what is dirty
        const ChanCell& c = chanCells[i];
        const uint8_t st = c.state & ~CELL_CURRENT;
        char num[4], val[4];

        lv_draw_label_dsc_t l;
        lv_draw_label_dsc_init(&l);
        l.font = font;
        l.text_local = 1;   // draw tasks run later: copy the stack buffer
        snprintf(num, sizeof(num), "%u", c.ch);
        l.text = num;
        l.align = LV_TEXT_ALIGN_RIGHT;
        l.color = (c.state & CELL_CURRENT) ? c565(CYAN_565) : st == CELL_UNAVAIL ? c565(GREY_565) : c565(WHITE_565);
        drawClippedText(layer, l, lv_area_t{cell.x1, cell.y1 + ty, cell.x1 + 19, cell.y2 - ty});

        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.radius = 2;
        r.bg_color = c565(BLACK_565);
        r.bg_opa = LV_OPA_30;
        lv_area_t bar{cell.x1 + 22, cell.y1 + 6, cell.x1 + 32, cell.y1 + 13};
        lv_draw_rect(layer, &r, &bar);
        const int fill = (st == CELL_DATA) ? (11 * c.val + 50) / 100 : 0;
        if (fill > 0) {
            r.radius = LV_RADIUS_CIRCLE;   // lv_bar's indicator is pill-shaped in the default theme
            r.bg_color = scoreColor(c.val);
            r.bg_opa = LV_OPA_COVER;
            bar.x2 = bar.x1 + fill - 1;
            lv_draw_rect(layer, &r, &bar);
        }

        snprintf(val, sizeof(val), "%u", c.val);
        l.text = st == CELL_UNAVAIL ? "x" : st == CELL_NODATA ? "-" : val;
        l.align = LV_TEXT_ALIGN_LEFT;
        l.color = c565(GREY_565);
        drawClippedText(layer, l, lv_area_t{cell.x1 + 35, cell.y1 + ty, cell.x2, cell.y2 - ty});
    }
}

void buildChannelsPage(lv_obj_t* page) {
    make_header(page, "Channels", &listChanLabel);
    chanGrid = make_panel(page, 262, BG_565, 0);
    lv_obj_add_event_cb(chanGrid, chanGridDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
    chanCellCount = 0;   // a fresh page is wholly dirty; the first refresh fills every cell
    listFoot = make_label(page, "", c565(GREY_565), &lv_font_montserrat_12);
    lv_obj_set_width(listFoot, LV_PCT(100));
    lv_obj_set_style_text_align(listFoot, LV_TEXT_ALIGN_CENTER, 0);
}

void buildDevicesPage(lv_obj_t* page) {
    make_header(page, "Devices", &devHdrRight);
    devList = make_panel(page, 246, BG_565, 0);
    lv_obj_add_event_cb(devList, devListDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
    devListShown = 0;   // a fresh page is wholly dirty
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
    makeBarStrip(spec, spStrip, 124);
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
    const uint32_t before = ESP.getFreeHeap();
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
    // Keep the largest build cost seen per page (radio tasks allocating mid-build only push it up, which errs
    // safe). lcdPageHeadroomB() turns it into the reserve the capture ring must leave for a page switch.
    const uint32_t after = ESP.getFreeHeap();
    if (before > after && before - after > pageCostB[n]) {
        pageCostB[n] = before - after;
        if (serialRoom(80))
            Serial.printf("{\"t\":\"log\",\"msg\":\"page %d build cost %lu B\"}\n", n, static_cast<unsigned long>(pageCostB[n]));
    }
}

// Heap a switch from the visible page to the heaviest page available in this mode would take. The capture ring
// leaves this on top of kMinFreeHeapB (capture.cpp), or starting a capture on a light page (Channels, System)
// and stepping to Overview lands under the floor - measured 16.4 kB in 1.15.3 before this existed.
uint32_t lcdPageHeadroomB() {
    uint32_t worst = 0;
    for (int i = 0; i < PAGE_COUNT; i++) if (pageAvailable(i) && pageCostB[i] > worst) worst = pageCostB[i];
    const uint32_t cur = pages[currentPage] ? pageCostB[currentPage] : 0;
    return worst > cur ? worst - cur : 0;
}

// --- Live LCD mirror pacing ---------------------------------------------------------------------
// g_mirror (host_proto.cpp) gates emission in the LVGL flush callback. Within one lv_timer_handler every
// dirty region flushes synchronously, so a bulk repaint (page switch, splash, the first frame) would push
// far more than the 8 KB serial TX buffer at once and most regions would drop. serviceMirror() instead
// re-sends the screen one ~10-row strip per loop, each well under the TX buffer, so the buffer drains
// between strips and a full frame streams over ~32 loops (<1 s) without dropping. A dropped steady-state
// region (mirrorOnFlush) schedules a fresh scan here. Only runs while a full refresh is pending.
// mirrorScanY = next strip's top row during a full re-send, or -1 between passes. A strip dropped mid-pass
// (TX buffer full when it collides with a data-refresh flush) sets mirrorDirty, so another full pass runs
// after this one. Without that, a strip dropped during the single initial scan would leave a permanent
// black/stale band - and we can't just restart the scan on every drop, or a busy screen would never finish
// a pass. Passes repeat until one completes with no drop, then stop (steady-state only sends dirty regions).
// Step the LCD page like a BOOT tap, driven from the host (dashboard interface-stepping buttons). Runs on the
// loop task via handleCommand, same task as LVGL, so touching the UI here is safe. showPage() skips pages that
// don't apply to the current mode; clearing splashDurMs stops a lingering mode card from shadowing the new page.
void stepPage(int dir) {
    showPage(currentPage + (dir < 0 ? -1 : 1));
    splashDurMs = 0;
}

static int mirrorScanY = -1;
static bool mirrorDirty = false;
void mirrorRequestFull() { mirrorScanY = 0; mirrorDirty = false; }   // enable / explicit full frame
void mirrorNoteDrop() { mirrorDirty = true; }                        // a region didn't fit; re-scan after this pass
void serviceMirror() {
    if (!g_mirror) return;
    if (mirrorScanY < 0) {                 // between passes: start another only if a region was dropped
        if (!mirrorDirty) return;
        mirrorScanY = 0;
        mirrorDirty = false;
    }
    constexpr int kStripH = 8;   // 172*8*2 = 2752 B raw (~3.7 KB base64): headroom for a concurrent data flush
    if (!serialRoom(LCD_WIDTH * kStripH * 2 * 4 / 3 + 64)) return;   // wait for the TX buffer to drain
    int y2 = mirrorScanY + kStripH - 1;
    if (y2 > LCD_HEIGHT - 1) y2 = LCD_HEIGHT - 1;
    lv_area_t a;
    a.x1 = 0; a.y1 = mirrorScanY; a.x2 = LCD_WIDTH - 1; a.y2 = y2;
    lv_obj_invalidate_area(lv_scr_act(), &a);   // forces this strip to re-render + flush -> mirrorOnFlush emits it
    mirrorScanY = (y2 >= LCD_HEIGHT - 1) ? -1 : y2 + 1;
}

namespace {
inline bool splashActive() { return splashDurMs > 0 && (millis() - splashStartMs) < splashDurMs; }
inline bool logoActive()   { return logoSplash != nullptr && !lv_obj_has_flag(logoSplash, LV_OBJ_FLAG_HIDDEN); }
} // namespace

void showBandSplash(BandMode m, uint32_t durMs) {
    splashStartMs = millis();
    splashDurMs = durMs;
    // A mode card supersedes any picture: hide the logo overlay so logoActive() reads false. Otherwise a
    // photo shown earlier in the walk (the slot after Spectrum, or the page-wrap flash) stays "active"
    // underneath this card, and a tap during the committed-mode splash is misread as photo-stepping.
    if (logoSplash) lv_obj_add_flag(logoSplash, LV_OBJ_FLAG_HIDDEN);
    logoThenModeCard = false;
    lv_label_set_text(splashTitle, kSplash[m].title);
    lv_label_set_text(splashSub, kSplash[m].sub);
    lv_obj_remove_flag(splashBg, LV_OBJ_FLAG_HIDDEN);
    const int n = lv_obj_get_child_count(lv_scr_act());      // keep it above any page just (re)built below it
    if (n > 0) lv_obj_move_to_index(splashBg, n - 1);
}

// SD card face: a sad face when the card goes away, a happy one when it comes back. Drawn on LVGL's top layer so
// it sits above any page, mode card or photo and survives a page rebuild underneath; one custom-drawn object,
// created on demand and deleted by serviceSdFace() after kSdFaceMs.
namespace {
constexpr uint32_t kSdFaceMs = 3000;
lv_obj_t* sdFace = nullptr;
bool sdFaceHappy = false;
char sdFaceWhy[40] = "";
uint32_t sdFaceUntilMs = 0;

void sdFaceDraw(lv_event_t* e) {
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(sdFace, &a);
    const int32_t cx = (a.x1 + a.x2) / 2, cy = a.y1 + 120;
    const uint16_t face565 = sdFaceHappy ? YELLOW_565 : 0x5D9F;   // sunny yellow / a glum pale blue

    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.radius = LV_RADIUS_CIRCLE;
    r.bg_color = c565(face565);
    const lv_area_t head{cx - 62, cy - 62, cx + 62, cy + 62};
    lv_draw_rect(layer, &r, &head);
    r.bg_color = c565(BLACK_565);
    for (int s = -1; s <= 1; s += 2) {
        const lv_area_t eye{cx + s * 22 - 8, cy - 26 - 8, cx + s * 22 + 8, cy - 26 + 8};
        lv_draw_rect(layer, &r, &eye);
    }

    lv_draw_arc_dsc_t m;
    lv_draw_arc_dsc_init(&m);
    m.color = c565(BLACK_565);
    m.width = 8;
    m.rounded = 1;
    m.radius = 34;
    if (sdFaceHappy) { m.center = {cx, cy + 2};  m.start_angle = 25;  m.end_angle = 155; }   // smile: bottom arc
    else             { m.center = {cx, cy + 58}; m.start_angle = 215; m.end_angle = 325; }   // frown: top arc, lower
    lv_draw_arc(layer, &m);

    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.font = &lv_font_montserrat_20;
    l.color = c565(WHITE_565);
    l.align = LV_TEXT_ALIGN_CENTER;
    l.text = sdFaceHappy ? "SD card in" : "SD card out";
    const lv_area_t t1{a.x1, cy + 80, a.x2, cy + 104};
    lv_draw_label(layer, &l, &t1);
    if (sdFaceWhy[0]) {
        l.font = &lv_font_montserrat_12;
        l.color = c565(GREY_565);
        l.text = sdFaceWhy;
        l.text_local = 1;
        const lv_area_t t2{a.x1 + 4, cy + 110, a.x2 - 4, cy + 140};
        lv_draw_label(layer, &l, &t2);
    }
}
} // namespace

void showSdFace(bool happy, const char* why) {
    sdFaceHappy = happy;
    snprintf(sdFaceWhy, sizeof(sdFaceWhy), "%s", why ? why : "");
    sdFaceUntilMs = millis() + kSdFaceMs;
    if (!sdFace) {
        sdFace = lv_obj_create(lv_layer_top());
        lv_obj_set_size(sdFace, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(sdFace, 0, 0);
        lv_obj_set_style_bg_color(sdFace, c565(BG_565), 0);
        lv_obj_set_style_bg_opa(sdFace, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sdFace, 0, 0);
        lv_obj_set_style_radius(sdFace, 0, 0);
        lv_obj_remove_flag(sdFace, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(sdFace, sdFaceDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
    }
    lv_obj_invalidate(sdFace);   // a second change while one is up: repaint with the new mood
}

void serviceSdFace() {
    if (sdFace && static_cast<int32_t>(millis() - sdFaceUntilMs) >= 0) { lv_obj_delete(sdFace); sdFace = nullptr; }
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

    // Boot logo on top of everything: a random picture for kBootLogoMs, then refreshUi() hides it and
    // shows the boot mode card (showBandSplash) while Wi-Fi is still coming up.
    showLogoSplash(kBootLogoMs, true);
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

    // Lay the strip out the way the old flex row did (SPACE_BETWEEN, 3 px extra before each band group), then
    // repaint only what changed: everything on a layout change, otherwise just the columns whose bar moved.
    const int nEnabled = enabledCount();
    int barW = (150 / (nEnabled > 0 ? nEnabled : 1)) - 1;
    if (barW < 2) barW = 2;
    if (barW > 10) barW = 10;
    lv_obj_update_layout(ovStrip.obj);   // a just-built page has no width yet
    const int stripW = lv_obj_get_width(ovStrip.obj);
    int used = 0, n = 0;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i)) continue;
        used += barW;
        for (int g = 1; g < 5; g++) if (i == kGroupStart[g]) used += 3;
        n++;
    }
    int gap = n > 1 ? (stripW - used) / (n - 1) : 0;
    if (gap < 1) gap = 1;
    const bool relayout = (n != ovStrip.count) || (ovStrip.count && ovStrip.bars[0].w != barW);
    int x = 0, k = 0;
    for (int i = 0; i < kChannelCount; i++) {
        if (!chanEnabled(i)) continue;
        for (int g = 1; g < 5; g++) if (i == kGroupStart[g]) x += 3;
        const ChannelState& ch = channels[i];
        StripBar b{static_cast<uint8_t>(x), static_cast<uint8_t>(barW), 2, DIM_565};
        if (ch.unavailable) {
            b.col565 = BLACK_565;
        } else if (ch.hasData) {
            b.h = static_cast<uint8_t>(2 + static_cast<int>(ch.busyEma * 34.0f / 100.0f));
            b.col565 = (ch.busyEma < 3.0f) ? GREY_565 : score565(ch.busyEma);
        }
        if (i == currentIdx && monitorReady) b.col565 = CYAN_565;
        barStripSet(ovStrip, k, b, relayout);
        x += barW + gap;
        k++;
    }
    ovStrip.count = n;
    if (relayout) lv_obj_invalidate(ovStrip.obj);
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

    // "APs" is a Wi-Fi concept (beacon senders); in 802.15.4 there are no APs - count live nodes instead, so
    // the footer stops reading a misleading "APs 0" in Zigbee/Thread mode.
    const bool is154 = mode154();
    const char* entL = is154 ? "nodes" : "APs";
    const unsigned entN = is154 ? static_cast<unsigned>(collect154Refs(g_devRefs, kDev154Slots, kDevLcdFreshMs)) : lastApSeen;
    if (captureEnabled) {
        snprintf(buf, sizeof(buf), "%s %u  " LV_SYMBOL_DOWNLOAD " %lu  drop %lu", entL, entN,
                 static_cast<unsigned long>(capSent), static_cast<unsigned long>(capDropped));
    } else {
        const int q = quietestChannel();   // C8: name the quietest channel when the line has room (capture off)
        if (q >= 0)
            snprintf(buf, sizeof(buf), "%s %u   quiet ch%u %.0f", entL, entN, kChannels[q], channels[q].busyEma);
        else
            snprintf(buf, sizeof(buf), "%s %u", entL, entN);
    }
    lv_label_set_text(footLabel, buf);
}

void refreshChannels(float global) {
    char buf[48];
    chanHeaderText(buf, sizeof(buf));
    lv_label_set_text(listChanLabel, buf);
    applyRecColor(listChanLabel);
    int slot = 0;
    lv_area_t a;
    for (int i = 0; i < kChannelCount && slot < kListSlots; i++) {
        if (!chanEnabled(i)) continue;
        const ChannelState& ch = channels[i];
        ChanCell c{kChannels[i], 0, CELL_DATA};
        if (ch.unavailable) c.state = CELL_UNAVAIL;
        else if (!ch.hasData) c.state = CELL_NODATA;
        else c.val = static_cast<uint8_t>(ch.busyEma + 0.5f);
        if (i == currentIdx && monitorReady) c.state |= CELL_CURRENT;
        const ChanCell& old = chanCells[slot];
        if (slot >= chanCellCount || old.ch != c.ch || old.val != c.val || old.state != c.state) {
            chanCells[slot] = c;
            chanCellArea(slot, a);
            lv_obj_invalidate_area(chanGrid, &a);
        }
        slot++;
    }
    for (int i = slot; i < chanCellCount; i++) { chanCellArea(i, a); lv_obj_invalidate_area(chanGrid, &a); }   // rows gone
    chanCellCount = slot;
    const bool is154 = mode154();   // "APs" is Wi-Fi-only; show live node count in 802.15.4
    snprintf(buf, sizeof(buf), "max %.0f  sweep %lu  %s %u", global, static_cast<unsigned long>(sweepCount),
             is154 ? "nodes" : "APs", is154 ? static_cast<unsigned>(collect154Refs(g_devRefs, kDev154Slots, kDevLcdFreshMs)) : lastApSeen);
    lv_label_set_text(listFoot, buf);
}

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
    // Fingerprint each row as drawn (text, colour, RSSI) and repaint only rows that changed or went away.
    lv_area_t a;
    for (int i = 0; i < kDevRows; i++) {
        uint32_t h = 0;
        if (i < n) {
            char text[48];
            const uint16_t col = devRowText(i, text, sizeof(text));
            h = 2166136261u;   // FNV-1a
            for (const char* c = text; *c; c++) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
            h = (h ^ col) * 16777619u;
            h = (h ^ static_cast<uint8_t>(devRows[i].rssi)) * 16777619u;
            h |= 1;   // never 0, so an empty row always differs from a live one
        }
        if (h != devRowHash[i]) { devRowArea(i, a); lv_obj_invalidate_area(devList, &a); devRowHash[i] = h; }
    }
    if (devListShown == 0) lv_obj_invalidate(devList);   // first paint after a page build
    devListShown = n;
    if (mode154()) snprintf(buf, sizeof(buf), "* = beacons (router)  seen < 20 s");
    else if (wifiMode()) snprintf(buf, sizeof(buf), "* = AP (beacons)  seen < 20 s");
    else {
        // Discovery runs continuously (BLE_HS_FOREVER), so the old "scan cycle" counter never moved; show the advert
        // rate instead, measured over at least a second so it does not flicker at the UI tick.
        static uint32_t advMark = 0, advMarkMs = 0, advRate = 0;
        const uint32_t nowMs = millis(), adv = bleScan.advSeen;
        if (nowMs - advMarkMs >= 1000) {
            advRate = advMarkMs ? (adv - advMark) * 1000 / (nowMs - advMarkMs) : 0;
            advMark = adv; advMarkMs = nowMs;
        }
        snprintf(buf, sizeof(buf), "%lu adv/s  seen < 20 s", static_cast<unsigned long>(advRate));
    }
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
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " - kick %s", d);   // buf is 64: both MACs fit
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
        snprintf(buf, sizeof(buf), "at %d MHz, step %d", peakMhz, specStepMhz);   // ASCII: Montserrat 12 has no U+00B7
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
    // Each bar owns an exact 1/42 slice of the strip, less a 1 px gap. The old flex row used fixed 3 px bars on a
    // 2 px gap (208 px) in a ~156 px panel, so the top three bars (~2477-2483 MHz) were clipped off the edge.
    lv_obj_update_layout(spStrip.obj);
    const int stripW = lv_obj_get_width(spStrip.obj);
    const bool relayout = spStrip.count != kLcdSpecBars;
    for (int j = 0; j < kLcdSpecBars; j++) {
        int lo = j * nb / kLcdSpecBars, hi = (j + 1) * nb / kLcdSpecBars;
        if (hi <= lo) hi = lo + 1;
        if (lo >= nb) lo = nb - 1, hi = nb;
        int mx = -128; bool any = false;
        for (int i = lo; i < hi && i < nb; i++) if (specFine[i].edSamples > 0) { any = true; if (specFine[i].edMax > mx) mx = specFine[i].edMax; }
        const int x0 = j * stripW / kLcdSpecBars, x1 = (j + 1) * stripW / kLcdSpecBars;
        StripBar b{static_cast<uint8_t>(x0), static_cast<uint8_t>(LV_MAX(1, x1 - x0 - 1)), 2, DIM_565};
        if (any) {
            const float sc = edDbmToScore(mx);
            b.h = static_cast<uint8_t>(2 + static_cast<int>(sc * 120.0f / 100.0f));
            b.col565 = (sc < 3.0f) ? GREY_565 : score565(sc);
        }
        if (j == curBar && monitorReady) b.col565 = CYAN_565;
        barStripSet(spStrip, j, b, relayout);
    }
    spStrip.count = kLcdSpecBars;
    if (relayout) lv_obj_invalidate(spStrip.obj);
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

    if (logoActive()) {                          // picture: owns the screen until its timer runs out
        if (millis() < logoSplashUntilMs) return;
        lv_obj_add_flag(logoSplash, LV_OBJ_FLAG_HIDDEN);
        if (logoThenModeCard) {                     // boot only: name the mode while Wi-Fi is still coming up
            logoThenModeCard = false;
            showBandSplash(bandMode, kSplashShowMs);
        }
        return;
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
// BOOT button: tap = next page. Hold = walk the mode splashes one by one - with a photo stop after
// Spectrum (the last mode) before wrapping; release commits the one on screen, and stopping on the
// photo lets taps step through the pictures. Holding from the hunt page stops the hunt first, then walks.
// See showBandSplash + kSplash*Ms, walkPhoto below.
// ---------------------------------------------------------------------------------------------
void pollButton() {
    static bool wasDown = false;
    static uint32_t downSince = 0;
    static bool holding = false;      // long-press engaged: mode walk (or the hunt-stop that opens one)
    static bool stepped = false;      // at least one band step during this hold (release re-splashes it)
    static bool walkPhoto = false;    // current walk position is the photo slot (after Spectrum, before the wrap)
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
            walkPhoto = false;
        } else if (!holding) {
            const int prev = currentPage;
            if (logoActive()) {                   // stopped on a picture instead of a scan page: step through them
                showLogoPic((logoIdx + 1) % K_BOOT_LOGO_COUNT, kWrapLogoMs, false);   // loops at the end
            } else {
                showPage(prev + 1);               // tap: next page
                splashDurMs = 0;                  // ... without a lingering mode splash shadowing it
            }
            refreshUi();
        } else if (stepped) {                     // release commits the mode on screen: linger on its splash
            if (walkPhoto)                        // ... stopped on the photo slot: keep it up so taps step through pictures
                showLogoPic(logoIdx, kWrapLogoMs, false);
            else
                showBandSplash(bandMode, kSplashTailMs);
            saveSettings();                        // persist the committed mode once, not per walk-step (C3)
        }
        return;
    }

    if (!down) return;
    auto step = [&]() {                           // one walk-step: next position for one interval (modes, then the photo slot)
        const bool toPhoto = !walkPhoto && bandMode == kBandModes - 1;   // after Spectrum comes the picture, not another mode
        if (!toPhoto) {
            setBandMode(walkPhoto ? BandMode(0) : static_cast<BandMode>(bandMode + 1));
            showBandSplash(bandMode, kSplashStepMs);   // (release still names the committed mode with the tail card)
        } else {
            showLogoSplash(kSplashStepMs, false);      // photo slot: one random picture for the interval
        }
        walkPhoto = toPhoto;
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

