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
#include "common/game_state.h"
#include "common/log.h"
#include "engine/hw.h"
#include "port/config.h"
#include "port/frame_accounting.h"
#include "port/guest_trace.h"
#include "port/port.h"
#include "runtime/guest_runtime.h"

#include "port/input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET http_socket_t;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int http_socket_t;
#endif
#include <SDL3/SDL.h>

#define HTTP_FB_W 352
#define HTTP_FB_H 282

static int s_port = 0;

static unsigned hexval(const char *s) { return (unsigned)strtoul(s, NULL, 16); }

/* Pull a "key=" value out of a query string into out (decimal or hex per caller). */
static int query_get(const char *q, const char *key, char *out, int outsz) {
    if (!q)
        return 0;
    size_t kl = strlen(key);
    const char *p = q;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            int i = 0;
            while (*v && *v != '&' && i < outsz - 1)
                out[i++] = *v++;
            out[i] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return 0;
}

static int http_socket_error_code(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static int http_socket_is_invalid(http_socket_t socket) {
#ifdef _WIN32
    return socket == INVALID_SOCKET;
#else
    return socket < 0;
#endif
}

static void http_socket_close(http_socket_t socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

static int http_socket_send(http_socket_t socket, const char *buf, size_t n) {
#ifdef _WIN32
    return send(socket, buf, (int)n, 0);
#else
    return (int)write(socket, buf, n);
#endif
}

static int http_socket_receive(http_socket_t socket, char *buf, size_t n) {
#ifdef _WIN32
    return recv(socket, buf, (int)n, 0);
#else
    return (int)read(socket, buf, n);
#endif
}

static void send_all(http_socket_t fd, const char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        int w = http_socket_send(fd, buf + off, n - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
}

static void send_response(http_socket_t fd, const char *status, const char *ctype, const void *body,
                          size_t blen) {
    char hdr[256];
    int h = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %llu\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n\r\n",
                     status, ctype, (unsigned long long)blen);
    send_all(fd, hdr, (size_t)h);
    if (body && blen)
        send_all(fd, (const char *)body, blen);
}

static void handle_state(http_socket_t fd) {
    uint16_t level = (uint16_t)((g_mem[0x20] << 8) | g_mem[0x21]);
    uint32_t cop1lc = (((uint32_t)s_regs[0x080 >> 1] << 16) | s_regs[0x082 >> 1]) & 0xFFFFFFu;
    /* player position+state block at $57FEB8 ($10A6(a5)) — 4 words */
    uint16_t p0 = (uint16_t)((g_mem[0x57FEB8] << 8) | g_mem[0x57FEB9]);
    uint16_t p1 = (uint16_t)((g_mem[0x57FEBA] << 8) | g_mem[0x57FEBB]);
    uint16_t p2 = (uint16_t)((g_mem[0x57FEBC] << 8) | g_mem[0x57FEBD]);
    uint16_t p3 = (uint16_t)((g_mem[0x57FEBE] << 8) | g_mem[0x57FEBF]);
    const char *why = NULL;
    int saveable = pc_savestate_allowed(&why);
    char body[2048];
    int n = snprintf(
        body, sizeof body,
        "{\"frame\":%d,\"level\":%u,\"cop1lc\":\"%06X\","
        "\"gameplay_active\":%d,\"overlay_active\":%d,\"credits_active\":%d,"
        "\"saveable\":%d,\"save_reason\":\"%s\","
        "\"player_block\":[%u,%u,%u,%u],\"instructions\":%llu,"
        "\"guest_cycles\":%llu,\"blit_cycles\":%llu,\"fps\":%d,"
        "\"audio\":{\"dmacon\":\"%04X\",\"vol\":[%u,%u,%u,%u],\"per\":[%u,%u,%u,%u]},"
        "\"beam\":{\"crossed\":%u,\"taken\":%u,\"declined\":%u,\"off_flow\":%u,"
        "\"pending_blit\":%u,\"blt_reg\":\"%03X\","
        "\"by_flow\":%u,\"by_irq\":%u,\"by_host\":%u},"
        "\"cycles\":{\"flow\":%llu,\"irq3\":%llu,\"irq6\":%llu,\"frame\":%llu,\"base\":%llu,"
        "\"elapsed\":%llu,\"present\":%llu,\"iter\":%llu,\"iter_max\":%llu,"
        "\"flow_max\":%llu,\"irq3_max\":%llu,\"irq6_max\":%llu},"
        "\"present\":{\"calls\":%u,\"reentrant\":%u},"
        "\"irq_calls\":{\"irq3\":%u,\"irq6\":%u},"
        "\"exec\":{\"on_game_thread\":%d,\"owner\":%u},"
        "\"yield\":{\"calls\":%u,\"refused\":%u,\"parks\":%u},\"title_draws\":%u,"
        "\"us\":{\"game\":%u,\"render\":%u,\"compose\":%u,\"present\":%u}}\n",
        hw_get_frame_num(), level, cop1lc, g_gameplay_active, g_overlay_active, g_credits_active,
        saveable, why ? why : "", p0, p1, p2, p3,
        (unsigned long long)rt_get_executed_instructions(),
        (unsigned long long)rt_get_guest_cycles(), (unsigned long long)g_hw_blit_cycles,
        g_hw_perf.fps, s_regs[0x096 >> 1], s_regs[0x0A8 >> 1], s_regs[0x0B8 >> 1],
        s_regs[0x0C8 >> 1], s_regs[0x0D8 >> 1], s_regs[0x0A6 >> 1], s_regs[0x0B6 >> 1],
        s_regs[0x0C6 >> 1], s_regs[0x0D6 >> 1], g_hw_beam_crossed, g_hw_beam_taken,
        g_hw_beam_declined, g_hw_beam_declined_off_flow, g_hw_beam_pending_blit, g_hw_blt_last_reg,
        g_hw_beam_by_flow, g_hw_beam_by_irq, g_hw_beam_by_host,
        (unsigned long long)g_pc_cycles_flow, (unsigned long long)g_pc_cycles_irq3,
        (unsigned long long)g_pc_cycles_irq6, (unsigned long long)g_pc_cycles_frame,
        (unsigned long long)rt_get_cycle_base(), (unsigned long long)rt_get_cycles_elapsed(),
        (unsigned long long)g_pc_cycles_present, (unsigned long long)g_pc_cycles_outside,
        (unsigned long long)g_pc_cycles_iter_max, (unsigned long long)g_pc_cycles_flow_max,
        (unsigned long long)g_pc_cycles_irq3_max, (unsigned long long)g_pc_cycles_irq6_max,
        g_hw_present_calls, g_hw_present_reentrant, g_pc_irq3_calls, g_pc_irq6_calls,
        pc_on_game_thread(), g_pc_guest_owner, g_pc_yield_calls, g_pc_yield_refused,
        g_pc_yield_parks, g_pc_title_draws, g_hw_perf.game_us, g_hw_perf.render_us,
        g_hw_perf.compose_us, g_hw_perf.present_us);
    /* snprintf returns what it WOULD have written; sending that as the length
     * over-reads the buffer and truncates the JSON mid-token. */
    if (n < 0)
        return;
    if ((size_t)n >= sizeof body)
        n = (int)sizeof body - 1;
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
}

static void handle_mem(http_socket_t fd, const char *q) {
    char a[32] = {0}, l[32] = {0};
    if (!query_get(q, "addr", a, sizeof a)) {
        send_response(fd, "400 Bad Request", "text/plain", "need addr\n", 10);
        return;
    }
    query_get(q, "len", l, sizeof l);
    unsigned addr = hexval(a);
    unsigned len = l[0] ? (unsigned)strtoul(l, NULL, 10) : 16u;
    if (len > 4096)
        len = 4096;
    if (addr >= RT_MEM_SIZE) {
        send_response(fd, "400 Bad Request", "text/plain", "addr OOB\n", 9);
        return;
    }
    if (addr + len > RT_MEM_SIZE)
        len = RT_MEM_SIZE - addr;
    /* JSON {addr,len,hex} */
    char *body = (char *)malloc(len * 2 + 128);
    if (!body) {
        send_response(fd, "500 Internal", "text/plain", "oom\n", 4);
        return;
    }
    int n = snprintf(body, 128, "{\"addr\":\"%06X\",\"len\":%u,\"hex\":\"", addr, len);
    for (unsigned i = 0; i < len; i++)
        n += sprintf(body + n, "%02X", g_mem[addr + i]);
    n += sprintf(body + n, "\"}\n");
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
    free(body);
}

static void handle_poke(http_socket_t fd, const char *q) {
    char a[32] = {0}, v[32] = {0};
    if (!query_get(q, "addr", a, sizeof a) || !query_get(q, "val", v, sizeof v)) {
        send_response(fd, "400 Bad Request", "text/plain", "need addr&val\n", 14);
        return;
    }
    unsigned addr = hexval(a), val = hexval(v) & 0xFF;
    if (addr >= RT_MEM_SIZE) {
        send_response(fd, "400 Bad Request", "text/plain", "addr OOB\n", 9);
        return;
    }
    g_mem[addr] = (uint8_t)val;
    char body[96];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"addr\":\"%06X\",\"val\":\"%02X\"}\n", addr,
                     val);
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
}

/* /input?interact=1&fire=0&u=0&d=0&l=0&r=0 — drive the game over HTTP (held until
 * changed). Lets the debugger move the player, fire, and interact without a window. */
static void handle_input(http_socket_t fd, const char *q) {
    char b[8];
    int interact = query_get(q, "interact", b, sizeof b) ? atoi(b) : 0;
    int fire = query_get(q, "fire", b, sizeof b) ? atoi(b) : 0;
    int u = query_get(q, "u", b, sizeof b) ? atoi(b) : 0;
    int d = query_get(q, "d", b, sizeof b) ? atoi(b) : 0;
    int l = query_get(q, "l", b, sizeof b) ? atoi(b) : 0;
    int r = query_get(q, "r", b, sizeof b) ? atoi(b) : 0;
    int drop = query_get(q, "drop", b, sizeof b) ? atoi(b) : 0;
    int hop = query_get(q, "hop", b, sizeof b) ? atoi(b) : 0;
    {
        if (query_get(q, "ffwd", b, sizeof b))
            hw_set_ffwd(atoi(b));
    }
    hw_set_joystick(u, d, l, r, fire);
    hw_set_mouse_lmb(fire);
    hw_set_interact(interact);
    hw_set_drop(drop);
    hw_set_hop(hop);
    char body[192];
    int n = snprintf(body, sizeof body,
                     "{\"ok\":true,\"interact\":%d,\"fire\":%d,\"u\":%d,\"d\":%d,\"l\":%d,\"r\":%d,"
                     "\"drop\":%d,\"hop\":%d}\n",
                     interact, fire, u, d, l, r, drop, hop);
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
}

/* /pickup?extend=N — live-tune the extra horizontal pickup/interaction reach (px). */
static void handle_pickup(http_socket_t fd, const char *q) {
    char b[8];
    if (query_get(q, "extend", b, sizeof b))
        pc_cfg_set("interact_extend", b); /* unified store */
    char body[64];
    int n =
        snprintf(body, sizeof body, "{\"interact_extend\":%d}\n", pc_cfg_int("interact_extend", 0));
    send_response(fd, "200 OK", "application/json", body, (size_t)n);
}

static void handle_fb(http_socket_t fd, int as_ppm) {
    const uint32_t *fb = hw_get_framebuffer();
    size_t npx = (size_t)HTTP_FB_W * HTTP_FB_H;
    if (!fb) {
        send_response(fd, "503 Unavailable", "text/plain", "no fb\n", 6);
        return;
    }
    if (as_ppm) {
        char hdr[64];
        int h = snprintf(hdr, sizeof hdr, "P6\n%d %d\n255\n", HTTP_FB_W, HTTP_FB_H);
        unsigned char *rgb = (unsigned char *)malloc(npx * 3);
        if (!rgb) {
            send_response(fd, "500 Internal", "text/plain", "oom\n", 4);
            return;
        }
        for (size_t i = 0; i < npx; i++) {
            uint32_t p = fb[i];
            rgb[i * 3 + 0] = (unsigned char)((p >> 16) & 0xFF);
            rgb[i * 3 + 1] = (unsigned char)((p >> 8) & 0xFF);
            rgb[i * 3 + 2] = (unsigned char)(p & 0xFF);
        }
        /* Body = header + pixels; send header then pixels with one Content-Length. */
        char rhdr[256];
        int rh = snprintf(
            rhdr, sizeof rhdr,
            "HTTP/1.1 200 OK\r\nContent-Type: image/x-portable-pixmap\r\n"
            "Content-Length: %llu\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
            (unsigned long long)((size_t)h + npx * 3));
        send_all(fd, rhdr, (size_t)rh);
        send_all(fd, hdr, (size_t)h);
        send_all(fd, (const char *)rgb, npx * 3);
        free(rgb);
    } else {
        send_response(fd, "200 OK", "application/octet-stream", fb, npx * 4);
    }
}

static void handle_request(http_socket_t fd, char *req) {
    /* First line: "GET /path?query HTTP/1.1" */
    if (strncmp(req, "GET ", 4) != 0) {
        send_response(fd, "405 Method Not Allowed", "text/plain", "GET only\n", 9);
        return;
    }
    char *path = req + 4;
    char *sp = strchr(path, ' ');
    if (sp)
        *sp = 0;
    char *query = strchr(path, '?');
    if (query) {
        *query = 0;
        query++;
    }

    if (!strcmp(path, "/state"))
        handle_state(fd);
    else if (!strcmp(path, "/mem"))
        handle_mem(fd, query);
    else if (!strcmp(path, "/poke"))
        handle_poke(fd, query);
    else if (!strcmp(path, "/input"))
        handle_input(fd, query);
    else if (!strcmp(path, "/pickup"))
        handle_pickup(fd, query);
    else if (!strcmp(path, "/fb.ppm"))
        handle_fb(fd, 1);
    else if (!strcmp(path, "/fb.bin"))
        handle_fb(fd, 0);
    else if (!strcmp(path, "/save")) {
        g_pc_pending_save = 1;
        send_response(fd, "200 OK", "text/plain", "save queued\n", 12);
    } else if (!strcmp(path, "/load")) {
        g_pc_pending_load = 1;
        send_response(fd, "200 OK", "text/plain", "load queued\n", 12);
    } else if (!strcmp(path, "/gameover")) { /* debug: force a death (drain a life) */
        pc_debug_game_over();
        send_response(fd, "200 OK", "text/plain", "death triggered\n", 16);
    } else if (!strcmp(path, "/trace")) { /* recently retired guest instructions */
        char body[2048];
        size_t n = pc_format_retired_instructions(body, sizeof body);
        send_response(fd, "200 OK", "text/plain", body, n);
    } else if (!strcmp(path, "/recent")) { /* debug: recent rt_call targets (oldest..newest) */
        uint32_t r[48];
        int n = rt_recent_snapshot(r, 48);
        char body[1024];
        int p = 0;
        for (int i = 0; i < n && p < (int)sizeof body - 10; i++)
            p += snprintf(body + p, sizeof body - p, "%06X\n", r[i]);
        send_response(fd, "200 OK", "text/plain", body, (size_t)p);
    } else if (!strcmp(path, "/")) {
        const char *help = "Benefactor debug HTTP. Endpoints:\n"
                           "  /state\n  /mem?addr=HEX&len=N\n  /poke?addr=HEX&val=HEX\n"
                           "  /fb.ppm\n  /fb.bin\n  /trace\n  /recent\n";
        send_response(fd, "200 OK", "text/plain", help, strlen(help));
    } else
        send_response(fd, "404 Not Found", "text/plain", "no\n", 3);
}

static int http_thread(void *arg) {
    (void)arg;
#ifdef _WIN32
    WSADATA winsock;
    int winsock_status = WSAStartup(MAKEWORD(2, 2), &winsock);
    if (winsock_status != 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "http", "Winsock startup failed (%d)",
                             winsock_status);
        return 0;
    }
#endif
    http_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (http_socket_is_invalid(srv)) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "http", "socket creation failed (%d)",
                             http_socket_error_code());
        return 0;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* localhost only */
    addr.sin_port = htons((uint16_t)s_port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) < 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "http", "bind port %d failed (%d)", s_port,
                             http_socket_error_code());
        http_socket_close(srv);
        return 0;
    }
    if (listen(srv, 4) < 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "http", "listen failed on port %d (%d)", s_port,
                             http_socket_error_code());
        http_socket_close(srv);
        return 0;
    }
    benefactor_log_write(BENEFACTOR_LOG_INFO, "http",
                         "[http] debug server on http://127.0.0.1:%d/\n", s_port);
    for (;;) {
        http_socket_t c = accept(srv, NULL, NULL);
        if (http_socket_is_invalid(c))
            continue;
        char buf[2048];
        int n = http_socket_receive(c, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            handle_request(c, buf);
        }
        http_socket_close(c);
    }
    return 0;
}

void pc_http_debug_start(void) {
    s_port = pc_cfg_int("http", 0);
    if (s_port <= 0)
        return;
    SDL_Thread *thread = SDL_CreateThread(http_thread, "benefactor-http", NULL);
    if (thread)
        SDL_DetachThread(thread);
}
