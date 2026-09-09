// SPDX-License-Identifier: LGPL-3.0-or-later
#include "device_auth.h"

#include <string.h>

#include "mobile_data.h"
#include "commands.h"
#include "dns.h"
#include "sha256.h"
#include "util.h"
#include "compat.h"

// The device-auth server's hostname. Lives here, and only here -- every
//   frontend gets the resolved address through the callback instead of
//   needing to know this string (or reimplement its own resolution/DNS
//   port handling) independently.
static const char device_auth_host[] = "device.auth.dion.ne.jp";
#define DEVICE_AUTH_HOST_LEN (sizeof(device_auth_host) - 1)

// Longest possible "ppp_id|device|action|counter" message: 0x20 (ppp_id) +
//   1 ('|') + 16 (device id) + 1 ('|') + 14 ("query-response", the longest
//   action word) + 1 ('|') + 20 (2^64-1). The device id and its separator
//   are absent in the older form, which is shorter, so this bounds both.
#define ACTION_MAX_LEN 14
#define MESSAGE_MAX_SIZE \
    (0x20 + 1 + (MOBILE_DEVICE_ID_SIZE * 2) + 1 + ACTION_MAX_LEN + 1 + 20)

static void debug_prefix(struct mobile_adapter *adapter)
{
    mobile_debug_print(adapter, PSTR("<DEVICE-AUTH> "));
}

// Mirrors dns_get_addr() in commands.c exactly: a configured override
//   (config.dns1/dns2, with whatever port it was set up with) is always
//   preferred over the game-chosen server (commands.dns1/dns2, which -- like
//   the real adapter -- only ever uses the fixed DNS_PORT), and DNS2 is a
//   fallback for either. This is the same address a DNS_REQUEST triggered
//   by the game itself would end up using.
static struct mobile_addr *device_auth_get_addr(struct mobile_adapter *adapter, unsigned char id)
{
    switch (id) {
        default:
        case 0: return &adapter->config.dns1;
        case 1: return (struct mobile_addr *)&adapter->commands.dns1;
        case 2: return &adapter->config.dns2;
        case 3: return (struct mobile_addr *)&adapter->commands.dns2;
    }
}

// Renders counter as decimal ASCII, without leading zeros, matching exactly
//   what the device-auth server expects to find in the signed message.
static unsigned counter_to_decimal(uint64_t counter, unsigned char *out)
{
    unsigned char tmp[20];
    unsigned len = 0;

    do {
        tmp[len++] = (unsigned char)('0' + counter % 10);
        counter /= 10;
    } while (counter != 0);

    for (unsigned i = 0; i < len; i++) {
        out[i] = tmp[len - 1 - i];
    }
    return len;
}

void mobile_device_auth_init(struct mobile_adapter *adapter)
{
    adapter->device_auth.state = MOBILE_DEVICE_AUTH_IDLE;
    adapter->device_auth.pending = false;
    adapter->device_auth.device_id[0] = '\0';
    adapter->device_auth.device_id_init = false;
    adapter->device_auth.addr_resolved = false;
    adapter->device_auth.query_pending = false;
    adapter->device_auth.query_inflight = false;
}

void mobile_device_auth_session_start(struct mobile_adapter *adapter)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    // The resolved address deliberately survives: it belongs to the device's
    //   run, not to one session.
    s->query_inflight = false;
    s->query_pending = adapter->config.device_auth_key_init;
}

// Builds "ppp_id|device|action" (or "ppp_id|action" without a device id),
//   the common prefix of every signed device-auth message. Returns its
//   length; the caller appends whatever else that message carries.
static unsigned device_auth_message_prefix(unsigned char *message, const unsigned char *ppp_id, unsigned ppp_id_size, const char *device, const char *action)
{
    unsigned pos = 0;
    memcpy(message + pos, ppp_id, ppp_id_size);
    pos += ppp_id_size;
    message[pos++] = '|';
    if (device) {
        memcpy(message + pos, device, MOBILE_DEVICE_ID_SIZE * 2);
        pos += MOBILE_DEVICE_ID_SIZE * 2;
        message[pos++] = '|';
    }
    unsigned action_len = (unsigned)strlen(action);
    memcpy(message + pos, action, action_len);
    pos += action_len;
    return pos;
}

