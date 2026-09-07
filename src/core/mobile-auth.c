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

// Where a cooperating relay listens. Deliberately not configurable, and not
// overridable at build time either: every implementation of this reports to
// the same name, and the adapter's own DNS decides what that name means.
#define MOBILE_AUTH_HOST "device.auth.dion.ne.jp"
#define MOBILE_AUTH_PATH "/api/adapter/device-auth"
#define MOBILE_AUTH_PORT 80

// Driven once per emulated frame, so these are roughly ten and three seconds.
#define MOBILE_AUTH_TIMEOUT_TICKS 600
#define MOBILE_AUTH_DRAIN_TICKS 180
#define MOBILE_AUTH_DNS_TICKS 180

#define MOBILE_AUTH_DNS_TRIES 2

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

static void _authNotify(void* user, enum mobile_device_auth_action action, const unsigned char* ppp_id, unsigned ppp_id_size, uint64_t counter, const unsigned char* sig) {
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
	if (ppp_id_size > sizeof(event->pppId)) {
		ppp_id_size = sizeof(event->pppId);
	}
	memcpy(event->pppId, ppp_id, ppp_id_size);
	event->pppIdSize = ppp_id_size;
	memcpy(event->sig, sig, MOBILE_DEVICE_AUTH_SIG_SIZE);
	++auth->queued;
}

void MobileAdapterAuthInit(struct MobileAdapterGB* mobile) {
	struct MobileAdapterAuth* auth = &mobile->auth;
	memset(auth, 0, sizeof(*auth));
	auth->fd = INVALID_SOCKET;
	mobile_def_update_device_auth(mobile->adapter, _authNotify);
}

// Writes an A-record query for name into out, returning its length, or 0 if it
// would not fit.
static size_t _dnsQuery(const char* name, uint16_t id, uint8_t* out, size_t outLength) {
	size_t needed = 12 + strlen(name) + 2 + 4;
	if (outLength < needed) {
		return 0;
	}
	memset(out, 0, 12);
	out[0] = id >> 8;
	out[1] = id & 0xFF;
	out[2] = 0x01;  // Recursion desired
	out[5] = 0x01;  // One question
	size_t len = 12;

	const char* label = name;
	while (*label) {
		const char* dot = strchr(label, '.');
		size_t part = dot ? (size_t) (dot - label) : strlen(label);
		if (!part || part > 63) {
			return 0;
		}
		out[len++] = part;
		memcpy(&out[len], label, part);
		len += part;
		label += dot ? part + 1 : part;
	}
	out[len++] = 0;
	out[len++] = 0;
	out[len++] = 1;  // A
	out[len++] = 0;
	out[len++] = 1;  // IN
	return len;
}

// Steps over one name, which may end in a pointer to an earlier one.
static bool _dnsSkipName(const uint8_t* reply, size_t size, size_t* offset) {
	while (*offset < size) {
		uint8_t len = reply[*offset];
		if (!len) {
			++*offset;
			return true;
		}
		if ((len & 0xC0) == 0xC0) {
			*offset += 2;
			return *offset <= size;
		}
		if (len > 63) {
			return false;
		}
		*offset += len + 1;
	}
	return false;
}

// Pulls the first A record out of a reply to the query that id identifies.
static bool _dnsParse(const uint8_t* reply, size_t size, uint16_t id, struct Address* out) {
	if (size < 12) {
		return false;
	}
	if (((reply[0] << 8) | reply[1]) != id) {
		return false;
	}
	if (!(reply[2] & 0x80) || (reply[3] & 0x0F)) {
		return false;
	}
	unsigned questions = (reply[4] << 8) | reply[5];
	unsigned answers = (reply[6] << 8) | reply[7];
	if (!answers) {
		return false;
	}

	size_t offset = 12;
	unsigned i;
	for (i = 0; i < questions; ++i) {
		if (!_dnsSkipName(reply, size, &offset)) {
			return false;
		}
		offset += 4;
	}

	for (i = 0; i < answers; ++i) {
		if (!_dnsSkipName(reply, size, &offset) || offset + 10 > size) {
			return false;
		}
		unsigned type = (reply[offset] << 8) | reply[offset + 1];
		unsigned class = (reply[offset + 2] << 8) | reply[offset + 3];
		unsigned length = (reply[offset + 8] << 8) | reply[offset + 9];
		offset += 10;
		if (offset + length > size) {
			return false;
		}
		if (type == 1 && class == 1 && length == 4) {
			out->version = IPV4;
			out->ipv4 = (reply[offset] << 24) | (reply[offset + 1] << 16) |
			            (reply[offset + 2] << 8) | reply[offset + 3];
			return true;
		}
		offset += length;
	}
	return false;
}

