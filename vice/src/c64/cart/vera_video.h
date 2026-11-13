/*
 * vera_video.h - VERA video output (vera_video.c).
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

#ifndef VICE_VERA_VIDEO_H
#define VICE_VERA_VIDEO_H

#include "vera_compat.h"

#define VERA_VIDEO_SCALE_MIN 1
#define VERA_VIDEO_SCALE_MAX 4

int  VERA_VIDEO_Init(void);
void VERA_VIDEO_Frame(void);
int  VERA_VIDEO_VramWrite(ULONG lo, ULONG hi);
void VERA_VIDEO_SetScale(int scale);
int  VERA_VIDEO_GetScale(void);
void VERA_VIDEO_Reset(void);
void VERA_VIDEO_Scanline(UWORD scanline);
void VERA_VIDEO_Midline(UWORD scanline, UWORD xstart);
void VERA_VIDEO_Recolor(UWORD scanline, UWORD xstart);
void VERA_VIDEO_Exit(void);

/* Renderer: EXACT renders every line of every frame from VRAM and the
 * registers as the chip does (no line cache, no line copy, no fast tile
 * path); FAST keeps a line until something it shows is written; AUTO starts
 * EXACT and keeps it while it costs at most half a VERA frame of host time
 * and the emulator keeps up with real time, else FAST (tried again every
 * 10 s; vera_video.c vera_auto_frame).  VERA_VIDEO_GetRenderer() = the mode
 * in use now. */
#define VERA_RENDER_EXACT 0
#define VERA_RENDER_FAST  1
#define VERA_RENDER_AUTO  2
void VERA_VIDEO_SetRenderer(int mode);
int  VERA_VIDEO_GetRenderer(void);

/* sprite_renderer.v render time: how many pixels (in screen order) of each of the
 * 128 sprites get drawn on scaled sprite line ly (0 = none). */
void VERA_VIDEO_SpriteDrawCounts(const UBYTE *vram, int ly, UBYTE draw_n[128]);

/* composer.v's scaled vertical counter: the source (layer/sprite) line shown on
 * output line py of the current frame, accumulated line by line. */
int VERA_VIDEO_SourceLine(int py);

#endif