// Derives this device's id from the frontend's identity bytes, hashing them
//   so nothing the frontend considers identifying (a MAC, a machine id, a
//   host name) is ever put on the wire as-is. Computed once and cached;
//   returns NULL when the frontend offers no identity, which selects the
//   older, device-less form of the message.
static const char *device_auth_device_id(struct mobile_adapter *adapter)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;
    if (s->device_id_init) {
        return s->device_id[0] ? s->device_id : NULL;
    }
    s->device_id_init = true;

    unsigned char identity[MOBILE_DEVICE_IDENTITY_MAX_SIZE];
    unsigned size = mobile_cb_device_identity(adapter, identity,
        sizeof(identity));
    if (size == 0 || size > sizeof(identity)) return NULL;

    unsigned char digest[MOBILE_SHA256_SIZE];
    struct mobile_sha256 ctx;
    mobile_sha256_init(&ctx);
    mobile_sha256_update(&ctx, identity, size);
    mobile_sha256_final(&ctx, digest);

    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < MOBILE_DEVICE_ID_SIZE; i++) {
        s->device_id[i * 2] = hex[digest[i] >> 4];
        s->device_id[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    s->device_id[MOBILE_DEVICE_ID_SIZE * 2] = '\0';

    debug_prefix(adapter);
    mobile_debug_print(adapter, PSTR("Device id "));
    mobile_debug_write(adapter, s->device_id, MOBILE_DEVICE_ID_SIZE * 2);
    mobile_debug_endl(adapter);

    return s->device_id;
}

void mobile_device_auth_notify(struct mobile_adapter *adapter, enum mobile_device_auth_action action, const unsigned char *ppp_id, unsigned ppp_id_size)
{
    if (ppp_id_size == 0 || ppp_id_size > 0x20) return;
    if (!adapter->config.device_auth_key_init) return;

    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    // Say exactly what was lost and how far it got. This is the log that
    //   disambiguates a relay server seeing "deauthorize with no authorize":
    //   that used to mean only one thing (the authorize could never
    //   dispatch at all), but since device-auth yields its connection slot
    //   to the game it can also just mean the authorize kept losing the
    //   slot to a busy game. Only the client-side log can tell those apart.
    if (s->pending) {
        debug_prefix(adapter);
        if (s->pending_action == MOBILE_DEVICE_AUTH_AUTHORIZE) {
            if (s->state == MOBILE_DEVICE_AUTH_IDLE) {
                mobile_debug_print(adapter,
                    PSTR("Replacing authorize that never got a turn to resolve"));
            } else {
                mobile_debug_print(adapter,
                    PSTR("Replacing authorize still resolving"));
            }
        } else {
            if (s->state == MOBILE_DEVICE_AUTH_IDLE) {
                mobile_debug_print(adapter,
                    PSTR("Replacing deauthorize that never got a turn to resolve"));
            } else {
                mobile_debug_print(adapter,
                    PSTR("Replacing deauthorize still resolving"));
            }
        }
        mobile_debug_endl(adapter);
    }

    s->pending = true;
    s->pending_action = action;
    memcpy(s->pending_ppp_id, ppp_id, ppp_id_size);
    s->pending_ppp_id_size = (unsigned char)ppp_id_size;
}

// Signs and dispatches the event that just finished resolving (or failed
//   to), then clears it either way.
static void device_auth_sign_and_dispatch(struct mobile_adapter *adapter, const unsigned char *addr_ipv4)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    uint64_t counter;
    if (!mobile_config_device_auth_next(adapter, &counter)) return;

    unsigned char key[MOBILE_DEVICE_AUTH_KEY_SIZE];
    if (!mobile_config_get_device_auth_key(adapter, key)) return;

    const char *action_name = s->pending_action == MOBILE_DEVICE_AUTH_AUTHORIZE ?
        "authorize" : "deauthorize";

    // "ppp_id|device|action|counter", or "ppp_id|action|counter" when this
    //   device has no id to give. The device field sits where it does so a
    //   server can tell the two forms apart without ambiguity.
    const char *device = device_auth_device_id(adapter);

    unsigned char message[MESSAGE_MAX_SIZE];
    unsigned pos = device_auth_message_prefix(message, s->pending_ppp_id,
        s->pending_ppp_id_size, device, action_name);
    message[pos++] = '|';
    pos += counter_to_decimal(counter, message + pos);

    unsigned char sig[MOBILE_SHA256_SIZE];
    mobile_hmac_sha256(key, sizeof(key), message, pos, sig);

    static_assert(MOBILE_SHA256_SIZE == MOBILE_DEVICE_AUTH_SIG_SIZE,
        "device-auth signature size mismatch");
    mobile_cb_update_device_auth(adapter, s->pending_action,
        s->pending_ppp_id, s->pending_ppp_id_size, counter, sig, addr_ipv4,
        device);
}

