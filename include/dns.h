#ifndef __DNS_H
#define __DNS_H

#include <stdint.h>
#include <stdbool.h>

// Resolve `host` to an IPv4 address (host byte order) via a DNS A-record query
// to the server from DHCP (net_dns_server). A dotted-quad literal is returned
// directly without a query. Waits up to `timeout_ms`. Exposed to Lua as
// net.resolve().
bool net_resolve(const char* host, uint32_t timeout_ms, uint32_t* out_ip);

#endif
