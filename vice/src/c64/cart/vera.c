/*
 * vera.c - VERA video card (Commander X16) on the C64 expansion port.
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

/*
    VERA video card

    The VERA of the Commander X16 (an iCE40 FPGA with 128 KB of video RAM, a
    640x480 VGA output, a 16 voice PSG, PCM audio and an SPI port for an SD
    card) on the expansion port: its 32 registers in IO1 ($DE00-$DE1F,
    images up to $DEFF, the card decodes A0-A4 and /IO1) or in IO2
    ($DF00-$DF1F), its IRQ output on /IRQ. The picture has its own window,
    the VERA runs free at 31468.75 lines per second (59.94 Hz), asynchronous
    to the C64.

    The chip is the one of the VeraX16 PBI card of atari800 (vera_core.c,
    vera_video.c); this file is the bus of the C64.
*/

#include "vice.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alarm.h"
#include "cartio.h"
#include "cartridge.h"
#include "cmdline.h"
#include "export.h"
#include "interrupt.h"
#include "lib.h"
#include "log.h"
#include "machine.h"
#include "maincpu.h"
#include "monitor.h"
#include "resources.h"
#include "snapshot.h"
#include "util.h"
#include "vera.h"
#include "vera_core.h"
#include "vera_video.h"

/* The VERA is advanced every VERA_HOST_PERIOD cycles, a "host line" of the
   core; it catches up on the 31468.75 lines per second of the VGA timing
   by the ratio of the rates (about 2 VERA lines per period). */
#define VERA_HOST_PERIOD 64

/* the FPGA loads its bitstream in about 100 ms after power-on: the bus is
   not driven meanwhile */
#define VERA_CONFIG_MS 100

static log_t vera_log = LOG_DEFAULT;

/* resources */
static int vera_enabled = 0;
static int vera_base_address = 0xde00;
static char *vera_sdcard_name = NULL;
static int vera_psg_volume = 100;
static int vera_debug_level = 0;

/* runtime */
static int vera_initialized = 0;        /* vera_init(): alarm and IRQ line exist */
static alarm_t *vera_alarm = NULL;
static CLOCK vera_alarm_clk = 0;
static unsigned int vera_int_num = 0;
static int vera_irq_level = 0;

static vera_frame_callback_t vera_ui_frame = NULL;
static vera_enabled_callback_t vera_ui_enabled = NULL;

/* ---------------------------------------------------------------------*/

static uint8_t vera_read(uint16_t addr);
static uint8_t vera_peek(uint16_t addr);
static void vera_store(uint16_t addr, uint8_t value);
static int vera_dump(void);

static io_source_t vera_device = {
    CARTRIDGE_NAME_VERA,      /* name of the device */
    IO_DETACH_RESOURCE,       /* use resource to detach the device when involved in a read-collision */
    "VERA",                   /* resource to set to '0' */
    0xde00, 0xdeff, 0x1f,     /* range for the device, regs: $de00-$de1f, images up to $deff */
    1,                        /* read is always valid */
    vera_store,               /* store function */
    NULL,                     /* NO poke function */
    vera_read,                /* read function */
    vera_peek,                /* peek function */
    vera_dump,                /* device state information dump function */
    CARTRIDGE_VERA,           /* cartridge ID */
    IO_PRIO_NORMAL,           /* normal priority, device read needs to be checked for collisions */
    0,                        /* insertion order, gets filled in by the registration function */
    IO_MIRROR_MASK            /* the registers are mirrored by the address mask */
};

static io_source_list_t *vera_list_item = NULL;

static export_resource_t export_res = {
    CARTRIDGE_NAME_VERA, 0, 0, &vera_device, NULL, CARTRIDGE_VERA
};

/* ---------------------------------------------------------------------*/
/* what the core takes from the host (vera_compat.h)                   */

void vera_log_print(const char *format, ...)
{
    char *text;
    va_list ap;

    va_start(ap, format);
    text = lib_mvsprintf(format, ap);
    va_end(ap);
    log_message(vera_log, "%s", text);
    lib_free(text);
}

void vera_host_set_irq(int level)
{
    vera_irq_level = level;
    if (vera_initialized) {
        maincpu_set_irq(vera_int_num, level ? IK_IRQ : IK_NONE);
    }
}

void vera_host_frame(const uint32_t *fb, int width, int height)
{
    if (vera_ui_frame != NULL) {
        vera_ui_frame(fb, width, height);
    }
}

void vera_set_ui_callbacks(vera_frame_callback_t frame, vera_enabled_callback_t enabled)
{
    vera_ui_frame = frame;
    vera_ui_enabled = enabled;
    if (vera_ui_enabled != NULL) {
        vera_ui_enabled(vera_enabled);
    }
}

