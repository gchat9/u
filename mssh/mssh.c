/*
 * mssh.c — minimal SSH-2 client: the protocol, and the flow of a session
 *
 * The terminal side -- the user's own tty, or an X11 window -- is not here:
 * it is the tty_* functions declared in mssh.h, implemented by tty.c next to
 * this file for the plain client, and by ../xmssh/tty.c for the one with an
 * xterm emulator window of its own.
 *
 * KEX:     curve25519-sha256  (RFC 8731)
 * HostKey: ssh-ed25519        (RFC 8709)
 * Cipher:  chacha20-poly1305@openssh.com
 * Auth:    publickey, ed25519 only, unencrypted key files
 *
 * Dependency: libsodium's API; ../_lib/sodium_lite provides the part used here
 *              (make SODIUM=system builds with the real library instead)
 * Build:      make     (or: cc -O2 -Wall -Wextra -o mssh mssh.c tty.c -lsodium)
 * Usage:      mssh [-i identity] [-p port] [user@]host [command ...]
 *
 * Known MVP gaps:
 *   - No encrypted private key support (bcrypt kdf)
 *   - known_hosts: plain hostname lines only (no hashed entries, no certs)
 *   - No TCP keepalive / SSH keepalive
 *   - Single blocking poll loop (fine; needs ring-buffers for production)
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(__has_include)
# if __has_include(<sys/prctl.h>)
#  include <sys/prctl.h>
#  define HAVE_PRCTL 1
# endif
#endif
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <strings.h>
#include <sys/wait.h>
#include <sodium.h>

#include "mssh.h"

/* ═══════════════════════ SSH message type constants ══════════════════════ */

#define SSH_MSG_DISCONNECT                 1
#define SSH_MSG_IGNORE                     2
#define SSH_MSG_UNIMPLEMENTED              3
#define SSH_MSG_DEBUG                      4
#define SSH_MSG_SERVICE_REQUEST            5
#define SSH_MSG_SERVICE_ACCEPT             6
#define SSH_MSG_KEXINIT                   20
#define SSH_MSG_NEWKEYS                   21
#define SSH_MSG_KEX_ECDH_INIT             30
#define SSH_MSG_KEX_ECDH_REPLY            31
#define SSH_MSG_USERAUTH_REQUEST          50
#define SSH_MSG_USERAUTH_FAILURE          51
#define SSH_MSG_USERAUTH_SUCCESS          52
#define SSH_MSG_USERAUTH_BANNER           53
#define SSH_MSG_USERAUTH_PK_OK            60
#define SSH_MSG_GLOBAL_REQUEST            80
#define SSH_MSG_REQUEST_SUCCESS           81
#define SSH_MSG_REQUEST_FAILURE           82
#define SSH_MSG_CHANNEL_OPEN              90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE      92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST     93
#define SSH_MSG_CHANNEL_DATA              94
#define SSH_MSG_CHANNEL_EXTENDED_DATA     95
#define SSH_MSG_CHANNEL_EOF               96
#define SSH_MSG_CHANNEL_CLOSE             97
#define SSH_MSG_CHANNEL_REQUEST           98
#define SSH_MSG_CHANNEL_SUCCESS           99
#define SSH_MSG_CHANNEL_FAILURE          100

#define MAX_PKT     35000u       /* largest packet we accept, whole (RFC 4253 6.1 asks for 35000) */
#define CHAN_MAX_PKT 32768u      /* largest CHANNEL_DATA we ask for: a packet carrying that much
                                  * (plus ~9 bytes of header, padding, ...) stays under MAX_PKT */
#define WINDOW_SIZE (1u << 20)   /* 1 MiB advertised receive window */
#define MAX_PRE_BANNER_LINES 1024       /* text lines a server may send before its SSH-2.0 line */
#define MAX_PRE_BANNER_BYTES 65536

/* The handshake time limit (-o ConnectTimeout=N): from the start of the
 * connection until the session begins.  A server that stalls at any point
 * before that -- connect, version, key exchange, authentication -- gets this
 * long before we give up.  The session itself, once running, is not timed.
 * The time the user spends on the host key question does not count. */
#define DEFAULT_CONNECT_TIMEOUT 60              /* seconds; 0: no limit */
static int       g_connect_timeout = DEFAULT_CONNECT_TIMEOUT;
static int       g_hs_active;                   /* the handshake is being timed */
static long long g_hs_deadline_ms;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void hs_arm(void) {
    if (g_connect_timeout <= 0) return;
    g_hs_active = 1;
    g_hs_deadline_ms = now_ms() + (long long)g_connect_timeout * 1000;
}
static void hs_pause(void)  { g_hs_active = 0; }   /* the user is being asked a question */
static void hs_resume(void) { hs_arm(); }
static void hs_done(int fd) {                      /* the session begins: no limit any more */
    g_hs_active = 0;
    struct timeval none = { 0, 0 };                /* and no send limit either (see tcp_connect) */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof none);
}

/* Waits until fd is readable, or the handshake limit is reached (then dies).
 * Called before every read of the connection; a no-op outside the handshake. */
static void wait_readable(int fd) {
    if (!g_hs_active) return;
    for (;;) {
        long long left = g_hs_deadline_ms - now_ms();
        if (left <= 0)
            die("timed out: the server did not finish the handshake within %d seconds"
                " (ConnectTimeout)", g_connect_timeout);
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int r = poll(&p, 1, (int)left);
        if (r > 0) return;
        if (r < 0 && errno != EINTR) die("poll: %s", strerror(errno));
    }
}

/* ═══════════════════════════ Fatal error helper ══════════════════════════ */

/* Discard write() return: for stderr/diagnostic paths where failure
 * is genuinely unrecoverable and we're already in an error handler. */
void noret_write(int fd, const void *b, size_t n)
    { ssize_t r = write(fd, b, n); (void)r; }

/* Messages for the user outside the session (prompts, warnings, fatal errors)
 * and the end of the program are the terminal's: tty_msg(), tty_exit(). */
void err(const char *s) { tty_msg(s); }

void die(const char *fmt, ...) __attribute__((noreturn));
void die(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (n > 0) tty_msg(buf);
    tty_msg("\n");
    tty_exit(1);
}

/* ══════════════════════════ Growable byte buffer ═════════════════════════
 *
 * Buf serves two roles:
 *
 *   Writer  – append with buf_u8 / buf_u32 / buf_bytes / buf_str /
 *              buf_cstr / buf_mpint, then pass .b / .len to the send path.
 *
 *   Reader  – wrap a received blob, advance .pos with rdu8 / rdu32 / rdstr.
 *
 * "replace later" note: these ~80 lines cover all SSH wire types. There is
 * no off-the-shelf C library worth pulling in for this; they are small
 * enough to keep as-is permanently.
 */

typedef struct { uint8_t *b; size_t len, cap, pos; } Buf;

static void buf_init(Buf *b) { b->b = NULL; b->len = b->cap = b->pos = 0; }

static void buf_grow(Buf *b, size_t n) {
    if (b->len + n <= b->cap) return;
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n) nc *= 2;
    if (!(b->b = realloc(b->b, nc))) die("realloc failed");
    b->cap = nc;
}

static void buf_u8(Buf *b, uint8_t v) {
    buf_grow(b, 1); b->b[b->len++] = v;
}
static void buf_bytes(Buf *b, const void *d, size_t n) {
    buf_grow(b, n); memcpy(b->b + b->len, d, n); b->len += n;
}
static void buf_u32(Buf *b, uint32_t v) {
    uint8_t x[4] = { v>>24, (v>>16)&0xff, (v>>8)&0xff, v&0xff };
    buf_bytes(b, x, 4);
}
/* SSH "string": uint32 length prefix followed by raw bytes */
static void buf_str(Buf *b, const void *d, size_t n) {
    buf_u32(b, (uint32_t)n); buf_bytes(b, d, n);
}
static void buf_cstr(Buf *b, const char *s) { buf_str(b, s, strlen(s)); }

/*
 * SSH "mpint": big-endian positive integer, leading zeros stripped,
 * with a 0x00 prefix byte added when the high bit of the first remaining
 * byte is set (so the value is not misread as negative).
 *
 * Getting this right for K (the DH shared secret from curve25519) is the
 * single most common source of "MAC mismatch with no useful error" bugs.
 */
static void buf_mpint(Buf *b, const uint8_t *p, size_t n) {
    while (n > 1 && p[0] == 0) { p++; n--; }           /* strip leading zeros */
    if (n == 1 && p[0] == 0) { buf_u32(b, 0); return; }/* encode zero */
    int pad = (p[0] & 0x80) ? 1 : 0;
    buf_u32(b, (uint32_t)(n + pad));
    if (pad) buf_u8(b, 0);
    buf_bytes(b, p, n);
}

/* Buffers carry K (mpint), the data to sign, and so on: wipe what was used. */
static void buf_free(Buf *b) { if (b->b) sodium_memzero(b->b, b->len); free(b->b); buf_init(b); }

/* Reader helpers — exit on underflow (MVP: protocol errors are fatal) */
/* "n > len - pos", never "pos + n > len": pos <= len always holds, and the
 * sum can wrap where size_t is 32 bits (i386, armhf) when n is a hostile
 * 32-bit length from the wire. */
static uint8_t rdu8(Buf *b) {
    if (b->len - b->pos < 1) die("buf underflow");
    return b->b[b->pos++];
}
static uint32_t rdu32(Buf *b) {
    if (b->len - b->pos < 4) die("buf underflow");
    uint32_t v = ((uint32_t)b->b[b->pos  ] << 24)
               | ((uint32_t)b->b[b->pos+1] << 16)
               | ((uint32_t)b->b[b->pos+2] <<  8)
               |  (uint32_t)b->b[b->pos+3];
    b->pos += 4; return v;
}
/* Zero-copy: returns a pointer into b's backing store, not a copy. */
static const uint8_t *rdstr(Buf *b, size_t *olen) {
    uint32_t n = rdu32(b);
    if (n > b->len - b->pos) die("buf underflow (str len %u)", n);
    const uint8_t *p = b->b + b->pos; b->pos += n;
    if (olen) *olen = n;
    return p;
}

/* ══════════════════════════════ Base-64 ══════════════════════════════════
 * "replace later" note: ~50 lines, trivially self-contained.
 * Used for OpenSSH PEM key files and known_hosts entries.
 */

