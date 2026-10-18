/*
 * vera_video.c - VERA video output: layers, sprites and palette composed
 *                line by line into a 640x480 frame, handed to the host at
 *                the end of the visible area (vera_host_frame).
 *
 * Ported from atari800 (src/vera_video.c), without its SDL window: in VICE
 * the user interface shows the frame (arch/gtk3/vera_window.c).
 *
 * Written by
 *  Gianluca Renzi <gianlucarenzi@eurek.it>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */

#include "vice.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vera_compat.h"
#include "vera_core.h"
#include "vera_video.h"

#define VERA_W 640
#define VERA_H 480

static uint32_t      vera_fb[VERA_W * VERA_H];
static UBYTE         vera_layer_fb[2][VERA_W * VERA_H];
static UBYTE         vera_sprite_col_fb[VERA_W * VERA_H];
static UBYTE         vera_sprite_z_fb[VERA_W * VERA_H];
/* vera_core_get_video_version() value used by the last full render of each line */
static unsigned int  vera_line_version[VERA_H];
/* VRAM each layer read on each line at its last render (one map row or one
 * bitmap row): [lo, hi], lo > hi = nothing. A VRAM write outside the tile
 * data, palette and sprites only re-renders the lines that read it. */
static ULONG         vera_line_lo[2][VERA_H];
static ULONG         vera_line_hi[2][VERA_H];
/* VRAM in 512-byte buckets: for each bucket the lines whose ranges above
 * touch it, so a VRAM write only checks those lines */
#define VERA_BUCKET_SHIFT 9
#define VERA_BUCKETS      (0x20000u >> VERA_BUCKET_SHIFT)
#define VERA_LINE_WORDS   ((VERA_H + 31) / 32)
static uint32_t      vera_bucket_lines[VERA_BUCKETS][VERA_LINE_WORDS];
/* palette writes only recolour: a line rendered with an older palette
 * version is recoloured from its stored indices, not rendered again */
static unsigned int  vera_palette_version = 1;
static unsigned int  vera_line_palver[VERA_H];
/* vera_fb changed since the last VERA_VIDEO_Frame(): an unchanged picture
 * is not handed to the UI again */
static int           vera_fb_dirty = 1;

/* VRAM writes seen (VERA_VIDEO_VramWrite): a line copied from the one above
 * needs nothing written since that one was rendered */
static unsigned int  vera_vram_wcount = 0;

/* -DVERA_VIDEO_CHECK: every line is rendered again with the reference code
 * (no fast tile path, no line copy, palette decoded per pixel) and the two
 * are compared; without it vera_ref is the constant 0 */
#ifdef VERA_VIDEO_CHECK
static int           vera_ref = 0;
static unsigned long vera_check_lines = 0, vera_check_bad = 0;
#else
#define vera_ref 0
#endif
/* integer window size multiplier, for high-DPI screens (the UI reads it) */
static int           vera_scale = 1;

void VERA_VIDEO_SetScale(int scale)
{
    if (scale < VERA_VIDEO_SCALE_MIN || scale > VERA_VIDEO_SCALE_MAX)
        return;
    vera_scale = scale;
}

int VERA_VIDEO_GetScale(void)
{
    return vera_scale;
}

/* ------------------------------------------------------------------
 * Palette conversion.
 * ------------------------------------------------------------------ */
static inline uint32_t get_pal_color(const UBYTE *vram, int idx)
{
    ULONG addr = 0x1FA00u + (ULONG)(idx & 0xFF) * 2u;
    UBYTE g4b4 = vram[addr];
    UBYTE r4 = vram[addr + 1] & 0x0F;
    UBYTE g4 = (g4b4 >> 4) & 0x0F;
    UBYTE b4 = g4b4 & 0x0F;
    return 0xFF000000u |
           ((uint32_t)(r4 | (r4 << 4)) << 16) |
           ((uint32_t)(g4 | (g4 << 4)) << 8) |
           (uint32_t)(b4 | (b4 << 4));
}

static inline uint32_t get_output_color(const UBYTE *vram, UBYTE dc_video, int idx)
{
    uint32_t color;

    if ((dc_video & 0x03u) == 0)
        return 0xFF0000FFu;

    color = get_pal_color(vram, idx);
    if ((dc_video & 0x07u) == 0x06u) {
        uint32_t r = (color >> 16) & 0xFFu;
        uint32_t g = (color >> 8) & 0xFFu;
        uint32_t b = color & 0xFFu;
        uint32_t y = (r + g + b) / 3u;

        return 0xFF000000u | (y << 16) | (y << 8) | y;
    }

    return color;
}

/* colour of each index for the current palette and output mode: rebuilt
 * when the palette, a register or the mode changed (256 entries instead of
 * one palette decode per output pixel) */
static uint32_t     vera_pal_cache[256];
static unsigned int vera_pal_cache_pver = 0;
static unsigned int vera_pal_cache_vver = 0;
static UBYTE        vera_pal_cache_mode = 0xFF;

