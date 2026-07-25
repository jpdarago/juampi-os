#ifndef __DHCP_H
#define __DHCP_H

#include <stdint.h>
#include <stdbool.h>

// DHCP client. net_dhcp() runs one DORA exchange (DISCOVER/OFFER/REQUEST/ACK)
// over UDP 68<->67, waiting up to `timeout_ms`. On success it installs the
// lease via net_config(), records the offered DNS server, and returns true.
// Called from net_init(); also exposed to Lua as net.dhcp().
bool net_dhcp(uint32_t timeout_ms);

// The DNS server learned from DHCP (host byte order), or QEMU's user-mode
// forwarder (10.0.2.3) as a fallback when none was offered.
uint32_t net_dns_server(void);

#endif
