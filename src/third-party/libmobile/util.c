// SPDX-License-Identifier: LGPL-3.0-or-later
#include "util.h"

#include <string.h>

#include "mobile_data.h"

static unsigned mobile_addr_size(const struct mobile_addr *addr)
{
    unsigned size = 0;
    if (addr->type == MOBILE_ADDRTYPE_IPV4) size = sizeof(struct mobile_addr4);
    if (addr->type == MOBILE_ADDRTYPE_IPV6) size = sizeof(struct mobile_addr6);
    return size;
}

// Copy enough bytes of address <src> into <dest>, without exceeding the
//   contents of <src>.
// <dest> must have enough space to hold any kind of address.
void mobile_addr_copy(struct mobile_addr *dest, const struct mobile_addr *src)
{
    unsigned size = mobile_addr_size(src);
    if (!size) {
        dest->type = MOBILE_ADDRTYPE_NONE;
        return;
    }
    memcpy(dest, src, size);
}

// Compare addresses <addr1> and <addr2> without reading out of their bounds.
bool mobile_addr_compare(const struct mobile_addr *addr1, const struct mobile_addr *addr2)
{
    if (addr1->type != addr2->type) return false;

    // Comparing these byte for byte would take in the padding a compiler is
    // free to leave after the type, which is three bytes wide wherever an enum
    // is stored in one byte, and which nothing here ever sets. Two addresses
    // that are the same in every field would then still fail to match.
    if (addr1->type == MOBILE_ADDRTYPE_IPV4) {
        const struct mobile_addr4 *a1 = (const struct mobile_addr4 *)addr1;
        const struct mobile_addr4 *a2 = (const struct mobile_addr4 *)addr2;
        return a1->port == a2->port &&
            memcmp(a1->host, a2->host, MOBILE_HOSTLEN_IPV4) == 0;
    }
    if (addr1->type == MOBILE_ADDRTYPE_IPV6) {
        const struct mobile_addr6 *a1 = (const struct mobile_addr6 *)addr1;
        const struct mobile_addr6 *a2 = (const struct mobile_addr6 *)addr2;
        return a1->port == a2->port &&
            memcmp(a1->host, a2->host, MOBILE_HOSTLEN_IPV6) == 0;
    }
    return false;
}

// Converts a string of 12 characters to a binary representation for an IPv4
//   address. It also checks for the validity of the address while doing so.
// The output will be a buffer of 4 bytes, representing the address.
bool mobile_parse_phoneaddr(unsigned char *address, const char *data)
{
    const char *cur_data = data;
    unsigned char *cur_addr = address;
    for (unsigned y = 0; y < 4; y++) {
        unsigned cur_num = 0;
        for (unsigned x = 0; x < 3; x++) {
            if (*cur_data < '0' || *cur_data > '9') return false;
            cur_num *= 10;
            cur_num += *cur_data++ - '0';
        }
        if (cur_num > 255) return false;
        *cur_addr++ = cur_num;
    }
    return true;
}

// Check if a string is an IP address, or a dns address.
bool mobile_is_ipaddr(const char *str, unsigned length)
{
    // If there's a colon, it's definitely not a DNS address, it's ipv6
    for (const char *c = str; c < str + length; c++) {
        if (*c == ':') return true;
    }
    // If there's only numbers and periods, it's an ipv4 address
    // This is also a valid DNS address, but blame the mobile adapter devs.
    for (const char *c = str; c < str + length; c++) {
        if ((*c < '0' || *c > '9') && *c != '.') return false;
    }
    return true;
}
