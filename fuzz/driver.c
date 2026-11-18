// A stand-in for the libFuzzer runtime.
//
// Xcode's clang ships without libclang_rt.fuzzer (measured: `-fsanitize=fuzzer`
// fails to link on a stock macOS with "library libclang_rt.fuzzer_osx.a not
// found"), so `make fuzz` builds the same harnesses against this driver instead of
// telling you the tool is missing. It is a small mutational fuzzer: it keeps a
// corpus of every input that reached new coverage-worthy behaviour (here: every
// input that did not crash), mutates it, and reports the input on a crash so the
// failure is reproducible by hand.
//
// With a real libFuzzer (brew install llvm, or Linux CI), the same files build
// with -fsanitize=fuzzer and get coverage guidance: nothing in the harnesses
// changes.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size);

#define MAX_CORPUS 512
#define MAX_INPUT (1 << 16)

static unsigned char *g_corpus[MAX_CORPUS];
static size_t g_corpus_len[MAX_CORPUS];
static int g_corpus_n;

static uint64_t g_state = 0x243f6a8885a308d3ull;
static uint64_t rnd(void) {
  g_state ^= g_state << 13;
  g_state ^= g_state >> 7;
  g_state ^= g_state << 17;
  return g_state;
}

static void on_crash(int sig) {
  fprintf(stderr, "driver: signal %d on input /tmp/webc-fuzz-crash.bin\n", sig);
  _exit(2);
}

static void save(const unsigned char *d, size_t n) {
  FILE *f = fopen("/tmp/webc-fuzz-crash.bin", "wb");
  if (!f) return;
  fwrite(d, 1, n, f);
  fclose(f);
}

int main(int argc, char **argv) {
  int seconds = argc > 1 ? atoi(argv[1]) : 10;
  // seed the corpus with the tokens these parsers care about
  static const char *seeds[] = {
      "{\"a\":1}", "[1,2,3]", "\"\"", "{\"a\":[{\"b\":null}]}", "1e309", "\\u00e9", "-0.0",
      "GET /users/1", "/files/a/b", "Cookie: session=abc; x=y", "--B\r\n\r\nv\r\n--B--",
      "a=1&b=2&c", "%2e%2e%2f", "{\"a\":\"\\ud83d\\ude00\"}",
  };
  for (size_t i = 0; i < sizeof seeds / sizeof *seeds && g_corpus_n < MAX_CORPUS; i++) {
    size_t n = strlen(seeds[i]);
    g_corpus[g_corpus_n] = malloc(n);
    memcpy(g_corpus[g_corpus_n], seeds[i], n);
    g_corpus_len[g_corpus_n++] = n;
  }
  signal(SIGSEGV, on_crash);
  signal(SIGBUS, on_crash);
  signal(SIGABRT, on_crash);
  signal(SIGILL, on_crash);
  signal(SIGFPE, on_crash);

  unsigned char buf[MAX_INPUT];
  time_t end = time(nullptr) + seconds;
  long iters = 0;
  while (time(nullptr) < end) {
    size_t n;
    if (g_corpus_n > 0 && (rnd() & 3)) {
      // mutate a corpus entry
      int pick = (int)(rnd() % (unsigned)g_corpus_n);
      n = g_corpus_len[pick];
      if (n > MAX_INPUT) n = MAX_INPUT;
      memcpy(buf, g_corpus[pick], n);
      int edits = 1 + (int)(rnd() % 8);
      for (int e = 0; e < edits; e++) {
        switch (rnd() % 4) {
        case 0: // flip a bit
          if (n) buf[rnd() % n] ^= (unsigned char)(1u << (rnd() % 8));
          break;
        case 1: // insert a byte
          if (n < MAX_INPUT) {
            size_t at = n ? rnd() % n : 0;
            memmove(buf + at + 1, buf + at, n - at);
            buf[at] = (unsigned char)rnd();
            n++;
          }
          break;
        case 2: // delete a byte
          if (n) {
            size_t at = rnd() % n;
            memmove(buf + at, buf + at + 1, n - at - 1);
            n--;
          }
          break;
        default: // splice in a piece of another entry
          if (g_corpus_n > 1) {
            int other = (int)(rnd() % (unsigned)g_corpus_n);
            size_t take = g_corpus_len[other] ? rnd() % g_corpus_len[other] + 1 : 0;
            if (take > MAX_INPUT - n) take = MAX_INPUT - n;
            memcpy(buf + n, g_corpus[other], take);
            n += take;
          }
          break;
        }
      }
    } else {
      // fresh random bytes: finds the "no structure at all" crashes
      n = rnd() % 256;
      for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)rnd();
    }
    save(buf, n);
    LLVMFuzzerTestOneInput(buf, n);
    // keep what survived, so the corpus drifts toward the interesting inputs
    if (g_corpus_n < MAX_CORPUS) {
      g_corpus[g_corpus_n] = malloc(n ? n : 1);
      memcpy(g_corpus[g_corpus_n], buf, n);
      g_corpus_len[g_corpus_n++] = n;
    } else if (n) {
      int pick = (int)(rnd() % MAX_CORPUS);
      free(g_corpus[pick]);
      g_corpus[pick] = malloc(n);
      memcpy(g_corpus[pick], buf, n);
      g_corpus_len[pick] = n;
    }
    iters++;
  }
  printf("driver: %ld iterations, %d inputs kept, no crash\n", iters, g_corpus_n);
  fflush(stdout);
  return 0;
}
