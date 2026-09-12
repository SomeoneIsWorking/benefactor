#include "harness/puae_options.h"

#include <stddef.h>
#include <string.h>

struct Option {
    const char *key;
    const char *value;
};

static const struct Option options[] = {
    {"puae_model", "A1200"},
    {"puae_model_fd", "A500"},
    {"puae_model_hd", "A1200"},
    {"puae_model_cd", "CD32"},
    {"puae_kickstart", "auto"},
    {"puae_chipmem_size", "2MB"},
    {"puae_bogomem_size", "none"},
    {"puae_fastmem_size", "4MB"},
    {"puae_cpu_model", "68020"},
    {"puae_cpu_multiplier", "0"},
    {"puae_cpu_throttle", "0.0"},
    {"puae_cpu_compatibility", "normal"},
    {"puae_fpu_model", "none"},
    {"puae_immediate_blits", "false"},
    {"puae_collision_level", "sprites"},
    {"puae_gfx_framerate", "0"},
    {"puae_gfx_colors", "16bit"},
    {"puae_floppy_speed", "100"},
    {"puae_floppy_multidrive", "disabled"},
    {"puae_floppy_sound", "disabled"},
    {"puae_floppy_sound_empty_mute", "disabled"},
    {"puae_floppy_write_protection", "disabled"},
    {"puae_floppy_write_redirect", "disabled"},
    {"puae_statusbar", "disabled"},
    {"puae_use_whdload_buttonwait", "disabled"},
};

const char *puae_option_value(const char *key) {
    if (key == NULL) {
        return NULL;
    }
    for (size_t index = 0; index < sizeof options / sizeof options[0]; ++index) {
        if (strcmp(key, options[index].key) == 0) {
            return options[index].value;
        }
    }
    return NULL;
}
