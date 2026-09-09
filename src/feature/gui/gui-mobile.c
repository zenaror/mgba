/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-mobile.h"

#ifdef USE_LIBMOBILE

#include "feature/gui/gui-runner.h"

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/mobile.h>
#include <mgba-util/gui/font.h>
#include <mgba-util/gui/menu.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#ifdef M_CORE_GB
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/sio/mobile.h>
#endif
#ifdef M_CORE_GBA
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/mobile.h>
#endif

#include <mobile_inet.h>

#ifdef __3DS__
#include <3ds/services/soc.h>
#elif defined(PSP2)
#include <psp2/net/net.h>
#endif

#define MOBILE_CONFIG_FILE "mobile_config.bin"
#define ADDR_TEXT_LEN 64
#define TOKEN_TEXT_LEN (MOBILE_RELAY_TOKEN_SIZE * 2 + 1)
#define MOBILE_LOG_LINES 48
#define MOBILE_LOG_LEN 96

mLOG_DECLARE_CATEGORY(GUI_MOBILE);
mLOG_DEFINE_CATEGORY(GUI_MOBILE, "Mobile Adapter", "gui.mobile");

// What the library reported, kept for the session so a connection attempt can
// be inspected afterwards. There is nowhere to watch a log scroll by on a
// handheld, and reading one off the SD card means powering the console down.
static char _log[MOBILE_LOG_LINES][MOBILE_LOG_LEN];
static size_t _logNext;
static size_t _logCount;
// Off unless asked for: a working session has nothing to say that is worth
// covering the bottom screen with while playing.
static bool _showLog = false;

// Narrating every socket the adapter opens, sends on and reads from. Off by
// default, because it is a line of log per packet; worth turning on from the
// adapter screen when a session fails for reasons the library does not explain,
// which on a console is the only way to see any of this.
static bool _traceSockets = false;

static void _debugLog(void* user, const char* line) {
	UNUSED(user);
	strlcpy(_log[_logNext], line, MOBILE_LOG_LEN);
	_logNext = (_logNext + 1) % MOBILE_LOG_LINES;
	if (_logCount < MOBILE_LOG_LINES) {
		++_logCount;
	}
	mLOG(GUI_MOBILE, DEBUG, "%s", line);
}

// Oldest first, so index 0 is the start of what is still remembered.
static const char* _logLine(size_t i) {
	size_t oldest = _logCount == MOBILE_LOG_LINES ? _logNext : 0;
	return _log[(oldest + i) % MOBILE_LOG_LINES];
}

ATTRIBUTE_FORMAT(printf, 1, 2)
static void _logPrintf(const char* format, ...) {
	char line[MOBILE_LOG_LEN];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	_debugLog(NULL, line);
}

// Whether the console can actually reach the network is otherwise only visible
// as the library failing much later, with nothing to say which part gave up.
static void _logNetworkState(void) {
#ifdef __3DS__
	// Zero here means the console never joined a network, which would other-
	// wise only show up as everything past this point quietly failing.
	uint32_t ip = gethostid();
	const uint8_t* octet = (const uint8_t*) &ip;
	_logPrintf("<mGBA> console is %u.%u.%u.%u", octet[0], octet[1], octet[2], octet[3]);
#endif
	Socket test = SocketCreate(false, SOCK_DGRAM, IPPROTO_UDP);
	if (SOCKET_FAILED(test)) {
		_logPrintf("<mGBA> no network: socket() failed (%i)", SocketError());
		return;
	}
	struct Address any = {0};
	any.version = IPV4;
	if (SocketOpen(test, 0, &any)) {
		_logPrintf("<mGBA> no network: bind failed (%i)", SocketError());
		SocketClose(test);
		return;
	}
	SocketClose(test);
	_logPrintf("<mGBA> network ready");
}

#if defined(__3DS__) || defined(PSP2)
// A console has one radio, and its address is the most stable thing about it:
// it survives the config being copied, wiped or downloaded again, which is
// the whole point of naming devices. The library only ever hashes these
// bytes, so they are handed over exactly as the system reports them.
static unsigned _deviceIdentity(void* user, void* data, unsigned size) {
	UNUSED(user);
#ifdef __3DS__
	// Six octets, however the service chooses to lay them out.
	socklen_t len = size < 24 ? size : 24;
	if (SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_MAC_ADDRESS, data, &len) != 0) {
		return 0;
	}
	return len;
#else
	SceNetEtherAddr addr;
	if (size < sizeof(addr.data) || sceNetGetMacAddress(&addr, 0) < 0) {
		return 0;
	}
	memcpy(data, addr.data, sizeof(addr.data));
	return sizeof(addr.data);
