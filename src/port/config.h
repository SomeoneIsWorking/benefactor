/* pc_config.h — unified config for user-tunable settings.
 *
 * A setting is one named knob (e.g. "interact_extend", "modern_controls",
 * "widescreen"). Its value is resolved with a single, consistent precedence:
 *
 *     ENV  >  REPL (session)  >  JSON file  >  caller default
 *
 *   - ENV: the canonical env var is BENEFACTOR_<KEY uppercased>, e.g.
 *     interact_extend -> BENEFACTOR_INTERACT_EXTEND. One rule, always aligned
 *     with the JSON key (no more bespoke names like PICKUP_RX).
 *   - REPL: set live at runtime via pc_cfg_set() (harness `cfg <key> <value>`).
 *     Outranks the file so you can tune without editing/restarting; prefer this.
 *   - JSON: benefactor.json / $BENEFACTOR_CONFIG, the user-facing persistent knob.
 *   - default: the caller's fallback.
 *
 * Read knobs with pc_cfg_int/pc_cfg_bool (NOT the legacy pc_config_* below, which
 * are JSON-only primitives kept for the loader internals).
 */
#pragma once

void pc_config_load(void);                          /* load benefactor.json / $BENEFACTOR_CONFIG */

/* Unified resolvers (ENV > REPL > JSON > def). Use these for all settings. */
int  pc_cfg_int (const char *key, int def);
int  pc_cfg_bool(const char *key, int def);
void pc_cfg_set (const char *key, const char *val); /* REPL/session override; val=NULL clears */
/* Resolve to a display string + report which source won ("env"/"repl"/"json"/
 * "default"). Returns 1 if a value was found (else *src="default", out=""). */
int  pc_cfg_show(const char *key, char *out, int cap, const char **src);
/* Declared knobs, for `cfg` with no args (discoverability). */
int          pc_cfg_count(void);
const char  *pc_cfg_key (int i);
const char  *pc_cfg_desc(int i);

/* Legacy JSON-only primitives (no ENV/REPL layer). Prefer pc_cfg_* above. */
int  pc_config_int (const char *key, int def);      /* number, or def if absent  */
int  pc_config_bool(const char *key, int def);      /* true/false/1/0, or def    */
int  pc_config_str (const char *key, char *out, int cap); /* quoted string → out; 1 if found */
