/* pc_config.c — tiny flat-JSON config loader (see pc_config.h). No external
 * deps: it just locates "key": <value> in the file text and parses a
 * number/bool. Good enough for a flat object of settings; not a general JSON
 * parser. */
#include "port/config.h"
#include "common/log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */

static char *s_cfg = NULL; /* whole file text, NUL-terminated */
static int s_loaded;

enum {
    CONFIG_FILE_MAX_BYTES = 1 << 20,
};

static const char *cfg_path(void) {
    const char *path = getenv("BENEFACTOR_CONFIG");
    return (path && *path) ? path : "benefactor.json";
}

static BenefactorLogLevel parse_log_level(const char *value) {
    if (strcasecmp(value, "trace") == 0)
        return BENEFACTOR_LOG_TRACE;
    if (strcasecmp(value, "debug") == 0)
        return BENEFACTOR_LOG_DEBUG;
    if (strcasecmp(value, "warning") == 0 || strcasecmp(value, "warn") == 0)
        return BENEFACTOR_LOG_WARNING;
    if (strcasecmp(value, "error") == 0)
        return BENEFACTOR_LOG_ERROR;
    return BENEFACTOR_LOG_INFO;
}

static void configure_logging(void) {
    char value[16];
    pc_cfg_string("log_level", "info", value, sizeof value);
    BenefactorLogLevel level = parse_log_level(value);
    benefactor_log_set_level(level);
    if (level == BENEFACTOR_LOG_INFO && strcasecmp(value, "info") != 0)
        benefactor_log_write(BENEFACTOR_LOG_WARNING, "config", "unknown log_level '%s'; using info",
                             value);
}

void pc_config_load(void) {
    if (s_loaded)
        return; /* idempotent */
    s_loaded = 1;
    const char *path = cfg_path();
    FILE *f = fopen(path, "rb");
    if (!f) {
        configure_logging();
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > CONFIG_FILE_MAX_BYTES) {
        fclose(f);
        configure_logging();
        return;
    }
    s_cfg = (char *)malloc(CONFIG_FILE_MAX_BYTES + 1u);
    if (s_cfg) {
        size_t expected = (size_t)n;
        size_t got = fread(s_cfg, 1, expected, f);
        if (got != expected) {
            free(s_cfg);
            s_cfg = NULL;
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "config", "cannot read complete file %s",
                                 path);
        } else
            s_cfg[expected] = 0;
    }
    fclose(f);
    configure_logging();
    if (s_cfg)
        benefactor_log_write(BENEFACTOR_LOG_INFO, "config", "loaded %s", path);
}

/* Return a pointer just past the ':' for "key", or NULL. Matches the quoted key
 * exactly (keys are distinct full names, so no substring collisions). */
static const char *find_value(const char *key) {
    if (!s_cfg)
        return NULL;
    char pat[96];
    int pl = snprintf(pat, sizeof pat, "\"%s\"", key);
    if (pl <= 0 || pl >= (int)sizeof pat)
        return NULL;
    const char *p = strstr(s_cfg, pat);
    if (!p)
        return NULL;
    p += pl;
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p != ':')
        return NULL;
    p++;
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

int pc_config_int(const char *key, int def) {
    const char *v = find_value(key);
    if (!v)
        return def;
    return (int)strtol(v, NULL, 0); /* base 0: supports 0x.. and -.. */
}

int pc_config_bool(const char *key, int def) {
    const char *v = find_value(key);
    if (!v)
        return def;
    if (!strncmp(v, "true", 4))
        return 1;
    if (!strncmp(v, "false", 5))
        return 0;
    return (int)strtol(v, NULL, 0) != 0;
}

/* Copy a quoted string value into out (NUL-terminated, truncated to cap).
 * Returns 1 on success, 0 if the key is absent or not a string. */
int pc_config_str(const char *key, char *out, int cap) {
    const char *v = find_value(key);
    if (!v || *v != '"' || cap <= 0)
        return 0;
    v++;
    int i = 0;
    while (*v && *v != '"' && i < cap - 1) {
        if (*v == '\\' && v[1])
            v++; /* allow simple escapes */
        out[i++] = *v++;
    }
    out[i] = 0;
    return 1;
}

/* ── Unified settings: ENV > REPL(session) > JSON file > default
 * ─────────────── One resolution path for every knob. The canonical env var for
 * a key is BENEFACTOR_<KEY uppercased>; REPL overrides are set live via
 * pc_cfg_set(). */

/* Declared knobs — only used for `cfg` listing/discoverability; resolution
 * works for ANY key whether declared or not. */
