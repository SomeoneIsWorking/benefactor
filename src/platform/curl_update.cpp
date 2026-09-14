/* The desktop update check: one HTTPS GET for the latest release, on its own
 * thread, through libcurl when this build has one.
 *
 * libcurl is optional at configure time: a build without it still runs, and the
 * check then reports that it cannot run rather than pretending the build is
 * current (update_check.h's rule). */
#include "platform/update_transport.h"

#include "port/update_check.h"

#ifdef BENEFACTOR_HAVE_CURL

#include <curl/curl.h>

#include <lucent/version.h>

#include <cstddef>
#include <string>
#include <thread>

namespace {

constexpr long kTimeoutSeconds = 10;

std::size_t collect(char *data, std::size_t size, std::size_t count, void *userdata) {
    auto *body = static_cast<std::string *>(userdata);
    body->append(data, size * count);
    return size * count;
}

void run_check() {
    std::string body;
    CURL *handle = curl_easy_init();
    if (handle == nullptr) {
        pc_update_report(nullptr, "could not start an HTTP request");
        return;
    }
    curl_easy_setopt(handle, CURLOPT_URL, pc_update_release_url());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, kTimeoutSeconds);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "benefactor-update-check");
    const CURLcode result = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(handle);

    if (result != CURLE_OK) {
        pc_update_report(nullptr, curl_easy_strerror(result));
        return;
    }
    if (status != 200) {
        pc_update_report(nullptr, "the release service refused the request");
        return;
    }
    const auto tag = lucent::version::tag_from_release_json(body);
    if (!tag.has_value()) {
        pc_update_report(nullptr, "no release tag in the response");
        return;
    }
    pc_update_report(tag->c_str(), nullptr);
}

} // namespace

/* Called once per launch, by platform_update_check_start(). Detached on purpose:
 * the check outlives the call site and its only result is handed to
 * pc_update_report(), which the main thread adopts. */
extern "C" void platform_update_check_begin(void) { std::thread(run_check).detach(); }

#else

extern "C" void platform_update_check_begin(void) {
    pc_update_report(nullptr, "this build has no HTTP client");
}

#endif
