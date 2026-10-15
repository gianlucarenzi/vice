/** \file   uivera.c
 * \brief   Window of the VERA video card (c64/cart/vera.c)
 *
 * The VERA has its own VGA output: its 640x480 picture is shown in a window
 * of its own, beside the one of the VIC-II. The emulation thread hands over
 * each complete VERA frame (vera_set_ui_callbacks); this file copies it
 * under a lock and draws it from the UI thread, scaled to the window with
 * the 4:3 aspect kept.
 *
 * \author  Gianluca Renzi <gianlucarenzi@eurek.it>
 */

/*
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

#include <gtk/gtk.h>
#include <string.h>

#include "vera.h"
#include "uivera.h"

#define VERA_WIDTH  640
#define VERA_HEIGHT 480

static GtkWidget *vera_window = NULL;
static GtkWidget *vera_area = NULL;

/* the last frame, written by the emulation thread, read by the UI thread */
static GMutex frame_lock;
static uint32_t frame[VERA_WIDTH * VERA_HEIGHT];
static int frame_redraw_pending = 0;   /* an idle redraw is queued */
static int card_enabled = 0;           /* the card is in: the window is shown */

static gboolean on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    cairo_surface_t *surface;
    int w = gtk_widget_get_allocated_width(widget);
    int h = gtk_widget_get_allocated_height(widget);
    double scale, ox, oy;

    /* black bars around the 4:3 picture */
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    scale = (double)w / VERA_WIDTH;
    if ((double)h / VERA_HEIGHT < scale) {
        scale = (double)h / VERA_HEIGHT;
    }
    ox = (w - VERA_WIDTH * scale) / 2.0;
    oy = (h - VERA_HEIGHT * scale) / 2.0;

    g_mutex_lock(&frame_lock);
    surface = cairo_image_surface_create_for_data((unsigned char *)frame, CAIRO_FORMAT_RGB24,
                                                  VERA_WIDTH, VERA_HEIGHT, VERA_WIDTH * 4);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0, 0);
    /* whole pixels when the window is a multiple of 640x480 */
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_surface_destroy(surface);
    g_mutex_unlock(&frame_lock);
    return TRUE;
}

/* closing the window only hides it: the card is still in */
static gboolean on_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    gtk_widget_hide(widget);
    return TRUE;
}

static void create_window(void)
{
    vera_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(vera_window), "VERA");
    gtk_window_set_default_size(GTK_WINDOW(vera_window), VERA_WIDTH, VERA_HEIGHT);
    vera_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(vera_area, VERA_WIDTH / 2, VERA_HEIGHT / 2);
    gtk_container_add(GTK_CONTAINER(vera_window), vera_area);
    g_signal_connect(vera_area, "draw", G_CALLBACK(on_draw), NULL);
    g_signal_connect(vera_window, "delete-event", G_CALLBACK(on_delete), NULL);
}

/* UI thread: the window follows the card */
static gboolean update_visibility(gpointer data)
{
    if (card_enabled) {
        if (vera_window == NULL) {
            create_window();
        }
        gtk_widget_show_all(vera_window);
    } else if (vera_window != NULL) {
        gtk_widget_hide(vera_window);
    }
    return G_SOURCE_REMOVE;
}

/* UI thread: draw the frame that came in */
static gboolean redraw(gpointer data)
{
    g_mutex_lock(&frame_lock);
    frame_redraw_pending = 0;
    g_mutex_unlock(&frame_lock);
    if (vera_area != NULL && gtk_widget_get_visible(vera_window)) {
        gtk_widget_queue_draw(vera_area);
    }
    return G_SOURCE_REMOVE;
}

/* emulation thread: a complete VERA frame */
static void vera_frame(const uint32_t *fb, int width, int height)
{
    int queue;

    if (width != VERA_WIDTH || height != VERA_HEIGHT) {
        return;
    }
    g_mutex_lock(&frame_lock);
    memcpy(frame, fb, sizeof(frame));
    queue = !frame_redraw_pending;
    frame_redraw_pending = 1;
    g_mutex_unlock(&frame_lock);
    if (queue) {
        g_idle_add(redraw, NULL);
    }
}

/* any thread: the card was inserted or removed */
static void vera_enabled(int enabled)
{
    card_enabled = enabled;
    g_idle_add(update_visibility, NULL);
}

void uivera_init(void)
{
    g_mutex_init(&frame_lock);
    vera_set_ui_callbacks(vera_frame, vera_enabled);
}
