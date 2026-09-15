/* The Windows update check: one HTTPS GET through WinHTTP, the platform's own
 * client. Using it rather than libcurl keeps the Windows package one executable
 * with no runtime library to bundle, and it inherits the system trust store for
 * free — the same reasoning that puts the check on Java for Android. */
#include "platform/update_transport.h"

#include "port/update_check.h"

#include <windows.h>

#include <winhttp.h>

#include <lucent/version.h>

#include <cstddef>
#include <string>
#include <thread>

namespace {

constexpr int kTimeoutMilliseconds = 10000;
constexpr INTERNET_PORT kDefaultPort = 443;

/* WinHTTP takes the host and the path separately, so both come from the policy
 * rather than from a URL this host would have to parse. */
std::wstring widen(const char *text) {
    std::wstring wide;
    for (const char *at = text; at != nullptr && *at != '\0'; ++at) {
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*at)));
    }
    return wide;
}

/* A WinHTTP handle that cannot leak: this runs on a detached thread, so an early
 * return must still close what it opened. */
class Internet {
  public:
    explicit Internet(HINTERNET handle) : handle_(handle) {
    }
    ~Internet() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }
    Internet(const Internet &) = delete;
    Internet &operator=(const Internet &) = delete;
    [[nodiscard]] HINTERNET get() const {
        return handle_;
    }
    [[nodiscard]] bool valid() const {
        return handle_ != nullptr;
    }

  private:
    HINTERNET handle_;
};

std::string read_body(HINTERNET request) {
    std::string body;
    char buffer[4096];
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) != FALSE && available > 0) {
        const DWORD wanted = available < sizeof buffer ? available : sizeof buffer;
        DWORD received = 0;
        if (WinHttpReadData(request, buffer, wanted, &received) == FALSE || received == 0) {
            break;
        }
        body.append(buffer, received);
    }
    return body;
}

void run_check() {
    const std::wstring host = widen(pc_update_release_host());
    const std::wstring object = widen(pc_update_release_path());
    const Internet session(WinHttpOpen(L"benefactor-update-check",
                                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.valid()) {
        pc_update_report(nullptr, "could not open an HTTP session");
        return;
    }
    WinHttpSetTimeouts(session.get(), kTimeoutMilliseconds, kTimeoutMilliseconds,
                       kTimeoutMilliseconds, kTimeoutMilliseconds);
    const Internet connection(WinHttpConnect(session.get(), host.c_str(), kDefaultPort, 0));
    if (!connection.valid()) {
        pc_update_report(nullptr, "could not reach the release service");
        return;
    }
    /* The release address is HTTPS; without this flag the request would be
     * plaintext and the service would refuse it. */
    const Internet request(WinHttpOpenRequest(connection.get(), L"GET", object.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
    if (!request.valid()) {
        pc_update_report(nullptr, "could not build the request");
        return;
    }
    if (WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                           0, 0, 0) == FALSE ||
        WinHttpReceiveResponse(request.get(), nullptr) == FALSE) {
        pc_update_report(nullptr, "the update check could not reach the network");
        return;
    }
    DWORD status = 0;
    DWORD size = sizeof status;
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX) == FALSE) {
        pc_update_report(nullptr, "the release service did not answer");
        return;
    }
    if (status != 200) {
        pc_update_report(nullptr, "the release service refused the request");
        return;
    }
    const std::string body = read_body(request.get());
    const auto tag = lucent::version::tag_from_release_json(body);
    if (!tag.has_value()) {
        pc_update_report(nullptr, "no release tag in the response");
        return;
    }
    pc_update_report(tag->c_str(), nullptr);
}

} // namespace

/* Called once per launch, by platform_update_check_start(). */
extern "C" void platform_update_check_begin(void) {
    std::thread(run_check).detach();
}
