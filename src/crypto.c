// SPDX-License-Identifier: Apache-2.0
// WebC — hashing and encoding primitives with no dependencies.
//
// SHA-256/HMAC/PBKDF2 exist because the postgres driver must speak SCRAM-SHA-256
// itself (SPEC §6.5: no libpq dependency) and because cookie signing needs a MAC.
// This is textbook, constant-time where it matters, and audited by its own tests.
#include <webc/core.h>
#include <webc/crypto.h> // the declarations these definitions have to match

#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/random.h>
#endif

// ---------------------------------------------------------------- base64

static const char b64_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool base64_encode(Arena *a, str in, str *out) {
  size_t need = ((in.n + 2) / 3) * 4 + 1;
  char *p = a ? arena_alloc(a, need) : wm_alloc(need);
  size_t o = 0;
  size_t i = 0;
  while (i + 2 < in.n) {
    u32 v = ((u32)(u8)in.p[i] << 16) | ((u32)(u8)in.p[i + 1] << 8) | (u32)(u8)in.p[i + 2];
    p[o++] = b64_tab[(v >> 18) & 63];
    p[o++] = b64_tab[(v >> 12) & 63];
    p[o++] = b64_tab[(v >> 6) & 63];
    p[o++] = b64_tab[v & 63];
    i += 3;
  }
  if (i < in.n) {
    bool two = (i + 1) < in.n;
    u32 v = ((u32)(u8)in.p[i] << 16) | (two ? ((u32)(u8)in.p[i + 1] << 8) : 0);
    p[o++] = b64_tab[(v >> 18) & 63];
    p[o++] = b64_tab[(v >> 12) & 63];
    p[o++] = two ? b64_tab[(v >> 6) & 63] : '=';
    p[o++] = '=';
  }
  p[o] = 0;
  *out = (str){p, o};
  return true;
}

static int b64_val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool base64_decode(Arena *a, str in, str *out) {
  char *p = a ? arena_alloc(a, in.n / 4 * 3 + 4) : wm_alloc(in.n / 4 * 3 + 4);
  size_t o = 0;
  u32 acc = 0;
  int bits = 0;
  for (size_t i = 0; i < in.n; i++) {
    char c = in.p[i];
    if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
    int v = b64_val(c);
    if (v < 0) {
      if (!a) wm_free(p);
      return false;
    }
    acc = (acc << 6) | (u32)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      p[o++] = (char)((acc >> bits) & 0xFF);
    }
  }
  p[o] = 0;
  *out = (str){p, o};
  return true;
}

// ---------------------------------------------------------------- SHA-256

typedef struct Sha256 {
  u32 h[8];
  u64 len;
  u8 buf[64];
  size_t fill;
} Sha256;

static const u32 K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline u32 rotr32(u32 x, int n) {
  return (x >> n) | (x << (32 - n));
}

static void sha256_block(Sha256 *s, const u8 *p) {
  u32 w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((u32)p[i * 4] << 24) | ((u32)p[i * 4 + 1] << 16) | ((u32)p[i * 4 + 2] << 8) |
           (u32)p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    u32 s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    u32 s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  u32 a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
  u32 e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
  for (int i = 0; i < 64; i++) {
    u32 S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    u32 ch = (e & f) ^ ((~e) & g);
    u32 t1 = h + S1 + ch + K256[i] + w[i];
    u32 S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    u32 mj = (a & b) ^ (a & c) ^ (b & c);
    u32 t2 = S0 + mj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}

static void sha256_init(Sha256 *s) {
  static const u32 iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s->h, iv, sizeof iv);
  s->len = 0;
  s->fill = 0;
}

static void sha256_update(Sha256 *s, const void *data, size_t n) {
  const u8 *p = data;
  s->len += n;
  while (n > 0) {
    size_t take = 64 - s->fill;
    if (take > n) take = n;
    memcpy(s->buf + s->fill, p, take);
    s->fill += take;
    p += take;
    n -= take;
    if (s->fill == 64) {
      sha256_block(s, s->buf);
      s->fill = 0;
    }
  }
}

static void sha256_final(Sha256 *s, u8 out[32]) {
  u64 bits = s->len * 8;
  u8 pad = 0x80;
  sha256_update(s, &pad, 1);
  u8 zero = 0;
  while (s->fill != 56)
    sha256_update(s, &zero, 1);
  u8 lenb[8];
  for (int i = 0; i < 8; i++)
    lenb[i] = (u8)(bits >> (56 - 8 * i));
  sha256_update(s, lenb, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (u8)(s->h[i] >> 24);
    out[i * 4 + 1] = (u8)(s->h[i] >> 16);
    out[i * 4 + 2] = (u8)(s->h[i] >> 8);
    out[i * 4 + 3] = (u8)s->h[i];
  }
}

