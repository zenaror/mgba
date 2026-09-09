/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef MOBILE_H
#define MOBILE_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba-util/socket.h>
#include <mobile.h>

struct MobileAdapterGB {
	void* p;

	struct mobile_adapter* adapter;
	uint8_t config[MOBILE_CONFIG_SIZE];
	struct {
		Socket fd;
		enum mobile_socktype socktype;
	} socket[MOBILE_MAX_CONNECTIONS];
	int serial;
	char number[2][MOBILE_MAX_NUMBER_SIZE + 1];
	bool statusUpdate;
	// Set whenever the library writes into config, and cleared by whoever
	// carries config to storage, so nothing the library changed is lost when
	// the console is switched off before the adapter is put away.
	bool configDirty;
};

struct mobile_adapter* MobileAdapterGBNew(struct MobileAdapterGB* mobile);

CXX_GUARD_END

#endif
