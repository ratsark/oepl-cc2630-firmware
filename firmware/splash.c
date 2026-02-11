// -----------------------------------------------------------------------------
//  Splash Screen Renderer for CC2630 OEPL Tag
//  Streams 4bpp rows directly to UC8159 — no framebuffer needed.
//
//  Layout (600x448, 4bpp BWR):
//    Black border frame (2px all around)
//    Red banner with dithered L-to-R fade (rows 4-79): "OpenEPaperLink" WHITE 3x
//    Black sub-banner (rows 80-119): "CC2630 6.0\" BWR" WHITE 3x
//    Red accent line (rows 120-121)
//    Info section (white bg): MAC, Bat/Temp, AP status in BLACK/RED, 3x
//    Red divider line (rows 370-371)
//    Footer: "FW v0.1" in BLACK, 2x scale
// -----------------------------------------------------------------------------

#include "splash.h"
#include "font8x8.h"
#include "oepl_hw_abstraction_cc2630.h"
#include "drivers/oepl_display_driver_uc8159_600x448.h"
#include "rtt.h"
#include <string.h>

#define DISP_W  600
#define DISP_H  448
#define ROW_BYTES (DISP_W / 2)  // 300 bytes per 4bpp row

// 4bpp color nibbles
#define COL_BLACK  0x0
#define COL_WHITE  0x3
#define COL_RED    0x4

// Fill bytes (two pixels packed)
#define FILL_BLACK 0x00
#define FILL_WHITE 0x33
#define FILL_RED   0x44

// --- Layout constants ---
#define BORDER      2
#define INNER_W     (DISP_W - 2 * BORDER)  // 596

// Red banner with dithered fade: rows 4-79 (76px tall)
#define BANNER_Y0   4
#define BANNER_Y1   80

// Black sub-banner: rows 80-119 (40px tall)
#define SUBBAR_Y0   80
#define SUBBAR_Y1   120

// Red accent line: rows 120-121
#define ACCENT_Y0   120
#define ACCENT_Y1   122

// Red divider above footer: rows 370-371, cols 100-499
#define DIVIDER_Y0  370
#define DIVIDER_Y1  372
#define DIVIDER_X0  100
#define DIVIDER_X1  500

// 4x4 Bayer ordered dither matrix (threshold values 0-15)
static const uint8_t bayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

// --- Text items ---
enum {
    TXT_TITLE = 0,  // "OpenEPaperLink" in red banner
    TXT_MODEL,      // "CC2630 6.0\" BWR" in black sub-banner
    TXT_MAC,        // MAC address (no prefix, fits at 3x)
    TXT_BAT,        // Battery/temperature
    TXT_AP,         // AP status
    TXT_FW,         // Firmware version
    NUM_TEXTS
};

typedef struct {
    uint16_t y_start;
    uint8_t  scale;
    uint8_t  fg;
    const char *text;
    uint16_t x_start;    // pre-computed centering offset
    uint16_t text_len;   // pre-computed string length
} text_item_t;

// --- String formatting helpers ---

static void format_mac(char *buf, const uint8_t *mac_lsb)
{
    static const char hex[] = "0123456789ABCDEF";
    uint8_t p = 0;
    for (int i = 7; i >= 0; i--) {
        buf[p++] = hex[(mac_lsb[i] >> 4) & 0xF];
        buf[p++] = hex[mac_lsb[i] & 0xF];
        if (i > 0) buf[p++] = ':';
    }
    buf[p] = '\0';
}

static char *fmt_u16(char *buf, uint16_t val)
{
    char tmp[6];
    uint8_t n = 0;
    if (val == 0) { *buf++ = '0'; return buf; }
    while (val > 0) { tmp[n++] = '0' + (val % 10); val /= 10; }
    while (n > 0) *buf++ = tmp[--n];
    return buf;
}

static char *fmt_i8(char *buf, int8_t val)
{
    if (val < 0) { *buf++ = '-'; val = -val; }
    return fmt_u16(buf, (uint16_t)val);
}

// --- Text rendering ---

static void precompute_text(text_item_t *t)
{
    t->text_len = 0;
    const char *s = t->text;
    while (*s++) t->text_len++;
    uint16_t text_w = t->text_len * 8 * t->scale;
    t->x_start = (DISP_W > text_w) ? (DISP_W - text_w) / 2 : 0;
}

