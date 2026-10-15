/** \file   settings_vera.c
 * \brief   Settings widget controlling the VERA video card resources
 *
 * \author  Gianluca Renzi <gianlucarenzi@eurek.it>
 */

/*
 * $VICERES VERA              x64 x64sc xscpu64 x128
 * $VICERES VERAbase          x64 x64sc xscpu64 x128
 * $VICERES VERASDCardImage   x64 x64sc xscpu64 x128
 * $VICERES VERAPSGVolume     x64 x64sc xscpu64 x128
 * $VICERES VERADebugLevel    x64 x64sc xscpu64 x128
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

#include "cartridge.h"
#include "resources.h"
#include "vice_gtk3.h"

#include "settings_vera.h"


/** \brief  I/O base of the card: IO1 or IO2 */
static const int io_base[] = { 0xde00, 0xdf00, -1 };

/** \brief  Log levels of the emulation */
static const vice_gtk3_combo_entry_int_t debug_levels[] = {
    { "Errors only",           0 },
    { "Events",                1 },
    { "Every register access", 2 },
    { NULL,                   -1 }
};


/** \brief  Reference to the grid of the settings that need the card */
static GtkWidget *options_grid = NULL;


/** \brief  Handler for the "toggled" event of the enable widget
 *
 * \param[in,out]   widget      "enable" toggle button
 * \param[in]       user_data   extra event data (unused)
 */
static void on_enable_toggled(GtkWidget *widget, gpointer user_data)
{
    gtk_widget_set_sensitive(options_grid,
                             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

/** \brief  Add a label and a widget to a row of \a grid
 *
 * \param[in]   grid    grid
 * \param[in]   row     row
 * \param[in]   text    text of the label
 * \param[in]   widget  widget
 */
static void add_row(GtkWidget *grid, int row, const char *text, GtkWidget *widget)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label,  0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
}


/** \brief  Create widget to control the VERA video card
 *
 * \param[in]   parent  parent widget
 *
 * \return  GtkGrid
 */
GtkWidget *settings_vera_widget_create(GtkWidget *parent)
{
    GtkWidget *grid;
    GtkWidget *enable;
    GtkWidget *sdcard;
    GtkWidget *volume;

    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);

    enable = carthelpers_create_enable_check_button(CARTRIDGE_NAME_VERA,
                                                    CARTRIDGE_VERA);
    gtk_grid_attach(GTK_GRID(grid), enable, 0, 0, 1, 1);

    options_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(options_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(options_grid), 8);

    add_row(options_grid, 0, "I/O base address",
            vice_gtk3_resource_combo_hex_new_list("VERAbase", io_base));

    sdcard = vice_gtk3_resource_browser_new("VERASDCardImage",
                                            NULL,
                                            NULL,
                                            "Select the SD card image",
                                            NULL,
                                            NULL);
    gtk_widget_set_hexpand(sdcard, TRUE);
    add_row(options_grid, 1, "SD card image", sdcard);

    volume = vice_gtk3_resource_spin_int_new("VERAPSGVolume", 0, 400, 10);
    gtk_widget_set_halign(volume, GTK_ALIGN_START);
    add_row(options_grid, 2, "PSG level (%)", volume);

    add_row(options_grid, 3, "Log level",
            vice_gtk3_resource_combo_int_new("VERADebugLevel", debug_levels));

    gtk_grid_attach(GTK_GRID(grid), options_grid, 0, 1, 1, 1);

    g_signal_connect_unlocked(G_OBJECT(enable),
                              "toggled",
                              G_CALLBACK(on_enable_toggled),
                              NULL);
    on_enable_toggled(enable, NULL);

    gtk_widget_show_all(grid);
    return grid;
}
