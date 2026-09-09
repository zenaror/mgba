/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GB_MOBILE_H
#define GB_MOBILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/gb/interface.h>
#include <mgba/core/mobile.h>

struct GBSIOMobileAdapter {
	struct GBSIODriver d;
	struct MobileAdapterGB m;
	uint32_t timeLatch[MOBILE_MAX_TIMERS];
	uint8_t byte;
	uint8_t next;
};

void GBSIOMobileAdapterCreate(struct GBSIOMobileAdapter*);
void GBSIOMobileAdapterUpdate(struct GBSIOMobileAdapter*);

CXX_GUARD_END

#endif