static const uint32_t *vera_palette(const UBYTE *vram, UBYTE dc_video)
{
    unsigned int vver = vera_core_get_video_version();
    UBYTE mode = dc_video & 0x07u;

    if (vera_pal_cache_pver != vera_palette_version || vera_pal_cache_vver != vver
        || vera_pal_cache_mode != mode) {
        vera_pal_cache_pver = vera_palette_version;
        vera_pal_cache_vver = vver;
        vera_pal_cache_mode = mode;
        for (int i = 0; i < 256; i++)
            vera_pal_cache[i] = get_output_color(vram, dc_video, i);
    }
    return vera_pal_cache;
}

static inline UBYTE compose_pixel_index(UBYTE sprite_z, UBYTE sprite_col,
                                        UBYTE layer0_col, UBYTE layer1_col)
{
    switch (sprite_z & 0x03u) {
    case 3:
        return sprite_col ? sprite_col : (layer1_col ? layer1_col : layer0_col);
    case 2:
        return layer1_col ? layer1_col : (sprite_col ? sprite_col : layer0_col);
    case 1:
        return layer1_col ? layer1_col : (layer0_col ? layer0_col : sprite_col);
    default:
        return layer1_col ? layer1_col : layer0_col;
    }
}

/* ------------------------------------------------------------------
 * Render a tile layer.
 * ------------------------------------------------------------------ */