static const char B64C[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64enc(const uint8_t *in, size_t n, char *out) {
    size_t j = 0;
    for (size_t i = 0; i + 2 < n; i += 3) {
        out[j++] = B64C[ in[i]   >> 2];
        out[j++] = B64C[((in[i]   & 3)    << 4) | (in[i+1] >> 4)];
        out[j++] = B64C[((in[i+1] & 0x0f) << 2) | (in[i+2] >> 6)];
        out[j++] = B64C[ in[i+2] & 0x3f];
    }
    size_t r = n % 3;
    if (r) {
        out[j++] = B64C[in[n-r] >> 2];
        if (r == 2) {
            out[j++] = B64C[((in[n-2] & 3) << 4) | (in[n-1] >> 4)];
            out[j++] = B64C[ (in[n-1] & 0x0f) << 2];
        } else {
            out[j++] = B64C[(in[n-1] & 3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0'; return j;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes into out[outmax]; -1 on a bad character or if the result would not
 * fit.  (acc is unsigned: shifting a signed int left until it overflows is
 * undefined; only the low 13 bits are ever used.) */
static ssize_t b64dec(const char *in, size_t n, uint8_t *out, size_t outmax) {
    size_t oi = 0; int bits = 0, v; uint32_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '=' || in[i] == '\n' || in[i] == '\r') continue;
        if ((v = b64val(in[i])) < 0) return -1;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) {
            if (oi >= outmax) return -1;
            bits -= 8; out[oi++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    return (ssize_t)oi;
}

/* ══════════════════════ OpenSSH ed25519 key loading ══════════════════════
 *
 * Parses "-----BEGIN OPENSSH PRIVATE KEY-----" (openssh-key-v1 format).
 * Only unencrypted ed25519 keys are supported.
 *
 * Wire layout after base64-decode:
 *   "openssh-key-v1\0"   15-byte magic
 *   string  ciphername   ("none")
 *   string  kdfname      ("none")
 *   string  kdfoptions   ("")
 *   uint32  nkeys        (1)
 *   string  pubkey_blob  (wire-format ed25519 pubkey)
 *   string  private_section
 *     uint32  checkint × 2      must match; random corruption check
 *     string  key_type          "ssh-ed25519"
 *     string  pubkey            32 bytes
 *     string  privkey           64 bytes = seed[32] || pubkey[32]
 *     string  comment
 *     byte[]  padding           1,2,3,... up to block boundary
 */
static void load_ed25519_key(const char *path,
                              uint8_t sk[crypto_sign_SECRETKEYBYTES],
                              uint8_t pk[crypto_sign_PUBLICKEYBYTES])
{
    FILE *f = fopen(path, "r");
    if (!f) die("%s: %s", path, strerror(errno));
    /* As OpenSSH: a key anyone else can read is no longer a secret. */
    struct stat kst;
    if (fstat(fileno(f), &kst) == 0 && (kst.st_mode & 077))
        die("%s: unprotected private key file (mode %03o): remove group and other access (chmod 600)",
            path, (unsigned)(kst.st_mode & 0777));

    /* Every copy of the key we make is wiped below; that includes stdio's
     * buffer, which would otherwise be freed by fclose() with the key in it. */
    char iobuf[4096];
    setvbuf(f, iobuf, _IOFBF, sizeof iobuf);

    char b64buf[16384]; size_t b64len = 0; int in_key = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "BEGIN OPENSSH PRIVATE KEY")) { in_key = 1; continue; }
        if (strstr(line, "END OPENSSH PRIVATE KEY"))   { break; }
        if (!in_key) continue;
        size_t l = strlen(line);
        while (l && (line[l-1] == '\n' || line[l-1] == '\r')) l--;
        if (b64len + l >= sizeof(b64buf)) die("key file too large");
        memcpy(b64buf + b64len, line, l); b64len += l;
    }
    fclose(f);
    sodium_memzero(iobuf, sizeof iobuf);
    sodium_memzero(line, sizeof line);

    uint8_t raw[8192];
    ssize_t rawlen = b64dec(b64buf, b64len, raw, sizeof raw);
    sodium_memzero(b64buf, sizeof b64buf);
    if (rawlen < 15) die("key too short, too long or bad base64");
    if (memcmp(raw, "openssh-key-v1", 14) != 0 || raw[14] != 0)
        die("bad openssh-key-v1 magic");

    Buf b = { raw, (size_t)rawlen, (size_t)rawlen, 15 };

    size_t slen;
    const uint8_t *cipher = rdstr(&b, &slen);
    if (slen != 4 || memcmp(cipher, "none", 4) != 0)
        die("encrypted private keys not supported (decrypt with ssh-keygen first)");
    rdstr(&b, NULL); rdstr(&b, NULL);  /* kdfname, kdfoptions */
    if (rdu32(&b) != 1) die("expected exactly 1 key in file");
    rdstr(&b, NULL);  /* pubkey blob — we get pubkey from the private section */

    size_t privlen;
    const uint8_t *priv = rdstr(&b, &privlen);
    Buf pb = { (uint8_t *)priv, privlen, privlen, 0 };

    uint32_t c1 = rdu32(&pb), c2 = rdu32(&pb);
    if (c1 != c2) die("checkint mismatch — key file may be corrupt");

    const uint8_t *kt = rdstr(&pb, &slen);
    if (slen != 11 || memcmp(kt, "ssh-ed25519", 11) != 0)
        die("not an ed25519 key");
    rdstr(&pb, NULL);  /* pubkey (32 bytes) — repeated below in privkey */

    /*
     * privkey is 64 bytes: seed[0..31] || pubkey[32..63]
     * This is exactly the layout libsodium uses for crypto_sign_SECRETKEYBYTES.
     */
    const uint8_t *privkey = rdstr(&pb, &slen);
    if (slen != 64) die("unexpected ed25519 privkey length %zu", slen);
    memcpy(sk, privkey, 64);
    memcpy(pk, privkey + 32, 32);
    sodium_memzero(raw, sizeof raw);          /* the decoded file: seed, comment, ... */
}

/* ════════════════════════════ TCP connect ════════════════════════════════ */

static int tcp_connect(const char *host, const char *port) {
    struct addrinfo hints = {0}, *res, *r;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc) die("getaddrinfo: %s", gai_strerror(rc));
    int fd = -1, why = 0;
    for (r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (g_connect_timeout > 0) {          /* a blocking connect() honours SO_SNDTIMEO on Linux */
            struct timeval tv = { g_connect_timeout, 0 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        }
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        why = errno;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) die("connect to %s:%s failed: %s", host, port, strerror(why));
    int one = 1;                                   /* keystrokes must not wait for an ACK */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

/* ═══════════════════════════ Names from the command line ═════════════════
 *
 * The host and the port are expanded into a ProxyCommand (%h, %p) that runs
 * under sh -c, so anything else in them would be a shell command: only the
 * characters of host names, and digits for the port, are accepted (as OpenSSH
 * does).  IPv6 zone identifiers (fe80::1%eth0) are therefore refused.
 */
static int valid_host(const char *h) {
    size_t n = strlen(h);
    if (n == 0 || n > 255 || h[0] == '-') return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)h[i]) && !strchr("-._:", h[i])) return 0;
    return 1;
}
static int valid_port(const char *p) {
    size_t n = strlen(p);
    if (n == 0 || n > 5) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isdigit((unsigned char)p[i])) return 0;
    return 1;
}

/* ════════════════════════════ Proxy connect ═════════════════════════════
 *
 * Runs the given shell command and returns a single bidirectional fd
 * connected to its stdin/stdout.  SSH never knows it's not a TCP socket.
 *
 * Mechanism: socketpair(AF_UNIX) gives us two connected endpoints.  The
 * child gets sv[1] as both stdin and stdout, then execs the command via
 * "sh -c".  The parent closes sv[1] and uses sv[0] as the transport fd.
 * This is the standard ProxyCommand implementation strategy.
 *
 * %h and %p in the command string are expanded to host and port before
 * exec, matching OpenSSH behaviour.
 */
static int proxy_connect(const char *cmd, const char *host, const char *port) {
    /* Expand %h → host, %p → port in cmd */
    char expanded[4096]; size_t ei = 0;
    for (size_t i = 0; cmd[i]; i++) {
        char lit[3] = { cmd[i], 0, 0 };
        const char *sub = lit;
        if (cmd[i] == '%' && cmd[i+1]) {
            i++;
            if      (cmd[i] == 'h') sub = host;
            else if (cmd[i] == 'p') sub = port;
            else    lit[1] = cmd[i];     /* unknown token: pass through literally */
        }
        /* One bounds check for every kind of piece, leaving room for the NUL;
         * a command that does not fit is an error, not a truncated command. */
        size_t sl = strlen(sub);
        if (sl >= sizeof(expanded) - ei) die("ProxyCommand expansion too long");
        memcpy(expanded + ei, sub, sl); ei += sl;
    }
    expanded[ei] = '\0';

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        die("socketpair: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) die("fork: %s", strerror(errno));

    if (pid == 0) {
        /* Child: wire sv[1] to stdin and stdout, then exec */
        close(sv[0]);
        if (dup2(sv[1], STDIN_FILENO)  < 0) _exit(127);
        if (dup2(sv[1], STDOUT_FILENO) < 0) _exit(127);
        if (sv[1] != STDIN_FILENO && sv[1] != STDOUT_FILENO) close(sv[1]);
        execl("/bin/sh", "sh", "-c", expanded, (char *)NULL);
        _exit(127);
    }

    /* Parent: close child's end, return our end */
    close(sv[1]);
    return sv[0];
}

/* ═══════════════════════════ Connection state ════════════════════════════ */

/* The connection state (struct SSH) is in mssh.h, which the terminal front-ends share. */

/* ════════════════════════════ I/O primitives ═════════════════════════════ */

static void xread(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n > 0) {
        wait_readable(fd);
        ssize_t r = read(fd, p, n);
        if (r < 0) die("read: %s", strerror(errno));
        if (r == 0) die("connection closed by remote");
        p += r; n -= r;
    }
}
void xwrite(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) die("write: %s", strerror(errno));
        p += r; n -= r;
    }
}

/* Write to the connection.  send() with MSG_NOSIGNAL, so that a peer that
 * went away is an error message ("write: Broken pipe") and not SIGPIPE, which
 * killed the process before the terminal could be restored. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0x4000          /* the same on every Linux architecture here */
#endif
static void xsend(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) die("write: %s", strerror(errno));
        p += r; n -= (size_t)r;
    }
}

