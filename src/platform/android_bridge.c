#include "platform/android_bridge.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <jni.h>
#include <lucent/platform_c.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { ANDROID_DISK_COUNT = 3, ANDROID_PATH_CAPACITY = 4096 };

typedef struct AndroidImportState {
  SDL_mutex *mutex;
  SDL_cond *condition;
  int waiting;
  int complete;
  char staging[ANDROID_PATH_CAPACITY];
  char error[512];
} AndroidImportState;

static AndroidImportState s_import;
static char s_disks[ANDROID_DISK_COUNT][ANDROID_PATH_CAPACITY];

static int copy_text(char *out, size_t capacity, const char *value) {
  if (!out || capacity == 0 || !value)
    return 0;
  int written = snprintf(out, capacity, "%s", value);
  return written >= 0 && (size_t)written < capacity;
}

static int ensure_import_state(void) {
  if (s_import.mutex && s_import.condition)
    return 1;
  s_import.mutex = SDL_CreateMutex();
  s_import.condition = SDL_CreateCond();
  return s_import.mutex && s_import.condition;
}

static int call_activity_picker(const char *message, char *error,
                                size_t error_capacity) {
  JNIEnv *environment = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if (!environment || !activity) {
    snprintf(error, error_capacity, "Android Activity is unavailable: %s",
             SDL_GetError());
    return 0;
  }
  jclass activity_class = (*environment)->GetObjectClass(environment, activity);
  jmethodID method =
      activity_class
          ? (*environment)
                ->GetMethodID(environment, activity_class,
                              "requestBenefactorDisks", "(Ljava/lang/String;)V")
          : NULL;
  jstring reason = (*environment)->NewStringUTF(environment, message);
  if (!activity_class || !method || !reason) {
    snprintf(error, error_capacity,
             "Android Activity does not provide Benefactor disk setup");
  } else {
    (*environment)->CallVoidMethod(environment, activity, method, reason);
    if ((*environment)->ExceptionCheck(environment)) {
      (*environment)->ExceptionClear(environment);
      snprintf(error, error_capacity, "Android could not open the disk picker");
    }
  }
  if (reason)
    (*environment)->DeleteLocalRef(environment, reason);
  if (activity_class)
    (*environment)->DeleteLocalRef(environment, activity_class);
  (*environment)->DeleteLocalRef(environment, activity);
  return error[0] == '\0';
}

static int call_activity_void(const char *method_name, char *error,
                              size_t error_capacity) {
  JNIEnv *environment = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if (!environment || !activity) {
    snprintf(error, error_capacity, "Android Activity is unavailable: %s",
             SDL_GetError());
    return 0;
  }
  jclass activity_class = (*environment)->GetObjectClass(environment, activity);
  jmethodID method =
      activity_class
          ? (*environment)
                ->GetMethodID(environment, activity_class, method_name, "()V")
          : NULL;
  if (!activity_class || !method) {
    snprintf(error, error_capacity, "Android Activity does not provide %s()",
             method_name);
  } else {
    (*environment)->CallVoidMethod(environment, activity, method);
    if ((*environment)->ExceptionCheck(environment)) {
      (*environment)->ExceptionClear(environment);
      snprintf(error, error_capacity,
               "Android Activity failed while calling %s", method_name);
    }
  }
  if (activity_class)
    (*environment)->DeleteLocalRef(environment, activity_class);
  (*environment)->DeleteLocalRef(environment, activity);
  return error[0] == '\0';
}

static int call_activity_commit(const char *staging_root, char *installed_root,
                                size_t installed_capacity, char *error,
                                size_t error_capacity) {
  JNIEnv *environment = (JNIEnv *)SDL_AndroidGetJNIEnv();
  jobject activity = (jobject)SDL_AndroidGetActivity();
  if (!environment || !activity) {
    snprintf(error, error_capacity, "Android Activity is unavailable: %s",
             SDL_GetError());
    return 0;
  }
  jclass activity_class = (*environment)->GetObjectClass(environment, activity);
  jmethodID method =
      activity_class
          ? (*environment)
                ->GetMethodID(environment, activity_class,
                              "commitBenefactorDisks",
                              "(Ljava/lang/String;)Ljava/lang/String;")
          : NULL;
  jstring staging = (*environment)->NewStringUTF(environment, staging_root);
  jstring result = NULL;
  if (!activity_class || !method || !staging) {
    snprintf(error, error_capacity,
             "Android Activity does not provide Benefactor disk installation");
  } else {
    result = (jstring)(*environment)
                 ->CallObjectMethod(environment, activity, method, staging);
    if ((*environment)->ExceptionCheck(environment)) {
      (*environment)->ExceptionClear(environment);
      snprintf(error, error_capacity,
               "Android could not retain the validated disk set");
    } else if (!result) {
      snprintf(error, error_capacity, "Android refused the selected disk set");
    } else {
      const char *value =
          (*environment)->GetStringUTFChars(environment, result, NULL);
      if (!value || !copy_text(installed_root, installed_capacity, value)) {
        snprintf(error, error_capacity,
                 "Android returned an invalid installed disk path");
      }
      if (value)
        (*environment)->ReleaseStringUTFChars(environment, result, value);
    }
  }
  if (result)
    (*environment)->DeleteLocalRef(environment, result);
  if (staging)
    (*environment)->DeleteLocalRef(environment, staging);
  if (activity_class)
    (*environment)->DeleteLocalRef(environment, activity_class);
  (*environment)->DeleteLocalRef(environment, activity);
  return error[0] == '\0';
}

