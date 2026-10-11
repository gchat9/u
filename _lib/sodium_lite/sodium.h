#pragma once
/*
 * sodium.h -- the part of libsodium's API that mssh uses, implemented by
 * sodium_lite.c in a few hundred lines of portable C: same names, same
 * signatures, same results, so code written for libsodium builds unchanged
 * by putting this directory in the include path (-I) instead of linking
 * -lsodium.  Anything else of libsodium is not here.
 *
 *   hashing         SHA-256 (streaming and one-shot)
 *   encryption      ChaCha20 (the original 64-bit nonce, 64-bit counter form)
 *   authentication  Poly1305
 *   key exchange    X25519 (crypto_scalarmult_curve25519), crypto_box_keypair
 *   signatures      Ed25519, detached
 *   misc            randombytes_buf, sodium_memcmp, sodium_memzero
 */
#include <stddef.h>
#include <stdint.h>

int  sodium_init(void);                        /* nothing to initialise: 0 */
void randombytes_buf(void *buf, size_t size);  /* from the kernel (getrandom) */
void sodium_memzero(void *pnt, size_t len);
int  sodium_memcmp(const void *b1, const void *b2, size_t len);   /* 0 if equal, else -1; constant time */

#define crypto_hash_sha256_BYTES 32U
typedef struct crypto_hash_sha256_state {
    uint32_t state[8];
    uint64_t count;                            /* bytes so far */
    uint8_t  buf[64];
} crypto_hash_sha256_state;
int crypto_hash_sha256(unsigned char *out, const unsigned char *in, unsigned long long inlen);
int crypto_hash_sha256_init(crypto_hash_sha256_state *state);
int crypto_hash_sha256_update(crypto_hash_sha256_state *state, const unsigned char *in, unsigned long long inlen);
int crypto_hash_sha256_final(crypto_hash_sha256_state *state, unsigned char *out);

/* c = m xor ChaCha20 keystream, from block counter ic (n: 8 bytes, k: 32); c may equal m */
int crypto_stream_chacha20_xor_ic(unsigned char *c, const unsigned char *m, unsigned long long mlen,
                                  const unsigned char *n, uint64_t ic, const unsigned char *k);

#define crypto_onetimeauth_poly1305_BYTES    16U
#define crypto_onetimeauth_poly1305_KEYBYTES 32U
typedef struct crypto_onetimeauth_poly1305_state {
    uint32_t r[5], h[5], pad[4];
    uint32_t leftover, final;
    uint8_t  buf[16];
} crypto_onetimeauth_poly1305_state;
int crypto_onetimeauth_poly1305_init(crypto_onetimeauth_poly1305_state *state, const unsigned char *key);
int crypto_onetimeauth_poly1305_update(crypto_onetimeauth_poly1305_state *state, const unsigned char *in, unsigned long long inlen);
int crypto_onetimeauth_poly1305_final(crypto_onetimeauth_poly1305_state *state, unsigned char *out);

#define crypto_scalarmult_curve25519_BYTES 32U
/* q = n * p; -1 if the result is all zeros (p of small order) */
int crypto_scalarmult_curve25519(unsigned char *q, const unsigned char *n, const unsigned char *p);
int crypto_scalarmult_curve25519_base(unsigned char *q, const unsigned char *n);
#define crypto_box_PUBLICKEYBYTES 32U
#define crypto_box_SECRETKEYBYTES 32U
int crypto_box_keypair(unsigned char *pk, unsigned char *sk);

#define crypto_sign_BYTES          64U
#define crypto_sign_PUBLICKEYBYTES 32U
#define crypto_sign_SECRETKEYBYTES (32U + 32U)    /* seed, then public key */
/* sk is the seed followed by the public key; siglen_p may be NULL */
int crypto_sign_ed25519_detached(unsigned char *sig, unsigned long long *siglen_p,
                                 const unsigned char *m, unsigned long long mlen, const unsigned char *sk);
int crypto_sign_verify_detached(const unsigned char *sig, const unsigned char *m,
                                unsigned long long mlen, const unsigned char *pk);
