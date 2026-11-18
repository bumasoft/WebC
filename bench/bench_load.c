// Socket-level load test against a running WebC server.
//
//   ./build/bench_load [-c conns] [-d seconds] [-p port] [-H host] [-m method] [-b body] [-k path] [-K]
//
//   -K  one request per connection (Connection: close): exercises accept/close
//       rather than keep-alive reuse
//
// Every connection is a plain thread doing keep-alive requests, which is the
// shape a real client makes: the server's worker count, not this number,
// decides how much parallelism it sees.
#define WEBC_NO_SELECT_MACRO 1
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  const char *host;
  int port;
  const char *method, *path, *body, *ctype;
  int keep_alive;
  double deadline;
  long ok, bad;      // requests completed / failed
  long closed, status_err; // server closed the connection / non-2xx answer
  long bytes;
  int last_errno;
  long last_have;
  double lat_max, lat_sum;
} Worker;

typedef struct {
  long ok, bad, closed, status_err, bytes;
  double lat_max, lat_sum;
} Result;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int connect_to(const char *host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
  if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
    close(fd);
    return -1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  // A stalled server must show up as an error, not as a hung load test.
  struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return fd;
}

// Read one HTTP response. Returns the status, or -1 when the peer closed the
// connection before answering (a normal end of a keep-alive connection, so the
// caller reconnects rather than counting a failure), or -2 on a malformed answer.
static int read_response(int fd, Worker *w) {
  static _Thread_local char buf[1 << 16];
  size_t have = 0;
  int status = 0;
  long clen = -1;
  size_t head_end = 0;
  for (;;) {
    // headers
    while (head_end == 0) {
      if (have == sizeof buf) return -2;
      ssize_t n = recv(fd, buf + have, sizeof buf - have, 0);
      if (n < 0) { w->last_errno = errno; w->last_have = (long)have; }
      if (n <= 0) return have == 0 ? -1 : -2;
      have += (size_t)n;
      for (size_t i = 0; i + 3 < have; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
          head_end = i + 4;
          break;
        }
      }
    }
    if (!status) {
      if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) {
        w->last_errno = -100;  // unparseable head
        if (getenv("WEBC_LOAD_DEBUG")) {
          fprintf(stderr, "[load] head (%zu bytes, have=%zu):\n---\n", head_end, have);
          fwrite(buf, 1, head_end < 300 ? head_end : 300, stderr);
          fprintf(stderr, "\n---\n");
        }
        return -3;
      }
      static const char CL[] = "\r\ncontent-length:";
      const size_t cl_n = sizeof CL - 1;  // strlen, computed, never counted by hand
      for (size_t i = 0; i + cl_n < head_end; i++) {
        if (strncasecmp(buf + i, CL, cl_n) == 0) {
          clen = strtol(buf + i + cl_n, nullptr, 10);
          break;
        }
      }
      // No length means chunked (or a body that runs to the close): the caller
      // cannot frame that on a keep-alive socket, so say so instead of guessing.
      if (clen < 0) {
        if (strcasestr(buf, "transfer-encoding:") != nullptr) {
          w->last_errno = -101;
          return -4;
        }
        clen = 0;
      }
    }
    size_t total = head_end + (size_t)clen;
    while (have < total) {
      if (have == sizeof buf) return -1;
      ssize_t n = recv(fd, buf + have, sizeof buf - have, 0);
      if (n < 0) { w->last_errno = errno; w->last_have = (long)have; }
      if (n <= 0) return -1;
      have += (size_t)n;
    }
    w->bytes += (long)total;
    return status;
  }
}

