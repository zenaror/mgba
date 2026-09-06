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

#define MOBILE_CONFIG_FILE "mobile_config.bin"
#define ADDR_TEXT_LEN 64
#define TOKEN_TEXT_LEN (MOBILE_RELAY_TOKEN_SIZE * 2 + 1)

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
	char dns1[ADDR_TEXT_LEN];
	char dns2[ADDR_TEXT_LEN];
	char p2pPort[8];
	char relay[ADDR_TEXT_LEN];
	char token[TOKEN_TEXT_LEN];
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
	if (!_adapter(m)) {
		return;
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
}

static bool _attach(struct mGUIRunner* runner) {
	if (!runner->core) {
		return false;
	}
	if (runner->mobile && runner->mobile->attached) {
		return true;
	}
	if (!runner->mobile) {
		runner->mobile = calloc(1, sizeof(*runner->mobile));
		if (!runner->mobile) {
			return false;
		}
	}
	struct mGUIMobileAdapter* m = runner->mobile;
	m->platform = runner->core->platform(runner->core);

	// Nothing else on these frontends uses sockets, so the network stack is
	// only brought up once an adapter is actually plugged in.
	SocketSubsystemInit();

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
	return m->attached;
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

	if (!adapter) {
		strlcpy(text->status, "Adapter not running", sizeof(text->status));
		text->dns1[0] = '\0';
		text->dns2[0] = '\0';
		text->relay[0] = '\0';
		text->token[0] = '\0';
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
	}
}

static void _applyToggles(struct mGUIRunner* runner, struct GUIMenu* menu) {
	// Switching it off doesn't unplug the adapter yet: it stays up while this
	// screen is open so the settings below still have something to edit. The
	// teardown happens once the screen closes.
	if (GUIMenuItemListGetPointer(&menu->items, MOBILE_ITEM_ENABLE)->state) {
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

static bool _editText(struct mGUIRunner* runner, const char* title, char* buffer, size_t bufferLength) {
	if (!runner->params.getText) {
		return false;
	}
	struct GUIKeyboardParams keyboard;
	GUIKeyboardParamsInit(&keyboard);
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
	if (!adapter || !_editText(runner, title, buffer, bufferLength)) {
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
	if (!adapter || !_editText(runner, "Relay token (32 hex digits, empty to clear)", buffer, bufferLength)) {
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
	if (!adapter || !_editText(runner, "P2P port", buffer, bufferLength)) {
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
		.validStates = (const char*[]) { text.token },
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

	// Attach temporarily if needed, so the settings have live data to show and
	// edit even when the adapter isn't enabled to keep running afterwards.
	bool wasAttached = runner->mobile && runner->mobile->attached;
	if (!wasAttached) {
		_attach(runner);
	}
	_refresh(runner, &menu, &text);
	GUIMenuItemListGetPointer(&menu.items, MOBILE_ITEM_ENABLE)->state = wasAttached;

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

	if (GUIMenuItemListGetPointer(&menu.items, MOBILE_ITEM_ENABLE)->state) {
		_saveConfig(runner->mobile);
	} else {
		mGUIMobileAdapterDetach(runner);
	}
	GUIMenuItemListDeinit(&menu.items);
}

#endif
