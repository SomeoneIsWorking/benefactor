/* pc_config.h — tiny JSON config for user-tunable settings.
 *
 * Loads a flat JSON object (e.g. benefactor.json in the cwd, or $BENEFACTOR_CONFIG)
 * with numeric/boolean values, e.g.:
 *   { "widescreen": 480, "pickup_enabled": true, "interact_enabled": true,
 *     "interact_extend": 5 }
 *
 * Precedence for a setting is: matching env var (if set) > config file > default.
 * (Env stays as a quick dev override; the config file is the user-facing knob.)
 */
#pragma once

void pc_config_load(void);                          /* load benefactor.json / $BENEFACTOR_CONFIG */
int  pc_config_int (const char *key, int def);      /* number, or def if absent  */
int  pc_config_bool(const char *key, int def);      /* true/false/1/0, or def    */
