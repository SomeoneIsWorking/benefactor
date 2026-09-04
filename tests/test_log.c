#include "common/log.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

typedef struct SinkProbe {
    int calls;
    BenefactorLogLevel level;
    const char *category;
    char message[32];
} SinkProbe;

static void capture_sink(void *context, BenefactorLogLevel level, const char *category,
                         const char *message) {
    SinkProbe *probe = context;
    ++probe->calls;
    probe->level = level;
    probe->category = category;
    size_t length = strlen(message);
    assert(length < sizeof probe->message);
    memcpy(probe->message, message, length + 1);
}

int main(void) {
    SinkProbe probe = {0};
    benefactor_log_configure(BENEFACTOR_LOG_WARNING, capture_sink, &probe);
    benefactor_log_write(BENEFACTOR_LOG_INFO, "test", "filtered");
    assert(probe.calls == 0);

    benefactor_log_write(BENEFACTOR_LOG_ERROR, "test", "visible");
    assert(probe.calls == 1);
    assert(probe.level == BENEFACTOR_LOG_ERROR);
    assert(strcmp(probe.category, "test") == 0);
    assert(strcmp(probe.message, "visible") == 0);

    benefactor_log_set_level(BENEFACTOR_LOG_INFO);
    benefactor_log_write(BENEFACTOR_LOG_INFO, "test", "new level");
    assert(probe.calls == 2);

    assert(benefactor_log_set_category_level("override", BENEFACTOR_LOG_DEBUG));
    char output[32];
    assert(benefactor_log_capture_begin(output, sizeof output));
    assert(!benefactor_log_capture_begin(output, sizeof output));
    benefactor_log_write(BENEFACTOR_LOG_DEBUG, "override", "captured\n");
    size_t captured = benefactor_log_capture_end();
    assert(captured == strlen("captured\n"));
    assert(memcmp(output, "captured\n", captured) == 0);

    assert(benefactor_log_set_category_level("override", BENEFACTOR_LOG_ERROR));
    assert(benefactor_log_capture_begin(output, sizeof output));
    benefactor_log_write(BENEFACTOR_LOG_DEBUG, "override", "hidden");
    assert(benefactor_log_capture_end() == 0);
    return 0;
}
