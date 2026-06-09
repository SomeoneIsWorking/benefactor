/* pc_config.c — tiny flat-JSON config loader (see pc_config.h). No external deps:
 * it just locates "key": <value> in the file text and parses a number/bool. Good
 * enough for a flat object of settings; not a general JSON parser. */
#include "port/config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static char *s_cfg = NULL;     /* whole file text, NUL-terminated */

void pc_config_load(void)
{
    if (s_cfg) return;                    /* idempotent */
    const char *path = getenv("BENEFACTOR_CONFIG");
    if (!path || !*path) path = "benefactor.json";
    FILE *f = fopen(path, "rb");
    if (!f) return;                       /* no config -> all defaults */
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (1 << 20)) { fclose(f); return; }
    s_cfg = (char *)malloc((size_t)n + 1);
    if (s_cfg) { size_t got = fread(s_cfg, 1, (size_t)n, f); s_cfg[got] = 0; }
    fclose(f);
    if (s_cfg) fprintf(stderr, "[config] loaded %s\n", path);
}

/* Return a pointer just past the ':' for "key", or NULL. Matches the quoted key
 * exactly (keys are distinct full names, so no substring collisions). */
static const char *find_value(const char *key)
{
    if (!s_cfg) return NULL;
    char pat[96];
    int pl = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (pl <= 0 || pl >= (int)sizeof pat) return NULL;
    const char *p = strstr(s_cfg, pat);
    if (!p) return NULL;
    p += pl;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return NULL;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

int pc_config_int(const char *key, int def)
{
    const char *v = find_value(key);
    if (!v) return def;
    return (int)strtol(v, NULL, 0);       /* base 0: supports 0x.. and -.. */
}

int pc_config_bool(const char *key, int def)
{
    const char *v = find_value(key);
    if (!v) return def;
    if (!strncmp(v, "true", 4))  return 1;
    if (!strncmp(v, "false", 5)) return 0;
    return (int)strtol(v, NULL, 0) != 0;
}

/* Copy a quoted string value into out (NUL-terminated, truncated to cap). Returns 1 on
 * success, 0 if the key is absent or not a string. */
int pc_config_str(const char *key, char *out, int cap)
{
    const char *v = find_value(key);
    if (!v || *v != '"' || cap <= 0) return 0;
    v++;
    int i = 0;
    while (*v && *v != '"' && i < cap - 1) {
        if (*v == '\\' && v[1]) v++;      /* allow simple escapes */
        out[i++] = *v++;
    }
    out[i] = 0;
    return 1;
}

/* ── Unified settings: ENV > REPL(session) > JSON file > default ───────────────
 * One resolution path for every knob. The canonical env var for a key is
 * BENEFACTOR_<KEY uppercased>; REPL overrides are set live via pc_cfg_set(). */

/* Declared knobs — only used for `cfg` listing/discoverability; resolution works
 * for ANY key whether declared or not. */
static const struct { const char *key, *desc; } s_cfg_decl[] = {
    { "modern_controls", "modern controls: X=interact/pickup, FIRE no longer interacts (bool)" },
    { "interact_extend", "extra horizontal pickup/interact reach, px (0 = vanilla)" },
    { "widescreen",      "widescreen output width, px (0 = native 352)" },
};
int         pc_cfg_count(void)    { return (int)(sizeof s_cfg_decl / sizeof s_cfg_decl[0]); }
const char *pc_cfg_key (int i)    { return (i >= 0 && i < pc_cfg_count()) ? s_cfg_decl[i].key  : NULL; }
const char *pc_cfg_desc(int i)    { return (i >= 0 && i < pc_cfg_count()) ? s_cfg_decl[i].desc : NULL; }

#define CFG_MAX_OVERRIDES 32
static struct { char key[48]; char val[48]; } s_over[CFG_MAX_OVERRIDES];
static int s_over_n = 0;

/* getenv("BENEFACTOR_<KEY uppercased>"), or NULL. */
static const char *cfg_env(const char *key)
{
    char name[96];
    int n = snprintf(name, sizeof name, "BENEFACTOR_");
    for (const char *p = key; *p && n < (int)sizeof name - 1; p++)
        name[n++] = (char)toupper((unsigned char)*p);
    name[n] = 0;
    return getenv(name);
}

static const char *cfg_session(const char *key)
{
    for (int i = 0; i < s_over_n; i++)
        if (!strcmp(s_over[i].key, key)) return s_over[i].val;
    return NULL;
}

void pc_cfg_set(const char *key, const char *val)
{
    for (int i = 0; i < s_over_n; i++) {
        if (strcmp(s_over[i].key, key)) continue;
        if (val) snprintf(s_over[i].val, sizeof s_over[i].val, "%s", val);
        else     s_over[i] = s_over[--s_over_n];        /* clear → compact */
        return;
    }
    if (val && s_over_n < CFG_MAX_OVERRIDES) {
        snprintf(s_over[s_over_n].key, sizeof s_over[s_over_n].key, "%s", key);
        snprintf(s_over[s_over_n].val, sizeof s_over[s_over_n].val, "%s", val);
        s_over_n++;
    }
}

static int cfg_parse_bool(const char *v, int def)
{
    if (!v) return def;
    while (*v && isspace((unsigned char)*v)) v++;
    if (!strncmp(v, "true", 4))  return 1;
    if (!strncmp(v, "false", 5)) return 0;
    return (int)strtol(v, NULL, 0) != 0;
}

int pc_cfg_int(const char *key, int def)
{
    const char *v;
    if ((v = cfg_env(key)))     return (int)strtol(v, NULL, 0);
    if ((v = cfg_session(key))) return (int)strtol(v, NULL, 0);
    if ((v = find_value(key)))  return (int)strtol(v, NULL, 0);
    return def;
}

int pc_cfg_bool(const char *key, int def)
{
    const char *v;
    if ((v = cfg_env(key)))     return cfg_parse_bool(v, def);
    if ((v = cfg_session(key))) return cfg_parse_bool(v, def);
    if ((v = find_value(key)))  return cfg_parse_bool(v, def);
    return def;
}

/* Copy a JSON scalar/string token (stops at , } ] or whitespace; unquotes). */
static void cfg_copy_token(const char *v, char *out, int cap)
{
    if (cap <= 0) return;
    int i = 0;
    if (*v == '"') {
        v++;
        while (*v && *v != '"' && i < cap - 1) { if (*v == '\\' && v[1]) v++; out[i++] = *v++; }
    } else {
        while (*v && !strchr(",}] \t\r\n", *v) && i < cap - 1) out[i++] = *v++;
    }
    out[i] = 0;
}

int pc_cfg_show(const char *key, char *out, int cap, const char **src)
{
    const char *v;
    if ((v = cfg_env(key)))     { snprintf(out, cap, "%s", v); if (src) *src = "env";  return 1; }
    if ((v = cfg_session(key))) { snprintf(out, cap, "%s", v); if (src) *src = "repl"; return 1; }
    if ((v = find_value(key)))  { cfg_copy_token(v, out, cap); if (src) *src = "json"; return 1; }
    if (cap > 0) out[0] = 0;
    if (src) *src = "default";
    return 0;
}
