/* Copyright (c) 2013-2026 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "libretro-mobile.h"

#ifdef USE_LIBMOBILE

#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/mobile.h>
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

mLOG_DECLARE_CATEGORY(RETRO_MOBILE);
mLOG_DEFINE_CATEGORY(RETRO_MOBILE, "Mobile Adapter", "retro.mobile");

#define MOBILE_OPTION "mgba_mobile_adapter"
#define MOBILE_CONFIG_FILE "mobile_config.bin"
#define MOBILE_IDENTITY_FILE "mobile_identity.bin"
#define MOBILE_IDENTITY_SIZE 16
// The keys DoubleCherryGB reads from its own ini are spelled the same here,
// so a line copied from one works in the other.
#define MOBILE_INI_FILE "magb_config.ini"
#define MOBILE_INI_MAX 4096
#define ADDR_TEXT_LEN 64
// Roughly five seconds on the frontend's message line.
#define MOBILE_MESSAGE_FRAMES 300

static retro_environment_t _env;
static bool _wanted;
static bool _attached;
static enum mPlatform _platform;
#ifdef M_CORE_GB
static struct GBSIOMobileAdapter _gb;
#endif
#ifdef M_CORE_GBA
static struct GBASIOMobileAdapter _gba;
#endif
static char _dir[PATH_MAX];

// What names this device to the server. A core loaded by a frontend has no
// radio and no machine id it can reach, so it draws a few random bytes once
// and keeps them in a file of its own beside the config — never inside the
// config, which a user may legitimately copy to another device. The library
// only ever hashes these bytes.
static uint8_t _identity[MOBILE_IDENTITY_SIZE];
static bool _hasIdentity;

static enum mobile_device_auth_block_state _blockShown;
static unsigned _reportsSeen;

static struct MobileAdapterGB* _adapter(void) {
	switch (_platform) {
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		return &_gb.m;
#endif
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		return &_gba.m;
#endif
	default:
		return NULL;
	}
}

static void _message(const char* text) {
	mLOG(RETRO_MOBILE, INFO, "%s", text);
	struct retro_message msg = {
		.msg = text,
		.frames = MOBILE_MESSAGE_FRAMES
	};
	_env(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

// The library's own chatter, a line per command. The frontend's log is the
// only place it can go, and it goes at the level the frontend shows by
// default: it is only produced while the adapter is on, and on the consoles
// every line of it is on screen, which is what makes a failed session
// readable at all.
static void _debugLog(void* user, const char* line) {
	UNUSED(user);
	mLOG(RETRO_MOBILE, INFO, "%s", line);
}

static unsigned _deviceIdentity(void* user, void* data, unsigned size) {
	UNUSED(user);
	if (!_hasIdentity || size < MOBILE_IDENTITY_SIZE) {
		return 0;
	}
	memcpy(data, _identity, MOBILE_IDENTITY_SIZE);
	return MOBILE_IDENTITY_SIZE;
}

// Put on every adapter the driver builds, which it does again on each reset.
static void _setup(struct MobileAdapterGB* gb) {
	mobile_def_debug_log(gb->adapter, _debugLog);
	// The name is part of the id and is agreed with the other frontends; a
	// plain literal that a rename must never reach.
	mobile_def_device_identity(gb->adapter, _deviceIdentity, "mgba");
}

static void _path(char* out, size_t size, const char* name) {
	snprintf(out, size, "%s%s%s", _dir, PATH_SEP, name);
}

// The system directory, where a libretro frontend keeps what its cores need
// and is not a game's own — BIOS images, and the other core with a Mobile
// Adapter (DoubleCherryGB) keeps its config there too, so it is where a user
// already looks. The save directory failing that. The file keeps the name
// mGBA gives it on every platform, and is deliberately not the other core's
// file: a config carries the device-auth counter state, and two adapters
// writing one would replay each other's counters.
static bool _resolveDir(void) {
	const char* dir = NULL;
	if (!_env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir || !dir[0]) {
		if (!_env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || !dir || !dir[0]) {
			return false;
		}
	}
	strlcpy(_dir, dir, sizeof(_dir));
	return true;
}

static void _loadConfig(struct MobileAdapterGB* gb) {
	char path[PATH_MAX];
	_path(path, sizeof(path), MOBILE_CONFIG_FILE);
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		mLOG(RETRO_MOBILE, WARN, "No %s in %s; the adapter starts unconfigured", MOBILE_CONFIG_FILE, _dir);
		return;
	}
	vf->read(vf, gb->config, MOBILE_CONFIG_SIZE);
	vf->close(vf);
}

// Written the moment the library changes anything, not when the adapter is
// put away: a device-auth counter reservation that only reached disk at the
// end of a session would be replayed after a crash, and the server refuses
// those.
static void _saveConfig(struct MobileAdapterGB* gb) {
	char path[PATH_MAX];
	_path(path, sizeof(path), MOBILE_CONFIG_FILE);
	struct VFile* vf = VFileOpen(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (!vf) {
		mLOG(RETRO_MOBILE, ERROR, "Could not write %s", path);
		return;
	}
	vf->write(vf, gb->config, MOBILE_CONFIG_SIZE);
	vf->close(vf);
	gb->configDirty = false;
}

static bool _identityUsable(void) {
	unsigned i;
	for (i = 0; i < MOBILE_IDENTITY_SIZE; ++i) {
		if (_identity[i]) {
			return true;
		}
	}
	return false;
}

static void _loadIdentity(void) {
	char path[PATH_MAX];
	_path(path, sizeof(path), MOBILE_IDENTITY_FILE);
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (vf) {
		_hasIdentity = vf->read(vf, _identity, MOBILE_IDENTITY_SIZE) == MOBILE_IDENTITY_SIZE;
		vf->close(vf);
		// The right length is not enough. A file of zeros -- a write cut
		// short, a filesystem that padded it -- would be taken as a perfectly
		// good identity, and every install that ever hit it would derive the
		// same device id and share one entry on the account. Draw again
		// instead, which costs a new device and not a merged one.
		if (_hasIdentity && !_identityUsable()) {
			mLOG(RETRO_MOBILE, WARN, "%s is all zeroes; drawing a new identity", path);
			_hasIdentity = false;
		}
		if (_hasIdentity) {
			return;
		}
	}

	// Drawn once. They need to be stable and to differ from any other
	// device's, not to be secret, so the system's random source is used
	// where there is one and the clock where there is not.
	struct VFile* random = VFileOpen("/dev/urandom", O_RDONLY);
	if (random) {
		_hasIdentity = random->read(random, _identity, MOBILE_IDENTITY_SIZE) == MOBILE_IDENTITY_SIZE;
		random->close(random);
		_hasIdentity = _hasIdentity && _identityUsable();
	}
	if (!_hasIdentity) {
		uint64_t seed = (uint64_t) time(NULL) ^ ((uint64_t) (uintptr_t) &seed << 16) ^ (uint64_t) clock();
		unsigned i;
		for (i = 0; i < MOBILE_IDENTITY_SIZE; ++i) {
			seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
			_identity[i] = seed >> 56;
		}
		_hasIdentity = true;
	}

	vf = VFileOpen(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (!vf) {
		mLOG(RETRO_MOBILE, WARN, "Could not keep %s; this device will look new next time", path);
		return;
	}
	vf->write(vf, _identity, MOBILE_IDENTITY_SIZE);
	vf->close(vf);
}

// "host", "host:port" or "[v6]:port", as the console frontend reads them.
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

// What a line of the ini may set. A frontend with no way to type a setting
// needs somewhere to put what the other frontends take from a menu.
struct RetroMobileIni {
	char dns1[ADDR_TEXT_LEN];
	char dns2[ADDR_TEXT_LEN];
	char relay[ADDR_TEXT_LEN];
	unsigned dnsPort;
	unsigned dns2Port;
	unsigned relayPort;
	unsigned p2pPort;
	char token[MOBILE_RELAY_TOKEN_SIZE * 2 + 1];
	char device[16];
	int unmetered;
	int altMail;
	bool hasToken;
};

static const char* const _deviceNames[] = { "blue", "yellow", "green", "red", "purple", "black", "pink", "grey" };

static void _trim(char* s) {
	char* start = s;
	while (isspace((int) *start)) {
		++start;
	}
	size_t len = strlen(start);
	while (len && isspace((int) start[len - 1])) {
		start[--len] = '\0';
	}
	// Quotes are how the other core's config library writes strings.
	if (len >= 2 && (start[0] == '"' || start[0] == '\'') && start[len - 1] == start[0]) {
		start[len - 1] = '\0';
		++start;
	}
	memmove(s, start, strlen(start) + 1);
}

static bool _iniBool(const char* value) {
	return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 ||
	       strcasecmp(value, "on") == 0;
}

static void _iniLine(struct RetroMobileIni* ini, char* line) {
	_trim(line);
	if (!line[0] || line[0] == '#' || line[0] == ';') {
		return;
	}
	char* eq = strchr(line, '=');
	if (!eq) {
		return;
	}
	*eq = '\0';
	char* key = line;
	char* value = eq + 1;
	_trim(key);
	_trim(value);

	if (strcmp(key, "dns_ip") == 0) {
		strlcpy(ini->dns1, value, sizeof(ini->dns1));
	} else if (strcmp(key, "dns_port") == 0) {
		ini->dnsPort = strtoul(value, NULL, 10);
	} else if (strcmp(key, "dns2_ip") == 0) {
		strlcpy(ini->dns2, value, sizeof(ini->dns2));
	} else if (strcmp(key, "dns2_port") == 0) {
		ini->dns2Port = strtoul(value, NULL, 10);
	} else if (strcmp(key, "relay_ip") == 0) {
		strlcpy(ini->relay, value, sizeof(ini->relay));
	} else if (strcmp(key, "relay_port") == 0) {
		ini->relayPort = strtoul(value, NULL, 10);
	} else if (strcmp(key, "p2p_port") == 0) {
		ini->p2pPort = strtoul(value, NULL, 10);
	} else if (strcmp(key, "relay_token") == 0) {
		strlcpy(ini->token, value, sizeof(ini->token));
		ini->hasToken = true;
	} else if (strcmp(key, "device") == 0) {
		strlcpy(ini->device, value, sizeof(ini->device));
	} else if (strcmp(key, "unmetered") == 0) {
		ini->unmetered = _iniBool(value);
	} else if (strcmp(key, "mail_port_587") == 0) {
		ini->altMail = _iniBool(value);
	}
}

// Left for the user to fill in, with every key present and switched off, so
// that nobody has to guess the spelling.
static void _writeIniTemplate(const char* path) {
	static const char template[] =
	    "# Mobile Adapter GB settings for the mGBA libretro core.\n"
	    "# Anything set here overrides mobile_config.bin when the adapter is\n"
	    "# switched on; a line starting with # is ignored. The keys dns_ip,\n"
	    "# dns_port, relay_ip and relay_port mean the same as in DoubleCherryGB.\n"
	    "\n"
	    "#dns_ip = \"152.67.55.127\"\n"
	    "#dns_port = 53\n"
	    "#dns2_ip = \"\"\n"
	    "#dns2_port = 53\n"
	    "#relay_ip = \"\"\n"
	    "#relay_port = 31227\n"
	    "#p2p_port = 1027\n"
	    "#relay_token = \"\"\n"
	    "#device = \"blue\"\n"
	    "#unmetered = false\n"
	    "#mail_port_587 = false\n";
	struct VFile* vf = VFileOpen(path, O_WRONLY | O_CREAT | O_TRUNC);
	if (!vf) {
		return;
	}
	vf->write(vf, template, sizeof(template) - 1);
	vf->close(vf);
}

static bool _applyAddr(struct mobile_adapter* adapter, const char* what, const char* text, unsigned port,
                       unsigned defaultPort, struct mobile_addr* out) {
	if (!text[0]) {
		return false;
	}
	if (!_parseAddr(text, out, port ? port : defaultPort)) {
		mLOG(RETRO_MOBILE, WARN, "%s: '%s' is not an address, ignored", what, text);
		return false;
	}
	UNUSED(adapter);
	return true;
}

// Read every time the adapter is switched on, so a change takes effect on
// the next switch-on rather than the next launch.
static void _applyIni(struct mobile_adapter* adapter) {
	char path[PATH_MAX];
	_path(path, sizeof(path), MOBILE_INI_FILE);
	struct VFile* vf = VFileOpen(path, O_RDONLY);
	if (!vf) {
		_writeIniTemplate(path);
		return;
	}
	char text[MOBILE_INI_MAX + 1];
	ssize_t size = vf->read(vf, text, MOBILE_INI_MAX);
	vf->close(vf);
	if (size <= 0) {
		return;
	}
	text[size] = '\0';

	struct RetroMobileIni ini;
	memset(&ini, 0, sizeof(ini));
	ini.unmetered = -1;
	ini.altMail = -1;
	char* line = text;
	while (line) {
		char* next = strchr(line, '\n');
		if (next) {
			*next = '\0';
			++next;
		}
		_iniLine(&ini, line);
		line = next;
	}

	struct mobile_addr addr;
	if (_applyAddr(adapter, "dns_ip", ini.dns1, ini.dnsPort, MOBILE_DNS_PORT, &addr)) {
		mobile_config_set_dns(adapter, &addr, MOBILE_DNS1);
		mLOG(RETRO_MOBILE, INFO, "ini: DNS1 %s", ini.dns1);
	}
	if (_applyAddr(adapter, "dns2_ip", ini.dns2, ini.dns2Port, MOBILE_DNS_PORT, &addr)) {
		mobile_config_set_dns(adapter, &addr, MOBILE_DNS2);
		mLOG(RETRO_MOBILE, INFO, "ini: DNS2 %s", ini.dns2);
	}
	if (_applyAddr(adapter, "relay_ip", ini.relay, ini.relayPort, MOBILE_DEFAULT_RELAY_PORT, &addr)) {
		mobile_config_set_relay(adapter, &addr);
		mLOG(RETRO_MOBILE, INFO, "ini: relay %s", ini.relay);
	}
	if (ini.p2pPort && ini.p2pPort <= 0xFFFF) {
		mobile_config_set_p2p_port(adapter, ini.p2pPort);
	}
	if (ini.hasToken) {
		if (!ini.token[0]) {
			mobile_config_set_relay_token(adapter, NULL);
		} else if (strlen(ini.token) == MOBILE_RELAY_TOKEN_SIZE * 2) {
			unsigned char token[MOBILE_RELAY_TOKEN_SIZE];
			bool ok = true;
			unsigned i;
			for (i = 0; i < MOBILE_RELAY_TOKEN_SIZE && ok; ++i) {
				char byte[3] = { ini.token[i * 2], ini.token[i * 2 + 1], '\0' };
				char* term;
				token[i] = strtoul(byte, &term, 16);
				ok = !*term && isxdigit((int) byte[0]) && isxdigit((int) byte[1]);
			}
			if (ok) {
				mobile_config_set_relay_token(adapter, token);
			} else {
				mLOG(RETRO_MOBILE, WARN, "relay_token: not 32 hex digits, ignored");
			}
		} else {
			mLOG(RETRO_MOBILE, WARN, "relay_token: not 32 hex digits, ignored");
		}
	}
	if (ini.device[0] || ini.unmetered >= 0) {
		enum mobile_adapter_device device;
		bool unmetered;
		mobile_config_get_device(adapter, &device, &unmetered);
		if (ini.device[0]) {
			unsigned i;
			bool found = false;
			for (i = 0; i < sizeof(_deviceNames) / sizeof(_deviceNames[0]); ++i) {
				if (strcasecmp(ini.device, _deviceNames[i]) == 0) {
					device = MOBILE_ADAPTER_BLUE + i;
					found = true;
				}
			}
			if (!found) {
				mLOG(RETRO_MOBILE, WARN, "device: '%s' is not an adapter colour, ignored", ini.device);
			}
		}
		if (ini.unmetered >= 0) {
			unmetered = ini.unmetered;
		}
		mobile_config_set_device(adapter, device, unmetered);
	}
	if (ini.altMail >= 0) {
		mobile_config_set_alt_mail(adapter, ini.altMail);
	}
}

static bool _readWanted(void) {
	struct retro_variable var = {
		.key = MOBILE_OPTION,
		.value = NULL
	};
	if (!_env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) {
		return false;
	}
	return strcmp(var.value, "enabled") == 0;
}

void mRetroMobileInit(retro_environment_t env) {
	_env = env;
}

void mRetroMobileSettingsChanged(struct mCore* core) {
	_wanted = _readWanted();
	if (!core) {
		return;
	}
	if (_wanted && !_attached) {
		mRetroMobileAttach(core);
	} else if (!_wanted && _attached) {
		mRetroMobileDetach(core);
	}
}

void mRetroMobileAttach(struct mCore* core) {
	if (_attached) {
		return;
	}
	_wanted = _readWanted();
	if (!_wanted) {
		return;
	}
	if (!_resolveDir()) {
		mLOG(RETRO_MOBILE, ERROR, "No save or system directory; the adapter has nowhere to keep its config");
		return;
	}

	_platform = core->platform(core);
	struct MobileAdapterGB* gb = NULL;
	switch (_platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		GBASIOMobileAdapterCreate(&_gba);
		gb = &_gba.m;
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		GBSIOMobileAdapterCreate(&_gb);
		gb = &_gb.m;
		break;
#endif
	default:
		return;
	}

	// Sockets first, config and identity next, driver last: initializing the
	// driver is what starts the adapter, and starting it is what parses the
	// config and asks the setup hook to finish the adapter.
	SocketSubsystemInit();
	_loadConfig(gb);
	_loadIdentity();
	gb->setup = _setup;

	switch (_platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &_gba);
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		GBSIOSetDriver(&((struct GB*) core->board)->sio, &_gb.d);
		break;
#endif
	default:
		break;
	}

	_attached = gb->adapter;
	if (!_attached) {
		SocketSubsystemDeinit();
		mLOG(RETRO_MOBILE, ERROR, "Could not start the Mobile Adapter");
		return;
	}
	_blockShown = MOBILE_DEVICE_AUTH_BLOCK_UNKNOWN;
	_reportsSeen = gb->auth.reported + gb->auth.failed + gb->auth.queried;

	// After the adapter has parsed its config, so the ini overrides it, and
	// whatever it changes goes to the config on the next frame.
	_applyIni(gb->adapter);

	char text[80];
	char code[MOBILE_PAIRING_CODE_LEN];
	bool haveCode = MobileAdapterGBPairingCode(gb, code, sizeof(code));
	if (haveCode) {
		snprintf(text, sizeof(text), "Mobile Adapter GB on, pairing code %s", code);
	} else {
		strlcpy(text, "Mobile Adapter GB on", sizeof(text));
	}
	_message(text);

	// The pairing code is derived from the device id alone, so it reads the
	// same whether or not mail can authenticate. Without this, a core with no
	// key announces itself as ready and then fetches nothing.
	if (haveCode && !MobileAdapterGBHasAuthKey(gb)) {
		_message("No mail key yet - download mobile_config.bin from your account. "
		         "Put it in RetroArch's system folder; mail will not work until then.");
		mLOG(RETRO_MOBILE, WARN,
		     "No mail key yet - download mobile_config.bin from your account. "
		     "Put it in RetroArch's system folder; mail will not work until then.");
	}
}

void mRetroMobileDetach(struct mCore* core) {
	if (!_attached) {
		return;
	}
	struct MobileAdapterGB* gb = _adapter();
	if (gb) {
		_saveConfig(gb);
	}
	switch (_platform) {
#ifdef M_CORE_GBA
	case mPLATFORM_GBA:
		core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, NULL);
		break;
#endif
#ifdef M_CORE_GB
	case mPLATFORM_GB:
		if (core->board) {
			GBSIOSetDriver(&((struct GB*) core->board)->sio, NULL);
		}
		break;
#endif
	default:
		break;
	}
	_attached = false;
	SocketSubsystemDeinit();
	_message("Mobile Adapter GB off");
}

void mRetroMobilePoll(struct mCore* core) {
	UNUSED(core);
	if (!_attached) {
		return;
	}
	struct MobileAdapterGB* gb = _adapter();
	if (!gb || !gb->adapter) {
		return;
	}

	if (gb->configDirty) {
		_saveConfig(gb);
	}

	// Said once, on a verified verdict from the server; a missing answer says
	// nothing, and the game shows its own failure screen either way.
	enum mobile_device_auth_block_state block = mobile_device_auth_block_state(gb->adapter);
	if (block == MOBILE_DEVICE_AUTH_BLOCK_YES && _blockShown != MOBILE_DEVICE_AUTH_BLOCK_YES) {
		_message("Mobile Adapter GB: this device is blocked on the site");
	}
	_blockShown = block;

	// The side channel reports on its own schedule; the log is the only way
	// to line one up against the server's.
	unsigned done = gb->auth.reported + gb->auth.failed + gb->auth.queried;
	if (done != _reportsSeen) {
		_reportsSeen = done;
		mLOG(RETRO_MOBILE, INFO, "relay: %s", gb->auth.last);
	}
}

#endif
