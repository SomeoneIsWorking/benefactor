/* pc_http_debug.c — tiny opt-in HTTP/JSON debug server for live inspection.
 *
 * Enabled by setting the env var BENEFACTOR_HTTP=<port> (e.g. 8080) before the
 * game starts; otherwise nothing is created and there is zero overhead. It runs
 * its own detached thread with a blocking accept() loop and serves simple GET
 * requests so a developer can poke at the running game (read/poke memory, grab
 * the framebuffer, read engine state) WHILE someone plays.
 *
 * Endpoints (all GET):
 *   /state                       JSON: level, cop1lc, player block, frame
 *   /mem?addr=HEX&len=N          JSON: {addr, len, hex:"AABB.."}
 *   /poke?addr=HEX&val=HEX       poke one byte into g_mem; JSON {ok,addr,val}
 *   /fb.ppm                      current framebuffer as a binary PPM (P6) image
 *   /fb.bin                      current framebuffer as raw ARGB8888 (352x282)
 *
 * Reads are best-effort/diagnostic: the game's two threads (SDL main + game)
 * alternate, and this server reads g_mem / the framebuffer without locking, so
 * a response may capture a mid-frame view. That is fine for debugging.
 */
#include "pc.h"
#include "game_state.h"
#include "recomp/hw.h"
#include "recomp/rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HTTP_FB_W 352
#define HTTP_FB_H 282

static int s_port = 0;

static unsigned hexval(const char *s)
{
    return (unsigned)strtoul(s, NULL, 16);
}

/* Pull a "key=" value out of a query string into out (decimal or hex per caller). */
static int query_get(const char *q, const char *key, char *out, int outsz)
{
    if (!q) return 0;
    size_t kl = strlen(key);
    const char *p = q;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            int i = 0;
            while (*v && *v != '&' && i < outsz - 1) out[i++] = *v++;
            out[i] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

static void send_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

static void send_response(int fd, const char *status, const char *ctype,
                          const void *body, size_t blen)
{
    char hdr[256];
    int h = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        status, ctype, blen);
    send_all(fd, hdr, (size_t)h);
    if (body && blen) send_all(fd, (const char *)body, blen);
}

static void handle_state(int fd)
{
    extern uint8_t *g_mem;
    uint16_t level   = (uint16_t)((g_mem[0x20] << 8) | g_mem[0x21]);
    uint32_t cop1lc  = (((uint32_t)s_regs[0x080 >> 1] << 16) | s_regs[0x082 >> 1]) & 0xFFFFFFu;
    /* player position+state block at $57FEB8 ($10A6(a5)) — 4 words */
    uint16_t p0 = (uint16_t)((g_mem[0x57FEB8] << 8) | g_mem[0x57FEB9]);
    uint16_t p1 = (uint16_t)((g_mem[0x57FEBA] << 8) | g_mem[0x57FEBB]);
    uint16_t p2 = (uint16_t)((g_mem[0x57FEBC] << 8) | g_mem[0x57FEBD]);
    uint16_t p3 = (uint16_t)((g_mem[0x57FEBE] << 8) | g_mem[0x57FEBF]);
    char body[512];
    int n = snprintf(body, sizeof body,
        "{\"level\":%u,\"cop1lc\":\"%06X\","
        "\"gameplay_active\":%d,\"overlay_active\":%d,\"credits_active\":%d,"
        "\"player_block\":[%u,%u,%u,%u]}\n",
        level, cop1lc, g_gameplay_active, g_overlay_active, g_credits_active,
        p0, p1, p2, p3);
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
}

static void handle_mem(int fd, const char *q)
{
    extern uint8_t *g_mem;
    char a[32] = {0}, l[32] = {0};
    if (!query_get(q, "addr", a, sizeof a)) { send_response(fd, "400 Bad Request", "text/plain", "need addr\n", 10); return; }
    query_get(q, "len", l, sizeof l);
    unsigned addr = hexval(a);
    unsigned len  = l[0] ? (unsigned)strtoul(l, NULL, 10) : 16u;
    if (len > 4096) len = 4096;
    if (addr >= RT_MEM_SIZE) { send_response(fd, "400 Bad Request", "text/plain", "addr OOB\n", 9); return; }
    if (addr + len > RT_MEM_SIZE) len = RT_MEM_SIZE - addr;
    /* JSON {addr,len,hex} */
    char *body = (char *)malloc(len * 2 + 128);
    if (!body) { send_response(fd, "500 Internal", "text/plain", "oom\n", 4); return; }
    int n = snprintf(body, 128, "{\"addr\":\"%06X\",\"len\":%u,\"hex\":\"", addr, len);
    for (unsigned i = 0; i < len; i++)
        n += sprintf(body + n, "%02X", g_mem[addr + i]);
    n += sprintf(body + n, "\"}\n");
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
    free(body);
}

static void handle_poke(int fd, const char *q)
{
    extern uint8_t *g_mem;
    char a[32] = {0}, v[32] = {0};
    if (!query_get(q, "addr", a, sizeof a) || !query_get(q, "val", v, sizeof v)) {
        send_response(fd, "400 Bad Request", "text/plain", "need addr&val\n", 14); return;
    }
    unsigned addr = hexval(a), val = hexval(v) & 0xFF;
    if (addr >= RT_MEM_SIZE) { send_response(fd, "400 Bad Request", "text/plain", "addr OOB\n", 9); return; }
    g_mem[addr] = (uint8_t)val;
    char body[96];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"addr\":\"%06X\",\"val\":\"%02X\"}\n", addr, val);
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
}

/* /input?interact=1&fire=0&u=0&d=0&l=0&r=0 — drive the game over HTTP (held until
 * changed). Lets the debugger move the player, fire, and interact without a window. */
