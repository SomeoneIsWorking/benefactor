#include "common/log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
    LOG_MESSAGE_CAPACITY = 2048,
    LOG_CATEGORY_CAPACITY = 16,
    LOG_CATEGORY_NAME_CAPACITY = 32,
    SIGNAL_MESSAGE_CAPACITY = 256,
};

typedef struct CategoryLevel {
    atomic_int state;
    atomic_int minimum_level;
    char name[LOG_CATEGORY_NAME_CAPACITY];
} CategoryLevel;

static atomic_int s_minimum_level = BENEFACTOR_LOG_INFO;
static BenefactorLogSink s_sink;
static void *s_sink_context;
static CategoryLevel s_category_levels[LOG_CATEGORY_CAPACITY];
static _Thread_local char *s_capture_buffer;
static _Thread_local size_t s_capture_capacity;
static _Thread_local size_t s_capture_used;

static const char *level_name(BenefactorLogLevel level) {
    switch (level) {
    case BENEFACTOR_LOG_TRACE:
        return "trace";
    case BENEFACTOR_LOG_DEBUG:
        return "debug";
    case BENEFACTOR_LOG_INFO:
        return "info";
    case BENEFACTOR_LOG_WARNING:
        return "warning";
    case BENEFACTOR_LOG_ERROR:
        return "error";
    }
    return "unknown";
}

static void default_sink(void *context, BenefactorLogLevel level, const char *category,
                         const char *message) {
    (void)context;
    fprintf(stderr, "%s: %s: %s\n", level_name(level), category, message);
}

void benefactor_log_configure(BenefactorLogLevel minimum_level, BenefactorLogSink sink,
                              void *context) {
    benefactor_log_set_level(minimum_level);
    s_sink = sink;
    s_sink_context = context;
}

void benefactor_log_set_level(BenefactorLogLevel minimum_level) {
    atomic_store_explicit(&s_minimum_level, minimum_level, memory_order_release);
}

int benefactor_log_set_category_level(const char *category, BenefactorLogLevel minimum_level) {
    if (!category || !category[0] || strlen(category) >= LOG_CATEGORY_NAME_CAPACITY)
        return 0;

    for (size_t index = 0; index < LOG_CATEGORY_CAPACITY; ++index) {
        CategoryLevel *entry = &s_category_levels[index];
        int state = atomic_load_explicit(&entry->state, memory_order_acquire);
        if (state == 2 && strcmp(entry->name, category) == 0) {
            atomic_store_explicit(&entry->minimum_level, minimum_level, memory_order_release);
            return 1;
        }
    }

    for (size_t index = 0; index < LOG_CATEGORY_CAPACITY; ++index) {
        CategoryLevel *entry = &s_category_levels[index];
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(&entry->state, &expected, 1,
                                                     memory_order_acq_rel, memory_order_acquire))
            continue;
        memcpy(entry->name, category, strlen(category) + 1);
        atomic_store_explicit(&entry->minimum_level, minimum_level, memory_order_relaxed);
        atomic_store_explicit(&entry->state, 2, memory_order_release);
        return 1;
    }
    return 0;
}

int benefactor_log_is_enabled(BenefactorLogLevel level, const char *category) {
    if (category) {
        for (size_t index = 0; index < LOG_CATEGORY_CAPACITY; ++index) {
            const CategoryLevel *entry = &s_category_levels[index];
            if (atomic_load_explicit(&entry->state, memory_order_acquire) == 2 &&
                strcmp(entry->name, category) == 0)
                return (int)level >=
                       atomic_load_explicit(&entry->minimum_level, memory_order_acquire);
        }
    }
    return (int)level >= atomic_load_explicit(&s_minimum_level, memory_order_acquire);
}

void benefactor_log_write_va(BenefactorLogLevel level, const char *category, const char *format,
                             va_list arguments) {
    if (!benefactor_log_is_enabled(level, category))
        return;

    char message[LOG_MESSAGE_CAPACITY];
    int length = vsnprintf(message, sizeof message, format, arguments);
    if (length < 0)
        return;
    size_t used = (size_t)length < sizeof message ? (size_t)length : sizeof message - 1;
    while (used > 0 && (message[used - 1] == '\n' || message[used - 1] == '\r'))
        message[--used] = '\0';

    if (s_capture_buffer) {
        size_t available = s_capture_capacity - s_capture_used;
        size_t copied = used < available ? used : available;
        if (copied > 0) {
            memcpy(s_capture_buffer + s_capture_used, message, copied);
            s_capture_used += copied;
        }
        if (s_capture_used < s_capture_capacity)
            s_capture_buffer[s_capture_used++] = '\n';
        return;
    }

    BenefactorLogSink sink = s_sink ? s_sink : default_sink;
    sink(s_sink_context, level, category ? category : "app", message);
}

void benefactor_log_write(BenefactorLogLevel level, const char *category, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    benefactor_log_write_va(level, category, format, arguments);
    va_end(arguments);
}

void benefactor_log_flush(void) {
    /* The test oracle still contains vendor-owned stdout writes. Keeping the
     * process-stream flush here lets its HTTP adapter capture them without
     * leaking direct stream policy into first-party call sites. */
    fflush(stdout);
    fflush(stderr);
}

int benefactor_log_capture_begin(char *buffer, size_t capacity) {
    if (!buffer || capacity == 0 || s_capture_buffer)
        return 0;
    s_capture_buffer = buffer;
    s_capture_capacity = capacity;
    s_capture_used = 0;
    return 1;
}

size_t benefactor_log_capture_end(void) {
    size_t used = s_capture_used;
    s_capture_buffer = NULL;
    s_capture_capacity = 0;
    s_capture_used = 0;
    return used;
}

static size_t signal_append_text(char *output, size_t capacity, size_t used, const char *text) {
    if (!text)
        return used;
    while (*text && used < capacity)
        output[used++] = *text++;
    return used;
}

static size_t signal_append_hex(char *output, size_t capacity, size_t used, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    used = signal_append_text(output, capacity, used, " $");
    for (int shift = 28; shift >= 0 && used < capacity; shift -= 4)
        output[used++] = digits[(value >> shift) & 0xFu];
    return used;
}

void benefactor_log_signal_hex(const char *message, const uint32_t *values, size_t value_count) {
    char output[SIGNAL_MESSAGE_CAPACITY];
    size_t used = signal_append_text(output, sizeof output, 0, "error: signal: ");
    used = signal_append_text(output, sizeof output, used, message);
    for (size_t index = 0; values && index < value_count; ++index)
        used = signal_append_hex(output, sizeof output, used, values[index]);
    if (used < sizeof output)
        output[used++] = '\n';
    (void)write(STDERR_FILENO, output, used);
}