// Signs and hands the frontend a counter query. Returns whether it was
//   taken; if not, the session simply proceeds on the counter it has.
static bool device_auth_dispatch_query(struct mobile_adapter *adapter)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;
    struct mobile_adapter_commands *c = &adapter->commands;

    if (c->ppp_id_size == 0 || c->ppp_id_size > 0x20) return false;

    unsigned char key[MOBILE_DEVICE_AUTH_KEY_SIZE];
    if (!mobile_config_get_device_auth_key(adapter, key)) return false;

    const char *device = device_auth_device_id(adapter);

    // "ppp_id|device|query" -- no counter, because this changes nothing on
    //   the server, so replaying it yields only a number that isn't secret.
    unsigned char message[MESSAGE_MAX_SIZE];
    unsigned pos = device_auth_message_prefix(message, c->ppp_id,
        c->ppp_id_size, device, "query");

    unsigned char sig[MOBILE_SHA256_SIZE];
    mobile_hmac_sha256(key, sizeof(key), message, pos, sig);

    if (!mobile_cb_device_auth_query(adapter, s->addr_ipv4, c->ppp_id,
            c->ppp_id_size, sig, device)) {
        return false;
    }

    debug_prefix(adapter);
    mobile_debug_print(adapter, PSTR("Asking the server for our counter"));
    mobile_debug_endl(adapter);
    return true;
}

void mobile_device_auth_query_result(struct mobile_adapter *adapter, const void *data, unsigned size)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;
    struct mobile_adapter_commands *c = &adapter->commands;

    if (!s->query_inflight) return;
    s->query_inflight = false;
    if (!data || size == 0) return;

    // Body is exactly "<counter> <sig>": 1-20 digits, one space, 64 lowercase
    //   hex, and nothing else. Anything else is discarded rather than
    //   salvaged -- this sets a counter, and a bad one strands the device.
    const unsigned char *body = data;
    unsigned digits = 0;
    uint64_t value = 0;
    while (digits < size && body[digits] >= '0' && body[digits] <= '9') {
        // Refuse anything that would wrap, rather than taking it modulo.
        if (value > (UINT64_MAX - (uint64_t)(body[digits] - '0')) / 10) return;
        value = value * 10 + (uint64_t)(body[digits] - '0');
        digits++;
    }
    if (digits == 0 || digits > 20) return;
    if (digits > 1 && body[0] == '0') return;  // no leading zeros
    if (size != digits + 1 + (MOBILE_SHA256_SIZE * 2)) return;
    if (body[digits] != ' ') return;

    unsigned char given[MOBILE_SHA256_SIZE];
    const unsigned char *hex = body + digits + 1;
    for (unsigned i = 0; i < MOBILE_SHA256_SIZE; i++) {
        unsigned char byte = 0;
        for (unsigned j = 0; j < 2; j++) {
            unsigned char ch = hex[i * 2 + j];
            unsigned char nibble;
            if (ch >= '0' && ch <= '9') nibble = (unsigned char)(ch - '0');
            else if (ch >= 'a' && ch <= 'f') nibble = (unsigned char)(ch - 'a' + 10);
            else return;  // uppercase and anything else is not this format
            byte = (unsigned char)(byte << 4 | nibble);
        }
        given[i] = byte;
    }

    unsigned char key[MOBILE_DEVICE_AUTH_KEY_SIZE];
    if (!mobile_config_get_device_auth_key(adapter, key)) return;
    if (c->ppp_id_size == 0 || c->ppp_id_size > 0x20) return;

    unsigned char message[MESSAGE_MAX_SIZE];
    unsigned pos = device_auth_message_prefix(message, c->ppp_id,
        c->ppp_id_size, device_auth_device_id(adapter), "query-response");
    message[pos++] = '|';
    pos += counter_to_decimal(value, message + pos);

    unsigned char want[MOBILE_SHA256_SIZE];
    mobile_hmac_sha256(key, sizeof(key), message, pos, want);

    unsigned diff = 0;
    for (unsigned i = 0; i < MOBILE_SHA256_SIZE; i++) diff |= given[i] ^ want[i];
    if (diff != 0) {
        debug_prefix(adapter);
        mobile_debug_print(adapter, PSTR("Counter answer failed its signature, ignored"));
        mobile_debug_endl(adapter);
        return;
    }

    if (mobile_config_device_auth_catch_up(adapter, value)) {
        debug_prefix(adapter);
        mobile_debug_print(adapter, PSTR("Counter caught up to the server"));
        mobile_debug_endl(adapter);
    }
}