static void render_tile_layer(UBYTE *row, const UBYTE *vram, int layer,
                              const VERA_RegSnap *rs, int py, int ax0, int ay0, int start, int end)
{
    const UBYTE *l = (layer == 0) ? rs->l0 : rs->l1;
    UBYTE l_config = l[0];
    UBYTE l_mapbase = l[1];
    UBYTE l_tilebase = l[2];
    int hscroll = ((int)(l[4] & 0x0F) << 8) | (int)l[3];
    int vscroll = ((int)(l[6] & 0x0F) << 8) | (int)l[5];
    int hscale = rs->dc[0][1];
    int vscale = rs->dc[0][2];

    if (hscale == 0 || vscale == 0) return;

    int color_depth = l_config & 0x03;
    int t256c = (l_config & 0x08) != 0;
    int text_mode_256c = (color_depth == 0) && t256c;
    int map_w = 32 << ((l_config >> 4) & 0x03);
    int map_h = 32 << ((l_config >> 6) & 0x03);
    int tile_w = (l_tilebase & 1) ? 16 : 8;
    int tile_h = (l_tilebase & 2) ? 16 : 8;
    ULONG tile_base = (ULONG)(l_tilebase & 0xFCu) * 512u;
    ULONG map_base = (ULONG)l_mapbase * 512u;

    {
        int ly_raw = ((py - ay0) * vscale) >> 7;
        int ly = (ly_raw + vscroll) & 0xFFF;
        int tile_row = (ly / tile_h) % map_h;
        int tile_fy = ly % tile_h;
        ULONG row_lo = map_base + (ULONG)tile_row * (ULONG)map_w * 2u;

        /* the whole map row: horizontal scroll wraps around it */
        if (row_lo < vera_line_lo[layer][py])
            vera_line_lo[layer][py] = row_lo;
        if (row_lo + (ULONG)map_w * 2u - 1u > vera_line_hi[layer][py])
            vera_line_hi[layer][py] = row_lo + (ULONG)map_w * 2u - 1u;

        if (!vera_ref && color_depth != 0) {
            /* 2/4/8 bpp: shifts instead of divisions, x in fixed point, the
             * map entry and the tile row fetched once per tile */
            int bpp_log2 = color_depth;             /* 1, 2, 3: 2, 4, 8 bpp */
            int bpp = 1 << bpp_log2;
            int tw_log2 = (tile_w == 16) ? 4 : 3;
            int row_bytes = (tile_w * bpp) >> 3;
            ULONG tile_bytes = (ULONG)tile_h * (ULONG)row_bytes;
            int ppb_mask = (8 >> bpp_log2) - 1;     /* pixels per byte - 1 */
            int byte_sh = 3 - bpp_log2;             /* pixel -> byte in the row */
            int pix_mask = (1 << bpp) - 1;
            UBYTE t256 = t256c ? 0x80u : 0u;
            ULONG map_row = map_base + (ULONG)tile_row * (ULONG)map_w * 2u;
            int lx_fp = (start - ax0) * hscale + (hscroll << 7);
            int cur_col = -1;
            int hflip = 0;
            UBYTE palhi = 0;
            ULONG gaddr = 0;
            int prev_lx = -1;
            int color_idx = 0;

            for (int px = start; px < end; px++, lx_fp += hscale) {
                int lx = (lx_fp >> 7) & 0xFFF;
                int tile_col, tfx;

                /* DC_HSCALE < 128: the same source pixel on several output
                 * pixels, fetched once */
                if (lx == prev_lx) {
                    if (color_idx != 0)
                        row[px] = (UBYTE)color_idx;
                    continue;
                }
                prev_lx = lx;
                tile_col = (lx >> tw_log2) & (map_w - 1);
                tfx = lx & (tile_w - 1);

                if (tile_col != cur_col) {
                    ULONG map_addr = map_row + (ULONG)tile_col * 2u;
                    UBYTE ch_l = vram[map_addr & 0x1FFFFu];
                    UBYTE attr = vram[(map_addr + 1) & 0x1FFFFu];
                    int vflip = (attr >> 3) & 1;
                    int tfy = vflip ? (tile_h - 1 - tile_fy) : tile_fy;
                    int tile_idx = ch_l | ((int)(attr & 0x03) << 8);

                    cur_col = tile_col;
                    hflip = (attr >> 2) & 1;
                    palhi = (UBYTE)((attr & 0xF0u) | t256);
                    gaddr = tile_base + (ULONG)tile_idx * tile_bytes + (ULONG)tfy * (ULONG)row_bytes;
                }
                if (hflip)
                    tfx = tile_w - 1 - tfx;
                color_idx = (vram[(gaddr + (ULONG)(tfx >> byte_sh)) & 0x1FFFFu]
                             >> ((ppb_mask - (tfx & ppb_mask)) << bpp_log2)) & pix_mask;
                if (color_idx >= 1 && color_idx <= 15)
                    color_idx |= palhi;
                if (color_idx != 0)
                    row[px] = (UBYTE)color_idx;
            }
            return;
        }
        for (int px = start; px < end; px++) {
            int lx_raw = ((px - ax0) * hscale) >> 7;
            int lx = (lx_raw + hscroll) & 0xFFF;
            int tile_col = (lx / tile_w) % map_w;
            int tile_fx = lx % tile_w;

            ULONG map_addr = map_base + (ULONG)(tile_row * map_w + tile_col) * 2u;
            UBYTE ch_l = vram[map_addr & 0x1FFFFu];
            UBYTE attr = vram[(map_addr + 1) & 0x1FFFFu];
            int vflip = 0;
            int hflip = 0;
            int tile_idx = ch_l;
            int color_idx = 0;

            if (color_depth == 0) {
                int tfy = tile_fy;
                int tfx = tile_fx;
                ULONG glyph_addr = tile_base + (ULONG)tile_idx * (tile_h * (tile_w / 8)) + (tfy * (tile_w / 8)) + (tfx / 8);
                int pixel_bit = (vram[glyph_addr & 0x1FFFFu] >> (7 - (tfx % 8))) & 1;
                if (text_mode_256c)
                    color_idx = pixel_bit ? attr : 0;
                else
                    color_idx = pixel_bit ? (attr & 0x0F) : (attr >> 4);
            }
            else {
                int tfy;
                int tfx;
                int bpp = 1 << color_depth;
                int pixels_per_byte = 8 / bpp;
                ULONG glyph_addr;
                UBYTE b;
                int shift;

                vflip = (attr >> 3) & 1;
                hflip = (attr >> 2) & 1;
                tile_idx |= (int)(attr & 0x03) << 8;

                tfy = vflip ? (tile_h - 1 - tile_fy) : tile_fy;
                tfx = hflip ? (tile_w - 1 - tile_fx) : tile_fx;

                glyph_addr = tile_base + (ULONG)tile_idx * (tile_h * tile_w * bpp / 8) + (tfy * tile_w * bpp / 8) + (tfx * bpp / 8);
                b = vram[glyph_addr & 0x1FFFFu];
                shift = (pixels_per_byte - 1 - (tfx % pixels_per_byte)) * bpp;
                color_idx = (b >> shift) & ((1 << bpp) - 1);

                /* layer_renderer.v "Apply palette offset": for 2/4/8 bpp,
                 * pixels 1-15 take attr[7:4] as high nibble, with bit 7
                 * forced by T256C (Lx_CONFIG bit 3); 8 bpp included. */
                if (color_idx >= 1 && color_idx <= 15)
                    color_idx |= (int)((attr & 0xF0u) | (t256c ? 0x80u : 0u));
            }

            if (color_idx != 0)
                row[px] = (UBYTE)color_idx;
        }
    }
}

/* ------------------------------------------------------------------
 * Render a bitmap layer.
 * ------------------------------------------------------------------ */
