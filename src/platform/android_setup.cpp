/* android_setup.cpp — Android entry into the shared setup flow.
 *
 * The screen is drawn in-app by the shared setup-ui host. The file chooser is
 * Android's own SAF document picker (provided by the activity through the
 * shared android-port importer), and the chosen images are validated and
 * published by setup_flow. No browser and no system message box are involved.
 */
#include "platform/android_bridge.h"
#include "platform/selection_report.h"
#include "platform/setup_flow.h"

#include "common/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <lucent/platform_c.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using benefactor::platform::SetupDeliver;

std::string private_storage_root() {
    const char *root = SDL_GetAndroidInternalStoragePath();
    return root != nullptr ? std::string(root) : std::string{};
}

// One pending delivery slot: the Activity's import result arrives on the
// Android UI thread while the setup screen runs on the SDL thread.
std::mutex g_delivery_mutex;
SetupDeliver g_delivery;
bool g_delivery_waiting = false;

void request_selection(SetupDeliver deliver) {
    {
        std::lock_guard lock(g_delivery_mutex);
        g_delivery = std::move(deliver);
        g_delivery_waiting = true;
    }
    JNIEnv *environment = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) {
        std::lock_guard lock(g_delivery_mutex);
        SetupDeliver pending = std::move(g_delivery);
        g_delivery_waiting = false;
        if (pending) {
            pending({});
        }
        return;
    }
    jclass klass = environment->GetObjectClass(activity);
    jmethodID method =
        klass != nullptr ? environment->GetMethodID(klass, "pickBenefactorDisks", "()V") : nullptr;
    std::string error;
    if (klass == nullptr || method == nullptr) {
        error = "the Android activity does not provide a disk picker";
    } else {
        environment->CallVoidMethod(activity, method);
        if (environment->ExceptionCheck()) {
            environment->ExceptionClear();
            error = "Android could not open the file picker";
        }
    }
    if (klass != nullptr) {
        environment->DeleteLocalRef(klass);
    }
    environment->DeleteLocalRef(activity);
    if (!error.empty()) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "android", "%s", error.c_str());
        std::lock_guard lock(g_delivery_mutex);
        SetupDeliver pending = std::move(g_delivery);
        g_delivery_waiting = false;
        if (pending) {
            pending({});
        }
    }
}

/* Releases the picker's staged copy once the setup flow holds its own. The
 * activity owns that directory, so it performs the removal; a build without the
 * method simply keeps the copy. */
void release_staged_import() {
    JNIEnv *environment = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) {
        return;
    }
    jclass klass = environment->GetObjectClass(activity);
    jmethodID method = klass != nullptr
                           ? environment->GetMethodID(klass, "releaseBenefactorStaging", "()V")
                           : nullptr;
    if (klass != nullptr && method != nullptr) {
        environment->CallVoidMethod(activity, method);
        environment->ExceptionClear();
    }
    if (klass != nullptr) {
        environment->DeleteLocalRef(klass);
    }
    environment->DeleteLocalRef(activity);
}

} // namespace

extern "C" int android_bridge_select_disks(const char **disks, size_t capacity) {
    if (disks == nullptr || capacity < 3) {
        return 0;
    }
    const std::string root = private_storage_root();
    if (root.empty() || lucent_platform_set_user_data_directory(root.c_str()) == 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "android",
                             "cannot establish app-private storage");
        return 0;
    }

    const std::filesystem::path store_root = std::filesystem::path(root) / "disks";
    std::array<std::filesystem::path, 3> committed;
    std::string error;
    if (benefactor::platform::committed_disks(store_root, committed, error)) {
        static std::array<std::string, 3> stable;
        for (std::size_t index = 0; index < committed.size(); ++index) {
            stable[index] = committed[index].string();
            disks[index] = stable[index].c_str();
        }
        if (chdir(root.c_str()) != 0) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "android", "cannot enter private storage");
        }
        return 1;
    }

    // Apply the title's window policy before creating the setup window: the
    // screen must come up landscape on a phone, and the game's own window is
    // created later.
    android_bridge_enforce_window_policy();
    auto flow = benefactor::platform::run_setup_flow(
        request_selection, std::filesystem::path(root) / "setup", store_root);
    release_staged_import();
    if (!flow.ok) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "android", "disk setup: %s", flow.error.c_str());
        return 0;
    }
    static std::array<std::string, 3> stable;
    for (std::size_t index = 0; index < flow.disks.size(); ++index) {
        stable[index] = flow.disks[index].string();
        disks[index] = stable[index].c_str();
    }
    if (chdir(root.c_str()) != 0) {
        benefactor_log_write(BENEFACTOR_LOG_ERROR, "android", "cannot enter private storage");
    }
    return 1;
}

/* Called from the Activity while it copies the chosen folder (any thread). */
extern "C" JNIEXPORT void JNICALL
Java_io_github_someoneisworking_benefactor_BenefactorActivity_nativeDiskSelectionProgress(
    JNIEnv * /*environment*/, jclass /*unused*/, jdouble /*fraction*/) {
    /* The screen owns its own progress rendering today; Android's copy is
     * bounded and short enough that the honest report is "working". Retained
     * so the Activity's callback cannot fail to resolve. */
}

extern "C" int android_bridge_enforce_window_policy(void) {
    JNIEnv *environment = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) {
        return 0;
    }
    jclass klass = environment->GetObjectClass(activity);
    jmethodID method = klass != nullptr
                           ? environment->GetMethodID(klass, "enforceBenefactorWindowPolicy", "()V")
                           : nullptr;
    if (klass != nullptr && method != nullptr) {
        environment->CallVoidMethod(activity, method);
        environment->ExceptionClear();
    }
    if (klass != nullptr) {
        environment->DeleteLocalRef(klass);
    }
    environment->DeleteLocalRef(activity);
    return 1;
}

/* Called from the Activity when its SAF import finishes (any thread). */
extern "C" JNIEXPORT void JNICALL
Java_io_github_someoneisworking_benefactor_BenefactorActivity_nativeDiskSelectionResult(
    JNIEnv *environment, jclass /*unused*/, jstring staging_directory, jobjectArray document_names,
    jstring error) {
    std::string directory;
    if (staging_directory != nullptr) {
        const char *value = environment->GetStringUTFChars(staging_directory, nullptr);
        if (value != nullptr) {
            directory = value;
            environment->ReleaseStringUTFChars(staging_directory, value);
        }
    }
    std::vector<std::string> names;
    if (document_names != nullptr) {
        const jsize count = environment->GetArrayLength(document_names);
        for (jsize index = 0; index < count; ++index) {
            auto *entry =
                static_cast<jstring>(environment->GetObjectArrayElement(document_names, index));
            if (entry == nullptr) {
                continue;
            }
            const char *value = environment->GetStringUTFChars(entry, nullptr);
            if (value != nullptr) {
                names.emplace_back(value);
                environment->ReleaseStringUTFChars(entry, value);
            }
            environment->DeleteLocalRef(entry);
        }
    }
    const std::vector<std::filesystem::path> chosen =
        benefactor::platform::selection_report_paths(std::filesystem::path{directory}, names);
    if (error != nullptr) {
        const char *message = environment->GetStringUTFChars(error, nullptr);
        if (message != nullptr) {
            benefactor_log_write(BENEFACTOR_LOG_ERROR, "android", "disk picker: %s", message);
            environment->ReleaseStringUTFChars(error, message);
        }
    }
    std::lock_guard lock(g_delivery_mutex);
    SetupDeliver deliver = std::move(g_delivery);
    g_delivery_waiting = false;
    if (deliver) {
        deliver(chosen);
    }
}
