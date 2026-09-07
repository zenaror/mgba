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

enum MobileAdapterAuthState {
	MOBILE_AUTH_IDLE = 0,
	MOBILE_AUTH_RESOLVING,
	MOBILE_AUTH_CONNECTING,
	MOBILE_AUTH_SENDING,
	MOBILE_AUTH_DRAINING
};

struct MobileAdapterAuthEvent {
	enum mobile_device_auth_action action;
	unsigned char pppId[MOBILE_MAX_NUMBER_SIZE];
	unsigned pppIdSize;
	uint64_t counter;
	unsigned char sig[MOBILE_DEVICE_AUTH_SIG_SIZE];
};

struct MobileAdapterAuth {
	// The server only answers to the name the adapter's own DNS knows it by,
	// so it is looked up the same way a game's host would be, and kept until a
	// connection to it fails.
	struct Address address;
	bool resolved;
	unsigned dnsAttempt;
	uint16_t dnsId;

	struct MobileAdapterAuthEvent queue[MOBILE_AUTH_QUEUE_LEN];
	unsigned queued;

	enum MobileAdapterAuthState state;
	Socket fd;
	char request[MOBILE_AUTH_REQUEST_LEN];
	size_t requestSize;
	size_t sent;
	// Counted in calls rather than seconds; this is driven once per frame.
	unsigned ticks;
};

struct MobileAdapterGB {
	void *p;

	struct mobile_adapter *adapter;
	uint8_t config[MOBILE_CONFIG_SIZE];
	struct {
		Socket fd;
		enum mobile_socktype socktype;
	} socket[MOBILE_MAX_CONNECTIONS];
	int serial;
	char number[2][MOBILE_MAX_NUMBER_SIZE + 1];
	bool status_update;
	struct MobileAdapterAuth auth;
};

struct mobile_adapter* MobileAdapterGBNew(struct MobileAdapterGB *mobile);

// Wires the side channel up to an adapter. Called for you when one is made.
void MobileAdapterAuthInit(struct MobileAdapterGB *mobile);

// Moves a pending report along by one step. Must be called regularly, and
// never blocks.
void MobileAdapterAuthUpdate(struct MobileAdapterGB *mobile);

CXX_GUARD_END

#endif