#endif
}
#endif

// Stand in for the core's own callbacks to report what the adapter's sockets
// actually do, which is otherwise invisible from a console.
static bool _loggingSockOpen(void* user, unsigned conn, enum mobile_socktype type, enum mobile_addrtype addrtype,
                             unsigned bindport) {
	struct MobileAdapterGB* mobile = user;

	mobile->socket[conn].socktype = type;

	struct Address bindaddr = {0};
	bindaddr.version = addrtype != MOBILE_ADDRTYPE_IPV6 ? IPV4 : IPV6;

	Socket fd;
	if (type != MOBILE_SOCKTYPE_UDP) {
		fd = SocketOpenTCP(bindport, &bindaddr);
	} else {
		fd = SocketOpenUDP(bindport, &bindaddr);
	}
	if (SOCKET_FAILED(fd)) {
		if (_traceSockets) {
			_logPrintf("<mGBA> conn %u open %s failed (%i)", conn,
			           type == MOBILE_SOCKTYPE_UDP ? "udp" : "tcp", SocketError());
		}
	} else {
		SocketSetBlocking(fd, false);
		if (_traceSockets) {
			_logPrintf("<mGBA> conn %u open %s ok, port %u", conn,
			           type == MOBILE_SOCKTYPE_UDP ? "udp" : "tcp", bindport);
		}
	}

	mobile->socket[conn].fd = fd;
	return !SOCKET_FAILED(fd);
}

static int _loggingSockSend(void* user, unsigned conn, const void* data, unsigned size,
                            const struct mobile_addr* addr) {
	struct MobileAdapterGB* mobile = user;

	struct Address sendaddr = {0};
	int destport = 0;
	struct Address* destaddr = NULL;
	if (addr) {
		destaddr = &sendaddr;
		if (addr->type == MOBILE_ADDRTYPE_IPV6) {
			const struct mobile_addr6* addr6 = (const struct mobile_addr6*) addr;
			sendaddr.version = IPV6;
			memcpy(&sendaddr.ipv6, addr6->host, MOBILE_HOSTLEN_IPV6);
			destport = addr6->port;
		} else {
			const struct mobile_addr4* addr4 = (const struct mobile_addr4*) addr;
			sendaddr.version = IPV4;
			sendaddr.ipv4 = ntohl(*(const uint32_t*) &addr4->host);
			destport = addr4->port;
		}
	}

	ssize_t res = SocketSendTo(mobile->socket[conn].fd, data, size, destport, destaddr);
	if (SOCKET_RESERROR(res)) {
		if (_traceSockets) {
			_logPrintf("<mGBA> conn %u send failed (%i)", conn, SocketError());
		}
		return -1;
	}
	if (_traceSockets) {
		_logPrintf("<mGBA> conn %u sent %i to %u.%u.%u.%u:%i", conn, (int) res,
		           (unsigned) ((sendaddr.ipv4 >> 24) & 0xFF), (unsigned) ((sendaddr.ipv4 >> 16) & 0xFF),
		           (unsigned) ((sendaddr.ipv4 >> 8) & 0xFF), (unsigned) (sendaddr.ipv4 & 0xFF), destport);
	}
	return res;
}

static int _loggingSockRecv(void* user, unsigned conn, void* data, unsigned size, struct mobile_addr* addr) {
	struct MobileAdapterGB* mobile = user;

	struct Address srcaddr = {0};
	int srcport = 0;
	ssize_t res = SocketRecvFrom(mobile->socket[conn].fd, data, size, &srcport, &srcaddr);
	if (SOCKET_RESERROR(res)) {
		if (SocketWouldBlock()) {
			return 0;
		}
		if (_traceSockets) {
			_logPrintf("<mGBA> conn %u read error %i", conn, SocketError());
		}
		return -1;
	}

	if (res > 0 && _traceSockets) {
		_logPrintf("<mGBA> conn %u got %i from %u.%u.%u.%u:%i", conn, (int) res,
		           (unsigned) ((srcaddr.ipv4 >> 24) & 0xFF), (unsigned) ((srcaddr.ipv4 >> 16) & 0xFF),
		           (unsigned) ((srcaddr.ipv4 >> 8) & 0xFF), (unsigned) (srcaddr.ipv4 & 0xFF), srcport);
	}

	if (res > 0 && addr && srcaddr.version == IPV6) {
		struct mobile_addr6* addr6 = (struct mobile_addr6*) addr;
		addr6->type = MOBILE_ADDRTYPE_IPV6;
		memcpy(&addr6->host, &srcaddr.ipv6, MOBILE_HOSTLEN_IPV6);
		addr6->port = srcport;
	} else if (res > 0 && addr) {
		struct mobile_addr4* addr4 = (struct mobile_addr4*) addr;
		addr4->type = MOBILE_ADDRTYPE_IPV4;
		*(uint32_t*) &addr4->host = htonl(srcaddr.ipv4);
		addr4->port = srcport;
	}

	return (res || mobile->socket[conn].socktype == MOBILE_SOCKTYPE_UDP) ? res : -2;
}

