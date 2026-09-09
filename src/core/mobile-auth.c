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
	auth->responseSize = 0;
	auth->ticks = 0;
}

static void _authPop(struct MobileAdapterAuth* auth) {
	if (auth->queued) {
		memmove(&auth->queue[0], &auth->queue[1], sizeof(auth->queue[0]) * (auth->queued - 1));
		--auth->queued;
	}
}

// Finishes with the event at the head of the queue. A query owes the library
// an answer whatever happened to it, since the library waits on nothing else;
// a report owes nobody anything.
static void _authDrop(struct MobileAdapterGB* mobile, const void* data, unsigned size) {
	struct MobileAdapterAuth* auth = &mobile->auth;
	bool query = auth->queued && auth->queue[0].query;
	_authClose(auth);
	_authPop(auth);
	if (query) {
		mobile_device_auth_query_result(mobile->adapter, data, size);
	}
}

static struct MobileAdapterAuthEvent* _authEnqueue(struct MobileAdapterAuth* auth, const unsigned char* pppId,
                                                   unsigned pppIdSize, const unsigned char* sig,
                                                   const unsigned char* addrIpv4, const char* device) {
	if (auth->queued >= MOBILE_AUTH_QUEUE_LEN) {
		// Whatever is already queued was asked for first and describes an
		// earlier state of the session, so it keeps its place.
		mLOG(MOBILE_AUTH, WARN, "Too many reports in flight, dropping one");
		return NULL;
	}

	struct MobileAdapterAuthEvent* event = &auth->queue[auth->queued];
	memset(event, 0, sizeof(*event));
	if (pppIdSize > sizeof(event->pppId)) {
		pppIdSize = sizeof(event->pppId);
	}
	memcpy(event->pppId, pppId, pppIdSize);
	event->pppIdSize = pppIdSize;
	memcpy(event->sig, sig, MOBILE_DEVICE_AUTH_SIG_SIZE);
	if (device) {
		strlcpy(event->device, device, sizeof(event->device));
	}
	event->address.version = IPV4;
	event->address.ipv4 = (addrIpv4[0] << 24) | (addrIpv4[1] << 16) |
	                      (addrIpv4[2] << 8) | addrIpv4[3];
	++auth->queued;
	return event;
}

static void _authNotify(void* user, enum mobile_device_auth_action action, const unsigned char* pppId,
                        unsigned pppIdSize, uint64_t counter, const unsigned char* sig,
                        const unsigned char* addrIpv4, const char* device) {
	struct MobileAdapterGB* mobile = user;
	struct MobileAdapterAuth* auth = &mobile->auth;

	struct MobileAdapterAuthEvent* event = _authEnqueue(auth, pppId, pppIdSize, sig, addrIpv4, device);
	if (!event) {
		return;
	}
	event->action = action;
	event->counter = counter;
	snprintf(auth->last, sizeof(auth->last), "%s queued, #%" PRIu64,
	    action == MOBILE_DEVICE_AUTH_AUTHORIZE ? "authorize" : "deauthorize", counter);
}

static bool _authQuery(void* user, const unsigned char* addrIpv4, const unsigned char* pppId, unsigned pppIdSize,
                       const unsigned char* sig, const char* device) {
	struct MobileAdapterGB* mobile = user;
	struct MobileAdapterAuth* auth = &mobile->auth;

	struct MobileAdapterAuthEvent* event = _authEnqueue(auth, pppId, pppIdSize, sig, addrIpv4, device);
	if (!event) {
		return false;
	}
	event->query = true;
	strlcpy(auth->last, "counter asked", sizeof(auth->last));
	return true;
}

void MobileAdapterAuthInit(struct MobileAdapterGB* mobile) {
	struct MobileAdapterAuth* auth = &mobile->auth;
	memset(auth, 0, sizeof(*auth));
	auth->fd = INVALID_SOCKET;
	mobile_def_update_device_auth(mobile->adapter, _authNotify);
	mobile_def_device_auth_query(mobile->adapter, _authQuery);
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

	int written;
	if (event->query) {
		// Asked as an HTTP/1.0 client on purpose: the body has to reach the
		// library byte for byte, and a 1.0 client is never sent it in chunks,
		// so it is exactly what follows the headers.
		written = snprintf(auth->request, sizeof(auth->request),
		    "GET " MOBILE_AUTH_PATH "?ppp_id=%s%s&action=query&sig=%s HTTP/1.0\r\n"
		    "Host: " MOBILE_AUTH_HOST "\r\n"
		    "Connection: close\r\n"
		    "\r\n",
		    pppId, device, sig);
	} else {
		written = snprintf(auth->request, sizeof(auth->request),
		    "GET " MOBILE_AUTH_PATH "?ppp_id=%s%s&action=%s&counter=%" PRIu64 "&sig=%s HTTP/1.1\r\n"
		    "Host: " MOBILE_AUTH_HOST "\r\n"
		    "Connection: close\r\n"
		    "\r\n",
		    pppId, device, event->action == MOBILE_DEVICE_AUTH_AUTHORIZE ? "authorize" : "deauthorize",
		    event->counter, sig);
	}
	if (written <= 0 || (size_t) written >= sizeof(auth->request)) {
		mLOG(MOBILE_AUTH, ERROR, "Report did not fit in a request");
		return false;
	}
	auth->requestSize = written;
	auth->sent = 0;
	return true;
}