// Overlay text glyph pixels onto row_buf at row y with given color.
// Only sets pixels where glyph bits are 1 — leaves background untouched.
static void overlay_text(uint16_t y, uint8_t *row_buf,
                         const text_item_t *t, uint8_t color)
{
    uint16_t line_h = 8 * t->scale;
    if (y < t->y_start || y >= t->y_start + line_h) return;

    uint16_t row_in_line = y - t->y_start;
    uint8_t glyph_row = row_in_line / t->scale;
    uint8_t scale = t->scale;

    for (uint16_t ci = 0; ci < t->text_len; ci++) {
        uint8_t ch = (uint8_t)t->text[ci];
        if (ch < 0x20 || ch > 0x7E) ch = 0x20;
        uint8_t row_bits = font8x8[ch - 0x20][glyph_row];
        if (row_bits == 0) continue;

        for (uint8_t bit = 0; bit < 8; bit++) {
            if (!(row_bits & (0x80 >> bit))) continue;
            uint16_t x_base = t->x_start + (uint16_t)ci * 8 * scale
                            + (uint16_t)bit * scale;
            for (uint8_t sx = 0; sx < scale; sx++) {
                uint16_t x = x_base + sx;
                if (x >= DISP_W) break;
                uint16_t bi = x / 2;
                if (x & 1)
                    row_buf[bi] = (row_buf[bi] & 0xF0) | color;
                else
                    row_buf[bi] = (row_buf[bi] & 0x0F) | (color << 4);
            }
        }
    }
}

// Apply 2px black border on left (cols 0-1) and right (cols 598-599) edges
static inline void apply_side_borders(uint8_t *row_buf)
{
    row_buf[0] = FILL_BLACK;
    row_buf[299] = FILL_BLACK;
}

// Render a banner row with dithered red-to-white gradient.
// Solid red from left edge to FADE_X, then Bayer-dithered fade to white.
// FADE_X set past the title text ("OpenEPaperLink" 4x ends at x=524).
#define FADE_X      530
#define FADE_W      (DISP_W - BORDER - FADE_X)  // 68px fade zone

static void render_dithered_banner_row(uint16_t y, uint8_t *row_buf)
{
    row_buf[0] = FILL_BLACK;  // left border

    for (uint16_t x = BORDER; x < DISP_W - BORDER; x += 2) {
        uint8_t c0, c1;

        if (x < FADE_X) {
            c0 = COL_RED;
        } else {
            uint16_t g = (uint16_t)((x - FADE_X) * 17) / FADE_W;
            c0 = (g > bayer4[y & 3][x & 3]) ? COL_WHITE : COL_RED;
        }

        uint16_t x1 = x + 1;
        if (x1 < FADE_X) {
            c1 = COL_RED;
        } else {
            uint16_t g = (uint16_t)((x1 - FADE_X) * 17) / FADE_W;
            c1 = (g > bayer4[y & 3][x1 & 3]) ? COL_WHITE : COL_RED;
        }

        row_buf[x / 2] = (c0 << 4) | c1;
    }

    row_buf[299] = FILL_BLACK;  // right border
}

// --- Main splash renderer ---