/* ═══════════════════════════ Packet layer ════════════════════════════════
 *
 * Plaintext (before NEWKEYS):
 *   uint32  packet_length  = 1 + payload_len + padding_len
 *   uint8   padding_length
 *   byte[]  payload
 *   byte[]  random_padding (to next 8-byte boundary, min 4 bytes)
 *
 * chacha20-poly1305@openssh.com (after NEWKEYS):
 *   enc_len[4]   ChaCha20(K_1, nonce=seqno, ic=0)  XOR  packet_length
 *   enc_body[N]  ChaCha20(K_2, nonce=seqno, ic=1)  XOR  (pad_len || payload || padding)
 *   mac[16]      Poly1305( key = ChaCha20(K_2, nonce=seqno, ic=0)[0..31],
 *                          msg = enc_len || enc_body )
 *
 * The MAC covers the *encrypted* bytes. Sequence numbers are implicit in
 * the ChaCha20 nonce and are not transmitted on the wire.
 */

/* Encode the 32-bit sequence number as a big-endian 64-bit ChaCha20 nonce */
static void make_nonce(uint8_t n[8], uint32_t seq) {
    n[0] = n[1] = n[2] = n[3] = 0;
    n[4] = (seq >> 24) & 0xff; n[5] = (seq >> 16) & 0xff;
    n[6] = (seq >>  8) & 0xff; n[7] =  seq        & 0xff;
}

static void pkt_send_plain(SSH *ssh, const uint8_t *pay, size_t plen) {
    uint8_t pad = (uint8_t)(8 - ((5 + plen) % 8));
    if (pad < 4) pad += 8;
    uint32_t pl = (uint32_t)(1 + plen + pad);
    /* One send() per packet: see pkt_send_enc. */
    uint8_t *out = malloc(4 + (size_t)pl); if (!out) die("malloc");
    out[0] = pl>>24; out[1] = (pl>>16)&0xff; out[2] = (pl>>8)&0xff; out[3] = pl&0xff;
    out[4] = pad;
    memcpy(out + 5, pay, plen);
    memset(out + 5 + plen, 0, pad);
    xsend(ssh->fd, out, 4 + (size_t)pl);
    free(out);
    ssh->send_seq++;
}

static uint8_t *pkt_recv_plain(SSH *ssh, size_t *plen) {
    uint8_t hdr[5]; xread(ssh->fd, hdr, 5);
    uint32_t pl = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                | ((uint32_t)hdr[2] <<  8) |  (uint32_t)hdr[3];
    uint8_t pad = hdr[4];
    if (!pl || pl > MAX_PKT) die("bad plaintext packet length %u", pl);
    /* RFC 4253 6: padding is at least 4 bytes and must fit in the packet.
     * Without this, pl - 1 - pad wraps to ~4 GiB (the server is not yet
     * authenticated here, so this is attacker-controlled). */
    if (pad < 4 || (uint32_t)pad > pl - 1) die("bad padding length %u in packet of %u", pad, pl);
    uint8_t *tmp = malloc(pl - 1);
    if (!tmp) die("malloc");
    xread(ssh->fd, tmp, pl - 1);
    *plen = (size_t)(pl - 1 - pad);
    uint8_t *r = malloc(*plen ? *plen : 1);
    if (!r) die("malloc");
    memcpy(r, tmp, *plen); free(tmp);
    ssh->recv_seq++;
    return r;
}

static void pkt_send_enc(SSH *ssh, const uint8_t *pay, size_t plen) {
    /* The ChaCha20 nonce is the 32-bit sequence number: once it wraps, the
     * keystream and the Poly1305 key of an earlier packet are reused.  Refuse
     * instead; a rekey (or a new connection) is needed long before this. */
    if (ssh->send_seq == UINT32_MAX) die("send sequence number exhausted: reconnect");
    uint8_t pad = (uint8_t)(8 - ((1 + plen) % 8));
    if (pad < 4) pad += 8;
    uint32_t pl = (uint32_t)(1 + plen + pad);

    uint8_t nonce[8]; make_nonce(nonce, ssh->send_seq);

    /* Encrypt length field with K_header (key[32..63]), ic=0 */
    uint8_t elen[4] = { pl>>24, (pl>>16)&0xff, (pl>>8)&0xff, pl&0xff };
    crypto_stream_chacha20_xor_ic(elen, elen, 4, nonce, 0, ssh->send_key + 32);

    /* The whole packet - length, body, MAC - is built in one buffer and sent
     * with one send().  Three small writes (as before) interact badly with
     * Nagle's algorithm and the peer's delayed ACKs: in the re-key exchange,
     * which is a handful of small request/response packets, each costs
     * ~40-50 ms, and a re-key took most of a second. */
    uint8_t *out = malloc(4 + (size_t)pl + 16); if (!out) die("malloc");
    uint8_t *cbody = out + 4;
    memcpy(out, elen, 4);

    /* Build plaintext body (in the output buffer; encrypted in place below) */
    cbody[0] = pad;
    memcpy(cbody + 1, pay, plen);
    randombytes_buf(cbody + 1 + plen, pad);

    /* Derive Poly1305 key: first 32 bytes of ChaCha20(K_main (key[0..31]), ic=0) */
    uint8_t poly_block[64] = {0};
    crypto_stream_chacha20_xor_ic(poly_block, poly_block, 64, nonce, 0,
                                   ssh->send_key);

    /* Encrypt body with K_main (key[0..31]), ic=1 */
    crypto_stream_chacha20_xor_ic(cbody, cbody, pl, nonce, 1,
                                   ssh->send_key);

    /* MAC over enc_len || enc_body */
    crypto_onetimeauth_poly1305_state mst;
    crypto_onetimeauth_poly1305_init(&mst, poly_block);
    crypto_onetimeauth_poly1305_update(&mst, out, 4 + (size_t)pl);
    crypto_onetimeauth_poly1305_final(&mst, out + 4 + pl);
    sodium_memzero(poly_block, sizeof poly_block);

    xsend(ssh->fd, out, 4 + (size_t)pl + 16);
    free(out);
    ssh->send_seq++;
}

static uint8_t *pkt_recv_enc(SSH *ssh, size_t *plen) {
    uint8_t nonce[8]; make_nonce(nonce, ssh->recv_seq);

    /* Decrypt 4-byte length with K_header (key[32..63]), ic=0 */
    uint8_t elen[4]; xread(ssh->fd, elen, 4);
    uint8_t dlen[4]; memcpy(dlen, elen, 4);
    crypto_stream_chacha20_xor_ic(dlen, dlen, 4, nonce, 0, ssh->recv_key + 32);
    uint32_t pl = ((uint32_t)dlen[0] << 24) | ((uint32_t)dlen[1] << 16)
                | ((uint32_t)dlen[2] <<  8) |  (uint32_t)dlen[3];
    if (!pl || pl > MAX_PKT) die("bad encrypted packet length %u", pl);

    uint8_t *cbody = malloc(pl); if (!cbody) die("malloc");
    xread(ssh->fd, cbody, pl);
    uint8_t mac[16]; xread(ssh->fd, mac, 16);

    /* Derive Poly1305 key: first 32 bytes of ChaCha20(K_main (key[0..31]), ic=0) */
    uint8_t poly_block[64] = {0};
    crypto_stream_chacha20_xor_ic(poly_block, poly_block, 64, nonce, 0,
                                   ssh->recv_key);

    /* Verify MAC *before* decrypting — fail closed */
    uint8_t cmac[16];
    crypto_onetimeauth_poly1305_state mst;
    crypto_onetimeauth_poly1305_init(&mst, poly_block);
    crypto_onetimeauth_poly1305_update(&mst, elen, 4);
    crypto_onetimeauth_poly1305_update(&mst, cbody, pl);
    crypto_onetimeauth_poly1305_final(&mst, cmac);
    if (sodium_memcmp(cmac, mac, 16) != 0)
        die("MAC verification FAILED — sequence number drift or tampering");

    /* Decrypt body with K_main (key[0..31]), ic=1 */
    uint8_t *plain = malloc(pl); if (!plain) die("malloc");
    crypto_stream_chacha20_xor_ic(plain, cbody, pl, nonce, 1,
                                   ssh->recv_key);
    free(cbody);

    uint8_t pad = plain[0];
    if (pad < 4 || (uint32_t)pad > pl - 1) die("bad padding length in decrypted packet");
    *plen = (size_t)(pl - 1 - pad);
    uint8_t *r = malloc(*plen ? *plen : 1); if (!r) die("malloc");
    memcpy(r, plain + 1, *plen); free(plain);
    ssh->recv_seq++;
    return r;
}

static void pkt_send(SSH *ssh, const uint8_t *pay, size_t plen) {
    if (ssh->send_enc) pkt_send_enc(ssh, pay, plen);
    else               pkt_send_plain(ssh, pay, plen);
}

/*
 * Text from the server that we show the user (disconnect reasons, channel
 * open failures, ...): copied into dst, NUL-terminated, with everything but
 * printable ASCII replaced by '?'.  Before authentication the server is
 * not trusted, and an escape sequence in a message would be interpreted by
 * the user's terminal (or by xmssh's emulator): title changes, screen
 * rewriting that fakes a prompt, and so on.
 */
