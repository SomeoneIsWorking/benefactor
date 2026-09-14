/* The Android update check: the activity performs the request with the
 * platform's own HTTP client (Java's HttpURLConnection, so the system trust
 * store applies) and reports the tag back through JNI. The native side never
 * links a TLS stack. */
#include "platform/update_transport.h"

#include "common/log.h"
#include "port/update_check.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <string>

namespace {

constexpr const char *kRequestMethod = "checkForBenefactorUpdates";

std::string from_java(JNIEnv *environment, jstring text) {
    std::string value;
    if (text == nullptr) {
        return value;
    }
    const char *characters = environment->GetStringUTFChars(text, nullptr);
    if (characters != nullptr) {
        value = characters;
        environment->ReleaseStringUTFChars(text, characters);
    }
    return value;
}

} // namespace

extern "C" void platform_update_check_begin(void) {
    JNIEnv *environment = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (environment == nullptr || activity == nullptr) {
        pc_update_report(nullptr, "Android did not provide the application activity");
        return;
    }
    jclass klass = environment->GetObjectClass(activity);
    jmethodID method =
        klass != nullptr ? environment->GetMethodID(klass, kRequestMethod, "(Ljava/lang/String;)V")
                         : nullptr;
    jstring url = method != nullptr ? environment->NewStringUTF(pc_update_release_url()) : nullptr;
    if (klass == nullptr || method == nullptr || url == nullptr) {
        if (url != nullptr) {
            environment->DeleteLocalRef(url);
        }
        if (klass != nullptr) {
            environment->DeleteLocalRef(klass);
        }
        environment->DeleteLocalRef(activity);
        pc_update_report(nullptr, "the Android activity does not provide an update check");
        return;
    }
    environment->CallVoidMethod(activity, method, url);
    environment->DeleteLocalRef(url);
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        pc_update_report(nullptr, "Android could not start the update check");
    }
    environment->DeleteLocalRef(klass);
    environment->DeleteLocalRef(activity);
}

/* Called by the activity when its request finishes (any thread). `tag` is the
 * release tag when the request succeeded, `error` a short reason otherwise. */
extern "C" JNIEXPORT void JNICALL
Java_io_github_someoneisworking_benefactor_BenefactorActivity_nativeUpdateResult(
    JNIEnv *environment, jclass /*unused*/, jstring tag, jstring error) {
    const std::string tag_text = from_java(environment, tag);
    const std::string error_text = from_java(environment, error);
    pc_update_report(tag_text.empty() ? nullptr : tag_text.c_str(),
                     error_text.empty() ? nullptr : error_text.c_str());
    benefactor_log_write(BENEFACTOR_LOG_INFO, "update", "check finished: %s%s",
                         tag_text.empty() ? "failed: " : tag_text.c_str(),
                         tag_text.empty() ? error_text.c_str() : "");
}
