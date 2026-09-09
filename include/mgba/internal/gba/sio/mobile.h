/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_MOBILE_H
#define GBA_MOBILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/gba/interface.h>
#include <mgba/core/mobile.h>

struct GBASIOMobileAdapter {
	struct GBASIODriver d;
	struct MobileAdapterGB m;
	uint32_t timeLatch[MOBILE_MAX_TIMERS];
	uint32_t next;
};

void GBASIOMobileAdapterCreate(struct GBASIOMobileAdapter*);
void GBASIOMobileAdapterUpdate(struct GBASIOMobileAdapter*);

CXX_GUARD_END

#endif
