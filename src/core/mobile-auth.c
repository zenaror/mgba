/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/core/mobile.h>

#include <mgba/core/log.h>
#include <mgba-util/string.h>

mLOG_DECLARE_CATEGORY(MOBILE_AUTH);
mLOG_DEFINE_CATEGORY(MOBILE_AUTH, "Mobile Adapter auth", "mobile.auth");

// The name behind this address, and the DNS it is looked up against, are the
// library's business; all that reaches here is where to send the report.
#define MOBILE_AUTH_HOST "device.auth.dion.ne.jp"
#define MOBILE_AUTH_PATH "/api/adapter/device-auth"
#define MOBILE_AUTH_PORT 80

// Driven once per emulated frame, so these are roughly ten and three seconds.
#define MOBILE_AUTH_TIMEOUT_TICKS 600
#define MOBILE_AUTH_DRAIN_TICKS 180

static void _authClose(struct MobileAdapterAuth* auth) {
	if (!SOCKET_FAILED(auth->fd)) {
		SocketClose(auth->fd);
	}
	auth->fd = INVALID_SOCKET;
	auth->state = MOBILE_AUTH_IDLE;
	auth->requestSize = 0;
	auth->sent = 0;
	auth->ticks = 0;
}

static void _authPop(struct MobileAdapterAuth* auth) {
	if (auth->queued) {
		memmove(&auth->queue[0], &auth->queue[1], sizeof(auth->queue[0]) * (auth->queued - 1));
		--auth->queued;
	}
}

static void _authDrop(struct MobileAdapterAuth* auth) {
	_authClose(auth);
	_authPop(auth);
}

static void _authNotify(void* user, enum mobile_device_auth_action action, const unsigned char* pppId,
                        unsigned pppIdSize, uint64_t counter, const unsigned char* sig,
                        const unsigned char* addrIpv4, const char* device) {
	struct MobileAdapterGB* mobile = user;
	struct MobileAdapterAuth* auth = &mobile->auth;

	if (auth->queued >= MOBILE_AUTH_QUEUE_LEN) {
		// Whatever is already queued was asked for first and describes an
		// earlier state of the session, so it keeps its place.
		mLOG(MOBILE_AUTH, WARN, "Too many reports in flight, dropping one");
		return;
	}

	struct MobileAdapterAuthEvent* event = &auth->queue[auth->queued];
	memset(event, 0, sizeof(*event));
	event->action = action;
	event->counter = counter;
	if (pppIdSize > sizeof(event->pppId)) {
		pppIdSize = sizeof(event->pppId);
	}
	memcpy(event->pppId, pppId, pppIdSize);
	event->pppIdSize = pppIdSize;
	memcpy(event->sig, sig, MOBILE_DEVICE_AUTH_SIG_SIZE);
	if (device) {
		strlcpy(event->device, device, sizeof(event->device));
	}
	snprintf(auth->last, sizeof(auth->last), "%s queued, #%" PRIu64,
	    action == MOBILE_DEVICE_AUTH_AUTHORIZE ? "authorize" : "deauthorize", counter);
	event->address.version = IPV4;
	event->address.ipv4 = (addrIpv4[0] << 24) | (addrIpv4[1] << 16) |
	                      (addrIpv4[2] << 8) | addrIpv4[3];
	++auth->queued;
}

void MobileAdapterAuthInit(struct MobileAdapterGB* mobile) {
	struct MobileAdapterAuth* auth = &mobile->auth;
	memset(auth, 0, sizeof(*auth));
	auth->fd = INVALID_SOCKET;
	mobile_def_update_device_auth(mobile->adapter, _authNotify);
}

static bool _authBuildRequest(struct MobileAdapterAuth* auth, const struct MobileAdapterAuthEvent* event) {
	char pppId[MOBILE_MAX_NUMBER_SIZE + 1];
	memcpy(pppId, event->pppId, event->pppIdSize);
	pppId[event->pppIdSize] = '\0';

	char sig[MOBILE_DEVICE_AUTH_SIG_SIZE * 2 + 1];
	unsigned i;
	for (i = 0; i < MOBILE_DEVICE_AUTH_SIG_SIZE; ++i) {
		snprintf(&sig[i * 2], 3, "%02x", event->sig[i]);
	}

	// Named only when the library named it: without a name the server files
	// the report under the account's one unnamed device, and that is also the
	// message the library signed.
	char device[sizeof("&device=") + MOBILE_AUTH_DEVICE_LEN];
	device[0] = '\0';
	if (event->device[0]) {
		snprintf(device, sizeof(device), "&device=%s", event->device);
	}

	int written = snprintf(auth->request, sizeof(auth->request),
	    "GET " MOBILE_AUTH_PATH "?ppp_id=%s%s&action=%s&counter=%" PRIu64 "&sig=%s HTTP/1.1\r\n"
	    "Host: " MOBILE_AUTH_HOST "\r\n"
	    "Connection: close\r\n"
	    "\r\n",
	    pppId, device, event->action == MOBILE_DEVICE_AUTH_AUTHORIZE ? "authorize" : "deauthorize",
	    event->counter, sig);
	if (written <= 0 || (size_t) written >= sizeof(auth->request)) {
		mLOG(MOBILE_AUTH, ERROR, "Report did not fit in a request");
		return false;
	}
	auth->requestSize = written;
	auth->sent = 0;
	return true;
}