static void render_bitmap_layer(UBYTE *row, const UBYTE *vram, int layer,
                                const VERA_RegSnap *rs, int py, int ax0, int ay0, int start, int end)
{
    const UBYTE *l = (layer == 0) ? rs->l0 : rs->l1;
    UBYTE l_config = l[0];
    UBYTE l_tilebase = l[2];
    
    int hscale = rs->dc[0][1];
    int vscale = rs->dc[0][2];
    if (hscale == 0 || vscale == 0) return;

    int color_depth = l_config & 0x03;
    int bpp = 1 << color_depth;
    int bitmap_w = (l_tilebase & 1) ? 640 : 320;
    /* layer_renderer.v: bitmap data from TILEBASE[7:2] (MAPBASE unused);
     * palette offset from HSCROLL_H[3:0], bit 7 forced by T256C */
    ULONG bitmap_base = (ULONG)(l_tilebase & 0xFCu) * 512u;
    int palette_offset = ((l[4] & 0x0F) << 4) | ((l_config & 0x08) ? 0x80 : 0);

    {
        int ly = ((py - ay0) * vscale) >> 7;
        ULONG row_bytes = (ULONG)bitmap_w * (ULONG)bpp / 8u;
        ULONG row_lo = bitmap_base + (ULONG)ly * row_bytes;

        if (row_lo < vera_line_lo[layer][py])
            vera_line_lo[layer][py] = row_lo;
        if (row_lo + row_bytes - 1u > vera_line_hi[layer][py])
            vera_line_hi[layer][py] = row_lo + row_bytes - 1u;
        for (int px = start; px < end; px++) {
            int lx = ((px - ax0) * hscale) >> 7;
            ULONG pixel_offset_bits;
            ULONG byte_offset;
            int bit_offset;
            UBYTE b;
            int color_idx;

            if (lx >= bitmap_w) continue;

            pixel_offset_bits = ((ULONG)ly * bitmap_w + lx) * bpp;
            byte_offset = pixel_offset_bits / 8;
            bit_offset = 7 - (pixel_offset_bits % 8) - (bpp - 1);
            if (bit_offset < 0) bit_offset = 0;

            b = vram[(bitmap_base + byte_offset) & 0x1FFFFu];
            color_idx = (b >> bit_offset) & ((1 << bpp) - 1);
            if (color_idx == 0) continue;

            /* no offset at 1 bpp; at 2/4/8 bpp only for pixels 1-15 */
            if (color_depth != 0 && color_idx <= 15)
                color_idx = (color_idx & 0x0F) | palette_offset;
            row[px] = (UBYTE)color_idx;
        }
    }
}

/* ------------------------------------------------------------------
 * Render sprite layer.
 * ------------------------------------------------------------------ */
static void render_sprites(UBYTE *col_row, UBYTE *z_row, const UBYTE *vram,
                           const VERA_RegSnap *rs, int py, int ax0, int ay0,
                           int xstart, int xend)
{
    /* sprite_renderer.v works in the composer's line buffer, before the
     * output scaler: sprite X / Y are in the same scaled coordinates as the
     * layers (320x240 at DC_HSCALE = DC_VSCALE = 64) */
    int hscale = rs->dc[0][1];
    int vscale = rs->dc[0][2];
    int ly;

    if (hscale == 0 || vscale == 0)
        return;
    ly = ((py - ay0) * vscale) >> 7;

    /* Sprite attributes are at $1FC00-$1FFFF (128 sprites × 8 bytes) */
    for (int i = 0; i < 128; i++) {
        ULONG addr = 0x1FC00u + (ULONG)i * 8u;
        UBYTE attr0 = vram[addr];
        UBYTE attr1 = vram[addr + 1];
        UBYTE x_l = vram[addr + 2];
        UBYTE x_h = vram[addr + 3];
        UBYTE y_l = vram[addr + 4];
        UBYTE y_h = vram[addr + 5];
        UBYTE attr6 = vram[addr + 6];
        UBYTE attr7 = vram[addr + 7];
        int width_log2 = ((attr7 >> 4) & 0x03) + 3;
        int height_log2 = (attr7 >> 6) + 3;
        int width = 1 << width_log2;
        int height = 1 << height_log2;
        int hflip = attr6 & 0x01;
        int vflip = (attr6 >> 1) & 0x01;
        int color_mode = (attr1 >> 7) & 0x01;
        int palette_offset = (attr7 & 0x0F) << 4;
        ULONG sprite_addr = ((ULONG)attr0 << 5) | ((ULONG)(attr1 & 0x0F) << 13);

        int z_depth = (attr6 >> 2) & 3;
        if (z_depth == 0) continue; /* Disabled */

        int x = x_l | ((x_h & 3) << 8);
        int y = y_l | ((y_h & 3) << 8);
        if (x >= 0x400 - width) x -= 0x400;
        if (y >= 0x400 - height) y -= 0x400;
        if (x >= VERA_W || y >= VERA_H || x + width <= 0 || y + height <= 0) continue;
        if (ly < y || ly >= y + height) continue;

        {
            int sy = ly - y;
            int eff_sy = vflip ? (height - 1 - sy) : sy;
            ULONG row_addr = sprite_addr + ((ULONG)eff_sy << (width_log2 - (1 - color_mode)));

            for (int px = xstart; px < xend; px++) {
                int sx = (((px - ax0) * hscale) >> 7) - x;
                int eff_sx;
                ULONG pixel_addr;
                UBYTE color_idx;

                if (sx < 0 || sx >= width) continue;

                eff_sx = hflip ? (width - 1 - sx) : sx;
                if (color_mode == 0) {
                    UBYTE packed;

                    pixel_addr = row_addr + ((ULONG)eff_sx >> 1);
                    packed = vram[pixel_addr & 0x1FFFFu];
                    color_idx = (eff_sx & 1) ? (packed & 0x0Fu) : (packed >> 4);
                    if (color_idx != 0)
                        color_idx = (UBYTE)(color_idx + palette_offset);
                }
                else {
                    pixel_addr = row_addr + (ULONG)eff_sx;
                    color_idx = vram[pixel_addr & 0x1FFFFu];
                }

                if (color_idx != 0 && z_depth > z_row[px]) {
                    z_row[px] = (UBYTE)z_depth;
                    col_row[px] = color_idx;
                }
            }
        }
    }
}