// Starts (or restarts, for the next fallback candidate) resolution at
//   addr_id, borrowing a connection slot via mobile_commands_connection_new()
//   -- the same accounting the game's own connections use, so this can
//   never collide with one it already has open.
// Returns 1 if resolution started (state moved to RESOLVE_SEND); 0 if every
//   candidate address is set, but no connection slot is free right now (the
//   caller must leave the event pending and retry later, NOT give up); -1
//   if there's no address left to try, or the socket/query couldn't be set
//   up (the caller must give up).
static int device_auth_resolve_start(struct mobile_adapter *adapter, unsigned char addr_id)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    struct mobile_addr *addr_send;
    for (; addr_id < 4; addr_id++) {
        addr_send = device_auth_get_addr(adapter, addr_id);
        if (addr_send->type != MOBILE_ADDRTYPE_NONE) break;
    }
    if (addr_id >= 4) return -1;

    int conn = mobile_commands_connection_new(adapter);
    if (conn < 0) return 0;

    mobile_addr_copy(&s->addr, addr_send);
    s->addr_id = addr_id;

    if (!mobile_cb_sock_open(adapter, (unsigned)conn, MOBILE_SOCKTYPE_UDP,
            s->addr.type, 0)) {
        return -1;
    }
    if (!mobile_dns_request_build(adapter, device_auth_host, DEVICE_AUTH_HOST_LEN)) {
        mobile_cb_sock_close(adapter, (unsigned)conn);
        return -1;
    }

    adapter->commands.connections[conn] = true;
    s->conn = (unsigned char)conn;

    debug_prefix(adapter);
    mobile_debug_print(adapter, PSTR("Resolving "));
    mobile_debug_write(adapter, device_auth_host, DEVICE_AUTH_HOST_LEN);
    mobile_debug_endl(adapter);

    mobile_cb_time_latch(adapter, MOBILE_TIMER_COMMAND);
    s->state = MOBILE_DEVICE_AUTH_RESOLVE_SEND;
    return 1;
}

// Closes and releases the connection slot the current attempt was using.
static void device_auth_release_conn(struct mobile_adapter *adapter)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;
    mobile_cb_sock_close(adapter, s->conn);
    adapter->commands.connections[s->conn] = false;
}

void mobile_device_auth_cancel(struct mobile_adapter *adapter)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    // Nothing in flight means nothing to give back. This is also what makes
    //   it safe for device_auth_resolve_start() to go through
    //   mobile_commands_connection_new(), which calls this: it only ever
    //   does so with state == IDLE, so it can never cancel itself.
    if (s->state == MOBILE_DEVICE_AUTH_IDLE) return;

    debug_prefix(adapter);
    mobile_debug_print(adapter, PSTR("Yielding connection to the game"));
    mobile_debug_endl(adapter);

    device_auth_release_conn(adapter);
    s->state = MOBILE_DEVICE_AUTH_IDLE;
    // pending is deliberately left set: the event is retried once a slot
    //   frees up again, rather than being lost.
}

