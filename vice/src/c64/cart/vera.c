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
    the VERA runs free on its own 25 MHz clock, asynchronous to the C64:
    VGA at 59.52 Hz, NTSC composite at 59.97 Hz (59.86 Hz in 240p), while
    the C64 runs at 50.12 Hz (PAL) or 59.83 Hz (NTSC).

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
#include "georam.h"
#include "interrupt.h"
#include "lib.h"
#include "log.h"
#include "machine.h"
#include "maincpu.h"
#include "monitor.h"
#include "resources.h"
#include "snapshot.h"
#include "sound.h"
#include "util.h"
#include "vera.h"
#include "vera_core.h"
#include "vera_video.h"

/* The VERA is advanced every VERA_HOST_PERIOD cycles, a "host line" of the
   core; it catches up on the VERA line rate (31250 lines per second in VGA,
   15743 in composite) by the ratio of the rates (about 2 VERA lines per
   period in VGA). */
#define VERA_HOST_PERIOD 64

/* the FPGA loads its bitstream in about 100 ms after power-on: the bus is
   not driven meanwhile */
#define VERA_CONFIG_MS 100

static log_t vera_log = LOG_DEFAULT;

/* resources */
static int vera_enabled = 0;
static int vera_base_address = 0xdf00;
static char *vera_sdcard_name = NULL;
static int vera_psg_volume = 100;
static int vera_debug_level = 0;

/* runtime */
static int vera_initialized = 0;        /* vera_init(): alarm and IRQ line exist */
static alarm_t *vera_alarm = NULL;
static CLOCK vera_alarm_clk = 0;
static unsigned int vera_int_num = 0;
static int vera_irq_level = 0;

static unsigned int vera_sound_speed = 0; /* sample rate of the host mixer */

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
    0xdf00, 0xdf1f, 0x1f,     /* range for the device: regs $df00-$df1f, no mirrors (the GeoRAM
                                 of the card uses $de00-$deff and $df80-$dfff) */
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
    CARTRIDGE_NAME_VERA, 0, 0, NULL, &vera_device, CARTRIDGE_VERA
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

unsigned int vera_host_cpu_clock(void)
{
    return (unsigned int)maincpu_clk;
}