struct mGUIMobileAdapter {
#ifdef M_CORE_GB
	struct GBSIOMobileAdapter gb;
#endif
#ifdef M_CORE_GBA
	struct GBASIOMobileAdapter gba;
#endif
	int platform;
	bool attached;
};

enum mGUIMobileItem {
	MOBILE_ITEM_ENABLE = 0,
	MOBILE_ITEM_STATUS,
	MOBILE_ITEM_RELAY_REPORTS,
	MOBILE_ITEM_PAIRING,
	MOBILE_ITEM_SHOW_LOG,
	MOBILE_ITEM_TRACE,
	MOBILE_ITEM_TYPE,
	MOBILE_ITEM_UNMETERED,
	MOBILE_ITEM_DNS1,
	MOBILE_ITEM_DNS2,
	MOBILE_ITEM_P2P_PORT,
	MOBILE_ITEM_RELAY,
	MOBILE_ITEM_TOKEN,
	MOBILE_ITEM_ALT_MAIL,
	MOBILE_ITEM_CLOSE,
	MOBILE_ITEM_MAX
};

struct mGUIMobileText {
	char status[64];
	char reports[64];
	char pairing[16];
	char dns1[ADDR_TEXT_LEN];
	char dns2[ADDR_TEXT_LEN];
	char p2pPort[8];
	char relay[ADDR_TEXT_LEN];
	char token[TOKEN_TEXT_LEN];
	// The token is 32 characters and runs straight into its own label at this
	// width, so the list shows a stub. Editing it still shows the whole thing.
	char tokenShown[16];
};

static struct MobileAdapterGB* _adapter(struct mGUIMobileAdapter* m) {
	if (!m) {
		return NULL;
	}
#ifdef M_CORE_GBA
	if (m->platform == mPLATFORM_GBA) {
		return &m->gba.m;
	}
#endif
#ifdef M_CORE_GB
	if (m->platform == mPLATFORM_GB) {
		return &m->gb.m;
	}
#endif
	return NULL;
}

static struct mobile_adapter* _live(struct mGUIRunner* runner) {
	struct MobileAdapterGB* gb = _adapter(runner->mobile);
	return gb ? gb->adapter : NULL;
}

static void _configPath(char* out, size_t outLength) {
	mCoreConfigDirectory(out, outLength);
	if (!out[0]) {
		return;
	}
	strncat(out, PATH_SEP MOBILE_CONFIG_FILE, outLength - strlen(out) - 1);
}

static void _loadConfig(struct mGUIMobileAdapter* m) {
	char path[PATH_MAX];
	_configPath(path, sizeof(path));
	if (!path[0]) {
		return;
	}
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		return;
	}
	vf->read(vf, _adapter(m)->config, MOBILE_CONFIG_SIZE);
	vf->close(vf);
}

