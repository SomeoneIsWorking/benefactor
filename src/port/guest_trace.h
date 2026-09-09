#pragma once

#include <stddef.h>

/* Write the interpreter's recently retired guest instructions (oldest first,
 * `PC/opcode`) to the log under `category`. Use it wherever a guest flow ends
 * somewhere unexpected: the ring names the path that got there. */
void pc_log_retired_instructions(const char *category);

/* Same ring as plain `PC opcode` lines, for the debug HTTP server. Returns the
 * number of bytes written. */
size_t pc_format_retired_instructions(char *buffer, size_t capacity);
