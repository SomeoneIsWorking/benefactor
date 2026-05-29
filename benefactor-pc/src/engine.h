/*
 * engine.h  –  Native PC game engine
 */
#pragma once

#include "recomp/rt.h"

void engine_install_overrides(void);
void engine_run(M68KCtx *ctx);
