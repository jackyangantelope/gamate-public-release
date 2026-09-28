#pragma once

#include <cstdint>
#include <cstring>

// Exact nearest-neighbour 160x150 -> 256x240, centred in a 320-pixel row.
// Five input pixels become eight output pixels; duplicate rows reuse the result.
inline uint16_t gamate_rgb555(uint16_t c)
{
    return static_cast<uint16_t>(((c >> 1u) & 0x7fe0u) | (c & 0x1fu));
}

__attribute__((noinline)) inline void gamate_scale_frame(const uint16_t *src, uint16_t *dst)
{
    unsigned y = 0;
    for (unsigned sy = 0; sy < 150; ++sy) {
        uint16_t *row = dst + y * 320u + 32u;
        for (unsigned group = 0; group < 32; ++group) {
            const uint16_t *s = src + sy * 160u + group * 5u;
            uint16_t *d = row + group * 8u;
            d[0] = d[1] = gamate_rgb555(s[0]);
            d[2] = d[3] = gamate_rgb555(s[1]);
            d[4] = gamate_rgb555(s[2]);
            d[5] = d[6] = gamate_rgb555(s[3]);
            d[7] = gamate_rgb555(s[4]);
        }
        ++y;
        const unsigned next_y = ((sy + 1u) * 8u + 4u) / 5u;
        if (y < next_y) {
            std::memcpy(dst + y * 320u + 32u, row, 256u * sizeof(uint16_t));
            ++y;
        }
    }
}
