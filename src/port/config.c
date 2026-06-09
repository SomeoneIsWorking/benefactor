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