static void _saveConfig(struct mGUIMobileAdapter* m) {
	// An adapter that never ran has nothing but zeroes to write back, which
	// would clobber a perfectly good config file.
	if (!m || !m->attached || !_adapter(m)) {
		return;
	}

	// Settings reach the config blob only when the library flushes them, which
	// it normally does from mobile_loop(). Emulation is paused for as long as
	// this screen is up, so no frame will run to do it for us.
	if (_adapter(m)->adapter) {
		mobile_config_save(_adapter(m)->adapter);
	}
	char path[PATH_MAX];
	_configPath(path, sizeof(path));
	if (!path[0]) {
		return;
	}
	struct VFile* vf = VFileOpen(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (!vf) {
		return;
	}
	vf->write(vf, _adapter(m)->config, MOBILE_CONFIG_SIZE);
	vf->close(vf);
	_adapter(m)->configDirty = false;
}

static bool _alloc(struct mGUIRunner* runner) {
	if (!runner->mobile) {
		runner->mobile = calloc(1, sizeof(*runner->mobile));
	}
	return runner->mobile;
}

// Without a game there is no serial port to plug into, but the stored settings
// can still be read and edited through an adapter that is never started.
static bool _attach(struct mGUIRunner* runner) {
	if (!runner->core) {
		return false;
	}
	if (runner->mobile && runner->mobile->attached) {
		return true;
	}
	if (!_alloc(runner)) {
		return false;
	}
	struct mGUIMobileAdapter* m = runner->mobile;
	m->platform = runner->core->platform(runner->core);

	switch (m->platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		GBASIOMobileAdapterCreate(&m->gba);
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		GBSIOMobileAdapterCreate(&m->gb);
		break;
#endif
	default:
		return false;
	}

	// Nothing else on these frontends uses sockets, so the network stack is
	// only brought up once an adapter is actually plugged in.
	SocketSubsystemInit();

	// The config blob has to be in place before the driver is initialized,
	// since starting the adapter is what parses it.
	_loadConfig(m);

	switch (m->platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		runner->core->setPeripheral(runner->core, mPERIPH_GBA_LINK_PORT, &m->gba);
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		GBSIOSetDriver(&((struct GB*) runner->core->board)->sio, &m->gb.d);
		break;
#endif
	default:
		break;
	}

	m->attached = _adapter(m)->adapter;
	if (!m->attached) {
		SocketSubsystemDeinit();
		return false;
	}
	// Replaces the driver's own logger, which only ever reached a file nobody
	// can read without powering the console down.
	mobile_def_debug_log(_adapter(m)->adapter, _debugLog);
	mobile_def_sock_open(_adapter(m)->adapter, _loggingSockOpen);
	mobile_def_sock_send(_adapter(m)->adapter, _loggingSockSend);
	mobile_def_sock_recv(_adapter(m)->adapter, _loggingSockRecv);
#if defined(__3DS__) || defined(PSP2)
	// Same name as the core registers with, since it is part of the id.
	mobile_def_device_identity(_adapter(m)->adapter, _deviceIdentity, "mgba");
#endif
	return true;
}

void mGUIMobileAdapterAttach(struct mGUIRunner* runner) {
	_attach(runner);
}

bool mGUIMobileAdapterHasLog(struct mGUIRunner* runner) {
	// Deliberately true before anything has been logged: the library only says
	// something once a game talks to the adapter, and a blank screen until then
	// is indistinguishable from the adapter not being plugged in at all.
	return _showLog && runner->mobile && runner->mobile->attached;
}

// The side channel reports on its own schedule, so the only way to line one up
// against a server's log is to say so the moment it happens.
static void _noteRelayReports(struct mGUIRunner* runner) {
	static unsigned seen;
	struct MobileAdapterGB* gb = _adapter(runner->mobile);
	if (!gb) {
		return;
	}
	unsigned done = gb->auth.reported + gb->auth.failed + gb->auth.queried;
	if (done == seen) {
		return;
	}
	seen = done;
	_logPrintf("<mGBA> relay: %s", gb->auth.last);
}

void mGUIMobileAdapterPoll(struct mGUIRunner* runner) {
	if (runner->mobile && runner->mobile->attached) {
		_noteRelayReports(runner);
		// Whatever the library just wrote goes to the card now, not when this
		// adapter is put away: a console is switched off mid-game far more often
		// than it is shut down cleanly, and a lost device-auth counter reservation
		// means every report of the next session is one the server has already
		// refused.
		if (_adapter(runner->mobile)->configDirty) {
			_saveConfig(runner->mobile);
		}
	}
}

void mGUIMobileAdapterDrawLog(struct mGUIRunner* runner) {
	if (!mGUIMobileAdapterHasLog(runner)) {
		return;
	}
	// Resetting the core tears the driver down and builds it again, which
	// restores its own logger, so ours has to be put back.
	if (_live(runner)) {
		mobile_def_debug_log(_live(runner), _debugLog);
	}
	unsigned lineHeight = GUIFontHeight(runner->params.font);
	if (!lineHeight) {
		return;
	}
	unsigned rows = runner->params.height / lineHeight;
	if (rows < 3) {
		return;
	}

	// Second row down: the framerate counter owns the first.
	unsigned y = lineHeight * 2;
	struct MobileAdapterGB* gb = _adapter(runner->mobile);
	// The pairing code goes on the same line so it is in view whenever the
	// log is, which is where someone matching this console against the
	// account's device list will be looking.
	char pairing[MOBILE_PAIRING_CODE_LEN];
	if (!gb || !MobileAdapterGBPairingCode(gb, pairing, sizeof(pairing))) {
		strlcpy(pairing, "-", sizeof(pairing));
	}
	GUIFontPrintf(runner->params.font, 0, y, GUI_ALIGN_LEFT, 0xFFFFFFFF, "Mobile Adapter: %s  [%s]",
	              gb && gb->number[0][0] ? gb->number[0] : "waiting for game", pairing);

	// Oldest first going down, so the newest line sits at the bottom.
	size_t visible = rows - 2;
	if (visible > _logCount) {
		visible = _logCount;
	}
	size_t i;
	for (i = 0; i < visible; ++i) {
		y += lineHeight;
		GUIFontPrint(runner->params.font, 0, y, GUI_ALIGN_LEFT, 0xC0FFFFFF, _logLine(_logCount - visible + i));
	}
}

void mGUIMobileAdapterDetach(struct mGUIRunner* runner) {
	struct mGUIMobileAdapter* m = runner->mobile;
	if (!m || !m->attached) {
		return;
	}

	switch (m->platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		if (runner->core) {
			runner->core->setPeripheral(runner->core, mPERIPH_GBA_LINK_PORT, NULL);
		}
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		if (runner->core && runner->core->board) {
			GBSIOSetDriver(&((struct GB*) runner->core->board)->sio, NULL);
		}
		break;
#endif
	default:
		break;
	}

	_saveConfig(m);
	m->attached = false;
	SocketSubsystemDeinit();
}

static void _addrToString(const struct mobile_addr* addr, unsigned defaultPort, char* out, size_t outLength) {
	out[0] = '\0';
	if (addr->type == MOBILE_ADDRTYPE_IPV4) {
		const struct mobile_addr4* addr4 = (const struct mobile_addr4*) addr;
		const unsigned char* host = addr4->host;
		if (addr4->port != defaultPort) {
			snprintf(out, outLength, "%u.%u.%u.%u:%u", host[0], host[1], host[2], host[3], addr4->port);
		} else {
			snprintf(out, outLength, "%u.%u.%u.%u", host[0], host[1], host[2], host[3]);
		}
	} else if (addr->type == MOBILE_ADDRTYPE_IPV6) {
		const struct mobile_addr6* addr6 = (const struct mobile_addr6*) addr;
		const unsigned char* host = addr6->host;
		char plain[ADDR_TEXT_LEN];
		snprintf(plain, sizeof(plain), "%x:%x:%x:%x:%x:%x:%x:%x",
		         host[0] << 8 | host[1], host[2] << 8 | host[3],
		         host[4] << 8 | host[5], host[6] << 8 | host[7],
		         host[8] << 8 | host[9], host[10] << 8 | host[11],
		         host[12] << 8 | host[13], host[14] << 8 | host[15]);
		if (addr6->port != defaultPort) {
			snprintf(out, outLength, "[%s]:%u", plain, addr6->port);
		} else {
			strlcpy(out, plain, outLength);
		}
	}
}

static bool _parseAddr(const char* text, struct mobile_addr* addr, unsigned defaultPort) {
	memset(addr, 0, sizeof(*addr));
	while (isspace((int) *text)) {
		++text;
	}
	if (!text[0]) {
		addr->type = MOBILE_ADDRTYPE_NONE;
		return true;
	}

	char host[ADDR_TEXT_LEN];
	unsigned port = defaultPort;

	if (text[0] == '[') {
		const char* end = strchr(text, ']');
		if (!end || (size_t) (end - text - 1) >= sizeof(host)) {
			return false;
		}
		memcpy(host, text + 1, end - text - 1);
		host[end - text - 1] = '\0';
		if (end[1] == ':') {
			char* term;
			unsigned long parsed = strtoul(&end[2], &term, 10);
			if (*term || !parsed || parsed > 0xFFFF) {
				return false;
			}
			port = parsed;
		} else if (end[1]) {
			return false;
		}
	} else {
		const char* colon = strchr(text, ':');
		if (colon && !strchr(&colon[1], ':')) {
			if ((size_t) (colon - text) >= sizeof(host)) {
				return false;
			}
			memcpy(host, text, colon - text);
			host[colon - text] = '\0';
			char* term;
			unsigned long parsed = strtoul(&colon[1], &term, 10);
			if (*term || !parsed || parsed > 0xFFFF) {
				return false;
			}
			port = parsed;
		} else {
			strlcpy(host, text, sizeof(host));
		}
	}

	unsigned char parsed[MOBILE_INET_PTON_MAXLEN];
	switch (mobile_inet_pton(MOBILE_INET_PTON_ANY, host, parsed)) {
	case MOBILE_INET_PTON_IPV4: {
		struct mobile_addr4* addr4 = (struct mobile_addr4*) addr;
		addr4->type = MOBILE_ADDRTYPE_IPV4;
		memcpy(addr4->host, parsed, MOBILE_HOSTLEN_IPV4);
		addr4->port = port;
		return true;
	}
#ifdef HAS_IPV6
	case MOBILE_INET_PTON_IPV6: {
		struct mobile_addr6* addr6 = (struct mobile_addr6*) addr;
		addr6->type = MOBILE_ADDRTYPE_IPV6;
		memcpy(addr6->host, parsed, MOBILE_HOSTLEN_IPV6);
		addr6->port = port;
		return true;
	}
#endif
	default:
		return false;
	}
}

static void _refresh(struct mGUIRunner* runner, struct GUIMenu* menu, struct mGUIMobileText* text) {
	struct mobile_adapter* adapter = _live(runner);
	struct MobileAdapterGB* gb = _adapter(runner->mobile);

	GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_SHOW_LOG)->state = _showLog;
	GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_TRACE)->state = _traceSockets;

	if (!adapter) {
		strlcpy(text->status, "Adapter not running", sizeof(text->status));
		strlcpy(text->reports, "-", sizeof(text->reports));
		strlcpy(text->pairing, "-", sizeof(text->pairing));
		text->dns1[0] = '\0';
		text->dns2[0] = '\0';
		text->relay[0] = '\0';
		text->token[0] = '\0';
		text->tokenShown[0] = '\0';
		strlcpy(text->p2pPort, "-", sizeof(text->p2pPort));
		return;
	}

	if (gb->number[0][0]) {
		snprintf(text->status, sizeof(text->status), "Line %s", gb->number[0]);
		if (gb->number[1][0]) {
			size_t used = strlen(text->status);
			snprintf(&text->status[used], sizeof(text->status) - used, " to %s", gb->number[1]);
		}
	} else {
		strlcpy(text->status, "Idle", sizeof(text->status));
	}

	const struct MobileAdapterAuth* auth = &gb->auth;
	if (!auth->reported && !auth->failed && !auth->last[0]) {
		strlcpy(text->reports, "none yet", sizeof(text->reports));
	} else {
		snprintf(text->reports, sizeof(text->reports), "%u ok, %u failed: %s",
		         auth->reported, auth->failed, auth->last);
	}

	if (!MobileAdapterGBPairingCode(gb, text->pairing, sizeof(text->pairing))) {
		strlcpy(text->pairing, "unavailable", sizeof(text->pairing));
	}

	enum mobile_adapter_device device;
	bool unmetered;
	mobile_config_get_device(adapter, &device, &unmetered);
	GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_TYPE)->state = device - MOBILE_ADAPTER_BLUE;
	GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_UNMETERED)->state = unmetered;

	bool altMail;
	mobile_config_get_alt_mail(adapter, &altMail);
	GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_ALT_MAIL)->state = altMail;

	struct mobile_addr addr;
	mobile_config_get_dns(adapter, &addr, MOBILE_DNS1);
	_addrToString(&addr, MOBILE_DNS_PORT, text->dns1, sizeof(text->dns1));
	mobile_config_get_dns(adapter, &addr, MOBILE_DNS2);
	_addrToString(&addr, MOBILE_DNS_PORT, text->dns2, sizeof(text->dns2));
	mobile_config_get_relay(adapter, &addr);
	_addrToString(&addr, MOBILE_DEFAULT_RELAY_PORT, text->relay, sizeof(text->relay));

	unsigned p2pPort;
	mobile_config_get_p2p_port(adapter, &p2pPort);
	snprintf(text->p2pPort, sizeof(text->p2pPort), "%u", p2pPort);

	text->token[0] = '\0';
	unsigned char token[MOBILE_RELAY_TOKEN_SIZE];
	if (mobile_config_get_relay_token(adapter, token)) {
		size_t i;
		for (i = 0; i < MOBILE_RELAY_TOKEN_SIZE; ++i) {
			snprintf(&text->token[i * 2], 3, "%02x", token[i]);
		}
		snprintf(text->tokenShown, sizeof(text->tokenShown), "%.8s...", text->token);
	} else {
		strlcpy(text->tokenShown, "not set", sizeof(text->tokenShown));
	}
}