static const struct {
    const char *key, *desc;
} s_cfg_decl[] = {
    {"modern_controls", "legacy: default for BOTH per-device modern flags (bool)"},
    {"modern_controls_keyboard", "modern controls on the keyboard: X=interact, jump button (bool)"},
    {"modern_controls_controller", "modern controls on the controller (bool)"},
    {"interact_extend", "extra horizontal pickup/interact reach, px (0 = vanilla)"},
    {"widescreen", "widescreen output width, px (0 = native 352)"},
    {"widescreen_mode", "widescreen preset: disabled | 16:9 | ultrawide | auto (window aspect)"},
    {"renderer", "frame renderer: vanilla (Amiga blit) | benren (sprite-based, "
                 "native; hosts widescreen+effects)"},
    {"log_level", "minimum process log level: trace | debug | info | warning | error"},
    {"present", "present backend: sdl (software) | vulkan (BenRen VK "
                "per-sprite GPU renderer; benren only)"},
    {"fx_ambient", "GPU effect (Hardware): ambient darkness vignette (bool)"},
    {"fx_shadow", "GPU effect (Hardware): drop shadow behind characters (bool)"},
};
int pc_cfg_count(void) { return (int)(sizeof s_cfg_decl / sizeof s_cfg_decl[0]); }
const char *pc_cfg_key(int i) { return (i >= 0 && i < pc_cfg_count()) ? s_cfg_decl[i].key : NULL; }
const char *pc_cfg_desc(int i) {
    return (i >= 0 && i < pc_cfg_count()) ? s_cfg_decl[i].desc : NULL;
}

#define CFG_MAX_OVERRIDES 32
static struct {
    char key[48];
    char val[48];
} s_over[CFG_MAX_OVERRIDES];
static int s_over_n = 0;

/* getenv("BENEFACTOR_<KEY uppercased>"), or NULL. */
static const char *cfg_env(const char *key) {
    char name[96];
    int n = snprintf(name, sizeof name, "BENEFACTOR_");
    for (const char *p = key; *p && n < (int)sizeof name - 1; p++)
        name[n++] = (char)toupper((unsigned char)*p);
    name[n] = 0;
    return getenv(name);
}

static const char *cfg_session(const char *key) {
    for (int i = 0; i < s_over_n; i++)
        if (!strcmp(s_over[i].key, key))
            return s_over[i].val;
    return NULL;
}

void pc_cfg_set(const char *key, const char *val) {
    for (int i = 0; i < s_over_n; i++) {
        if (strcmp(s_over[i].key, key) != 0)
            continue;
        if (val)
            snprintf(s_over[i].val, sizeof s_over[i].val, "%s", val);
        else
            s_over[i] = s_over[--s_over_n]; /* clear → compact */
        if (strcmp(key, "log_level") == 0)
            configure_logging();
        return;
    }
    if (val && s_over_n < CFG_MAX_OVERRIDES) {
        snprintf(s_over[s_over_n].key, sizeof s_over[s_over_n].key, "%s", key);
        snprintf(s_over[s_over_n].val, sizeof s_over[s_over_n].val, "%s", val);
        s_over_n++;
        if (strcmp(key, "log_level") == 0)
            configure_logging();
    }
}

static int cfg_parse_bool(const char *v, int def) {
    if (!v)
        return def;
    while (*v && isspace((unsigned char)*v))
        v++;
    if (!strncmp(v, "true", 4))
        return 1;
    if (!strncmp(v, "false", 5))
        return 0;
    return (int)strtol(v, NULL, 0) != 0;
}

int pc_cfg_int(const char *key, int def) {
    const char *v;
    v = cfg_env(key);
    if (v)
        return (int)strtol(v, NULL, 0);
    v = cfg_session(key);
    if (v)
        return (int)strtol(v, NULL, 0);
    v = find_value(key);
    if (v)
        return (int)strtol(v, NULL, 0);
    return def;
}

int pc_cfg_bool(const char *key, int def) {
    const char *v;
    v = cfg_env(key);
    if (v)
        return cfg_parse_bool(v, def);
    v = cfg_session(key);
    if (v)
        return cfg_parse_bool(v, def);
    v = find_value(key);
    if (v)
        return cfg_parse_bool(v, def);
    return def;
}

int pc_cfg_string(const char *key, const char *def, char *out, int cap) {
    const char *source;
    if (pc_cfg_show(key, out, cap, &source))
        return 1;
    if (cap <= 0)
        return 0;
    snprintf(out, (size_t)cap, "%s", def ? def : "");
    return 0;
}

/* Copy a JSON scalar/string token (stops at , } ] or whitespace; unquotes). */
static void cfg_copy_token(const char *v, char *out, int cap) {
    if (cap <= 0)
        return;
    int i = 0;
    if (*v == '"') {
        v++;
        while (*v && *v != '"' && i < cap - 1) {
            if (*v == '\\' && v[1])
                v++;
            out[i++] = *v++;
        }
    } else {
        while (*v && !strchr(",}] \t\r\n", *v) && i < cap - 1)
            out[i++] = *v++;
    }
    out[i] = 0;
}

