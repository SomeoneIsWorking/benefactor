#include "port/update_check.h"

#include "common/version.h"

#include <lucent/version.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

constexpr std::size_t kTagCapacity = 32;
constexpr std::size_t kDetailCapacity = 96;
constexpr std::size_t kLineCapacity = 160;

/* Main-thread state: the UI reads only this. */
PcUpdateState s_state = PC_UPDATE_IDLE;
char s_latest[kTagCapacity] = "";
char s_detail[kDetailCapacity] = "";
char s_line[kLineCapacity] = "";

/* The hand-off from a fetch thread: one pending result, adopted by the main
 * thread in pc_update_tick(). Storing a result is all a fetch thread does, so no
 * reader ever sees a half-written line. */
std::mutex s_pending_mutex;
bool s_pending = false;
char s_pending_tag[kTagCapacity] = "";
char s_pending_detail[kDetailCapacity] = "";

void copy(char *destination, std::size_t capacity, const char *text) {
    std::snprintf(destination, capacity, "%s", text != nullptr ? text : "");
}

void fail(const char *reason) {
    s_state = PC_UPDATE_FAILED;
    copy(s_detail, sizeof s_detail, reason);
    std::snprintf(s_line, sizeof s_line, "UPDATE CHECK FAILED: %s", s_detail);
}

void adopt(bool have_tag, const char *tag, const char *detail) {
    if (!have_tag) {
        fail(detail != nullptr && detail[0] != '\0' ? detail : "no release information");
        return;
    }
    if (!lucent::version::parse(tag).has_value()) {
        /* A tag that is not a version is not a newer release: better to say the
         * check failed than to claim this build is current. */
        fail("the latest release tag is not a version");
        return;
    }
    copy(s_latest, sizeof s_latest, tag);
    if (lucent::version::is_newer(tag, pc_version())) {
        s_state = PC_UPDATE_AVAILABLE;
        s_detail[0] = '\0';
        std::snprintf(s_line, sizeof s_line, "UPDATE %s AVAILABLE", tag);
        return;
    }
    s_state = PC_UPDATE_CURRENT;
    s_detail[0] = '\0';
    std::snprintf(s_line, sizeof s_line, "UP TO DATE (v%s)", pc_version());
}

} // namespace

extern "C" void pc_update_reset(void) {
    const std::lock_guard<std::mutex> guard(s_pending_mutex);
    /* Main-thread state only; the pending slot is the field a fetch thread
     * writes, and the lock is what keeps that hand-off whole. */
    s_pending = false;
    s_state = PC_UPDATE_IDLE;
    s_latest[0] = '\0';
    s_detail[0] = '\0';
    s_line[0] = '\0';
}

extern "C" void pc_update_begin_checking(void) {
    {
        const std::lock_guard<std::mutex> guard(s_pending_mutex);
        s_pending = false;
    }
    s_state = PC_UPDATE_CHECKING;
    s_latest[0] = '\0';
    s_detail[0] = '\0';
    copy(s_line, sizeof s_line, "CHECKING FOR UPDATES...");
}

extern "C" void pc_update_report(const char *tag, const char *error) {
    const std::lock_guard<std::mutex> guard(s_pending_mutex);
    s_pending = true;
    copy(s_pending_tag, sizeof s_pending_tag, tag);
    copy(s_pending_detail, sizeof s_pending_detail, error);
}

extern "C" void pc_update_tick(void) {
    bool have_tag = false;
    char tag[kTagCapacity];
    char detail[kDetailCapacity];
    {
        const std::lock_guard<std::mutex> guard(s_pending_mutex);
        if (!s_pending) {
            return;
        }
        have_tag = s_pending_tag[0] != '\0';
        copy(tag, sizeof tag, s_pending_tag);
        copy(detail, sizeof detail, s_pending_detail);
        s_pending = false;
    }
    adopt(have_tag, tag, detail);
}

extern "C" PcUpdateState pc_update_state(void) {
    return s_state;
}

namespace {

/* Public release metadata for this project: no authentication, no variables. */
constexpr const char *kReleaseHost = "api.github.com";
constexpr const char *kReleasePath = "/repos/SomeoneIsWorking/benefactor/releases/latest";
char s_release_url[128];

} // namespace

extern "C" const char *pc_update_release_host(void) {
    return kReleaseHost;
}

extern "C" const char *pc_update_release_path(void) {
    return kReleasePath;
}

extern "C" const char *pc_update_release_url(void) {
    /* Composed from the same parts every other host reads, once. */
    if (s_release_url[0] == '\0') {
        std::snprintf(s_release_url, sizeof s_release_url, "https://%s%s", kReleaseHost,
                      kReleasePath);
    }
    return s_release_url;
}

extern "C" const char *pc_update_latest(void) {
    return s_latest;
}

extern "C" const char *pc_update_detail(void) {
    return s_detail;
}

extern "C" const char *pc_update_line(void) {
    return s_line;
}