static int validate_disks(const char *root, const char **disks,
                          size_t capacity) {
  if (!root || !*root || capacity < ANDROID_DISK_COUNT)
    return 0;
  for (int index = 0; index < ANDROID_DISK_COUNT; ++index) {
    int written = snprintf(s_disks[index], sizeof s_disks[index], "%s/Disk.%d",
                           root, index + 1);
    if (written < 0 || (size_t)written >= sizeof s_disks[index] ||
        access(s_disks[index], R_OK) != 0)
      return 0;
    disks[index] = s_disks[index];
  }
  return 1;
}

int android_bridge_select_disks(const char **disks, size_t capacity) {
  const char *private_root = SDL_AndroidGetInternalStoragePath();
  if (!private_root || !*private_root ||
      !lucent_platform_set_user_data_directory(private_root)) {
    fprintf(stderr, "android: cannot establish Lucent app-private storage\n");
    return 0;
  }
  if (!ensure_import_state()) {
    fprintf(stderr, "android: cannot create import synchronization state: %s\n",
            SDL_GetError());
    return 0;
  }
  char installed_root[ANDROID_PATH_CAPACITY];
  int installed_written = snprintf(installed_root, sizeof installed_root,
                                   "%s/benefactor-disks", private_root);
  if (installed_written < 0 ||
      (size_t)installed_written >= sizeof installed_root) {
    fprintf(stderr, "android: app-private disk path is too long\n");
    return 0;
  }
  if (validate_disks(installed_root, disks, capacity)) {
    if (chdir(private_root) != 0)
      fprintf(stderr, "android: cannot enter private storage\n");
    return 1;
  }

  SDL_LockMutex(s_import.mutex);
  s_import.waiting = 1;
  s_import.complete = 0;
  s_import.staging[0] = '\0';
  s_import.error[0] = '\0';
  SDL_UnlockMutex(s_import.mutex);

  char call_error[512] = "";
  if (!call_activity_picker("Choose the folder containing your original "
                            "Disk.1, Disk.2, and Disk.3 images.",
                            call_error, sizeof call_error)) {
    fprintf(stderr, "android: %s\n", call_error);
    return 0;
  }

  char staging_root[ANDROID_PATH_CAPACITY] = "";
  char import_error[sizeof s_import.error] = "";
  SDL_LockMutex(s_import.mutex);
  while (!s_import.complete)
    SDL_CondWait(s_import.condition, s_import.mutex);
  copy_text(staging_root, sizeof staging_root, s_import.staging);
  copy_text(import_error, sizeof import_error, s_import.error);
  s_import.waiting = 0;
  SDL_UnlockMutex(s_import.mutex);
  if (import_error[0]) {
    fprintf(stderr, "android: disk setup failed: %s\n", import_error);
    return 0;
  }
  if (!validate_disks(staging_root, disks, capacity)) {
    fprintf(stderr, "android: disk setup failed: the selected folder must "
                    "contain readable Disk.1, Disk.2, and Disk.3 files\n");
    return 0;
  }
  char commit_error[512] = "";
  if (!call_activity_commit(staging_root, installed_root, sizeof installed_root,
                            commit_error, sizeof commit_error) ||
      !validate_disks(installed_root, disks, capacity)) {
    fprintf(stderr, "android: disk setup failed: %s\n",
            commit_error[0] ? commit_error
                            : "Android installed an incomplete disk set");
    return 0;
  }
  if (chdir(private_root) != 0)
    fprintf(stderr, "android: cannot enter private storage\n");
  return 1;
}

int android_bridge_enforce_window_policy(void) {
  char error[256] = "";
  if (call_activity_void("enforceBenefactorWindowPolicy", error, sizeof error))
    return 1;
  fprintf(stderr, "android: %s\n", error);
  return 0;
}

JNIEXPORT void JNICALL
Java_io_github_someoneisworking_benefactor_BenefactorActivity_nativeDiskDirectoryResult(
    JNIEnv *environment, jclass unused, jstring directory, jstring error) {
  (void)unused;
  if (!ensure_import_state())
    return;
  SDL_LockMutex(s_import.mutex);
  if (s_import.waiting) {
    if (directory) {
      const char *value =
          (*environment)->GetStringUTFChars(environment, directory, NULL);
      if (value) {
        copy_text(s_import.staging, sizeof s_import.staging, value);
        (*environment)->ReleaseStringUTFChars(environment, directory, value);
      }
    }
    if (error) {
      const char *value =
          (*environment)->GetStringUTFChars(environment, error, NULL);
      if (value) {
        copy_text(s_import.error, sizeof s_import.error, value);
        (*environment)->ReleaseStringUTFChars(environment, error, value);
      }
    }
    s_import.complete = 1;
    SDL_CondBroadcast(s_import.condition);
  }
  SDL_UnlockMutex(s_import.mutex);
}