static void server_text(char *dst, size_t dstsz, const uint8_t *src, size_t n) {
    if (n >= dstsz) n = dstsz - 1;
    for (size_t i = 0; i < n; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '?';
    dst[n] = '\0';
}

/* Skip IGNORE/DEBUG/GLOBAL_REQUEST; treat DISCONNECT as fatal */
static uint8_t *pkt_recv(SSH *ssh, size_t *plen) {
    for (;;) {
        uint8_t *p;
        if (ssh->recv_enc) p = pkt_recv_enc(ssh, plen);
        else               p = pkt_recv_plain(ssh, plen);
        /* Strict KEX: until NEWKEYS, only key exchange messages may arrive.
         * Quietly skipping an IGNORE here (or an empty packet) is what lets
         * a man in the middle shift the sequence numbers (Terrapin,
         * CVE-2023-48795). */
        if (ssh->strict_kex && !ssh->recv_enc) {
            if (!*plen || p[0] == SSH_MSG_IGNORE || p[0] == SSH_MSG_DEBUG
                       || p[0] == SSH_MSG_GLOBAL_REQUEST)
                die("strict KEX violation: unexpected packet during key exchange");
        }
        if (!*plen) { free(p); continue; }
        switch (p[0]) {
            case SSH_MSG_IGNORE: case SSH_MSG_DEBUG:
                free(p); continue;
            case SSH_MSG_DISCONNECT: {
                Buf b = { p, *plen, *plen, 1 };
                rdu32(&b); /* reason code */
                size_t ml; const uint8_t *msg = rdstr(&b, &ml);
                char dmsg[256];
                server_text(dmsg, sizeof dmsg, msg, ml);
                die("Server disconnected: %s", dmsg);
            }
            case SSH_MSG_GLOBAL_REQUEST: {
                /*
                 * Servers send these unsolicited (e.g. hostkeys-00@openssh.com).
                 * We don't act on any of them; reply REQUEST_FAILURE when asked.
                 */
                if (*plen >= 2) {
                    Buf b = { p, *plen, *plen, 1 };
                    rdstr(&b, NULL);          /* request name */
                    uint8_t want_reply = rdu8(&b);
                    if (want_reply) {
                        uint8_t fail = SSH_MSG_REQUEST_FAILURE;
                        pkt_send(ssh, &fail, 1);
                    }
                }
                free(p); continue;
            }
            default: return p;
        }
    }
}

/* Convenience: build payload with Buf and send it */
static void send_buf(SSH *ssh, Buf *b) {
    pkt_send(ssh, b->b, b->len);
    buf_free(b);
}

/* ══════════════════════ Key derivation (RFC 4253 §7.2) ═══════════════════
 *
 * key = HASH(K || H || letter || session_id)
 *   then extend with HASH(K || H || key_so_far) if more bytes are needed.
 *
 * K here is the SSH-mpint-encoded shared secret (with the length prefix and
 * the possible leading 0x00 byte already included).
 */
static void derive_key(const uint8_t *K_mpint, size_t K_mpint_len,
                        const uint8_t *H,       /* 32-byte exchange hash */
                        char           letter,
                        const uint8_t *sid,     /* 32-byte session id    */
                        uint8_t       *out,
                        size_t         need)
{
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, K_mpint, K_mpint_len);
    crypto_hash_sha256_update(&st, H, 32);
    uint8_t let = (uint8_t)letter;
    crypto_hash_sha256_update(&st, &let, 1);
    crypto_hash_sha256_update(&st, sid, 32);
    uint8_t blk[32]; crypto_hash_sha256_final(&st, blk);

    size_t done = need < 32 ? need : 32;
    memcpy(out, blk, done);

    while (done < need) {
        crypto_hash_sha256_init(&st);
        crypto_hash_sha256_update(&st, K_mpint, K_mpint_len);
        crypto_hash_sha256_update(&st, H, 32);
        crypto_hash_sha256_update(&st, out, done);
        crypto_hash_sha256_final(&st, blk);
        size_t cp = (need - done) < 32 ? (need - done) : 32;
        memcpy(out + done, blk, cp); done += cp;
    }
    sodium_memzero(blk, sizeof blk);
    sodium_memzero(&st, sizeof st);
}

/* ═══════════════════════════ Known hosts ═════════════════════════════════
 *
 * MVP format: plain lines of the form
 *   hostname ssh-ed25519 BASE64_PUBKEY
 *
 * "replace later" notes:
 *   - No hashed hostname support (|1|...)
 *   - No [host]:port notation when port != 22
 *   - Appends new entries rather than doing in-place update
 */

/* NULL  → use ~/.ssh/known_hosts
 * non-NULL → use that path (set by -o UserKnownHostsFile=...) */
static const char *g_known_hosts_file = NULL;
static const char *g_proxy_command     = NULL;

static char *known_hosts_path(void) {
    if (g_known_hosts_file) return strdup(g_known_hosts_file);
    const char *home = getenv("HOME");
    if (!home) die("$HOME not set");
    char *p = malloc(strlen(home) + 32);
    if (!p) die("malloc");
    strcpy(p, home); strcat(p, "/.ssh/known_hosts");
    return p;
}

/* Reads one SSH string at *pos of a blob; 0 if it does not fit (a malformed
 * entry is skipped, not fatal).  *p is set to the bytes if p is not NULL. */
static int wire_str(const uint8_t *b, size_t blen, size_t *pos, size_t *n, const uint8_t **p) {
    if (blen - *pos < 4) return 0;
    uint32_t l = ((uint32_t)b[*pos] << 24) | ((uint32_t)b[*pos + 1] << 16)
               | ((uint32_t)b[*pos + 2] << 8) | (uint32_t)b[*pos + 3];
    *pos += 4;
    if (l > blen - *pos) return 0;
    if (p) *p = b + *pos;
    *n = l; *pos += l;
    return 1;
}

/* Returns 1 if found and matches, 0 if not found, -1 if found but mismatch */
static int kh_check(const char *host, const uint8_t *pubkey /* 32 bytes */) {
    char *path = known_hosts_path();
    FILE *f = fopen(path, "r");
    /* Only a file that does not exist means that no key is known.  Any other
     * failure (permissions, a directory, an I/O error) must not be taken for
     * "unknown host": that would ask the user about a host whose key is in the
     * file, and accept the answer. */
    if (!f) {
        if (errno == ENOENT) { free(path); return 0; }
        die("%s: cannot open: %s", path, strerror(errno));
    }
    free(path);

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *sp1 = strchr(line, ' '); if (!sp1) continue;
        *sp1 = '\0'; const char *h = line;
        char *sp2 = strchr(sp1+1, ' '); if (!sp2) continue;
        /* h = hostname, sp1+1 = key type, sp2+1 = base64 key blob */
        char *type = sp1+1; *sp2 = '\0';
        char *b64  = sp2+1;
        /* strip trailing newline */
        size_t bl = strlen(b64);
        while (bl && (b64[bl-1] == '\n' || b64[bl-1] == '\r')) b64[--bl] = '\0';

        if (strcmp(h, host) != 0) continue;
        if (strcmp(type, "ssh-ed25519") != 0) continue;

        uint8_t blob[256]; ssize_t blen = b64dec(b64, strlen(b64), blob, sizeof blob);
        if (blen < 0) continue;
        /* blob is the SSH wire format: string("ssh-ed25519") || string(32-byte key) */
        size_t pos = 0, tl, kl; const uint8_t *k;
        if (!wire_str(blob, (size_t)blen, &pos, &tl, NULL)) continue;  /* type */
        if (!wire_str(blob, (size_t)blen, &pos, &kl, &k)) continue;    /* key */
        if (kl != 32) continue;

        fclose(f);
        return (sodium_memcmp(k, pubkey, 32) == 0) ? 1 : -1;
    }
    if (ferror(f)) die("known_hosts: cannot read: %s", strerror(errno));
    fclose(f); return 0;
}

/* The wire form of an ssh-ed25519 public key: string("ssh-ed25519") ||
 * string(pk), 51 bytes.  What is stored and fingerprinted is built from the
 * verified key, never copied from what the server sent. */
#define ED25519_BLOB_LEN 51
static void ed25519_blob(const uint8_t pk[32], uint8_t out[ED25519_BLOB_LEN]) {
    static const uint8_t hdr[19] = { 0,0,0,11, 's','s','h','-','e','d','2','5','5','1','9', 0,0,0,32 };
    memcpy(out, hdr, sizeof hdr);
    memcpy(out + sizeof hdr, pk, 32);
}

static void kh_add(const char *host, const uint8_t pk[32])
{
    char *path = known_hosts_path();
    int fd = open(path, O_RDWR | O_APPEND | O_CREAT, 0600);
    free(path);
    if (fd < 0) { err("known_hosts: append failed\n"); return; }
    /* A file whose last line has no newline (edited by hand, say) would have
     * the new entry glued onto that line, and both would then be unusable. */
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        char last;
        if (pread(fd, &last, 1, st.st_size - 1) == 1 && last != '\n')
            noret_write(fd, "\n", 1);
    }
    uint8_t blob[ED25519_BLOB_LEN]; ed25519_blob(pk, blob);
    char b64[128]; b64enc(blob, sizeof blob, b64);      /* 68 characters */
    noret_write(fd, host, strlen(host));
    noret_write(fd, " ssh-ed25519 ", 13);
    noret_write(fd, b64, strlen(b64));
    noret_write(fd, "\n", 1);
    close(fd);
}

/* ══════════════════════════ Key exchange ═════════════════════════════════
 *
 * curve25519-sha256 (RFC 8731)
 *
 * Message flow:
 *   C→S  SSH_MSG_KEXINIT
 *   S→C  SSH_MSG_KEXINIT
 *   C→S  SSH_MSG_KEX_ECDH_INIT    (client ephemeral pubkey, 32 bytes)
 *   S→C  SSH_MSG_KEX_ECDH_REPLY   (server host key blob, server ephemeral pubkey,
 *                                   signature over exchange hash H)
 *   C→S  SSH_MSG_NEWKEYS
 *   S→C  SSH_MSG_NEWKEYS
 *
 * Exchange hash H = SHA-256 of:
 *   string  client_version_string
 *   string  server_version_string
 *   string  client_KEXINIT_payload
 *   string  server_KEXINIT_payload
 *   string  server_host_key_blob
 *   string  client_ephemeral_pubkey   (32 bytes)
 *   string  server_ephemeral_pubkey   (32 bytes)
 *   mpint   shared_secret K
 */
/*
 * The first key exchange (srv_init == NULL), or a re-key started by the
 * server, whose KEXINIT payload the caller has already read (srv_init; the
 * caller keeps ownership).  OpenSSH asks for one after 1 GiB of traffic, and
 * before this the session simply froze at that point.  A re-key differs in:
 * the server's KEXINIT is not read here; the strict-KEX marker is not sent
 * again; the host key is not asked about but must be the one accepted at
 * the start; the session id stays; and the new keys replace the old ones
 * only after both NEWKEYS, which travel under the old keys.
 */
