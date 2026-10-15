/*
 * vera_core.h - VERA (Video Enhanced Retro Adapter of the Commander X16),
 *               machine independent core.
 *
 * Ported from the VeraX16 PBI card of atari800 (src/pbi_verax16.c, same
 * author): the chip itself (registers, 128 KB VRAM, FX, PSG and PCM audio,
 * SPI/SD card, IRQ); the host machine maps the 32 registers and drives the
 * time (vera.c for the C64: IO1 or IO2).
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

#ifndef VICE_VERA_CORE_H
#define VICE_VERA_CORE_H

#include <stddef.h>

#include "vera_compat.h"

#define VERA_CORE_REG_COUNT 32

/* the chip is present: set by the host, read by the renderer */
extern int vera_core_enabled;

/* FPGA power-on: VRAM cleared, default palette, registers reset; the bus is
   not driven for config_ms milliseconds (the iCE40 loads its bitstream) */
void vera_core_power_on(const char *caller, unsigned int config_ms);

/* register access, offset 0..31 */
UBYTE vera_core_read(int offset, int no_side_effects);
void vera_core_write(int offset, UBYTE byte);

/* time: the host calls vera_core_host_line() line_rate_x100 / 100 times per
   second (any rate: the VERA runs free at 31468.75 lines per second) */
void vera_core_set_host_line_rate(unsigned int line_rate_x100);
void vera_core_host_line(void);

/* settings */
void vera_core_set_debuglevel(int level);
void vera_core_set_psg_volume(int percent);     /* 0..400 */
int vera_core_sdcard_attach(const char *filename);  /* raw image, 0 = ok */
void vera_core_sdcard_detach(void);

/* state for the renderer and for inspection */
typedef struct {
    UBYTE dc[8][4]; /* DC banks: 0=Video/Scale, 1=Start/Stop, 2=FX Ctrl, 3=FX Incr, 4=FX Pos, 5=FX Pos, 6=FX Cache */
    UBYTE l0[7];    /* Layer 0: CONFIG, MAPBASE, TILEBASE, HSCR_L/H, VSCR_L/H */
    UBYTE l1[7];    /* Layer 1: same */
} VERA_RegSnap;

void vera_core_get_regsnap(VERA_RegSnap *s);
const UBYTE *vera_core_get_vram_ptr(void);
UBYTE vera_core_get_vram(ULONG addr);
void vera_core_put_vram(ULONG addr, UBYTE byte);
unsigned int vera_core_get_video_version(void);

/* audio: PSG and PCM mixed into a host buffer of 16 bit samples */
void vera_core_sound_init(unsigned int playback_freq, unsigned int channels);
void vera_core_sound_mix(int16_t *buffer, int frames, unsigned int channels);

/* the whole state of the chip for a snapshot of the host (the SD card
   image is not in it); load returns -1 if buf is not a state of this
   version, and the chip is then in an undefined state */
size_t vera_core_state_size(void);
void vera_core_state_save(UBYTE *buf);
int vera_core_state_load(const UBYTE *buf, size_t len);

void vera_core_shutdown(void);

#endif
