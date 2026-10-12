/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/core/mobile.h>

static void _serialDisable(void* user) {
	struct MobileAdapterGB* mobile = user;

	mobile->serial = 0;
}

static void _serialEnable(void* user, bool mode32Bit) {
	struct MobileAdapterGB* mobile = user;

	mobile->serial = mode32Bit ? 4 : 1;
}

static bool _configRead(void* user, void* dest, uintptr_t offset, size_t size) {
	struct MobileAdapterGB* mobile = user;

	memcpy(dest, mobile->config + offset, size);
	return true;
}

static bool _configWrite(void* user, const void* src, uintptr_t offset, size_t size) {
	struct MobileAdapterGB* mobile = user;

	// Only a change is worth a trip to storage: the library rewrites whole
	// sections it has not touched, and a card has only so many writes in it.
	if (memcmp(mobile->config + offset, src, size) != 0) {
		memcpy(mobile->config + offset, src, size);
		mobile->configDirty = true;
	}
	return true;
}

static bool _sockOpen(void* user, unsigned conn, enum mobile_socktype type, enum mobile_addrtype addrType,
                      unsigned bindPort) {
	struct MobileAdapterGB* mobile = user;

	Socket fd;

	mobile->socket[conn].sockType = type;

	struct Address bindAddr = {0};
	if (addrType != MOBILE_ADDRTYPE_IPV6) {
		bindAddr.version = IPV4;
	} else {
		bindAddr.version = IPV6;
	}

	if (type != MOBILE_SOCKTYPE_UDP) {
		fd = SocketOpenTCP(bindPort, &bindAddr);
	} else {
		fd = SocketOpenUDP(bindPort, &bindAddr);
	}
	if (!SOCKET_FAILED(fd)) {
		SocketSetBlocking(fd, false);
	}

	mobile->socket[conn].fd = fd;
	return !SOCKET_FAILED(fd);
}

static void _sockClose(void* user, unsigned conn) {
	struct MobileAdapterGB* mobile = user;

	if (!SOCKET_FAILED(mobile->socket[conn].fd)) {
		SocketClose(mobile->socket[conn].fd);
	}
	mobile->socket[conn].fd = INVALID_SOCKET;
	mobile->socket[conn].sockType = 0;
}

static int _sockConnect(void* user, unsigned conn, const struct mobile_addr* addr) {
	struct MobileAdapterGB* mobile = user;

	Socket fd = mobile->socket[conn].fd;

	struct Address connAddr;
	int connPort;
	if (addr->type == MOBILE_ADDRTYPE_IPV6) {
		const struct mobile_addr6* addr6 = (struct mobile_addr6*) addr;
		connAddr.version = IPV6;
		memcpy(&connAddr.ipv6, addr6->host, MOBILE_HOSTLEN_IPV6);
		connPort = addr6->port;
	} else {
		const struct mobile_addr4* addr4 = (struct mobile_addr4*) addr;
		connAddr.version = IPV4;
		connAddr.ipv4 = ntohl(*(uint32_t*) &addr4->host);
		connPort = addr4->port;
	}

	// Two slightly uncommon assumptions are being made about SocketConnect:
	// (these are true under any *strict* implementation of BSD sockets)
	// - It works on a socket that has been obtained through SocketOpenTCP/UDP()
	// - It doesn't block when SocketSetBlocking() has been used
	int rc = SocketConnect(fd, connPort, &connAddr);
	if (!rc || SocketIsConnected()) {
		return 1;
	}

	return SocketIsConnecting() ? 0 : -1;
}

static bool _sockListen(void* user, unsigned conn) {
	struct MobileAdapterGB* mobile = user;

	return !SOCKET_RESERROR(SocketListen(mobile->socket[conn].fd, 1));
}

static bool _sockAccept(void* user, unsigned conn) {
	struct MobileAdapterGB* mobile = user;

	Socket fd = SocketAccept(mobile->socket[conn].fd, NULL);
	if (!SOCKET_FAILED(fd)) {
		SocketSetBlocking(fd, false);
		SocketClose(mobile->socket[conn].fd);
		mobile->socket[conn].fd = fd;
	}

	return !SOCKET_FAILED(fd);
}

static int _sockSend(void* user, unsigned conn, const void* data, unsigned size, const struct mobile_addr* addr) {
	struct MobileAdapterGB* mobile = user;

	struct Address sendAddr;
	int destPort = 0;
	struct Address* destAddr = NULL;

	if (addr) {
		if (addr->type == MOBILE_ADDRTYPE_IPV6) {
			const struct mobile_addr6* addr6 = (struct mobile_addr6*) addr;
			sendAddr.version = IPV6;
			memcpy(&sendAddr.ipv6, addr6->host, MOBILE_HOSTLEN_IPV6);
			destAddr = &sendAddr;
			destPort = addr6->port;
		} else {
			const struct mobile_addr4* addr4 = (struct mobile_addr4*) addr;
			sendAddr.version = IPV4;
			sendAddr.ipv4 = ntohl(*(uint32_t*) &addr4->host);
			destAddr = &sendAddr;
			destPort = addr4->port;
		}
	}

	ssize_t res = SocketSendTo(mobile->socket[conn].fd, data, size, destPort, destAddr);
	return !SOCKET_RESERROR(res) ? res : -1;
}