// Sends whatever is due now that the address is known. Both the query and a
//   queued event go out without any lookup in front of them.
static void device_auth_dispatch_due(struct mobile_adapter *adapter)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    // The query first, so a counter corrected by the server is already in
    //   place when the authorize that follows it is signed.
    if (s->query_pending && !s->query_inflight) {
        s->query_pending = false;
        s->query_inflight = device_auth_dispatch_query(adapter);
    }

    if (s->pending) {
        s->pending = false;
        device_auth_sign_and_dispatch(adapter, s->addr_ipv4);
    }
}

void mobile_device_auth_handle(struct mobile_adapter *adapter)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    if (s->state == MOBILE_DEVICE_AUTH_IDLE) {
        if (!s->pending && !s->query_pending) return;

        // Already know where the server is: nothing to look up, send now.
        if (s->addr_resolved) {
            device_auth_dispatch_due(adapter);
            return;
        }

        int rc = device_auth_resolve_start(adapter, 0);
        if (rc == 0) return;  // No connection slot free right now, retry later
        if (rc < 0) {
            debug_prefix(adapter);
            mobile_debug_print(adapter, PSTR("No DNS server available, dropping"));
            mobile_debug_endl(adapter);
            s->pending = false;
            s->query_pending = false;
            return;
        }
        // fallthrough
    }

    if (s->state == MOBILE_DEVICE_AUTH_RESOLVE_SEND) {
        int rc = mobile_dns_request_send(adapter, s->conn, &s->addr);
        if (rc == 0) {
            if (mobile_cb_time_check_ms(adapter, MOBILE_TIMER_COMMAND, 3000)) rc = -1;
            else return;
        }
        if (rc < 0) {
            debug_prefix(adapter);
            mobile_debug_print(adapter, PSTR("Failed to send query, dropping"));
            mobile_debug_endl(adapter);
            device_auth_release_conn(adapter);
            s->state = MOBILE_DEVICE_AUTH_IDLE;
            s->pending = false;
            s->query_pending = false;
            return;
        }

        mobile_cb_time_latch(adapter, MOBILE_TIMER_COMMAND);
        s->state = MOBILE_DEVICE_AUTH_RESOLVE_RECV;
    }

    if (s->state == MOBILE_DEVICE_AUTH_RESOLVE_RECV) {
        unsigned char ip[MOBILE_HOSTLEN_IPV4] = {0};
        int rc = mobile_dns_request_recv(adapter, s->conn, &s->addr,
            device_auth_host, DEVICE_AUTH_HOST_LEN, ip);
        if (rc == 0 && !mobile_cb_time_check_ms(adapter, MOBILE_TIMER_COMMAND, 3000)) {
            return;
        }

        device_auth_release_conn(adapter);
        s->state = MOBILE_DEVICE_AUTH_IDLE;

        if (rc <= 0) {
            // Same fallback the game's own DNS_REQUEST uses: if DNS1 (ids
            //   0-1) just failed, try DNS2 (ids 2-3) before giving up. If no
            //   connection slot is free for that attempt right now (rc2==0),
            //   stay pending -- state is already IDLE, so the next tick
            //   retries cleanly from addr_id 0 (a harmless extra DNS1 round
            //   trip at worst).
            if (s->addr_id < 2) {
                int rc2 = device_auth_resolve_start(adapter, 2);
                if (rc2 != -1) return;
            }

            debug_prefix(adapter);
            mobile_debug_print(adapter, PSTR("Resolution failed, dropping"));
            mobile_debug_endl(adapter);
            s->pending = false;
            s->query_pending = false;
            return;
        }

        // Kept for the rest of this adapter's run, so this is the only
        //   lookup device-auth ever does.
        memcpy(s->addr_ipv4, ip, sizeof(s->addr_ipv4));
        s->addr_resolved = true;

        debug_prefix(adapter);
        mobile_debug_print(adapter, PSTR("Resolved to %u.%u.%u.%u"),
            ip[0], ip[1], ip[2], ip[3]);
        mobile_debug_endl(adapter);

        device_auth_dispatch_due(adapter);
    }
}