static void handle_input(int fd, const char *q)
{
    extern void hw_set_interact(int), hw_set_fire(int), hw_set_mouse_lmb(int);
    extern void hw_set_joystick(int,int,int,int,int);
    char b[8];
    int interact = query_get(q, "interact", b, sizeof b) ? atoi(b) : 0;
    int fire = query_get(q, "fire", b, sizeof b) ? atoi(b) : 0;
    int u = query_get(q, "u", b, sizeof b) ? atoi(b) : 0;
    int d = query_get(q, "d", b, sizeof b) ? atoi(b) : 0;
    int l = query_get(q, "l", b, sizeof b) ? atoi(b) : 0;
    int r = query_get(q, "r", b, sizeof b) ? atoi(b) : 0;
    hw_set_joystick(u, d, l, r, fire);
    hw_set_mouse_lmb(fire);
    hw_set_interact(interact);
    char body[128];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"interact\":%d,\"fire\":%d,\"u\":%d,\"d\":%d,\"l\":%d,\"r\":%d}\n",
        interact, fire, u, d, l, r);
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
}

/* /pickup?extend=N — live-tune the extra horizontal pickup/interaction reach (px). */
static void handle_pickup(int fd, const char *q)
{
    extern int g_interact_extend;
    char b[8];
    if (query_get(q, "extend", b, sizeof b)) g_interact_extend = atoi(b);
    char body[64];
    int n = snprintf(body, sizeof body, "{\"interact_extend\":%d}\n", g_interact_extend);
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
}

static void handle_fb(int fd, int as_ppm)
{
    const uint32_t *fb = hw_get_framebuffer();
    size_t npx = (size_t)HTTP_FB_W * HTTP_FB_H;
    if (!fb) { send_response(fd, "503 Unavailable", "text/plain", "no fb\n", 6); return; }
    if (as_ppm) {
        char hdr[64];
        int h = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", HTTP_FB_W, HTTP_FB_H);
        unsigned char *rgb = (unsigned char *)malloc(npx * 3);
        if (!rgb) { send_response(fd, "500 Internal", "text/plain", "oom\n", 4); return; }
        for (size_t i = 0; i < npx; i++) {
            uint32_t p = fb[i];
            rgb[i * 3 + 0] = (unsigned char)((p >> 16) & 0xFF);
            rgb[i * 3 + 1] = (unsigned char)((p >> 8) & 0xFF);
            rgb[i * 3 + 2] = (unsigned char)(p & 0xFF);
        }
        /* Body = header + pixels; send header then pixels with one Content-Length. */
        char rhdr[256];
        int rh = snprintf(rhdr, sizeof rhdr,
            "HTTP/1.1 200 OK\r\nContent-Type: image/x-portable-pixmap\r\n"
            "Content-Length: %zu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
            (size_t)h + npx * 3);
        send_all(fd, rhdr, (size_t)rh);
        send_all(fd, hdr, (size_t)h);
        send_all(fd, (const char *)rgb, npx * 3);
        free(rgb);
    } else {
        send_response(fd, "200 OK", "application/octet-stream", fb, npx * 4);
    }
}

static void handle_request(int fd, char *req)
{
    /* First line: "GET /path?query HTTP/1.1" */
    if (strncmp(req, "GET ", 4) != 0) {
        send_response(fd, "405 Method Not Allowed", "text/plain", "GET only\n", 9);
        return;
    }
    char *path = req + 4;
    char *sp = strchr(path, ' ');
    if (sp) *sp = 0;
    char *query = strchr(path, '?');
    if (query) { *query = 0; query++; }

    if      (!strcmp(path, "/state"))   handle_state(fd);
    else if (!strcmp(path, "/mem"))     handle_mem(fd, query);
    else if (!strcmp(path, "/poke"))    handle_poke(fd, query);
    else if (!strcmp(path, "/input"))   handle_input(fd, query);
    else if (!strcmp(path, "/pickup"))  handle_pickup(fd, query);
    else if (!strcmp(path, "/fb.ppm"))  handle_fb(fd, 1);
    else if (!strcmp(path, "/fb.bin"))  handle_fb(fd, 0);
    else if (!strcmp(path, "/")) {
        const char *help =
            "Benefactor debug HTTP. Endpoints:\n"
            "  /state\n  /mem?addr=HEX&len=N\n  /poke?addr=HEX&val=HEX\n"
            "  /fb.ppm\n  /fb.bin\n";
        send_response(fd, "200 OK", "text/plain", help, strlen(help));
    }
    else send_response(fd, "404 Not Found", "text/plain", "no\n", 3);
}

static void *http_thread(void *arg)
{
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("[http] socket"); return NULL; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  /* localhost only */
    addr.sin_port = htons((uint16_t)s_port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "[http] bind port %d failed\n", s_port); close(srv); return NULL;
    }
    if (listen(srv, 4) < 0) { perror("[http] listen"); close(srv); return NULL; }
    fprintf(stderr, "[http] debug server on http://127.0.0.1:%d/\n", s_port);
    for (;;) {
        int c = accept(srv, NULL, NULL);
        if (c < 0) continue;
        char buf[2048];
        ssize_t n = read(c, buf, sizeof buf - 1);
        if (n > 0) { buf[n] = 0; handle_request(c, buf); }
        close(c);
    }
    return NULL;
}

void pc_http_debug_start(void)
{
    const char *e = getenv("BENEFACTOR_HTTP");
    if (!e || !*e) return;
    s_port = atoi(e);
    if (s_port <= 0) return;
    pthread_t t;
    if (pthread_create(&t, NULL, http_thread, NULL) == 0)
        pthread_detach(t);
}