/* set or clear line py in the buckets of the VRAM range [lo, hi] */
static void vera_bucket_mark(int py, ULONG lo, ULONG hi, int set)
{
    uint32_t bit = 1u << (py & 31);
    int word = py >> 5;
    ULONG b, last;

    if (lo > hi || lo > 0x1FFFFu)
        return;
    if (hi > 0x1FFFFu)
        hi = 0x1FFFFu;
    last = hi >> VERA_BUCKET_SHIFT;
    for (b = lo >> VERA_BUCKET_SHIFT; b <= last; b++) {
        if (set)
            vera_bucket_lines[b][word] |= bit;
        else
            vera_bucket_lines[b][word] &= ~bit;
    }
}

static void vera_line_buckets(int py, int set)
{
    vera_bucket_mark(py, vera_line_lo[0][py], vera_line_hi[0][py], set);
    vera_bucket_mark(py, vera_line_lo[1][py], vera_line_hi[1][py], set);
}

/* the last full line rendered (or copied) and what it was rendered from */
static int           vera_dup_py = -1;
static int           vera_dup_src;
static unsigned int  vera_dup_vver, vera_dup_wcount, vera_dup_pver;
static VERA_RegSnap  vera_dup_rs;

static void render_scanline_range_inner(int py, int xstart, int xend)
{
    VERA_RegSnap rs;
    const UBYTE *vram;
    int ax0;
    int ax1;
    int ay0;
    int ay1;
    int start;
    int end;
    UBYTE *layer0_row;
    UBYTE *layer1_row;
    UBYTE *sprite_col_row;
    UBYTE *sprite_z_row;
    uint32_t *fb_row;
    uint32_t border;

    if (!vera_core_enabled)
        return;
    if (py < 0 || py >= VERA_H)
        return;

    vera_core_get_regsnap(&rs);
    vram = vera_core_get_vram_ptr();

    ax0 = (int)rs.dc[1][0] * 4;
    ax1 = (int)rs.dc[1][1] * 4;
    ay0 = (int)rs.dc[1][2] * 2;
    ay1 = (int)rs.dc[1][3] * 2;
    if (ax0 < 0) ax0 = 0;
    if (ax1 > VERA_W) ax1 = VERA_W;
    if (ay0 < 0) ay0 = 0;
    if (ay1 > VERA_H) ay1 = VERA_H;

    start = xstart;
    if (start < 0) start = 0;
    end = xend;
    if (end > VERA_W) end = VERA_W;
    if (end <= start)
        return;

    /* a full render records afresh what the layers read; a partial one
     * (mid-line change) adds to it */
    vera_fb_dirty = 1;
    if (start == 0 && end == VERA_W) {
        unsigned int vver = vera_core_get_video_version();
        int inside = (py >= ay0 && py < ay1);
        int vscale = rs.dc[0][2];
        int src = inside ? ((py - ay0) * vscale) >> 7 : -1;

        /* DC_VSCALE < 128 shows a source line on several output lines: when
         * the line above shows the same one and nothing was written since it
         * was rendered, this line is the same, copy it */
        if (!vera_ref && vera_dup_py >= 0 && py == vera_dup_py + 1 && src == vera_dup_src && vver == vera_dup_vver
            && vera_vram_wcount == vera_dup_wcount && vera_palette_version == vera_dup_pver
            && memcmp(&rs, &vera_dup_rs, sizeof rs) == 0) {
            size_t o = (size_t)py * VERA_W;

            memcpy(vera_layer_fb[0] + o, vera_layer_fb[0] + o - VERA_W, VERA_W);
            memcpy(vera_layer_fb[1] + o, vera_layer_fb[1] + o - VERA_W, VERA_W);
            memcpy(vera_sprite_col_fb + o, vera_sprite_col_fb + o - VERA_W, VERA_W);
            memcpy(vera_sprite_z_fb + o, vera_sprite_z_fb + o - VERA_W, VERA_W);
            memcpy(vera_fb + o, vera_fb + o - VERA_W, VERA_W * sizeof(uint32_t));
            vera_line_lo[0][py] = vera_line_lo[0][py - 1];
            vera_line_lo[1][py] = vera_line_lo[1][py - 1];
            vera_line_hi[0][py] = vera_line_hi[0][py - 1];
            vera_line_hi[1][py] = vera_line_hi[1][py - 1];
            vera_dup_py = py;
            return;
        }
        if (!vera_ref) {
            vera_dup_py = py;
            vera_dup_src = src;
            vera_dup_vver = vver;
            vera_dup_wcount = vera_vram_wcount;
            vera_dup_pver = vera_palette_version;
            vera_dup_rs = rs;
        }
        vera_line_lo[0][py] = vera_line_lo[1][py] = 0xFFFFFFFFu;
        vera_line_hi[0][py] = vera_line_hi[1][py] = 0;
    } else if (!vera_ref) {
        vera_dup_py = -1;                   /* partial render: no copy from it */
    }

    layer0_row = vera_layer_fb[0] + (size_t)py * VERA_W;
    layer1_row = vera_layer_fb[1] + (size_t)py * VERA_W;
    sprite_col_row = vera_sprite_col_fb + (size_t)py * VERA_W;
    sprite_z_row = vera_sprite_z_fb + (size_t)py * VERA_W;
    fb_row = vera_fb + (size_t)py * VERA_W;

    memset(layer0_row + start, 0, (size_t)(end - start));
    memset(layer1_row + start, 0, (size_t)(end - start));
    memset(sprite_col_row + start, 0, (size_t)(end - start));
    memset(sprite_z_row + start, 0, (size_t)(end - start));

    border = get_output_color(vram, rs.dc[0][0], rs.dc[0][3]);
    for (int px = start; px < end; px++)
        fb_row[px] = border;

    if (py < ay0 || py >= ay1)
        return;
    if (start < ax0) start = ax0;
    if (end > ax1) end = ax1;
    if (end <= start)
        return;

    /* Lx_CONFIG bit 3 is T256C (attr_mode in layer_renderer.v), not an
     * affine mode: VERA has no affine layer.  Bit 2 selects bitmap mode. */
    if (rs.dc[0][0] & 0x10) {
        if (rs.l0[0] & 0x04)
            render_bitmap_layer(layer0_row, vram, 0, &rs, py, ax0, ay0, start, end);
        else
            render_tile_layer(layer0_row, vram, 0, &rs, py, ax0, ay0, start, end);
    }
    if (rs.dc[0][0] & 0x20) {
        if (rs.l1[0] & 0x04)
            render_bitmap_layer(layer1_row, vram, 1, &rs, py, ax0, ay0, start, end);
        else
            render_tile_layer(layer1_row, vram, 1, &rs, py, ax0, ay0, start, end);
    }
    if (rs.dc[0][0] & 0x40)
        render_sprites(sprite_col_row, sprite_z_row, vram, &rs, py, ax0, ay0, start, end);

    {
        const uint32_t *pal = vera_palette(vram, rs.dc[0][0]);
        UBYTE bg = rs.dc[0][3];

        for (int px = start; px < end; px++) {
            UBYTE color_idx = compose_pixel_index(sprite_z_row[px], sprite_col_row[px],
                                                  layer0_row[px], layer1_row[px]);
            fb_row[px] = vera_ref ? get_output_color(vram, rs.dc[0][0], color_idx ? color_idx : bg)
                                  : pal[color_idx ? color_idx : bg];
        }
    }
}

