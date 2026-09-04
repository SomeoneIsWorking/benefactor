#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

typedef enum BenefactorLogLevel {
    BENEFACTOR_LOG_TRACE = 0,
    BENEFACTOR_LOG_DEBUG = 1,
    BENEFACTOR_LOG_INFO = 2,
    BENEFACTOR_LOG_WARNING = 3,
    BENEFACTOR_LOG_ERROR = 4,
} BenefactorLogLevel;

typedef void (*BenefactorLogSink)(void *context, BenefactorLogLevel level, const char *category,
                                  const char *message);

#if defined(__GNUC__) || defined(__clang__)
#define BENEFACTOR_PRINTF_FORMAT(format_index, first_argument)                                     \
    __attribute__((format(printf, format_index, first_argument)))
#else
#define BENEFACTOR_PRINTF_FORMAT(format_index, first_argument)
#endif

/* Configure once during composition. A NULL sink selects the default process
 * sink. Filtering belongs here; call sites remain unconditional. */
void benefactor_log_configure(BenefactorLogLevel minimum_level, BenefactorLogSink sink,
                              void *context);
void benefactor_log_set_level(BenefactorLogLevel minimum_level);
int benefactor_log_set_category_level(const char *category, BenefactorLogLevel minimum_level);
int benefactor_log_is_enabled(BenefactorLogLevel level, const char *category);
void benefactor_log_write(BenefactorLogLevel level, const char *category, const char *format, ...)
    BENEFACTOR_PRINTF_FORMAT(3, 4);
void benefactor_log_write_va(BenefactorLogLevel level, const char *category, const char *format,
                             va_list arguments);
void benefactor_log_flush(void);

/* Capture enabled messages from the calling thread without redirecting a
 * process stream. Capture is deliberately thread-local so a test control
 * response cannot steal unrelated runtime logs. */
int benefactor_log_capture_begin(char *buffer, size_t capacity);
size_t benefactor_log_capture_end(void);

/* Fatal POSIX signal handlers cannot call the formatted logger. This bounded
 * emergency path uses only async-signal-safe operations and intentionally
 * bypasses the configured sink because the process cannot resume safely. */
void benefactor_log_signal_hex(const char *message, const uint32_t *values, size_t value_count);

#define GLOBAL_LOG(...) benefactor_log_write(BENEFACTOR_LOG_INFO, "app", __VA_ARGS__)
#define GLOBAL_LOG_FLUSH() benefactor_log_flush()
