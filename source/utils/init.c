/* Modern Combat 2 single-module loader, derived from soloader-boilerplate. */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/settings.h"
#include "utils/utils.h"

#include <reimpl/controls.h>

#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <fios/fios.h>
#include <so_util/so_util.h>

#define GAME_LOAD_ADDRESS 0x98000000

extern so_module so_mod;
extern void port_trace(const char *format, ...);

static void verify_campaign_data(void) {
    static const char * const required_files[] = {
        DATA_PATH "GloftBPHP/data/Constants.bin",
        DATA_PATH "GloftBPHP/data/Res.array",
        DATA_PATH "GloftBPHP/data/Story.array",
        DATA_PATH "GloftBPHP/data/2d.header",
        DATA_PATH "GloftBPHP/data/2d.pak",
        DATA_PATH "GloftBPHP/data/3d.header",
        DATA_PATH "GloftBPHP/data/3d.pak",
        DATA_PATH "GloftBPHP/data/automat.header",
        DATA_PATH "GloftBPHP/data/automat.pak",
        DATA_PATH "GloftBPHP/data/structs.header",
        DATA_PATH "GloftBPHP/data/structs.pak",
        DATA_PATH "GloftBPHP/data/texts.header",
        DATA_PATH "GloftBPHP/data/texts.pak",
    };

    for (unsigned i = 0;
         i < sizeof(required_files) / sizeof(required_files[0]); ++i) {
        if (!file_exists(required_files[i]))
            fatal_error("Missing campaign file: %s. Prepare and copy the "
                        "original game data again.", required_files[i]);
    }
    port_trace("loader: all %u essential campaign archives are present",
               (unsigned)(sizeof(required_files) /
                          sizeof(required_files[0])));
}

void soloader_init_all(void) {
    port_trace("loader: soloader_init_all entered");

    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    port_trace("loader: starting FIOS for %s", DATA_PATH);
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
    port_trace("loader: FIOS initialization returned");
#endif

    port_trace("loader: checking kubridge");
    if (!module_loaded("kubridge"))
        fatal_error("Error: kubridge.skprx is not installed.");
    port_trace("loader: kubridge check passed");

    if (!file_exists(GAME_SO_PATH))
        fatal_error("Missing %s. Copy the prepared Modern Combat 2 data to %s.",
                    GAME_SO_PATH, DATA_PATH);
    verify_campaign_data();

    port_trace("loader: loading libsandstorm2.so at 0x%x", GAME_LOAD_ADDRESS);
    if (so_file_load(&so_mod, GAME_SO_PATH, GAME_LOAD_ADDRESS) < 0)
        fatal_error("Error: could not load %s.", GAME_SO_PATH);
    port_trace("loader: libsandstorm2.so loaded text=%p", so_mod.text_base);

    settings_load();
    port_trace("loader: settings loaded");

    so_relocate(&so_mod);
    port_trace("loader: libsandstorm2.so relocated");
    resolve_imports(&so_mod);
    port_trace("loader: imports resolved");
    so_patch();
    port_trace("loader: patches applied");
    so_flush_caches(&so_mod);
    port_trace("loader: caches flushed");
    so_initialize(&so_mod);
    port_trace("loader: constructors completed");

    gl_preload();
    port_trace("loader: shader compiler checked");
    jni_init();
    port_trace("loader: FalsoJNI initialized");
    controls_init();
    port_trace("loader: controls initialized");
}