// Picks the body out of a query's reply, if the server said it was one.
// Nothing here judges what the body says; the library does that.
static bool _authReplyBody(const struct MobileAdapterAuth* auth, const char** body, unsigned* size) {
	if (auth->responseSize < sizeof("HTTP/1.0 200") - 1) {
		return false;
	}
	if (memcmp(auth->response, "HTTP/1.", sizeof("HTTP/1.") - 1) != 0) {
		return false;
	}
	const char* status = memchr(auth->response, ' ', auth->responseSize);
	if (!status || (size_t) (status - auth->response) + 4 > auth->responseSize) {
		return false;
	}
	if (memcmp(status, " 200", 4) != 0) {
		return false;
	}
	size_t i;
	for (i = 0; i + 4 <= auth->responseSize; ++i) {
		if (memcmp(&auth->response[i], "\r\n\r\n", 4) == 0) {
			*body = &auth->response[i + 4];
			*size = auth->responseSize - (i + 4);
			return true;
		}
	}
	return false;
}

static void _authFail(struct MobileAdapterGB* mobile, const char* why) {
	struct MobileAdapterAuth* auth = &mobile->auth;
	if (auth->queue[0].query) {
		mLOG(MOBILE_AUTH, WARN, "Counter query %s", why);
		++auth->queried;
		strlcpy(auth->last, "counter unanswered", sizeof(auth->last));
	} else {
		mLOG(MOBILE_AUTH, WARN, "Report %s", why);
		++auth->failed;
		strlcpy(auth->last, why, sizeof(auth->last));
	}
	_authDrop(mobile, NULL, 0);
}

static void _authDone(struct MobileAdapterGB* mobile) {
	struct MobileAdapterAuth* auth = &mobile->auth;
	if (auth->queue[0].query) {
		const char* body = NULL;
		unsigned size = 0;
		++auth->queried;
		if (_authReplyBody(auth, &body, &size)) {
			strlcpy(auth->last, "counter answered", sizeof(auth->last));
		} else {
			mLOG(MOBILE_AUTH, WARN, "Counter query got no answer");
			strlcpy(auth->last, "counter unanswered", sizeof(auth->last));
		}
		_authDrop(mobile, body, size);
		return;
	}
	++auth->reported;
	snprintf(auth->last, sizeof(auth->last), "%s sent, #%" PRIu64,
	    auth->queue[0].action == MOBILE_DEVICE_AUTH_AUTHORIZE ? "authorize" : "deauthorize",
	    auth->queue[0].counter);
	_authDrop(mobile, NULL, 0);
}

void MobileAdapterAuthUpdate(struct MobileAdapterGB* mobile) {
	struct MobileAdapterAuth* auth = &mobile->auth;

	if (auth->state == MOBILE_AUTH_IDLE) {
		if (!auth->queued) {
			return;
		}
		if (!_authBuildRequest(auth, &auth->queue[0])) {
			_authDrop(mobile, NULL, 0);
			return;
		}
		auth->fd = SocketConnectTCP(MOBILE_AUTH_PORT, &auth->queue[0].address);
		if (SOCKET_FAILED(auth->fd)) {
			// Nothing to be done about it: this is a side channel, and the
			// emulated session neither knows nor cares that it failed.
			_authFail(mobile, "unreachable");
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
			_authFail(mobile, "timed out connecting");
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
			_authFail(mobile, "timed out sending");
			return;
		}
		while (auth->sent < auth->requestSize) {
			ssize_t res = SocketSend(auth->fd, &auth->request[auth->sent], auth->requestSize - auth->sent);
			if (SOCKET_RESERROR(res)) {
				if (SocketWouldBlock()) {
					return;
				}
				_authFail(mobile, "send failed");
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
		// client that gave up, so the reply is read to its end. A report needs
		// nothing it says; a query keeps it for the library.
		if (auth->ticks > MOBILE_AUTH_DRAIN_TICKS) {
			_authFail(mobile, "timed out reading");
			return;
		}
		while (true) {
			char chunk[256];
			ssize_t res = SocketRecv(auth->fd, chunk, sizeof(chunk));
			if (res > 0) {
				if (auth->queue[0].query) {
					if (auth->responseSize + res > sizeof(auth->response)) {
						_authFail(mobile, "answered at length");
						return;
					}
					memcpy(&auth->response[auth->responseSize], chunk, res);
					auth->responseSize += res;
				}
				continue;
			}
			if (SOCKET_RESERROR(res) && SocketWouldBlock()) {
				return;
			}
			// Either the server closed cleanly or the read failed; done either way
			_authDone(mobile);
			return;
		}
	}
	case MOBILE_AUTH_IDLE:
		return;
	}
}