/* the line's ranges change while it renders: keep the buckets in step */
static void render_scanline_range(int py, int xstart, int xend)
{
    if (py < 0 || py >= VERA_H)
        return;
    vera_line_buckets(py, 0);
    render_scanline_range_inner(py, xstart, xend);
#ifdef VERA_VIDEO_CHECK
    {
        static UBYTE l0[VERA_W], l1[VERA_W], sc[VERA_W], sz[VERA_W];
        static uint32_t fb[VERA_W];
        size_t o = (size_t)py * VERA_W;

        memcpy(l0, vera_layer_fb[0] + o, VERA_W);
        memcpy(l1, vera_layer_fb[1] + o, VERA_W);
        memcpy(sc, vera_sprite_col_fb + o, VERA_W);
        memcpy(sz, vera_sprite_z_fb + o, VERA_W);
        memcpy(fb, vera_fb + o, sizeof fb);
        vera_ref = 1;
        render_scanline_range_inner(py, xstart, xend);
        vera_ref = 0;
        ++vera_check_lines;
        if (memcmp(l0, vera_layer_fb[0] + o, VERA_W) || memcmp(l1, vera_layer_fb[1] + o, VERA_W)
            || memcmp(sc, vera_sprite_col_fb + o, VERA_W) || memcmp(sz, vera_sprite_z_fb + o, VERA_W)
            || memcmp(fb, vera_fb + o, sizeof fb)) {
            if (++vera_check_bad <= 20)
                fprintf(stderr, "VERA_VIDEO_CHECK: line %d (%d-%d) differs\n", py, xstart, xend);
        }
        if ((vera_check_lines & 0xFFFFu) == 0)
            fprintf(stderr, "VERA_VIDEO_CHECK: %lu lines, %lu different\n", vera_check_lines, vera_check_bad);
    }
#endif
    vera_line_buckets(py, 1);
}