static void do_kex(SSH *ssh, const char *host, const uint8_t *srv_init, size_t srv_init_len) {
    int rekey = srv_init != NULL;
    /* ── Send our KEXINIT ── */
    Buf ck; buf_init(&ck);
    buf_u8(&ck, SSH_MSG_KEXINIT);
    uint8_t cookie[16]; randombytes_buf(cookie, 16);
    buf_bytes(&ck, cookie, 16);
    /* The 10 name-list fields; we only support one algorithm per slot */
    /* The second name is not an algorithm: it tells the server we know
     * "strict KEX" (OpenSSH's countermeasure to the Terrapin attack). */
    buf_cstr(&ck, rekey ? "curve25519-sha256"
                        : "curve25519-sha256,kex-strict-c-v00@openssh.com"); /* kex_algorithms */
    buf_cstr(&ck, "ssh-ed25519");                   /* server_host_key_algorithms */
    buf_cstr(&ck, "chacha20-poly1305@openssh.com"); /* encryption c→s */
    buf_cstr(&ck, "chacha20-poly1305@openssh.com"); /* encryption s→c */
    buf_cstr(&ck, "hmac-sha2-256");                 /* mac c→s (unused with AEAD) */
    buf_cstr(&ck, "hmac-sha2-256");                 /* mac s→c (unused with AEAD) */
    buf_cstr(&ck, "none");                          /* compression c→s */
    buf_cstr(&ck, "none");                          /* compression s→c */
    buf_cstr(&ck, "");                              /* languages c→s */
    buf_cstr(&ck, "");                              /* languages s→c */
    buf_u8(&ck, 0);                                 /* first_kex_packet_follows */
    buf_u32(&ck, 0);                                /* reserved */

    /* Save verbatim payload for the exchange hash */
    free(ssh->client_kexinit);                      /* (a re-key replaces the first one's) */
    ssh->client_kexinit = malloc(ck.len);
    if (!ssh->client_kexinit) die("malloc");
    memcpy(ssh->client_kexinit, ck.b, ck.len);
    ssh->client_kexinit_len = ck.len;
    send_buf(ssh, &ck);

    /* ── Receive server KEXINIT ── */
    size_t sk_len; const uint8_t *sk_pkt; uint8_t *sk_own = NULL;
    if (rekey) { sk_pkt = srv_init; sk_len = srv_init_len; }
    else {
        sk_own = pkt_recv(ssh, &sk_len); sk_pkt = sk_own;
        if (sk_pkt[0] != SSH_MSG_KEXINIT) die("expected KEXINIT, got %d", sk_pkt[0]);
    }
    /* The strict-KEX scan below reads the name-lists after the type byte and
     * the 16-byte cookie.  A shorter payload would be read past its end. */
    if (sk_len < 17) die("server KEXINIT too short (%zu bytes)", sk_len);
    free(ssh->server_kexinit);
    ssh->server_kexinit = malloc(sk_len);
    if (!ssh->server_kexinit) die("malloc");
    memcpy(ssh->server_kexinit, sk_pkt, sk_len);
    ssh->server_kexinit_len = sk_len;
    free(sk_own);

    /* Strict KEX is on if the server's kex_algorithms list has its marker.
     * Then its KEXINIT had to be the very first packet (nothing may have
     * been slipped in before it), and the sequence numbers restart at
     * NEWKEYS, below.  A server without it is served as before. */
    if (!rekey) {
        Buf kb = { ssh->server_kexinit, sk_len, sk_len, 17 };   /* type + cookie */
        size_t kl; const uint8_t *kx = rdstr(&kb, &kl);
        static const char mark[] = "kex-strict-s-v00@openssh.com";
        size_t ml = sizeof mark - 1;
        for (size_t i = 0; i + ml <= kl; i++)
            if (memcmp(kx + i, mark, ml) == 0
                && (i == 0 || kx[i-1] == ',') && (i + ml == kl || kx[i+ml] == ','))
                ssh->strict_kex = 1;
        if (ssh->strict_kex && ssh->recv_seq != 1)
            die("strict KEX violation: KEXINIT was not the first packet");
    }
    /* (Algorithm negotiation: we only advertise one of each, so if the
     *  server accepted our KEXINIT at all, we're using our single choice.) */

    /* ── Generate ephemeral curve25519 keypair ── */
    uint8_t eph_pk[32], eph_sk[32];
    crypto_box_keypair(eph_pk, eph_sk);   /* curve25519 DH keypair */

    /* ── Send ECDH_INIT ── */
    Buf ei; buf_init(&ei);
    buf_u8(&ei, SSH_MSG_KEX_ECDH_INIT);
    buf_str(&ei, eph_pk, 32);
    send_buf(ssh, &ei);

    /* ── Receive ECDH_REPLY ── */
    size_t rlen; uint8_t *reply = pkt_recv(ssh, &rlen);
    if (reply[0] != SSH_MSG_KEX_ECDH_REPLY)
        die("expected ECDH_REPLY, got %d", reply[0]);
    Buf rb = { reply, rlen, rlen, 1 };

    size_t hkb_len; const uint8_t *hkb = rdstr(&rb, &hkb_len); /* host key blob */
    size_t srv_eph_len; const uint8_t *srv_eph = rdstr(&rb, &srv_eph_len);
    size_t sig_len;     const uint8_t *sig_blob = rdstr(&rb, &sig_len);

    if (srv_eph_len != 32) die("bad server ephemeral key length");

    /* ── Parse host key blob: string("ssh-ed25519") || string(pubkey) ── */
    Buf hkbuf = { (uint8_t *)hkb, hkb_len, hkb_len, 0 };
    size_t kt_len; const uint8_t *kt = rdstr(&hkbuf, &kt_len);
    if (kt_len != 11 || memcmp(kt, "ssh-ed25519", 11) != 0)
        die("server host key is not ed25519");
    size_t srv_pk_len; const uint8_t *srv_pk = rdstr(&hkbuf, &srv_pk_len);
    if (srv_pk_len != 32) die("bad server ed25519 key length");
    /* Nothing may follow the key: a longer blob is not an ed25519 key. */
    if (hkbuf.pos != hkb_len) die("server host key blob has trailing data");

    /* ── Compute shared secret K = X25519(eph_sk, srv_eph) ── */
    uint8_t K_raw[32];
    if (crypto_scalarmult_curve25519(K_raw, eph_sk, srv_eph) != 0)
        die("curve25519 scalarmult failed (bad server key?)");
    sodium_memzero(eph_sk, sizeof eph_sk);      /* forward secrecy: nothing may keep this */

    /* ── Build exchange hash H ── */
    /*
     * H = SHA256(
     *   string(client_version)    string(server_version)
     *   string(client_KEXINIT)    string(server_KEXINIT)
     *   string(host_key_blob)
     *   string(client_eph_pubkey) string(server_eph_pubkey)
     *   mpint(K)
     * )
     * Every field is length-prefixed per SSH "string" or "mpint" encoding.
     */
    Buf hb; buf_init(&hb);
    buf_cstr(&hb, ssh->client_ver);
    buf_cstr(&hb, ssh->server_ver);
    buf_str(&hb, ssh->client_kexinit, ssh->client_kexinit_len);
    buf_str(&hb, ssh->server_kexinit, ssh->server_kexinit_len);
    buf_str(&hb, hkb, hkb_len);
    buf_str(&hb, eph_pk, 32);
    buf_str(&hb, srv_eph, 32);
    buf_mpint(&hb, K_raw, 32);

    uint8_t H[32]; crypto_hash_sha256(H, hb.b, hb.len);
    buf_free(&hb);

    /* Session ID is set from the *first* exchange hash and never changes */
    if (!ssh->have_session_id) {
        memcpy(ssh->session_id, H, 32);
        ssh->have_session_id = 1;
    }

    /* ── Verify server's signature over H ── */
    /*
     * sig_blob wire format: string("ssh-ed25519") || string(64-byte_sig)
     */
    Buf sb = { (uint8_t *)sig_blob, sig_len, sig_len, 0 };
    size_t st_len; const uint8_t *st = rdstr(&sb, &st_len);
    if (st_len != 11 || memcmp(st, "ssh-ed25519", 11) != 0)
        die("server signature is not ssh-ed25519");   /* the key is ed25519, its signature too */
    size_t raw_sig_len; const uint8_t *raw_sig = rdstr(&sb, &raw_sig_len);
    if (raw_sig_len != 64) die("bad ed25519 signature length");
    if (sb.pos != sig_len) die("trailing data after the server's signature");
    if (crypto_sign_verify_detached(raw_sig, H, 32, srv_pk) != 0)
        die("server host key signature FAILED — aborting");

    /* ── Known-hosts check / TOFU ──
     * Only now, once the server has proved it holds the private key of the
     * host key it showed: before, a man in the middle could make us ask
     * about (and, on "yes", store) a key it cannot even use. */
    if (rekey) {
        if (sodium_memcmp(srv_pk, ssh->host_pk, 32) != 0)
            die("server host key changed during re-key — aborting");
    } else {
        int kh = kh_check(host, srv_pk);
        if (kh == -1) {
            die("HOST KEY MISMATCH for %s — possible man-in-the-middle attack!\n"
                "If you changed the server, edit ~/.ssh/known_hosts.", host);
        }
        if (kh == 0) {
            hs_pause();                        /* waiting for the user does not count */
            /* First time: show fingerprint, ask user */
            uint8_t blob[ED25519_BLOB_LEN], fp[32];
            ed25519_blob(srv_pk, blob);
            crypto_hash_sha256(fp, blob, sizeof blob);
            char fp_b64[64]; size_t fl = b64enc(fp, 32, fp_b64);
            while (fl && fp_b64[fl-1] == '=') fp_b64[--fl] = '\0';   /* as ssh-keygen -l prints it */
            err("The authenticity of host '");
            err(host);
            err("\' can't be established.\n");
            err("ED25519 key fingerprint is SHA256:");
            err(fp_b64);
            err("\nAre you sure you want to continue connecting (yes/no)? ");
            char ans[8];
            ssize_t an = tty_ask(ans, sizeof(ans) - 1);
            while (an > 0 && (ans[an-1] == '\n' || ans[an-1] == '\r')) an--;
            if (an != 3 || memcmp(ans, "yes", 3) != 0) {      /* exactly "yes" */
                err("Host key verification failed.\n");
                tty_exit(1);
            }
            kh_add(host, srv_pk);
            err("Warning: Permanently added '");
            err(host);
            err("\' to known hosts.\n");
            hs_resume();
        }
        memcpy(ssh->host_pk, srv_pk, 32);
    }
    free(reply);

    /* ── Build the mpint-encoded K for key derivation ── */
    Buf km; buf_init(&km);
    buf_mpint(&km, K_raw, 32);

    /* ── Derive session keys ── */
    /*
     * For chacha20-poly1305@openssh.com each direction needs 64 bytes:
     *   letter 'C' → 64-byte send key
     *   letter 'D' → 64-byte recv key
     * (Letters 'A'/'B' would be IVs; this cipher uses the seqno as its IV.)
     */
    uint8_t new_send[64], new_recv[64];
    derive_key(km.b, km.len, H, 'C', ssh->session_id, new_send, 64);
    derive_key(km.b, km.len, H, 'D', ssh->session_id, new_recv, 64);
    buf_free(&km);
    sodium_memzero(K_raw, sizeof K_raw);

    /* ── Exchange NEWKEYS ── */
    uint8_t nk = SSH_MSG_NEWKEYS;
    pkt_send(ssh, &nk, 1);                      /* under the old keys, in a re-key */
    memcpy(ssh->send_key, new_send, 64);
    ssh->send_enc = 1;   /* our side encrypts from this point */
    if (ssh->strict_kex) ssh->send_seq = 0;

    size_t nk_len; uint8_t *nk_pkt = pkt_recv(ssh, &nk_len);
    if (nk_pkt[0] != SSH_MSG_NEWKEYS) die("expected NEWKEYS, got %d", nk_pkt[0]);
    free(nk_pkt);
    memcpy(ssh->recv_key, new_recv, 64);
    if (ssh->strict_kex) ssh->recv_seq = 0;
    ssh->recv_enc = 1;   /* server side now encrypts too */
    sodium_memzero(new_send, sizeof new_send);
    sodium_memzero(new_recv, sizeof new_recv);
}