void MobileAdapterAuthUpdate(struct MobileAdapterGB* mobile) {
	struct MobileAdapterAuth* auth = &mobile->auth;

	if (auth->state == MOBILE_AUTH_IDLE) {
		if (!auth->queued) {
			return;
		}
		if (!_authBuildRequest(auth, &auth->queue[0])) {
			_authPop(auth);
			return;
		}
		auth->fd = SocketConnectTCP(MOBILE_AUTH_PORT, &auth->queue[0].address);
		if (SOCKET_FAILED(auth->fd)) {
			// Nothing to be done about it: this is a side channel, and the
			// emulated session neither knows nor cares that it failed.
			mLOG(MOBILE_AUTH, WARN, "Could not reach the authorization server");
			++auth->failed;
			strlcpy(auth->last, "unreachable", sizeof(auth->last));
			_authDrop(auth);
			return;
		}
		SocketSetBlocking(auth->fd, false);
		auth->state = MOBILE_AUTH_CONNECTING;
		auth->ticks = 0;
		return;
	}

	++auth->ticks;

	switch (auth->state) {
	case MOBILE_AUTH_CONNECTING: {
		if (auth->ticks > MOBILE_AUTH_TIMEOUT_TICKS) {
			mLOG(MOBILE_AUTH, WARN, "Report timed out connecting");
			++auth->failed;
			strlcpy(auth->last, "timed out connecting", sizeof(auth->last));
			_authDrop(auth);
			return;
		}
		Socket writable = auth->fd;
		if (SocketPoll(1, NULL, &writable, NULL, 0) <= 0) {
			return;
		}
		auth->state = MOBILE_AUTH_SENDING;
	}
	// The socket is ready, so there is no reason to wait another frame
	// fallthrough
	case MOBILE_AUTH_SENDING: {
		if (auth->ticks > MOBILE_AUTH_TIMEOUT_TICKS) {
			mLOG(MOBILE_AUTH, WARN, "Report timed out sending");
			++auth->failed;
			strlcpy(auth->last, "timed out sending", sizeof(auth->last));
			_authDrop(auth);
			return;
		}
		while (auth->sent < auth->requestSize) {
			ssize_t res = SocketSend(auth->fd, &auth->request[auth->sent], auth->requestSize - auth->sent);
			if (SOCKET_RESERROR(res)) {
				if (SocketWouldBlock()) {
					return;
				}
				mLOG(MOBILE_AUTH, WARN, "Report could not be sent");
				++auth->failed;
				strlcpy(auth->last, "send failed", sizeof(auth->last));
				_authDrop(auth);
				return;
			}
			// A send may take less than it was given, and the rest goes next.
			auth->sent += res;
		}
		auth->state = MOBILE_AUTH_DRAINING;
		auth->ticks = 0;
		return;
	}
	case MOBILE_AUTH_DRAINING: {
		// Hanging up before the server has finished writing looks to it like a
		// client that gave up, so the reply is read to its end, though nothing
		// here needs to know what it says.
		if (auth->ticks > MOBILE_AUTH_DRAIN_TICKS) {
			_authDrop(auth);
			return;
		}
		while (true) {
			char discard[256];
			ssize_t res = SocketRecv(auth->fd, discard, sizeof(discard));
			if (res > 0) {
				continue;
			}
			if (SOCKET_RESERROR(res) && SocketWouldBlock()) {
				return;
			}
			// Either the server closed cleanly or the read failed; done either way
			++auth->reported;
			snprintf(auth->last, sizeof(auth->last), "%s sent, #%" PRIu64,
			    auth->queue[0].action == MOBILE_DEVICE_AUTH_AUTHORIZE ? "authorize" : "deauthorize",
			    auth->queue[0].counter);
			_authDrop(auth);
			return;
		}
	}
	case MOBILE_AUTH_IDLE:
		return;
	}
}