void splash_display(const uint8_t *mac, uint16_t battery_mv, int8_t temp_c,
                    bool ap_found, uint8_t channel)
{
    rtt_puts("Splash...\r\n");

    // Pre-format text strings
    // MAC without label prefix — fits at 3x (23 chars × 24px = 552px)
    char mac_str[24];
    format_mac(mac_str, mac);

    char bat_temp_str[32];
    {
        char *p = bat_temp_str;
        memcpy(p, "Bat: ", 5); p += 5;
        p = fmt_u16(p, battery_mv / 1000);
        *p++ = '.';
        uint16_t frac = battery_mv % 1000;
        *p++ = '0' + (frac / 100);
        *p++ = '0' + ((frac / 10) % 10);
        *p++ = 'V';
        memcpy(p, "  Temp: ", 8); p += 8;
        p = fmt_i8(p, temp_c);
        *p++ = 'C';
        *p = '\0';
    }

    char ap_str[32];
    {
        char *p = ap_str;
        if (ap_found) {
            memcpy(p, "AP: Found (ch ", 14); p += 14;
            p = fmt_u16(p, channel);
            *p++ = ')';
        } else {
            memcpy(p, "AP: Not found", 13); p += 13;
        }
        *p = '\0';
    }

    // Text layout — banner/sub-banner text centered in bars,
    // info text at 3x for legibility, evenly spaced in white area
    text_item_t texts[NUM_TEXTS] = {
        [TXT_TITLE] = { 26,  4, COL_WHITE, "OpenEPaperLink",    0, 0},
        [TXT_MODEL] = { 88,  3, COL_WHITE, "CC2630 6.0\" BWR",  0, 0},
        [TXT_MAC]   = {166,  3, COL_BLACK, mac_str,              0, 0},
        [TXT_BAT]   = {234,  3, COL_BLACK, bat_temp_str,         0, 0},
        [TXT_AP]    = {302,  3, ap_found ? COL_BLACK : COL_RED, ap_str, 0, 0},
        [TXT_FW]    = {404,  2, COL_BLACK, "FW v0.2 (OTA)",      0, 0},
    };
    for (uint8_t i = 0; i < NUM_TEXTS; i++)
        precompute_text(&texts[i]);

    // Wake display (full re-init)
    uc8159_wake();

    // Open DTM1 (cmd 0x10) for pixel data
    oepl_hw_gpio_set(15, false);  // DC = command
    oepl_hw_spi_cs_assert();
    { uint8_t c = 0x10; oepl_hw_spi_send_raw(&c, 1); }
    oepl_hw_gpio_set(15, true);   // DC = data

    // Stream 448 rows
    uint8_t row_buf[ROW_BYTES];

    for (uint16_t y = 0; y < DISP_H; y++) {

        if (y < BORDER || y >= DISP_H - BORDER) {
            // Top/bottom border: solid black
            memset(row_buf, FILL_BLACK, ROW_BYTES);

        } else if (y >= BANNER_Y0 && y < BANNER_Y1) {
            // Red banner with dithered fade + white title text
            render_dithered_banner_row(y, row_buf);
            overlay_text(y, row_buf, &texts[TXT_TITLE], COL_WHITE);

        } else if (y >= SUBBAR_Y0 && y < SUBBAR_Y1) {
            // Black sub-banner with white model text
            memset(row_buf, FILL_BLACK, ROW_BYTES);
            overlay_text(y, row_buf, &texts[TXT_MODEL], COL_WHITE);
            apply_side_borders(row_buf);

        } else if (y >= ACCENT_Y0 && y < ACCENT_Y1) {
            // Red accent line
            memset(row_buf, FILL_RED, ROW_BYTES);
            apply_side_borders(row_buf);

        } else if (y >= DIVIDER_Y0 && y < DIVIDER_Y1) {
            // Red divider (partial width, centered)
            memset(row_buf, FILL_WHITE, ROW_BYTES);
            memset(&row_buf[DIVIDER_X0 / 2], FILL_RED,
                   (DIVIDER_X1 - DIVIDER_X0) / 2);
            apply_side_borders(row_buf);

        } else {
            // White background with info/footer text
            memset(row_buf, FILL_WHITE, ROW_BYTES);
            for (uint8_t i = TXT_MAC; i < NUM_TEXTS; i++)
                overlay_text(y, row_buf, &texts[i], texts[i].fg);
            apply_side_borders(row_buf);
        }

        oepl_hw_spi_send_raw(row_buf, ROW_BYTES);
    }

    oepl_hw_spi_cs_deassert();

    // DATA_STOP (0x11)
    oepl_hw_gpio_set(15, false);
    oepl_hw_spi_cs_assert();
    { uint8_t c = 0x11; oepl_hw_spi_send_raw(&c, 1); }
    oepl_hw_spi_cs_deassert();

    // DISPLAY_REFRESH (0x12)
    oepl_hw_gpio_set(15, false);
    oepl_hw_spi_cs_assert();
    { uint8_t c = 0x12; oepl_hw_spi_send_raw(&c, 1); }
    oepl_hw_spi_cs_deassert();

    rtt_puts("Splash REF...");

    // Wait for refresh (~26s)
    for (uint32_t i = 0; i < 30000; i++) {
        if (oepl_hw_gpio_get(13)) break;  // BUSY HIGH = ready
        oepl_hw_delay_ms(1);
    }
    rtt_puts("done\r\n");
}
