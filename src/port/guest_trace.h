#pragma once

#include "runtime/guest_runtime.h"

#include <stddef.h>

/* Write the interpreter's recently retired guest instructions (oldest first,
 * `PC/opcode`) to the log under `category`. Use it wherever a guest flow ends
 * somewhere unexpected: the ring names the path that got there. */
void pc_log_retired_instructions(const char *category);

/* Same ring as plain `PC opcode` lines, for the debug HTTP server. Returns the
 * number of bytes written. */
size_t pc_format_retired_instructions(char *buffer, size_t capacity);

/* Registered on the exception-vector table itself: reaching it means a guest
 * RTS or jump took a wild address, and the interpreter has been walking zeros
 * ever since. It names the fault at its FIRST instruction, while the ring still
 * holds the code that ran before the jump, then unwinds the flow. */
void pc_trap_vector_execution(M68KCtx *ctx);