int pc_cfg_show(const char *key, char *out, int cap, const char **src) {
    const char *v;
    v = cfg_env(key);
    if (v) {
        snprintf(out, cap, "%s", v);
        if (src)
            *src = "env";
        return 1;
    }
    v = cfg_session(key);
    if (v) {
        snprintf(out, cap, "%s", v);
        if (src)
            *src = "repl";
        return 1;
    }
    v = find_value(key);
    if (v) {
        cfg_copy_token(v, out, cap);
        if (src)
            *src = "json";
        return 1;
    }
    if (cap > 0)
        out[0] = 0;
    if (src)
        *src = "default";
    return 0;
}

/* ── Persistence (options menu)
 * ──────────────────────────────────────────────── Rewrite/insert one flat
 * "key": value pair in the config file. The file is a flat JSON object (that's
 * all the loader supports), so text-level editing is the matching writer:
 * replace the existing value token in place, or insert the pair before the
 * closing '}'. Creates the file if missing. Also refreshes the in-memory text +
 * session layer so the change is live this frame. */
void pc_cfg_persist(const char *key, const char *json_val) {
    pc_config_load();
    const char *old = s_cfg ? s_cfg : "{\n}\n";

    size_t cap = strlen(old) + strlen(key) + strlen(json_val) + 16;
    char *out = (char *)malloc(cap);
    if (!out)
        return;

    const char *v = find_value(key); /* points at the old value token */
    if (v) {
        const char *end = v;
        if (*end == '"') {
            end++;
            while (*end && *end != '"') {
                if (*end == '\\' && end[1])
                    end++;
                end++;
            }
            if (*end)
                end++;
        } else {
            while (*end && !strchr(",}] \t\r\n", *end))
                end++;
        }
        size_t pre = (size_t)(v - old);
        snprintf(out, cap, "%.*s%s%s", (int)pre, old, json_val, end);
    } else {
        const char *close = strrchr(old, '}');
        if (close) {
            /* insert right after the last member (comma on its line), before '}' */
            const char *q = close;
            int has_member = 0;
            while (q > old) {
                q--;
                if (!isspace((unsigned char)*q)) {
                    has_member = (*q != '{');
                    break;
                }
            }
            size_t pre = (size_t)(q + 1 - old);
            snprintf(out, cap, "%.*s%s\n  \"%s\": %s\n%s", (int)pre, old, has_member ? "," : "",
                     key, json_val, close);
        } else {
            snprintf(out, cap, "{\n  \"%s\": %s\n}\n", key, json_val);
        }
    }

    const char *path = cfg_path();
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(out, 1, strlen(out), f);
        fclose(f);
    } else {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "config", "cannot write %s", path);
    }

    free(s_cfg);
    s_cfg = out; /* adopt as the live file text */

    /* Session layer too, so an active REPL override doesn't mask the new value
     * (env still wins, by design). Strip quotes for the session token. */
    char tok[64];
    cfg_copy_token(json_val, tok, sizeof tok);
    pc_cfg_set(key, tok);
}

/* ── Per-device modern controls ──────────────────────────────────────────────
 * Resolved LIVE (cheap text scan) so the options menu toggles take effect with
 * no restart. The legacy single "modern_controls" knob is each flag's default.
 */
int pc_modern_kb(void) {
    return pc_cfg_bool("modern_controls_keyboard", pc_cfg_bool("modern_controls", 0));
}
int pc_modern_pad(void) {
    return pc_cfg_bool("modern_controls_controller", pc_cfg_bool("modern_controls", 0));
}
int pc_modern_touch(void) {
    return pc_cfg_bool("modern_controls_touch", pc_cfg_bool("modern_controls", 0));
}
int pc_modern_any(void) { return pc_modern_kb() || pc_modern_pad() || pc_modern_touch(); }

PcRenderMode pc_render_mode(void) {
    char buf[32];
    const char *src;
    if (pc_cfg_show("renderer", buf, sizeof buf, &src) && *buf) {
        if (!strcasecmp(buf, "benren"))
            return PC_RENDER_BENREN;
        if (!strcasecmp(buf, "vanilla"))
            return PC_RENDER_VANILLA;
        /* unknown value: don't silently pick — warn once, fall through to AUTO */
        static int warned = 0;
        if (!warned) {
            warned = 1;
            benefactor_log_write(BENEFACTOR_LOG_WARNING, "render",
                                 "unknown renderer '%s' (use vanilla|benren); using auto", buf);
        }
    }
    return PC_RENDER_AUTO;
}
