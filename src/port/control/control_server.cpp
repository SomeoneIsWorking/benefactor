#include "port/control/control_server.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <lucent/http.h>

#include "port/control/input_script.h"
#include "port/debug/debugger.h"

#include "common/log.h"

extern "C" {
#include "common/game_state.h"
#include "engine/hw.h"
#include "port/config.h"
#include "port/frame_accounting.h"
#include "port/guest_trace.h"
#include "port/input.h"
#include "port/overlay_ui.h"
#include "port/port.h"
#include "runtime/guest_runtime.h"
}

namespace benefactor::control {

using benefactor::debug::Debugger;
using benefactor::debug::Stop;
namespace {

constexpr int kFramebufferWidth = 352;
constexpr int kFramebufferHeight = 282;

using lucent::http::Request;
using lucent::http::Response;

/* One query parameter, decoded. Absent and malformed both give `fallback`, so
 * a route never acts on a half-parsed value. */
std::string parameter(const Request &request, std::string_view name) {
    const std::string_view query = request.query();
    std::size_t at = 0;
    while (at < query.size()) {
        const std::size_t end = std::min(query.find('&', at), query.size());
        const std::string_view field = query.substr(at, end - at);
        const std::size_t equals = field.find('=');
        if (equals != std::string_view::npos && field.substr(0, equals) == name)
            return std::string(field.substr(equals + 1));
        at = end + 1;
    }
    return {};
}

int number(const Request &request, std::string_view name, int fallback = 0) {
    const std::string text = parameter(request, name);
    if (text.empty())
        return fallback;
    return static_cast<int>(std::strtol(text.c_str(), nullptr, 10));
}

bool has(const Request &request, std::string_view name) {
    return !parameter(request, name).empty();
}

unsigned hex(const Request &request, std::string_view name) {
    const std::string text = parameter(request, name);
    return static_cast<unsigned>(std::strtoul(text.c_str(), nullptr, 16));
}

Buttons buttons_from(const Request &request) {
    Buttons buttons;
    buttons.up = number(request, "u") != 0;
    buttons.down = number(request, "d") != 0;
    buttons.left = number(request, "l") != 0;
    buttons.right = number(request, "r") != 0;
    buttons.fire = number(request, "fire") != 0;
    buttons.interact = number(request, "interact") != 0;
    buttons.drop = number(request, "drop") != 0;
    buttons.hop = number(request, "hop") != 0;
    return buttons;
}

std::string formatted(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    char text[4096];
    const int written = std::vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    return std::string(text, written < 0 ? 0
                                         : std::min<std::size_t>(static_cast<std::size_t>(written),
                                                                 sizeof text - 1));
}

Response route_state() {
    const std::uint16_t level = static_cast<std::uint16_t>((g_mem[0x20] << 8) | g_mem[0x21]);
    const std::uint32_t cop1lc =
        (((std::uint32_t)s_regs[0x080 >> 1] << 16) | s_regs[0x082 >> 1]) & 0xFFFFFFu;
    /* There was a "player_block" here, four words read from a hardcoded
     * $57FEB8 and labelled as the player's position. It is not: it reported a
     * fixed [1056, 54, 0, 1] while the character walked across the screen,
     * which is worse than reporting nothing, because it reads as evidence that
     * movement is dead. The gameplay code reads that offset from the LIVE a5
     * (platformer.c uses ctx->A[5] + $10A6), and a5 is now in /cpu — so ask
     * /cpu for a5 and /mem for the block, rather than guessing here. */
    const char *why = nullptr;
    const int saveable = pc_savestate_allowed(&why);
    /* One read of the frame's numbers, so every field below describes the same
     * instant rather than drifting while the reply is formatted. */
    PcFrameAccounting frame;
    pc_frame_accounting(&frame);

    return Response::json(
        200, "OK",
        formatted(
            "{\"frame\":%d,\"level\":%u,\"cop1lc\":\"%06X\","
            "\"gameplay_active\":%d,\"overlay_active\":%d,\"credits_active\":%d,"
            "\"saveable\":%d,\"save_reason\":\"%s\",\"paused\":%d,\"press_left\":%d,"
            "\"instructions\":%llu,"
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
            hw_get_frame_num(), level, cop1lc, g_gameplay_active, g_overlay_active,
            g_credits_active, saveable, why ? why : "", InputScript::instance().paused() ? 1 : 0,
            InputScript::instance().press_frames_left(),
            (unsigned long long)rt_get_executed_instructions(),
            (unsigned long long)rt_get_guest_cycles(), (unsigned long long)g_hw_blit_cycles,
            g_hw_perf.fps, s_regs[0x096 >> 1], s_regs[0x0A8 >> 1], s_regs[0x0B8 >> 1],
            s_regs[0x0C8 >> 1], s_regs[0x0D8 >> 1], s_regs[0x0A6 >> 1], s_regs[0x0B6 >> 1],
            s_regs[0x0C6 >> 1], s_regs[0x0D6 >> 1], g_hw_beam_crossed, g_hw_beam_taken,
            g_hw_beam_declined, g_hw_beam_declined_off_flow, g_hw_beam_pending_blit,
            g_hw_blt_last_reg, g_hw_beam_by_flow, g_hw_beam_by_irq, g_hw_beam_by_host,
            (unsigned long long)frame.flow, (unsigned long long)frame.irq3,
            (unsigned long long)frame.irq6, (unsigned long long)frame.frame,
            (unsigned long long)rt_get_cycle_base(), (unsigned long long)rt_get_cycles_elapsed(),
            (unsigned long long)frame.present, (unsigned long long)frame.iteration,
            (unsigned long long)frame.iteration_peak, (unsigned long long)frame.flow_peak,
            (unsigned long long)frame.irq3_peak, (unsigned long long)frame.irq6_peak,
            g_hw_present_calls, g_hw_present_reentrant, frame.irq3_deliveries,
            frame.irq6_deliveries, pc_on_game_thread(), frame.running_owner, frame.waits_reached,
            frame.waits_refused, frame.waits_parked, frame.title_draws, g_hw_perf.game_us,
            g_hw_perf.render_us, g_hw_perf.compose_us, g_hw_perf.present_us));
}

Response route_cpu() {
    std::uint32_t data[8] = {0}, address[8] = {0}, program_counter = 0;
    std::uint16_t status = 0;
    rt_cpu_registers(data, address, &program_counter, &status);

    std::string body =
        formatted("{\"pc\":\"%06X\",\"sr\":\"%04X\",\"d\":[", program_counter, status);
    for (int index = 0; index < 8; index++)
        body += formatted("%s\"%08X\"", index ? "," : "", data[index]);
    body += "],\"a\":[";
    for (int index = 0; index < 8; index++)
        body += formatted("%s\"%08X\"", index ? "," : "", address[index]);
    body += "]}\n";
    return Response::json(200, "OK", body);
}

Response route_memory(const Request &request) {
    if (!has(request, "addr"))
        return Response::text(400, "Bad Request", "need addr\n");
    unsigned addr = hex(request, "addr");
    unsigned length = has(request, "len") ? (unsigned)number(request, "len") : 16u;
    if (length > 4096)
        length = 4096;
    if (addr >= RT_MEM_SIZE)
        return Response::text(400, "Bad Request", "addr OOB\n");
    if (addr + length > RT_MEM_SIZE)
        length = RT_MEM_SIZE - addr;

    std::string body = formatted("{\"addr\":\"%06X\",\"len\":%u,\"hex\":\"", addr, length);
    body.reserve(body.size() + length * 2 + 8);
    for (unsigned index = 0; index < length; index++)
        body += formatted("%02X", g_mem[addr + index]);
    body += "\"}\n";
    return Response::json(200, "OK", body);
}

Response route_poke(const Request &request) {
    if (!has(request, "addr") || !has(request, "val"))
        return Response::text(400, "Bad Request", "need addr&val\n");
    const unsigned addr = hex(request, "addr");
    const unsigned value = hex(request, "val") & 0xFFu;
    if (addr >= RT_MEM_SIZE)
        return Response::text(400, "Bad Request", "addr OOB\n");
    g_mem[addr] = static_cast<std::uint8_t>(value);
    return Response::json(
        200, "OK", formatted("{\"ok\":true,\"addr\":\"%06X\",\"val\":\"%02X\"}\n", addr, value));
}

Response route_hold(const Request &request) {
    if (has(request, "ffwd"))
        hw_set_ffwd(number(request, "ffwd"));
    const Buttons buttons = buttons_from(request);
    InputScript::instance().hold(buttons);
    return Response::json(200, "OK",
                          formatted("{\"ok\":true,\"held\":{\"u\":%d,\"d\":%d,\"l\":%d,\"r\":%d,"
                                    "\"fire\":%d,\"interact\":%d,\"drop\":%d,\"hop\":%d}}\n",
                                    buttons.up, buttons.down, buttons.left, buttons.right,
                                    buttons.fire, buttons.interact, buttons.drop, buttons.hop));
}

Response route_press(const Request &request) {
    const int frames = number(request, "frames", 2);
    const Buttons buttons = buttons_from(request);
    if (!buttons.any())
        return Response::text(400, "Bad Request", "need at least one button\n");
    InputScript::instance().press(buttons, frames);
    return Response::json(200, "OK", formatted("{\"ok\":true,\"frames\":%d}\n", frames));
}

Response route_break(const Request &request) {
    Debugger &debugger = Debugger::instance();
    if (has(request, "clear")) {
        if (parameter(request, "clear") == "all") {
            debugger.clear_all();
            return Response::json(200, "OK", "{\"ok\":true,\"cleared\":\"all\"}\n");
        }
        const unsigned address = hex(request, "clear");
        const bool cleared = debugger.clear(address);
        return Response::json(
            cleared ? 200 : 404, cleared ? "OK" : "Not Found",
            formatted("{\"ok\":%s,\"cleared\":\"%06X\"}\n", cleared ? "true" : "false", address));
    }
    if (!has(request, "at"))
        return Response::text(400, "Bad Request",
                              "need at=<hex address>, or clear=<hex address>, or clear=all\n");
    const unsigned address = hex(request, "at");
    const bool added = debugger.set(address);
    /* Refused means already set or the set is full — say which, so a script
     * does not have to guess from a bare false. */
    return Response::json(added ? 200 : 409, added ? "OK" : "Conflict",
                          formatted("{\"ok\":%s,\"at\":\"%06X\",\"set\":%d,\"capacity\":%d}\n",
                                    added ? "true" : "false", address,
                                    (int)debugger.addresses().size(), debugger.capacity()));
}

Response route_breaks() {
    Debugger &debugger = Debugger::instance();
    std::string list;
    for (const std::uint32_t address : debugger.addresses()) {
        if (!list.empty())
            list += ',';
        list += formatted("\"%06X\"", address);
    }
    const Stop stop = debugger.last_stop();
    std::string stopped = "null";
    if (stop.valid)
        stopped = formatted("{\"at\":\"%06X\",\"pc\":\"%06X\",\"frame\":%d,"
                            "\"guest_cycles\":%llu}",
                            stop.address, stop.program_counter, stop.frame,
                            (unsigned long long)stop.guest_cycles);
    return Response::json(200, "OK",
                          formatted("{\"breakpoints\":[%s],\"capacity\":%d,\"paused\":%d,"
                                    "\"stopped\":%s}\n",
                                    list.c_str(), debugger.capacity(),
                                    InputScript::instance().paused() ? 1 : 0, stopped.c_str()));
}

Response route_step(const Request &request) {
    const int frames = number(request, "frames", 1);
    InputScript::instance().step(frames);
    return Response::json(200, "OK", formatted("{\"ok\":true,\"stepping\":%d}\n", frames));
}

Response route_framebuffer(bool as_ppm) {
    const std::uint32_t *framebuffer = hw_get_framebuffer();
    if (framebuffer == nullptr)
        return Response::text(503, "Unavailable", "no fb\n");
    const std::size_t pixels = (std::size_t)kFramebufferWidth * kFramebufferHeight;

    if (!as_ppm)
        return Response::binary(200, "OK", "application/octet-stream",
                                std::string(reinterpret_cast<const char *>(framebuffer),
                                            pixels * sizeof(std::uint32_t)));

    std::string body = formatted("P6\n%d %d\n255\n", kFramebufferWidth, kFramebufferHeight);
    body.reserve(body.size() + pixels * 3);
    for (std::size_t index = 0; index < pixels; index++) {
        const std::uint32_t pixel = framebuffer[index];
        body.push_back(static_cast<char>((pixel >> 16) & 0xFF));
        body.push_back(static_cast<char>((pixel >> 8) & 0xFF));
        body.push_back(static_cast<char>(pixel & 0xFF));
    }
    return Response::binary(200, "OK", "image/x-portable-pixmap", body);
}

/* The page a person plays in: the live screen, the buttons, and the frame
 * counter. It only calls the routes above, so anything it can do a script can
 * do too. */
const char *const kPlayPage = R"PAGE(<!doctype html>
<title>Benefactor control</title>
<style>
 body{background:#111;color:#ddd;font:14px system-ui;margin:0;padding:16px}
 img{image-rendering:pixelated;width:704px;max-width:100%;background:#000}
 button{font:13px system-ui;padding:6px 10px;margin:2px;background:#222;color:#ddd;
        border:1px solid #444;border-radius:4px}
 button:active{background:#3a3a3a}
 #row{display:flex;gap:24px;align-items:flex-start;flex-wrap:wrap}
 pre{color:#8c8;font:12px ui-monospace}
</style>
<div id=row>
 <div><img id=screen alt="game screen"><div id=status></div></div>
 <div>
  <div><button data-hold="u=1">up</button></div>
  <div><button data-hold="l=1">left</button><button data-hold="">centre</button>
       <button data-hold="r=1">right</button></div>
  <div><button data-hold="d=1">down</button></div>
  <hr>
  <div><button data-press="fire=1">fire</button>
       <button data-press="interact=1">interact</button>
       <button data-press="hop=1">hop</button>
       <button data-press="drop=1">drop</button></div>
  <hr>
  <div><button data-go="/pause">pause</button><button data-go="/resume">resume</button>
       <button data-go="/step?frames=1">step 1</button>
       <button data-go="/step?frames=25">step 25</button></div>
  <div><button data-go="/save">save</button><button data-go="/load">load</button></div>
  <pre id=cpu></pre>
 </div>
</div>
<script>
const go = u => fetch(u).then(r => r.text());
document.querySelectorAll('[data-hold]').forEach(b =>
  b.onclick = () => go('/hold?' + b.dataset.hold));
document.querySelectorAll('[data-press]').forEach(b =>
  b.onclick = () => go('/press?frames=3&' + b.dataset.press));
document.querySelectorAll('[data-go]').forEach(b =>
  b.onclick = () => go(b.dataset.go));
const keys = {ArrowUp:'u=1',ArrowDown:'d=1',ArrowLeft:'l=1',ArrowRight:'r=1'};
onkeydown = e => { if (keys[e.key]) { go('/hold?' + keys[e.key]); e.preventDefault(); }
                   else if (e.key === ' ') { go('/press?frames=3&fire=1'); e.preventDefault(); } };
onkeyup = e => { if (keys[e.key]) go('/hold?'); };
async function tick() {
  try {
    const s = await (await fetch('/state')).json();
    status.textContent = `frame ${s.frame} · ${s.fps} fps · ${s.cop1lc}` +
                         (s.paused ? ' · HELD' : '');
    const c = await (await fetch('/cpu')).json();
    cpu.textContent = 'pc ' + c.pc + '  a: ' + c.a.join(' ');
    screen.src = '/fb.ppm?' + Date.now();
  } catch (e) { status.textContent = 'not responding'; }
  setTimeout(tick, 200);
}
tick();
</script>
)PAGE";

Response dispatch(const Request &request) {
    const std::string_view path = request.path();
    if (path == "/state")
        return route_state();
    if (path == "/cpu")
        return route_cpu();
    if (path == "/mem")
        return route_memory(request);
    if (path == "/poke")
        return route_poke(request);
    if (path == "/hold" || path == "/input") /* /input kept: existing scripts use it */
        return route_hold(request);
    if (path == "/press")
        return route_press(request);
    if (path == "/step")
        return route_step(request);
    if (path == "/break")
        return route_break(request);
    if (path == "/breaks")
        return route_breaks();
    /* The game's own pause menu, which only a key opens otherwise. Here so a
     * headless run can enter it and be measured: the paused loop has its own
     * clock, and "is the music still at tempo in there" is a question with an
     * answer (/state irq_calls.irq6 against wall time). */
    if (path == "/menu") {
        pc_pause_toggle();
        return Response::json(200, "OK", formatted("{\"menu\":%d}\n", pc_pause_active() ? 1 : 0));
    }
    if (path == "/pause") {
        InputScript::instance().pause();
        return Response::json(200, "OK", "{\"paused\":true}\n");
    }
    if (path == "/resume") {
        InputScript::instance().resume();
        return Response::json(200, "OK", "{\"paused\":false}\n");
    }
    if (path == "/fb.ppm")
        return route_framebuffer(true);
    if (path == "/fb.bin")
        return route_framebuffer(false);
    if (path == "/pickup") {
        if (has(request, "extend"))
            pc_cfg_set("interact_extend", parameter(request, "extend").c_str());
        return Response::json(
            200, "OK", formatted("{\"interact_extend\":%d}\n", pc_cfg_int("interact_extend", 0)));
    }
    if (path == "/save") {
        g_pc_pending_save = 1;
        return Response::text(200, "OK", "save queued\n");
    }
    if (path == "/load") {
        g_pc_pending_load = 1;
        return Response::text(200, "OK", "load queued\n");
    }
    if (path == "/gameover") {
        pc_debug_game_over();
        return Response::text(200, "OK", "death triggered\n");
    }
    if (path == "/trace") {
        /* While the game is held at a breakpoint, serve the trace FROZEN at
         * that stop. The live ring keeps filling from the interrupts that are
         * still being delivered during the hold, so by the time anyone asks it
         * describes the music driver rather than how the guest got to the
         * breakpoint. Measured: stopping at $00345A returned a ring with no
         * trace of $00345A left in it. */
        char text[4096];
        const auto &debugger = debug::Debugger::instance();
        if (debugger.last_stop().valid) {
            const std::size_t frozen = debugger.format_stop_trace(text, sizeof text);
            if (frozen > 0)
                return Response::text(200, "OK", std::string(text, frozen) + "\n");
        }
        const std::size_t written = pc_format_retired_instructions(text, sizeof text);
        return Response::text(200, "OK", std::string(text, written));
    }
    if (path == "/recent") {
        std::uint32_t targets[48];
        const int found = rt_recent_snapshot(targets, 48);
        std::string body;
        for (int index = 0; index < found; index++)
            body += formatted("%06X\n", targets[index]);
        return Response::text(200, "OK", body);
    }
    if (path == "/")
        return Response{200, "OK", "text/html; charset=utf-8", kPlayPage, {}};
    return Response::text(404, "Not Found", "no such route\n");
}

std::unique_ptr<lucent::http::Server> g_server;

} // namespace
} // namespace benefactor::control

extern "C" void pc_control_server_start(void) {
    const int port = pc_cfg_int("http", 0);
    if (port <= 0 || benefactor::control::g_server != nullptr)
        return;
    benefactor::control::g_server = std::make_unique<lucent::http::Server>(
        lucent::http::ServerOptions{.port = static_cast<std::uint16_t>(port)},
        benefactor::control::dispatch);
    if (!benefactor::control::g_server->start()) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "control", "cannot listen on port %d", port);
        benefactor::control::g_server.reset();
        return;
    }
    benefactor_log_write(BENEFACTOR_LOG_INFO, "control", "control channel on http://127.0.0.1:%d/",
                         port);
}