unsigned int vera_host_cpu_hz(void)
{
    return (unsigned int)machine_get_cycles_per_second();
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

/* the host line rate follows the C64 clock: PAL 985248 Hz, NTSC 1022727 Hz */
static void vera_set_host_rate(void)
{
    long cycles = machine_get_cycles_per_second();

    vera_core_set_host_line_rate((unsigned int)((cycles * 100) / VERA_HOST_PERIOD));
}

static void vera_start(void)
{
    vera_set_host_rate();
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
/* sound: the PSG (16 voices) and the PCM FIFO of the VERA, in stereo,
   added to the output of the other chips. The core renders the samples
   when the mixer asks for them, so the PCM FIFO drains with the sound
   output (as on the atari800 card). */

static int vera_sound_machine_init(sound_t *psid, int speed, int cycles_per_sec)
{
    vera_sound_speed = (unsigned int)speed;
    vera_core_sound_init(vera_sound_speed, 1);
    return 1;
}

static void vera_sound_machine_close(sound_t *psid)
{
}

static int vera_sound_machine_calculate_samples(sound_t **psid, int16_t *pbuf, int nr, int soc, int scc, CLOCK *delta_t)
{
    vera_core_sound_init(vera_sound_speed, (unsigned int)soc);
    vera_core_sound_mix(pbuf, nr, (unsigned int)soc);
    return nr;
}

static void vera_sound_machine_store(sound_t *psid, uint16_t addr, uint8_t val)
{
}

static uint8_t vera_sound_machine_read(sound_t *psid, uint16_t addr)
{
    return 0;
}

static void vera_sound_reset(sound_t *psid, CLOCK cpu_clk)
{
}

static int vera_sound_machine_cycle_based(void)
{
    return 0;
}

static int vera_sound_machine_channels(void)
{
    return 2;   /* the PSG voices and the PCM have a left and a right route */
}

static sound_chip_t vera_sound_chip = {
    NULL,                                 /* NO sound chip open function */
    vera_sound_machine_init,              /* sound chip init function */
    vera_sound_machine_close,             /* sound chip close function */
    vera_sound_machine_calculate_samples, /* sound chip calculate samples function */
    vera_sound_machine_store,             /* sound chip store function */
    vera_sound_machine_read,              /* sound chip read function */
    vera_sound_reset,                     /* sound chip reset function */
    vera_sound_machine_cycle_based,       /* sound chip 'is_cycle_based()' function, sound chip is NOT cycle based */
    vera_sound_machine_channels,          /* sound chip 'get_amount_of_channels()' function, sound chip has 2 channels */
    0                                     /* chip enabled, toggled when the card is inserted or removed */
};

/* called once by cartridge_sound_chip_init */
void vera_sound_chip_init(void)
{
    sound_chip_register(&vera_sound_chip);
}

/* ---------------------------------------------------------------------*/
/* resources                                                            */

int vera_cart_enabled(void)
{
    return vera_enabled;
}

/* The card carries a GeoRAM of 256 KB: window $de00-$deff, registers
   $dffe/$dfff (mirrors $df80-$dffd), with VERA at $df00-$df1f.  Enabled
   with the card when VERA is at $df00, and while the card is there the
   GeoRAM cannot be larger (georam_set_max_size); with VERA at $de00 the two
   would collide, so there is none. */
#define VERA_GEORAM_KB 256
static int vera_georam_on = 0;

static void vera_georam(int on)
{
    if (on && vera_base_address == 0xdf00) {
        int en = 0;
        georam_set_max_size(VERA_GEORAM_KB);
        resources_set_int("GEORAMsize", VERA_GEORAM_KB);
        resources_get_int("GEORAM", &en);
        if (!en) {
            if (resources_set_int("GEORAM", 1) == 0) {
                vera_georam_on = 1;
                log_message(vera_log, "GeoRAM %d KB of the VERA card enabled", VERA_GEORAM_KB);
            }
        }
    } else if (!on) {
        if (vera_georam_on) {
            resources_set_int("GEORAM", 0);
            vera_georam_on = 0;
        }
        georam_set_max_size(4096);
    }
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
        vera_sound_chip.chip_enabled = 1;
        sound_state_changed = TRUE;     /* open the output again, in stereo */
        if (vera_initialized) {
            vera_start();
        }
        if (vera_ui_enabled != NULL) {
            vera_ui_enabled(1);
        }
        vera_georam(1);
    } else if (vera_enabled && !val) {
        vera_georam(0);
        if (vera_list_item != NULL) {
            export_remove(&export_res);
            io_source_unregister(vera_list_item);
            vera_list_item = NULL;
        }
        vera_stop();
        vera_core_shutdown();
        vera_enabled = 0;
        vera_sound_chip.chip_enabled = 0;
        sound_state_changed = TRUE;
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
    vera_device.end_address = (uint16_t)(val + 0x1f);
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
    { "VERAbase", 0xdf00, RES_EVENT_NO, NULL,
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
      "<Base address>", "Base address of the VERA video card. (0xDF00: IO2 $DF00-$DF1F, default, with the GeoRAM 256 KB of the card; 0xDE00: IO1, no GeoRAM)" },
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
   of the C64 leaves it as it is. A change of PAL / NTSC resets the machine
   with its new clock: the host line rate is taken again. */
void vera_reset(void)
{
    if (vera_enabled && vera_initialized) {
        vera_set_host_rate();
    }
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

/* CARTVERA snapshot module format:

   type  | name     | description
   ------------------------------
   DWORD | base     | base address of the card
   DWORD | alarm    | cycles to the next VERA host line (since 0.1)
   DWORD | size     | size of the chip state (since 0.1)
   ARRAY | state    | the chip state, VRAM included (vera_core_state_save, since 0.1)

   The SD card image is not in the snapshot: VERASDCardImage stays as it is.
 */

static const char snap_module_name[] = "CARTVERA";
#define SNAP_MAJOR   0
#define SNAP_MINOR   1

int vera_snapshot_write_module(snapshot_t *s)
{
    snapshot_module_t *m;
    size_t size = vera_core_state_size();
    uint8_t *state;
    int rc = -1;

    m = snapshot_module_create(s, snap_module_name, SNAP_MAJOR, SNAP_MINOR);
    if (m == NULL) {
        return -1;
    }
    state = lib_malloc(size);
    vera_core_state_save(state);
    if (SMW_DW(m, (uint32_t)vera_base_address) >= 0
        && SMW_DW(m, (uint32_t)(vera_alarm_clk - maincpu_clk)) >= 0
        && SMW_DW(m, (uint32_t)size) >= 0
        && SMW_BA(m, state, (unsigned int)size) >= 0) {
        rc = 0;
    }
    lib_free(state);
    if (rc < 0) {
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
    uint32_t alarm_delta = VERA_HOST_PERIOD;
    uint32_t size = 0;
    uint8_t *state = NULL;

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
    if (!snapshot_version_is_smaller(vmajor, vminor, 0, 1)) {
        if (SMR_DW(m, &alarm_delta) < 0 || SMR_DW(m, &size) < 0) {
            goto fail;
        }
        if (size != vera_core_state_size()) {
            snapshot_set_error(SNAPSHOT_MODULE_INCOMPATIBLE);
            goto fail;
        }
        state = lib_malloc(size);
        if (SMR_BA(m, state, size) < 0) {
            goto fail;
        }
    }
    snapshot_module_close(m);

    if (set_vera_base(base, NULL) < 0 || vera_enable() < 0) {
        lib_free(state);
        return -1;
    }
    if (state != NULL) {
        int rc = vera_core_state_load(state, size);

        lib_free(state);
        if (rc < 0) {
            snapshot_set_error(SNAPSHOT_MODULE_INCOMPATIBLE);
            return -1;
        }
        if (alarm_delta == 0 || alarm_delta > VERA_HOST_PERIOD) {
            alarm_delta = VERA_HOST_PERIOD;
        }
        vera_alarm_clk = maincpu_clk + alarm_delta;
        alarm_set(vera_alarm, vera_alarm_clk);
    }
    return 0;

fail:
    lib_free(state);
    snapshot_module_close(m);
    return -1;
}
