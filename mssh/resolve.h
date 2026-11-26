#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The IPv4 addresses of a host name, without the resolver of the C library
 * (which allocates, and is most of getaddrinfo()).  A name is, in this order:
 *
 *   - four decimal numbers with dots: that address;
 *   - a name in /etc/hosts (the lines with an IPv4 address);
 *   - looked up with one query, of type A, to the first nameserver of
 *     /etc/resolv.conf that has an IPv4 address (127.0.0.1 if there is none).
 *
 * What it does not do: IPv6 (not as an address, not as a nameserver), the
 * search domains and options of resolv.conf, a second nameserver, TCP (a reply
 * that is cut is an error), CNAMEs that it would have to ask for (those the
 * nameserver follows, and puts in its reply, are followed).
 */
#define RES_MAX_ADDRS 8

enum {                      /* what resolve4() returns, when it is less than 1 */
    RES_BADNAME  = -1,      /* not a name: an empty label, a label over 63 bytes, ... */
    RES_IPV6     = -2,      /* an IPv6 address (it has a ':') */
    RES_NOTFOUND = -3,      /* the nameserver knows no such host, or none with an address of type A */
    RES_TIMEOUT  = -4,      /* no answer in the time that there was */
    RES_SERVFAIL = -5,      /* the nameserver's answer is an error, or is not to be understood */
    RES_SYSERR   = -6,      /* a system call failed: errno has the reason */
    RES_IGNORE   = -100     /* (dns_answer() only) the packet is not the answer to this query */
};

/* Up to max addresses (at most RES_MAX_ADDRS is sensible) into addr, and how many;
 * or a negative RES_*.  budget_ms is the time there is, for all of it: 0 is none
 * given, which is up to 3 times 2 seconds. */
int resolve4(const char *name, uint8_t addr[][4], int max, int budget_ms);

/* A short text for a negative return, to follow "cannot resolve host: ". */
const char *resolve_strerror(int code);

/* The parts, for the tests (and for resolve4()): */

/* The query for name, in buf, which has DNS_QUERY_MAX bytes: its length, or 0 if name is not one. */
#define DNS_QUERY_MAX (12 + 255 + 4)
size_t dns_query(uint8_t *buf, const char *name, uint16_t id);

/* What a packet says to a query: the number of addresses of type A that are in it (up to
 * max, into addr; 0 is "no such record"), or RES_NOTFOUND, RES_SERVFAIL or RES_IGNORE. */
int dns_answer(const uint8_t *msg, size_t len, const uint8_t *query, size_t qlen,
               uint8_t addr[][4], int max);

/* resolve4() with the files, and the port of the nameserver, as parameters. */
int resolve4_with(const char *name, uint8_t addr[][4], int max, int budget_ms,
                  const char *hosts_path, const char *resolv_path, int port);