void sha256(const void *data, size_t n, u8 out[32]) {
  Sha256 s;
  sha256_init(&s);
  sha256_update(&s, data, n);
  sha256_final(&s, out);
}

void hmac_sha256(const void *key, size_t klen, const void *msg, size_t mlen, u8 out[32]) {
  u8 k[64] = {0};
  if (klen > 64)
    sha256(key, klen, k);
  else
    memcpy(k, key, klen);
  u8 ipad[64], opad[64];
  for (int i = 0; i < 64; i++) {
    ipad[i] = (u8)(k[i] ^ 0x36);
    opad[i] = (u8)(k[i] ^ 0x5c);
  }
  Sha256 s;
  u8 inner[32];
  sha256_init(&s);
  sha256_update(&s, ipad, 64);
  sha256_update(&s, msg, mlen);
  sha256_final(&s, inner);
  sha256_init(&s);
  sha256_update(&s, opad, 64);
  sha256_update(&s, inner, 32);
  sha256_final(&s, out);
}

void pbkdf2_sha256(const void *pw, size_t pwlen, const void *salt, size_t saltlen, u32 iters,
                   u8 *out, size_t outlen) {
  u8 block[32];
  u32 counter = 1;
  size_t done = 0;
  while (done < outlen) {
    // U1 = HMAC(pw, salt || INT(i))
    size_t buf_len = saltlen + 4;
    u8 *buf = malloc(buf_len);
    if (!buf) wc_panic(__FILE__, __LINE__, "oom in pbkdf2");
    memcpy(buf, salt, saltlen);
    buf[saltlen] = (u8)(counter >> 24);
    buf[saltlen + 1] = (u8)(counter >> 16);
    buf[saltlen + 2] = (u8)(counter >> 8);
    buf[saltlen + 3] = (u8)counter;
    hmac_sha256(pw, pwlen, buf, buf_len, block);
    free(buf);
    u8 u[32];
    memcpy(u, block, 32);
    for (u32 i = 1; i < iters; i++) {
      hmac_sha256(pw, pwlen, u, 32, u);
      for (int j = 0; j < 32; j++)
        block[j] ^= u[j];
    }
    size_t take = outlen - done < 32 ? outlen - done : 32;
    memcpy(out + done, block, take);
    done += take;
    counter++;
  }
}

void constant_time_eq(const void *a, const void *b, size_t n, bool *eq) {
  const u8 *x = a, *y = b;
  u8 diff = 0;
  for (size_t i = 0; i < n; i++)
    diff |= (u8)(x[i] ^ y[i]);
  *eq = diff == 0;
}
// ---------------------------------------------------------------- SHA-1
// Websocket handshakes (RFC 6455) require SHA-1; it is used for nothing else.

static inline u32 rol32(u32 x, int n) {
  return (x << n) | (x >> (32 - n));
}