static void _applyToggles(struct mGUIRunner* runner, struct GUIMenu* menu) {
	// Switching it off doesn't unplug the adapter yet: it stays up while this
	// screen is open so the settings below still have something to edit. The
	// teardown happens once the screen closes.
	runner->mobileEnabled = GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_ENABLE)->state;
	_showLog = GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_SHOW_LOG)->state;

	// Switching tracing on says what the network looks like right away, rather
	// than leaving that until whatever is being chased happens again.
	bool trace = GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_TRACE)->state;
	if (trace && !_traceSockets) {
		_traceSockets = true;
		_logNetworkState();
	}
	_traceSockets = trace;
	if (runner->mobileEnabled && runner->core) {
		_attach(runner);
	}

	struct mobile_adapter* adapter = _live(runner);
	if (!adapter) {
		return;
	}

	mobile_config_set_device(adapter,
	    MOBILE_ADAPTER_BLUE + GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_TYPE)->state,
	    GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_UNMETERED)->state);
	mobile_config_set_alt_mail(adapter, GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_ALT_MAIL)->state);
}

static bool _editText(struct mGUIRunner* runner, const char* title, char* buffer, size_t bufferLength, bool numeric) {
	if (!runner->params.getText) {
		return false;
	}
	struct GUIKeyboardParams keyboard;
	GUIKeyboardParamsInit(&keyboard);
	keyboard.numeric = numeric;
	strlcpy(keyboard.title, title, sizeof(keyboard.title));
	strlcpy(keyboard.result, buffer, sizeof(keyboard.result));
	keyboard.maxLen = bufferLength - 1;
	if (runner->params.getText(&keyboard) != GUI_KEYBOARD_DONE) {
		return false;
	}
	strlcpy(buffer, keyboard.result, bufferLength);
	return true;
}

