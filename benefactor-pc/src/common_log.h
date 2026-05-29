#pragma once

#include <stdio.h>
#include <stdarg.h>

#define GLOBAL_LOG(...)       printf(__VA_ARGS__)
#define GLOBAL_LOG_IF(cond, ...) do { if (cond) printf(__VA_ARGS__); } while (0)
#define GLOBAL_LOG_FLUSH()    fflush(stdout)