static bool _authSendQuery(struct MobileAdapterGB* mobile) {
	struct MobileAdapterAuth* auth = &mobile->auth;

	struct mobile_addr dns;
	mobile_config_get_dns(mobile->adapter, &dns, auth->dnsAttempt ? MOBILE_DNS2 : MOBILE_DNS1);
	if (dns.type != MOBILE_ADDRTYPE_IPV4) {
		return false;
	}
	const struct mobile_addr4* dns4 = (const struct mobile_addr4*) &dns;

	auth->dnsId = 0x4d47 + auth->dnsAttempt;
	uint8_t query[128];
	size_t len = _dnsQuery(MOBILE_AUTH_HOST, auth->dnsId, query, sizeof(query));
	if (!len) {
		return false;
	}

	struct Address any = {0};
	any.version = IPV4;
	Socket sock = SocketOpenUDP(0, &any);
	if (SOCKET_FAILED(sock)) {
		return false;
	}
	SocketSetBlocking(sock, false);

	struct Address to = {0};
	to.version = IPV4;
	to.ipv4 = ntohl(*(const uint32_t*) dns4->host);
	if (SOCKET_RESERROR(SocketSendTo(sock, query, len, dns4->port, &to))) {
		SocketClose(sock);
		return false;
	}

	auth->fd = sock;
	auth->state = MOBILE_AUTH_RESOLVING;
	auth->ticks = 0;
	return true;
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

	int written = snprintf(auth->request, sizeof(auth->request),
	    "GET " MOBILE_AUTH_PATH "?ppp_id=%s&action=%s&counter=%" PRIu64 "&sig=%s HTTP/1.1\r\n"
	    "Host: " MOBILE_AUTH_HOST "\r\n"
	    "Connection: close\r\n"
	    "\r\n",
	    pppId, event->action == MOBILE_DEVICE_AUTH_AUTHORIZE ? "authorize" : "deauthorize",
	    event->counter, sig);
	if (written <= 0 || (size_t) written >= sizeof(auth->request)) {
		mLOG(MOBILE_AUTH, ERROR, "Report did not fit in a request");
		return false;
	}
	auth->requestSize = written;
	auth->sent = 0;
	return true;
}

static bool _authConnect(struct MobileAdapterAuth* auth) {
	auth->fd = SocketConnectTCP(MOBILE_AUTH_PORT, &auth->address);
	if (SOCKET_FAILED(auth->fd)) {
		// The address is the first thing to doubt once it cannot be reached.
		auth->resolved = false;
		mLOG(MOBILE_AUTH, WARN, "Could not reach the authorization server");
		return false;
	}
	SocketSetBlocking(auth->fd, false);
	auth->state = MOBILE_AUTH_CONNECTING;
	auth->ticks = 0;
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
		if (auth->resolved) {
			if (!_authConnect(auth)) {
				_authDrop(auth);
			}
			return;
		}
		auth->dnsAttempt = 0;
		if (!_authSendQuery(mobile)) {
			mLOG(MOBILE_AUTH, WARN, "No usable DNS server to find the authorization server");
			_authDrop(auth);
		}
		return;
	}

	++auth->ticks;

	switch (auth->state) {
	case MOBILE_AUTH_RESOLVING: {
		uint8_t reply[512];
		ssize_t res = SocketRecv(auth->fd, reply, sizeof(reply));
		if (res > 0 && _dnsParse(reply, res, auth->dnsId, &auth->address)) {
			auth->resolved = true;
			SocketClose(auth->fd);
			auth->fd = INVALID_SOCKET;
			if (!_authConnect(auth)) {
				_authDrop(auth);
			}
			return;
		}
		if (auth->ticks < MOBILE_AUTH_DNS_TICKS) {
			return;
		}
		// Whatever came back was not an answer to this, so try the other server
		SocketClose(auth->fd);
		auth->fd = INVALID_SOCKET;
		++auth->dnsAttempt;
		if (auth->dnsAttempt >= MOBILE_AUTH_DNS_TRIES || !_authSendQuery(mobile)) {
			mLOG(MOBILE_AUTH, WARN, "Could not look up the authorization server");
			_authDrop(auth);
		}
		return;
	}
	case MOBILE_AUTH_CONNECTING: {
		if (auth->ticks > MOBILE_AUTH_TIMEOUT_TICKS) {
			mLOG(MOBILE_AUTH, WARN, "Report timed out connecting");
			auth->resolved = false;
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
		for (;;) {
			char discard[256];
			ssize_t res = SocketRecv(auth->fd, discard, sizeof(discard));
			if (res > 0) {
				continue;
			}
			if (SOCKET_RESERROR(res) && SocketWouldBlock()) {
				return;
			}
			// Either the server closed cleanly or the read failed; done either way
			_authDrop(auth);
			return;
		}
	}
	case MOBILE_AUTH_IDLE:
		return;
	}
}
