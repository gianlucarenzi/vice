/*
 * vera.h - VERA video card (Commander X16) on the C64 expansion port.
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

#ifndef VICE_VERA_H
#define VICE_VERA_H

#include <stdint.h>

#include "types.h"

struct snapshot_s;

void vera_init(void);
void vera_sound_chip_init(void);
int vera_resources_init(void);
void vera_resources_shutdown(void);
int vera_cmdline_options_init(void);

int vera_cart_enabled(void);
int vera_enable(void);
int vera_disable(void);
void vera_detach(void);
void vera_reset(void);

int vera_snapshot_write_module(struct snapshot_s *s);
int vera_snapshot_read_module(struct snapshot_s *s);

/* The user interface shows the VERA picture: frame() gets each complete
   frame (640x480, 0xAARRGGBB) from the emulation thread, enabled() tells
   when the card is inserted (1) or removed (0). */
typedef void (*vera_frame_callback_t)(const uint32_t *fb, int width, int height);
typedef void (*vera_enabled_callback_t)(int enabled);
void vera_set_ui_callbacks(vera_frame_callback_t frame, vera_enabled_callback_t enabled);

#endif
