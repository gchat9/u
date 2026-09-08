#ifndef _SYS_LIB_NET_H
#define _SYS_LIB_NET_H

#include <stdint.h>  // or define uint8_t etc. yourself

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define POLLIN      1

typedef uint32_t in_addr_t;
typedef uint32_t socklen_t;

struct in_addr {
    uint32_t s_addr;
};

typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    uint16_t       sin_family;
    uint16_t       sin_port;
    struct in_addr sin_addr;
    uint8_t        sin_zero[8];
};

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define htons(x) __builtin_bswap16(x)
#define htonl(x) __builtin_bswap32(x)
#else
#define htons(x) (x)
#define htonl(x) (x)
#endif

#define ipv4(a,b,c,d) (((uint32_t)(a) << 24) | \
                       ((uint32_t)(b) << 16) | \
                       ((uint32_t)(c) <<  8) | \
                       ((uint32_t)(d)))

#ifdef EXPORT_IMPLEMENTATIONS
static inline struct sockaddr_in ipv4_with_port(
  uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(ipv4(a, b, c, d))
    };
    return addr;
}
#endif

#endif // _SYS_LIB_NET_H