static void *run(void *arg) {
  Worker *w = arg;
  char req[2048];
  int rlen = snprintf(req, sizeof req,
                      "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: %s\r\n%s%s%s"
                      "Content-Length: %zu\r\n\r\n%s",
                      w->method, w->path, w->host, w->port,
                      w->keep_alive ? "keep-alive" : "close",
                      w->body ? "Content-Type: " : "", w->body ? w->ctype : "", w->body ? "\r\n" : "",
                      w->body ? strlen(w->body) : 0, w->body ? w->body : "");
  int fd = -1;
  while (now_s() < w->deadline) {
    if (fd < 0) {
      fd = connect_to(w->host, w->port);
      if (fd < 0) {
        w->bad++;
        usleep(1000);
        continue;
      }
    }
    double t0 = now_s();
    ssize_t sent = send(fd, req, (size_t)rlen, 0);
    if (sent != rlen) {
      close(fd);
      fd = -1;
      w->bad++;
      continue;
    }
    int status = read_response(fd, w);
    if (!w->keep_alive) {       // one request per connection: the close is the point
      close(fd);
      fd = -1;
    }
    if (status == -1) {          // peer closed: reconnect, not an error
      if (getenv("WEBC_LOAD_DEBUG"))
        fprintf(stderr, "[load] eof after %ld ok (errno=%d have=%ld)\n", w->ok, w->last_errno,
                w->last_have);
      close(fd);
      fd = -1;
      w->closed++;
      continue;
    }
    if (status < 0) {
      if (getenv("WEBC_LOAD_DEBUG"))
        fprintf(stderr, "[load] bad answer (%d) after %ld ok\n", status, w->ok);
      close(fd);
      fd = -1;
      w->bad++;
      continue;
    }
    double dt = now_s() - t0;
    w->lat_sum += dt;
    if (dt > w->lat_max) w->lat_max = dt;
    if (status >= 400) w->status_err++;
    else w->ok++;
  }
  if (fd >= 0) close(fd);
  return nullptr;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}

int main(int argc, char **argv) {
  int conns = 16;
  double dur = 3.0;
  Worker proto = { .host = "127.0.0.1", .port = 8080, .method = "GET", .path = "/",
                   .ctype = "application/json", .keep_alive = 1 };
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-c") && i + 1 < argc) conns = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-d") && i + 1 < argc) dur = atof(argv[++i]);
    else if (!strcmp(argv[i], "-p") && i + 1 < argc) proto.port = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-H") && i + 1 < argc) proto.host = argv[++i];
    else if (!strcmp(argv[i], "-m") && i + 1 < argc) proto.method = argv[++i];
    else if (!strcmp(argv[i], "-b") && i + 1 < argc) proto.body = argv[++i];
    else if (!strcmp(argv[i], "-k") && i + 1 < argc) proto.path = argv[++i];
    else if (!strcmp(argv[i], "-K")) proto.keep_alive = 0;
    else {
      fprintf(stderr,
              "usage: %s [-c conns] [-d seconds] [-H host] [-p port] [-m method] [-k path] [-b body] "
              "[-K]\n",
              argv[0]);
      return 2;
    }
  }
  if (conns < 1) conns = 1;
  if (conns > 512) conns = 512;

  Worker *ws = calloc((size_t)conns, sizeof *ws);
  pthread_t *th = calloc((size_t)conns, sizeof *th);
  double start = now_s();
  for (int i = 0; i < conns; i++) {
    ws[i] = proto;
    ws[i].deadline = start + dur;
    pthread_create(&th[i], nullptr, run, &ws[i]);
  }
  Result r = { 0 };
  double *lats = calloc((size_t)conns, sizeof *lats);
  for (int i = 0; i < conns; i++) {
    pthread_join(th[i], nullptr);
    r.ok += ws[i].ok;
    r.bad += ws[i].bad;
    r.closed += ws[i].closed;
    r.status_err += ws[i].status_err;
    r.bytes += ws[i].bytes;
    r.lat_sum += ws[i].lat_sum;
    if (ws[i].lat_max > r.lat_max) r.lat_max = ws[i].lat_max;
    lats[i] = ws[i].ok ? ws[i].lat_sum / ws[i].ok : 0;
  }
  double elapsed = now_s() - start;
  qsort(lats, (size_t)conns, sizeof *lats, cmp_double);
  printf("%-6s %-18s %5d conns  %7.0f req/s  %ld ok  %ld %s  %ld closed  %ld timeouts  "
         "%.1f MB  mean %.2f ms  max %.1f ms  p50conn %.2f ms\n",
         proto.method, proto.path, conns, (double)r.ok / elapsed, r.ok, r.status_err,
         proto.body ? "status" : "4xx/5xx", r.closed, r.bad,
         (double)r.bytes / (1024 * 1024), r.ok ? r.lat_sum * 1000 / (double)r.ok : 0,
         r.lat_max * 1000, lats[conns / 2] * 1000);
  free(lats);
  free(ws);
  free(th);
  return r.bad == 0 && r.status_err == 0 && r.ok > 0 ? 0 : 1;
}