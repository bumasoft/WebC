// SPDX-License-Identifier: Apache-2.0
// WebC — hashing, MAC and encoding primitives (no external crypto dependency).
//
// SHA-256/HMAC/PBKDF2 exist because the postgres driver speaks SCRAM-SHA-256
// itself (SPEC §6.5: no libpq) and cookie signing needs a MAC. SHA-1 is here for
// the RFC 6455 websocket handshake and nothing else.
#ifndef WEBC_CRYPTO_H
#define WEBC_CRYPTO_H

#include <webc/core.h>

// base64_encode/decode, sha256, hmac_sha256, pbkdf2_sha256 and constant_time_eq
// are declared in core.h; this header adds the pieces built on them.
void sha1(const void *data, size_t n, u8 out[20]);

str  crypto_sign(Arena *a, str key, str payload);          // "<payload>.<mac>"
bool crypto_verify(Arena *a, str key, str signed_value, str *payload_out);
str  crypto_random_hex(Arena *a, size_t bytes);
void crypto_random_bytes(void *out, size_t n);   // /dev/urandom via getentropy, no per-call open

#endif // WEBC_CRYPTO_H
