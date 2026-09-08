// SPDX-License-Identifier: LGPL-3.0-or-later
#include "device_auth.h"

#include <string.h>

#include "mobile_data.h"
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

// Reuses the same connection slot as the relay number-fetch background
//   task (mobile.c) -- see mobile_device_auth_handle()'s gating for why
//   that's safe (mutually exclusive, both only run while idle).
static const unsigned device_auth_conn = 0;

// Longest possible "ppp_id|action|counter" message:
//   0x20 (ppp_id) + 1 ('|') + 11 ("deauthorize") + 1 ('|') + 20 (2^64-1)
#define MESSAGE_MAX_SIZE (0x20 + 1 + 11 + 1 + 20)

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
}

void mobile_device_auth_notify(struct mobile_adapter *adapter, enum mobile_device_auth_action action, const unsigned char *ppp_id, unsigned ppp_id_size)
{
    if (ppp_id_size == 0 || ppp_id_size > 0x20) return;
    if (!adapter->config.device_auth_key_init) return;

    struct mobile_adapter_device_auth *s = &adapter->device_auth;
    if (s->pending) {
        debug_prefix(adapter);
        mobile_debug_print(adapter, PSTR("Dropping event still resolving"));
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
    unsigned action_len = (unsigned)strlen(action_name);

    unsigned char message[MESSAGE_MAX_SIZE];
    unsigned pos = 0;

    memcpy(message + pos, s->pending_ppp_id, s->pending_ppp_id_size);
    pos += s->pending_ppp_id_size;
    message[pos++] = '|';
    memcpy(message + pos, action_name, action_len);
    pos += action_len;
    message[pos++] = '|';
    pos += counter_to_decimal(counter, message + pos);

    unsigned char sig[MOBILE_SHA256_SIZE];
    mobile_hmac_sha256(key, sizeof(key), message, pos, sig);

    static_assert(MOBILE_SHA256_SIZE == MOBILE_DEVICE_AUTH_SIG_SIZE,
        "device-auth signature size mismatch");
    mobile_cb_update_device_auth(adapter, s->pending_action,
        s->pending_ppp_id, s->pending_ppp_id_size, counter, sig, addr_ipv4);
}

// Starts (or restarts, for the next fallback candidate) resolution at
//   addr_id. Returns false if there's nothing left to try, or the socket/
//   query couldn't be set up -- either way the caller must give up.
static bool device_auth_resolve_start(struct mobile_adapter *adapter, unsigned char addr_id)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    struct mobile_addr *addr_send;
    for (; addr_id < 4; addr_id++) {
        addr_send = device_auth_get_addr(adapter, addr_id);
        if (addr_send->type != MOBILE_ADDRTYPE_NONE) break;
    }
    if (addr_id >= 4) return false;

    mobile_addr_copy(&s->addr, addr_send);
    s->addr_id = addr_id;

    if (!mobile_cb_sock_open(adapter, device_auth_conn, MOBILE_SOCKTYPE_UDP,
            s->addr.type, 0)) {
        return false;
    }
    if (!mobile_dns_request_build(adapter, device_auth_host, DEVICE_AUTH_HOST_LEN)) {
        mobile_cb_sock_close(adapter, device_auth_conn);
        return false;
    }

    debug_prefix(adapter);
    mobile_debug_print(adapter, PSTR("Resolving "));
    mobile_debug_write(adapter, device_auth_host, DEVICE_AUTH_HOST_LEN);
    mobile_debug_endl(adapter);

    mobile_cb_time_latch(adapter, MOBILE_TIMER_COMMAND);
    s->state = MOBILE_DEVICE_AUTH_RESOLVE_SEND;
    return true;
}

void mobile_device_auth_handle(struct mobile_adapter *adapter)
{
    struct mobile_adapter_device_auth *s = &adapter->device_auth;

    if (s->state == MOBILE_DEVICE_AUTH_IDLE) {
        if (!s->pending) return;

        if (!device_auth_resolve_start(adapter, 0)) {
            debug_prefix(adapter);
            mobile_debug_print(adapter, PSTR("No DNS server available, dropping"));
            mobile_debug_endl(adapter);
            s->pending = false;
            return;
        }
        // fallthrough
    }

    if (s->state == MOBILE_DEVICE_AUTH_RESOLVE_SEND) {
        int rc = mobile_dns_request_send(adapter, device_auth_conn, &s->addr);
        if (rc == 0) {
            if (mobile_cb_time_check_ms(adapter, MOBILE_TIMER_COMMAND, 3000)) rc = -1;
            else return;
        }
        if (rc < 0) {
            debug_prefix(adapter);
            mobile_debug_print(adapter, PSTR("Failed to send query, dropping"));
            mobile_debug_endl(adapter);
            mobile_cb_sock_close(adapter, device_auth_conn);
            s->state = MOBILE_DEVICE_AUTH_IDLE;
            s->pending = false;
            return;
        }

        mobile_cb_time_latch(adapter, MOBILE_TIMER_COMMAND);
        s->state = MOBILE_DEVICE_AUTH_RESOLVE_RECV;
    }

    if (s->state == MOBILE_DEVICE_AUTH_RESOLVE_RECV) {
        unsigned char ip[MOBILE_HOSTLEN_IPV4] = {0};
        int rc = mobile_dns_request_recv(adapter, device_auth_conn, &s->addr,
            device_auth_host, DEVICE_AUTH_HOST_LEN, ip);
        if (rc == 0 && !mobile_cb_time_check_ms(adapter, MOBILE_TIMER_COMMAND, 3000)) {
            return;
        }

        mobile_cb_sock_close(adapter, device_auth_conn);
        s->state = MOBILE_DEVICE_AUTH_IDLE;

        if (rc <= 0) {
            // Same fallback the game's own DNS_REQUEST uses: if DNS1 (ids
            //   0-1) just failed, try DNS2 (ids 2-3) before giving up.
            if (s->addr_id < 2 && device_auth_resolve_start(adapter, 2)) return;

            debug_prefix(adapter);
            mobile_debug_print(adapter, PSTR("Resolution failed, dropping"));
            mobile_debug_endl(adapter);
            s->pending = false;
            return;
        }

        s->pending = false;

        debug_prefix(adapter);
        mobile_debug_print(adapter, PSTR("Resolved to %u.%u.%u.%u"),
            ip[0], ip[1], ip[2], ip[3]);
        mobile_debug_endl(adapter);

        device_auth_sign_and_dispatch(adapter, ip);
    }
}