/* ════════════════════════ Service / auth ════════════════════════════════ */

/*
 * The next packet, for a wait that is not about key exchange.  A server may
 * start a re-key at any time after the first one; the KEXINIT that starts it
 * is answered here.  Before, it was discarded, and the server then waited
 * for our KEXINIT while we waited for its reply: the session froze.
 */
static uint8_t *pkt_recv_wait(SSH *ssh, size_t *plen) {
    for (;;) {
        uint8_t *p = pkt_recv(ssh, plen);
        if (*plen && p[0] == SSH_MSG_KEXINIT) {
            do_kex(ssh, NULL, p, *plen);
            free(p);
            continue;
        }
        return p;
    }
}

static void request_service(SSH *ssh, const char *name) {
    Buf b; buf_init(&b);
    buf_u8(&b, SSH_MSG_SERVICE_REQUEST);
    buf_cstr(&b, name);
    send_buf(ssh, &b);

    size_t rlen; uint8_t *r = pkt_recv_wait(ssh, &rlen);
    if (r[0] != SSH_MSG_SERVICE_ACCEPT) die("service request '%s' denied", name);
    free(r);
}

/*
 * The next authentication message.  A server may send a USERAUTH_BANNER (the
 * "Banner" of sshd_config: a legal notice, say) at any point before the
 * result; it is shown to the user, as ssh does, and not an error, which it
 * used to be ("unexpected auth response 53").  Control characters other than
 * newlines are neutralised, as for any text from the server.
 */
static uint8_t *auth_recv(SSH *ssh, size_t *rlen) {
    for (;;) {
        uint8_t *r = pkt_recv_wait(ssh, rlen);
        if (r[0] != SSH_MSG_USERAUTH_BANNER) return r;
        Buf b = { r, *rlen, *rlen, 1 };
        size_t ml; const uint8_t *msg = rdstr(&b, &ml);
        char text[4096];
        server_text(text, sizeof text, msg, ml);
        for (size_t i = 0; i < ml && i < sizeof text - 1; i++)
            if (msg[i] == '\n') text[i] = '\n';
        err(text);
        err("\n");
        free(r);
    }
}

/*
 * Publickey auth (RFC 4252 §7)
 *
 * Step 1: send a "query" (no actual_signature=false) to check if the server
 *         accepts this key at all — avoids burning the signature if not needed.
 *         Some servers just always say OK here; that's fine.
 * Step 2: sign and send with actual_signature=true.
 *
 * Signed payload:
 *   string  session_id
 *   byte    SSH_MSG_USERAUTH_REQUEST
 *   string  username
 *   string  "ssh-connection"
 *   string  "publickey"
 *   bool    TRUE
 *   string  "ssh-ed25519"
 *   string  pubkey_blob
 */
static void do_auth(SSH *ssh, const char *user,
                    const uint8_t sk[crypto_sign_SECRETKEYBYTES],
                    const uint8_t pk[crypto_sign_PUBLICKEYBYTES])
{
    /* Wire-format public key blob used in multiple places */
    Buf pkblob; buf_init(&pkblob);
    buf_cstr(&pkblob, "ssh-ed25519");
    buf_str(&pkblob, pk, 32);

    /* ── Step 1: probe ── */
    Buf probe; buf_init(&probe);
    buf_u8(&probe, SSH_MSG_USERAUTH_REQUEST);
    buf_cstr(&probe, user);
    buf_cstr(&probe, "ssh-connection");
    buf_cstr(&probe, "publickey");
    buf_u8(&probe, 0);                              /* actual_signature = false */
    buf_cstr(&probe, "ssh-ed25519");
    buf_str(&probe, pkblob.b, pkblob.len);
    send_buf(ssh, &probe);

    size_t rlen; uint8_t *r = auth_recv(ssh, &rlen);
    if (r[0] == SSH_MSG_USERAUTH_FAILURE) die("server rejected our key type");
    if (r[0] != SSH_MSG_USERAUTH_PK_OK && r[0] != SSH_MSG_USERAUTH_SUCCESS)
        die("unexpected auth response %d during probe", r[0]);
    int already_ok = (r[0] == SSH_MSG_USERAUTH_SUCCESS);
    free(r);
    if (already_ok) { buf_free(&pkblob); return; }

    /* ── Step 2: build signing blob, sign, send ── */
    Buf to_sign; buf_init(&to_sign);
    buf_str(&to_sign, ssh->session_id, 32);
    buf_u8(&to_sign, SSH_MSG_USERAUTH_REQUEST);
    buf_cstr(&to_sign, user);
    buf_cstr(&to_sign, "ssh-connection");
    buf_cstr(&to_sign, "publickey");
    buf_u8(&to_sign, 1);                            /* actual_signature = true */
    buf_cstr(&to_sign, "ssh-ed25519");
    buf_str(&to_sign, pkblob.b, pkblob.len);

    uint8_t raw_sig[64];
    crypto_sign_ed25519_detached(raw_sig, NULL, to_sign.b, to_sign.len, sk);
    buf_free(&to_sign);

    /* Wire-format signature blob */
    Buf sigblob; buf_init(&sigblob);
    buf_cstr(&sigblob, "ssh-ed25519");
    buf_str(&sigblob, raw_sig, 64);

    Buf req; buf_init(&req);
    buf_u8(&req, SSH_MSG_USERAUTH_REQUEST);
    buf_cstr(&req, user);
    buf_cstr(&req, "ssh-connection");
    buf_cstr(&req, "publickey");
    buf_u8(&req, 1);                                /* actual_signature = true */
    buf_cstr(&req, "ssh-ed25519");
    buf_str(&req, pkblob.b, pkblob.len);
    buf_str(&req, sigblob.b, sigblob.len);
    buf_free(&pkblob); buf_free(&sigblob);
    send_buf(ssh, &req);

    r = auth_recv(ssh, &rlen);
    if (r[0] != SSH_MSG_USERAUTH_SUCCESS) {
        if (r[0] == SSH_MSG_USERAUTH_FAILURE)
            die("authentication failed — check key is in authorized_keys");
        die("unexpected auth response %d", r[0]);
    }
    free(r);
}

/* ════════════════════════ Channel / session ══════════════════════════════ */

static void open_session(SSH *ssh) {
    ssh->local_chan = 0;

    Buf b; buf_init(&b);
    buf_u8(&b, SSH_MSG_CHANNEL_OPEN);
    buf_cstr(&b, "session");
    buf_u32(&b, ssh->local_chan);
    buf_u32(&b, WINDOW_SIZE);
    buf_u32(&b, CHAN_MAX_PKT);
    send_buf(ssh, &b);

    size_t rlen; uint8_t *r = pkt_recv_wait(ssh, &rlen);
    if (r[0] == SSH_MSG_CHANNEL_OPEN_FAILURE) {
        Buf rb = { r, rlen, rlen, 1 };
        rdu32(&rb); /* local channel */
        uint32_t code = rdu32(&rb);
        size_t ml; const uint8_t *msg = rdstr(&rb, &ml);
        char omsg[256];
        server_text(omsg, sizeof omsg, msg, ml);
        die("channel open failed (code %u): %s", code, omsg);
    }
    if (r[0] != SSH_MSG_CHANNEL_OPEN_CONFIRMATION)
        die("expected CHANNEL_OPEN_CONFIRMATION, got %d", r[0]);
    Buf rb = { r, rlen, rlen, 1 };
    rdu32(&rb);                          /* recipient (our local) channel */
    ssh->remote_chan = rdu32(&rb);
    ssh->remote_window = rdu32(&rb);
    ssh->remote_maxpkt = rdu32(&rb);
    if (!ssh->remote_maxpkt) die("server announced a maximum packet size of 0");
    if (ssh->remote_maxpkt > 32768) ssh->remote_maxpkt = 32768;  /* ours is the lower limit anyway */
    free(r);
}

/* The server's window only grows by what it says, and must not wrap. */
static void add_window(SSH *ssh, uint32_t n) {
    ssh->remote_window = n > UINT32_MAX - ssh->remote_window ? UINT32_MAX
                                                             : ssh->remote_window + n;
}

/* Sends a channel request; with want_reply, returns 1 if the server accepted
 * it and 0 if it refused it.  The caller decides whether a refusal matters. */
