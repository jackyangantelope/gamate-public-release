// http://blog.kevtris.org/blogfiles/Gamate%20Inside.txt
#pragma GCC optimize("Ofast")
// license:BSD-3-Clause
#include <stdint.h>
#include <string.h>
#include "vdp.h"

// Gamate VDP internal state encapsulated in a struct
struct GamateVDP {
    // Registers / control
    int vram_address; // 13-bit VRAM address
    int active_bitplane; // 0 = plane0, 1 = plane1
    int horizontal_scroll; // 0–255
    int vertical_scroll; // 0–255
    int top_window_enabled; // top 16-row window enable
    int plane_swap_enabled; // swap plane bits for pixel output
    int vram_increment_mode32; // VRAM increment mode: 0=+1, 1=+32
    int display_blank; // LCD blank

    // 8KB VRAM (2 interleaved bitplanes, 4KB each)
    uint8_t /*__aligned(4)*/ VRAM[16384];
} vdp;


//--- VDP port/register names ---
typedef enum {
    LCD_CONTROL = 1,
    SCROLL_X = 2,
    SCROLL_Y = 3,
    XPOS = 4,
    YPOS = 5,
    VRAM_DATA = 7
} vdp_port_t;

//--- CPU I/O ---
__forceinline static void increment_vram_address() {
    vdp.vram_address = (vdp.vram_address + (vdp.vram_increment_mode32 ? 0x20 : 1)) & 0x1FFF;
}

uint8_t vdp_read() {
    const uint8_t value = vdp.VRAM[(vdp.vram_address << 1) | vdp.active_bitplane];
    increment_vram_address();
    return value;
}

void vdp_write(const uint16_t port, const uint8_t value) {
    switch ((vdp_port_t) (port & 7)) {
        case LCD_CONTROL:
            vdp.display_blank = value & 0x80;
            vdp.vram_increment_mode32 = value & 0x40;
            vdp.top_window_enabled = value & 0x20;
            vdp.plane_swap_enabled = value & 0x10;
            break;

        case SCROLL_X:
            vdp.horizontal_scroll = value;
            break;

        case SCROLL_Y:
            vdp.vertical_scroll = value;
            break;

        case XPOS:
            vdp.active_bitplane = (value >> 7) & 1;
            // BUGFIX: Corrected VRAM address mask from 0x3FE0 to 0x1FE0 for 13-bit address
            vdp.vram_address = (vdp.vram_address & 0x1FE0) | (value & 0x1F);
            break;

        case YPOS:
            vdp.vram_address = (vdp.vram_address & 0x001F) | ((int) value << 5);
            break;

        case VRAM_DATA:
            vdp.VRAM[(vdp.vram_address << 1) | vdp.active_bitplane] = value;
            increment_vram_address();
            break;
    }
}

//--- Rendering helpers ---
__forceinline static void compute_real_coords(uint8_t *out_x, uint8_t *out_y, const uint8_t scanline) {
    const int scroll_x = vdp.horizontal_scroll;
    const int scroll_y = vdp.vertical_scroll;

    if (scroll_y < 200) {
        // BUGFIX: Use wider type and modulo to prevent overflow and correctly wrap scroll
        const uint16_t real_y_temp = scanline + scroll_y;
        *out_y = real_y_temp % 200;
        *out_x = scroll_x;

        if (vdp.top_window_enabled && scanline < 0x10) {
            *out_x = 0;
            *out_y = 0xD0 + scanline;
        }
    } else {
        *out_x = scroll_x;
        if (scroll_y & 8) {
            *out_y = 0;
        } else {
            const int fixed_rows = scroll_y & 7;
            if (scanline <= fixed_rows) {
                *out_x = 0;
                *out_y = 0xF8 + scanline + (7 - fixed_rows);
            } else {
                *out_y = scanline;
            }
        }
    }
}


void screen_update(uint16_t *screen_buffer) {
    static const uint8_t color_lut[2][2][2] = {
        {{0, 1}, {2, 3}}, // swap = 0
        {{0, 2}, {1, 3}}, // swap = 1
    };

    if (vdp.display_blank) {
        // Clear screen fast
        memset(screen_buffer, 0, GAMATE_SCREEN_WIDTH * GAMATE_SCREEN_HEIGHT * sizeof(uint16_t));
        return;
    }

    for (int y = 0; y < GAMATE_SCREEN_HEIGHT; y++) {
        uint8_t real_x, real_y;
        compute_real_coords(&real_x, &real_y, y);

        const uint32_t base_vram_addr = real_y << 6; // (y*32)*2

        for (int x = 0; x < GAMATE_SCREEN_WIDTH; x += 16) {
            const uint16_t pixel_x = x + real_x;
            const uint32_t byte_index = pixel_x >> 3;

            // Load 2 bitplanes (8 pixels)
            const uint32_t planes = *(const uint32_t *) &vdp.VRAM[base_vram_addr + (byte_index << 1)];
            uint8_t plane_0 = planes & 0xFF;
            uint8_t plane_1 = planes >> 8 & 0xFF;


            // Decode 8 pixels
#pragma GCC unroll 8
            for (int bit = 7; bit >= 0; bit--) {
                const uint8_t bit_0 = plane_0 >> bit & 1;
                const uint8_t bit_1 = plane_1 >> bit & 1;
                *screen_buffer++ = palette_gamate[color_lut[vdp.plane_swap_enabled][bit_0][bit_1]];
            }

            plane_0 = planes >> 16 & 0xFF;
            plane_1 = planes >> 24;

            // Decode next 8 pixels
#pragma GCC unroll 8
            for (int bit = 7; bit >= 0; bit--) {
                const uint8_t bit_0 = plane_0 >> bit & 1;
                const uint8_t bit_1 = plane_1 >> bit & 1;
                *screen_buffer++ = palette_gamate[color_lut[vdp.plane_swap_enabled][bit_0][bit_1]];
            }
        }
    }
}