/* ---------------------------------------------------------------------*/
/* time                                                                 */

static void vera_alarm_handler(CLOCK offset, void *data)
{
    vera_core_host_line();
    vera_alarm_clk += VERA_HOST_PERIOD;
    alarm_set(vera_alarm, vera_alarm_clk);
}

static void vera_start(void)
{
    long cycles = machine_get_cycles_per_second();

    vera_core_set_host_line_rate((unsigned int)((cycles * 100) / VERA_HOST_PERIOD));
    vera_alarm_clk = maincpu_clk + VERA_HOST_PERIOD;
    alarm_set(vera_alarm, vera_alarm_clk);
}

static void vera_stop(void)
{
    if (vera_alarm != NULL) {
        alarm_unset(vera_alarm);
    }
    vera_host_set_irq(0);
}

/* called once by cart_init at machine init */
void vera_init(void)
{
    vera_log = log_open("VERA");
    vera_alarm = alarm_new(maincpu_alarm_context, "VERA", vera_alarm_handler, NULL);
    vera_int_num = interrupt_cpu_status_int_new(maincpu_int_status, "VERA");
    vera_initialized = 1;
    if (vera_enabled) {
        vera_start();
    }
}

/* ---------------------------------------------------------------------*/
/* resources                                                            */

int vera_cart_enabled(void)
{
    return vera_enabled;
}

static int set_vera_enabled(int value, void *param)
{
    int val = value ? 1 : 0;

    if (!vera_enabled && val) {
        if (export_add(&export_res) < 0) {
            return -1;
        }
        vera_list_item = io_source_register(&vera_device);
        vera_core_enabled = TRUE;
        vera_core_set_debuglevel(vera_debug_level);
        vera_core_set_psg_volume(vera_psg_volume);
        vera_core_power_on("C64 VERA card inserted", VERA_CONFIG_MS);
        if (vera_sdcard_name != NULL && *vera_sdcard_name != '\0') {
            vera_core_sdcard_attach(vera_sdcard_name);
        }
        vera_enabled = 1;
        if (vera_initialized) {
            vera_start();
        }
        if (vera_ui_enabled != NULL) {
            vera_ui_enabled(1);
        }
    } else if (vera_enabled && !val) {
        if (vera_list_item != NULL) {
            export_remove(&export_res);
            io_source_unregister(vera_list_item);
            vera_list_item = NULL;
        }
        vera_stop();
        vera_core_shutdown();
        vera_enabled = 0;
        if (vera_ui_enabled != NULL) {
            vera_ui_enabled(0);
        }
    }
    return 0;
}

static int set_vera_base(int val, void *param)
{
    int old = vera_enabled;

    if (val == vera_base_address) {
        return 0;
    }

    switch (val) {
        case 0xde00:
            export_res.io1 = &vera_device;
            export_res.io2 = NULL;
            break;
        case 0xdf00:
            export_res.io1 = NULL;
            export_res.io2 = &vera_device;
            break;
        default:
            return -1;
    }

    if (old) {
        set_vera_enabled(0, NULL);
    }
    vera_device.start_address = (uint16_t)val;
    vera_device.end_address = (uint16_t)(val + 0xff);
    vera_base_address = val;
    if (old) {
        set_vera_enabled(1, NULL);
    }
    return 0;
}

static int set_vera_sdcard_name(const char *name, void *param)
{
    if (util_string_set(&vera_sdcard_name, name)) {
        return 0;
    }
    if (vera_enabled) {
        if (vera_sdcard_name != NULL && *vera_sdcard_name != '\0') {
            if (vera_core_sdcard_attach(vera_sdcard_name) < 0) {
                log_error(vera_log, "cannot open the SD card image '%s'", vera_sdcard_name);
            }
        } else {
            vera_core_sdcard_detach();
        }
    }
    return 0;
}

static int set_vera_psg_volume(int val, void *param)
{
    if (val < 0 || val > 400) {
        return -1;
    }
    vera_psg_volume = val;
    vera_core_set_psg_volume(val);
    return 0;
}

static int set_vera_debug_level(int val, void *param)
{
    vera_debug_level = val;
    vera_core_set_debuglevel(val);
    return 0;
}

static const resource_string_t resources_string[] = {
    { "VERASDCardImage", "", RES_EVENT_NO, NULL,
      &vera_sdcard_name, set_vera_sdcard_name, NULL },
    RESOURCE_STRING_LIST_END
};

