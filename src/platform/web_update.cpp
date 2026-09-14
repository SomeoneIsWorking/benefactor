/* The browser update check: the page performs the request with fetch() and hands
 * the release document's tag back through benefactor_web_update_result(). The
 * page is the only place the browser's own network stack exists, and the release
 * address still comes from the policy so every host asks the same service. */
#include "platform/update_transport.h"

#include "port/update_check.h"

#include <emscripten.h>

/* Called by the page with the tag when the request succeeded, or a short reason
 * when it did not. Either string may be empty. */
extern "C" EMSCRIPTEN_KEEPALIVE void benefactor_web_update_result(const char *tag,
                                                                  const char *error) {
    const bool have_tag = tag != nullptr && tag[0] != '\0';
    const bool have_error = error != nullptr && error[0] != '\0';
    pc_update_report(have_tag ? tag : nullptr, have_error ? error : nullptr);
}

/* The block below becomes JavaScript source. It spells the two-character
 * operator deliberately: a three-character JavaScript operator cannot be written
 * in a C++ source at all — the preprocessor scans it as two tokens and the
 * formatter splits them, so the page would receive `!= =`. */
extern "C" void platform_update_check_begin(void) {
    EM_ASM(
        {
            const url = UTF8ToString($0);
            if (typeof window.benefactorWebCheckRelease != "function") {
                Module.ccall("benefactor_web_update_result", null, [ "string", "string" ],
                             [ null, "this page cannot check for updates" ]);
                return;
            }
            window.benefactorWebCheckRelease(url);
        },
        pc_update_release_url());
}