/* ------------------------------------------------------------------
 * Sprite attribute writes: only the lines of the sprite, before and after
 * ------------------------------------------------------------------ */
static uint8_t vera_sprite_pending[128 / 8];
static int     vera_sprite_pending_any = 0;

/* lines whose layer line (scaled) crosses sprite i as VRAM holds it now */
static void vera_sprite_invalidate(int i)
{
    VERA_RegSnap rs;
    const UBYTE *vram = vera_core_get_vram_ptr();
    ULONG a = 0x1FC00u + (ULONG)i * 8u;
    int height = 1 << ((vram[a + 7] >> 6) + 3);
    int y = vram[a + 4] | ((vram[a + 5] & 3) << 8);
    int vscale, ay0, py;

    if (((vram[a + 6] >> 2) & 3) == 0)
        return;                                 /* disabled: draws nothing */
    vera_core_get_regsnap(&rs);
    vscale = rs.dc[0][2];
    ay0 = (int)rs.dc[1][2] * 2;
    if (y >= 0x400 - height) y -= 0x400;
    for (py = ay0; py < VERA_H; py++) {
        int ly = ((py - ay0) * vscale) >> 7;
        if (ly >= y + height)
            break;
        if (ly >= y)
            vera_line_version[py] = 0;
    }
}

/* the new rectangles of the sprites written since the last line: VRAM
 * holds the new attributes by now */
static void vera_sprite_flush(void)
{
    int i;

    vera_sprite_pending_any = 0;
    for (i = 0; i < 128; i++) {
        if (vera_sprite_pending[i >> 3] & (1u << (i & 7))) {
            vera_sprite_pending[i >> 3] &= (uint8_t)~(1u << (i & 7));
            vera_sprite_invalidate(i);
        }
    }
}

/* ------------------------------------------------------------------
 * VRAM write: which lines does it change?
 * ------------------------------------------------------------------ */
static int vera_ranges_overlap(ULONG lo, ULONG hi, ULONG a_lo, ULONG a_hi)
{
    return lo <= a_hi && a_lo <= hi;
}