static int channel_request(SSH *ssh, const char *type, int want_reply, Buf *extra)
{
    Buf b; buf_init(&b);
    buf_u8(&b, SSH_MSG_CHANNEL_REQUEST);
    buf_u32(&b, ssh->remote_chan);
    buf_cstr(&b, type);
    buf_u8(&b, want_reply ? 1 : 0);
    if (extra) buf_bytes(&b, extra->b, extra->len);
    send_buf(ssh, &b);

    if (!want_reply) return 1;

    /*
     * Loop until we get a definitive channel reply.  The server may
     * interleave WINDOW_ADJUST or server-initiated CHANNEL_REQUESTs
     * (e.g. exit-status notifications) before our actual reply arrives.
     */
    for (;;) {
        size_t rlen; uint8_t *r = pkt_recv_wait(ssh, &rlen);
        if (r[0] == SSH_MSG_CHANNEL_SUCCESS) { free(r); return 1; }
        if (r[0] == SSH_MSG_CHANNEL_FAILURE) { free(r); return 0; }
        if (r[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST && rlen >= 9) {
            /* honour window updates that arrive before our reply */
            Buf rb = { r, rlen, rlen, 1 };
            rdu32(&rb);
            add_window(ssh, rdu32(&rb));
            free(r); continue;
        }
        /* Anything else (e.g. unexpected CHANNEL_REQUEST from server):
         * skip it and keep waiting — do not die. */
        free(r);
    }
}

/* A request the session cannot do without: refusal is fatal. */
static void send_channel_request(SSH *ssh, const char *type, int want_reply, Buf *extra)
{
    if (!channel_request(ssh, type, want_reply, extra))
        die("channel request '%s' failed", type);
}

/*
 * Request a PTY of the terminal's current size, announcing `term`.  Returns 1
 * if the server gave one.  A refusal is not an error: the command runs without
 * a pty, as with ssh (PermitTTY no on the server, for instance).
 */
static int request_pty(SSH *ssh, const char *term) {
    uint32_t cols = 0, rows = 0, xpix, ypix;
    tty_size(&cols, &rows, &xpix, &ypix);
    if (!cols) cols = 80;
    if (!rows) rows = 24;

    Buf ex; buf_init(&ex);
    buf_cstr(&ex, term);
    buf_u32(&ex, cols); buf_u32(&ex, rows);  /* width (chars), height (chars) */
    buf_u32(&ex, 0);    buf_u32(&ex, 0);     /* width (pixels), height (pixels) */
    buf_u32(&ex, 0);                         /* empty terminal modes list */
    int granted = channel_request(ssh, "pty-req", 1, &ex);
    buf_free(&ex);
    return granted;
}

/* ═════════════════════════ Session I/O ═══════════════════════════════════
 *
 * The loop itself -- poll() on the terminal and the TCP socket -- belongs to
 * the terminal front-end (tty_run), which has to wait for whatever its input
 * is.  It calls these:
 *   terminal → server:  ssh_send_input(), ssh_send_eof(), ssh_window_change()
 *   server → terminal:  ssh_pump(), which hands the data to tty_out()
 */

/* Tell the server the terminal changed size. */
void ssh_window_change(SSH *ssh) {
    uint32_t cols, rows, xpix, ypix;
    if (tty_size(&cols, &rows, &xpix, &ypix) < 0) return;
    Buf ex; buf_init(&ex);
    buf_u32(&ex, cols);
    buf_u32(&ex, rows);
    buf_u32(&ex, xpix);
    buf_u32(&ex, ypix);
    send_channel_request(ssh, "window-change", 0, &ex);
    buf_free(&ex);
}

/* One CHANNEL_DATA packet; the caller has checked n against window and maxpkt. */
static void send_data(SSH *ssh, const uint8_t *data, size_t n) {
    Buf b; buf_init(&b);
    buf_u8(&b, SSH_MSG_CHANNEL_DATA);
    buf_u32(&b, ssh->remote_chan);
    buf_str(&b, data, n);
    send_buf(ssh, &b);
    ssh->remote_window -= (uint32_t)n;
}

static size_t chunk_size(const SSH *ssh, size_t n) {
    if (n > ssh->remote_window) n = ssh->remote_window;
    if (n > ssh->remote_maxpkt) n = ssh->remote_maxpkt;
    return n;
}

static void send_eof_packet(SSH *ssh) {
    uint8_t eof_pkt[5];
    eof_pkt[0] = SSH_MSG_CHANNEL_EOF;
    eof_pkt[1] = ssh->remote_chan >> 24;
    eof_pkt[2] = (ssh->remote_chan >> 16) & 0xff;
    eof_pkt[3] = (ssh->remote_chan >>  8) & 0xff;
    eof_pkt[4] =  ssh->remote_chan        & 0xff;
    pkt_send(ssh, eof_pkt, 5);
}

/* Send as much of the queue as the window allows (the server just grew it). */
static void flush_outq(SSH *ssh) {
    size_t done = 0, k;
    while (done < ssh->outq_len && (k = chunk_size(ssh, ssh->outq_len - done)) > 0) {
        send_data(ssh, ssh->outq + done, k);
        done += k;
    }
    ssh->outq_len -= done;
    if (ssh->outq_len) memmove(ssh->outq, ssh->outq + done, ssh->outq_len);
    else { free(ssh->outq); ssh->outq = NULL; }
    if (!ssh->outq_len && ssh->eof_pending) { ssh->eof_pending = 0; send_eof_packet(ssh); }
}

size_t ssh_send_room(const SSH *ssh) {
    return ssh->outq_len ? 0 : chunk_size(ssh, (size_t)-1);
}

/* Terminal input → CHANNEL_DATA → server.  Input beyond the server's window
 * used to be dropped silently (8 MiB piped to a slow command arrived as 2);
 * it is queued now, and the front-end can stop reading with ssh_send_room(). */
void ssh_send_input(SSH *ssh, const uint8_t *data, size_t n) {
    size_t k;
    if (!ssh->outq_len)                       /* nothing waiting ahead of it: send directly */
        while (n && (k = chunk_size(ssh, n)) > 0) { send_data(ssh, data, k); data += k; n -= k; }
    if (!n) return;
    uint8_t *q = realloc(ssh->outq, ssh->outq_len + n);
    if (!q) die("malloc");
    memcpy(q + ssh->outq_len, data, n);
    ssh->outq = q; ssh->outq_len += n;
}

/* The terminal's input has ended: tell server we're done sending (after
 * whatever input is still waiting for window). */
void ssh_send_eof(SSH *ssh) {
    ssh->client_eof = 1;
    if (ssh->outq_len) ssh->eof_pending = 1;
    else send_eof_packet(ssh);
}

/* Data of ours the front-end has shown: give the server window to send more.
 * Batched: one WINDOW_ADJUST per half window, not one packet per packet. */
static void consumed(SSH *ssh, size_t n) {
    ssh->recv_pending += (uint32_t)n;
    if (ssh->recv_pending < WINDOW_SIZE / 2) return;
    Buf adj; buf_init(&adj);
    buf_u8(&adj, SSH_MSG_CHANNEL_WINDOW_ADJUST);
    buf_u32(&adj, ssh->remote_chan);
    buf_u32(&adj, ssh->recv_pending);
    send_buf(ssh, &adj);
    ssh->recv_pending = 0;
}

/* The shell's convention: 128 + the signal's number; 255 if we do not know it. */
static int signal_status(const uint8_t *name, size_t n) {
    static const struct { const char *name; int num; } sigs[] = {
        {"HUP", 1}, {"INT", 2}, {"QUIT", 3}, {"ILL", 4}, {"ABRT", 6}, {"FPE", 8},
        {"KILL", 9}, {"USR1", 10}, {"SEGV", 11}, {"USR2", 12}, {"PIPE", 13},
        {"ALRM", 14}, {"TERM", 15},
    };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++)
        if (strlen(sigs[i].name) == n && memcmp(name, sigs[i].name, n) == 0)
            return 128 + sigs[i].num;
    return 255;
}

/*
 * Read and handle one packet from the server (call when ssh->fd is readable):
 *   CHANNEL_DATA          → tty_out(0, ...)
 *   CHANNEL_EXTENDED_DATA → tty_out(1, ...)
 * and WINDOW_ADJUST, EOF, CLOSE and the exit status.  Returns nonzero when
 * the session is over; the exit status is then in ssh->exit_status.
 */
int ssh_pump(SSH *ssh) {
    size_t plen; uint8_t *pkt = pkt_recv(ssh, &plen);
    if (!plen) { free(pkt); return 0; }

    switch (pkt[0]) {
        case SSH_MSG_CHANNEL_DATA: {
            Buf b = { pkt, plen, plen, 1 };
            rdu32(&b);  /* recipient channel */
            size_t dl; const uint8_t *data = rdstr(&b, &dl);
            tty_out(0, data, dl);
            consumed(ssh, dl);
            break;
        }
        case SSH_MSG_CHANNEL_EXTENDED_DATA: {
            Buf b = { pkt, plen, plen, 1 };
            rdu32(&b); rdu32(&b);  /* channel, data_type */
            size_t dl; const uint8_t *data = rdstr(&b, &dl);
            tty_out(1, data, dl);
            consumed(ssh, dl);   /* stderr counts against the same window; it was never given back,
                                  * so a command writing 1 MiB to stderr stalled for good */
            break;
        }
        case SSH_MSG_CHANNEL_WINDOW_ADJUST: {
            Buf b = { pkt, plen, plen, 1 };
            rdu32(&b);
            add_window(ssh, rdu32(&b));
            flush_outq(ssh);
            break;
        }
        case SSH_MSG_CHANNEL_REQUEST: {
            /* We only care about the remote command's fate: exit-status, or
             * exit-signal (a command killed by a signal has no status; it
             * used to exit with 0, as if it had succeeded). */
            Buf b = { pkt, plen, plen, 1 };
            rdu32(&b);  /* channel */
            size_t tl; const uint8_t *type = rdstr(&b, &tl);
            uint8_t want_reply = rdu8(&b);
            int known = 0;
            if (tl == 11 && memcmp(type, "exit-status", 11) == 0) {
                ssh->exit_status = (int)rdu32(&b); known = 1;
            } else if (tl == 11 && memcmp(type, "exit-signal", 11) == 0) {
                size_t nl; const uint8_t *name = rdstr(&b, &nl);
                ssh->exit_status = signal_status(name, nl); known = 1;
            }
            if (want_reply) {
                Buf r2; buf_init(&r2);
                buf_u8(&r2, known ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE);
                buf_u32(&r2, ssh->remote_chan);
                send_buf(ssh, &r2);
            }
            break;
        }
        case SSH_MSG_CHANNEL_EOF:
            ssh->server_eof = 1;
            break;
        case SSH_MSG_CHANNEL_CLOSE: {
            /* Echo CLOSE back and exit */
            Buf b; buf_init(&b);
            buf_u8(&b, SSH_MSG_CHANNEL_CLOSE);
            buf_u32(&b, ssh->remote_chan);
            send_buf(ssh, &b);
            free(pkt);
            return 1;
        }
        case SSH_MSG_KEXINIT:
            do_kex(ssh, NULL, pkt, plen);      /* the server wants new keys */
            break;
        default:
            /* Politely say we don't understand */
            if (pkt[0] >= 128) {  /* channel messages only */
                Buf b; buf_init(&b);
                buf_u8(&b, SSH_MSG_UNIMPLEMENTED);
                buf_u32(&b, ssh->recv_seq - 1);
                send_buf(ssh, &b);
            }
            break;
    }
    free(pkt);

    /* Over only at the server's CLOSE (handled above).  Ending when both sides
     * had sent EOF was wrong: sshd sends the command's EOF *before* its
     * exit-status, so with stdin already at EOF (< file, | pipe, </dev/null:
     * every scripted use) the status was never read and mssh exited 0 for a
     * failed command. */
    return 0;
}