static void _editAddr(struct mGUIRunner* runner, const char* title, char* buffer, size_t bufferLength, int which) {
	struct mobile_adapter* adapter = _live(runner);
	if (!adapter || !_editText(runner, title, buffer, bufferLength, true)) {
		return;
	}

	unsigned defaultPort = which == MOBILE_ITEM_RELAY ? MOBILE_DEFAULT_RELAY_PORT : MOBILE_DNS_PORT;
	struct mobile_addr addr;
	if (!_parseAddr(buffer, &addr, defaultPort)) {
		GUIShowMessageBox(&runner->params, GUI_MESSAGE_BOX_OK, 240, "Invalid address: %s", buffer);
		return;
	}

	switch (which) {
	case MOBILE_ITEM_DNS1:
		mobile_config_set_dns(adapter, &addr, MOBILE_DNS1);
		break;
	case MOBILE_ITEM_DNS2:
		mobile_config_set_dns(adapter, &addr, MOBILE_DNS2);
		break;
	case MOBILE_ITEM_RELAY:
		mobile_config_set_relay(adapter, &addr);
		break;
	default:
		break;
	}
}

static void _editToken(struct mGUIRunner* runner, char* buffer, size_t bufferLength) {
	struct mobile_adapter* adapter = _live(runner);
	if (!adapter || !_editText(runner, "Relay token (32 hex digits, empty to clear)", buffer, bufferLength, false)) {
		return;
	}

	if (!buffer[0]) {
		mobile_config_set_relay_token(adapter, NULL);
		return;
	}

	uint8_t token[MOBILE_RELAY_TOKEN_SIZE];
	const char* pos = buffer;
	size_t i;
	for (i = 0; i < MOBILE_RELAY_TOKEN_SIZE && pos; ++i) {
		pos = hex8(pos, &token[i]);
	}
	if (!pos || *pos) {
		GUIShowMessageBox(&runner->params, GUI_MESSAGE_BOX_OK, 240, "Token must be 32 hexadecimal digits");
		return;
	}
	mobile_config_set_relay_token(adapter, token);
}

