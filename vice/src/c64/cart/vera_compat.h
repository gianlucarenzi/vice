/*
 * vera_compat.h - what the VERA core (vera_core.c, vera_video.c) takes from
 *                 its host: the types and helpers of atari800, where the core
 *                 comes from, mapped onto VICE.
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

#ifndef VICE_VERA_COMPAT_H
#define VICE_VERA_COMPAT_H

#include <stdint.h>
#include <stdio.h>

/* the integer types of atari800 */
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef int16_t SWORD;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* log line of the core, in the VICE log (vera.c) */
void vera_log_print(const char *format, ...);
#define Log_print vera_log_print

/* ---- what the host machine provides (vera.c) ---- */

/* the IRQ output of the VERA (level: 1 asserted, 0 released) */
void vera_host_set_irq(int level);

/* a VERA frame is complete (scanline 480): 640x480 pixels, 0xAARRGGBB */
void vera_host_frame(const uint32_t *fb, int width, int height);

#endif
