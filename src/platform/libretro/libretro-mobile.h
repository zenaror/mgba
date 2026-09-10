/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef LIBRETRO_MOBILE_H
#define LIBRETRO_MOBILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include "libretro.h"

struct mCore;

// The Mobile Adapter GB behind the link port of a core run by a libretro
// frontend. There is no screen of its own here: the adapter is switched on
// from the core options, takes its settings from the mobile_config.bin the
// user puts in the frontend's system directory and the magb_config.ini beside
// it, and says what it has to say through the frontend's message line and log.
#ifdef USE_LIBMOBILE
void mRetroMobileInit(retro_environment_t env);
void mRetroMobileSettingsChanged(struct mCore* core);
void mRetroMobileAttach(struct mCore* core);
void mRetroMobileDetach(struct mCore* core);
void mRetroMobilePoll(struct mCore* core);
#else
static inline void mRetroMobileInit(retro_environment_t env) {
	UNUSED(env);
}
static inline void mRetroMobileSettingsChanged(struct mCore* core) {
	UNUSED(core);
}
static inline void mRetroMobileAttach(struct mCore* core) {
	UNUSED(core);
}
static inline void mRetroMobileDetach(struct mCore* core) {
	UNUSED(core);
}
static inline void mRetroMobilePoll(struct mCore* core) {
	UNUSED(core);
}
#endif

CXX_GUARD_END

#endif