static void _editPort(struct mGUIRunner* runner, char* buffer, size_t bufferLength) {
	struct mobile_adapter* adapter = _live(runner);
	if (!adapter || !_editText(runner, "P2P port", buffer, bufferLength, true)) {
		return;
	}

	char* term;
	unsigned long port = strtoul(buffer, &term, 10);
	if (*term || !port || port > 0xFFFF) {
		GUIShowMessageBox(&runner->params, GUI_MESSAGE_BOX_OK, 240, "Port must be between 1 and 65535");
		return;
	}
	mobile_config_set_p2p_port(adapter, port);
}

void mGUIShowMobileAdapter(struct mGUIRunner* runner) {
	struct mGUIMobileText text = {0};
	struct GUIMenu menu = {
		.title = "Mobile Adapter GB",
		.index = 0,
		.background = &runner->background.d
	};
	GUIMenuItemListInit(&menu.items, MOBILE_ITEM_MAX);

	// Enabling the adapter is deliberately not persisted: it always starts off
	// for a fresh session, and stays on only for as long as this game runs.
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Enable Mobile Adapter GB",
		.data = GUI_V_U(MOBILE_ITEM_ENABLE),
		.validStates = (const char*[]) { "Off", "On" },
		.nStates = 2
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Status",
		.data = GUI_V_U(MOBILE_ITEM_STATUS),
		.validStates = (const char*[]) { text.status },
		.nStates = 1,
		.readonly = true
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Relay reports",
		.data = GUI_V_U(MOBILE_ITEM_RELAY_REPORTS),
		.validStates = (const char*[]) { text.reports },
		.nStates = 1,
		.readonly = true
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Pairing code",
		.data = GUI_V_U(MOBILE_ITEM_PAIRING),
		.validStates = (const char*[]) { text.pairing },
		.nStates = 1,
		.readonly = true
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Log on bottom screen",
		.data = GUI_V_U(MOBILE_ITEM_SHOW_LOG),
		.validStates = (const char*[]) { "Off", "On" },
		.nStates = 2
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Trace sockets",
		.data = GUI_V_U(MOBILE_ITEM_TRACE),
		.validStates = (const char*[]) { "Off", "On" },
		.nStates = 2
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Adapter type",
		.data = GUI_V_U(MOBILE_ITEM_TYPE),
		.validStates = (const char*[]) { "Blue", "Yellow", "Green", "Red" },
		.nStates = 4
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Unmetered connection",
		.data = GUI_V_U(MOBILE_ITEM_UNMETERED),
		.validStates = (const char*[]) { "Off", "On" },
		.nStates = 2
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "DNS server 1",
		.data = GUI_V_U(MOBILE_ITEM_DNS1),
		.validStates = (const char*[]) { text.dns1 },
		.nStates = 1
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "DNS server 2",
		.data = GUI_V_U(MOBILE_ITEM_DNS2),
		.validStates = (const char*[]) { text.dns2 },
		.nStates = 1
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "P2P port",
		.data = GUI_V_U(MOBILE_ITEM_P2P_PORT),
		.validStates = (const char*[]) { text.p2pPort },
		.nStates = 1
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Relay server",
		.data = GUI_V_U(MOBILE_ITEM_RELAY),
		.validStates = (const char*[]) { text.relay },
		.nStates = 1
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Relay token",
		.data = GUI_V_U(MOBILE_ITEM_TOKEN),
		.validStates = (const char*[]) { text.tokenShown },
		.nStates = 1
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Use mail port 587",
		.data = GUI_V_U(MOBILE_ITEM_ALT_MAIL),
		.validStates = (const char*[]) { "Off", "On" },
		.nStates = 2
	};
	*GUIMenuItemListAppend(&menu.items) = (struct GUIMenuItem) {
		.title = "Close",
		.data = GUI_V_U(MOBILE_ITEM_CLOSE)
	};

	// Bring an adapter up if there isn't one, so the settings have live data to
	// show and edit even when it isn't enabled to keep running afterwards.
	if (!runner->mobile || !runner->mobile->attached) {
		_attach(runner);
	}
	if (!runner->mobile) {
		GUIShowMessageBox(&runner->params, GUI_MESSAGE_BOX_OK, 240, "Could not start the Mobile Adapter");
		GUIMenuItemListDeinit(&menu.items);
		return;
	}
	_refresh(runner, &menu, &text);
	GUIMenuItemListGetPointer(&menu.items, MOBILE_ITEM_ENABLE)->state = runner->mobileEnabled;

	while (true) {
		struct GUIMenuItem* item;
		enum GUIMenuExitReason reason = GUIShowMenu(&runner->params, &menu, &item);
		bool accept = reason == GUI_MENU_EXIT_ACCEPT && GUIVariantIsUInt(item->data);

		// Left/right already cycled multi-state items in the menu itself; A
		// advances them too, so both ways of changing a setting work.
		if (accept && item->nStates > 1 && !item->readonly) {
			item->state = (item->state + 1) % item->nStates;
		}
		_applyToggles(runner, &menu);

		if (!accept) {
			break;
		}

		bool done = false;
		switch (item->data.v.u) {
		case MOBILE_ITEM_DNS1:
			_editAddr(runner, "DNS server 1", text.dns1, sizeof(text.dns1), MOBILE_ITEM_DNS1);
			break;
		case MOBILE_ITEM_DNS2:
			_editAddr(runner, "DNS server 2", text.dns2, sizeof(text.dns2), MOBILE_ITEM_DNS2);
			break;
		case MOBILE_ITEM_RELAY:
			_editAddr(runner, "Relay server", text.relay, sizeof(text.relay), MOBILE_ITEM_RELAY);
			break;
		case MOBILE_ITEM_TOKEN:
			_editToken(runner, text.token, sizeof(text.token));
			break;
		case MOBILE_ITEM_P2P_PORT:
			_editPort(runner, text.p2pPort, sizeof(text.p2pPort));
			break;
		case MOBILE_ITEM_CLOSE:
			done = true;
			break;
		default:
			break;
		}
		if (done) {
			break;
		}
		_refresh(runner, &menu, &text);
	}

	// The adapter is left plugged in unless the user switched it off.
	if (runner->mobileEnabled) {
		_saveConfig(runner->mobile);
	} else {
		mGUIMobileAdapterDetach(runner);
	}
	GUIMenuItemListDeinit(&menu.items);
}

#endif