static int _sockRecv(void* user, unsigned conn, void* data, unsigned size, struct mobile_addr* addr) {
	struct MobileAdapterGB* mobile = user;

	// No polling first: the socket is non-blocking, so the read below already
	// answers "has anything arrived" by itself, and it answers honestly on
	// services whose polling does not.
	struct Address srcAddr = {0};
	int srcPort = 0;
	ssize_t res = SocketRecvFrom(mobile->socket[conn].fd, data, size, &srcPort, &srcAddr);
	if (SOCKET_RESERROR(res)) {
		return SocketWouldBlock() ? 0 : -1;
	}

	if (res > 0 && addr) {
		if (srcAddr.version == IPV6) {
			struct mobile_addr6* addr6 = (struct mobile_addr6*) addr;
			addr6->type = MOBILE_ADDRTYPE_IPV6;
			memcpy(&addr6->host, &srcAddr.ipv6, MOBILE_HOSTLEN_IPV6);
			addr6->port = srcPort;
		} else {
			struct mobile_addr4* addr4 = (struct mobile_addr4*) addr;
			addr4->type = MOBILE_ADDRTYPE_IPV4;
			*(uint32_t*) &addr4->host = htonl(srcAddr.ipv4);
			addr4->port = srcPort;
		}
	}

	return (res || (mobile->socket[conn].sockType == MOBILE_SOCKTYPE_UDP)) ? res : -2;
}

static void _updateNumber(void* user, enum mobile_number type, const char* number) {
	struct MobileAdapterGB* mobile = user;

	char* dest = mobile->number[type];
	if (number) {
		strncpy(dest, number, MOBILE_MAX_NUMBER_SIZE);
		dest[MOBILE_MAX_NUMBER_SIZE] = '\0';
	} else {
		dest[0] = '\0';
	}

	mobile->statusUpdate = true;
}

// What tells this machine apart from another one on the same account. The
// library only ever hashes these bytes; they are never sent or stored, so any
// stable ones will do, and the machine's name is the floor when nothing better
// is known. A console has a radio whose address is better still, and its own
// frontend answers with that instead of this.
static unsigned _deviceIdentity(void* user, void* data, unsigned size) {
	UNUSED(user);
#if defined(__3DS__) || defined(PSP2) || defined(__SWITCH__) || defined(GEKKO)
	UNUSED(data);
	UNUSED(size);
	return 0;
#else
	char* out = data;
	unsigned len = 0;
#ifdef _WIN32
	DWORD guidSize = size;
	if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ, NULL,
	                 out, &guidSize) == ERROR_SUCCESS && guidSize > 1) {
		return guidSize - 1;
	}
#else
	FILE* f = fopen("/etc/machine-id", "r");
	if (!f) {
		f = fopen("/var/lib/dbus/machine-id", "r");
	}
	if (f) {
		len = fread(out, 1, size, f);
		fclose(f);
		while (len && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
			--len;
		}
		if (len) {
			return len;
		}
	}
#endif
	if (gethostname(out, size) != 0) {
		return 0;
	}
	out[size - 1] = '\0';
	len = strlen(out);
	const char* who = getenv("USER");
	if (!who) {
		who = getenv("USERNAME");
	}
	if (who && len + 1 < size) {
		unsigned room = size - len;
		int n = snprintf(out + len, room, "|%s", who);
		if (n > 0) {
			len += (unsigned) n < room ? (unsigned) n : room - 1;
		}
	}
	return len;
#endif
}

bool MobileAdapterGBPairingCode(struct MobileAdapterGB* mobile, char* out, size_t size) {
	if (size) {
		out[0] = '\0';
	}
	if (size < MOBILE_PAIRING_CODE_LEN || !mobile->adapter) {
		return false;
	}
	if (!mobile_device_auth_get_pairing_code(mobile->adapter, out)) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool MobileAdapterGBHasAuthKey(struct MobileAdapterGB* mobile) {
	if (!mobile->adapter) {
		return false;
	}
	// The library hands the key out rather than answering a question about
	// it, so take it into a buffer that dies here and keep only the answer.
	// Wiped through a volatile pointer: explicit_bzero() is not there on
	// every toolchain this builds with, and a plain memset() on a buffer
	// nothing reads again is exactly what a compiler is allowed to drop.
	unsigned char key[MOBILE_DEVICE_AUTH_KEY_SIZE];
	bool has = mobile_config_get_device_auth_key(mobile->adapter, key);
	volatile unsigned char* wipe = key;
	for (size_t i = 0; i < sizeof(key); ++i) {
		wipe[i] = 0;
	}
	return has;
}

struct mobile_adapter* MobileAdapterGBNew(struct MobileAdapterGB* mobile) {
	struct mobile_adapter* adapter = mobile_new(mobile);
	if (!adapter) {
		return NULL;
	}

	mobile_def_serial_disable(adapter, _serialDisable);
	mobile_def_serial_enable(adapter, _serialEnable);
	mobile_def_config_read(adapter, _configRead);
	mobile_def_config_write(adapter, _configWrite);
	mobile_def_sock_open(adapter, _sockOpen);
	mobile_def_sock_close(adapter, _sockClose);
	mobile_def_sock_connect(adapter, _sockConnect);
	mobile_def_sock_listen(adapter, _sockListen);
	mobile_def_sock_accept(adapter, _sockAccept);
	mobile_def_sock_send(adapter, _sockSend);
	mobile_def_sock_recv(adapter, _sockRecv);
	mobile_def_update_number(adapter, _updateNumber);
	// The name is part of the device's id, so it is a plain literal that a
	// rename must never reach, and its spelling is agreed with the other
	// frontends: see the library's list of names in use.
	mobile_def_device_identity(adapter, _deviceIdentity, "mgba");

	mobile->adapter = adapter;
	MobileAdapterAuthInit(mobile);

	return adapter;
}