static const resource_int_t resources_int[] = {
    { "VERA", 0, RES_EVENT_STRICT, (resource_value_t)0,
      &vera_enabled, set_vera_enabled, NULL },
    { "VERAbase", 0xde00, RES_EVENT_NO, NULL,
      &vera_base_address, set_vera_base, NULL },
    { "VERAPSGVolume", 100, RES_EVENT_NO, NULL,
      &vera_psg_volume, set_vera_psg_volume, NULL },
    { "VERADebugLevel", 0, RES_EVENT_NO, NULL,
      &vera_debug_level, set_vera_debug_level, NULL },
    RESOURCE_INT_LIST_END
};

int vera_resources_init(void)
{
    if (resources_register_string(resources_string) < 0) {
        return -1;
    }
    return resources_register_int(resources_int);
}

void vera_resources_shutdown(void)
{
    vera_core_shutdown();
    lib_free(vera_sdcard_name);
    vera_sdcard_name = NULL;
}

static const cmdline_option_t cmdline_options[] =
{
    { "-vera", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "VERA", (resource_value_t)1,
      NULL, "Enable the VERA video card" },
    { "+vera", SET_RESOURCE, CMDLINE_ATTRIB_NONE,
      NULL, NULL, "VERA", (resource_value_t)0,
      NULL, "Disable the VERA video card" },
    { "-verabase", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "VERAbase", NULL,
      "<Base address>", "Base address of the VERA video card. (0xDE00: IO1, 0xDF00: IO2)" },
    { "-verasdcard", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "VERASDCardImage", NULL,
      "<Name>", "Raw SD card image (for example made by dd) on the SPI port of the VERA" },
    { "-verapsgvolume", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "VERAPSGVolume", NULL,
      "<Percent>", "Level of the VERA PSG, 0-400 (default 100)" },
    { "-veradebuglevel", SET_RESOURCE, CMDLINE_ATTRIB_NEED_ARGS,
      NULL, NULL, "VERADebugLevel", NULL,
      "<Level>", "Log level of the VERA emulation (0: errors, 1: events, 2: every register access)" },
    CMDLINE_LIST_END
};

int vera_cmdline_options_init(void)
{
    return cmdline_register_options(cmdline_options);
}

/* ---------------------------------------------------------------------*/

int vera_enable(void)
{
    return resources_set_int("VERA", 1);
}

int vera_disable(void)
{
    return resources_set_int("VERA", 0);
}

void vera_detach(void)
{
    resources_set_int("VERA", 0);
}

/* The VERA has no reset input (only an internal power-on reset): the reset
   of the C64 leaves it as it is. */
void vera_reset(void)
{
}

/* ---------------------------------------------------------------------*/

static uint8_t vera_read(uint16_t addr)
{
    return vera_core_read(addr & 0x1f, 0);
}

static uint8_t vera_peek(uint16_t addr)
{
    return vera_core_read(addr & 0x1f, 1);
}

static void vera_store(uint16_t addr, uint8_t value)
{
    vera_core_write(addr & 0x1f, value);
}

static int vera_dump(void)
{
    int i;

    mon_out("VERA at $%04X, IRQ %s\n", (unsigned int)vera_base_address, vera_irq_level ? "asserted" : "released");
    for (i = 0; i < VERA_CORE_REG_COUNT; i++) {
        mon_out("%s%02X", (i & 15) ? " " : (i ? "\n" : ""), vera_core_read(i, 1));
    }
    mon_out("\n");
    return 0;
}

/* ---------------------------------------------------------------------*/

/* CARTVERA snapshot module format (the VRAM and the registers are not saved yet):

   type  | name     | description
   ------------------------------
   DWORD | base     | base address of the card
 */

static const char snap_module_name[] = "CARTVERA";
#define SNAP_MAJOR   0
#define SNAP_MINOR   0

int vera_snapshot_write_module(snapshot_t *s)
{
    snapshot_module_t *m;

    m = snapshot_module_create(s, snap_module_name, SNAP_MAJOR, SNAP_MINOR);
    if (m == NULL) {
        return -1;
    }
    if (SMW_DW(m, (uint32_t)vera_base_address) < 0) {
        snapshot_module_close(m);
        return -1;
    }
    return snapshot_module_close(m);
}

int vera_snapshot_read_module(snapshot_t *s)
{
    uint8_t vmajor, vminor;
    snapshot_module_t *m;
    int base;

    m = snapshot_module_open(s, snap_module_name, &vmajor, &vminor);
    if (m == NULL) {
        return -1;
    }
    if (snapshot_version_is_bigger(vmajor, vminor, SNAP_MAJOR, SNAP_MINOR)) {
        snapshot_set_error(SNAPSHOT_MODULE_HIGHER_VERSION);
        goto fail;
    }
    if (SMR_DW_INT(m, &base) < 0) {
        goto fail;
    }
    snapshot_module_close(m);

    if (set_vera_base(base, NULL) < 0) {
        return -1;
    }
    return vera_enable();

fail:
    snapshot_module_close(m);
    return -1;
}