/* ═══════════════════════════════ main ═══════════════════════════════════ */

/* Secrets that live in main's frame, wiped on every way out: die() ends in
 * exit() too, so this runs for failures as well as for a normal end. */
static SSH     *g_ssh;
static uint8_t *g_sk;
static void wipe_secrets(void) {
    if (g_ssh) { sodium_memzero(g_ssh->send_key, sizeof g_ssh->send_key);
                 sodium_memzero(g_ssh->recv_key, sizeof g_ssh->recv_key); }
    if (g_sk)  sodium_memzero(g_sk, crypto_sign_SECRETKEYBYTES);
}

/* If stdin, stdout or stderr is closed, the next open() or socket() takes its
 * number: the connection itself became stdin, and the host key prompt read
 * the server's bytes as the answer (a server could say "yes" after its signed
 * reply, and the key was stored without the user being asked).  Standard
 * descriptors that are closed are opened on /dev/null instead. */
static void ensure_std_fds(void) {
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
        if (fcntl(fd, F_GETFD) >= 0) continue;
        int n = open("/dev/null", O_RDWR);
        if (n < 0) die("/dev/null: %s", strerror(errno));
        if (n != fd) {
            if (dup2(n, fd) < 0) die("dup2: %s", strerror(errno));
            close(n);
        }
    }
}

int main(int argc, char **argv) {
    ensure_std_fds();
    if (sodium_init() < 0) die("libsodium init failed");
#if defined(HAVE_PRCTL) && defined(PR_SET_DUMPABLE)
    /* The process holds the private key and the session keys: no core file,
     * and no ptrace()/ /proc/PID/mem for other processes of the same user. */
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif

    const char *identity = NULL;
    const char *port     = "22";
    int         force_pty = 0;
    int opt;
    while ((opt = getopt(argc, argv, "i:p:o:t")) != -1) {
        switch (opt) {
            case 'i': identity = optarg; break;
            case 'p': port     = optarg; break;
            case 't': force_pty = 1;     break;
            case 'o': {
                /*
                 * Minimal -o parsing: "Key=Value" only.
                 * Supported options:
                 *   UserKnownHostsFile=<path>
                 */
                char *eq = strchr(optarg, '=');
                if (!eq)
                    die("mssh: bad -o syntax (expected Key=Value): %s", optarg);
                *eq = '\0';
                const char *key = optarg, *val = eq + 1;
                if (strcasecmp(key, "UserKnownHostsFile") == 0) {
                    g_known_hosts_file = val;
                } else if (strcasecmp(key, "ProxyCommand") == 0) {
                    g_proxy_command = val;
                } else if (strcasecmp(key, "ConnectTimeout") == 0) {
                    char *end; long v = strtol(val, &end, 10);
                    if (*val == '\0' || *end != '\0' || v < 0 || v > 3600)
                        die("mssh: bad ConnectTimeout (0-3600 seconds): %s", val);
                    g_connect_timeout = (int)v;
                } else {
                    die("mssh: unsupported option: %s", key);
                }
                break;
            }
            default:
                err("usage: mssh [-t] [-i identity] [-p port]\n"
                    "            [-o Key=Value] [user@]host [command ...]\n"
                    "\n"
                    "Supported -o options:\n"
                    "  UserKnownHostsFile=<path>\n"
                    "  ProxyCommand=<cmd>  (%h/%p expanded to host/port)\n"
                    "  ConnectTimeout=<s>  (handshake time limit, default 60; 0: none)\n");
                return 1;
        }
    }
    if (optind >= argc) {
        err("usage: mssh [-t] [-i identity] [-p port]\n"
            "            [-o Key=Value] [user@]host [command ...]\n");
        return 1;
    }

    /* Parse [user@]host */
    char *hostarg = argv[optind++];
    const char *user = getenv("USER");
    if (!user) user = "root";
    const char *host = hostarg;
    char *at = strchr(hostarg, '@');
    if (at) { *at = '\0'; user = hostarg; host = at + 1; }
    if (!valid_host(host)) die("mssh: invalid host name: %s", host);
    if (!valid_port(port)) die("mssh: invalid port: %s", port);

    /* Build command string (remaining argv joined with spaces) */
    char command[4096] = {0};
    size_t cl = 0;
    for (int i = optind; i < argc; i++) {
        size_t al = strlen(argv[i]), sep = i > optind;
        /* Running a silently shortened command would be worse than failing. */
        if (al + sep >= sizeof(command) - cl) die("mssh: command too long");
        if (sep) command[cl++] = ' ';
        memcpy(command + cl, argv[i], al); cl += al;
        command[cl] = '\0';
    }
    int have_command = (command[0] != '\0');

    /* Default identity: ~/.ssh/id_ed25519 */
    char default_id[512];
    if (!identity) {
        const char *home = getenv("HOME");
        if (!home) die("$HOME not set");
        if (snprintf(default_id, sizeof default_id, "%s/.ssh/id_ed25519", home)
            >= (int)sizeof default_id) die("$HOME path too long");
        identity = default_id;
    }

    /* The terminal is set up from here on, so that what follows (errors, the
     * host key question) can be shown in it. */
    char title[512];
    snprintf(title, sizeof(title), "%s@%s", user, host);
    tty_open(title);

    /* static: wipe_secrets() runs from exit(), when main's frame is gone if
     * main returned (it would be wiping dead stack); die() exits mid-flight. */
    static uint8_t sk[crypto_sign_SECRETKEYBYTES];
    uint8_t pk[crypto_sign_PUBLICKEYBYTES];
    g_sk = sk;
    atexit(wipe_secrets);
    load_ed25519_key(identity, sk, pk);

    /* ── Connect (direct TCP or ProxyCommand) ── */
    hs_arm();
    int fd = g_proxy_command ? proxy_connect(g_proxy_command, host, port)
                             : tcp_connect(host, port);

    static SSH ssh;
    g_ssh = &ssh;
    ssh.exit_status = 255;     /* as ssh: until the server reports a status (or a signal) */
    ssh.fd = fd;

    /* ── Version exchange (RFC 4253 §4.2) ── */
    /*
     * Each side sends a UTF-8 version string ending in \r\n.
     * We send ours first, then read the server's (which may be preceded
     * by extra banner lines that don't start with "SSH-").
     */
#define OUR_VER "SSH-2.0-mssh AAAAAA"
    {
        char t[sizeof(OUR_VER) - 1 + 2];
        memcpy(t, OUR_VER "\r\n", sizeof(OUR_VER) - 1 + 2);
        char r[6];
        randombytes_buf(r,6);
        t[sizeof(OUR_VER) - 7]+=(r[4]%26)+(r[2]&32);
        t[sizeof(OUR_VER) - 6]+=(r[2]%26)+(r[5]&32);
        t[sizeof(OUR_VER) - 5]+=(r[3]%26)+(r[4]&32);
        t[sizeof(OUR_VER) - 4]+=(r[0]%26)+(r[1]&32);
        t[sizeof(OUR_VER) - 3]+=(r[1]%26)+(r[0]&32);
        t[sizeof(OUR_VER) - 2]+=(r[5]%26)+(r[3]&32);
        xsend(fd, t, sizeof(OUR_VER) - 1 + 2);
        memcpy(ssh.client_ver, t, sizeof(OUR_VER)-1);
        ssh.client_ver[sizeof(OUR_VER)-1]=0;
    }

    /* Read lines until we see one starting with "SSH-2.0" */
    /* The server may send text before its version line (RFC 4253 4.2), but a
     * server that never sends one must not keep us reading forever. */
    char sv[256]; size_t svlen = 0;
    size_t pre_bytes = 0, pre_lines = 0;
    for (;;) {
        char c;
        xread(fd, &c, 1);
        if (++pre_bytes > MAX_PRE_BANNER_BYTES) die("too much text before the server's SSH version line");
        if (c == '\n') {
            if (++pre_lines > MAX_PRE_BANNER_LINES) die("too many lines before the server's SSH version line");
            if (svlen > 0 && sv[svlen-1] == '\r') sv[--svlen] = '\0';
            else sv[svlen] = '\0';
            if (strncmp(sv, "SSH-2.0", 7) == 0) break;
            svlen = 0;  /* banner line — discard and keep reading */
        } else {
            if (svlen < sizeof(sv) - 1) sv[svlen++] = c;
        }
    }
    strcpy(ssh.server_ver, sv);   /* sv is null-terminated, same fixed size */

    /* ── Key exchange + auth ── */
    do_kex(&ssh, host, NULL, 0);
    request_service(&ssh, "ssh-userauth");
    do_auth(&ssh, user, sk, pk);
    sodium_memzero(sk, sizeof sk);       /* the key has signed; nothing needs it now */

    /* ── Open channel + request session ── */
    open_session(&ssh);

    /*
     * Request a PTY when:
     *   - running an interactive shell (no command given), OR
     *   - the user passed -t (force PTY, like real ssh)
     * Suppress it otherwise — exec commands expect plain pipes.
     */
    const char *term = tty_term(force_pty || !have_command);
    int got_pty = term != NULL && request_pty(&ssh, term);
    if (term && !got_pty) err("Pseudo-terminal will not be allocated: the server refused it.\n");

    if (have_command) {
        Buf ex; buf_init(&ex);
        buf_cstr(&ex, command);
        send_channel_request(&ssh, "exec", 1, &ex);
        buf_free(&ex);
    } else {
        send_channel_request(&ssh, "shell", 1, NULL);
    }

    /* ── I/O loop (the terminal's) ── */
    hs_done(fd);
    int status = tty_run(&ssh, got_pty, !have_command);

    /* Clean up */
    free(ssh.client_kexinit);
    free(ssh.server_kexinit);
    sodium_memzero(sk, sizeof(sk));
    close(fd);
    return status;
}
