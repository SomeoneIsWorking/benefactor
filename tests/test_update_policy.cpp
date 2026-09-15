// The update check as state: what a fetch result means, and that a result
// arriving on another thread is adopted only by the thread that reads it.

#include "port/update_check.h"

#include <iostream>
#include <string>
#include <string_view>
#include <thread>

// The policy reads the build's own version; this test is not linked with the
// product's version.c, so it states the version it is testing against.
extern "C" const char *pc_version(void) {
    return "0.3.0";
}

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (condition) {
        return;
    }
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

bool line_contains(std::string_view needle) {
    return std::string(pc_update_line()).find(needle) != std::string::npos;
}

void test_idle_reports_nothing() {
    pc_update_reset();
    check(pc_update_state() == PC_UPDATE_IDLE, "reset is idle");
    check(std::string(pc_update_line()).empty(), "idle has no line");
    check(std::string(pc_update_latest()).empty(), "idle has no tag");
}

void test_newer_release_is_available() {
    pc_update_reset();
    pc_update_begin_checking();
    check(pc_update_state() == PC_UPDATE_CHECKING, "checking state");
    check(line_contains("CHECKING"), "checking line");
    pc_update_report("v9.9.9", nullptr);
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_AVAILABLE, "newer release available");
    check(std::string(pc_update_latest()) == "v9.9.9", "latest tag");
    check(line_contains("AVAILABLE"), "available line names the state");
    check(line_contains("v9.9.9"), "available line names the release");
}

void test_same_or_older_release_is_current() {
    pc_update_report("v0.3.0", nullptr);
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_CURRENT, "same version is current");
    check(line_contains("UP TO DATE"), "current line");

    pc_update_report("v0.2.0", nullptr);
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_CURRENT, "older release is current");
}

void test_failures_never_claim_current() {
    pc_update_report(nullptr, "the network is unreachable");
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_FAILED, "error is a failure");
    check(std::string(pc_update_detail()) == "the network is unreachable", "detail is the reason");
    check(line_contains("FAILED"), "failed line");
    check(!line_contains("UP TO DATE"), "a failure is not up to date");

    // A tag that is not a version is a failure, not "current": the check did not
    // learn anything about releases.
    pc_update_report("nightly", nullptr);
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_FAILED, "unparsable tag is a failure");
    check(!line_contains("UP TO DATE"), "unparsable tag is not up to date");

    pc_update_report(nullptr, nullptr);
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_FAILED, "no reason still fails");
    check(line_contains("UPDATE CHECK FAILED"), "failure line");
}

void test_result_from_another_thread() {
    pc_update_reset();
    pc_update_begin_checking();
    std::thread reporter([] {
        pc_update_report("v1.0.0", nullptr);
    });
    reporter.join();
    // Nothing is visible until the main thread adopts it.
    check(pc_update_state() == PC_UPDATE_CHECKING, "a report is not visible before its tick");
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_AVAILABLE, "adopted from the fetch thread");
    // Adopting twice changes nothing twice.
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_AVAILABLE, "tick is idempotent");
}

void test_begin_clears_a_previous_answer() {
    pc_update_report("v9.9.9", nullptr);
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_AVAILABLE, "available before the next check");
    // A repeated check must not keep showing the previous answer while it runs.
    pc_update_report("v9.9.9", nullptr);
    pc_update_begin_checking();
    check(pc_update_state() == PC_UPDATE_CHECKING, "restart clears the answer");
    pc_update_tick();
    check(pc_update_state() == PC_UPDATE_CHECKING, "the stale report is dropped");
}

} // namespace

int main() {
    test_idle_reports_nothing();
    test_newer_release_is_available();
    test_same_or_older_release_is_current();
    test_failures_never_claim_current();
    test_result_from_another_thread();
    test_begin_clears_a_previous_answer();
    if (failures != 0) {
        std::cerr << failures << " update-policy check(s) failed\n";
        return 1;
    }
    std::cout << "update-policy tests passed\n";
    return 0;
}
