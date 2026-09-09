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

// A mail relay that cooperates with the adapter wants to be told, out of band,
// when a game starts and stops using mail. The library signs those events; all
// that is left is carrying them to the server over HTTP, which cannot happen
// where the library asks for it, since that call must return promptly.
#define MOBILE_AUTH_QUEUE_LEN 4
#define MOBILE_AUTH_REQUEST_LEN 320
// Sixteen hex digits, as the library names a device.
#define MOBILE_AUTH_DEVICE_LEN 16
// Room for a counter query's reply: headers plus a body of at most a
// twenty-digit counter, a space and a signature. Anything longer is not the
// answer being waited for.
#define MOBILE_AUTH_RESPONSE_LEN 1024

enum MobileAdapterAuthState {
	MOBILE_AUTH_IDLE = 0,
	MOBILE_AUTH_CONNECTING,
	MOBILE_AUTH_SENDING,
	MOBILE_AUTH_DRAINING
};

struct MobileAdapterAuthEvent {
	// A query asks the server where this device's counter stands instead of
	// reporting anything; it carries no counter, and its reply goes back to
	// the library, which is the only thing that can tell a real answer from
	// a forged one. Then action is meaningless.
	bool query;
	enum mobile_device_auth_action action;
	unsigned char pppId[MOBILE_MAX_NUMBER_SIZE];
	unsigned pppIdSize;
	uint64_t counter;
	unsigned char sig[MOBILE_DEVICE_AUTH_SIG_SIZE];
	// Which of the account's devices this is, as the library names it, or
	// empty when this frontend offered no identity; the server then files the
	// report under the account's one unnamed device.
	char device[MOBILE_AUTH_DEVICE_LEN + 1];
	// Where to report it, resolved by the library against the DNS the adapter
	// is configured with, so this never has to know the name behind it.
	struct Address address;
};

struct MobileAdapterAuth {
	struct MobileAdapterAuthEvent queue[MOBILE_AUTH_QUEUE_LEN];
	unsigned queued;

	// What the side channel has been up to, for a frontend to show. There is
	// otherwise no way to tell a report that was sent from one that was never
	// asked for, which are the same silence from outside.
	unsigned reported;
	unsigned failed;
	// Counter queries that came to an end, answered or not; they are not
	// reports and are not counted as such, but last says how each one went.
	unsigned queried;
	char last[48];

	enum MobileAdapterAuthState state;
	Socket fd;
	char request[MOBILE_AUTH_REQUEST_LEN];
	size_t requestSize;
	size_t sent;
	// The reply: all of it for a query, whose body goes to the library, and
	// as much as fits for a report, which is read only for its status line.
	char response[MOBILE_AUTH_RESPONSE_LEN];
	size_t responseSize;
	// Counted in calls rather than seconds; this is driven once per frame.
	unsigned ticks;
};

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
	// carries config to storage. The library hands over a device-auth counter
	// ceiling the moment it reserves one; if that only reached storage when the
	// adapter was put away, powering off mid-game would replay counters the
	// server has already seen, and it refuses those.
	bool configDirty;
	struct MobileAdapterAuth auth;

	// Called by the driver on every adapter it builds, after its own
	// callbacks are in place and before the adapter starts. The adapter is
	// torn down and built again on every core reset, so anything a frontend
	// registers on it directly is gone after the first one; what it sets
	// here is put back each time.
	void (*setup)(struct MobileAdapterGB*);
};

struct mobile_adapter* MobileAdapterGBNew(struct MobileAdapterGB* mobile);

// "XXXX-XXXX" and its terminator, as the library formats it.
#define MOBILE_PAIRING_CODE_LEN MOBILE_PAIRING_CODE_STR_SIZE

// The code a person matches this device against the account's device list
// by. Formatted by the library, not here, so that every frontend shows the
// same characters the server does. False when this device has no id, in
// which case out is left empty.
bool MobileAdapterGBPairingCode(struct MobileAdapterGB* mobile, char* out, size_t size);

// Wires the side channel up to an adapter. Called for you when one is made.
void MobileAdapterAuthInit(struct MobileAdapterGB* mobile);

// Moves a pending report along by one step. Must be called regularly, and
// never blocks.
void MobileAdapterAuthUpdate(struct MobileAdapterGB* mobile);

CXX_GUARD_END

#endif