int VERA_VIDEO_VramWrite(ULONG lo, ULONG hi)
{
    VERA_RegSnap rs;
    UBYTE video;
    int layer, py, w;
    ULONG b, last;

    vera_vram_wcount++;
    vera_core_get_regsnap(&rs);
    video = rs.dc[0][0];

    /* palette: every pixel, but the layer and sprite indices stay valid */
    if (vera_ranges_overlap(lo, hi, 0x1FA00u, 0x1FBFFu)) {
        vera_palette_version++;
        return 0;
    }
    /* sprites: their attributes change the lines of the sprite only (the
     * old rectangle now, the new one once written: vera_sprite_flush);
     * their data can be anywhere */
    if (video & 0x40) {
        if (lo < 0x1FC00u)
            return 1;
        if (lo <= 0x1FFFFu) {
            int i, last_i = (int)(((hi > 0x1FFFFu ? 0x1FFFFu : hi) - 0x1FC00u) >> 3);

            for (i = (int)((lo - 0x1FC00u) >> 3); i <= last_i; i++) {
                vera_sprite_invalidate(i);
                vera_sprite_pending[i >> 3] |= (uint8_t)(1u << (i & 7));
            }
            vera_sprite_pending_any = 1;
        }
    }

    /* tile data of an enabled tile layer: any line can show that tile */
    for (layer = 0; layer < 2; layer++) {
        const UBYTE *l = layer ? rs.l1 : rs.l0;
        int depth, tile_w, tile_h;
        ULONG base, size;

        if (!(video & (0x10 << layer)) || (l[0] & 0x04))
            continue;
        depth = l[0] & 0x03;
        tile_w = (l[2] & 1) ? 16 : 8;
        tile_h = (l[2] & 2) ? 16 : 8;
        base = (ULONG)(l[2] & 0xFCu) * 512u;
        size = (ULONG)(depth ? 1024 : 256) * (ULONG)(tile_w * tile_h * (1 << depth) / 8);
        if (vera_ranges_overlap(lo, hi, base, base + size - 1u))
            return 1;
    }

    /* map or bitmap rows: only the lines that read them (a line that was not
     * rendered since a register change is out of date anyway) */
    if (lo > 0x1FFFFu)
        return 0;
    last = (hi > 0x1FFFFu ? 0x1FFFFu : hi) >> VERA_BUCKET_SHIFT;
    for (b = lo >> VERA_BUCKET_SHIFT; b <= last; b++) {
        for (w = 0; w < VERA_LINE_WORDS; w++) {
            uint32_t m = vera_bucket_lines[b][w];

            while (m != 0u) {
                py = w * 32 + __builtin_ctz(m);
                m &= m - 1u;
                if (vera_ranges_overlap(lo, hi, vera_line_lo[0][py], vera_line_hi[0][py])
                    || vera_ranges_overlap(lo, hi, vera_line_lo[1][py], vera_line_hi[1][py]))
                    vera_line_version[py] = 0;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Public: compose and display one VERA frame.
 * ------------------------------------------------------------------ */
void VERA_VIDEO_Reset(void)
{
    memset(vera_layer_fb, 0, sizeof(vera_layer_fb));
    memset(vera_sprite_col_fb, 0, sizeof(vera_sprite_col_fb));
    memset(vera_sprite_z_fb, 0, sizeof(vera_sprite_z_fb));
    memset(vera_fb, 0, sizeof(vera_fb));
    memset(vera_line_version, 0, sizeof(vera_line_version));
    memset(vera_line_palver, 0, sizeof(vera_line_palver));
    vera_fb_dirty = 1;
    memset(vera_line_lo, 0xFF, sizeof(vera_line_lo));
    memset(vera_line_hi, 0, sizeof(vera_line_hi));
    memset(vera_bucket_lines, 0, sizeof(vera_bucket_lines));
    memset(vera_sprite_pending, 0, sizeof(vera_sprite_pending));
    vera_sprite_pending_any = 0;
}

void VERA_VIDEO_Scanline(UWORD scanline)
{
    unsigned int version;

    if (vera_sprite_pending_any)
        vera_sprite_flush();
    version = vera_core_get_video_version();

    if (scanline < VERA_H) {
        /* Nothing that affects the picture was written since this line was
         * last rendered: the buffers are still valid. */
        if (vera_line_version[scanline] == version) {
            if (vera_line_palver[scanline] != vera_palette_version) {
                vera_line_palver[scanline] = vera_palette_version;
                VERA_VIDEO_Recolor(scanline, 0);
            }
            return;
        }
        vera_line_version[scanline] = version;
        vera_line_palver[scanline] = vera_palette_version;
    }
    render_scanline_range((int)scanline, 0, VERA_W);
}

/* Palette/border change mid-line: only the colour lookup is redone from the
 * indices already rendered at line start, as the real chip reads the palette
 * at pixel output time while layer data was fetched a line earlier. */
void VERA_VIDEO_Recolor(UWORD scanline, UWORD xstart)
{
    VERA_RegSnap rs;
    const UBYTE *vram;
    int py = (int)scanline;
    int ax0, ax1, ay0, ay1, start, end;
    size_t row;

    if (!vera_core_enabled || py < 0 || py >= VERA_H)
        return;

    vera_core_get_regsnap(&rs);
    vram = vera_core_get_vram_ptr();

    ax0 = (int)rs.dc[1][0] * 4;
    ax1 = (int)rs.dc[1][1] * 4;
    ay0 = (int)rs.dc[1][2] * 2;
    ay1 = (int)rs.dc[1][3] * 2;
    if (ax0 < 0) ax0 = 0;
    if (ax1 > VERA_W) ax1 = VERA_W;
    if (ay0 < 0) ay0 = 0;
    if (ay1 > VERA_H) ay1 = VERA_H;

    start = (int)xstart;
    if (start < 0) start = 0;
    end = VERA_W;
    if (end <= start)
        return;

    vera_fb_dirty = 1;
    row = (size_t)py * VERA_W;
    {
        const uint32_t *pal = vera_palette(vram, rs.dc[0][0]);

        for (int px = start; px < end; px++) {
            int inside = (py >= ay0 && py < ay1 && px >= ax0 && px < ax1);
            UBYTE color_idx = 0;

            if (inside)
                color_idx = compose_pixel_index(vera_sprite_z_fb[row + px],
                                                vera_sprite_col_fb[row + px],
                                                vera_layer_fb[0][row + px],
                                                vera_layer_fb[1][row + px]);
            vera_fb[row + px] = pal[color_idx ? color_idx : rs.dc[0][3]];
        }
    }
}

void VERA_VIDEO_Midline(UWORD scanline, UWORD xstart)
{
    render_scanline_range((int)scanline, (int)xstart, VERA_W);
}

void VERA_VIDEO_Frame(void)
{
    if (!vera_core_enabled || !vera_fb_dirty)
        return;
    vera_fb_dirty = 0;
    vera_host_frame(vera_fb, VERA_W, VERA_H);
}

int VERA_VIDEO_Init(void)
{
    return 1;
}

void VERA_VIDEO_Exit(void)
{
}