void sha1(const void *data, size_t n, u8 out[20]) {
  u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  size_t total = n + 1 + 8;
  size_t padded = (total + 63) / 64 * 64;
  u8 *buf = calloc(1, padded);
  if (!buf) wc_panic(__FILE__, __LINE__, "oom in sha1");
  memcpy(buf, data, n);
  buf[n] = 0x80;
  u64 bits = (u64)n * 8;
  for (int i = 0; i < 8; i++)
    buf[padded - 1 - (size_t)i] = (u8)(bits >> (8 * i));

  for (size_t off = 0; off < padded; off += 64) {
    u32 w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = ((u32)buf[off + i * 4] << 24) | ((u32)buf[off + i * 4 + 1] << 16) |
             ((u32)buf[off + i * 4 + 2] << 8) | (u32)buf[off + i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
      w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      u32 f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      u32 tmp = rol32(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol32(b, 30);
      b = a;
      a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  free(buf);
  for (int i = 0; i < 5; i++) {
    out[i * 4] = (u8)(h[i] >> 24);
    out[i * 4 + 1] = (u8)(h[i] >> 16);
    out[i * 4 + 2] = (u8)(h[i] >> 8);
    out[i * 4 + 3] = (u8)h[i];
  }
}

// ---------------------------------------------------------------- signing

static str b64url(Arena *a, const u8 *p, size_t n) {
  str raw = {0};
  base64_encode(a, (str){(char *)p, n}, &raw);
  char *w = (char *)raw.p; // arena memory: ours to rewrite
  size_t len = raw.n;
  for (size_t i = 0; i < len; i++) {
    if (w[i] == '+')
      w[i] = '-';
    else if (w[i] == '/')
      w[i] = '_';
  }
  while (len && w[len - 1] == '=')
    len--;
  return (str){w, len};
}

str crypto_sign(Arena *a, str key, str payload) {
  u8 mac[32];
  hmac_sha256(key.p, key.n, payload.p, payload.n, mac);
  str m = b64url(a, mac, sizeof mac);
  return str_fmt(a, "%.*s.%.*s", (int)payload.n, payload.p, (int)m.n, m.p);
}

bool crypto_verify(Arena *a, str key, str signed_value, str *payload_out) {
  i64 dot = str_rindex(signed_value, '.');
  if (dot <= 0) return false;
  str payload = str_sub(signed_value, 0, (size_t)dot);
  str given = str_sub(signed_value, (size_t)dot + 1, signed_value.n);
  u8 mac[32];
  hmac_sha256(key.p, key.n, payload.p, payload.n, mac);
  str want = b64url(a, mac, sizeof mac);
  bool eq = false;
  constant_time_eq(want.p, given.p, want.n < given.n ? want.n : given.n, &eq);
  if (!eq || want.n != given.n) return false;
  if (payload_out) *payload_out = payload;
  return true;
}

// One cached descriptor for the life of the process. Opening /dev/urandom per
// call cost 165 us a request (measured: a per-request CSP nonce turned an 6 us
// SSR render into a 165 us one); the open was the whole cost, not the read.
static _Atomic int g_urandom_fd = -1;

static int urandom_fd(void) {
  int fd = atomic_load(&g_urandom_fd);
  if (fd >= 0) return fd;
  fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  int prev = -1;
  if (!atomic_compare_exchange_strong(&g_urandom_fd, &prev, fd)) {
    close(fd);
    fd = prev;
  }
  return fd;
}

// Per-thread entropy buffer. getentropy() is one syscall per nonce, and on a rendered page
// one nonce is issued per request: measured at four workers this was the single hottest
// kernel operation in the whole request (strace: 1.00 getrandom/request), because threads
// contend on the kernel's CRNG. Refilling 256 bytes at a time amortises it over sixteen
// requests without weakening anything — these are still the OS's own bytes, not a user
// space generator, so there is no new algorithm to trust.
typedef struct {
  u8 buf[256];
  size_t off; // next unread byte; == sizeof buf means empty
} TRand;
static _Thread_local TRand t_rnd;

static bool rnd_refill(void) {
  t_rnd.off = 0;
#if defined(__APPLE__) || defined(__linux__)
  return getentropy(t_rnd.buf, sizeof t_rnd.buf) == 0;
#else
  return false;
#endif
}

void crypto_random_bytes(void *out, size_t n) {
  u8 *p = out;
  if (n <= sizeof t_rnd.buf) {
    if (t_rnd.off == 0 || sizeof t_rnd.buf - t_rnd.off < n) {
      if (rnd_refill()) {
        memcpy(p, t_rnd.buf, n);
        t_rnd.off = n;
        return;
      }
    } else {
      memcpy(p, t_rnd.buf + t_rnd.off, n);
      t_rnd.off += n;
      return;
    }
  }
#if defined(__APPLE__) || defined(__linux__)
  // getentropy is the cheap path (a syscall, no descriptor); it is capped at 256
  // bytes per call, so longer requests loop.
  for (size_t off = 0; off < n;) {
    size_t chunk = n - off < 256 ? n - off : 256;
    if (getentropy(p + off, chunk) != 0) break;
    off += chunk;
    if (off >= n) return;
  }
#endif
  int fd = urandom_fd();
  if (fd >= 0) {
    size_t off = 0;
    while (off < n) {
      ssize_t got = read(fd, p + off, n - off);
      if (got <= 0) break;
      off += (size_t)got;
    }
    if (off == n) return;
  }
  // No /dev/urandom: still better than a zero nonce. Not a claim of security.
  for (size_t i = 0; i < n; i++) p[i] = (u8)(rand() ^ (int)wc_wall_ms());
}

str crypto_random_hex(Arena *a, size_t bytes) {
  u8 *buf = arena_alloc(a, bytes);
  crypto_random_bytes(buf, bytes);
  char *hex = arena_alloc(a, bytes * 2 + 1);
  static const char *d = "0123456789abcdef";
  for (size_t i = 0; i < bytes; i++) {
    hex[i * 2] = d[buf[i] >> 4];
    hex[i * 2 + 1] = d[buf[i] & 15];
  }
  hex[bytes * 2] = 0;
  return (str){hex, bytes * 2};
}
